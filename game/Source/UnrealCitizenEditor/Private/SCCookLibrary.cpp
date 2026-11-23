// USCCookLibrary: environment -> persistent level actor with pooled assets (WP-C1).
#include "SCCookLibrary.h"
#include "SCRigAssets.h"
#include "SCLift.h"
#include "SCRigComponent.h"
#include "SCTextureSource.h"
#include "SCMeshImporter.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "SCBridgeClient.h" // scbridge.h: sc_texels_decode_bgra8
#include "SCEnvironment.h"
#include "SCHangarIndex.h"
#include "SCInstancing.h"
#include "SCMaterialBuilder.h"
#include "SCVehicle.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Engine/StaticMeshSourceData.h"
#include "Hash/xxhash.h"
#include "Chaos/ChaosArchive.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/MemoryWriter.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCCook, Log, All);

namespace
{
/** A hash of a mesh's collision triangles (the importer derives them from the unsplit part, so they can change while the
 *  render description stays: a window's triangles), empty without any. */
FString CollisionId(const UStaticMesh& Mesh)
{
    UBodySetup* Body = Mesh.GetBodySetup();
    if (!Body || Body->TriMeshGeometries.IsEmpty()) return FString();
    TArray<uint8> Bytes;
    FMemoryWriter Writer(Bytes);
    Chaos::FChaosArchive Archive(Writer);
    for (const Chaos::FTriangleMeshImplicitObjectPtr& Geometry : Body->TriMeshGeometries)
        if (Geometry) Geometry->Serialize(Archive);
    return FString::Printf(TEXT("%016llx"), FXxHash64::HashBuffer(Bytes.GetData(), uint64(Bytes.Num())).Hash);
}

/** What a pooled asset is, beyond its source path (which an archive update or an exporter or importer fix keeps while
 *  the content changes): a mesh's committed source description id (a hash of its content, CommitSource's
 *  bUseHashAsGuid) and its collision triangles, a texture's first-mip bytes; Fallback when there is no description or
 *  no mip. */
FString ContentId(const UObject* Object, const FString& Fallback)
{
    if (const auto* Mesh = Cast<UStaticMesh>(Object))
    {
        const bool bDescription = Mesh->GetNumSourceModels() > 0 && Mesh->GetSourceModel(0).StaticMeshDescriptionBulkData &&
                                  !Mesh->GetSourceModel(0).StaticMeshDescriptionBulkData->GetBulkData().IsEmpty();
        return (bDescription ? Mesh->GetSourceModel(0).StaticMeshDescriptionBulkData->GetBulkData().GetUnversionedIdString() : Fallback) +
               TEXT(";collision=") + CollisionId(*Mesh);
    }
    if (const auto* Texture = Cast<UTexture2D>(Object))
    {
        FTexturePlatformData* Platform = const_cast<UTexture2D*>(Texture)->GetPlatformData();
        if (!Platform || Platform->Mips.IsEmpty()) return Fallback;
        FTexture2DMipMap& Mip = Platform->Mips[0];
        const void* Data = Mip.BulkData.LockReadOnly();
        const int64 Size = Mip.BulkData.GetBulkDataSize();
        const FXxHash64 Hash = Data ? FXxHash64::HashBuffer(Data, uint64(Size)) : FXxHash64();
        Mip.BulkData.Unlock();
        if (!Data) return Fallback;
        return FString::Printf(TEXT("%016llx_%dx%d_%d"), Hash.Hash, Mip.SizeX, Mip.SizeY, int32(Platform->PixelFormat));
    }
    return Fallback;
}

FString SafeName(FString Name)
{
    for (TCHAR& C : Name)
        if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    return Name.IsEmpty() ? TEXT("Object") : Name.Left(80);
}

struct FCooker
{
    FString Root, SharedRoot;
    TMap<UObject*, UObject*> Remap; // runtime object -> persistent asset
    TArray<UPackage*> ToSave;
    TArray<FString> Warnings;
    int32 Reused = 0, Created = 0;

