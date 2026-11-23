// Incremental package import. Same mapping as the former monolithic FSCImporter::Import, split into steps:
// Reading (worker: manifest + clip sidecars) -> Meshes (per asset: worker derive with look-ahead, worker
// prepare incl. collision BVH, game-thread UObject build) -> Materials (worker decode overlapped with meshes,
// game-thread textures/MIDs) -> Assembling (components, lights, publish).
#include "SCImportJob.h"
#include "SCInstancing.h"
#include "SCVehicleLifts.h"
#include "SCVehicle.h"
#include "SCPackageReader.h"
#include "SCImportMaterials.h"
#include "SCProcessedCache.h"
#include "SCGeometryStreaming.h"
#include "SCProcessedMeshCache.h"
#include "SCProcessedTextureCache.h"
#include "SCSkeletonBuilder.h"
#include "SCAnimImporter.h"
#include "SCLightImporter.h"
#include "SCLightAreas.h"
#include "SCAudioSystem.h"
#include "SCDirt.h"
#include "SCEmissiveLights.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "SCViewerSettings.h"
#include "SCRigComponent.h"
#include "Cargo/SCCargoRecords.h"
#include "Cargo/SCCargoPlanner.h"
#include "SCHangarWaypoints.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/NaniteResources.h"
#include "StaticMeshResources.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#endif
#include "UObject/Package.h"
#include "PhysicsEngine/BodySetup.h"
#include "Misc/ScopeExit.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Animation/AnimSequence.h"
#include <atomic>

namespace
{
// Unique assets whose derivation may run ahead of the game-thread build: one per worker thread, at most MaxDeriveLookAhead
// (bounds peak CPU mesh memory; a resource budget, not game data). A large ship has ~500 assets.
constexpr int32 MaxDeriveLookAhead = 12;
constexpr int32 PerVehicleTriangleBudget = 500000;
int32 DeriveLookAhead() { return FMath::Clamp(FTaskGraphInterface::Get().GetNumWorkerThreads(), 3, MaxDeriveLookAhead); }
// Fractions of the whole import assigned to each stage, for the progress bar only.
constexpr float ReadShare = 0.05f, MeshShare = 0.80f, MaterialShare = 0.05f;

FString SafeName(FString Name)
{
    for (TCHAR& C : Name)
        if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    return Name.IsEmpty() ? TEXT("Object") : Name;
}
FString AssetPackage(const FString& Session, const FString& Path)
{
    TArray<FString> Parts;
    FPaths::ChangeExtension(Path, TEXT("")).ParseIntoArray(Parts, TEXT("/"), true);
    for (FString& Part : Parts)
        Part = SafeName(Part);
    return Session / FString::Join(Parts, TEXT("/"));
}
struct FEntity
{
    USceneComponent* Anchor = nullptr;
    const FSCDerivedMesh* Mesh = nullptr;
    TMap<FString, USceneComponent*> Helpers;
    USCRigComponent* Rig = nullptr;
};
struct FPart
{
    struct FCacheVariant
    {
        FString Key;
        TUniquePtr<FSCProcessedCache::FReadPin> Pin;
        // A full processed hit can keep just the source selection and the light-channel samples.
        TArray<FTriangleID> SplitTriangles;
        TArray<FVector3f> LightSamples;
        bool bDisableNanite = false;
        TUniquePtr<FSCDerivedMesh> Coarse;
        double Deviation = 0;
    };
    int32 Owner = INDEX_NONE;
    FSCDerivedMesh Mesh, Translucent;
    FCacheVariant OpaqueCache, TranslucentCache;
    TArray<FTriangleID> CollisionTriangles; // unsplit source for collision and for a deferred unsplit description
    bool bDeferredGeometry = false;
    bool bGamePhysics = false; // WP-P3: the game's colliders of Owner; hidden, blocks the pawn only
};
/** One unique mesh asset moving through derive -> prepare -> build. */
struct FMeshSlot
{
    FString Path;
    TUniquePtr<FSCDerivedMesh> Data = MakeUnique<FSCDerivedMesh>();
    TFuture<bool> Derive;
    FString DeriveError;
    double DeriveSeconds = 0, DeriveWaitStart = 0, DeriveWaitSeconds = 0;
    double PrepareSeconds = 0, PrepareWaitStart = 0;
    int32 DeferredRigidParts = 0;
    bool bDeriveLaunched = false, bDeriveWarned = false;
    TFuture<void> Prepare;
    bool bPrepareLaunched = false;
    TArray<TPair<FString, FString>> PrepareWarnings; // (path, message), logged on the game thread
    TSet<int32> TranslucentSlots;
    TArray<int32> SlotTriangles;
    TMap<int32, TArray<int32>> ScreenOwners, SoftScreenOwners; // WP-I1: screen slot -> rig nodes owning its rigid / skinned triangles
    TMap<int32, FSCScreenFaceGeometry> ScreenGeometry; // WP-P5 floor panel: screen slot -> its triangles (the build drops the description)
    TMap<int32, FSCClipBinding> Bindings;
    bool bAnimated = false;
    FSCDerivedMesh Soft;
    int32 SoftTriangles = 0, RigidGroups = 0;
    TArray<FPart> Parts;
    double CollisionSeconds = 0;
    // Game-thread build cursor: one static-mesh variant (part or its translucent split) per job step.
    bool bBuildBegun = false, bBuildFailed = false;
    UPackage* Package = nullptr; // also held by FState::KeepAlive between steps (it may be empty)
    int32 NextPart = 0;
    bool bTranslucentNext = false;
    FString PartSuffix;
    TArray<UStaticMesh*> Variants;
    TSet<int32> GamePhysicsVariants; // indices into Variants
    bool bGamePhysicsFailed = false; // a game collider part failed to derive or build: render collision stays
};
} // namespace

struct FSCImportJob::FState
{
    const FSCPackageSource* Source = nullptr;
    TSharedPtr<const FSCPackageSource> SourceOwner;
    FSCImportOptions Options;
    FSCImportResult Result;
    FSCImportTimings Timings;
    int32 CachedNaniteStreamable = 0, CachedNaniteDemandPaged = 0;
    int32 FineTrianglesDeferred = 0;
    TSet<UStaticMesh*> OpaqueProxies;
    ESCImportStage Stage = ESCImportStage::Reading;
    std::atomic<bool> bCancel{false};
    double Start = 0;
    // The job running this state: the import that built it, then (once its assets are retained and it is gone) one
    // assembly from those assets at a time. Only the running job references the actor and tears a partial one down.
    const FSCImportJob* RunOwner = nullptr;
    bool bAssetsSynced = false; // this run's actor's objects are in the asset set (retained or adopted)

    // Reading (worker output)
    TFuture<bool> ReadTask;
    bool bReadLaunched = false;
    FString ReadError;
    FSCSceneManifest Manifest;
    TArray<FString> ReadWarnings;
    TArray<TPair<FString, FString>> ClipWarnings;
    TArray<FSCImportedClip> ReadClips;
    TArray<TSharedPtr<FJsonObject>> ClipJson;

    // Game-thread objects (also owned by Vehicle UPROPERTYs; Vehicle is referenced via AddReferencedObjects)
    TObjectPtr<ASCVehicle> Vehicle;
    TWeakObjectPtr<ASCVehicle> Spawned; // never cleared: lets hosts account for a destroyed partial actor
    TArray<TObjectPtr<UObject>> KeepAlive; // session packages between build steps
    FString Session;
    FString MeshCacheNamespace;
    bool bDeferCachedRigidGeometry = true;
    FSCImportMaterials Materials;
    TFuture<void> MaterialPrep;
    double MaterialStart = 0;

    TArray<FMeshSlot> Slots;
    int32 Current = 0;
    TMultiMap<FString, FString> Sidecars;
    TMap<FString, TUniquePtr<FSCDerivedMesh>> Derived;
    TMap<FString, TArray<UStaticMesh*>> Meshes;
    TMap<FString, TArray<int32>> SocketNodes;
    TMap<FString, USkeletalMesh*> RigMeshes;
    TMap<FString, TSet<int32>> MeshTranslucentSlots;
    TMap<FString, TSet<int32>> GamePhysicsVariants; // per mesh asset: variants holding the game's colliders (WP-P3)
    TSet<FString> GamePhysicsReplacesRender; // mesh assets whose every blocking collider was built: render ignores the pawn
    TMap<FString, TArray<int32>> MeshSlotTriangles; // host-paint weighting for POM decal tint
    TMap<FString, TMap<int32, FSCScreenFaceGeometry>> MeshScreenGeometry; // WP-P5 floor panel: per mesh asset, screen slot -> triangles
    TMap<FString, TMap<int32, TArray<int32>>> MeshScreenOwners, MeshSoftScreenOwners; // WP-I1: per mesh asset, screen slot -> owning rig nodes (rigid / skinned)

    void Warn(const FString& Path, const FString& Message)
    {
        Result.Warnings.Add(Path + TEXT(": ") + Message);
        UE_LOG(LogSCImport, Warning, TEXT("ASSET_WARNING %s: %s"), *Path, *Message);
    }
    void Fail(const FString& Error)
    {
        Result.Error = Error;
        Stage = ESCImportStage::Failed;
        UE_LOG(LogSCImport, Error, TEXT("IMPORT_FAILED %s"), *Error);
    }
    void Drain()
    {
        if (ReadTask.IsValid()) ReadTask.Wait();
        if (MaterialPrep.IsValid()) MaterialPrep.Wait();
        for (FMeshSlot& Slot : Slots)
        {
            if (Slot.Derive.IsValid()) Slot.Derive.Wait();
            if (Slot.Prepare.IsValid()) Slot.Prepare.Wait();
        }
    }
    void DestroyPartial()
    {
        if (Options.GeometryStreaming) Options.GeometryStreaming->Forget(Vehicle);
        if (Vehicle && IsValid(Vehicle) && Vehicle->GetWorld() && !Vehicle->IsActorBeingDestroyed()) Vehicle->Destroy();
        Vehicle = nullptr;
    }

    bool StepRead(bool bBlocking);
    void Begin();
    void LaunchDerives();
    bool StepMesh(bool bBlocking);
    void LaunchPrepare(FMeshSlot& Slot);
    bool DeferCachedRigidPart(FPart& Part, const FSCDerivedMesh& Data, TArray<FTriangleID>& Triangles,
        const TSet<int32>& TranslucentSlots, const FString& BaseKey);
    void ProbeMeshCache(FPart::FCacheVariant& Variant, const FString& Key);
    void BeginBuild(FMeshSlot& Slot);
    void BuildNextVariant(FMeshSlot& Slot);
    void FinishBuild(FMeshSlot& Slot);
    bool HasPendingWorkers() const;
    bool StepMaterials(bool bBlocking);
    void Assemble();
};

/** A finished import's state plus the first actor's asset references, copied onto every actor assembled from it. The
 *  arrays hold every shared object (the state's own maps point at them without holding them). */
class FSCImportedAssets : public FGCObject
{
public:
    /** Answers nothing: the package bytes are released on retention; gobo textures come with the shared objects. */
    class FReleasedSource final : public FSCPackageSource
    {
    public:
        bool Exists(const FString&) const override { return false; }
        bool ReadBytes(const FString& Path, TArray<uint8>&, FString& Error) const override
        {
            Error = TEXT("package bytes released when the import's assets were retained: ") + Path;
            return false;
        }
        TArray<FString> ListFiles(const FString&) const override { return {}; }
    };
    TSharedPtr<FSCImportJob::FState> State;
    FReleasedSource Released;
    TArray<TObjectPtr<UStaticMesh>> Meshes;
    TArray<TObjectPtr<USkeletalMesh>> SkeletalMeshes;
    TArray<TObjectPtr<UTexture2D>> Textures;
    TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials, EngineGlowMaterials;
    TMap<TObjectPtr<UObject>, FString> SourcePaths;           // keys are in the arrays above
    TMap<TObjectKey<UObject>, TArray<FVector3f>> MeshSamples;
    TMap<const USkeletalMesh*, TSharedPtr<const FSCDerivedMesh>> RigSources; // editor hosts (bRetainRigSources)
    TMap<FString, TObjectPtr<UTexture2D>> GoboTextures;       // values are in Textures
    TObjectPtr<USCClipCatalog> Catalog;                       // the clips with their bindings and sequences

    void Snapshot(const ASCVehicle* V)
    {
        Meshes = V->Meshes;
        SkeletalMeshes = V->SkeletalMeshes;
        Textures = V->Textures;
        Materials = V->Materials;
        EngineGlowMaterials = V->EngineGlowMaterials;
        SourcePaths = V->SourcePaths;
        MeshSamples = V->MeshSamples;
        RigSources = V->RigSources;
        GoboTextures = V->GoboTextures;
        Catalog = NewObject<USCClipCatalog>(GetTransientPackage(), NAME_None, RF_Transient);
        if (V->ClipCatalog) Catalog->Clips = V->ClipCatalog->Clips;
    }
    void CopyTo(ASCVehicle* V) const
    {
        V->Meshes = Meshes;
        V->SkeletalMeshes = SkeletalMeshes;
        V->Textures = Textures;
        V->Materials = Materials;
        V->EngineGlowMaterials = EngineGlowMaterials;
        V->SourcePaths = SourcePaths;
        V->MeshSamples = MeshSamples;
        V->RigSources = RigSources;
        V->GoboTextures = GoboTextures;
        V->ClipCatalog = NewObject<USCClipCatalog>(V);
        if (Catalog) V->ClipCatalog->Clips = Catalog->Clips;
    }
    /** Objects an assembly created on demand into the shared caches (a host-tinted variant first asked for by this
     *  actor) stay valid for the next one; returns how many were added. */
    int32 Adopt(const ASCVehicle* V)
    {
        const int32 Before = Materials.Num() + Textures.Num();
        TSet<const UObject*> Known;
        for (const UMaterialInstanceDynamic* M : Materials) Known.Add(M);
        for (const UTexture2D* T : Textures) Known.Add(T);
        for (UMaterialInstanceDynamic* M : V->Materials)
            if (M && !Known.Contains(M)) Materials.Add(M);
        for (UTexture2D* T : V->Textures)
            if (T && !Known.Contains(T)) Textures.Add(T);
        return Materials.Num() + Textures.Num() - Before;
    }
    int32 Count() const { return Meshes.Num() + SkeletalMeshes.Num() + Textures.Num() + Materials.Num(); }

    virtual void AddReferencedObjects(FReferenceCollector& Collector) override
    {
        Collector.AddReferencedObjects(Meshes);
        Collector.AddReferencedObjects(SkeletalMeshes);
        Collector.AddReferencedObjects(Textures);
        Collector.AddReferencedObjects(Materials);
        Collector.AddReferencedObjects(EngineGlowMaterials);
        Collector.AddReferencedObject(Catalog);
    }
    virtual FString GetReferencerName() const override { return TEXT("FSCImportedAssets"); }
};

