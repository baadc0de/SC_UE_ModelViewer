// WP-U0 synthetic feasibility probes. No Star Citizen data or importer policy lives here.
#include "CoreMinimal.h"
#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "Rendering/NaniteResources.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#if WITH_EDITOR
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "StaticMeshCompiler.h"
#include "UObject/ArchiveCookContext.h"
#include "SCSpikePackageWriter.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSCSpike, Log, All);

namespace SCSpike
{
static TWeakObjectPtr<UStaticMesh> LastMesh;
static TWeakObjectPtr<AStaticMeshActor> LastActor;
static FString CacheDirectory;
static int32 BuildCalls = 0;
static int32 CookCalls = 0;
static bool bPassed = true;
static FTSTicker::FDelegateHandle TickHandle;

static FString CacheRoot()
{
    if (CacheDirectory.IsEmpty())
    {
        if (!FParse::Value(FCommandLine::Get(), TEXT("SCSpikeCacheDir="), CacheDirectory))
        {
            // UE changes cwd during startup. PWD preserves the invoking shell's cwd.
            const FString LaunchDir = FPlatformMisc::GetEnvironmentVariable(TEXT("PWD"));
            CacheDirectory = FPaths::Combine(LaunchDir.IsEmpty() ? FPlatformProcess::GetCurrentWorkingDirectory() : LaunchDir, TEXT("cache/spike"));
        }
        CacheDirectory = FPaths::ConvertRelativePathToFull(CacheDirectory) / TEXT("");
        IFileManager::Get().MakeDirectory(*CacheDirectory, true);
        FPackageName::RegisterMountPoint(TEXT("/SCCache/"), CacheDirectory);
        UE_LOG(LogSCSpike, Display, TEXT("CACHE_ROOT=%s"), *CacheDirectory);
    }
    return CacheDirectory;
}

static UWorld* GameWorld()
{
    if (GEngine)
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
            if (Context.World() && Context.World()->IsGameWorld()) return Context.World();
    return nullptr;
}

static void Describe(UStaticMesh* Mesh, const TCHAR* Phase, double Seconds)
{
    const FStaticMeshRenderData* Data = Mesh->GetRenderData();
    const Nanite::FResources* Nanite = Data ? Data->NaniteResourcesPtr.Get() : nullptr;
    UE_LOG(LogSCSpike, Display, TEXT("%s seconds=%.6f editor=%d nanite=%d clusters=%u input_triangles=%u fallback_triangles=%d physics_meshes=%d build_calls=%d cook_calls=%d"),
        Phase, Seconds, WITH_EDITOR, Data && Data->HasValidNaniteData(), Nanite ? Nanite->NumClusters : 0,
        Nanite ? Nanite->NumInputTriangles : 0, Data && Data->LODResources.Num() ? Data->LODResources[0].GetNumTriangles() : 0,
        Mesh->GetBodySetup() ? Mesh->GetBodySetup()->TriMeshGeometries.Num() : 0, BuildCalls, CookCalls);
}

static bool PlaceAndTrace(UWorld* World, UStaticMesh* Mesh)
{
    if (LastActor.IsValid()) LastActor->Destroy();
    AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, 150), FRotator::ZeroRotator);
    UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
    Component->SetMobility(EComponentMobility::Movable);
    Component->SetStaticMesh(Mesh);
    Component->SetCollisionProfileName(TEXT("BlockAll"));
    LastActor = Actor;
    LastMesh = Mesh;
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCSpikeTrace), true);
    const bool bHit = World->LineTraceSingleByChannel(Hit, FVector(17, 23, 1000), FVector(17, 23, -100), ECC_Visibility, Params);
    const bool bSpikeHit = bHit && Hit.GetComponent() == Component;
    bPassed &= bSpikeHit;
    UE_LOG(LogSCSpike, Display, TEXT("TRACE hit_spike=%d hit_actor=%s impact_z=%.3f"), bSpikeHit, *GetNameSafe(Hit.GetActor()), Hit.ImpactPoint.Z);
    return bSpikeHit;
}

