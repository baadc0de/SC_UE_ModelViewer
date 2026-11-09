#include "SCImporter.h"
#include "SCVehicle.h"
#include "SCPackageReader.h"
#include "SCImportMaterials.h"
#include "Async/Async.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/Material.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "RenderingThread.h"
#include "StaticMeshResources.h"

DEFINE_LOG_CATEGORY(LogSCImport);
namespace
{
TMap<TWeakObjectPtr<UWorld>, TWeakObjectPtr<ASCVehicle>> Vehicles;
FString SafeName(FString Name)
{
    for (TCHAR& C : Name) if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    return Name.IsEmpty() ? TEXT("Object") : Name;
}
FString AssetPackage(const FString& Session, const FString& Path)
{
    TArray<FString> Parts; FPaths::ChangeExtension(Path, TEXT("")).ParseIntoArray(Parts, TEXT("/"), true);
    for (FString& Part : Parts) Part = SafeName(Part);
    return Session / FString::Join(Parts, TEXT("/"));
}
struct FEntity
{
    USceneComponent* Anchor = nullptr;
    const FSCDerivedMesh* Mesh = nullptr;
    TMap<FString, USceneComponent*> Helpers;
};
int32 StaticMeshCount()
{
    int32 Count = 0;
    for (TObjectIterator<UStaticMesh> It; It; ++It) if (!It->IsTemplate()) ++Count;
    return Count;
}
}
bool FSCImporter::Import(FSCPackageSource& Source, FSCImportOptions Options, FSCImportResult& Result)
{
    check(IsInGameThread()); Result = FSCImportResult();
    const double Start = FPlatformTime::Seconds();
    if (!Options.World) { Result.Error = TEXT("Import requires a world"); return false; }
    FSCSceneManifest Manifest;
    if (!Async(EAsyncExecution::ThreadPool, [&] { return FSCPackageReader::Read(Source, Manifest, Result.Error, Options.ScenePath, &Result.Warnings); }).Get()) return false;
    const FString Session = TEXT("/Temp/SC/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FActorSpawnParameters Spawn; Spawn.ObjectFlags |= RF_Transient;
    ASCVehicle* Vehicle = Options.World->SpawnActor<ASCVehicle>(ASCVehicle::StaticClass(), Options.ActorTransform, Spawn);
    if (!Vehicle) { Result.Error = TEXT("Cannot spawn vehicle"); return false; }
    Vehicle->Manifest = Manifest; Vehicle->SessionPath = Session;
    Vehicle->ClipCatalog = NewObject<USCClipCatalog>(Vehicle); Vehicle->ClipCatalog->Clips = Manifest.Clips;
    auto Warn = [&](const FString& Path, const FString& Message)
    {
        Result.Warnings.Add(Path + TEXT(": ") + Message);
        UE_LOG(LogSCImport, Warning, TEXT("ASSET_WARNING %s: %s"), *Path, *Message);
    };
    for (const FString& Warning : Result.Warnings) UE_LOG(LogSCImport, Warning, TEXT("ASSET_WARNING %s"), *Warning);
    // Material preparation (describe + texture decode) runs on a worker while meshes are derived and built;
    // textures and MIDs are created on this thread once it is done, before components are placed.
    // TODO(WP-V1): async import API with per-phase progress (this call still returns only when complete).
    FSCImportMaterials Materials;
    TFuture<void> MaterialPrep;
    const double MaterialStart = FPlatformTime::Seconds();
    if (Options.bImportMaterials)
    {
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
        MaterialPrep = Async(EAsyncExecution::ThreadPool, [&Materials, &Manifest, &Source] { Materials.Describe(Manifest); Materials.Decode(Source); });
    }
    TMap<FString, TUniquePtr<FSCDerivedMesh>> Derived;
    TMap<FString, TArray<UStaticMesh*>> Meshes;
    TMap<FString, TSet<int32>> MeshTranslucentSlots;
    TMap<FString, TArray<int32>> MeshSlotTriangles; // host-paint weighting for POM decal tint
    TMultiMap<FString, FString> Sidecars;
    auto CollectSidecar = [&](const FSCSceneInstance& I) { Sidecars.AddUnique(I.MeshAsset, I.MaterialSidecar); };
    CollectSidecar(Manifest.Root); for (const auto& I : Manifest.Children) CollectSidecar(I);
    for (const auto& Interior : Manifest.Interiors) for (const auto& I : Interior.Placements) CollectSidecar(I);
    for (const FString& Path : Manifest.MeshAssets())
    {
        auto Data = MakeUnique<FSCDerivedMesh>(); FString AssetError;
        const double DeriveStart = FPlatformTime::Seconds();
        // TODO(WP-V1): async import API with per-phase progress; prepare all unique meshes
        // in parallel, then batch async compilation and representation completion.
        if (!Async(EAsyncExecution::ThreadPool, [&] { return FSCMeshImporter::Derive(Source, Path, *Data, AssetError)
            && (!Options.bBuildCollision || FSCMeshImporter::DeriveCollision(*Data, AssetError)); }).Get())
        { Warn(Path, AssetError); continue; }
        const double DeriveSeconds = FPlatformTime::Seconds() - DeriveStart;
        for (const auto& Warning : Data->Warnings) Warn(Path, Warning);
        TSet<int32> TranslucentSlots = Data->BlendedSlots;
        auto IsTranslucent = [](const FString& Family)
        { return Family.StartsWith(TEXT("Glass"), ESearchCase::IgnoreCase) || Family.Equals(TEXT("MeshDecal"), ESearchCase::IgnoreCase); };
        for (int32 Slot = 0; Slot < Data->MaterialFamilies.Num(); ++Slot)
            if (IsTranslucent(Data->MaterialFamilies[Slot])) TranslucentSlots.Add(Slot);
        TArray<FString> Paths; Sidecars.MultiFind(Path, Paths);
        for (const auto& Sidecar : Paths)
        {
            const auto* Set = Manifest.Materials.FindByPredicate([&](const FSCMaterialSet& M) { return M.Path == Sidecar; });
            if (!Set) continue;
            for (int32 Slot = 0; Slot < Data->MaterialNames.Num(); ++Slot)
                if (const FSCSubmaterial* Sub = FSCImportMaterials::Resolve(Set, Data->MaterialSourceNames, Slot))
                {
                    const bool bGlowTarget = Manifest.EngineGlow.Targets.ContainsByPredicate([&](const FSCEngineGlowTarget& T)
                        { return T.MaterialSidecar == Sidecar && T.SourceMaterialIndex == Sub->SourceIndex; });
                    if (FSCMaterialBuilder::RequiresNonNanite(*Sub, bGlowTarget)) TranslucentSlots.Add(Slot); // D6, decided by the builder
                }
        }
        TArray<int32>& SlotTriangles = MeshSlotTriangles.Add(Path);
        SlotTriangles.SetNumZeroed(Data->MaterialNames.Num());
        for (auto T : Data->Description.Triangles().GetElementIDs())
            if (SlotTriangles.IsValidIndex(Data->Description.GetTrianglePolygonGroup(T).GetValue())) ++SlotTriangles[Data->Description.GetTrianglePolygonGroup(T).GetValue()];
        FSCDerivedMesh Translucent;
        FSCMeshImporter::SplitTranslucent(*Data, TranslucentSlots, Translucent);
        UPackage* Package = CreatePackage(*AssetPackage(Session, Path)); Package->SetFlags(RF_Transient);
        TArray<UStaticMesh*> Variants;
        auto Build = [&](const FSCDerivedMesh& Variant, const FString& Suffix, bool bCollision)
        {
            FSCMeshBuildStats Stats;
            UStaticMesh* Mesh = FSCMeshImporter::Instantiate(Variant, Package, FName(*(SafeName(FPaths::GetBaseFilename(Path)) + Suffix)), bCollision,
                UMaterial::GetDefaultMaterial(MD_Surface), Stats, AssetError);
            if (!Mesh) { Warn(Path + Suffix, AssetError); return false; }
            Vehicle->Meshes.Add(Mesh); Variants.Add(Mesh); Result.MeshStats.Add(Path + Suffix, Stats);
            UE_LOG(LogSCImport, Display, TEXT("MESH path=%s%s hash=%s triangles=%d degenerate=%d nanite=%s clusters=%d fallback=%d cards=%d mesh_cards=%d distance_field=%d rt_geometry=%d collision=%d collision_triangles=%d derive_s=%.4f build_s=%.4f representation_wait_s=%.4f cook_s=%.4f skins_static=%d"),
                *Path, *Suffix, *Data->ContentHash, Stats.Triangles, Data->DegenerateTriangles, *Stats.NaniteReason, Stats.Clusters,
                Stats.FallbackTriangles, Stats.Cards, Stats.bMeshCards, Stats.bDistanceField, Stats.bRayTracingGeometry, Stats.bCollision, Stats.CollisionTriangles, DeriveSeconds, Stats.BuildSeconds,
                Stats.RepresentationSeconds, Stats.CollisionSeconds, Data->bHasSkins);
            return true;
        };
        if (!Build(*Data, TEXT(""), Options.bBuildCollision)) continue;
        if (Translucent.bHasGeometry) Build(Translucent, TEXT("_Translucent"), false);
        Result.ContentHashes.Add(Path, Data->ContentHash); Meshes.Add(Path, MoveTemp(Variants)); MeshTranslucentSlots.Add(Path, TranslucentSlots);
        // Assembly needs helper matrices only. Release CPU geometry and serialized BVH now.
        Data->Description = FMeshDescription(); Data->CollisionBytes.Empty();
        // TODO(WP-C1): bound source-description lifetime retained internally by UStaticMesh;
        // cache CPU derivations independently of transient render UObjects.
        Derived.Add(Path, MoveTemp(Data));
    }
    if (Options.bImportMaterials)
    {
        const double WaitStart = FPlatformTime::Seconds();
        MaterialPrep.Wait();
        const double WaitSeconds = FPlatformTime::Seconds() - WaitStart;
        TArray<FString> MaterialWarnings;
        Materials.Instantiate(Vehicle, MaterialWarnings);
        for (const FString& W : MaterialWarnings) Warn(TEXT("material"), W);
        const auto& T = Materials.TextureStats;
        Result.MaterialSeconds = FPlatformTime::Seconds() - MaterialStart;
        Result.TextureCount = T.Created; Result.MaterialCount = Materials.MaterialCount;
        UE_LOG(LogSCImport, Display, TEXT("TEXTURES requested=%d created=%d failed=%d bytes=%lld decode_s=%.4f create_s=%.4f"),
            T.Requested, T.Created, T.Failed, T.Bytes, T.DecodeSeconds, T.CreateSeconds);
        FString Masters;
        for (const auto& Pair : Materials.MasterCounts) Masters += FString::Printf(TEXT(" %s=%d"), FSCMaterialBuilder::MasterName(Pair.Key), Pair.Value);
        UE_LOG(LogSCImport, Display, TEXT("MATERIALS sets=%d instances=%d engine_glow=%d describe_s=%.4f mid_s=%.4f overlapped_s=%.4f wait_s=%.4f masters:%s"),
            Materials.SetCount, Materials.MaterialCount, Materials.EngineGlowCount, Materials.DescribeSeconds, Materials.MaterialSeconds,
            Result.MaterialSeconds, WaitSeconds, *Masters);
    }
    TMap<FString, FEntity> Entities;
    auto NewAnchor = [&](const FString& Label, USceneComponent* Parent, const FMatrix& Relative)
    {
        auto* C = NewObject<USceneComponent>(Vehicle, MakeUniqueObjectName(Vehicle, USceneComponent::StaticClass(), FName(*SafeName(Label))), RF_Transient);
        Vehicle->AddInstanceComponent(C); C->SetMobility(EComponentMobility::Movable); C->SetupAttachment(Parent);
        C->SetRelativeTransform(FTransform(Relative)); C->RegisterComponent(); return C;
    };
    auto FindParent = [&](const FSCSceneInstance& I, bool& bHelper) -> USceneComponent*
    {
        bHelper = false;
        if (I.ParentEntityName.IsEmpty()) return Vehicle->GetRootComponent();
        FEntity* E = Entities.Find(I.ParentEntityName);
        if (!E || !E->Anchor) { Warn(I.EntityName, TEXT("Unresolved parent entity: ") + I.ParentEntityName); return nullptr; }
        if (I.ParentNodeName.IsEmpty()) return E->Anchor;
        if (auto* Existing = E->Helpers.Find(I.ParentNodeName)) { bHelper = true; return *Existing; }
        const FMatrix* Node = E->Mesh ? E->Mesh->Nodes.Find(I.ParentNodeName) : nullptr;
        if (!Node && E->Mesh)
            for (const auto& Pair : E->Mesh->Nodes) if (Pair.Key.Equals(I.ParentNodeName, ESearchCase::IgnoreCase)) { Node = &Pair.Value; break; }
        if (!Node && E->Mesh)
        {
            // The exporter falls back to the entity's source root when a port has no node.
            Node = &E->Mesh->AttachmentRootMatrix;
            UE_LOG(LogSCImport, Display, TEXT("ATTACHMENT_ROOT_FALLBACK entity=%s helper=%s (resolved local transform retained)"), *I.ParentEntityName, *I.ParentNodeName);
        }
        if (!Node) { Warn(I.EntityName, TEXT("Unresolved parent helper: ") + I.ParentEntityName + TEXT("/") + I.ParentNodeName); return nullptr; }
        auto* Helper = NewAnchor(I.ParentNodeName, E->Anchor, FSCMeshImporter::Transform(*Node));
        E->Helpers.Add(I.ParentNodeName, Helper); bHelper = true; return Helper;
    };
    int32 RenderInstances = 0;
    auto Place = [&](const FSCSceneInstance& I, USceneComponent* Parent, bool bAtHelper, bool bRoot) -> FEntity
    {
        FEntity E;
        if (auto* Data = Derived.Find(I.MeshAsset)) E.Mesh = Data->Get();
        const FMatrix Relative = FSCMeshImporter::InstanceTransform(I.LocalMatrix, E.Mesh ? E.Mesh->RootMatrix : FMatrix::Identity,
            bAtHelper, I.SourceTransformBasis == TEXT("cryengine_z_up"));
        E.Anchor = NewAnchor(I.EntityName, Parent, bRoot ? FMatrix::Identity : Relative);
        if (const auto* Variants = Meshes.Find(I.MeshAsset); Variants && E.Mesh && E.Mesh->bHasGeometry)
        {
            ++Result.InstanceCount;
            for (int32 Variant = 0; Variant < Variants->Num(); ++Variant)
            {
            auto* C = NewObject<UStaticMeshComponent>(Vehicle, MakeUniqueObjectName(Vehicle, UStaticMeshComponent::StaticClass(), TEXT("Mesh")), RF_Transient);
            Vehicle->AddInstanceComponent(C); C->SetMobility(EComponentMobility::Movable); C->SetupAttachment(E.Anchor); C->SetStaticMesh((*Variants)[Variant]);
            C->ComponentTags.Add(FName(*I.MeshAsset));
            C->SetCollisionProfileName(Options.bBuildCollision && Variant == 0 ? TEXT("BlockAll") : TEXT("NoCollision"));
            C->SetVisibility(!I.bInvisible); C->SetHiddenInGame(I.bInvisible);
            const UStaticMesh* Mesh = (*Variants)[Variant];
            const TSet<int32>& Translucent = MeshTranslucentSlots.FindRef(I.MeshAsset);
            const bool bNanite = Mesh->IsNaniteEnabled();
            if (!bNanite && !Translucent.IsEmpty())
            {
                // Glass/decal/screen split: lit by Lumen but not part of its scene (no cards, no DF).
                C->bAffectDistanceFieldLighting = false; C->bAffectDynamicIndirectLighting = false; C->CastShadow = false;
            }
            const FSCMaterialSet* Set = Manifest.Materials.FindByPredicate([&](const FSCMaterialSet& M) { return M.Path == I.MaterialSidecar; });
            const FString Host = FSCImportMaterials::HostChannel(Set, E.Mesh->MaterialSourceNames, MeshSlotTriangles.FindRef(I.MeshAsset));
            for (int32 Slot = 0; Set && Slot < E.Mesh->MaterialNames.Num(); ++Slot)
            {
                // Bind only sections this variant draws; Nanite never receives a translucent instance.
                if (bNanite && Translucent.Contains(Slot)) continue;
                const FSCSubmaterial* Sub = FSCImportMaterials::Resolve(Set, E.Mesh->MaterialSourceNames, Slot);
                UMaterialInterface* Material = Sub ? Materials.Find(I.MaterialSidecar, I.PaletteId, Sub->Index, Host) : nullptr;
                // A split (decal/glass/glow) section without its instance is hidden, never drawn opaque.
                if (!Material && !bNanite && Translucent.Contains(Slot)) Material = Materials.Invisible();
                if (Material) C->SetMaterial(Slot, Material);
            }
            C->RegisterComponent(); ++RenderInstances;
            }
        }
        return E;
    };
    auto CanPlace = [&](const FSCSceneInstance& I)
    {
        if (!I.ImportWarning.IsEmpty() || (!I.MeshAsset.IsEmpty() && !Meshes.Contains(I.MeshAsset)))
        { Warn(I.EntityName, TEXT("Instance skipped: ") + (I.ImportWarning.IsEmpty() ? I.MeshAsset : I.ImportWarning)); return false; }
        return true;
    };
    if (CanPlace(Manifest.Root)) Entities.Add(Manifest.Root.EntityName, Place(Manifest.Root, Vehicle->GetRootComponent(), false, true));
    for (const auto& I : Manifest.Children)
    {
        bool bHelper; auto* Parent = FindParent(I, bHelper);
        if (!Parent || !CanPlace(I)) { Entities.Add(I.EntityName, FEntity()); continue; }
        // Export order is depth-first; latest matching entity disambiguates repeated loadout instances.
        Entities.Add(I.EntityName, Place(I, Parent, bHelper, false));
    }
    for (const auto& Interior : Manifest.Interiors)
    {
        bool bHelper; auto* Parent = FindParent(Interior.Container, bHelper);
        if (!Parent) continue;
        const FMatrix W = FSCMeshImporter::CryToGltf();
        auto* Container = NewAnchor(Interior.Container.EntityName, Parent, FSCMeshImporter::Transform(bHelper ? Interior.Container.LocalMatrix : W.Inverse() * Interior.Container.LocalMatrix * W));
        for (const auto& P : Interior.Placements) if (CanPlace(P)) Place(P, Container, bHelper, false);
    }
    if (Options.bReplacePrevious) Unload(Options.World);
    Vehicles.Add(Options.World, Vehicle); Result.Vehicle = Vehicle; Result.MeshCount = Meshes.Num();
    Result.TotalSeconds = FPlatformTime::Seconds() - Start;
    UE_LOG(LogSCImport, Display, TEXT("IMPORT_OK seconds=%.4f assets=%d instances=%d children=%d interiors=%d materials=%d palettes=%d clips=%d game_world=%d render_meshes=%d render_instances=%d textures=%d material_instances=%d material_s=%.4f warnings=%d"),
        Result.TotalSeconds, Result.MeshCount, Result.InstanceCount, Manifest.Children.Num(), Manifest.Interiors.Num(), Manifest.Materials.Num(), Manifest.Palettes.Num(), Manifest.Clips.Num(), Options.World->IsGameWorld(), Vehicle->Meshes.Num(), RenderInstances,
        Result.TextureCount, Result.MaterialCount, Result.MaterialSeconds, Result.Warnings.Num());
    const FBox Bounds = Vehicle->GetComponentsBoundingBox(true);
    UE_LOG(LogSCImport, Display, TEXT("BOUNDS_CM min=%s max=%s size=%s"), *Bounds.Min.ToString(), *Bounds.Max.ToString(), *Bounds.GetSize().ToString());
    return true;
}
void FSCImporter::Unload(UWorld* World)
{
    if (auto* Previous = Vehicles.Find(World)) if (Previous->IsValid()) Previous->Get()->Destroy();
    Vehicles.Remove(World);
}
ASCVehicle* USCImportLibrary::ImportFolder(UObject* WorldContext, const FString& Folder, bool bBuildCollision)
{
    FSCFolderPackageSource Source(Folder); FSCImportOptions Options; Options.World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull); Options.bBuildCollision = bBuildCollision;
    Options.bImportMaterials = !FParse::Param(FCommandLine::Get(), TEXT("SCNoMaterials")); // timing baseline
    FSCImportResult Result;
    if (!FSCImporter::Import(Source, Options, Result)) UE_LOG(LogSCImport, Error, TEXT("IMPORT_FAILED %s"), *Result.Error);
    if (ASCVehicle* Vehicle = Result.Vehicle.Get())
    {
        // Test/UI convenience: keep the complete model above the Viewer floor.
        // 100 cm is display clearance, not a game-data value. C++ Import honors ActorTransform verbatim.
        const FBox Bounds = Vehicle->GetComponentsBoundingBox(true);
        if (Bounds.IsValid) Vehicle->AddActorWorldOffset(FVector(0, 0, 100.0 - Bounds.Min.Z));
    }
    return Result.Vehicle.Get();
}
bool USCImportLibrary::VerifyPackage(UObject* Context, const FString& Folder, const FString& Tar)
{ return FSCImporter::Verify(Folder, Tar, GEngine->GetWorldFromContextObject(Context, EGetWorldErrorMode::ReturnNull)); }
void USCImportLibrary::UnloadPackage(UObject* Context)
{ FSCImporter::Unload(GEngine->GetWorldFromContextObject(Context, EGetWorldErrorMode::ReturnNull)); }

