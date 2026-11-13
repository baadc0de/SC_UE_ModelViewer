// Incremental package import. Same mapping as the former monolithic FSCImporter::Import, split into steps:
// Reading (worker: manifest + clip sidecars) -> Meshes (per asset: worker derive with look-ahead, worker
// prepare incl. collision BVH, game-thread UObject build) -> Materials (worker decode overlapped with meshes,
// game-thread textures/MIDs) -> Assembling (components, lights, publish).
#include "SCImportJob.h"
#include "SCInstancing.h"
#include "SCVehicle.h"
#include "SCPackageReader.h"
#include "SCImportMaterials.h"
#include "SCSkeletonBuilder.h"
#include "SCAnimImporter.h"
#include "SCLightImporter.h"
#include "SCLightAreas.h"
#include "SCDirt.h"
#include "SCEmissiveLights.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "SCViewerSettings.h"
#include "SCRigComponent.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "PhysicsEngine/BodySetup.h"
#include "Misc/ScopeExit.h"
#include <atomic>

namespace
{
// Unique assets whose derivation may run ahead of the game-thread build: one per worker thread, at most MaxDeriveLookAhead
// (bounds peak CPU mesh memory; a resource budget, not game data). A large ship has ~500 assets.
constexpr int32 MaxDeriveLookAhead = 12;
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
    int32 Owner = INDEX_NONE;
    FSCDerivedMesh Mesh, Translucent;
    bool bGamePhysics = false; // WP-P3: the game's colliders of Owner; hidden, blocks the pawn only
};
/** One unique mesh asset moving through derive -> prepare -> build. */
struct FMeshSlot
{
    FString Path;
    TUniquePtr<FSCDerivedMesh> Data = MakeUnique<FSCDerivedMesh>();
    TFuture<bool> Derive;
    FString DeriveError;
    double DeriveSeconds = 0;
    bool bDeriveLaunched = false, bDeriveWarned = false;
    TFuture<void> Prepare;
    bool bPrepareLaunched = false;
    TArray<TPair<FString, FString>> PrepareWarnings; // (path, message), logged on the game thread
    TSet<int32> TranslucentSlots;
    TArray<int32> SlotTriangles;
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
    ESCImportStage Stage = ESCImportStage::Reading;
    std::atomic<bool> bCancel{false};
    double Start = 0;

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
        if (Vehicle && IsValid(Vehicle) && Vehicle->GetWorld() && !Vehicle->IsActorBeingDestroyed()) Vehicle->Destroy();
        Vehicle = nullptr;
    }

    bool StepRead(bool bBlocking);
    void Begin();
    void LaunchDerives();
    bool StepMesh(bool bBlocking);
    void LaunchPrepare(FMeshSlot& Slot);
    void BeginBuild(FMeshSlot& Slot);
    void BuildNextVariant(FMeshSlot& Slot);
    void FinishBuild(FMeshSlot& Slot);
    bool HasPendingWorkers() const;
    bool StepMaterials(bool bBlocking);
    void Assemble();
};