static UStaticMesh* Build(UWorld* World)
{
    const double Start = FPlatformTime::Seconds();
    FMeshDescription Description;
    FStaticMeshAttributes Attributes(Description);
    Attributes.Register();
    auto Positions = Attributes.GetVertexPositions();
    auto Normals = Attributes.GetVertexInstanceNormals();
    auto Tangents = Attributes.GetVertexInstanceTangents();
    auto Signs = Attributes.GetVertexInstanceBinormalSigns();
    auto UVs = Attributes.GetVertexInstanceUVs();
    auto Colours = Attributes.GetVertexInstanceColors();
    UVs.SetNumChannels(1);
    const FPolygonGroupID Group = Description.CreatePolygonGroup();
    Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Surface");
    // Deliberately synthetic 224 x 224 quad terrain: 100,352 triangles.
    constexpr int32 Side = 224;
    TArray<FVertexID> Vertices;
    for (int32 Y = 0; Y <= Side; ++Y)
        for (int32 X = 0; X <= Side; ++X)
        {
            FVertexID Vertex = Description.CreateVertex();
            Positions[Vertex] = FVector3f((X / float(Side) - 0.5f) * 1000, (Y / float(Side) - 0.5f) * 1000,
                20 * FMath::Sin(X * 0.12f) * FMath::Cos(Y * 0.12f));
            Vertices.Add(Vertex);
        }
    auto Triangle = [&](int32 A, int32 B, int32 C)
    {
        TArray<FVertexInstanceID> Instances;
        const FVector3f Normal = FVector3f::CrossProduct(Positions[Vertices[B]] - Positions[Vertices[A]], Positions[Vertices[C]] - Positions[Vertices[A]]).GetSafeNormal();
        for (int32 Index : { A, B, C })
        {
            FVertexInstanceID Instance = Description.CreateVertexInstance(Vertices[Index]);
            Normals[Instance] = Normal;
            Tangents[Instance] = FVector3f(1, 0, 0);
            Signs[Instance] = 1;
            Colours[Instance] = FVector4f(1, 1, 1, 1);
            UVs.Set(Instance, 0, FVector2f((Index % (Side + 1)) / float(Side), (Index / (Side + 1)) / float(Side)));
            Instances.Add(Instance);
        }
        Description.CreateTriangle(Group, Instances);
    };
    for (int32 Y = 0; Y < Side; ++Y)
        for (int32 X = 0; X < Side; ++X)
        {
            const int32 A = Y * (Side + 1) + X;
            Triangle(A, A + 1, A + Side + 2);
            Triangle(A, A + Side + 2, A + Side + 1);
        }
    UE_LOG(LogSCSpike, Display, TEXT("GENERATE triangles=%d seconds=%.6f"), Description.Triangles().Num(), FPlatformTime::Seconds() - Start);
    UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
    Mesh->GetStaticMaterials().Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface), TEXT("Surface")));
    Mesh->bAllowCPUAccess = true;
    Mesh->GetNaniteSettings().bEnabled = true;
    Mesh->GetNaniteSettings().FallbackTarget = ENaniteFallbackTarget::PercentTriangles;
    Mesh->GetNaniteSettings().FallbackPercentTriangles = 1.0f;
    Mesh->GetNaniteSettings().FallbackRelativeError = 0.0f;
    Mesh->CreateBodySetup();
    Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
    Mesh->GetBodySetup()->bDoubleSidedGeometry = true;
    UStaticMesh::FBuildMeshDescriptionsParams Params;
    Params.bFastBuild = !WITH_EDITOR;
    Params.bAllowCpuAccess = true;
    Params.bBuildSimpleCollision = false;
    const double BuildStart = FPlatformTime::Seconds();
    ++BuildCalls;
    Mesh->BuildFromMeshDescriptions({ &Description }, Params);
#if WITH_EDITOR
    FStaticMeshCompilingManager::Get().FinishAllCompilation();
#endif
    Describe(Mesh, TEXT("BUILD"), FPlatformTime::Seconds() - BuildStart);
    bPassed &= Mesh->GetRenderData()->HasValidNaniteData()
        && Mesh->GetRenderData()->LODResources[0].GetNumTriangles() == Description.Triangles().Num();
    // Invalidate any automatically generated collision so the timed cook is real.
    Mesh->GetBodySetup()->InvalidatePhysicsData();
    const double CookStart = FPlatformTime::Seconds();
    ++CookCalls;
    Mesh->GetBodySetup()->CreatePhysicsMeshes();
    Describe(Mesh, TEXT("COOK"), FPlatformTime::Seconds() - CookStart);
    PlaceAndTrace(World, Mesh);
    return Mesh;
}