    void MakePersistent(UObject* Object)
    {
        Object->ClearFlags(RF_Transient);
        ForEachObjectWithOuter(Object, [](UObject* Inner) { Inner->ClearFlags(RF_Transient); });
    }
    /** Runtime textures carry only platform mips: an editable source (SCTextureSource). */
    void AddTextureSource(UTexture2D* Texture)
    {
        FString Error;
        if (!SCTextureSource::Add(Texture, Error)) Warnings.Add(Texture->GetName() + TEXT(": ") + Error + TEXT("; saved without source"));
    }
    bool bFailed = false;
    /** Object moved into a package of its own, Folder/Name (made unique), saved with the cook. */
    void Persist(UObject* Object, const FString& Folder, const FString& Name)
    {
        if (!Object || Remap.Contains(Object)) return;
        FString Unique = SafeName(Name);
        const FString Base = Unique;
        // Not a package in memory or on disk (another level cooked to the same root in an earlier session).
        for (int32 Suffix = 2; FindPackage(nullptr, *(Folder / Unique)) || FPackageName::DoesPackageExist(Folder / Unique); ++Suffix)
            Unique = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
        UPackage* Package = CreatePackage(*(Folder / Unique));
        Object->Rename(*Unique, Package, REN_DontCreateRedirectors | REN_NonTransactional);
        Object->SetFlags(RF_Public | RF_Standalone);
        MakePersistent(Object);
        Package->MarkPackageDirty();
        ToSave.Add(Package);
        Remap.Add(Object, Object);
    }
    /** The pooled asset for a source key: an existing one (this run or on disk), else this object moved into the pool. */
    UObject* Pool(UObject* Object, const FString& Key, const FString& Folder)
    {
        if (!Object) return nullptr;
        if (UObject** Found = Remap.Find(Object)) return *Found;
        const FString Name = SafeName(FPaths::GetBaseFilename(Key.Left(Key.Find(TEXT("#"))).IsEmpty() ? Key : Key.Left(Key.Find(TEXT("#"))))) +
                             TEXT("_") + FMD5::HashAnsiString(*Key).Left(8);
        const FString PackageName = SharedRoot / Folder / Name;
        const FString ObjectPath = PackageName + TEXT(".") + Name;
        UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath);
        if (!Existing && FPackageName::DoesPackageExist(PackageName)) Existing = LoadObject<UObject>(nullptr, *ObjectPath);
        if (Existing)
        {
            ++Reused;
            Remap.Add(Object, Existing);
            return Existing;
        }
        if (auto* Texture = Cast<UTexture2D>(Object)) AddTextureSource(Texture);
        UPackage* Package = CreatePackage(*PackageName);
        Object->Rename(*Name, Package, REN_DontCreateRedirectors | REN_NonTransactional);
        Object->SetFlags(RF_Public | RF_Standalone);
        MakePersistent(Object);
        Package->MarkPackageDirty();
        ToSave.Add(Package);
        ++Created;
        Remap.Add(Object, Object);
        return Object;
    }
    /** Constant copy of a dynamic instance under Root/Materials, textures pointed at the pool. */
    UMaterialInterface* Constant(UMaterialInterface* Material)
    {
        auto* MID = Cast<UMaterialInstanceDynamic>(Material);
        if (!MID) return Material;
        if (UObject** Found = Remap.Find(MID)) return Cast<UMaterialInterface>(*Found);
        FString Name = SafeName(TEXT("MI_") + MID->GetName());
        const FString Base = Name;
        for (int32 Suffix = 2; FindPackage(nullptr, *(Root / TEXT("Materials") / Name)) || FPackageName::DoesPackageExist(Root / TEXT("Materials") / Name); ++Suffix)
            Name = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
        UPackage* Package = CreatePackage(*(Root / TEXT("Materials") / Name));
        auto* MIC = NewObject<UMaterialInstanceConstant>(Package, *Name, RF_Public | RF_Standalone);
        MIC->SetParentEditorOnly(Constant(MID->Parent));
        // The glass refraction switch is never baked: a cooked glass keeps the master default (on, the viewer's look since
        // 2026-11-19; the session diagnostic sc.Look.GlassRefraction 0 reaches it through a dynamic instance at load).
        for (const FScalarParameterValue& P : MID->ScalarParameterValues)
            if (P.ParameterInfo.Name != FSCMaterialBuilder::GlassRefractionLookParam) MIC->SetScalarParameterValueEditorOnly(P.ParameterInfo, P.ParameterValue);
        for (const FVectorParameterValue& P : MID->VectorParameterValues) MIC->SetVectorParameterValueEditorOnly(P.ParameterInfo, P.ParameterValue);
        for (const FTextureParameterValue& P : MID->TextureParameterValues)
        {
            UTexture* Texture = P.ParameterValue;
            if (UObject** Pooled = Texture ? Remap.Find(Texture) : nullptr) Texture = Cast<UTexture>(*Pooled);
            MIC->SetTextureParameterValueEditorOnly(P.ParameterInfo, Texture);
        }
        MIC->PostEditChange();
        Package->MarkPackageDirty();
        ToSave.Add(Package);
        Remap.Add(MID, MIC);
        return MIC;
    }
};
} // namespace