FSCImportJob::FSCImportJob(const FSCPackageSource& Source, FSCImportOptions Options, TSharedPtr<const FSCPackageSource> SourceOwner)
    : S(MakeUnique<FState>())
{
    check(IsInGameThread());
    S->Source = &Source;
    S->SourceOwner = MoveTemp(SourceOwner);
    S->Options = MoveTemp(Options);
    S->Start = FPlatformTime::Seconds();
    if (!S->Options.World) S->Fail(TEXT("Import requires a world"));
}
FSCImportJob::~FSCImportJob()
{
    S->Drain();
    if (S->Stage != ESCImportStage::Succeeded) S->DestroyPartial();
}
void FSCImportJob::AddReferencedObjects(FReferenceCollector& Collector)
{
    Collector.AddReferencedObject(S->Vehicle);
    Collector.AddReferencedObjects(S->KeepAlive);
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
            return FString::Printf(TEXT("Building mesh %d/%d (part %d/%d)"), S->Current + 1, N, FMath::Min(Slot.NextPart + 1, Slot.Parts.Num()),
                                   Slot.Parts.Num());
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
            Materials.Decode(*Source);
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

void FSCImportJob::FState::LaunchPrepare(FMeshSlot& Slot)
{
    Slot.bPrepareLaunched = true;
    // Runs while no game-thread build is in flight: it is the only writer of the clip catalogue at this time.
    Slot.Prepare = Async(EAsyncExecution::ThreadPool, [this, &Slot] {
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
        for (const auto& Sidecar : Paths)
        {
            const auto* Set = Manifest.Materials.FindByPredicate([&](const FSCMaterialSet& M) { return M.Path == Sidecar; });
            if (!Set) continue;
            for (int32 MaterialSlot = 0; MaterialSlot < Data.MaterialNames.Num(); ++MaterialSlot)
                if (const FSCSubmaterial* Sub = FSCImportMaterials::Resolve(Set, Data.MaterialSourceNames, MaterialSlot))
                {
                    const bool bGlowTarget = Manifest.EngineGlow.Targets.ContainsByPredicate([&](const FSCEngineGlowTarget& T) {
                        return T.MaterialSidecar == Sidecar && T.SourceMaterialIndex == Sub->SourceIndex;
                    });
                    if (FSCMaterialBuilder::RequiresNonNanite(*Sub, bGlowTarget)) TranslucentSlots.Add(MaterialSlot); // D6
                }
        }
        Slot.TranslucentSlots = TranslucentSlots;
        Slot.SlotTriangles.SetNumZeroed(Data.MaterialNames.Num());
        for (auto T : Data.Description.Triangles().GetElementIDs())
            if (Slot.SlotTriangles.IsValidIndex(Data.Description.GetTrianglePolygonGroup(T).GetValue()))
                ++Slot.SlotTriangles[Data.Description.GetTrianglePolygonGroup(T).GetValue()];
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
            if (Slot.bAnimated && Data.SkinWeights.Contains(V[0]))
                Soft.Add(T);
            else
                Rigid.FindOrAdd(Slot.bAnimated ? Data.TriangleNodes.FindRef(T) : INDEX_NONE).Add(T);
        }
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
        for (int32 Owner : Owners)
        {
            FPart Part;
            Part.Owner = Owner;
            Part.Mesh = FSCMeshImporter::Select(Data, Rigid[Owner]);
            Part.Mesh.SocketNode = Owner;
            // Collision includes all triangles of this owner, including glass/decal split, as in U1.
            FString Error;
            if (Options.bBuildCollision && !FSCMeshImporter::DeriveCollision(Part.Mesh, Error))
            {
                Slot.PrepareWarnings.Emplace(Path, Error);
                continue;
            }
            Slot.CollisionSeconds += Part.Mesh.CollisionDeriveSeconds;
            FSCMeshImporter::SplitTranslucent(Part.Mesh, TranslucentSlots, Part.Translucent);
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
                FString Error;
                if (!FSCMeshImporter::DeriveCollision(Part.Mesh, Error))
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
        if (!bBlocking && !Slot.Derive.IsReady()) return false;
        const bool bOK = Slot.Derive.Get();
        Slot.Derive = TFuture<bool>();
        Timings.Derive += Slot.DeriveSeconds;
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
        for (const auto& W : Slot.PrepareWarnings)
            Warn(W.Key, W.Value);
        Timings.Collision += Slot.CollisionSeconds;
        if (Options.bBuildCollision)
            UE_LOG(LogSCImport, Display, TEXT("COLLISION_DERIVE mesh=%s exact_s=%.6f"), *Slot.Path, Slot.CollisionSeconds);
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
    FSCMeshBuildStats Stats;
    FString AssetError;
    UStaticMesh* Mesh = FSCMeshImporter::Instantiate(Variant, Slot.Package, FName(*(SafeName(FPaths::GetBaseFilename(Path)) + Suffix)),
                                                     !bTranslucent && Options.bBuildCollision, UMaterial::GetDefaultMaterial(MD_Surface), Stats,
                                                     AssetError);
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
        else Vehicle->MeshSamples.Add(Mesh, FSCLightAreas::Sample(Variant.Description));
        Slot.Variants.Add(Mesh);
        SocketNodes.FindOrAdd(Path).Add(Variant.SocketNode);
        Result.MeshStats.Add(Path + Suffix, Stats);
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
        UE_LOG(LogSCImport, Display, TEXT("TEXTURES requested=%d created=%d failed=%d bytes=%lld decode_s=%.4f create_s=%.4f"), T.Requested,
               T.Created, T.Failed, T.Bytes, T.DecodeSeconds, T.CreateSeconds);
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
    auto FindParent = [&](const FSCSceneInstance& I, bool& bHelper) -> USceneComponent* {
        bHelper = false;
        if (I.ParentEntityName.IsEmpty() && I.ParentId.IsEmpty()) return V->GetRootComponent();
        FEntity* E = I.ParentId.IsEmpty() ? Entities.Find(I.ParentEntityName) : EntitiesById.Find(I.ParentId);
        if (!E || !E->Anchor)
        {
            Warn(I.EntityName, TEXT("Unresolved parent entity: ") + I.ParentEntityName);
            return nullptr;
        }
        if (I.ParentNodeName.IsEmpty()) return E->Anchor;
        if (auto* Existing = E->Helpers.Find(I.ParentNodeName))
        {
            bHelper = true;
            return *Existing;
        }
        if (E->Rig && E->Rig->DoesSocketExist(FName(*I.ParentNodeName)))
        {
            auto* H = NewAnchor(I.ParentNodeName, E->Rig, FMatrix::Identity);
            H->AttachToComponent(E->Rig, FAttachmentTransformRules::KeepRelativeTransform, FName(*I.ParentNodeName));
            E->Helpers.Add(I.ParentNodeName, H);
            bHelper = true;
            return H;
        }
        const FMatrix* Node = E->Mesh ? E->Mesh->Nodes.Find(I.ParentNodeName) : nullptr;
        if (!Node && E->Mesh)
            for (const auto& Pair : E->Mesh->Nodes)
                if (Pair.Key.Equals(I.ParentNodeName, ESearchCase::IgnoreCase))
                {
                    Node = &Pair.Value;
                    break;
                }
        if (!Node && E->Mesh)
        {
            // The exporter falls back to the entity's source root when a port has no node.
            Node = &E->Mesh->AttachmentRootMatrix;
            UE_LOG(LogSCImport, Display, TEXT("ATTACHMENT_ROOT_FALLBACK entity=%s helper=%s (resolved local transform retained)"),
                   *I.ParentEntityName, *I.ParentNodeName);
        }
        if (!Node)
        {
            Warn(I.EntityName, TEXT("Unresolved parent helper: ") + I.ParentEntityName + TEXT("/") + I.ParentNodeName);
            return nullptr;
        }
        auto* Helper = NewAnchor(I.ParentNodeName, E->Anchor, FSCMeshImporter::Transform(*Node));
        E->Helpers.Add(I.ParentNodeName, Helper);
        bHelper = true;
        return Helper;
    };
    int32 RenderInstances = 0;
    double RegisterSeconds = 0, LightSeconds = 0, RigSeconds = 0;
    // Host paint channel depends only on (sidecar, mesh asset); HostChannel parses submaterial JSON, so compute once.
    TMap<TPair<FString, FString>, FString> HostChannels;
    auto HostFor = [&](const FSCSceneInstance& I, const FSCMaterialSet* Set, const FSCDerivedMesh& Mesh) -> const FString& {
        const TPair<FString, FString> Key(I.MaterialSidecar, I.MeshAsset);
        if (const FString* Found = HostChannels.Find(Key)) return *Found;
        return HostChannels.Add(Key, FSCImportMaterials::HostChannel(Set, Mesh.MaterialSourceNames, MeshSlotTriangles.FindRef(I.MeshAsset)));
    };
    // Paints: each slot a paint touches keeps that paint's instance (built by the material phase) beside the one bound now.
    TMap<TPair<FString, FString>, FString> PaintHosts;
    auto RecordPaint = [&](const FSCSceneInstance& I, UMeshComponent* C, int32 Slot, const FSCDerivedMesh& Mesh)
    {
        if (Manifest.Paints.IsEmpty() || !C) return;
        FSCPaintSlot Entry;
        Entry.Component = C;
        Entry.Slot = Slot;
        Entry.Default = C->GetMaterial(Slot);
        bool bAny = false;
        for (int32 Paint = 0; Paint < Manifest.Paints.Num(); ++Paint)
        {
            FString Sidecar, Palette;
            UMaterialInterface* Material = nullptr;
            if (Manifest.PaintTarget(I, Paint, Sidecar, Palette))
                if (const FSCMaterialSet* Set = Manifest.Materials.FindByPredicate([&](const FSCMaterialSet& M) { return M.Path == Sidecar; }))
                    if (const FSCSubmaterial* Sub = FSCImportMaterials::Resolve(Set, Mesh.MaterialSourceNames, Slot))
                    {
                        const TPair<FString, FString> Key(Sidecar, I.MeshAsset);
                        const FString* Host = PaintHosts.Find(Key);
                        if (!Host) Host = &PaintHosts.Add(Key, FSCImportMaterials::HostChannel(Set, Mesh.MaterialSourceNames, MeshSlotTriangles.FindRef(I.MeshAsset)));
                        Material = Materials.Find(Sidecar, Palette, Sub->Index, *Host);
                    }
            Entry.Paints.Add(Material);
            bAny |= Material != nullptr;
        }
        if (bAny) V->PaintSlots.Add(MoveTemp(Entry));
    };
    auto Place = [&](const FSCSceneInstance& I, USceneComponent* Parent, bool bAtHelper, bool bRoot) -> FEntity {
        FEntity E;
        if (auto* Data = Derived.Find(I.MeshAsset)) E.Mesh = Data->Get();
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
        else if (!I.ParentId.IsEmpty() && E.Mesh) AddHelpers(V->AttachmentHelpers.FindOrAdd(E.Anchor));
        V->AddItemSpawn(I, E.Anchor);
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
            const FString& Host = HostFor(I, Set, *E.Mesh);
            for (int32 Slot = 0; Set && Slot < E.Mesh->MaterialNames.Num(); ++Slot)
                if (const auto* Sub = FSCImportMaterials::Resolve(Set, E.Mesh->MaterialSourceNames, Slot))
                    if (auto* Mat = Materials.Find(I.MaterialSidecar, I.PaletteId, Sub->Index, Host))
                    {
                        E.Rig->SetMaterial(Slot, Mat);
                        RecordPaint(I, E.Rig, Slot, *E.Mesh);
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
                const bool bNanite = Mesh->IsNaniteEnabled();
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
                const FString& Host = HostFor(I, Set, *E.Mesh);
                TArray<int32> GlowSlots;
                for (int32 Slot = 0; Set && Slot < E.Mesh->MaterialNames.Num(); ++Slot)
                {
                    // Bind only sections this variant draws; Nanite never receives a translucent instance.
                    if (bNanite && Translucent.Contains(Slot)) continue;
                    const FSCSubmaterial* Sub = FSCImportMaterials::Resolve(Set, E.Mesh->MaterialSourceNames, Slot);
                    UMaterialInterface* Material = Sub ? Materials.Find(I.MaterialSidecar, I.PaletteId, Sub->Index, Host) : nullptr;
                    // A split (decal/glass/glow) section without its instance is hidden, never drawn opaque.
                    if (!Material && !bNanite && Translucent.Contains(Slot)) Material = Materials.Invisible();
                    if (Material) C->SetMaterial(Slot, Material);
                    if (Material && Material != Materials.Invisible()) RecordPaint(I, C, Slot, *E.Mesh);
                    if (Material && V->EngineGlowMaterials.Contains(Material)) GlowSlots.Add(Slot);
                }
                const double RegisterStart = FPlatformTime::Seconds();
                C->RegisterComponent();
                RegisterSeconds += FPlatformTime::Seconds() - RegisterStart;
                ++RenderInstances;
                for (int32 Slot : GlowSlots) // engine-glow diagnostics: binding, visibility, bounds
                    UE_LOG(LogSCImport, Display,
                           TEXT("ENGINE_GLOW_BOUND entity=%s component=%s slot=%d nanite=%d visible=%d bounds_cm=%s material=%s"),
                           *I.EntityName, *C->GetName(), Slot, bNanite, C->IsVisible(), *C->Bounds.BoxExtent.ToString(),
                           *C->GetMaterial(Slot)->GetName());
            }
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
    }
    for (const auto& Interior : Manifest.Interiors)
    {
        bool bHelper;
        auto* Parent = FindParent(Interior.Container, bHelper);
        if (!Parent) continue;
        const FMatrix W = FSCMeshImporter::CryToGltf();
        auto* Container = NewAnchor(
            Interior.Container.EntityName, Parent,
            FSCMeshImporter::Transform(bHelper ? Interior.Container.LocalMatrix : W.Inverse() * Interior.Container.LocalMatrix * W));
        V->InteriorAnchors.Add(Container);
        FSCLightImporter::Build(
            *Source, Interior.Container, V, Container, [](const FString&) -> USceneComponent* { return nullptr; }, Result.Warnings, bHelper);
        const int32 AreasBefore = V->LightAreas.Num();
        FSCLightAreas::Register(V, Container, Interior.RoomVolumeJson, bHelper, Result.Warnings);
        TMap<FString, FEntity> ById; // entity ids are unique within one container
        for (const auto& P : Interior.Placements)
            if (CanPlace(P))
            {
                FEntity Placed = Place(P, Container, bHelper, false);
                if (!P.EntityId.IsEmpty()) ById.Add(P.EntityId, Placed);
            }
        // Door panels (pads): EntityLinks name the door, or the door's own loadout attached the panel.
        for (const auto& P : Interior.Placements)
        {
            const FEntity* Panel = ById.Find(P.EntityId);
            if (!Panel || !Panel->Anchor) continue;
            TArray<FString> Targets = P.Links;
            if (!P.LoadoutOwnerEntityId.IsEmpty()) Targets.Add(P.LoadoutOwnerEntityId);
            for (const FString& Target : Targets)
                if (const FEntity* Door = ById.Find(Target); Door && Door->Rig && Door->Anchor != Panel->Anchor)
                {
                    V->DoorControls.Add(Panel->Anchor, Door->Rig);
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
        // The container's largest room stands on its floor; without rooms, the bounds of what collides (a container's
        // collision can reach far below its rooms, e.g. a ship's lift down to the ground).
        FVector RoomFloor;
        if (V->LightAreas.Num() > AreasBefore && V->LightAreas.Last().LargestRoomFloor(RoomFloor))
            V->AddSpawnPoint(TEXT("Interior"), Interior.Container.EntityName, FTransform(RoomFloor));
        else if (Floor.IsValid)
            V->AddSpawnPoint(TEXT("Interior"), Interior.Container.EntityName,
                FTransform(FVector(Floor.GetCenter().X, Floor.GetCenter().Y, Floor.Min.Z)));
    }
    UE_LOG(LogSCImport, Display, TEXT("DOOR_LINKS panels=%d sensors=%d"), V->DoorControls.Num(), V->DoorSensors.Num());
    UE_LOG(LogSCImport, Display, TEXT("PAINTS variants=%d slots=%d"), Manifest.Paints.Num(), V->PaintSlots.Num());
    UE_LOG(LogSCImport, Display, TEXT("LIGHTS components=%d"), V->Lights.Num());
    UE_LOG(LogSCImport, Display, TEXT("PHYSICS_GRID triangles=%d hull_colliders=%d"), V->PhysicsGridTriangles.Num() / 3, V->HullColliders.Num());
    FSCLightAreas::Apply(V); // after every light and placement exists, in the rest pose
    FSCDirt::MarkExterior(V); // exterior = outside every room, from the channels just set
    FSCDirt::Push(V->GetWorld());
    if (Options.bInstanceRepeatedParts) // after light areas and dirt set channels / custom data, before emissive proxies attach
    {
        const FSCInstancingStats Instancing = FSCInstancing::Consolidate(V);
        UE_LOG(LogSCImport, Display, TEXT("INSTANCING components=%d consolidated=%d instanced_components=%d"), Instancing.Components,
               Instancing.Consolidated, Instancing.Instanced);
    }
    if (!V->Lights.IsEmpty()) FSCEmissive::SetLumen(V, USCViewerSettings::Get()->bEmissiveInLumen); // ships and environments, not the player kit
    if (EffectOnlySkipped) UE_LOG(LogSCImport, Display, TEXT("EFFECT_ONLY_PLACEMENTS skipped=%d"), EffectOnlySkipped);
    if (Options.bPublish)
    {
        V->BuildExploration();
        FSCImporter::Publish(Options.World, V, Options.bReplacePrevious);
    }
    Result.Vehicle = V;
    Result.MeshCount = Meshes.Num();
    Result.TotalSeconds = FPlatformTime::Seconds() - Start;
    Timings.Assemble = FPlatformTime::Seconds() - AssembleStart;
    Stage = ESCImportStage::Succeeded;
    UE_LOG(LogSCImport, Display,
           TEXT("IMPORT_OK seconds=%.4f assets=%d instances=%d children=%d interiors=%d materials=%d palettes=%d clips=%d game_world=%d "
                "render_meshes=%d render_instances=%d textures=%d material_instances=%d material_s=%.4f warnings=%d"),
           Result.TotalSeconds, Result.MeshCount, Result.InstanceCount, Manifest.Children.Num(), Manifest.Interiors.Num(),
           Manifest.Materials.Num(), Manifest.Palettes.Num(), Manifest.Clips.Num(), Options.World->IsGameWorld(), V->Meshes.Num(),
           RenderInstances, Result.TextureCount, Result.MaterialCount, Result.MaterialSeconds, Result.Warnings.Num());
    UE_LOG(LogSCImport, Display,
           TEXT("IMPORT_PHASES read_s=%.4f derive_s=%.4f collision_s=%.4f build_s=%.4f materials_s=%.4f assemble_s=%.4f (clips_s=%.4f rigs_s=%.4f register_s=%.4f lights_s=%.4f anchors_s=%.4f bind_s=%.4f)"),
           Timings.Read, Timings.Derive, Timings.Collision, Timings.Build, Timings.Materials, Timings.Assemble, FinalizeSeconds, RigSeconds,
           RegisterSeconds, LightSeconds, AnchorSeconds, BindSeconds);
    const FBox Bounds = V->GetComponentsBoundingBox(true);
    UE_LOG(LogSCImport, Display, TEXT("BOUNDS_CM min=%s max=%s size=%s"), *Bounds.Min.ToString(), *Bounds.Max.ToString(),
           *Bounds.GetSize().ToString());
}
