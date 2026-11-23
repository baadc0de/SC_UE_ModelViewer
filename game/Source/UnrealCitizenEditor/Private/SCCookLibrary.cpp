// USCCookLibrary: environment -> persistent level actor with pooled assets (WP-C1).
#include "SCCookLibrary.h"
#include "SCRigAssets.h"
#include "SCRigComponent.h"
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
    /** Runtime textures carry only platform mips (the package's DDS blocks as uploaded, or BGRA8 from PNG): an editable
     *  BGRA8 source from mip 0, block formats decoded by the bridge (sc_texels_decode_bgra8). */
    void AddTextureSource(UTexture2D* Texture)
    {
        FTexturePlatformData* Platform = Texture->GetPlatformData();
        if (!Platform || Platform->Mips.IsEmpty())
        {
            Warnings.Add(Texture->GetName() + TEXT(": no platform mips; saved without source"));
            return;
        }
        uint32 Dxgi = 0;
        switch (Platform->PixelFormat)
        {
        case PF_B8G8R8A8: Dxgi = 87; break;
        case PF_R8G8B8A8: Dxgi = 28; break;
        case PF_G8: Dxgi = 61; break;
        case PF_DXT1: Dxgi = 71; break;
        case PF_DXT3: Dxgi = 74; break;
        case PF_DXT5: Dxgi = 77; break;
        case PF_BC4: Dxgi = 80; break;
        case PF_BC5: Dxgi = 83; break;
        case PF_BC7: Dxgi = 98; break;
        default:
            Warnings.Add(FString::Printf(TEXT("%s: pixel format %s has no source decode; saved without source"), *Texture->GetName(),
                                         GPixelFormats[Platform->PixelFormat].Name));
            return;
        }
        FTexture2DMipMap& Mip = Platform->Mips[0];
        const uint8* Data = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
        TArray64<uint8> Bgra;
        Bgra.SetNumUninitialized(int64(Mip.SizeX) * Mip.SizeY * 4);
        const bool bOK = Data && sc_texels_decode_bgra8(Data, size_t(Mip.BulkData.GetBulkDataSize()), Dxgi, uint32(Mip.SizeX), uint32(Mip.SizeY),
                                                        Bgra.GetData(), size_t(Bgra.Num())) == SC_STATUS_OK;
        Mip.BulkData.Unlock();
        if (bOK)
            Texture->Source.Init(Mip.SizeX, Mip.SizeY, 1, 1, TSF_BGRA8, Bgra.GetData());
        else
            Warnings.Add(FString::Printf(TEXT("%s: decode failed (%s); saved without source"), *Texture->GetName(), UTF8_TO_TCHAR(sc_last_error(nullptr))));
    }
    bool bFailed = false;
    /** Object moved into a package of its own, Folder/Name (made unique), saved with the cook. */
    void Persist(UObject* Object, const FString& Folder, const FString& Name)
    {
        if (!Object || Remap.Contains(Object)) return;
        FString Unique = SafeName(Name);
        const FString Base = Unique;
        for (int32 Suffix = 2; FindPackage(nullptr, *(Folder / Unique)); ++Suffix) Unique = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
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
        for (int32 Suffix = 2; FindPackage(nullptr, *(Root / TEXT("Materials") / Name)); ++Suffix) Name = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
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
    if (!bKeepRigs) FSCEnvironment::BakeRigsStatic(Environment);
    else
    {
        // Each rig mesh (shared by the rigs that use it) becomes an asset with its skeleton; a rig with collision gets it
        // again in the built vertex order.
        TSet<USkeletalMesh*> Made;
        for (USCRigComponent* Rig : Environment->Rigs)
        {
            USkeletalMesh* Mesh = Rig ? Rig->GetSkeletalMeshAsset() : nullptr;
            if (!Mesh) continue;
            if (!Made.Contains(Mesh))
            {
                Made.Add(Mesh);
                const TSharedPtr<const FSCDerivedMesh>* Source = Environment->RigSources.Find(Mesh);
                FString Error;
                if (!Source || !SCRigAssets::MakeSavable(Mesh, **Source, Error))
                {
                    Cook.Warnings.Add(Rig->MeshAsset + TEXT(": ") + (Source ? Error : FString(TEXT("no rig source (FSCImportOptions::bRetainRigSources)"))));
                    Cook.bFailed = true;
                    continue;
                }
                const FString Name = SafeName(FPaths::GetBaseFilename(Rig->MeshAsset));
                Cook.Persist(Mesh->GetSkeleton(), Root / TEXT("Rigs"), Name + TEXT("_Skeleton"));
                Cook.Persist(Mesh, Root / TEXT("Rigs"), Name);
                ++RigMeshes;
            }
            // A soft rig has collision (the prep import builds it): again in the built vertex order, also when the import's
            // own collision failed, so a saved rig never lacks it.
            const TSharedPtr<const FSCDerivedMesh>* Source = Environment->RigSources.Find(Mesh);
            if (Source && (*Source)->bHasGeometry)
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
