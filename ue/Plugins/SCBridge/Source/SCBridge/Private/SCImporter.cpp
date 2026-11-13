// Package import entry points: synchronous Import over FSCImportJob, per-world vehicle registry, verification.
#include "SCImporter.h"
#include "SCEnvironment.h"
#include "SCImportJob.h"
#include "SCVehicle.h"
#include "SCLightImporter.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/Material.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "UObject/UObjectIterator.h"
#include "RenderingThread.h"

DEFINE_LOG_CATEGORY(LogSCImport);
namespace
{
TMap<TWeakObjectPtr<UWorld>, TWeakObjectPtr<ASCVehicle>> Vehicles;
int32 StaticMeshCount()
{
    int32 Count = 0;
    for (TObjectIterator<UStaticMesh> It; It; ++It)
        if (!It->IsTemplate()) ++Count;
    return Count;
}
} // namespace
bool FSCImporter::Import(FSCPackageSource& Source, FSCImportOptions Options, FSCImportResult& Result)
{
    // Synchronous wrapper over the incremental job: same steps, workers awaited in place.
    FSCImportJob Job(Source, MoveTemp(Options));
    Job.Tick(true);
    Result = MoveTemp(Job.GetResult());
    return Job.GetStage() == ESCImportStage::Succeeded;
}
void FSCImporter::Publish(UWorld* World, ASCVehicle* Vehicle, bool bReplacePrevious)
{
    if (bReplacePrevious) Unload(World);
    Vehicles.Add(World, Vehicle);
}
void FSCImporter::Unload(UWorld* World)
{
    if (auto* Previous = Vehicles.Find(World))
        if (Previous->IsValid()) Previous->Get()->Destroy();
    Vehicles.Remove(World);
}
ASCVehicle* USCImportLibrary::ImportFolder(UObject* WorldContext, const FString& Folder, bool bBuildCollision)
{
    FSCFolderPackageSource Source(Folder);
    FSCImportOptions Options;
    Options.World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
    Options.bBuildCollision = bBuildCollision;
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
ASCVehicle* USCImportLibrary::ImportEnvironment(UObject* WorldContext, const FString& Folder, const FString& ScenePath)
{
    FSCFolderPackageSource Source(Folder);
    FSCImportOptions Options;
    Options.World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
    Options.ScenePath = ScenePath;
    Options.bHideEffectOnlyPlacements = true;
    Options.bInstanceRepeatedParts = true;
    Options.bPublish = false; // not the world's current vehicle
    FSCImportResult Result;
    if (!FSCImporter::Import(Source, Options, Result)) UE_LOG(LogSCImport, Error, TEXT("IMPORT_FAILED %s"), *Result.Error);
    return Result.Vehicle.Get();
}
double USCImportLibrary::PresentEnvironment(ASCVehicle* Environment, const FString& State, double FloorZ)
{
    FSCEnvironment::SetState(Environment, State);
    return FSCEnvironment::PlaceOnFloor(Environment, FloorZ, nullptr);
}
bool USCImportLibrary::VerifyPackage(UObject* Context, const FString& Folder, const FString& Tar)
{
    return FSCImporter::Verify(Folder, Tar, GEngine->GetWorldFromContextObject(Context, EGetWorldErrorMode::ReturnNull));
}
void USCImportLibrary::UnloadPackage(UObject* Context)
{
    FSCImporter::Unload(GEngine->GetWorldFromContextObject(Context, EGetWorldErrorMode::ReturnNull));
}

bool FSCImporter::Verify(const FString& Folder, const FString& Tar, UWorld* World)
{
    if (!World) return false;
    FSCFolderPackageSource Disk(Folder);
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Tar))
    {
        UE_LOG(LogSCImport, Error, TEXT("Cannot read verification tar"));
        return false;
    }
    FSCTarPackageSource Memory(MoveTemp(Bytes));
    if (!Memory.IsValid())
    {
        UE_LOG(LogSCImport, Error, TEXT("%s"), *Memory.GetError());
        return false;
    }
    // Preload placeholder before baseline; it may lazily load engine dependencies.
    UMaterial::GetDefaultMaterial(MD_Surface);
    Unload(World);
    FlushRenderingCommands();
    CollectGarbage(RF_NoFlags);
    const int32 Baseline = StaticMeshCount();
    GEngine->Exec(World, TEXT("obj list class=StaticMesh"));
    FSCImportOptions Options;
    Options.World = World;
    Options.ActorTransform.SetLocation(FVector(0, 0, 2000));
    FSCImportResult First, Second;
    if (!Import(Disk, Options, First))
    {
        UE_LOG(LogSCImport, Error, TEXT("VERIFY folder failed: %s"), *First.Error);
        return false;
    }
    TArray<TWeakObjectPtr<UObject>> Old;
    for (TObjectIterator<UObject> It; It; ++It)
        if (It->IsIn(First.Vehicle.Get()) || *It == First.Vehicle.Get() ||
            It->GetOutermost()->GetName().StartsWith(First.Vehicle->SessionPath))
            Old.Add(*It);
    const int32 Loaded = StaticMeshCount();
    bool bLights = FSCLightImporter::Verify(First.Vehicle.Get());
    bool bRepresentations = true;
    for (const auto& Pair : First.MeshStats)
        if (Pair.Value.Triangles > 0)
            bRepresentations &=
                (Pair.Value.bNaniteEnabled ? Pair.Value.Clusters > 0 && Pair.Value.bDistanceField && Pair.Value.bMeshCards &&
                                                 Pair.Value.bRayTracingGeometry && Pair.Value.bCollision
                                           : Pair.Value.Clusters == 0) &&
                Pair.Value.Triangles == Pair.Value.FallbackTriangles;
    if (const auto* RootStats = First.MeshStats.Find(First.Vehicle->Manifest.Root.MeshAsset)) bRepresentations &= RootStats->Cards > 0;
    if (First.Vehicle->Manifest.Root.MeshAsset.IsEmpty())
    {
        UE_LOG(LogSCImport, Error, TEXT("Hull verification requires root geometry"));
        Unload(World);
        return false;
    }
    const FBox Bounds = First.Vehicle->GetComponentsBoundingBox(true);
    bool bHitHull = false;
    for (int32 X = 1; X < 10 && !bHitHull; ++X)
        for (int32 Y = 1; Y < 10 && !bHitHull; ++Y)
        {
            const FVector P(FMath::Lerp(Bounds.Min.X, Bounds.Max.X, X / 10.0), FMath::Lerp(Bounds.Min.Y, Bounds.Max.Y, Y / 10.0), 0);
            FHitResult Hit;
            FCollisionQueryParams Params(SCENE_QUERY_STAT(SCImportTrace), true);
            bHitHull = World->LineTraceSingleByChannel(Hit, P + FVector(0, 0, Bounds.Max.Z + 100), P + FVector(0, 0, Bounds.Min.Z - 100),
                                                       ECC_Visibility, Params) &&
                       Hit.GetActor() == First.Vehicle.Get() && Cast<UStaticMeshComponent>(Hit.GetComponent()) &&
                       Hit.GetComponent()->ComponentHasTag(FName(*First.Vehicle->Manifest.Root.MeshAsset));
        }
    UE_LOG(LogSCImport, Display, TEXT("TRACE hit_hull=%d"), bHitHull);
    if (!Import(Memory, Options, Second))
    {
        UE_LOG(LogSCImport, Error, TEXT("VERIFY tar failed: %s"), *Second.Error);
        return false;
    }
    FlushRenderingCommands();
    CollectGarbage(RF_NoFlags);
    int32 Leaks = 0;
    for (auto Object : Old)
        if (Object.IsValid()) ++Leaks;
    const int32 Reloaded = StaticMeshCount();
    bLights &= FSCLightImporter::Verify(Second.Vehicle.Get());
    bool bSame = First.MeshCount == Second.MeshCount && First.InstanceCount == Second.InstanceCount;
    for (const auto& Pair : First.ContentHashes)
        bSame &= Second.ContentHashes.FindRef(Pair.Key) == Pair.Value;
    Unload(World);
    FlushRenderingCommands();
    CollectGarbage(RF_NoFlags);
    const int32 Final = StaticMeshCount();
    GEngine->Exec(World, TEXT("obj list class=StaticMesh"));
    const bool bPassed = bLights && bSame && bHitHull && bRepresentations && Leaks == 0 && Final == Baseline && Loaded == Reloaded;
    UE_LOG(LogSCImport, Display,
           TEXT("VERIFY passed=%d folder_assets=%d tar_assets=%d folder_instances=%d tar_instances=%d representations=%d old_live=%d "
                "tracked_old=%d baseline=%d loaded=%d reloaded=%d final=%d first_s=%.4f second_s=%.4f"),
           bPassed, First.MeshCount, Second.MeshCount, First.InstanceCount, Second.InstanceCount, bRepresentations, Leaks, Old.Num(),
           Baseline, Loaded, Reloaded, Final, First.TotalSeconds, Second.TotalSeconds);
    return bPassed;
}