bool FSCImporter::Verify(const FString& Folder, const FString& Tar, UWorld* World)
{
    if (!World) return false;
    FSCFolderPackageSource Disk(Folder); TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Tar)) { UE_LOG(LogSCImport, Error, TEXT("Cannot read verification tar")); return false; }
    FSCTarPackageSource Memory(MoveTemp(Bytes));
    if (!Memory.IsValid()) { UE_LOG(LogSCImport, Error, TEXT("%s"), *Memory.GetError()); return false; }
    // Preload placeholder before baseline; it may lazily load engine dependencies.
    UMaterial::GetDefaultMaterial(MD_Surface);
    Unload(World); FlushRenderingCommands(); CollectGarbage(RF_NoFlags);
    const int32 Baseline = StaticMeshCount();
    GEngine->Exec(World, TEXT("obj list class=StaticMesh"));
    FSCImportOptions Options; Options.World = World; Options.ActorTransform.SetLocation(FVector(0,0,2000));
    FSCImportResult First, Second;
    if (!Import(Disk, Options, First)) { UE_LOG(LogSCImport, Error, TEXT("VERIFY folder failed: %s"), *First.Error); return false; }
    TArray<TWeakObjectPtr<UObject>> Old;
    for (TObjectIterator<UObject> It; It; ++It)
        if (It->IsIn(First.Vehicle.Get()) || *It == First.Vehicle.Get() || It->GetOutermost()->GetName().StartsWith(First.Vehicle->SessionPath)) Old.Add(*It);
    const int32 Loaded = StaticMeshCount();
    bool bRepresentations = true;
    for (const auto& Pair : First.MeshStats)
        if (Pair.Value.Triangles > 0)
            bRepresentations &= (Pair.Value.bNaniteEnabled
                ? Pair.Value.Clusters > 0 && Pair.Value.bDistanceField && Pair.Value.bMeshCards && Pair.Value.bRayTracingGeometry && Pair.Value.bCollision
                : Pair.Value.Clusters == 0) && Pair.Value.Triangles == Pair.Value.FallbackTriangles;
    if (const auto* RootStats = First.MeshStats.Find(First.Vehicle->Manifest.Root.MeshAsset)) bRepresentations &= RootStats->Cards > 0;
    if (First.Vehicle->Manifest.Root.MeshAsset.IsEmpty()) { UE_LOG(LogSCImport, Error, TEXT("Hull verification requires root geometry")); Unload(World); return false; }
    const FBox Bounds = First.Vehicle->GetComponentsBoundingBox(true);
    bool bHitHull = false;
    for (int32 X = 1; X < 10 && !bHitHull; ++X) for (int32 Y = 1; Y < 10 && !bHitHull; ++Y)
    {
        const FVector P(FMath::Lerp(Bounds.Min.X, Bounds.Max.X, X / 10.0), FMath::Lerp(Bounds.Min.Y, Bounds.Max.Y, Y / 10.0), 0);
        FHitResult Hit; FCollisionQueryParams Params(SCENE_QUERY_STAT(SCImportTrace), true);
        bHitHull = World->LineTraceSingleByChannel(Hit, P + FVector(0,0,Bounds.Max.Z + 100), P + FVector(0,0,Bounds.Min.Z - 100), ECC_Visibility, Params)
            && Hit.GetActor() == First.Vehicle.Get() && Cast<UStaticMeshComponent>(Hit.GetComponent())
            && Hit.GetComponent()->ComponentHasTag(FName(*First.Vehicle->Manifest.Root.MeshAsset));
    }
    UE_LOG(LogSCImport, Display, TEXT("TRACE hit_hull=%d"), bHitHull);
    if (!Import(Memory, Options, Second)) { UE_LOG(LogSCImport, Error, TEXT("VERIFY tar failed: %s"), *Second.Error); return false; }
    FlushRenderingCommands(); CollectGarbage(RF_NoFlags);
    int32 Leaks = 0; for (auto Object : Old) if (Object.IsValid()) ++Leaks;
    const int32 Reloaded = StaticMeshCount();
    bool bSame = First.MeshCount == Second.MeshCount && First.InstanceCount == Second.InstanceCount;
    for (const auto& Pair : First.ContentHashes) bSame &= Second.ContentHashes.FindRef(Pair.Key) == Pair.Value;
    Unload(World); FlushRenderingCommands(); CollectGarbage(RF_NoFlags);
    const int32 Final = StaticMeshCount();
    GEngine->Exec(World, TEXT("obj list class=StaticMesh"));
    const bool bPassed = bSame && bHitHull && bRepresentations && Leaks == 0 && Final == Baseline && Loaded == Reloaded;
    UE_LOG(LogSCImport, Display, TEXT("VERIFY passed=%d folder_assets=%d tar_assets=%d folder_instances=%d tar_instances=%d representations=%d old_live=%d tracked_old=%d baseline=%d loaded=%d reloaded=%d final=%d first_s=%.4f second_s=%.4f"),
        bPassed, First.MeshCount, Second.MeshCount, First.InstanceCount, Second.InstanceCount, bRepresentations, Leaks, Old.Num(), Baseline, Loaded, Reloaded, Final, First.TotalSeconds, Second.TotalSeconds);
    return bPassed;
}