// Compile this explicit experiment with SC_SPIKE_RAW_SERIALIZE=1. An installed
// engine may not export FStaticMeshRenderData::Serialize to plugin modules.
#if SC_SPIKE_RAW_SERIALIZE
static void SaveRaw()
{
    UStaticMesh* Mesh = LastMesh.Get();
    if (!Mesh) return;
    TArray<uint8> Bytes;
    FMemoryWriter Writer(Bytes, true);
    FObjectAndNameAsStringProxyArchive Archive(Writer, false);
    Mesh->GetRenderData()->Serialize(Archive, Mesh, false);
    Mesh->GetBodySetup()->Serialize(Archive);
    Mesh->GetBodySetup()->CookedFormatData.Serialize(Archive, Mesh->GetBodySetup());
    FFileHelper::SaveArrayToFile(Bytes, *(CacheRoot() / TEXT("render-body.bin")));
}
static void LoadRaw(UWorld* World)
{
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *(CacheRoot() / TEXT("render-body.bin")))) return;
    const double Start = FPlatformTime::Seconds();
    UStaticMesh* Mesh = NewObject<UStaticMesh>();
    Mesh->SetRenderData(MakeUnique<FStaticMeshRenderData>());
    Mesh->CreateBodySetup();
    FMemoryReader Reader(Bytes, true);
    FObjectAndNameAsStringProxyArchive Archive(Reader, true);
    Mesh->GetRenderData()->Serialize(Archive, Mesh, false);
    Mesh->GetBodySetup()->Serialize(Archive);
    Mesh->GetBodySetup()->CookedFormatData.Serialize(Archive, Mesh->GetBodySetup());
    Mesh->InitResources();
    Describe(Mesh, TEXT("LOAD_RAW"), FPlatformTime::Seconds() - Start);
    PlaceAndTrace(World, Mesh);
}
#endif

static void SavePackage()
{
#if WITH_EDITOR
    UStaticMesh* Mesh = LastMesh.Get();
    if (!Mesh) return;
    CacheRoot();
    const double Start = FPlatformTime::Seconds();
    UPackage* Package = CreatePackage(TEXT("/SCCache/SpikeMesh"));
    Mesh->Rename(TEXT("SpikeMesh"), Package, REN_DontCreateRedirectors | REN_NonTransactional);
    Mesh->ClearFlags(RF_Transient);
    Mesh->SetFlags(RF_Public | RF_Standalone);
    Mesh->GetBodySetup()->ClearFlags(RF_Transient);
    const ITargetPlatform* Platform = GetTargetPlatformManagerRef().FindTargetPlatform(TEXT("Linux"));
    check(Platform);
    FArchiveCookContext CookContext(Package, UE::Cook::ECookType::ByTheBook, UE::Cook::ECookingDLC::No, Platform, nullptr);
    FArchiveCookData CookData(*Platform, CookContext);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    Args.bSlowTask = false;
    Args.ArchiveCookData = &CookData;
    const FString Filename = CacheRoot() / TEXT("SpikeMesh.uasset");
    Package->SetPackageFlags(PKG_FilterEditorOnly);
    Mesh->BeginCacheForCookedPlatformData(Platform);
    FStaticMeshCompilingManager::Get().FinishAllCompilation();
    FSCSpikePackageWriter* Writer = new FSCSpikePackageWriter();
    FSavePackageContext SaveContext(Platform, Writer); // Context owns the writer.
    Args.SavePackageContext = &SaveContext;
    IPackageWriter::FBeginPackageInfo BeginInfo;
    BeginInfo.PackageName = Package->GetFName();
    BeginInfo.LooseFilePath = Filename;
    Writer->BeginPackage(BeginInfo);
    const bool bSaved = UPackage::SavePackage(Package, Mesh, *Filename, Args);
    IPackageWriter::FCommitPackageInfo CommitInfo;
    CommitInfo.PackageName = Package->GetFName();
    CommitInfo.Status = bSaved ? IPackageWriter::ECommitStatus::Success : IPackageWriter::ECommitStatus::Error;
    CommitInfo.WriteOptions = IPackageWriter::EWriteOptions::Write;
    Writer->CommitPackage(MoveTemp(CommitInfo));
    bPassed &= bSaved && Writer->bWritten;
    UE_LOG(LogSCSpike, Display, TEXT("PACKAGE_WRITER committed=%d"), Writer->bWritten);
    UE_LOG(LogSCSpike, Display, TEXT("SAVE_PACKAGE ok=%d seconds=%.6f bytes=%lld file=%s"), bSaved, FPlatformTime::Seconds() - Start, IFileManager::Get().FileSize(*Filename), *Filename);
#endif
}