FSCImportJob::FSCImportJob(const FSCPackageSource& Source, FSCImportOptions Options, TSharedPtr<const FSCPackageSource> SourceOwner)
    : S(MakeShared<FState>())
{
    check(IsInGameThread());
    S->RunOwner = this;
    S->Source = &Source;
    S->SourceOwner = MoveTemp(SourceOwner);
    S->Options = MoveTemp(Options);
    S->Start = FPlatformTime::Seconds();
    if (!S->Options.World) S->Fail(TEXT("Import requires a world"));
}
FSCImportJob::FSCImportJob(TSharedRef<FSCImportedAssets> InAssets, FSCImportOptions InOptions)
{
    check(IsInGameThread());
    if (!InAssets->State || InAssets->State->RunOwner || !InOptions.World)
    {
        // Nothing of the asset set is touched: this job fails on a state of its own.
        S = MakeShared<FState>();
        S->RunOwner = this;
        S->Start = FPlatformTime::Seconds();
        S->Fail(!InOptions.World ? TEXT("Import requires a world") : TEXT("The asset set is in use by another assembly"));
        return;
    }
    Assets = InAssets;
    S = InAssets->State;
    S->RunOwner = this;
    // Per-run state; what the import built (meshes, derived data, materials, the manifest) stays as it is.
    FSCImportOptions Merged = S->Options;
    Merged.World = InOptions.World;
    Merged.ActorTransform = InOptions.ActorTransform;
    Merged.bPublish = InOptions.bPublish;
    Merged.bReplacePrevious = InOptions.bReplacePrevious;
    Merged.bRetainAssets = false;
    S->Options = Merged;
    S->Result = FSCImportResult();
    S->Timings = FSCImportTimings();
    S->bCancel = false;
    S->bAssetsSynced = false;
    S->Start = FPlatformTime::Seconds();
    S->Vehicle = nullptr;
    S->Spawned = nullptr;
    S->Stage = ESCImportStage::Assembling;
    FActorSpawnParameters Spawn;
    Spawn.ObjectFlags |= RF_Transient;
    ASCVehicle* V = S->Options.World->SpawnActor<ASCVehicle>(ASCVehicle::StaticClass(), S->Options.ActorTransform, Spawn);
    if (!V)
    {
        S->Fail(TEXT("Cannot spawn vehicle"));
        return;
    }
    S->Vehicle = V;
    S->Spawned = V;
    V->Manifest = S->Manifest;
    V->SessionPath = S->Session; // the shared objects live in the original import's packages
    Assets->CopyTo(V);
    S->Materials.Rebind(V);
}
FSCImportJob::~FSCImportJob()
{
    if (S->RunOwner != this) return; // an asset set's state run by a later assembly, or by nobody
    S->Drain();
    if (S->Stage != ESCImportStage::Succeeded) S->DestroyPartial();
    S->RunOwner = nullptr;
    S->Vehicle = nullptr; // the actor lives on in its world; a retained state references no actor
}
void FSCImportJob::AddReferencedObjects(FReferenceCollector& Collector)
{
    if (S->RunOwner != this) return;
    Collector.AddReferencedObject(S->Vehicle);
    Collector.AddReferencedObjects(S->KeepAlive);
}
int32 FSCImportJob::CountRetainedObjects(const FSCImportedAssets& Retained) { return Retained.Count(); }
void FSCImportJob::RetainAssets()
{
    ASCVehicle* V = S->Vehicle;
    if (!V) return;
    TSharedRef<FSCImportedAssets> Retained = MakeShared<FSCImportedAssets>();
    Retained->State = S;
    Retained->Snapshot(V);
    // Assembly needs helper frames, rig nodes, material names and the rigs' collision source; per-triangle and
    // per-vertex maps and the clip JSON only served derivation and binding.
    S->ClipJson.Empty();
    for (auto& Pair : S->Derived)
        if (Pair.Value)
        {
            Pair.Value->TriangleNodes.Empty();
            Pair.Value->SkinWeights.Empty();
        }
    S->Source = &Retained->Released;
    S->SourceOwner.Reset();
    // Shared objects the first actor owns (textures, material instances, clip sequences) move to the import's session
    // package: the asset set and every actor assembled from it outlive any one actor, the first included.
    UPackage* SharedPackage = CreatePackage(*(S->Session / TEXT("Shared")));
    SharedPackage->SetFlags(RF_Transient);
    int32 Moved = 0;
    auto Detach = [&](UObject* Object) {
        if (!Object || Object->GetOuter() != V) return;
        Object->Rename(*MakeUniqueObjectName(SharedPackage, Object->GetClass(), Object->GetFName()).ToString(), SharedPackage,
                       REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
        ++Moved;
    };
    for (UTexture2D* T : Retained->Textures) Detach(T);
    for (UMaterialInstanceDynamic* M : Retained->Materials) Detach(M);
    for (FSCImportedClip& Clip : Retained->Catalog->Clips)
    {
        Detach(Clip.Sequence);
        for (FSCClipBinding& Binding : Clip.Bindings)
            for (UAnimSequence* Sequence : Binding.Sequences) Detach(Sequence);
    }
    Assets = Retained;
    UE_LOG(LogSCImport, Display, TEXT("IMPORT_ASSETS_RETAINED session=%s objects=%d meshes=%d rigs=%d textures=%d materials=%d detached=%d"),
           *S->Session, Retained->Count(), Retained->Meshes.Num(), Retained->SkeletalMeshes.Num(), Retained->Textures.Num(),
           Retained->Materials.Num(), Moved);
}
FString FSCImportJob::GetSessionPath() const { return S->Session; }
ASCVehicle* FSCImportJob::GetSpawnedVehicle() const { return S->Spawned.Get(/*bEvenIfGarbage*/ true); }
bool FSCImportJob::HasPendingWorkers() const { return S->HasPendingWorkers(); }
bool FSCImportJob::FState::HasPendingWorkers() const
{
    auto Pending = [](const auto& Future) { return Future.IsValid() && !Future.IsReady(); };
    if (Pending(ReadTask) || Pending(MaterialPrep)) return true;
    for (const FMeshSlot& Slot : Slots)
        if (Pending(Slot.Derive) || Pending(Slot.Prepare)) return true;
    return false;
}
void FSCImportJob::Cancel() { S->bCancel = true; }
ESCImportStage FSCImportJob::GetStage() const { return S->Stage; }
bool FSCImportJob::IsFinished() const { return S->Stage >= ESCImportStage::Succeeded; }
const FSCImportTimings& FSCImportJob::GetTimings() const { return S->Timings; }
const FSCImportResult& FSCImportJob::GetResult() const { return S->Result; }
FSCImportResult& FSCImportJob::GetResult() { return S->Result; }

FString FSCImportJob::GetPhase() const
{
    const int32 N = S->Slots.Num();
    switch (S->Stage)
    {
    case ESCImportStage::Reading: return TEXT("Reading package");
    case ESCImportStage::Meshes:
    {
        if (!S->Slots.IsValidIndex(S->Current)) return TEXT("Building meshes");
        const FMeshSlot& Slot = S->Slots[S->Current];
        if (Slot.bBuildBegun)
        {
            const FPart* Part = Slot.Parts.IsValidIndex(Slot.NextPart) ? &Slot.Parts[Slot.NextPart] : nullptr;
            const bool bCached = Part && (Slot.bTranslucentNext ? Part->TranslucentCache : Part->OpaqueCache).Pin;
            return FString::Printf(TEXT("%s mesh %d/%d (part %d/%d)"), bCached ? TEXT("Loading cached") : TEXT("Building"),
                S->Current + 1, N, FMath::Min(Slot.NextPart + 1, Slot.Parts.Num()), Slot.Parts.Num());
        }
        const TCHAR* Step = !Slot.bPrepareLaunched ? TEXT("Deriving mesh")
                            : S->Options.bBuildCollision ? TEXT("Deriving collision") : TEXT("Preparing mesh");
        return FString::Printf(TEXT("%s %d/%d"), Step, S->Current + 1, N);
    }
    case ESCImportStage::Materials: return TEXT("Creating textures and materials");
    case ESCImportStage::Assembling: return TEXT("Assembling actor");
    case ESCImportStage::Succeeded: return TEXT("Imported");
    case ESCImportStage::Failed: return TEXT("Import failed");
    case ESCImportStage::Cancelled: return TEXT("Import cancelled");
    }
    return FString();
}
float FSCImportJob::GetFraction() const
{
    switch (S->Stage)
    {
    case ESCImportStage::Reading: return 0;
    case ESCImportStage::Meshes:
        return ReadShare + MeshShare * (S->Slots.IsEmpty() ? 1.f : float(S->Current) / float(S->Slots.Num()));
    case ESCImportStage::Materials: return ReadShare + MeshShare;
    case ESCImportStage::Assembling: return ReadShare + MeshShare + MaterialShare;
    default: return 1;
    }
}

bool FSCImportJob::Tick(bool bBlocking, double BudgetSeconds)
{
    check(IsInGameThread());
    const double TickStart = FPlatformTime::Seconds();
    while (!IsFinished())
    {
        if (S->bCancel)
        {
            S->Drain();
            S->DestroyPartial();
            S->Result.Error = TEXT("Import cancelled");
            S->Stage = ESCImportStage::Cancelled;
            UE_LOG(LogSCImport, Display, TEXT("IMPORT_CANCELLED after %.3f s"), FPlatformTime::Seconds() - S->Start);
            break;
        }
        if (S->Vehicle && !IsValid(S->Vehicle))
        {
            S->Drain();
            S->Fail(TEXT("Vehicle destroyed during import (world teardown)"));
            break;
        }
        bool bProgressed = false;
        switch (S->Stage)
        {
        case ESCImportStage::Reading: bProgressed = S->StepRead(bBlocking); break;
        case ESCImportStage::Meshes: bProgressed = S->StepMesh(bBlocking); break;
        case ESCImportStage::Materials: bProgressed = S->StepMaterials(bBlocking); break;
        case ESCImportStage::Assembling:
            S->Assemble();
            bProgressed = true;
            break;
        default: break;
        }
        if (!bProgressed) break; // waiting on a worker
        if (!bBlocking && FPlatformTime::Seconds() - TickStart >= BudgetSeconds) break;
    }
    if (IsFinished()) S->Result.TotalSeconds = FPlatformTime::Seconds() - S->Start;
    if (IsFinished() && S->Stage == ESCImportStage::Succeeded && S->RunOwner == this && !S->bAssetsSynced)
    {
        S->bAssetsSynced = true;
        if (!Assets && S->Options.bRetainAssets) RetainAssets();
        else if (Assets && S->Vehicle)
            if (const int32 Added = Assets->Adopt(S->Vehicle))
                UE_LOG(LogSCImport, Display, TEXT("IMPORT_ASSETS_ADOPTED session=%s objects=%d"), *S->Session, Added);
    }
    return IsFinished();
}

bool FSCImportJob::FState::StepRead(bool bBlocking)
{
    if (!bReadLaunched)
    {
        bReadLaunched = true;
        ReadTask = Async(EAsyncExecution::ThreadPool, [this] {
            const double ReadStart = FPlatformTime::Seconds();
            if (!FSCPackageReader::Read(*Source, Manifest, ReadError, Options.ScenePath, &ReadWarnings)) return false;
            for (const auto& Record : Options.bImportClips ? Manifest.Clips : TArray<FSCClipRecord>())
            {
                FSCImportedClip Clip;
                TSharedPtr<FJsonObject> Json;
                FString Error;
                if (!FSCAnimImporter::Read(*Source, Record, Manifest.Root.EntityName, Clip, Json, Error))
                {
                    ClipWarnings.Emplace(Record.Sidecar, Error);
                    continue;
                }
                ReadClips.Add(MoveTemp(Clip));
                ClipJson.Add(MoveTemp(Json));
            }
            Timings.Read = FPlatformTime::Seconds() - ReadStart;
            return true;
        });
        return true;
    }
    if (!bBlocking && !ReadTask.IsReady()) return false;
    const bool bOK = ReadTask.Get();
    ReadTask = TFuture<bool>();
    if (!bOK)
    {
        Fail(ReadError);
        return true;
    }
    Begin();
    return true;
}

void FSCImportJob::FState::Begin()
{
    Session = TEXT("/Temp/SC/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    if (Options.ProcessedCache && Options.ProcessedCache->IsEnabled() && !Options.CacheArchiveBuild.IsEmpty())
        MeshCacheNamespace = FSCProcessedCache::Key(Options.CacheArchiveBuild, SCProcessedMeshCache::RenderFormat());
#if WITH_DEV_AUTOMATION_TESTS
    // Compare the same valid warm packages with and without rebuilding CPU descriptions.
    bDeferCachedRigidGeometry = !FParse::Param(FCommandLine::Get(), TEXT("SCDisableDeferredRigidGeometry"));
#endif
    FActorSpawnParameters Spawn;
    Spawn.ObjectFlags |= RF_Transient;
    Vehicle = Options.World->SpawnActor<ASCVehicle>(ASCVehicle::StaticClass(), Options.ActorTransform, Spawn);
    if (!Vehicle)
    {
        Fail(TEXT("Cannot spawn vehicle"));
        return;
    }
    Spawned = Vehicle;
    Vehicle->Manifest = Manifest;
    Vehicle->SessionPath = Session;
    Vehicle->ClipCatalog = NewObject<USCClipCatalog>(Vehicle);
    for (const FString& Warning : ReadWarnings)
    {
        Result.Warnings.Add(Warning);
        UE_LOG(LogSCImport, Warning, TEXT("ASSET_WARNING %s"), *Warning);
    }
    for (const auto& W : ClipWarnings)
        Warn(W.Key, W.Value);
    Vehicle->ClipCatalog->Clips = MoveTemp(ReadClips);
    // Material preparation (describe + texture decode) runs on a worker while meshes are derived and built;
    // textures and MIDs are created on the game thread once it is done, before components are placed.
    MaterialStart = FPlatformTime::Seconds();
    if (Options.bImportMaterials)
    {
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
        MaterialPrep = Async(EAsyncExecution::ThreadPool, [this] {
            Materials.Describe(Manifest);
            const FSCTextureCacheRequest Cache{Options.ProcessedCache.Get(), Options.CacheArchiveBuild, Options.bRebuildProcessed};
            Materials.Decode(*Source, &Cache);
        });
    }
    auto CollectSidecar = [&](const FSCSceneInstance& I) { Sidecars.AddUnique(I.MeshAsset, I.MaterialSidecar); };
    CollectSidecar(Manifest.Root);
    for (const auto& I : Manifest.Children)
        CollectSidecar(I);
    for (const auto& Interior : Manifest.Interiors)
        for (const auto& I : Interior.Placements)
            CollectSidecar(I);
    for (const FString& Path : Manifest.MeshAssets())
        Slots.AddDefaulted_GetRef().Path = Path;
    Stage = ESCImportStage::Meshes;
    LaunchDerives();
}

void FSCImportJob::FState::LaunchDerives()
{
    // Derivation is pure (reads the const source, writes its own slot); several assets run ahead of the build.
    for (int32 I = Current; I < FMath::Min(Slots.Num(), Current + DeriveLookAhead()); ++I)
    {
        FMeshSlot& Slot = Slots[I];
        if (Slot.bDeriveLaunched) continue;
        Slot.bDeriveLaunched = true;
        Slot.Derive = Async(EAsyncExecution::ThreadPool, [this, &Slot] {
            const double DeriveStart = FPlatformTime::Seconds();
            const bool bOK = FSCMeshImporter::Derive(*Source, Slot.Path, *Slot.Data, Slot.DeriveError);
            Slot.DeriveSeconds = FPlatformTime::Seconds() - DeriveStart;
            return bOK;
        });
    }
}

void FSCImportJob::FState::ProbeMeshCache(FPart::FCacheVariant& Variant, const FString& Key)
{
    Variant.Key = FSCProcessedCache::Key(MeshCacheNamespace, Key);
    if (!Options.bRebuildProcessed) Variant.Pin = Options.ProcessedCache->PinEntry(Variant.Key);
}

bool FSCImportJob::FState::DeferCachedRigidPart(FPart& Part, const FSCDerivedMesh& Data, TArray<FTriangleID>& Triangles,
    const TSet<int32>& TranslucentSlots, const FString& BaseKey)
{
    if (!bDeferCachedRigidGeometry || !Part.OpaqueCache.Pin) return false;
    TArray<FTriangleID> Opaque, Blend;
    if (!TranslucentSlots.IsEmpty())
        for (FTriangleID Triangle : Triangles)
            (TranslucentSlots.Contains(Data.Description.GetTrianglePolygonGroup(Triangle).GetValue()) ? Blend : Opaque).Add(Triangle);

    const bool bSplit = !Opaque.IsEmpty() && !Blend.IsEmpty();
    if (bSplit)
    {
        ProbeMeshCache(Part.TranslucentCache, BaseKey + TEXT("|translucent"));
        if (!Part.TranslucentCache.Pin) return false;
        Part.TranslucentCache.SplitTriangles = MoveTemp(Blend);
        Part.OpaqueCache.SplitTriangles = MoveTemp(Opaque);
        Part.TranslucentCache.LightSamples = FSCLightAreas::Sample(Data.Description, Part.TranslucentCache.SplitTriangles);
        Part.TranslucentCache.bDisableNanite = true;
        Part.Translucent.SocketNode = Part.Owner;
        Part.Translucent.bHasGeometry = true;
    }

    Part.OpaqueCache.bDisableNanite = !bSplit && !Blend.IsEmpty();
    Part.OpaqueCache.LightSamples = FSCLightAreas::Sample(Data.Description, bSplit ? Part.OpaqueCache.SplitTriangles : Triangles);
    Part.Mesh.SocketNode = Part.Owner;
    Part.Mesh.bHasGeometry = true;
    Part.CollisionTriangles = MoveTemp(Triangles);
    Part.bDeferredGeometry = true;
    return true;
}

void FSCImportJob::FState::LaunchPrepare(FMeshSlot& Slot)
{
    Slot.bPrepareLaunched = true;
    Slot.PrepareWaitStart = FPlatformTime::Seconds();
    // Runs while no game-thread build is in flight: it is the only writer of the clip catalogue at this time.
    Slot.Prepare = Async(EAsyncExecution::ThreadPool, [this, &Slot] {
        const double PrepareStart = FPlatformTime::Seconds();
        ON_SCOPE_EXIT { Slot.PrepareSeconds = FPlatformTime::Seconds() - PrepareStart; };
        FSCDerivedMesh& Data = *Slot.Data;
        const FString& Path = Slot.Path;
        TSet<int32> TranslucentSlots = Data.BlendedSlots;
        auto IsTranslucent = [](const FString& Family) {
            return Family.StartsWith(TEXT("Glass"), ESearchCase::IgnoreCase) || Family.Equals(TEXT("MeshDecal"), ESearchCase::IgnoreCase);
        };
        for (int32 Slot2 = 0; Slot2 < Data.MaterialFamilies.Num(); ++Slot2)
            if (IsTranslucent(Data.MaterialFamilies[Slot2])) TranslucentSlots.Add(Slot2);
        TArray<FString> Paths;
        Sidecars.MultiFind(Path, Paths);
        TSet<int32> ScreenSlots; // DisplayScreen submaterials in any sidecar of this mesh: the faces ui_bindings may light
        TSet<int32> DescribedSplit; // slots the material descriptions send to the non-Nanite split
        for (const auto& Sidecar : Paths)
        {
            const auto* Set = Manifest.Materials.FindByPredicate([&](const FSCMaterialSet& M) { return M.Path == Sidecar; });
            if (!Set) continue;
            for (int32 MaterialSlot = 0; MaterialSlot < Data.MaterialNames.Num(); ++MaterialSlot)
                if (const FSCSubmaterial* Sub = FSCImportMaterials::Resolve(Set, Data.MaterialSourceNames, MaterialSlot))
                {
                    if (Sub->ShaderFamily == TEXT("DisplayScreen")) ScreenSlots.Add(MaterialSlot);
                    const bool bGlowTarget = Manifest.EngineGlow.Targets.ContainsByPredicate([&](const FSCEngineGlowTarget& T) {
                        return T.MaterialSidecar == Sidecar && T.SourceMaterialIndex == Sub->SourceIndex;
                    });
                    if (FSCMaterialBuilder::RequiresNonNanite(*Sub, bGlowTarget)) { TranslucentSlots.Add(MaterialSlot); DescribedSplit.Add(MaterialSlot); } // D6
                }
        }
        // A screen's coverage is its description's (M_SC_Screen opaque, or _Translucent for authored partial opacity): a
        // GLB-only BLEND on a screen slot whose sidecar describes it opaque must not put an opaque screen in the split.
        for (int32 Screen : ScreenSlots)
            if (Data.BlendedSlots.Contains(Screen) && !DescribedSplit.Contains(Screen)) TranslucentSlots.Remove(Screen);
        Slot.TranslucentSlots = TranslucentSlots;
        Slot.SlotTriangles.SetNumZeroed(Data.MaterialNames.Num());
        for (auto T : Data.Description.Triangles().GetElementIDs())
        {
            const int32 Group = Data.Description.GetTrianglePolygonGroup(T).GetValue();
            if (Slot.SlotTriangles.IsValidIndex(Group)) ++Slot.SlotTriangles[Group];
        }
        // Binding is CPU-only. Static assets with no receiving channels retain the U1/U2 path.
        for (int32 Index = 0; Index < ClipJson.Num(); ++Index)
        {
            FSCClipBinding Binding;
            FString Error;
            if (!FSCAnimImporter::DeriveBinding(ClipJson[Index], Data, Path, Vehicle->ClipCatalog->Clips[Index], Binding, Error))
            {
                Slot.PrepareWarnings.Emplace(Path, Error);
                continue;
            }
            if (!Binding.Tracks.IsEmpty()) Slot.Bindings.Add(Index, MoveTemp(Binding));
        }
        Slot.bAnimated = !Slot.Bindings.IsEmpty() || (Options.bRigSkinnedMeshes && !Data.SkinWeights.IsEmpty());
        TArray<FTriangleID> Soft;
        TMap<int32, TArray<FTriangleID>> Rigid;
        for (auto T : Data.Description.Triangles().GetElementIDs())
        {
            const auto V = Data.Description.GetTriangleVertices(T);
            const bool bSoft = Slot.bAnimated && Data.SkinWeights.Contains(V[0]);
            if (bSoft)
                Soft.Add(T);
            else
                Rigid.FindOrAdd(Slot.bAnimated ? Data.TriangleNodes.FindRef(T) : INDEX_NONE).Add(T);
            // WP-I1: which nodes draw each screen slot, per drawn mesh (the skeletal soft part, or the rigid parts), so a
            // binding's helper can claim its own face.
            const int32 Group = Data.Description.GetTrianglePolygonGroup(T).GetValue();
            if (ScreenSlots.Contains(Group))
                (bSoft ? Slot.SoftScreenOwners : Slot.ScreenOwners).FindOrAdd(Group).AddUnique(Data.TriangleNodes.FindRef(T, INDEX_NONE));
        }
        for (int32 Screen : ScreenSlots)
            if (FSCScreenFaceGeometry G; FSCScreenFaceGeometry::FromDescription(Data.Description, Screen,
                    [&](FTriangleID T) { return Data.TriangleNodes.FindRef(T, INDEX_NONE); },
                    [&](FTriangleID T) { return Slot.bAnimated && Data.SkinWeights.Contains(Data.Description.GetTriangleVertices(T)[0]); }, G))
                Slot.ScreenGeometry.Add(Screen, MoveTemp(G));
        Slot.SoftTriangles = Soft.Num();
        Slot.RigidGroups = Rigid.Num();
        if (Slot.bAnimated)
        {
            Slot.Soft = FSCMeshImporter::Select(Data, Soft);
            Slot.Soft.GltfToRig = Data.GltfToRig;
        }
        if (Options.bBuildCollision && Slot.Soft.bHasGeometry)
        {
            FString Error;
            if (!FSCSkeletalMeshImporter::DeriveCollision(Slot.Soft, Error)) Slot.PrepareWarnings.Emplace(Path, Error);
            Slot.CollisionSeconds += Slot.Soft.CollisionDeriveSeconds;
        }
        TArray<int32> Owners;
        Rigid.GetKeys(Owners);
        Owners.Sort();
        FString SplitKey;
        if (!MeshCacheNamespace.IsEmpty())
        {
            TArray<int32> SortedSlots = TranslucentSlots.Array();
            SortedSlots.Sort();
            for (int32 MaterialSlot : SortedSlots) SplitKey += FString::Printf(TEXT(",%d"), MaterialSlot);
        }
        for (int32 Owner : Owners)
        {
            FPart Part;
            Part.Owner = Owner;
            FString BaseKey;
            if (!MeshCacheNamespace.IsEmpty() && !Data.ContentHash.IsEmpty())
            {
                BaseKey = FSCProcessedCache::Key(Path,
                    FString::Printf(TEXT("%s|owner=%d|animated=%d|split=%s"), *Data.ContentHash, Owner, Slot.bAnimated, *SplitKey));
                ProbeMeshCache(Part.OpaqueCache, BaseKey + (Options.bBuildCollision ? TEXT("|visual+collision") : TEXT("|visual")));
            }
            if (DeferCachedRigidPart(Part, Data, Rigid[Owner], TranslucentSlots, BaseKey))
            {
                ++Slot.DeferredRigidParts;
                Slot.Parts.Add(MoveTemp(Part));
                continue;
            }
            Part.Mesh = FSCMeshImporter::Select(Data, Rigid[Owner]);
            Part.Mesh.SocketNode = Owner;
            // Collision includes all triangles of this owner, including glass/decal split, as in U1.
            FString Error;
            if (Options.bBuildCollision && !Part.OpaqueCache.Pin && !FSCMeshImporter::DeriveCollision(Part.Mesh, Error))
            {
                Slot.PrepareWarnings.Emplace(Path, Error);
                continue;
            }
            if (Options.bBuildCollision && Part.OpaqueCache.Pin)
                Part.CollisionTriangles = MoveTemp(Rigid[Owner]);
            Slot.CollisionSeconds += Part.Mesh.CollisionDeriveSeconds;
            FSCMeshImporter::SplitTranslucent(Part.Mesh, TranslucentSlots, Part.Translucent);
            if (Part.Translucent.bHasGeometry && !BaseKey.IsEmpty() && Part.TranslucentCache.Key.IsEmpty())
                ProbeMeshCache(Part.TranslucentCache, BaseKey + TEXT("|translucent"));
            if (Options.GeometryStreaming && !Options.bRebuildProcessed && ScreenSlots.IsEmpty())
            {
                auto PrepareCoarse = [&](FPart::FCacheVariant& Cache, const FSCDerivedMesh& Variant) {
                    if (Cache.Pin || !Variant.bHasGeometry ||
                        Variant.Description.Triangles().Num() > PerVehicleTriangleBudget - FineTrianglesDeferred) return;
                    auto Coarse = MakeUnique<FSCDerivedMesh>();
                    if (Options.GeometryStreaming->Reduce(Variant, *Coarse, Cache.Deviation))
                        Cache.Coarse = MoveTemp(Coarse);
                };
                PrepareCoarse(Part.OpaqueCache, Part.Mesh);
                PrepareCoarse(Part.TranslucentCache, Part.Translucent);
            }
            Slot.Parts.Add(MoveTemp(Part));
        }
        // WP-P3: the game's colliders, one collision-only part per owner (the rig node they move with).
        if (Options.bBuildCollision && Data.bHasGamePhysics)
        {
            TArray<int32> PhysicsOwners;
            if (!Slot.bAnimated) PhysicsOwners.Add(INDEX_NONE);
            else
            {
                for (int32 Node : Data.PhysicsTriangleNodes) PhysicsOwners.AddUnique(Node);
                PhysicsOwners.Sort();
            }
            for (int32 Owner : PhysicsOwners)
            {
                FPart Part;
                Part.Owner = Owner;
                Part.bGamePhysics = true;
                Part.Mesh = FSCMeshImporter::SelectPhysics(Data, Owner);
                if (!Part.Mesh.bHasGeometry) continue;
                if (!MeshCacheNamespace.IsEmpty() && !Data.ContentHash.IsEmpty())
                    ProbeMeshCache(Part.OpaqueCache, FSCProcessedCache::Key(Path,
                        FString::Printf(TEXT("%s|physics=%d|animated=%d"), *Data.ContentHash, Owner, Slot.bAnimated)));
                FString Error;
                if (!Part.OpaqueCache.Pin && !FSCMeshImporter::DeriveCollision(Part.Mesh, Error))
                {
                    Slot.PrepareWarnings.Emplace(Path, Error);
                    Slot.bGamePhysicsFailed = true;
                    continue;
                }
                Slot.CollisionSeconds += Part.Mesh.CollisionDeriveSeconds;
                Slot.Parts.Add(MoveTemp(Part));
            }
        }
    });
}

bool FSCImportJob::FState::StepMesh(bool bBlocking)
{
    if (!Slots.IsValidIndex(Current))
    {
        Stage = ESCImportStage::Materials;
        return true;
    }
    LaunchDerives();
    FMeshSlot& Slot = Slots[Current];
    if (!Slot.bPrepareLaunched)
    {
        if (Slot.DeriveWaitStart <= 0.) Slot.DeriveWaitStart = FPlatformTime::Seconds();
        if (!bBlocking && !Slot.Derive.IsReady()) return false;
        const bool bOK = Slot.Derive.Get();
        Slot.Derive = TFuture<bool>();
        Slot.DeriveWaitSeconds = FPlatformTime::Seconds() - Slot.DeriveWaitStart;
        Timings.Derive += Slot.DeriveSeconds;
        Timings.DeriveWait += Slot.DeriveWaitSeconds;
        if (!bOK)
        {
            Warn(Slot.Path, Slot.DeriveError);
            Slot.Data.Reset();
            ++Current;
            return true;
        }
        for (const auto& Warning : Slot.Data->Warnings)
            Warn(Slot.Path, Warning);
        LaunchPrepare(Slot);
        return true;
    }
    if (!Slot.bBuildBegun)
    {
        if (!bBlocking && !Slot.Prepare.IsReady()) return false;
        Slot.Prepare.Wait();
        Slot.Prepare = TFuture<void>();
        const double PrepareWait = FPlatformTime::Seconds() - Slot.PrepareWaitStart;
        Timings.Prepare += Slot.PrepareSeconds;
        Timings.PrepareWait += PrepareWait;
        Timings.DeferredRigidParts += Slot.DeferredRigidParts;
        for (const auto& W : Slot.PrepareWarnings)
            Warn(W.Key, W.Value);
        Timings.Collision += Slot.CollisionSeconds;
        if (Options.bBuildCollision)
            UE_LOG(LogSCImport, Display, TEXT("COLLISION_DERIVE mesh=%s exact_s=%.6f derive_worker_s=%.4f derive_wait_s=%.4f prepare_worker_s=%.4f prepare_wait_s=%.4f deferred_rigid_parts=%d"),
                *Slot.Path, Slot.CollisionSeconds, Slot.DeriveSeconds, Slot.DeriveWaitSeconds, Slot.PrepareSeconds, PrepareWait, Slot.DeferredRigidParts);
    }
    const double BuildStart = FPlatformTime::Seconds(); // game-thread build time only, not the wait for workers
    ON_SCOPE_EXIT { Timings.Build += FPlatformTime::Seconds() - BuildStart; };
    if (!Slot.bBuildBegun)
        BeginBuild(Slot); // package + rig: one step
    else if (!Slot.bBuildFailed && Slot.NextPart < Slot.Parts.Num())
        BuildNextVariant(Slot); // one UStaticMesh build per step: the budget and cancellation are checked between them
    if (Slot.bBuildFailed || Slot.NextPart >= Slot.Parts.Num())
    {
        FinishBuild(Slot);
        ++Current;
    }
    return true;
}

void FSCImportJob::FState::BeginBuild(FMeshSlot& Slot)
{
    Slot.bBuildBegun = true;
    const FString& Path = Slot.Path;
    MeshSlotTriangles.Add(Path, MoveTemp(Slot.SlotTriangles));
    MeshScreenOwners.Add(Path, MoveTemp(Slot.ScreenOwners));
    MeshScreenGeometry.Add(Path, MoveTemp(Slot.ScreenGeometry));
    MeshSoftScreenOwners.Add(Path, MoveTemp(Slot.SoftScreenOwners));
    Slot.Package = CreatePackage(*AssetPackage(Session, Path));
    Slot.Package->SetFlags(RF_Transient);
    KeepAlive.Add(Slot.Package);
    if (!Slot.bAnimated) return;
    FString AssetError;
    USkeleton* Skeleton = nullptr;
    USkeletalMesh* RigMesh = FSCSkeletonBuilder::Build(*Slot.Data, Slot.Package, Skeleton);
    if (!FSCSkeletalMeshImporter::Build(Slot.Soft, RigMesh, AssetError))
    {
        Warn(Path, AssetError);
        Slot.bBuildFailed = true; // the asset is skipped, as before the split
        return;
    }
    Vehicle->SkeletalMeshes.Add(RigMesh);
    if (Options.bRetainRigSources) Vehicle->RigSources.Add(RigMesh, MakeShared<FSCDerivedMesh>(Slot.Soft));
    Vehicle->MeshSamples.Add(RigMesh, FSCLightAreas::Sample(Slot.Soft.Description));
    RigMeshes.Add(Path, RigMesh);
    for (auto& Pair : Slot.Bindings)
        FSCAnimImporter::AttachBinding(RigMesh, Vehicle, Vehicle->ClipCatalog->Clips[Pair.Key], MoveTemp(Pair.Value));
    UE_LOG(LogSCImport, Display, TEXT("RIG path=%s bones=%d soft_triangles=%d rigid_groups=%d"), *Path,
           RigMesh->GetRefSkeleton().GetNum(), Slot.SoftTriangles, Slot.RigidGroups);
}

void FSCImportJob::FState::BuildNextVariant(FMeshSlot& Slot)
{
    const FString& Path = Slot.Path;
    FPart& Part = Slot.Parts[Slot.NextPart];
    const bool bTranslucent = Slot.bTranslucentNext;
    if (!bTranslucent) Slot.PartSuffix = Part.bGamePhysics ? FString::Printf(TEXT("_GameCollision%d"), Part.Owner)
                                         : Slot.Variants.IsEmpty() ? TEXT("") : FString::Printf(TEXT("_Node%d"), Part.Owner);
    const FString Suffix = Slot.PartSuffix + (bTranslucent ? TEXT("_Translucent") : TEXT(""));
    FSCDerivedMesh& Variant = bTranslucent ? Part.Translucent : Part.Mesh;
    auto& Cached = bTranslucent ? Part.TranslucentCache : Part.OpaqueCache;
    const bool bNeedsCollision = !bTranslucent && Options.bBuildCollision;
    FSCMeshBuildStats Stats;
    FString AssetError;
    bool bUsesCoarse = false;
    UStaticMesh* Mesh = Cached.Pin ? SCProcessedMeshCache::Load(*Options.ProcessedCache, *Cached.Pin, Stats) : nullptr;
    Cached.Pin.Reset();
    if (Mesh)
    {
        ++Result.CachedMeshes;
        const FStaticMeshRenderData* Render = Mesh->GetRenderData();
        if (Render->HasValidNaniteData() && Render->NaniteResourcesPtr->HasStreamingData())
        {
            ++CachedNaniteStreamable;
            CachedNaniteDemandPaged += !Render->NaniteResourcesPtr->StreamablePages.IsBulkDataLoaded();
        }
    }
    else
    {
        if (Part.bDeferredGeometry)
        {
            const auto& Triangles = Cached.SplitTriangles.IsEmpty() ? Part.CollisionTriangles : Cached.SplitTriangles;
            Variant = FSCMeshImporter::Select(*Slot.Data, Triangles);
            Variant.SocketNode = Part.Owner;
            Variant.bEnableNanite &= !Cached.bDisableNanite;
        }
        if (bNeedsCollision && Variant.CollisionBytes.IsEmpty())
        {
            // The package failed to load despite valid disk checksums. Re-create
            // collision from the same unsplit triangles used on a regular miss.
            if (Part.bGamePhysics || (Part.bDeferredGeometry && Cached.SplitTriangles.IsEmpty()))
                FSCMeshImporter::DeriveCollision(Variant, AssetError);
            else
            {
                FSCDerivedMesh Unsplit = FSCMeshImporter::Select(*Slot.Data, Part.CollisionTriangles);
                if (FSCMeshImporter::DeriveCollision(Unsplit, AssetError))
                {
                    Variant.CollisionBytes = MoveTemp(Unsplit.CollisionBytes);
                    Variant.CollisionVersions = MoveTemp(Unsplit.CollisionVersions);
                    Variant.CollisionDeriveSeconds = Unsplit.CollisionDeriveSeconds;
                }
            }
            Timings.Collision += Variant.CollisionDeriveSeconds;
        }
        Result.MeshBuilds += Variant.bHasGeometry;
        Result.CollisionBuilds += bNeedsCollision && Variant.bHasGeometry;
        const FName Name(*(SafeName(FPaths::GetBaseFilename(Path)) + Suffix));
        FName ExactName = Name;
        if (Cached.Coarse && Variant.Description.Triangles().Num() <= PerVehicleTriangleBudget - FineTrianglesDeferred &&
            Options.GeometryStreaming->CanDefer(Variant))
        {
            Mesh = FSCMeshImporter::Instantiate(*Cached.Coarse, Slot.Package, Name,
                bNeedsCollision, UMaterial::GetDefaultMaterial(MD_Surface), Stats, AssetError, true);
            bUsesCoarse = Mesh != nullptr;
            if (bUsesCoarse)
                Mesh->SetExtendedBounds(Variant.Description.GetBounds());
            else
            {
                ExactName = MakeUniqueObjectName(Slot.Package, UStaticMesh::StaticClass(), Name);
                Stats = {};
                AssetError.Reset();
            }
        }
        Cached.Coarse.Reset();
        if (!Mesh)
            Mesh = FSCMeshImporter::Instantiate(Variant, Slot.Package, ExactName,
                bNeedsCollision, UMaterial::GetDefaultMaterial(MD_Surface), Stats, AssetError);
        if (Mesh && !Cached.Key.IsEmpty())
        {
            if (!bUsesCoarse && Options.GeometryStreaming)
                Options.GeometryStreaming->QueueWrite(Vehicle, Mesh, Variant, Stats, Options.ProcessedCache, Cached.Key);
            else if (!bUsesCoarse)
                SCProcessedMeshCache::Store(*Options.ProcessedCache, Cached.Key, *Mesh, Variant, Stats);
        }
    }
    if (!Mesh)
    {
        Warn(Path + Suffix, AssetError);
        if (Part.bGamePhysics) Slot.bGamePhysicsFailed = true;
    }
    else
    {
        Vehicle->Meshes.Add(Mesh);
        Vehicle->SourcePaths.Add(Mesh, Path + Suffix);
        if (Part.bGamePhysics) Slot.GamePhysicsVariants.Add(Slot.Variants.Num());
        else Vehicle->MeshSamples.Add(Mesh, Part.bDeferredGeometry ? MoveTemp(Cached.LightSamples) : FSCLightAreas::Sample(Variant.Description));
        Slot.Variants.Add(Mesh);
        SocketNodes.FindOrAdd(Path).Add(Variant.SocketNode);
        Result.MeshStats.Add(Path + Suffix, Stats);
        if (bUsesCoarse)
        {
            if (Variant.bEnableNanite) OpaqueProxies.Add(Mesh);
            FineTrianglesDeferred += Variant.Description.Triangles().Num();
            Options.GeometryStreaming->Defer(Vehicle, Mesh, MoveTemp(Variant), Path + Suffix, Cached.Deviation,
                bNeedsCollision, Options.ProcessedCache, Cached.Key);
        }
        UE_LOG(LogSCImport, Display,
               TEXT("MESH path=%s%s triangles=%d nanite=%s clusters=%d fallback=%d cards=%d collision=%d build_s=%.4f"), *Path, *Suffix,
               Stats.Triangles, *Stats.NaniteReason, Stats.Clusters, Stats.FallbackTriangles, Stats.Cards, Stats.bCollision,
               Stats.BuildSeconds);
    }
    // Advance: a part's translucent split (if any) follows the part itself; release built CPU geometry now.
    if (!bTranslucent && Part.Translucent.bHasGeometry)
    {
        Part.Mesh = FSCDerivedMesh();
        Slot.bTranslucentNext = true;
        return;
    }
    Part = FPart();
    Slot.bTranslucentNext = false;
    ++Slot.NextPart;
}

void FSCImportJob::FState::FinishBuild(FMeshSlot& Slot)
{
    const FString& Path = Slot.Path;
    KeepAlive.Remove(Slot.Package); // owned through its objects (meshes/rig referenced by the vehicle) from now on
    Slot.Parts.Empty();
    Slot.Soft.Description = FMeshDescription();
    Slot.Soft.SkinWeights.Empty(); // retain collision bytes until component assembly
    if (Slot.bBuildFailed)
    {
        Slot.Data.Reset();
        return;
    }
    SocketNodes.FindOrAdd(Path);
    Result.ContentHashes.Add(Path, Slot.Data->ContentHash);
    Meshes.Add(Path, MoveTemp(Slot.Variants));
    if (Slot.Data->bHasGamePhysics)
    {
        GamePhysicsVariants.Add(Path, MoveTemp(Slot.GamePhysicsVariants));
        // A collider that could not be built must not open a hole: then the render mesh keeps blocking the pawn too.
        const bool bReplaces = FSCMeshImporter::GamePhysicsReplacesRender(*Slot.Data) && !Slot.bGamePhysicsFailed;
        if (bReplaces) GamePhysicsReplacesRender.Add(Path);
        else Warn(Path, TEXT("Game collision incomplete: render geometry keeps blocking movement"));
        UE_LOG(LogSCImport, Display, TEXT("GAME_COLLISION path=%s colliders=%d filtered=%d failed=%d triangles=%d parts=%d replaces_render=%d"), *Path,
               Slot.Data->PhysicsShapes, Slot.Data->PhysicsFilteredShapes, Slot.Data->PhysicsFailedShapes + int32(Slot.bGamePhysicsFailed),
               Slot.Data->PhysicsTriangles.Num(), GamePhysicsVariants[Path].Num(), bReplaces);
    }
    Slot.Data->PhysicsPositions.Empty();
    Slot.Data->PhysicsTriangles.Empty();
    Slot.Data->PhysicsTriangleNodes.Empty();
    MeshTranslucentSlots.Add(Path, MoveTemp(Slot.TranslucentSlots));
    // Assembly needs helper matrices only. Release CPU geometry and serialized BVH now.
    Slot.Data->Description = FMeshDescription();
    Slot.Data->CollisionBytes.Empty();
    // TODO(WP-C1): bound source-description lifetime retained internally by UStaticMesh;
    // cache CPU derivations independently of transient render UObjects.
    Derived.Add(Path, MoveTemp(Slot.Data));
}

bool FSCImportJob::FState::StepMaterials(bool bBlocking)
{
    if (Options.bImportMaterials)
    {
        if (!bBlocking && !MaterialPrep.IsReady()) return false;
        const double WaitStart = FPlatformTime::Seconds();
        MaterialPrep.Wait();
        MaterialPrep = TFuture<void>();
        const double WaitSeconds = FPlatformTime::Seconds() - WaitStart;
        const double InstantiateStart = FPlatformTime::Seconds();
        TArray<FString> MaterialWarnings;
        Materials.Instantiate(Vehicle, MaterialWarnings);
        for (const FString& W : MaterialWarnings)
            Warn(TEXT("material"), W);
        Timings.Materials = FPlatformTime::Seconds() - InstantiateStart;
        const auto& T = Materials.TextureStats;
        Result.MaterialSeconds = FPlatformTime::Seconds() - MaterialStart;
        Result.TextureCount = T.Created;
        Result.MaterialCount = Materials.MaterialCount;
        UE_LOG(LogSCImport, Display, TEXT("TEXTURES requested=%d created=%d failed=%d bytes=%lld decode_s=%.4f create_s=%.4f png_cache_hits=%d"),
               T.Requested, T.Created, T.Failed, T.Bytes, T.DecodeSeconds, T.CreateSeconds, T.Cached);
        FString Masters;
        for (const auto& Pair : Materials.MasterCounts)
            Masters += FString::Printf(TEXT(" %s=%d"), FSCMaterialBuilder::MasterName(Pair.Key), Pair.Value);
        UE_LOG(LogSCImport, Display,
               TEXT("MATERIALS sets=%d instances=%d engine_glow=%d describe_s=%.4f mid_s=%.4f overlapped_s=%.4f wait_s=%.4f masters:%s"),
               Materials.SetCount, Materials.MaterialCount, Materials.EngineGlowCount, Materials.DescribeSeconds, Materials.MaterialSeconds,
               Result.MaterialSeconds, WaitSeconds, *Masters);
    }
    Stage = ESCImportStage::Assembling;
    return true;
}

void FSCImportJob::FState::Assemble()
{
    const double AssembleStart = FPlatformTime::Seconds();
    for (auto& Clip : Vehicle->ClipCatalog->Clips)
    {
        if (Clip.Bindings.IsEmpty()) Warn(Clip.Source, TEXT("No receiver bones in exported geometry"));
        FSCAnimImporter::Finalize(RigMeshes.FindRef(Manifest.Root.MeshAsset), Vehicle, Clip);
    }
    const double FinalizeSeconds = FPlatformTime::Seconds() - AssembleStart;
    UE_LOG(LogSCImport, Display, TEXT("ANIMATIONS imported=%d indexed=%d"), Vehicle->ClipCatalog->Clips.Num(), Manifest.Clips.Num());
    ASCVehicle* V = Vehicle;
    TMap<FString, FEntity> Entities, EntitiesById;
    double AnchorSeconds = 0, BindSeconds = 0;
    V->bCollisionReady = Options.bBuildCollision;
    auto NewAnchor = [&](const FString& Label, USceneComponent* Parent, const FMatrix& Relative) {
        const double AnchorStart = FPlatformTime::Seconds();
        ON_SCOPE_EXIT { AnchorSeconds += FPlatformTime::Seconds() - AnchorStart; };
        auto* C = NewObject<USceneComponent>(V, MakeUniqueObjectName(V, USceneComponent::StaticClass(), FName(*SafeName(Label))), RF_Transient);
        V->AddInstanceComponent(C);
        C->SetMobility(EComponentMobility::Movable);
        C->SetupAttachment(Parent);
        C->SetRelativeTransform(FTransform(Relative));
        C->RegisterComponent();
        return C;
    };
    // The frame an item attaches to on its parent entity: the entity's anchor, or the named helper/bone on it.
    auto ResolveHelper = [&](FEntity* E, const FString& EntityName, const FString& ParentName, const FString& NodeName, bool& bHelper) -> USceneComponent* {
        bHelper = false;
        if (NodeName.IsEmpty()) return E->Anchor;
        if (auto* Existing = E->Helpers.Find(NodeName))
        {
            bHelper = true;
            return *Existing;
        }
        if (E->Rig && E->Rig->DoesSocketExist(FName(*NodeName)))
        {
            auto* H = NewAnchor(NodeName, E->Rig, FMatrix::Identity);
            H->AttachToComponent(E->Rig, FAttachmentTransformRules::KeepRelativeTransform, FName(*NodeName));
            E->Helpers.Add(NodeName, H);
            bHelper = true;
            return H;
        }
        const FMatrix* Node = E->Mesh ? E->Mesh->Nodes.Find(NodeName) : nullptr;
        if (!Node && E->Mesh)
            for (const auto& Pair : E->Mesh->Nodes)
                if (Pair.Key.Equals(NodeName, ESearchCase::IgnoreCase))
                {
                    Node = &Pair.Value;
                    break;
                }
        if (!Node && E->Mesh)
        {
            // The exporter falls back to the entity's source root when a port has no node.
            Node = &E->Mesh->AttachmentRootMatrix;
            UE_LOG(LogSCImport, Display, TEXT("ATTACHMENT_ROOT_FALLBACK entity=%s helper=%s (resolved local transform retained)"),
                   *ParentName, *NodeName);
        }
        if (!Node)
        {
            Warn(EntityName, TEXT("Unresolved parent helper: ") + ParentName + TEXT("/") + NodeName);
            return nullptr;
        }
        auto* Helper = NewAnchor(NodeName, E->Anchor, FSCMeshImporter::Transform(*Node));
        E->Helpers.Add(NodeName, Helper);
        bHelper = true;
        return Helper;
    };
    auto FindParent = [&](const FSCSceneInstance& I, bool& bHelper) -> USceneComponent* {
        bHelper = false;
        if (I.ParentEntityName.IsEmpty() && I.ParentId.IsEmpty()) return V->GetRootComponent();
        FEntity* E = I.ParentId.IsEmpty() ? Entities.Find(I.ParentEntityName) : EntitiesById.Find(I.ParentId);
        if (!E || !E->Anchor)
        {
            Warn(I.EntityName, TEXT("Unresolved parent entity: ") + I.ParentEntityName);
            return nullptr;
        }
        return ResolveHelper(E, I.EntityName, I.ParentEntityName, I.ParentNodeName, bHelper);
    };
    int32 RenderInstances = 0;
    TArray<UStaticMeshComponent*> DeferredRegistration;
    double RegisterSeconds = 0, LightSeconds = 0, RigSeconds = 0;
    // Host paint channel depends only on (sidecar, mesh asset); HostChannel parses submaterial JSON, so compute once.
    TMap<TPair<FString, FString>, FString> HostChannels;
    auto HostFor = [&](const FString& Sidecar, const FString& Asset, const FSCMaterialSet* Set, const FSCDerivedMesh& Mesh) -> const FString& {
        const TPair<FString, FString> Key(Sidecar, Asset);
        if (const FString* Found = HostChannels.Find(Key)) return *Found;
        return HostChannels.Add(Key, FSCImportMaterials::HostChannel(Set, Mesh.MaterialSourceNames, MeshSlotTriangles.FindRef(Asset)));
    };
    // A split mesh can bind the same slot on many components. Compute its paint material list once for each
    // (mesh, sidecar, palette, slot) and use it for both the soft rig and rigid companions.
    using FPaintMaterials = TArray<TWeakObjectPtr<UMaterialInterface>>;
    using FPaintsBySlot = TMap<int32, FPaintMaterials>;
    TMap<TTuple<FString, FString, FString>, FPaintsBySlot> PaintVariants;
    auto RecordPaint = [&](const FSCSceneInstance& I, UMeshComponent* C, int32 Slot, const FSCDerivedMesh& Mesh, FPaintsBySlot* Slots)
    {
        if (!Slots) return;
        const FPaintMaterials* Variants = Slots->Find(Slot);
        if (!Variants)
        {
            FPaintMaterials& MaterialsForSlot = Slots->Add(Slot);
            bool bAny = false;
            MaterialsForSlot.Reserve(Manifest.Paints.Num());
            FString Sidecar, Palette;
            for (int32 Paint = 0; Paint < Manifest.Paints.Num(); ++Paint)
            {
                UMaterialInterface* Material = nullptr;
                if (Manifest.PaintTarget(I, Paint, Sidecar, Palette))
                    if (const auto* Set = Manifest.Materials.FindByPredicate([&](const FSCMaterialSet& M) { return M.Path == Sidecar; }))
                        if (const FSCSubmaterial* Sub = FSCImportMaterials::Resolve(Set, Mesh.MaterialSourceNames, Slot))
                            Material = Materials.Find(Sidecar, Palette, Sub->Index, HostFor(Sidecar, I.MeshAsset, Set, Mesh));
                MaterialsForSlot.Add(Material);
                bAny |= Material != nullptr;
            }
            if (!bAny) MaterialsForSlot.Reset();
            Variants = &MaterialsForSlot;
        }
        if (Variants->IsEmpty()) return;
        FSCPaintSlot& Entry = V->PaintSlots.AddDefaulted_GetRef();
        Entry.Component = C;
        Entry.Slot = Slot;
        Entry.Default = C->GetMaterial(Slot);
        Entry.Paints = *Variants;
    };
    // WP-I1: each screen face (a slot drawn by a screen master with triangles in the drawn variant / LOD) shows the
    // image its own binding resolves to (FSCSceneInstance::ScreenSlotImage); every other screen face stays dark glass.
    int32 ScreenBoundInstances = 0, ScreenBoundSlots = 0, ScreenDarkSlots = 0, ScreenAmbiguousSlots = 0;
    TSet<FString> ScreenImagesBound;
    TSet<FString> ScreenStateImagesBound; // I1-3 state images bound to faces (beside the default images)
    TSet<FString> ScreenHoverImagesBound; // WP-I1-4 hover renders bound to faces
    // WP-P5 floor panel: blank faces of placements that send interaction links (a lift panel may draw its list there).
    TArray<FSCBlankScreenFace> BlankFaces;
    TArray<FSCButtonFace> ButtonFaces; // C2 fixes: faces of the game's panel UIs with clickable nodes (light panels)
    int32 PlacingInterior = INDEX_NONE; // the interior whose placements Place() is placing (INDEX_NONE: children, the root)
    auto StaticSlotTriangles = [](const UStaticMesh* Mesh, int32 Slot) {
        uint32 N = 0;
        if (const FStaticMeshRenderData* R = Mesh ? Mesh->GetRenderData() : nullptr; R && !R->LODResources.IsEmpty())
            for (const FStaticMeshSection& S : R->LODResources[0].Sections) N += S.MaterialIndex == Slot ? S.NumTriangles : 0;
        return N;
    };
    auto SkeletalSlotTriangles = [](USkeletalMesh* Mesh, int32 Slot) {
        uint32 N = 0;
        if (const FSkeletalMeshRenderData* R = Mesh ? Mesh->GetResourceForRendering() : nullptr; R && !R->LODRenderData.IsEmpty())
            for (const FSkelMeshRenderSection& S : R->LODRenderData[0].RenderSections) N += S.MaterialIndex == Slot ? S.NumTriangles : 0;
        return N;
    };
    auto Place = [&](const FSCSceneInstance& I, USceneComponent* Parent, bool bAtHelper, bool bRoot) -> FEntity {
        FEntity E;
        if (auto* Data = Derived.Find(I.MeshAsset)) E.Mesh = Data->Get();
        FSCThrusterBinding Thruster;
        FPaintsBySlot* Paints = Manifest.Paints.IsEmpty() || !E.Mesh ? nullptr
            : &PaintVariants.FindOrAdd(TTuple<FString, FString, FString>(I.MeshAsset, I.MaterialSidecar, I.PaletteId));
        bool bScreenBound = false;
        ON_SCOPE_EXIT { ScreenBoundInstances += bScreenBound; };
        const TMap<int32, TArray<int32>>* ScreenOwners = MeshScreenOwners.Find(I.MeshAsset);
        const TMap<int32, TArray<int32>>* SoftScreenOwners = MeshSoftScreenOwners.Find(I.MeshAsset);
        TSet<FString> MeshNodes;
        // The screen instance for a slot, or null to keep Find's (a non-screen slot, or a dark face). Owner: the rig node
        // a rigid part variant belongs to (all its triangles do), INDEX_NONE for the whole mesh. Triangles: the slot's
        // triangles in the drawn variant / LOD (an unused slot binds and counts nothing).
        auto ScreenMaterial = [&](const FSCSubmaterial* Sub, int32 Slot, int32 Owner, uint32 Triangles, bool bSoft, UMeshComponent* Target) -> UMaterialInterface* {
            if (!Options.bImportMaterials || !Sub || !E.Mesh || !Triangles || !Materials.IsScreen(I.MaterialSidecar, I.PaletteId, Sub->Index))
                return nullptr;
            TArray<TArray<FString>> Chains; // per owning node: its name, then its ancestors' (lower case)
            const TMap<int32, TArray<int32>>* Map = bSoft ? SoftScreenOwners : ScreenOwners;
            if (const TArray<int32>* Owners = Map ? Map->Find(Slot) : nullptr)
                for (int32 Node : *Owners)
                {
                    if (Owner != INDEX_NONE && Node != Owner) continue;
                    TArray<FString>& Chain = Chains.AddDefaulted_GetRef();
                    for (int32 N = Node; E.Mesh->RigNodes.IsValidIndex(N); N = E.Mesh->RigNodes[N].Parent) Chain.Add(E.Mesh->RigNodes[N].SourceName.ToLower());
                }
            if (MeshNodes.IsEmpty())
                for (const FSCRigNode& Node : E.Mesh->RigNodes) MeshNodes.Add(Node.SourceName.ToLower());
            bool bAmbiguous = false;
            const FString Image = Chains.IsEmpty() ? FString() : I.ScreenSlotImage(MeshNodes, Chains, &bAmbiguous);
            ScreenAmbiguousSlots += bAmbiguous;
            UMaterialInterface* Screen = Image.IsEmpty() ? nullptr : Materials.FindScreen(I.MaterialSidecar, I.PaletteId, Sub->Index, Image);
            if (Screen) { ScreenImagesBound.Add(Image); ++ScreenBoundSlots; bScreenBound = true; }
            else
            {
                ++ScreenDarkSlots;
                FSCBlankScreenFace Blank;
                const TMap<int32, FSCScreenFaceGeometry>* MeshFaces = I.InteractionLinks.IsEmpty() ? nullptr : MeshScreenGeometry.Find(I.MeshAsset);
                const FSCScreenFaceGeometry* Captured = MeshFaces ? MeshFaces->Find(Slot) : nullptr;
                if (Captured && !(Blank.Geometry = Captured->Select(Owner, bSoft)).Positions.IsEmpty())
                {
                    Blank.Component = Target; Blank.Slot = Slot; Blank.Panel = E.Anchor;
                    Blank.MakeMaterial = [Mats = &Materials, Sidecar = I.MaterialSidecar, Palette = I.PaletteId, Ordinal = Sub->Index](UTexture* Image) -> UMaterialInterface* {
                        return Mats->CreateScreen(Sidecar, Palette, Ordinal, Image);
                    };
                    BlankFaces.Add(MoveTemp(Blank));
                }
            }
            // I1-3: the panel owner's other states, one instance each; the face swaps them (ASCVehicle::RefreshPanelScreens).
            // WP-I1-4: and its renders with the cursor on a button, swapped under the crosshair (ASCVehicle::SetPanelHover).
            if (const FSCUiBinding* Binding = Screen ? I.PanelBinding(Image) : nullptr;
                Binding && (!Binding->PanelStateImages().IsEmpty() || !Binding->PanelHoverImages().IsEmpty()))
            {
                FSCScreenFace& Face = V->ScreenFaces.AddDefaulted_GetRef();
                Face.Component = Target; Face.Slot = Slot; Face.Panel = E.Anchor; Face.Default = Screen; Face.Image = Image;
                for (const FSCUiStateImage& State : Binding->PanelStateImages())
                    if (UMaterialInterface* M = Materials.FindScreen(I.MaterialSidecar, I.PaletteId, Sub->Index, State.ImagePath))
                    {
                        Face.States.Add(State.Key(), M);
                        ScreenStateImagesBound.Add(State.ImagePath);
                    }
                for (const FSCUiHoverImage& Hover : Binding->PanelHoverImages())
                    if (UMaterialInterface* M = Materials.FindScreen(I.MaterialSidecar, I.PaletteId, Sub->Index, Hover.ImagePath))
                    {
                        Face.Hovers.Add(FSCUiHoverImage::FaceKey(Hover.Node, Hover.StateKey()), M);
                        ScreenHoverImagesBound.Add(Hover.ImagePath);
                    }
                // Its slot is swapped per component: never consolidated into a shared instance (FSCInstancing).
                Target->ComponentTags.AddUnique(FSCInstancing::KeepTag);
            }
            // C2 fixes: a render with clickable nodes (a light panel) is operated on its face (ASCVehicle::BuildButtonPanels),
            // whether or not the owner has other state renders.
            if (const FSCUiBinding* Binding = Screen ? I.PanelBinding(Image) : nullptr; Binding && !Binding->Buttons.IsEmpty())
            {
                const TMap<int32, FSCScreenFaceGeometry>* MeshFaces = Binding->Buttons.IsEmpty() ? nullptr : MeshScreenGeometry.Find(I.MeshAsset);
                if (const FSCScreenFaceGeometry* Captured = MeshFaces ? MeshFaces->Find(Slot) : nullptr; Captured && (!I.EntityId.IsEmpty() || !I.PortPath.IsEmpty()))
                {
                    FSCButtonFace Operable;
                    Operable.Geometry = Captured->Select(Owner, bSoft);
                    if (!Operable.Geometry.Positions.IsEmpty())
                    {
                        Operable.Component = Target; Operable.Slot = Slot; Operable.Panel = E.Anchor;
                        Operable.Interior = PlacingInterior; Operable.EntityId = I.EntityId.IsEmpty() ? SCCargoRecords::PortKey(I.PortPath) : I.EntityId; Operable.Links = I.Links; Operable.Sendables = I.SendableInteractions;
                        Operable.Buttons = Binding->Buttons; Operable.ImageSizePx = Binding->ImageSizePx;
                        Operable.bHideDoorLockControl = Binding->bHideDoorLockControl;
                        Operable.StateTypes = Binding->StateTypes; Operable.StateImages = Binding->PanelStateImages(); Operable.DefaultImage = Image;
                        Operable.HoverImages = Binding->PanelHoverImages();
                        Operable.MakeMaterial = [Mats = &Materials, Sidecar = I.MaterialSidecar, Palette = I.PaletteId, Ordinal = Sub->Index](UTexture* Img) -> UMaterialInterface* {
                            return Mats->CreateScreen(Sidecar, Palette, Ordinal, Img);
                        };
                        ButtonFaces.Add(MoveTemp(Operable));
                    }
                }
            }
            return Screen;
        };
        FMatrix Relative = FSCMeshImporter::InstanceTransform(I.LocalMatrix, E.Mesh ? E.Mesh->RootMatrix : FMatrix::Identity,
                                                              bAtHelper, FSCMeshImporter::InCryFrame(I.SourceTransformBasis, bAtHelper));
        // Helper frames in anchor space ("" = the item's source frame) for port alignment, now and at run time.
        auto AddHelpers = [&E](FSCInstanceHelpers& Helpers) {
            Helpers.Frames.Add(FString(), FTransform(FSCMeshImporter::Transform(E.Mesh ? E.Mesh->RootMatrix : FMatrix::Identity)));
            if (E.Mesh)
                for (const auto& Node : E.Mesh->Nodes)
                    if (!Node.Key.IsEmpty() && !Helpers.Frames.Contains(Node.Key)) Helpers.Frames.Add(Node.Key, FTransform(FSCMeshImporter::Transform(Node.Value)));
        };
        if (!I.Id.IsEmpty()) AddHelpers(V->InstanceHelpers.FindOrAdd(I.Id));
        if (I.bBonePort && bAtHelper && !I.Id.IsEmpty())
        {
            FTransform Port;
            if (V->PortRelative(I.Id, I.PortHelper, I.PortOffset, Port))
                Relative = Port.ToMatrixWithScale();
            else
                Warn(I.EntityName, TEXT("Port item offset helper not in the item: ") + I.PortHelper);
        }
        E.Anchor = NewAnchor(I.EntityName, Parent, bRoot ? FMatrix::Identity : Relative);
        if (!I.Id.IsEmpty()) V->InstanceAnchors.Add(I.Id, E.Anchor);
        else if (E.Mesh && (!I.ParentId.IsEmpty() || I.Seat.IsSet() || !I.SeatEntrances.IsEmpty()))
            AddHelpers(V->AttachmentHelpers.FindOrAdd(E.Anchor));
        V->AddItemSpawn(I, E.Anchor);
        // WP-I1-4: a placed usable offers, at its points, the interactions that start a use (a bed: Sit, LieDown, Sleep).
        // A door's usable is its walk-through (a navigation link: StepThrough): the door's own interaction is what E does
        // there (FindInteraction), so a door is not registered as a usable.
        if (!I.UsableInteractions.IsEmpty() && E.Anchor && !I.Door.bValid)
        {
            FSCUsableTarget Usable;
            Usable.Interior = PlacingInterior;
            Usable.EntityId = I.EntityId;
            Usable.Label = I.SocEntityName.IsEmpty() ? I.EntityName : I.SocEntityName;
            for (const FSCInteractionPoint& Point : I.InteractionPoints)
            {
                FSCUsablePoint P;
                P.Helper = Point.HelperName;
                P.RadiusCm = Point.Radius * 100.f;
                P.ViewAngleLimit = Point.ViewAngleLimit.Get(0.f);
                for (const FSCPointInteraction& X : Point.Interactions)
                    if (I.UsableInteractions.Contains(X.Name)) P.Options.Add(X);
                if (P.Options.IsEmpty()) continue;
                // The helper in the item's frame (as AddHelpers): the mesh keeps the helper nodes, named with or without '$'.
                const FMatrix* Node = E.Mesh ? E.Mesh->Nodes.Find(Point.HelperName) : nullptr;
                if (!Node && E.Mesh) Node = E.Mesh->Nodes.Find(Point.HelperName.Replace(TEXT("$"), TEXT("")));
                if (Node)
                {
                    P.Local = FTransform(FSCMeshImporter::Transform(*Node)).GetLocation();
                    P.bLocated = true;
                }
                Usable.Points.Add(MoveTemp(P));
            }
            if (!Usable.Points.IsEmpty()) V->UsableTargets.Add(E.Anchor, MoveTemp(Usable));
        }
        if (auto* RigMesh = RigMeshes.Find(I.MeshAsset))
        {
            E.Rig = NewObject<USCRigComponent>(V, NAME_None, RF_Transient);
            V->AddInstanceComponent(E.Rig);
            V->Rigs.Add(E.Rig);
            E.Rig->SetMobility(EComponentMobility::Movable);
            E.Rig->SetupAttachment(E.Anchor);
            E.Rig->SetSkeletalMesh(*RigMesh);
            E.Rig->MeshAsset = I.MeshAsset;
            E.Rig->OwnerEntity = I.EntityName;
            // C2 fixes: the door data of the instance that carries the rig (its spawn state, fragment tags, port links).
            if (I.Door.bValid || !I.ItemPortLinks.IsEmpty()) V->DoorRecords.Add(E.Rig, {I.Door, I.ItemPortLinks});
            E.Rig->SetCollisionProfileName(TEXT("NoCollision"));
            if (Options.bBuildCollision)
                for (const FMeshSlot& Slot : Slots)
                    if (Slot.Path == I.MeshAsset && Slot.Soft.bHasGeometry && !E.Rig->InitializeCollision(Slot.Soft))
                    {
                        V->bCollisionReady = false;
                        Warn(I.EntityName, TEXT("Skeletal collision unavailable"));
                    }
            // WP-P3: with the game's colliders present, render geometry answers traces but never blocks the pawn.
            if (GamePhysicsReplacesRender.Contains(I.MeshAsset)) E.Rig->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
            E.Rig->SetVisibility(!I.bInvisible);
            E.Rig->SetHiddenInGame(I.bInvisible);
            const auto* Set = Manifest.Materials.FindByPredicate([&](const FSCMaterialSet& M) { return M.Path == I.MaterialSidecar; });
            const FString Host = HostFor(I.MaterialSidecar, I.MeshAsset, Set, *E.Mesh); // paint lookup can grow HostChannels
            for (int32 Slot = 0; Set && Slot < E.Mesh->MaterialNames.Num(); ++Slot)
                if (const auto* Sub = FSCImportMaterials::Resolve(Set, E.Mesh->MaterialSourceNames, Slot))
                    if (auto* Mat = Materials.Find(I.MaterialSidecar, I.PaletteId, Sub->Index, Host))
                    {
                        UMaterialInterface* Screen = ScreenMaterial(Sub, Slot, INDEX_NONE, SkeletalSlotTriangles(*RigMesh, Slot), true, E.Rig);
                        E.Rig->SetMaterial(Slot, Screen ? Screen : Mat);
                        if (!I.ThrusterEffects.IsEmpty() && !Screen && V->EngineGlowMaterials.Contains(Mat))
                            Thruster.Materials.AddUnique(Cast<UMaterialInstanceDynamic>(Mat));
                        if (!Screen) RecordPaint(I, E.Rig, Slot, *E.Mesh, Paints); // a panel's image does not follow the paint
                    }
            const double RigStart = FPlatformTime::Seconds();
            E.Rig->RegisterComponent();
            E.Rig->ResetPose();
            RigSeconds += FPlatformTime::Seconds() - RigStart;
        }
        // An asset can carry game colliders without render triangles (collider-only geometry): still assembled.
        if (const auto* Variants = Meshes.Find(I.MeshAsset);
            Variants && E.Mesh && (E.Mesh->bHasGeometry || !GamePhysicsVariants.FindRef(I.MeshAsset).IsEmpty()))
        {
            ++Result.InstanceCount;
            for (int32 Variant = 0; Variant < Variants->Num(); ++Variant)
            {
                auto* C = NewObject<UStaticMeshComponent>(V, MakeUniqueObjectName(V, UStaticMeshComponent::StaticClass(), TEXT("Mesh")), RF_Transient);
                V->AddInstanceComponent(C);
                C->SetMobility(EComponentMobility::Movable);
                C->SetupAttachment(E.Anchor);
                C->SetStaticMesh((*Variants)[Variant]);
                const int32 Node = SocketNodes[I.MeshAsset][Variant];
                if (E.Rig && E.Mesh->RigNodes.IsValidIndex(Node))
                {
                    C->SetupAttachment(E.Rig, FSCSkeletonBuilder::BoneName(*E.Mesh, Node));
                    C->SetRelativeTransform(FTransform(FSCMeshImporter::Transform(E.Mesh->RigNodes[Node].World).Inverse()));
                }
                C->ComponentTags.Add(FName(*I.MeshAsset));
                const bool bCollides = Options.bBuildCollision && (*Variants)[Variant]->GetBodySetup() &&
                                       (*Variants)[Variant]->GetBodySetup()->bCreatedPhysicsMeshes;
                C->SetCollisionProfileName(bCollides ? TEXT("BlockAll") : TEXT("NoCollision"));
                if (const TSet<int32>* GameColliders = GamePhysicsVariants.Find(I.MeshAsset))
                {
                    if (GameColliders->Contains(Variant)) // the game's colliders: invisible, block movement only
                    {
                        C->SetCollisionResponseToAllChannels(ECR_Ignore);
                        C->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
                        C->SetVisibility(false);
                        C->SetHiddenInGame(true);
                        C->CastShadow = false;
                        C->bAffectDistanceFieldLighting = false;
                        C->bAffectDynamicIndirectLighting = false;
                        C->bVisibleInRayTracing = false;
                        C->ComponentTags.Add(TEXT("SCGameCollision"));
                        if (bRoot) // the vehicle's own colliders: not blocking inside its physics grid
                        {
                            C->ComponentTags.Add(TEXT("SCHullCollision"));
                            V->HullColliders.Add(C);
                        }
                        C->RegisterComponent();
                        continue;
                    }
                    if (GamePhysicsReplacesRender.Contains(I.MeshAsset))
                        C->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore); // render: traces only
                }
                C->SetVisibility(!I.bInvisible);
                C->SetHiddenInGame(I.bInvisible);
                const UStaticMesh* Mesh = (*Variants)[Variant];
                const TSet<int32>& Translucent = MeshTranslucentSlots.FindRef(I.MeshAsset);
                // Cooked cache packages strip the editor settings even when their Nanite resources are valid.
                const bool bNanite = Mesh->HasValidNaniteData() || OpaqueProxies.Contains(Mesh);
                if (!bNanite && !Translucent.IsEmpty())
                {
                    // Glass/decal/screen split: lit by Lumen but not part of its scene (no cards, no DF).
                    C->bAffectDistanceFieldLighting = false;
                    C->bAffectDynamicIndirectLighting = false;
                    C->CastShadow = false;
                }
                const double BindStart = FPlatformTime::Seconds();
                ON_SCOPE_EXIT { BindSeconds += FPlatformTime::Seconds() - BindStart; };
                const FSCMaterialSet* Set =
                    Manifest.Materials.FindByPredicate([&](const FSCMaterialSet& M) { return M.Path == I.MaterialSidecar; });
                const FString Host = HostFor(I.MaterialSidecar, I.MeshAsset, Set, *E.Mesh); // paint lookup can grow HostChannels
                TArray<int32> GlowSlots;
                for (int32 Slot = 0; Set && Slot < E.Mesh->MaterialNames.Num(); ++Slot)
                {
                    // Bind only sections this variant draws; Nanite never receives a translucent instance.
                    if (bNanite && Translucent.Contains(Slot)) continue;
                    const FSCSubmaterial* Sub = FSCImportMaterials::Resolve(Set, E.Mesh->MaterialSourceNames, Slot);
                    UMaterialInterface* Material = Sub ? Materials.Find(I.MaterialSidecar, I.PaletteId, Sub->Index, Host) : nullptr;
                    UMaterialInterface* Screen = Material ? ScreenMaterial(Sub, Slot, Node, StaticSlotTriangles(Mesh, Slot), false, C) : nullptr;
                    if (Screen) Material = Screen;
                    // A split (decal/glass/glow) section without its instance is hidden, never drawn opaque.
                    if (!Material && !bNanite && Translucent.Contains(Slot)) Material = Materials.Invisible();
                    if (Material) C->SetMaterial(Slot, Material);
                    // A panel's image does not follow the paint.
                    if (Material && Material != Materials.Invisible() && !Screen) RecordPaint(I, C, Slot, *E.Mesh, Paints);
                    if (Material && V->EngineGlowMaterials.Contains(Material))
                    {
                        GlowSlots.Add(Slot);
                        if (!I.ThrusterEffects.IsEmpty()) Thruster.Materials.AddUnique(Cast<UMaterialInstanceDynamic>(Material));
                    }
                }
                const double RegisterStart = FPlatformTime::Seconds();
                if (Options.bInstanceRepeatedParts && GlowSlots.IsEmpty() && !E.Rig && !C->ComponentHasTag(FSCInstancing::KeepTag))
                {
                    // Give room and dirt classification a world transform without creating a scene proxy or a
                    // physics body that instancing would immediately destroy.
                    C->UpdateComponentToWorld();
                    C->UpdateBounds();
                    DeferredRegistration.Add(C);
                }
                else C->RegisterComponent();
                RegisterSeconds += FPlatformTime::Seconds() - RegisterStart;
                ++RenderInstances;
                for (int32 Slot : GlowSlots) // engine-glow diagnostics: binding, visibility, bounds
                    UE_LOG(LogSCImport, Display,
                           TEXT("ENGINE_GLOW_BOUND entity=%s component=%s slot=%d nanite=%d visible=%d bounds_cm=%s material=%s"),
                           *I.EntityName, *C->GetName(), Slot, bNanite, C->IsVisible(), *C->Bounds.BoxExtent.ToString(),
                           *C->GetMaterial(Slot)->GetName());
            }
        }
        for (const FSCThrusterEffect& Source : I.ThrusterEffects)
        {
            USceneComponent* Anchor = Source.bParent ? Parent : E.Anchor;
            auto BindEmitter = [&](USceneComponent* Mount, FName Socket, const FTransform& Offset) {
                if (!Mount) return;
                FSCThrusterEmitter& Emitter = Thruster.Emitters.AddDefaulted_GetRef();
                Emitter.Source = Source;
                Emitter.Source.Offset = Offset;
                Emitter.Mount = Mount;
                Emitter.Socket = Socket;
            };
            if (Source.Bone.IsEmpty())
            {
                BindEmitter(Anchor, NAME_None, Source.Offset);
                continue;
            }
            const int32 Before = Thruster.Emitters.Num();
            USceneComponent* Rig = Source.bParent ? Parent : E.Rig;
            TSet<FName> Animated;
            const FName Exact(*Source.Bone);
            if (Rig && Rig->DoesSocketExist(Exact))
            {
                BindEmitter(Rig, Exact, Source.Offset);
                Animated.Add(Exact);
            }
            else if (USkinnedMeshComponent* MeshRig = Cast<USkinnedMeshComponent>(Rig))
            {
                TArray<FName> Bones;
                MeshRig->GetBoneNames(Bones);
                for (FName Bone : Bones)
                    if (Bone.ToString().MatchesWildcard(Source.Bone, ESearchCase::IgnoreCase))
                    {
                        BindEmitter(Rig, Bone, Source.Offset);
                        Animated.Add(Bone);
                    }
            }
            if (E.Mesh && !Source.bParent)
                for (const auto& Node : E.Mesh->Nodes)
                    if (Node.Key.MatchesWildcard(Source.Bone, ESearchCase::IgnoreCase) && !Animated.Contains(FName(*Node.Key)))
                        BindEmitter(Anchor, NAME_None, Source.Offset * FTransform(FSCMeshImporter::Transform(Node.Value)));
            if (Thruster.Emitters.Num() == Before)
                Warn(I.EntityName, TEXT("Thruster attachment not found: ") + Source.Bone);
        }
        if (!Thruster.Emitters.IsEmpty())
        {
            Thruster.Rig = E.Rig;
            Thruster.NozzleAnimation = I.NozzleAnimation;
            Thruster.Glow = I.ThrusterGlow;
            Thruster.bHasGlow = I.bHasThrusterGlow;
            Thruster.bManeuvering = I.ItemType.Equals(TEXT("ManneuverThruster"), ESearchCase::IgnoreCase);
            V->Thrusters.Add(MoveTemp(Thruster));
        }
        const double LightStart = FPlatformTime::Seconds();
        FSCLightImporter::Build(
            *Source, I, V, E.Anchor,
            [&](const FString& Name) -> USceneComponent* {
                if (E.Rig && E.Rig->DoesSocketExist(FName(*Name)))
                {
                    auto* H = NewAnchor(Name + TEXT("_LightAnchor"), E.Rig, FMatrix::Identity);
                    H->AttachToComponent(E.Rig, FAttachmentTransformRules::KeepRelativeTransform, FName(*Name));
                    return H;
                }
                // Unanimated assets still expose their static helper frames to attached lights.
                if (E.Mesh)
                    for (const auto& Node : E.Mesh->Nodes)
                        if (Node.Key.Equals(Name, ESearchCase::IgnoreCase))
                            return NewAnchor(Name + TEXT("_LightAnchor"), E.Anchor, FSCMeshImporter::Transform(Node.Value));
                return nullptr;
            },
            Result.Warnings);
        LightSeconds += FPlatformTime::Seconds() - LightStart;
        return E;
    };
    int32 EffectOnlySkipped = 0;
    auto EffectOnly = [&](const FSCSceneInstance& I) {
        const FSCMaterialSet* Set = Manifest.Materials.FindByPredicate([&](const FSCMaterialSet& M) { return M.Path == I.MaterialSidecar; });
        if (!Set || Set->Submaterials.IsEmpty()) return false;
        for (const FSCSubmaterial& Sub : Set->Submaterials)
        {
            TSharedPtr<FJsonObject> O;
            FString Shader;
            if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Sub.Json), O) || !O || !O->TryGetStringField(TEXT("shader"), Shader) ||
                !Shader.StartsWith(TEXT("VFX_")))
                return false;
        }
        return true;
    };
    auto CanPlace = [&](const FSCSceneInstance& I) {
        if (!I.ImportWarning.IsEmpty() || (!I.MeshAsset.IsEmpty() && !Meshes.Contains(I.MeshAsset)))
        {
            Warn(I.EntityName, TEXT("Instance skipped: ") + (I.ImportWarning.IsEmpty() ? I.MeshAsset : I.ImportWarning));
            return false;
        }
        if (Options.bHideEffectOnlyPlacements && !I.MaterialSidecar.IsEmpty() && EffectOnly(I)) { ++EffectOnlySkipped; return false; }
        return true;
    };
    V->PhysicsGridTriangles = Manifest.PhysicsGrid; // the root entity sits at the actor origin
    V->PhysicsGridShapeEnds = Manifest.PhysicsGridShapeEnds;
    V->PhysicsGridBounds.Init();
    for (const FVector& P : Manifest.PhysicsGrid) V->PhysicsGridBounds += P; // FBox(TArray) reads [0]: not for an empty grid
    TMap<FString, FEntity> EntitiesByPort; // WP-K1: loadout items by port path (siblings share entity names)
    int32 PanelsWithoutReachField = 0;     // door panels of an export before cache format 40 (no interaction_distance)
    if (CanPlace(Manifest.Root)) Entities.Add(Manifest.Root.EntityName, Place(Manifest.Root, V->GetRootComponent(), false, true));
    for (const auto& I : Manifest.Children)
    {
        bool bHelper;
        auto* Parent = FindParent(I, bHelper);
        if (!Parent || !CanPlace(I))
        {
            Entities.Add(I.EntityName, FEntity());
            if (!I.Id.IsEmpty()) EntitiesById.Add(I.Id, FEntity());
            continue;
        }
        // Export order is depth-first; latest matching entity disambiguates repeated loadout instances.
        FEntity Placed = Place(I, Parent, bHelper, false);
        if (!I.Id.IsEmpty()) EntitiesById.Add(I.Id, Placed);
        Entities.Add(I.EntityName, Placed);
        if (!I.PortPath.IsEmpty())
        {
            EntitiesByPort.Add(SCCargoRecords::PortKey(I.PortPath), Placed);
            V->PortAnchors.Add(SCCargoRecords::PortKey(I.PortPath), Placed.Anchor);
            if (Placed.Rig) V->PortRigs.Add(SCCargoRecords::PortKey(I.PortPath), Placed.Rig); // item_port_links deliver here
        }
        // A panel in a door's loadout operates that door (the rear ramp's pads, the front hatch's): its face follows the door.
        if (Placed.Anchor && I.UiBindings.ContainsByPredicate([](const FSCUiBinding& B) { return B.IsOperatingPanel(); }))
            for (USceneComponent* C = Placed.Anchor->GetAttachParent(); C; C = C->GetAttachParent())
                if (auto* Rig = Cast<USCRigComponent>(C))
                {
                    if (V->DoorRecords.Contains(Rig) && V->DoorRecords[Rig].Door.bValid)
                    {
                        V->DoorControls.Add(Placed.Anchor, Rig);
                        for (const FSCUiBinding& B : I.UiBindings)
                            if (B.IsOperatingPanel() && !B.bPropagated) // the panel's own screen, not one it carries for another entity
                            {
                                V->SetPanelReach(Placed.Anchor, B.InteractionDistance);
                                V->RememberDoorPanelLabel(Rig, B);
                                PanelsWithoutReachField += !B.bInteractionDistanceExported;
                            }
                    }
                    break;
                }
    }
    // WP-K1: one anchor per inventory container in the container entity's frame, axes X = SC y, Y = SC x, Z = SC z
    // (the frame placed items use: SC (x, y, z) m -> (y, x, z) cm). A loadout grid hangs on its parent item's helper
    // (following that item's rig); an interior grid on its container (below).
    V->InventoryAnchors.SetNum(Manifest.InventoryContainers.Num());
    V->InventoryOwnerRigs.SetNum(Manifest.InventoryContainers.Num());
    const FMatrix CryToGltf = FSCMeshImporter::CryToGltf();
    auto ContainerFrame = [&](const FMatrix& Local, bool bHelper) {
        return bHelper ? FSCMeshImporter::Transform(CryToGltf.Inverse() * Local) : FSCMeshImporter::InstanceTransform(Local, FMatrix::Identity, false, true);
    };
    for (int32 C = 0; C < Manifest.InventoryContainers.Num(); ++C)
    {
        const FSCInventoryContainerRecord& R = Manifest.InventoryContainers[C];
        if (R.InteriorIndex != INDEX_NONE) continue;
        FEntity* Owner = R.ParentPortPath.IsEmpty() ? Entities.Find(Manifest.Root.EntityName) : EntitiesByPort.Find(SCCargoRecords::PortKey(R.ParentPortPath));
        bool bHelper = false;
        USceneComponent* Parent = Owner && Owner->Anchor ? ResolveHelper(Owner, R.EntityName, SCCargoRecords::PortKey(R.ParentPortPath), R.ParentNodeName, bHelper) : nullptr;
        if (!Parent)
        {
            Warn(R.EntityName, TEXT("Inventory container without its parent item: ") + SCCargoRecords::PortKey(R.ParentPortPath));
            continue;
        }
        V->InventoryAnchors[C] = NewAnchor(TEXT("Inventory_") + R.EntityName, Parent, ContainerFrame(R.LocalMatrix, bHelper));
        V->InventoryOwnerRigs[C] = Owner->Rig;
    }
    // WP-A1: the package's audio (audio.json, its media and impulse responses), placed per interior below.
    FString AudioError;
    TSharedPtr<FSCAudioScene> Audio = FSCAudioScene::Load(*Source, Manifest.ScenePath, AudioError);
    if (Audio) Audio->Vehicle = V;
    else UE_LOG(LogSCImport, Display, TEXT("AUDIO_NONE %s: %s"), *Manifest.ScenePath, *AudioError);
    // Hangar waypoints (environments): the records the rule chooses, anchored per interior below, in the rule's order.
    const TArray<FSCWaypointCandidate> WaypointCandidates = Options.bEnvironment ? SCHangarWaypoints::Select(Manifest) : TArray<FSCWaypointCandidate>();
    TArray<FSCWaypointSource> WaypointSources;
    WaypointSources.SetNum(WaypointCandidates.Num());
    for (int32 InteriorIndex = 0; InteriorIndex < Manifest.Interiors.Num(); ++InteriorIndex)
    {
        const FSCInterior& Interior = Manifest.Interiors[InteriorIndex];
        bool bHelper;
        auto* Parent = FindParent(Interior.Container, bHelper);
        if (!Parent) continue;
        const FMatrix W = FSCMeshImporter::CryToGltf();
        auto* Container = NewAnchor(
            Interior.Container.EntityName, Parent,
            FSCMeshImporter::Transform(bHelper ? Interior.Container.LocalMatrix : W.Inverse() * Interior.Container.LocalMatrix * W));
        V->InteriorAnchors.Add(Container);
        V->InteriorHelperFrames.Add(bHelper);
        V->InteriorSceneIndex.Add(InteriorIndex);
        const int32 LightsBefore = V->LightStates.Num();
        FSCLightImporter::Build(
            *Source, Interior.Container, V, Container, [](const FString&) -> USceneComponent* { return nullptr; }, Result.Warnings, bHelper);
        for (int32 L = LightsBefore; L < V->LightStates.Num(); ++L) V->LightStates[L].Interior = InteriorIndex; // group ids are per container
        PlacingInterior = InteriorIndex; // the interior Place() runs for (button panels keep it)
        ON_SCOPE_EXIT { PlacingInterior = INDEX_NONE; };
        const int32 AreasBefore = V->LightAreas.Num();
        FSCLightAreas::Register(V, Container, Interior.RoomVolumeJson, bHelper, Result.Warnings);
        TMap<FString, FEntity> ById; // entity ids are unique within one container
        TMap<int32, FEntity> ByPlacement;
        const int32 FirstDeferred = DeferredRegistration.Num();
        for (int32 PlacementIndex = 0; PlacementIndex < Interior.Placements.Num(); ++PlacementIndex)
            if (const auto& P = Interior.Placements[PlacementIndex]; CanPlace(P))
            {
                FEntity Placed = Place(P, Container, bHelper, false);
                if (!P.EntityId.IsEmpty()) ById.Add(P.EntityId, Placed);
                ByPlacement.Add(PlacementIndex, Placed);
                if (P.Ladder.IsSet() && Placed.Anchor)
                {
                    USceneComponent* Ladder = NewAnchor(TEXT("LadderFrame"), Container, ContainerFrame(P.LocalMatrix, bHelper));
                    V->LadderAnchors.Add({Ladder, P.EntityId, P.Ladder.GetValue()});
                }
            }
        // Waypoints of this interior: a transit gateway at its entity frame (an anchor of its own: the entity has no
        // geometry), a panel at its placement's anchor.
        for (int32 WaypointIndex = 0; WaypointIndex < WaypointCandidates.Num(); ++WaypointIndex)
        {
            const FSCWaypointCandidate& Candidate = WaypointCandidates[WaypointIndex];
            if (Candidate.Interior != InteriorIndex) continue;
            WaypointSources[WaypointIndex].Source = Candidate.Source;
            WaypointSources[WaypointIndex].Label = Candidate.Label;
            if (Candidate.Source == ESCWaypointSource::TransitGateway && Interior.Entities.IsValidIndex(Candidate.Index))
                WaypointSources[WaypointIndex].Anchor = NewAnchor(TEXT("Waypoint_") + Interior.Entities[Candidate.Index].EntityName, Container,
                                                      ContainerFrame(Interior.Entities[Candidate.Index].LocalMatrix, bHelper));
            else if (const FEntity* Placed = ByPlacement.Find(Candidate.Index); Placed && Candidate.Source == ESCWaypointSource::Panel)
            {
                WaypointSources[WaypointIndex].Anchor = Placed->Anchor;
                if (Placed->Anchor && Interior.Placements.IsValidIndex(Candidate.Index))
                    for (const FSCUiBinding& Binding : Interior.Placements[Candidate.Index].UiBindings)
                        if (ASCVehicle::IsGarageKiosk(Binding))
                        {
                            V->GarageKiosks.Add(Placed->Anchor, Binding.InteractionDistance * 100.f);
                            break;
                        }
            }
        }
        if (Audio)
        {
            Audio->AddInterior(InteriorIndex, Container, bHelper, Interior.AudioSpotJson, Interior.AudioAreaJson, Result.Warnings);
            for (const auto& P : Interior.Placements)
                if (const FEntity* Placed = ById.Find(P.EntityId); Placed && Audio->Manifest.Classes.Contains(P.EntityClassGuid))
                    Audio->Entities.Add(FString::Printf(TEXT("%d/%s"), InteriorIndex, *P.EntityId.ToLower()),
                                        {P.EntityClassGuid, Placed->Anchor, Placed->Rig, InteriorIndex});
        }
        // Door panels (pads): EntityLinks name the door, or the door's own loadout attached the panel. A lift carriage links
        // its gate (a door) but operates it itself: an entity that is a lift is not a pad (WP-P5).
        const TSet<FString> Carriages = SCVehicleLifts::CarriageIds(Interior);
        for (const auto& P : Interior.Placements)
        {
            const FEntity* Panel = ById.Find(P.EntityId);
            if (!Panel || !Panel->Anchor || Carriages.Contains(P.EntityId)) continue;
            TArray<FString> Targets = P.Links;
            if (!P.LoadoutOwnerEntityId.IsEmpty()) Targets.Add(P.LoadoutOwnerEntityId);
            for (const FString& Target : Targets)
                if (const FEntity* Door = ById.Find(Target); Door && Door->Rig && Door->Anchor != Panel->Anchor)
                {
                    V->DoorControls.Add(Panel->Anchor, Door->Rig);
                    for (const FSCUiBinding& B : P.UiBindings)
                        if (B.IsOperatingPanel() && !B.bPropagated)
                        {
                            V->SetPanelReach(Panel->Anchor, B.InteractionDistance);
                            V->RememberDoorPanelLabel(Door->Rig, B);
                            PanelsWithoutReachField += !B.bInteractionDistanceExported;
                        }
                    break;
                }
        }
        // WP-P5: the interior's interaction links and lifts (carriage and riders re-parented under a moving anchor).
        {
            TMap<FString, FSCLiftBuildPart> Parts;
            for (const auto& [Id, Placed] : ById) Parts.Add(Id, {Placed.Anchor, Placed.Rig});
            SCVehicleLifts::BuildInteriorLifts(V, InteriorIndex, Interior, Container, bHelper, Parts);
        }
        // C2 fixes: the interior's light groups (what each receives; its lights carry its id).
        for (const FSCInteriorEntity& Entity : Interior.Entities)
            if (Entity.Kind == TEXT("light_group"))
                V->LightGroups.Add({InteriorIndex, Entity.EntityId, Entity.EntityName, Entity.Links, Entity.LightGroupInteractions, FString()});
        // WP-K1: the interior's inventory containers in its placement frame; the owner rig is the first linked entity's.
        for (int32 C = 0; C < Manifest.InventoryContainers.Num(); ++C)
        {
            const FSCInventoryContainerRecord& R = Manifest.InventoryContainers[C];
            if (R.InteriorIndex != InteriorIndex) continue;
            V->InventoryAnchors[C] = NewAnchor(TEXT("Inventory_") + R.EntityClass, Container, ContainerFrame(R.LocalMatrix, bHelper));
            for (const FString& Link : R.Links)
                if (const FEntity* Linked = ById.Find(Link); Linked && Linked->Rig)
                {
                    V->InventoryOwnerRigs[C] = Linked->Rig;
                    break;
                }
        }
        // Proximity sensors: an anchor in the placement frame; the box maps into it like mesh vertices do (CryEngine
        // local -> glTF -> UE: (y, x, z) x 100), half-extents about the offset.
        for (const FSCProximitySensor& Sensor : Interior.ProximitySensors)
        {
            FSCDoorSensor Door;
            for (const FString& Target : Sensor.Links)
                if (const FEntity* Linked = ById.Find(Target); Linked && Linked->Rig) Door.Doors.Add(Linked->Rig);
            if (Door.Doors.IsEmpty()) continue;
            Door.Anchor = NewAnchor(Sensor.EntityClass, Container,
                                    FSCMeshImporter::InstanceTransform(Sensor.LocalMatrix, FMatrix::Identity, bHelper,
                                                                       FSCMeshImporter::InCryFrame(TEXT("cryengine_z_up"), bHelper)));
            const FVector Centre = FVector(Sensor.Offset.Y, Sensor.Offset.X, Sensor.Offset.Z) * 100.0;
            const FVector Half = FVector(Sensor.Extent.Y, Sensor.Extent.X, Sensor.Extent.Z).GetAbs() * 100.0;
            Door.Box = FBox(Centre - Half, Centre + Half);
            Door.ExitDelay = Sensor.ExitDelay;
            V->DoorSensors.Add(MoveTemp(Door));
        }
        FBox Floor(ForceInit);
        TArray<USceneComponent*> Descendants;
        Container->GetChildrenComponents(true, Descendants);
        for (const auto* C : Descendants)
            if (const auto* Primitive = Cast<UPrimitiveComponent>(C); Primitive && Primitive->IsCollisionEnabled()) Floor += Primitive->Bounds.GetBox();
        for (int32 Index = FirstDeferred; Index < DeferredRegistration.Num(); ++Index)
        {
            UStaticMeshComponent* Component = DeferredRegistration[Index];
            Component->UpdateComponentToWorld();
            Component->UpdateBounds();
            if (Component->IsCollisionEnabled() && Component->IsAttachedTo(Container)) Floor += Component->Bounds.GetBox();
        }
        // The container's largest room stands on its floor; without rooms, the bounds of what collides (a container's
        // collision can reach far below its rooms, e.g. a ship's lift down to the ground).
        FVector RoomFloor;
        if (V->LightAreas.Num() > AreasBefore && V->LightAreas.Last().LargestRoomFloor(RoomFloor))
            V->AddSpawnPoint(TEXT("Interior"), Interior.Container.EntityName, FTransform(RoomFloor));
        else if (Floor.IsValid)
            V->AddSpawnPoint(TEXT("Interior"), Interior.Container.EntityName,
                FTransform(FVector(Floor.GetCenter().X, Floor.GetCenter().Y, Floor.Min.Z)));
    }
    // Inventory waypoints stand on the container's floor centre: the container entity sits at the centre of its lattice
    // floor, grid_pos_offset adds to it (USCCargoGridComponent::LatticeToWorld).
    for (int32 WaypointIndex = 0; WaypointIndex < WaypointCandidates.Num(); ++WaypointIndex)
        if (const FSCWaypointCandidate& Candidate = WaypointCandidates[WaypointIndex];
            Candidate.Source == ESCWaypointSource::Inventory && V->InventoryAnchors.IsValidIndex(Candidate.Index) && V->InventoryAnchors[Candidate.Index])
        {
            WaypointSources[WaypointIndex].Source = Candidate.Source;
            WaypointSources[WaypointIndex].Label = Candidate.Label;
            WaypointSources[WaypointIndex].Anchor = V->InventoryAnchors[Candidate.Index].Get();
            WaypointSources[WaypointIndex].Local = FSCCargoLattice::FromSC(Manifest.InventoryContainers[Candidate.Index].Container.GridPosOffset) * 100.0;
        }
    for (FSCWaypointSource& WaypointSource : WaypointSources)
        if (WaypointSource.Anchor.IsValid()) V->WaypointSources.Add(MoveTemp(WaypointSource));
    if (Options.bEnvironment)
        UE_LOG(LogSCImport, Display, TEXT("HANGAR_WAYPOINT_SOURCES chosen=%d anchored=%d"), WaypointCandidates.Num(), V->WaypointSources.Num());
    UE_LOG(LogSCImport, Display, TEXT("DOOR_LINKS panels=%d sensors=%d panel_reach=%d max_reach_cm=%.0f"), V->DoorControls.Num(), V->DoorSensors.Num(),
           V->PanelReach.Num(), V->MaxPanelReach);
    UE_LOG(LogSCImport, Display,
           TEXT("SCREEN_BIND instances=%d images=%d panel_images=%d skipped_mfd=%d slots_bound=%d slots_dark=%d ambiguous=%d unclassified=%d"),
           ScreenBoundInstances, ScreenImagesBound.Num(), Materials.PanelImages.Num(), Materials.SkippedVehicleScreens, ScreenBoundSlots,
           ScreenDarkSlots, ScreenAmbiguousSlots, Materials.UnclassifiedBindings);
    // I1-3: faces whose image follows their panel's state, and the state images they hold; then the faces' initial state.
    const int32 InitialSwaps = V->RefreshPanelScreens();
    UE_LOG(LogSCImport, Display, TEXT("SCREEN_STATES faces=%d state_images=%d hover_images=%d initial_swaps=%d usables=%d"), V->ScreenFaces.Num(),
           ScreenStateImagesBound.Num(), ScreenHoverImagesBound.Num(), InitialSwaps, V->UsableTargets.Num());
    // WP-P5 floor panel: lift panels draw their stops on a blank face (appended to ScreenFaces after the counts above).
    V->BuildFloorPanels(BlankFaces, [&](const FString& Path) { return Materials.Texture(Path); });
    V->BuildButtonPanels(ButtonFaces, [&](const FString& Path) { return Materials.Texture(Path); });
    if (PanelsWithoutReachField)
        Warn(Manifest.ScenePath, FString::Printf(TEXT("%d door-panel UI bindings carry no interaction_distance (an export before cache format 40): ")
            TEXT("those panels are operated from 3 m only (the C2's exterior pads are out of reach); re-export with the current exporter"),
            PanelsWithoutReachField));
    if (Materials.UnclassifiedBindings)
        Warn(Manifest.ScenePath, FString::Printf(TEXT("%d UI bindings carry no screen role (an export before source_item_type, or an unresolved ")
            TEXT("owner): their screens stay dark; re-export with the current exporter (cache format 30) to show door and lift panel images"),
            Materials.UnclassifiedBindings));
    Result.ScreenBoundInstances = ScreenBoundInstances; Result.ScreenBoundSlots = ScreenBoundSlots;
    Result.ScreenDarkSlots = ScreenDarkSlots; Result.ScreenAmbiguousSlots = ScreenAmbiguousSlots;
    UE_LOG(LogSCImport, Display, TEXT("PAINTS variants=%d slots=%d"), Manifest.Paints.Num(), V->PaintSlots.Num());
    UE_LOG(LogSCImport, Display, TEXT("LIGHTS components=%d"), V->Lights.Num());
    UE_LOG(LogSCImport, Display, TEXT("PHYSICS_GRID triangles=%d hull_colliders=%d"), V->PhysicsGridTriangles.Num() / 3, V->HullColliders.Num());
    const double RoomStart = FPlatformTime::Seconds();
    FSCLightAreas::Apply(V); // after every light and placement exists, in the rest pose
    const double RoomSeconds = FPlatformTime::Seconds() - RoomStart;
    FSCDirt::MarkExterior(V); // exterior = outside every room, from the channels just set
    FSCDirt::Push(V->GetWorld());
    // WP-I1-4: a usable is found by the geometry under its anchor (FindUsableTarget): its meshes stay its own.
    for (const auto& [Anchor, Usable] : V->UsableTargets)
        if (const USceneComponent* A = Anchor.Get())
        {
            TArray<USceneComponent*> Parts;
            A->GetChildrenComponents(true, Parts);
            for (USceneComponent* Part : Parts)
                if (Part->IsA<UStaticMeshComponent>()) Part->ComponentTags.AddUnique(FSCInstancing::KeepTag);
        }
    if (Options.bInstanceRepeatedParts) // after light areas and dirt set channels / custom data, before emissive proxies attach
    {
        const FSCInstancingStats Instancing = FSCInstancing::Consolidate(V, DeferredRegistration);
        int32 Registered = 0;
        for (UStaticMeshComponent* Component : DeferredRegistration)
            if (IsValid(Component) && !Component->IsBeingDestroyed())
            {
                const double RegisterStart = FPlatformTime::Seconds();
                Component->RegisterComponent();
                RegisterSeconds += FPlatformTime::Seconds() - RegisterStart;
                ++Registered;
            }
        UE_LOG(LogSCImport, Display, TEXT("INSTANCING components=%d consolidated=%d instanced_components=%d before_registration=%d registered_individually=%d"),
               Instancing.Components, Instancing.Consolidated, Instancing.Instanced, DeferredRegistration.Num() - Registered, Registered);
    }
    if (!V->Lights.IsEmpty()) FSCEmissive::SetLumen(V, USCViewerSettings::Get()->bEmissiveInLumen); // ships and environments, not the player kit
    if (EffectOnlySkipped) UE_LOG(LogSCImport, Display, TEXT("EFFECT_ONLY_PLACEMENTS skipped=%d"), EffectOnlySkipped);
    if (Audio && Options.World->IsGameWorld())
        if (USCAudioSubsystem* AudioSystem = USCAudioSubsystem::Get(V)) AudioSystem->AddScene(Audio.ToSharedRef());
    if (Options.bPublish)
    {
        V->BuildExploration();
        FSCImporter::Publish(Options.World, V, Options.bReplacePrevious);
    }
    if (Options.GeometryStreaming) Options.GeometryStreaming->Attach(V);
    Result.Vehicle = V;
    Result.MeshCount = Meshes.Num();
    Result.TotalSeconds = FPlatformTime::Seconds() - Start;
    Timings.Assemble = FPlatformTime::Seconds() - AssembleStart;
    if (!MeshCacheNamespace.IsEmpty())
    {
        const bool bCommitted = Options.ProcessedCache->Flush();
        UE_LOG(LogSCImport, Display, TEXT("PROCESSED_CACHE meshes_hit=%d mesh_builds=%d collision_builds=%d manifest_written=%d nanite_streamable=%d nanite_demand_paged=%d"),
            Result.CachedMeshes, Result.MeshBuilds, Result.CollisionBuilds, bCommitted, CachedNaniteStreamable, CachedNaniteDemandPaged);
    }
    Stage = ESCImportStage::Succeeded;
    UE_LOG(LogSCImport, Display,
           TEXT("IMPORT_OK seconds=%.4f assets=%d instances=%d children=%d interiors=%d materials=%d palettes=%d clips=%d game_world=%d "
                "render_meshes=%d render_instances=%d textures=%d material_instances=%d material_s=%.4f warnings=%d"),
           Result.TotalSeconds, Result.MeshCount, Result.InstanceCount, Manifest.Children.Num(), Manifest.Interiors.Num(),
           Manifest.Materials.Num(), Manifest.Palettes.Num(), Manifest.Clips.Num(), Options.World->IsGameWorld(), V->Meshes.Num(),
           RenderInstances, Result.TextureCount, Result.MaterialCount, Result.MaterialSeconds, Result.Warnings.Num());
    UE_LOG(LogSCImport, Display,
           TEXT("IMPORT_PHASES read_s=%.4f derive_s=%.4f collision_s=%.4f build_s=%.4f materials_s=%.4f assemble_s=%.4f (clips_s=%.4f rigs_s=%.4f register_s=%.4f lights_s=%.4f anchors_s=%.4f bind_s=%.4f room_s=%.4f)"),
           Timings.Read, Timings.Derive, Timings.Collision, Timings.Build, Timings.Materials, Timings.Assemble, FinalizeSeconds, RigSeconds,
           RegisterSeconds, LightSeconds, AnchorSeconds, BindSeconds, RoomSeconds);
    UE_LOG(LogSCImport, Display, TEXT("IMPORT_PREPARE derive_worker_s=%.4f derive_wait_s=%.4f prepare_worker_s=%.4f prepare_wait_s=%.4f deferred_rigid_parts=%d"),
           Timings.Derive, Timings.DeriveWait, Timings.Prepare, Timings.PrepareWait, Timings.DeferredRigidParts);
    const FBox Bounds = V->GetComponentsBoundingBox(true);
    UE_LOG(LogSCImport, Display, TEXT("BOUNDS_CM min=%s max=%s size=%s"), *Bounds.Min.ToString(), *Bounds.Max.ToString(),
           *Bounds.GetSize().ToString());
}
