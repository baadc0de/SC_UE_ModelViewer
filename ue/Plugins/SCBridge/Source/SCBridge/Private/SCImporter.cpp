#include "SCImporter.h"
#include "SCVehicle.h"
#include "SCPackageReader.h"
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/Material.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
    if (!Async(EAsyncExecution::ThreadPool, [&] { return FSCPackageReader::Read(Source, Manifest, Result.Error, Options.ScenePath); }).Get()) return false;
    const FString Session = TEXT("/Temp/SC/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FActorSpawnParameters Spawn; Spawn.ObjectFlags |= RF_Transient;
    ASCVehicle* Vehicle = Options.World->SpawnActor<ASCVehicle>(ASCVehicle::StaticClass(), Options.ActorTransform, Spawn);
    if (!Vehicle) { Result.Error = TEXT("Cannot spawn vehicle"); return false; }
    Vehicle->Manifest = Manifest; Vehicle->SessionPath = Session;
    Vehicle->ClipCatalog = NewObject<USCClipCatalog>(Vehicle); Vehicle->ClipCatalog->Clips = Manifest.Clips;
    TMap<FString, TUniquePtr<FSCDerivedMesh>> Derived;
    TMap<FString, UStaticMesh*> Meshes;
    for (const FString& Path : Manifest.MeshAssets())
    {
        auto Data = MakeUnique<FSCDerivedMesh>();
        const double DeriveStart = FPlatformTime::Seconds();
        if (!Async(EAsyncExecution::ThreadPool, [&] { return FSCMeshImporter::Derive(Source, Path, *Data, Result.Error)
            && (!Options.bBuildCollision || FSCMeshImporter::DeriveCollision(*Data, Result.Error)); }).Get())
        { Vehicle->Destroy(); return false; }
        const double DeriveSeconds = FPlatformTime::Seconds() - DeriveStart;
        // Keep primitive slot indices intact. Sparse sidecars may omit inactive source slots;
        // those slots use the placeholder until WP-U2 applies the authored material policy.
        Result.ContentHashes.Add(Path, Data->ContentHash);
        UPackage* Package = CreatePackage(*AssetPackage(Session, Path)); Package->SetFlags(RF_Transient);
        FSCMeshBuildStats Stats;
        UStaticMesh* Mesh = FSCMeshImporter::Instantiate(*Data, Package, FName(*SafeName(FPaths::GetBaseFilename(Path))), Options.bBuildCollision,
            UMaterial::GetDefaultMaterial(MD_Surface), Stats, Result.Error);
        if (!Mesh) { Vehicle->Destroy(); return false; }
        Vehicle->Meshes.Add(Mesh); Meshes.Add(Path, Mesh); Result.MeshStats.Add(Path, Stats);
        UE_LOG(LogSCImport, Display, TEXT("MESH path=%s hash=%s triangles=%d degenerate=%d nanite=%s clusters=%d fallback=%d cards=%d distance_field=%d rt_geometry=%d collision=%d derive_s=%.4f build_s=%.4f representation_wait_s=%.4f cook_s=%.4f skins_static=%d"),
            *Path, *Data->ContentHash, Stats.Triangles, Data->DegenerateTriangles, Stats.Clusters ? TEXT("accepted") : Stats.Triangles ? TEXT("disabled:no_clusters") : TEXT("disabled:empty_geometry"), Stats.Clusters,
            Stats.FallbackTriangles, Stats.Cards, Stats.bDistanceField, Stats.bRayTracingGeometry, Stats.bCollision, DeriveSeconds, Stats.BuildSeconds,
            Stats.RepresentationSeconds, Stats.CollisionSeconds, Data->bHasSkins);
        Derived.Add(Path, MoveTemp(Data));
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
        if (!E) { Result.Error = TEXT("Unresolved parent entity: ") + I.ParentEntityName; return nullptr; }
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
        if (!Node) { Result.Error = TEXT("Unresolved parent helper: ") + I.ParentEntityName + TEXT("/") + I.ParentNodeName; return nullptr; }
        auto* Helper = NewAnchor(I.ParentNodeName, E->Anchor, FSCMeshImporter::Transform(*Node));
        E->Helpers.Add(I.ParentNodeName, Helper); bHelper = true; return Helper;
    };
    auto Place = [&](const FSCSceneInstance& I, USceneComponent* Parent, bool bAtHelper, bool bRoot) -> FEntity
    {
        FEntity E;
        if (auto* Data = Derived.Find(I.MeshAsset)) E.Mesh = Data->Get();
        const FMatrix Relative = FSCMeshImporter::InstanceTransform(I.LocalMatrix, E.Mesh ? E.Mesh->RootMatrix : FMatrix::Identity,
            bAtHelper, I.SourceTransformBasis == TEXT("cryengine_z_up"));
        E.Anchor = NewAnchor(I.EntityName, Parent, bRoot ? FMatrix::Identity : Relative);
        if (UStaticMesh** Mesh = Meshes.Find(I.MeshAsset); Mesh && E.Mesh && E.Mesh->Description.Triangles().Num() > 0)
        {
            auto* C = NewObject<UStaticMeshComponent>(Vehicle, MakeUniqueObjectName(Vehicle, UStaticMeshComponent::StaticClass(), TEXT("Mesh")), RF_Transient);
            Vehicle->AddInstanceComponent(C); C->SetMobility(EComponentMobility::Movable); C->SetupAttachment(E.Anchor); C->SetStaticMesh(*Mesh);
            C->SetCollisionProfileName(Options.bBuildCollision ? TEXT("BlockAll") : TEXT("NoCollision"));
            C->SetVisibility(!I.bInvisible); C->SetHiddenInGame(I.bInvisible); C->RegisterComponent(); ++Result.InstanceCount;
        }
        return E;
    };
    Entities.Add(Manifest.Root.EntityName, Place(Manifest.Root, Vehicle->GetRootComponent(), false, true));
    for (const auto& I : Manifest.Children)
    {
        bool bHelper; auto* Parent = FindParent(I, bHelper);
        if (!Parent) { Vehicle->Destroy(); return false; }
        // Export order is depth-first; latest matching entity disambiguates repeated loadout instances.
        Entities.Add(I.EntityName, Place(I, Parent, bHelper, false));
    }
    for (const auto& Interior : Manifest.Interiors)
    {
        bool bHelper; auto* Parent = FindParent(Interior.Container, bHelper);
        if (!Parent) { Vehicle->Destroy(); return false; }
        const FMatrix W = FSCMeshImporter::CryToGltf();
        auto* Container = NewAnchor(Interior.Container.EntityName, Parent, FSCMeshImporter::Transform(bHelper ? Interior.Container.LocalMatrix : W.Inverse() * Interior.Container.LocalMatrix * W));
        for (const auto& P : Interior.Placements) Place(P, Container, bHelper, false);
    }
    if (Options.bReplacePrevious) Unload(Options.World);
    Vehicles.Add(Options.World, Vehicle); Result.Vehicle = Vehicle; Result.MeshCount = Meshes.Num();
    Result.TotalSeconds = FPlatformTime::Seconds() - Start;
    UE_LOG(LogSCImport, Display, TEXT("IMPORT_OK seconds=%.4f assets=%d instances=%d children=%d interiors=%d materials=%d palettes=%d clips=%d game_world=%d"),
        Result.TotalSeconds, Result.MeshCount, Result.InstanceCount, Manifest.Children.Num(), Manifest.Interiors.Num(), Manifest.Materials.Num(), Manifest.Palettes.Num(), Manifest.Clips.Num(), Options.World->IsGameWorld());
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
            bRepresentations &= Pair.Value.Clusters > 0 && Pair.Value.bDistanceField && Pair.Value.bMeshCards && Pair.Value.bRayTracingGeometry && Pair.Value.bCollision
                && Pair.Value.Triangles == Pair.Value.FallbackTriangles;
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
            && Cast<UStaticMeshComponent>(Hit.GetComponent())->GetStaticMesh() == First.Vehicle->Meshes[First.Vehicle->Manifest.MeshAssets().IndexOfByKey(First.Vehicle->Manifest.Root.MeshAsset)];
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