bool USCCookLibrary::CookEnvironment(ASCVehicle* Environment, const FString& Root, const FString& SharedRoot, const FString& SourceBuild,
                                     bool bKeepRigs)
{
    if (!Environment) return false;
    const double Start = FPlatformTime::Seconds();
    FCooker Cook;
    Cook.Root = Root;
    Cook.SharedRoot = SharedRoot;
    int32 RigMeshes = 0, RigCollisions = 0;
    // A lift is saved at rest (WP-O1-4b-1b-2): its doors' poses and recorded states are saved, the clips moving them are
    // not, so a lift mid-leg or mid-handshake would load with its doors and its state disagreeing. The prep step presents
    // the vehicle at rest; a lift that is not fails the cook before anything of the actor changes.
    if (bKeepRigs)
    {
        if (!Environment->LiftsAtRest())
        {
            for (const TSharedPtr<FSCLift>& Lift : Environment->Lifts)
                if (Lift) UE_LOG(LogSCCook, Error, TEXT("COOK_REFUSED lift %d (%s): %s"), Lift->Id, *Lift->EntityName, *Lift->Describe());
            UE_LOG(LogSCCook, Error, TEXT("COOK_REFUSED the vehicle is not at rest (a lift moving, or a clip playing: %d): lifts are saved idle at a stop"),
                   Environment->Playback.Num());
            return false;
        }
    }
    if (!bKeepRigs)
    {
        FSCEnvironment::BakeRigsStatic(Environment);
        // Baked static: no rig moves, so no lift either (its doors and carriage are geometry now).
        Environment->Lifts.Reset();
        Environment->SavedLifts.Reset();
        Environment->LiftPanels.Reset();
    }
    else
    {
        // Each rig mesh the actor references (its meshes, the clip bindings', the rigs': one whose placement was skipped is
        // still bound to clips) is saved as a savable copy with a copy of its skeleton, named after its source (the import's
        // package), and the actor is moved onto the copies: the import's meshes stay as they are for other vehicles
        // assembled from the same retained assets (a rebuilt mesh would no longer match their collision).
        TArray<USkeletalMesh*> Meshes;
        for (USkeletalMesh* Mesh : Environment->SkeletalMeshes)
            if (Mesh) Meshes.AddUnique(Mesh);
        if (const USCClipCatalog* Catalog = Environment->ClipCatalog)
        {
            for (const FSCImportedClip& Clip : Catalog->Clips)
                for (const FSCClipBinding& Binding : Clip.Bindings)
                    if (USkeletalMesh* Mesh = Binding.Mesh.Get()) Meshes.AddUnique(Mesh);
            if (USkeletalMesh* Root = Catalog->RootMesh.Get()) Meshes.AddUnique(Root);
        }
        for (const USCRigComponent* Rig : Environment->Rigs)
            if (USkeletalMesh* Mesh = Rig ? Rig->GetSkeletalMeshAsset() : nullptr) Meshes.AddUnique(Mesh);
        TMap<const USkeletalMesh*, USkeletalMesh*> Copies;
        for (USkeletalMesh* Mesh : Meshes)
        {
            const FString Name = SafeName(FPaths::GetBaseFilename(Mesh->GetPackage()->GetName()));
            // An owning copy: the map grows below (an element passed by reference into its own Add could be moved).
            const TSharedPtr<const FSCDerivedMesh> Source = Environment->RigSources.FindRef(Mesh);
            FString Error;
            USkeletalMesh* Copy = Source ? SCRigAssets::SavableCopy(Mesh, Source.Get(), Error) : nullptr;
            if (!Copy)
            {
                Cook.Warnings.Add(Name + TEXT(": ") + (Source ? Error : FString(TEXT("no rig source (FSCImportOptions::bRetainRigSources)"))));
                Cook.bFailed = true;
                continue;
            }
            Cook.Persist(Copy->GetSkeleton(), Root / TEXT("Rigs"), Name + TEXT("_Skeleton"));
            Cook.Persist(Copy, Root / TEXT("Rigs"), Name);
            Copies.Add(Mesh, Copy);
            Environment->RigSources.Add(Copy, Source);
            ++RigMeshes;
        }
        for (TObjectPtr<USkeletalMesh>& Mesh : Environment->SkeletalMeshes)
            if (USkeletalMesh** Copy = Copies.Find(Mesh)) Mesh = *Copy;
        if (USCClipCatalog* Catalog = Environment->ClipCatalog) // the actor's own catalog (its clips copied at assembly)
        {
            for (FSCImportedClip& Clip : Catalog->Clips)
                for (FSCClipBinding& Binding : Clip.Bindings)
                    if (USkeletalMesh** Copy = Copies.Find(Binding.Mesh.Get())) Binding.Mesh = *Copy;
            USkeletalMesh** Root = Copies.Find(Catalog->RootMesh.Get());
            Catalog->RootMesh = Root ? *Root : nullptr;
        }
        // Each rig on its copy, in the pose it holds (LocalPose); a soft rig of an import that built collision has a body:
        // again in the copy's vertex order, also when the import's own rig collision failed, so a saved rig never lacks it.
        for (USCRigComponent* Rig : Environment->Rigs)
        {
            USkeletalMesh** Copy = Rig ? Copies.Find(Rig->GetSkeletalMeshAsset()) : nullptr;
            if (!Copy) continue;
            Rig->SetSkeletalMesh(*Copy, /*bReinitPose*/ false);
            Rig->RefreshBoneTransforms();
            if (!Environment->bCollisionBuilt) continue;
            const TSharedPtr<const FSCDerivedMesh> Source = Environment->RigSources.FindRef(*Copy);
            if (Source && Source->bHasGeometry)
            {
                FString Error;
                if (!SCRigAssets::RefreshCollision(Rig, Error))
                {
                    Cook.Warnings.Add(Rig->OwnerEntity + TEXT(": rig collision: ") + Error);
                    Cook.bFailed = true;
                    continue;
                }
                ++RigCollisions;
            }
        }
        // Every soft rig has its body now (a failure failed the cook): the collision of an import that built it is ready,
        // also when the import's own rig collision had failed (first person explores only a ready vehicle).
        if (!Cook.bFailed && Environment->bCollisionBuilt) Environment->bCollisionReady = true;
        // Playback reads the bone tracks; the sequences are editor-only views of them, built against transient skeletons.
        if (USCClipCatalog* Catalog = Environment->ClipCatalog)
            for (FSCImportedClip& Clip : Catalog->Clips)
            {
                Clip.Sequence = nullptr;
                for (FSCClipBinding& Binding : Clip.Bindings) Binding.Sequences.Reset();
            }
    }
    const FSCInstancingStats Instancing = FSCInstancing::Consolidate(Environment); // pieces freed from the rigs
    // Runtime-only proxies (emissive Lumen copies with generated meshes) are rebuilt when the level loads.
    TArray<UActorComponent*> Proxies = Environment->GetComponentsByTag(UPrimitiveComponent::StaticClass(), TEXT("SCEmissiveExperiment"));
    for (UActorComponent* Proxy : Proxies) Proxy->DestroyComponent();

    // An asset without a content id is told apart by the archive build and the cook format at least.
    const FString Fallback = FString::Printf(TEXT("build=%s;format=%d"), *SourceBuild, USCCookLibrary::FormatVersion);
    for (const auto& [Object, SourceKey] : Environment->SourcePaths)
        if (const FString Key = SourceKey + TEXT("#content=") + FMD5::HashAnsiString(*ContentId(Object, Fallback)).Left(12);
            const auto* Mesh = Cast<UStaticMesh>(Object))
        {
            // The pool key carries the mesh's build (Nanite or conventional) and section topology (slot, triangles): a
            // source asset split or built differently (a screen or glass slot moved between the Nanite and the translucent
            // variant, a whole mesh turned translucent) is a different pooled mesh.
            FString Topology = Mesh->IsNaniteEnabled() ? TEXT("nanite;") : TEXT("conventional;");
            if (const FStaticMeshRenderData* Render = Mesh->GetRenderData(); Render && !Render->LODResources.IsEmpty())
                for (const FStaticMeshSection& S : Render->LODResources[0].Sections) Topology += FString::Printf(TEXT("%u:%u;"), S.MaterialIndex, S.NumTriangles);
            Cook.Pool(Object, Key + TEXT("#topology=") + FMD5::HashAnsiString(*Topology).Left(8), TEXT("Meshes"));
        }
        else if (Object)
        {
            // The exporter's generated UI images keep their path when what they render changes (a renderer fix): their
            // pool key carries the cook format, so a recook takes the new render instead of the pooled older one.
            const bool bGeneratedUi = Key.Contains(TEXT("/UI/Generated/"));
            Cook.Pool(Object, bGeneratedUi ? Key + FString::Printf(TEXT("#cookformat=%d"), USCCookLibrary::FormatVersion) : Key, TEXT("Textures"));
        }
    TArray<UStaticMeshComponent*> MeshComponents;
    Environment->GetComponents(MeshComponents);
    for (UStaticMeshComponent* C : MeshComponents)
    {
        if (UObject** Pooled = C->GetStaticMesh() ? Cook.Remap.Find(C->GetStaticMesh()) : nullptr)
            if (*Pooled != C->GetStaticMesh()) C->SetStaticMesh(Cast<UStaticMesh>(*Pooled));
        for (int32 Slot = 0; Slot < C->OverrideMaterials.Num(); ++Slot)
            if (C->OverrideMaterials[Slot]) C->SetMaterial(Slot, Cook.Constant(C->OverrideMaterials[Slot]));
    }
    // Kept rigs: their slot materials become constants as well.
    TArray<USkeletalMeshComponent*> SkinnedComponents;
    Environment->GetComponents(SkinnedComponents);
    for (USkeletalMeshComponent* C : SkinnedComponents)
        for (int32 Slot = 0; Slot < C->OverrideMaterials.Num(); ++Slot)
            if (C->OverrideMaterials[Slot]) C->SetMaterial(Slot, Cook.Constant(C->OverrideMaterials[Slot]));
    // Panels and screens (WP-O1-4b-1c). A drawn panel's render target is an asset of its own (a constant material in its
    // own package refers to it; what it shows is drawn again at load); the screen faces' state, hover and custom renders
    // become constants; the panels' renders and label masks are their pooled textures.
    int32 PanelTargets = 0;
    for (const TObjectPtr<UTextureRenderTarget2D>& Target : Environment->FloorPanelTargets)
        if (Target)
        {
            Cook.Persist(Target, Root / TEXT("Panels"), Target->GetName());
            ++PanelTargets;
        }
    for (TObjectPtr<UMaterialInterface>& Material : Environment->FloorPanelMaterials)
        if (Material) Material = Cook.Constant(Material);
    auto MakeConstant = [&Cook](TObjectPtr<UMaterialInterface>& Material) {
        if (Material) Material = Cook.Constant(Material);
    };
    for (FSCScreenFace& Face : Environment->ScreenFaces)
    {
        MakeConstant(Face.Default);
        MakeConstant(Face.Custom);
        for (auto& [Key, Material] : Face.States) MakeConstant(Material);
        for (auto& [Key, Material] : Face.Hovers) MakeConstant(Material);
    }
    auto Pooled = [&Cook](TObjectPtr<UTexture2D>& Texture) {
        if (UObject** Asset = Texture ? Cook.Remap.Find(Texture) : nullptr) Texture = Cast<UTexture2D>(*Asset);
    };
    // The light cookies a later light function is made from (a personal light's, FSCLightImporter::GoboLightFunction).
    for (auto& [Path, Texture] : Environment->GoboTextures) Pooled(Texture);
    for (TArray<FSCFloorPanel>* Panels : {&Environment->FloorPanels, &Environment->DoorPanelLocks})
        for (FSCFloorPanel& Panel : *Panels)
        {
            for (TObjectPtr<UTexture2D>& Mask : Panel.Masks) Pooled(Mask);
            for (auto& [Path, Image] : Panel.Images) Pooled(Image);
        }
    TArray<ULightComponent*> Lights;
    Environment->GetComponents(Lights);
    for (ULightComponent* L : Lights)
        if (L->LightFunctionMaterial) L->SetLightFunctionMaterial(Cook.Constant(L->LightFunctionMaterial));
    // The actor and its components persist with the level. Runtime-only objects the actor owns (clip animations of the
    // baked rigs, skeletal meshes, dynamic materials now replaced by constants) stay transient and are unreferenced, so
    // they are not saved (a saved clip without its skeleton fails to load).
    if (!bKeepRigs)
    {
        Environment->ClipCatalog = nullptr;
        Environment->SkeletalMeshes.Empty();
        Environment->WaypointSources.Reset(); // nothing moves: the waypoints saved in the baked pose stay
    }
    else if (Environment->ClipCatalog)
    {
        Environment->ClipCatalog->ClearFlags(RF_Transient);
    }
    Environment->Materials.Empty();
    Environment->EngineGlowMaterials.Empty();
    Environment->ClearFlags(RF_Transient);
    for (UActorComponent* C : Environment->GetComponents()) Cook.MakePersistent(C);

    int32 Saved = 0;
    for (UPackage* Package : Cook.ToSave)
    {
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        if (UPackage::SavePackage(Package, Package->FindAssetInPackage(), *File, Args)) ++Saved;
        else Cook.Warnings.Add(TEXT("save failed: ") + Package->GetName());
    }
    for (const FString& W : Cook.Warnings) UE_LOG(LogSCCook, Warning, TEXT("COOK_WARNING %s"), *W);
    UE_LOG(LogSCCook, Display, TEXT("COOK root=%s pooled_new=%d pooled_reused=%d packages=%d saved=%d instanced=%d rigs_kept=%d rig_meshes=%d rig_collisions=%d warnings=%d seconds=%.1f"), *Root,
           Cook.Created, Cook.Reused, Cook.ToSave.Num(), Saved, Instancing.Instanced, bKeepRigs ? Environment->Rigs.Num() : 0, RigMeshes, RigCollisions,
           Cook.Warnings.Num(), FPlatformTime::Seconds() - Start);
    return Saved == Cook.ToSave.Num() && !Cook.bFailed;
}