static void LoadPackage(UWorld* World)
{
    CacheRoot();
    const double Start = FPlatformTime::Seconds();
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/SCCache/SpikeMesh.SpikeMesh"));
    if (!Mesh)
    {
        UE_LOG(LogSCSpike, Error, TEXT("LOAD_PACKAGE failed"));
        bPassed = false;
        return;
    }
#if WITH_EDITOR
    FStaticMeshCompilingManager::Get().FinishAllCompilation();
    const bool bCooked = Mesh->GetPackage()->HasAnyPackageFlags(PKG_Cooked);
    UE_LOG(LogSCSpike, Display, TEXT("CACHE_OBJECT cooked=%d source_models=%d"), bCooked, Mesh->GetNumSourceModels());
    bPassed &= bCooked && Mesh->GetNumSourceModels() == 0;
#endif
    FStaticMeshRenderData* Data = Mesh->GetRenderData();
    if (!Data || !Data->HasValidNaniteData())
    {
        bPassed = false;
        UE_LOG(LogSCSpike, Error, TEXT("LOAD_PACKAGE missing Nanite data"));
        return;
    }
    // Force a real read of the streaming sidecar; NullRHI alone never requests GPU pages.
    Nanite::FResources& Nanite = *Data->NaniteResourcesPtr;
    const int64 StreamBytes = Nanite.StreamablePages.GetBulkDataSize();
    const void* Stream = Nanite.StreamablePages.LockReadOnly();
    bPassed &= Stream && StreamBytes > 0 && Nanite.RootData.Num() > 0;
    UE_LOG(LogSCSpike, Display, TEXT("CACHE_NANITE root_bytes=%d stream_bytes=%lld stream_read=%d"), Nanite.RootData.Num(), StreamBytes, Stream != nullptr);
    Nanite.StreamablePages.Unlock();
    bPassed &= BuildCalls == 0 && CookCalls == 0;
    Describe(Mesh, TEXT("LOAD_PACKAGE"), FPlatformTime::Seconds() - Start);
    PlaceAndTrace(World, Mesh);
}

static FAutoConsoleCommandWithWorld BuildCommand(TEXT("sc.SpikeBuildMesh"), TEXT("WP-U0: build 100,352 synthetic triangles, Nanite and collision; line trace."), FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) { if (World) Build(World); }));
static FAutoConsoleCommand SaveCommand(TEXT("sc.SpikeSavePackage"), TEXT("WP-U0: save processed mesh under ./cache/spike/."), FConsoleCommandDelegate::CreateStatic(&SavePackage));
static FAutoConsoleCommandWithWorld LoadCommand(TEXT("sc.SpikeLoadPackage"), TEXT("WP-U0: load in a fresh process without calling mesh build/cook."), FConsoleCommandWithWorldDelegate::CreateStatic(&LoadPackage));

void Start()
{
    FString Mode;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SCSpike="), Mode)) return;
#if !SC_SPIKE_RAW_SERIALIZE
    if (Mode == TEXT("save-raw") || Mode == TEXT("load-raw"))
    {
        UE_LOG(LogSCSpike, Error, TEXT("Raw serialization probe is not linked; see ue/README.md."));
        FPlatformMisc::RequestExitWithStatus(false, 1);
        return;
    }
#endif
    // Wait for the game world, then leave frames for renderer validation and captures.
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Mode, Executed = false, ExitAt = 0.0](float) mutable
    {
        UWorld* World = GameWorld();
        if (!World) return true;
        if (!Executed)
        {
            Executed = true;
            UE_LOG(LogSCSpike, Display, TEXT("HOST editor=%d game_world=%d mode=%s"), WITH_EDITOR, World->IsGameWorld(), *Mode);
            if (Mode == TEXT("load")) LoadPackage(World);
#if SC_SPIKE_RAW_SERIALIZE
            else if (Mode == TEXT("load-raw")) LoadRaw(World);
#endif
            else
            {
                Build(World);
                if (Mode == TEXT("save")) SavePackage();
#if SC_SPIKE_RAW_SERIALIZE
                if (Mode == TEXT("save-raw")) SaveRaw();
#endif
            }
            if (APlayerController* PC = World->GetFirstPlayerController())
            {
                ACameraActor* Camera = World->SpawnActor<ACameraActor>(FVector(900, -1200, 850), FRotator(-28, 125, 0));
                PC->SetViewTarget(Camera);
            }
            ExitAt = FPlatformTime::Seconds() + 20.0;
        }
        if (FPlatformTime::Seconds() >= ExitAt && !FParse::Param(FCommandLine::Get(), TEXT("SCSpikeStay")))
        {
            UE_LOG(LogSCSpike, Display, TEXT("RUN_COMPLETE passed=%d build_calls=%d cook_calls=%d"), bPassed, BuildCalls, CookCalls);
            FPlatformMisc::RequestExitWithStatus(false, bPassed ? 0 : 1);
            return false;
        }
        return true;
    }), 1.0f);
}
void Stop()
{
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
}
}