bool USCCookLibrary::RecordHangarCapacity(ASCVehicle* Environment, const FString& Style, const FString& Name, bool& bOutMeasured)
{
    FBox Bay;
    FString Elevator;
    bOutMeasured = FSCEnvironment::FindShipBay(Environment, Bay, &Elevator);
    if (!bOutMeasured)
    {
        // No measurement describes this hangar now: an older entry goes (hangar fitting must not use it).
        USCHangarIndex* Index = USCHangarIndex::Load();
        const int32 Removed = Index ? Index->Hangars.RemoveAll([&](const FSCHangarCapacity& H) { return H.Style == Style && H.Name == Name; }) : 0;
        UE_LOG(LogSCCook, Warning, TEXT("HANGAR_CAPACITY %s/%s: no ship elevator (nothing recorded, %d older entries removed)"), *Style, *Name, Removed);
        if (!Removed) return true;
        Index->GetPackage()->MarkPackageDirty();
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;
        const FString PackageName = Index->GetPackage()->GetName();
        return UPackage::SavePackage(Index->GetPackage(), Index, *FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), Args);
    }
    const double Clearance = FSCEnvironment::BayClearance(Environment, Bay);
    const FString PackageName = FPackageName::ObjectPathToPackageName(FString(USCHangarIndex::AssetPath));
    USCHangarIndex* Index = USCHangarIndex::Load();
    UPackage* Package = Index ? Index->GetPackage() : CreatePackage(*PackageName);
    if (!Index)
        Index = NewObject<USCHangarIndex>(Package, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone);
    FSCHangarCapacity& Entry = Index->FindOrAdd(Style, Name);
    Entry.BayCm = FVector2D(Bay.GetSize());
    Entry.ClearanceCm = Clearance;
    Entry.Elevator = Elevator;
    Entry.bTopLoading = FSCEnvironment::IsTopLoading(Environment, Bay);
    Package->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    const bool bSaved = UPackage::SavePackage(Package, Index, *FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), Args);
    UE_LOG(LogSCCook, Display, TEXT("HANGAR_CAPACITY %s/%s elevator=%s bay_m=%.1fx%.1f clearance_m=%.1f top_loading=%d saved=%d hangars=%d"), *Style, *Name,
           *Elevator, Entry.BayCm.X / 100, Entry.BayCm.Y / 100, Clearance / 100, Entry.bTopLoading, bSaved, Index->Hangars.Num());
    return bSaved;
}
