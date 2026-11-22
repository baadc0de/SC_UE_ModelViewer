// FSCVehicleInstances: extra vehicle instances (queue -> export/parse or retained assets -> import/assembly -> envelope,
// free-floor placement, rest on the landing gear), their console commands and the free-floor placement helper.
#include "SCVehicleInstances.h"
#include "SCEnvironment.h"
#include "SCImportJob.h"
#include "SCImporter.h"
#include "SCLook.h"
#include "SCPackageSource.h"
#include "SCVehicle.h"
#include "SCViewOptions.h"
#include "SCViewerPawn.h"
#include "SCViewerSubsystem.h"
#include "Cargo/SCCargoTypes.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RenderingThread.h"
#include "UObject/UObjectIterator.h"
#include "scbridge.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCVehicles, Log, All);

namespace
{
// Game-thread time per frame given to an instance's import (the viewer's own load budget).
constexpr double ImportBudgetSeconds = 0.030;
// The geometry test starts this far above the floor: floor trims, markings and lips are not obstacles.
constexpr double FloorLiftCm = 50.0;
// Vertical reach of the floor traces below the reference floor.
constexpr double FloorProbeCm = 500.0;

TAutoConsoleVariable<int32> CVarShareAssets(TEXT("sc.Vehicle.ShareAssets"), 1,
    TEXT("1: extra vehicle instances of an imported entity are assembled from its retained assets (shared meshes, rigs, "
         "textures, materials); 0: every instance is a fresh import (export cache, full build). Read at load / spawn."));

// The authored Viewer floor bounds the fallback search when no imported hangar is loaded. Derive the rectangle
// from collision-backed flat supports at the reference height, leaving the actual floor and obstruction tests intact.
FBox MapFloorArea(UWorld* World, double Height, double Tolerance)
{
    FBox Out(ForceInit);
    for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
        if (const UStaticMeshComponent* Mesh = It->GetStaticMeshComponent();
            It->GetLevel() == World->PersistentLevel && Mesh && Mesh->IsCollisionEnabled() &&
            Mesh->GetCollisionResponseToChannel(ECC_Visibility) == ECR_Block)
        {
            const FBox Bounds = Mesh->Bounds.GetBox();
            if (Bounds.IsValid && FMath::Abs(Bounds.Max.Z - Height) <= Tolerance &&
                FMath::Min(Bounds.GetSize().X, Bounds.GetSize().Y) > Bounds.GetSize().Z)
                Out += FBox(FVector(Bounds.Min.X, Bounds.Min.Y, 0), FVector(Bounds.Max.X, Bounds.Max.Y, 0));
        }
    return Out;
}

FString Metres(const FVector& Cm) { return FString::Printf(TEXT("%.2f,%.2f,%.2f"), Cm.X / 100, Cm.Y / 100, Cm.Z / 100); }

/** Objects of these actors (subobjects, garbage-marked included) and of these session packages. */
void Track(const TArray<const UObject*>& Actors, const TArray<FString>& Sessions, TArray<TWeakObjectPtr<UObject>>& Out)
{
    if (Actors.IsEmpty() && Sessions.IsEmpty()) return;
    for (TObjectIterator<UObject> It; It; ++It)
    {
        bool bTracked = false;
        for (const UObject* Actor : Actors) bTracked |= Actor && (*It == Actor || It->IsIn(Actor));
        if (!bTracked && !Sessions.IsEmpty())
        {
            const FString Package = It->GetOutermost()->GetName();
            for (const FString& Session : Sessions) bTracked |= !Session.IsEmpty() && Package.StartsWith(Session);
        }
        if (bTracked) Out.Add(*It);
    }
}
int32 CollectAndCount(const TArray<TWeakObjectPtr<UObject>>& Tracked)
{
    FlushRenderingCommands();
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    int32 Alive = 0;
    for (const auto& Object : Tracked) Alive += Object.IsValid(/*bEvenIfPendingKill*/ true);
    return Alive;
}
} // namespace

// ---------------------------------------------------------------- placement

FBox FSCVehiclePlacement::WorldBox(const FBox& Local, const FTransform& Actor)
{
    return Local.IsValid ? Local.TransformBy(Actor) : Local;
}

FBox FSCVehiclePlacement::Envelope(ASCVehicle* Vehicle)
{
    FBox Box(ForceInit);
    if (!Vehicle) return Box;
    const FTransform ToLocal = Vehicle->GetActorTransform().Inverse();
    auto Add = [&] {
        const FBox World = FSCEnvironment::SolidBounds(Vehicle);
        if (World.IsValid) Box += World.TransformBy(ToLocal);
    };
    Vehicle->ResetPose();
    Add();
    // Each clip on its own (clips on the same bones would hide each other's reach), at its last frame.
    if (Vehicle->ClipCatalog)
        for (int32 Index = 0; Index < Vehicle->ClipCatalog->Clips.Num(); ++Index)
            if (!Vehicle->ClipCatalog->Clips[Index].Bindings.IsEmpty() && Vehicle->SnapClipAt(Index, nullptr, true))
            {
                Add();
                Vehicle->ResetPose();
            }
    return Box;
}

TArray<FBox> FSCVehiclePlacement::AccessLanes(const ASCVehicle* Environment)
{
    TArray<FBox> Lanes;
    if (!Environment) return Lanes;
    const TArray<FSCInventoryContainerRecord>& Records = Environment->Manifest.InventoryContainers;
    for (int32 C = 0; C < Records.Num() && C < Environment->InventoryAnchors.Num(); ++C)
    {
        const FSCInventoryContainerRecord& R = Records[C];
        const USceneComponent* Anchor = Environment->InventoryAnchors[C];
        if (R.InteriorIndex == INDEX_NONE || !Anchor) continue;
        // The anchor frame: X = SC y, Y = SC x, Z = SC z, cm; the lattice's origin is its floor centre (+ grid offset).
        const FVector D = R.Container.InteriorDimensions * 100.0, O = R.Container.GridPosOffset * 100.0;
        if (D.X <= 0 || D.Y <= 0) continue;
        const FVector Offset(O.Y, O.X, O.Z);
        const FBox Lattice(Offset + FVector(-D.Y / 2, -D.X / 2, 0), Offset + FVector(D.Y / 2, D.X / 2, FMath::Max(D.Z, 1.0)));
        const double Reach = R.Container.MaxPermittedItemSize.GetMax() * 100.0;
        Lanes.Add(Lattice.ExpandBy(FVector(Reach, Reach, 0)).TransformBy(Anchor->GetComponentTransform()));
    }
    return Lanes;
}

FSCVehiclePlacementResult FSCVehiclePlacement::FindFreeFloor(UWorld* World, const FSCVehiclePlacementQuery& Q)
{
    FSCVehiclePlacementResult R;
    const double Start = FPlatformTime::Seconds();
    auto Done = [&]() -> FSCVehiclePlacementResult {
        R.Seconds = FPlatformTime::Seconds() - Start;
        return R;
    };
    if (!World || !Q.LocalEnvelope.IsValid)
    {
        R.Reason = TEXT("no world or no envelope");
        return Done();
    }
    const FVector Centre = Q.LocalEnvelope.GetCenter(), Extent = Q.LocalEnvelope.GetExtent();
    const double Height = Q.LocalEnvelope.GetSize().Z;
    // Candidate footprint centres on a grid an eighth of the footprint's short side apart, nearest the anchor first.
    const double Step = FMath::Clamp(FMath::Min(Extent.X, Extent.Y) / 4.0, 100.0, 500.0);
    const FBox Area = Q.SearchArea.IsValid ? Q.SearchArea : FBox(Q.Anchor - FVector(20000, 20000, 0), Q.Anchor + FVector(20000, 20000, 0));
    TArray<FVector2D> Points;
    for (double X = Area.Min.X; X <= Area.Max.X; X += Step)
        for (double Y = Area.Min.Y; Y <= Area.Max.Y; Y += Step) Points.Emplace(X, Y);
    const FVector2D Anchor(Q.Anchor);
    Points.Sort([&](const FVector2D& A, const FVector2D& B) { return FVector2D::DistSquared(A, Anchor) < FVector2D::DistSquared(B, Anchor); });

    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCVehiclePlace), true);
    for (const AActor* A : Q.Ignore) Params.AddIgnoredActor(A);
    const FBox Padded = Q.LocalEnvelope.ExpandBy(FVector(Q.ClearanceCm, Q.ClearanceCm, 0));
    // The geometry test box: the envelope plus clearance on every side, from FloorLiftCm above the floor to the top.
    const FVector Half(Extent.X + Q.ClearanceCm, Extent.Y + Q.ClearanceCm, (Height - FloorLiftCm) / 2 + Q.ClearanceCm / 2);
    const FCollisionShape Shape = FCollisionShape::MakeBox(Half);
    int32 KeptOut = 0, Unsupported = 0, Blocked = 0, MissedProbes = 0, ContactProbes = 0;
    double ContactMin = TNumericLimits<double>::Max(), ContactMax = -TNumericLimits<double>::Max();
    auto Refusal = [&](const TCHAR* Why) {
        R.Reason = FString::Printf(TEXT("%s: %d candidates (%d in keep-out boxes, %d without flat floor at %.0f cm, %d blocked)"),
            Why, R.Tested, KeptOut, Unsupported, Q.FloorZ, Blocked);
        const FString ContactRange = ContactProbes > 0
            ? FString::Printf(TEXT("%.1f..%.1f"), ContactMin, ContactMax) : FString(TEXT("-"));
        UE_LOG(LogSCVehicles, Display, TEXT("VEHICLE_FLOOR_SEARCH area_xy_cm=(%.0f,%.0f)..(%.0f,%.0f) datum_cm=%.1f tolerance_cm=%.1f contact_probes=%d missed_probes=%d contact_z_cm=%s"),
               Area.Min.X, Area.Min.Y, Area.Max.X, Area.Max.Y, Q.FloorZ, Q.FloorToleranceCm, ContactProbes, MissedProbes, *ContactRange);
        return Done();
    };
    for (double Yaw : Q.Yaws)
    {
        const FQuat Rot(FRotator(0, Yaw, 0));
        const FVector CentreOffset = Rot.RotateVector(FVector(Centre.X, Centre.Y, 0));
        for (const FVector2D& P : Points)
        {
            if (R.Tested >= Q.MaxCandidates)
                return Refusal(TEXT("candidate limit reached"));
            ++R.Tested;
            const FTransform Actor(Rot, FVector(P.X - CentreOffset.X, P.Y - CentreOffset.Y, Q.FloorZ - Q.LocalEnvelope.Min.Z));
            const FBox Footprint = WorldBox(Padded, Actor);
            if (Q.KeepOut.ContainsByPredicate([&](const FBox& K) { return K.Intersect(Footprint); }))
            {
                ++KeptOut;
                continue;
            }
            // Probe around the reference floor. The clearance-volume test below handles overhead geometry separately.
            double Lowest = TNumericLimits<double>::Max(), Highest = -TNumericLimits<double>::Max();
            bool bSupported = true;
            for (int32 I = -1; I <= 1 && bSupported; ++I)
                for (int32 J = -1; J <= 1 && bSupported; ++J)
                {
                    const FVector Local(Centre.X + I * Extent.X * 0.9, Centre.Y + J * Extent.Y * 0.9, 0);
                    const FVector XY = Actor.TransformPosition(Local);
                    FHitResult Hit;
                    const bool bHit = World->LineTraceSingleByChannel(Hit,
                        FVector(XY.X, XY.Y, Q.FloorZ + Q.FloorToleranceCm + 1.0),
                        FVector(XY.X, XY.Y, Q.FloorZ - FloorProbeCm), ECC_Visibility, Params);
                    ContactProbes += bHit;
                    MissedProbes += !bHit;
                    if (bHit)
                    {
                        ContactMin = FMath::Min(ContactMin, Hit.ImpactPoint.Z);
                        ContactMax = FMath::Max(ContactMax, Hit.ImpactPoint.Z);
                    }
                    bSupported = bHit && FMath::Abs(Hit.ImpactPoint.Z - Q.FloorZ) <= Q.FloorToleranceCm && Hit.ImpactNormal.Z > 0.9;
                    if (bSupported)
                    {
                        Lowest = FMath::Min(Lowest, Hit.ImpactPoint.Z);
                        Highest = FMath::Max(Highest, Hit.ImpactPoint.Z);
                    }
                }
            if (!bSupported || Highest - Lowest > Q.FloorToleranceCm)
            {
                ++Unsupported;
                continue;
            }
            const FVector BoxCentre = Actor.TransformPosition(FVector(Centre.X, Centre.Y, 0)) +
                                      FVector(0, 0, Highest + FloorLiftCm + Half.Z - Actor.GetLocation().Z);
            if (World->OverlapBlockingTestByChannel(FVector(BoxCentre.X, BoxCentre.Y, BoxCentre.Z), Rot, ECC_Visibility, Shape, Params))
            {
                ++Blocked;
                continue;
            }
            R.bFound = true;
            R.GroundZ = Highest;
            R.Transform = FTransform(Rot, FVector(Actor.GetLocation().X, Actor.GetLocation().Y, Highest - Q.LocalEnvelope.Min.Z));
            return Done();
        }
    }
    return Refusal(TEXT("no free floor"));
}

// ---------------------------------------------------------------- instances

bool FSCVehicleInstances::ShareAssets() { return CVarShareAssets.GetValueOnGameThread() != 0; }

FSCVehicleInstances::FSCVehicleInstances(USCViewerSubsystem& InViewer) : Viewer(InViewer)
{
    // -SCExtraVehicles=<Entity>[*N][,<Entity>[*N]...]: auto-placed instances once the loaded vehicle is there; an empty
    // entity ("*2") is the loaded one.
    FString Arg;
    if (FParse::Value(FCommandLine::Get(), TEXT("SCExtraVehicles="), Arg, false)) Arg.ParseIntoArray(CommandLine, TEXT(","), true);
}
FSCVehicleInstances::~FSCVehicleInstances() { Reset(nullptr, false); }

FString FSCVehicleInstances::KeyFor(const FString& Entity) const
{
    // The viewer's reuse key: archive session | entity | options hash.
    return FString::Printf(TEXT("%u|%s|%s"), uint32(Viewer.GetArchiveGeneration()), *Entity.ToLower(),
                           Viewer.GetOptions() ? *Viewer.GetOptions()->GetHash() : TEXT(""));
}
double FSCVehicleInstances::FloorZ() const
{
    // The floor the loaded vehicle stands on (its lowest visible point, rested on the landing gear).
    if (const ASCVehicle* P = Primary.Get())
        if (const double Z = P->LowestVisibleZ(); Z < TNumericLimits<double>::Max()) return Z;
    return 0;
}

int32 FSCVehicleInstances::Spawn(const FString& Entity, bool bAuto, const FTransform& World, bool bRest, bool bGarageFloor)
{
    FInstance& I = Instances.AddDefaulted_GetRef();
    I.Info.Id = NextId++;
    I.Info.Entity = Entity.TrimStartAndEnd();
    if (I.Info.Entity.Equals(TEXT("loaded"), ESearchCase::IgnoreCase)) I.Info.Entity.Empty();
    I.Info.bAutoPlaced = bAuto;
    I.Requested = World;
    I.bRest = bRest;
    I.bGarageFloor = bGarageFloor;
    UE_LOG(LogSCVehicles, Display, TEXT("VEHICLE_QUEUE idx=%d entity=%s placement=%s"), I.Info.Id, I.Info.Entity.IsEmpty() ? TEXT("<loaded>") : *I.Info.Entity,
           bAuto ? TEXT("auto") : *FString::Printf(TEXT("at=%s yaw=%.0f rest=%d"), *Metres(World.GetLocation()), World.Rotator().Yaw, bRest));
    return I.Info.Id;
}

FSCVehicleInstances::FInstance* FSCVehicleInstances::FindInstance(int32 Id)
{
    return Instances.FindByPredicate([Id](const FInstance& I) { return I.Info.Id == Id; });
}
ASCVehicle* FSCVehicleInstances::Find(int32 Id) const
{
    if (Id == 0) return Primary.Get();
    const FInstance* I = Instances.FindByPredicate([Id](const FInstance& X) { return X.Info.Id == Id; });
    return I && I->Info.State == ESCVehicleInstanceState::Ready ? I->Info.Actor.Get() : nullptr;
}
TArray<ASCVehicle*> FSCVehicleInstances::GetVehicles() const
{
    TArray<ASCVehicle*> Out;
    if (ASCVehicle* P = Primary.Get()) Out.Add(P);
    for (const FInstance& I : Instances)
        if (I.Info.State == ESCVehicleInstanceState::Ready && I.Info.Actor.IsValid()) Out.Add(I.Info.Actor.Get());
    return Out;
}
TArray<FSCVehicleInstanceInfo> FSCVehicleInstances::GetInstances() const
{
    TArray<FSCVehicleInstanceInfo> Out;
    if (ASCVehicle* P = Primary.Get())
    {
        FSCVehicleInstanceInfo& Info = Out.AddDefaulted_GetRef();
        Info.Id = 0;
        Info.Entity = Viewer.GetLoadedEntity();
        Info.State = ESCVehicleInstanceState::Ready;
        Info.Progress = 1.f;
        Info.Actor = P;
    }
    for (const FInstance& I : Instances)
    {
        FSCVehicleInstanceInfo& Info = Out.Add_GetRef(I.Info);
        if (Info.State == ESCVehicleInstanceState::Ready) Info.Progress = 1.f;
        else if (Info.Id == Running)
        {
            if (Info.State == ESCVehicleInstanceState::Exporting)
                Info.Progress = Parse.IsValid() ? 0.5f : Progress ? 0.5f * Progress->GetFraction() : 0.f;
            if (Info.State == ESCVehicleInstanceState::Importing && Job) Info.Progress = 0.5f + 0.5f * Job->GetFraction();
        }
    }
    return Out;
}
int32 FSCVehicleInstances::GetId(const ASCVehicle* Vehicle) const
{
    if (!Vehicle) return INDEX_NONE;
    if (Vehicle == Primary.Get()) return 0;
    const FInstance* I = Instances.FindByPredicate([Vehicle](const FInstance& X) { return X.Info.Actor.Get() == Vehicle; });
    return I ? I->Info.Id : INDEX_NONE;
}
bool FSCVehicleInstances::IsSpawning() const
{
    return Running != INDEX_NONE || !CommandLine.IsEmpty() ||
           Instances.ContainsByPredicate([](const FInstance& I) { return I.Info.State == ESCVehicleInstanceState::Queued; });
}
void FSCVehicleInstances::Changed()
{
    ++Revision;
    Viewer.OnVehiclesChanged.Broadcast();
}

void FSCVehicleInstances::SetPrimary(ASCVehicle* InPrimary, const TSharedPtr<FSCImportedAssets>& Assets, const FString& Key)
{
    const bool bChanged = Primary.Get() != InPrimary;
    if (!PrimaryKey.IsEmpty() && PrimaryKey != Key && !Instances.ContainsByPredicate([&](const FInstance& I) { return I.Key == PrimaryKey; }))
    {
        AssetSets.Remove(PrimaryKey);
        SetSessions.Remove(PrimaryKey);
    }
    Primary = InPrimary;
    PrimaryKey = Key;
    if (Assets)
    {
        AssetSets.Add(Key, Assets);
        if (InPrimary) SetSessions.Add(Key, InPrimary->SessionPath);
    }
    if (bChanged) Changed();
}

bool FSCVehicleInstances::RetainsSession(const FString& Session) const
{
    return SetSessions.FindKey(Session) != nullptr;
}

void FSCVehicleInstances::Reset(TArray<TWeakObjectPtr<UObject>>* Tracked, bool bWorldAlive)
{
    if (Progress) Progress->RequestCancel();
    if (Export.IsValid()) Export.Wait();
    if (Parse.IsValid()) Parse.Wait();
    Export = {};
    Parse = {};
    Progress.Reset();
    if (Job && bWorldAlive) Job->Cancel();
    TArray<const UObject*> Actors;
    TArray<FString> Sessions;
    if (Job)
    {
        if (ASCVehicle* Partial = Job->GetSpawnedVehicle()) Actors.Add(Partial);
        Sessions.AddUnique(Job->GetSessionPath());
    }
    Job.Reset(); // drains workers; a partial actor is destroyed
    Running = INDEX_NONE;
    const bool bAny = !Instances.IsEmpty() || Primary.IsValid();
    for (FInstance& I : Instances)
        if (ASCVehicle* V = I.Info.Actor.Get(/*bEvenIfPendingKill*/ true))
        {
            Viewer.ForgetGeometry(V);
            Actors.Add(V);
            // The viewer tracks the loaded vehicle's session (shared by its instances) itself.
            if (!Primary.IsValid() || V->SessionPath != Primary->SessionPath) Sessions.AddUnique(V->SessionPath);
            if (bWorldAlive && IsValid(V) && !V->IsActorBeingDestroyed() && V->GetWorld() && !V->GetWorld()->bIsTearingDown) V->Destroy();
        }
    if (Tracked) Track(Actors, Sessions, *Tracked);
    if (!Instances.IsEmpty() || !AssetSets.IsEmpty())
        UE_LOG(LogSCVehicles, Display, TEXT("VEHICLE_RESET instances=%d asset_sets=%d tracked_objects=%d"), Instances.Num(), AssetSets.Num(),
               Tracked ? Tracked->Num() : -1);
    Instances.Empty();
    AssetSets.Empty(); // the shared objects go with the last holder (GC after this)
    SetSessions.Empty();
    Primary = nullptr;
    PrimaryKey.Empty();
    PlacedIn = nullptr;
    if (bAny) Changed();
}

bool FSCVehicleInstances::Destroy(int32 Id)
{
    if (Id == 0)
    {
        UE_LOG(LogSCVehicles, Warning, TEXT("VEHICLE_DESTROY idx=0 refused: the loaded vehicle goes with sc.Unload or the next load"));
        return false;
    }
    const int32 Index = Instances.IndexOfByPredicate([Id](const FInstance& I) { return I.Info.Id == Id; });
    if (Index == INDEX_NONE) return false;
    FInstance I = MoveTemp(Instances[Index]);
    Instances.RemoveAt(Index);
    TArray<const UObject*> Actors;
    TArray<FString> Sessions;
    if (Running == Id)
    {
        if (Progress) Progress->RequestCancel();
        if (Export.IsValid()) Export.Wait();
        if (Parse.IsValid()) Parse.Wait();
        Export = {};
        Parse = {};
        if (Job)
        {
            Job->Cancel();
            if (ASCVehicle* Partial = Job->GetSpawnedVehicle()) Actors.Add(Partial);
        }
        Job.Reset();
        Running = INDEX_NONE;
    }
    if (ASCVehicle* V = I.Info.Actor.Get())
    {
        Viewer.ForgetGeometry(V);
        Actors.Add(V);
        V->Destroy();
    }
    // The last instance of an entity other than the loaded one takes its asset set (and session objects) along.
    const bool bLastOfKey = !I.Key.IsEmpty() && I.Key != PrimaryKey && !Instances.ContainsByPredicate([&](const FInstance& X) { return X.Key == I.Key; });
    int32 Released = 0;
    if (bLastOfKey)
        if (TSharedPtr<FSCImportedAssets> Set = AssetSets.FindRef(I.Key))
        {
            Released = FSCImportJob::CountRetainedObjects(*Set);
            AssetSets.Remove(I.Key);
            SetSessions.Remove(I.Key);
        }
    // Its import session's objects too, unless the loaded vehicle, another instance or a kept asset set still uses them.
    if (const ASCVehicle* V = I.Info.Actor.Get(/*bEvenIfPendingKill*/ true); V && !V->SessionPath.IsEmpty())
    {
        const FString& Session = V->SessionPath;
        const bool bInUse = (Primary.IsValid() && Primary->SessionPath == Session) ||
                            Instances.ContainsByPredicate([&](const FInstance& X) { return X.Info.Actor.IsValid() && X.Info.Actor->SessionPath == Session; }) ||
                            SetSessions.FindKey(Session) != nullptr;
        if (!bInUse) Sessions.Add(Session);
    }
    TArray<TWeakObjectPtr<UObject>> Tracked;
    Track(Actors, Sessions, Tracked);
    const int32 Alive = CollectAndCount(Tracked);
    UE_LOG(LogSCVehicles, Display, TEXT("VEHICLE_DESTROY idx=%d entity=%s tracked=%d alive_after_gc=%d asset_set_released=%d shared_objects=%d"), Id,
           *I.Info.Entity, Tracked.Num(), Alive, bLastOfKey && Released > 0, Released);
    Changed();
    return true;
}

void FSCVehicleInstances::Tick()
{
    UWorld* World = Viewer.GetGameInstance() ? Viewer.GetGameInstance()->GetWorld() : nullptr;
    if (!World || World->bIsTearingDown) return;
    if (!CommandLine.IsEmpty() && Primary.IsValid() && !Viewer.IsBusy())
    {
        for (const FString& Entry : CommandLine)
        {
            FString Entity = Entry, Count;
            int32 N = 1;
            if (Entry.Split(TEXT("*"), &Entity, &Count)) N = FMath::Max(1, FCString::Atoi(*Count));
            for (int32 K = 0; K < N; ++K) Spawn(Entity, true, FTransform::Identity, true);
        }
        CommandLine.Empty();
    }
    // A new environment (or the same one placed again): auto-placed instances move to free floor in it.
    if (PlacedIn.Get() != Viewer.GetEnvironment() && !Viewer.IsEnvironmentLoading() && Running == INDEX_NONE)
    {
        const bool bAnyAuto = Instances.ContainsByPredicate([](const FInstance& I) { return I.Info.bAutoPlaced && I.Info.State == ESCVehicleInstanceState::Ready; });
        PlacedIn = Viewer.GetEnvironment();
        if (bAnyAuto) ReplaceAll();
    }
    if (Running != INDEX_NONE)
    {
        StepRunning();
        return;
    }
    if (Viewer.IsBusy()) return;
    if (FInstance* Next = Instances.FindByPredicate([&](const FInstance& I) {
        return I.Info.State == ESCVehicleInstanceState::Queued && (Primary.IsValid() || (I.bGarageFloor && !I.Info.Entity.IsEmpty()));
    }))
        Start(*Next);
}

void FSCVehicleInstances::Start(FInstance& I)
{
    UWorld* World = Viewer.GetGameInstance()->GetWorld();
    if (I.Info.Entity.IsEmpty()) I.Info.Entity = Viewer.GetLoadedEntity();
    I.Key = KeyFor(I.Info.Entity);
    I.Started = FPlatformTime::Seconds();
    Running = I.Info.Id;
    FSCImportOptions Options;
    Options.World = World;
    Options.ActorTransform = I.Info.bAutoPlaced ? FTransform::Identity : I.Requested;
    Options.bPublish = false; // not the world's current import: the viewer's vehicle slot stays the loaded one
    Options.bReplacePrevious = false;
    if (const TSharedPtr<FSCImportedAssets>* Set = AssetSets.Find(I.Key); Set && *Set && ShareAssets())
    {
        // TODO(WP-K1): the assembly is one game-thread step (~5 s for a Hull A); split FSCImportJob's Assemble into budgeted steps.
        I.Info.bShared = true;
        I.Info.State = ESCVehicleInstanceState::Importing;
        Job = MakeShared<FSCImportJob>(Set->ToSharedRef(), Options);
        return;
    }
    if (Viewer.GetArchiveState() != ESCArchiveState::Ready || !Viewer.GetClient())
    {
        Fail(I, TEXT("the entity has no retained import and the archive is not open (packages are exported from Data.p4k)"));
        return;
    }
    sc_options Ffi = FSCBridgeClient::DefaultOptions();
    if (Viewer.GetOptions()) Viewer.GetOptions()->ToFfi(Ffi);
    Progress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
    Export = Viewer.GetClient()->ExportDecomposed(I.Info.Entity, Ffi, Progress.ToSharedRef());
    I.Info.State = ESCVehicleInstanceState::Exporting;
}

void FSCVehicleInstances::StepRunning()
{
    FInstance* I = FindInstance(Running);
    if (!I)
    {
        Running = INDEX_NONE;
        return;
    }
    if (Export.IsValid())
    {
        if (!Export.IsCompleted()) return;
        FSCExportResult Result = MoveTemp(Export.GetResult());
        Export = {};
        Progress.Reset();
        I->ExportSeconds = Result.Seconds;
        UE_LOG(LogSCVehicles, Display, TEXT("VEHICLE_EXPORT idx=%d entity=%s ok=%d seconds=%.2f cache_hit=%d bytes=%lld"), I->Info.Id, *I->Info.Entity, Result.bOK,
               Result.Seconds, Result.Cache.bLastHit, Result.Tar.Num());
        if (!Result.bOK)
        {
            Fail(*I, Result.Error.ToString());
            return;
        }
        Parse = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Tar = MoveTemp(Result.Tar)]() mutable {
            return TSharedPtr<FSCTarPackageSource>(MakeShared<FSCTarPackageSource>(MoveTemp(Tar)));
        });
        return;
    }
    if (Parse.IsValid())
    {
        if (!Parse.IsCompleted()) return;
        TSharedPtr<FSCTarPackageSource> Source = Parse.GetResult();
        Parse = {};
        if (!Source || !Source->IsValid())
        {
            Fail(*I, Source ? Source->GetError() : FString(TEXT("cannot read the package")));
            return;
        }
        FSCImportOptions Options;
        Options.World = Viewer.GetGameInstance()->GetWorld();
        Options.ActorTransform = I->Info.bAutoPlaced ? FTransform::Identity : I->Requested;
        Options.bBuildCollision = !Viewer.GetOptions() || Viewer.GetOptions()->bCollision;
        Options.bPublish = false;
        Options.bReplacePrevious = false;
        Options.bRetainAssets = ShareAssets();
        Viewer.ConfigureProcessedImport(Options);
        Job = MakeShared<FSCImportJob>(*Source, Options, Source);
        I->Info.State = ESCVehicleInstanceState::Importing;
        return;
    }
    if (!Job || !Job->Tick(false, ImportBudgetSeconds)) return;
    const FSCImportTimings T = Job->GetTimings();
    I->ImportSeconds = FPlatformTime::Seconds() - I->Started - I->ExportSeconds;
    if (Job->GetStage() != ESCImportStage::Succeeded)
    {
        const FString Error = Job->GetResult().Error;
        Job.Reset(); // a partial actor is destroyed
        Fail(*I, Error);
        return;
    }
    ASCVehicle* Actor = Job->GetResult().Vehicle.Get();
    if (const TSharedPtr<FSCImportedAssets> Set = Job->GetRetainedAssets(); Set && Actor && !AssetSets.Contains(I->Key))
    {
        AssetSets.Add(I->Key, Set);
        SetSessions.Add(I->Key, Actor->SessionPath);
    }
    UE_LOG(LogSCVehicles, Display, TEXT("VEHICLE_IMPORT idx=%d shared=%d import_s=%.2f derive_s=%.2f build_s=%.2f materials_s=%.2f assemble_s=%.2f warnings=%d"),
           I->Info.Id, I->Info.bShared, I->ImportSeconds, T.Derive, T.Build, T.Materials, T.Assemble, Job->GetResult().Warnings.Num());
    Job.Reset();
    Running = INDEX_NONE;
    Finish(*I, Actor);
}

void FSCVehicleInstances::Fail(FInstance& I, const FString& Error)
{
    I.Info.State = ESCVehicleInstanceState::Failed;
    I.Info.Error = Error;
    if (Running == I.Info.Id) Running = INDEX_NONE;
    UE_LOG(LogSCVehicles, Warning, TEXT("VEHICLE_SPAWN idx=%d entity=%s ok=0 error=\"%s\""), I.Info.Id, *I.Info.Entity, *Error);
    Changed();
}

bool FSCVehicleInstances::Place(FInstance& I, const TArray<const AActor*>& Others, FString& OutError)
{
    ASCVehicle* V = I.Info.Actor.Get();
    if (!V) return false;
    const bool bFresh = I.Info.State != ESCVehicleInstanceState::Ready;
    if (!I.Info.bAutoPlaced)
    {
        // TODO(WP-K1): an explicit transform is taken as given: no overlap check against geometry or other vehicles.
        if (bFresh && I.bRest) USCImportLibrary::RestOnFloor(V, I.Requested.GetLocation().Z);
        return true;
    }
    double EnvelopeSeconds = 0;
    const FBox* Cached = Envelopes.Find(I.Key);
    FBox Local = Cached ? *Cached : FBox(ForceInit);
    if (!Cached)
    {
        const double Start = FPlatformTime::Seconds();
        Local = FSCVehiclePlacement::Envelope(V); // poses every clip, then the bind pose
        EnvelopeSeconds = FPlatformTime::Seconds() - Start;
        Envelopes.Add(I.Key, Local);
    }
    FSCVehiclePlacementQuery Q;
    Q.LocalEnvelope = Local;
    Q.FloorZ = FloorZ();
    ASCVehicle* P = Primary.Get();
    // Beside the loaded vehicle: the search starts at its footprint centre (its envelope when known), so instances of
    // the same entity line up with it.
    const FBox* PrimaryEnvelope = Envelopes.Find(PrimaryKey);
    const FBox PrimaryBox = !P ? FBox(ForceInit)
                          : PrimaryEnvelope ? FSCVehiclePlacement::WorldBox(*PrimaryEnvelope, P->GetActorTransform())
                                            : FSCEnvironment::SolidBounds(P);
    Q.Anchor = PrimaryBox.IsValid ? PrimaryBox.GetCenter() : P ? P->GetActorLocation() : FVector::ZeroVector;
    const double PrimaryYaw = P ? P->GetActorRotation().Yaw : 0;
    for (double& Yaw : Q.Yaws) Yaw += PrimaryYaw;
    Q.Ignore = Others;
    Q.Ignore.AddUnique(V);
    const ASCVehicle* Environment = Viewer.GetEnvironment();
    if (Environment)
    {
        const FBox Bounds = Environment->GetComponentsBoundingBox(true);
        Q.SearchArea = FBox(FVector(Bounds.Min.X, Bounds.Min.Y, 0), FVector(Bounds.Max.X, Bounds.Max.Y, 0));
    }
    else
        Q.SearchArea = MapFloorArea(V->GetWorld(), Q.FloorZ, Q.FloorToleranceCm);
    Q.KeepOut = FSCVehiclePlacement::AccessLanes(Environment);
    FBox Bay(ForceInit);
    if (I.bGarageFloor && FSCEnvironment::FindShipBay(Environment, Bay))
    {
        // A primary ship can lower a ramp or change its landing gear. Use the authored pad to locate the surrounding floor.
        Q.FloorZ = Bay.Max.Z;
        Q.KeepOut.Add(Bay);
    }
    const int32 Lanes = Q.KeepOut.Num();
    // Every other vehicle that stays where it is: its envelope (cached for its entity), else its current solid bounds.
    auto KeepOutOf = [&](const ASCVehicle* Other, const FString& Key) {
        if (!Other || Q.Ignore.Contains(Other)) return;
        const FBox* Env = Envelopes.Find(Key);
        const FBox Box = Env ? FSCVehiclePlacement::WorldBox(*Env, Other->GetActorTransform()) : FSCEnvironment::SolidBounds(Other);
        if (Box.IsValid) Q.KeepOut.Add(Box.ExpandBy(FVector(Q.ClearanceCm, Q.ClearanceCm, 0)));
    };
    KeepOutOf(P, PrimaryKey);
    for (const FInstance& X : Instances)
        if (X.Info.Id != I.Info.Id && X.Info.State == ESCVehicleInstanceState::Ready) KeepOutOf(X.Info.Actor.Get(), X.Key);
    const FSCVehiclePlacementResult R = FSCVehiclePlacement::FindFreeFloor(V->GetWorld(), Q);
    UE_LOG(LogSCVehicles, Display, TEXT("VEHICLE_PLACE idx=%d found=%d tested=%d seconds=%.2f envelope_m=%.1fx%.1fx%.1f envelope_s=%.2f floor_z=%.1f lanes=%d keep_out=%d%s%s"),
           I.Info.Id, R.bFound, R.Tested, R.Seconds, Local.GetSize().X / 100, Local.GetSize().Y / 100, Local.GetSize().Z / 100, EnvelopeSeconds, Q.FloorZ, Lanes,
           Q.KeepOut.Num(), R.bFound ? *FString::Printf(TEXT(" at=%s yaw=%.0f"), *Metres(R.Transform.GetLocation()), R.Transform.Rotator().Yaw) : TEXT(""),
           R.bFound ? TEXT("") : *FString::Printf(TEXT(" reason=\"%s\""), *R.Reason));
    if (!R.bFound)
    {
        OutError = I.bGarageFloor && R.Reason.StartsWith(TEXT("no free floor"))
            ? TEXT("No clear floor spot. Try a smaller ship or retrieve to the bay.") : R.Reason;
        return false;
    }
    V->SetActorTransform(R.Transform, false, nullptr, ETeleportType::TeleportPhysics);
    if (bFresh) USCImportLibrary::RestOnFloor(V, R.GroundZ); // landing gear, then the lowest visible point on the floor
    else if (const double Lowest = V->LowestVisibleZ(); Lowest < TNumericLimits<double>::Max())
        V->AddActorWorldOffset(FVector(0, 0, R.GroundZ - Lowest)); // keeps its pose (plates, gear, doors)
    return true;
}

void FSCVehicleInstances::Finish(FInstance& I, ASCVehicle* Actor)
{
    if (!Actor)
    {
        Fail(I, TEXT("the import produced no actor"));
        return;
    }
    I.Info.Actor = Actor;
    Actor->SetActorHiddenInGame(true); // never drawn before it is placed
    const double PlaceStart = FPlatformTime::Seconds();
    FString Error;
    if (!Place(I, {}, Error))
    {
        Actor->Destroy();
        I.Info.Actor = nullptr;
        Fail(I, Error);
        return;
    }
    Actor->BuildExploration();
    Actor->SetActorHiddenInGame(false);
    PlacedIn = Viewer.GetEnvironment();
    SCLook::UpdateShortRangeAO(Actor->GetWorld());
    SCLook::RefreshGlassRefraction(Actor->GetWorld());
    I.Info.State = ESCVehicleInstanceState::Ready;
    I.Info.Seconds = FPlatformTime::Seconds() - I.Started;
    const FBox Bounds = FSCEnvironment::SolidBounds(Actor);
    UE_LOG(LogSCVehicles, Display,
           TEXT("VEHICLE_SPAWN idx=%d entity=%s ok=1 at=%s yaw=%.0f auto=%d shared=%d export_s=%.2f import_s=%.2f place_s=%.2f total_s=%.2f bounds_m=%s..%s "
                "rigs=%d lights=%d meshes=%d spawns=%d doors=%d"),
           I.Info.Id, *I.Info.Entity, *Metres(Actor->GetActorLocation()), Actor->GetActorRotation().Yaw, I.Info.bAutoPlaced, I.Info.bShared, I.ExportSeconds,
           I.ImportSeconds, FPlatformTime::Seconds() - PlaceStart, I.Info.Seconds, *Metres(Bounds.Min), *Metres(Bounds.Max), Actor->Rigs.Num(),
           Actor->Lights.Num(), Actor->Meshes.Num(), Actor->SpawnPoints.Num(), Actor->Doors.Num());
    Changed();
}

void FSCVehicleInstances::ReplaceAll()
{
    TArray<const AActor*> Moving;
    for (const FInstance& I : Instances)
        if (I.Info.bAutoPlaced && I.Info.State == ESCVehicleInstanceState::Ready && I.Info.Actor.IsValid()) Moving.Add(I.Info.Actor.Get());
    int32 Placed = 0;
    for (FInstance& I : Instances)
    {
        if (!I.Info.bAutoPlaced || I.Info.State != ESCVehicleInstanceState::Ready || !I.Info.Actor.IsValid()) continue;
        Moving.Remove(I.Info.Actor.Get());
        FString Error;
        if (Place(I, Moving, Error)) ++Placed;
        else UE_LOG(LogSCVehicles, Warning, TEXT("VEHICLE_REPLACE idx=%d kept where it was: %s"), I.Info.Id, *Error);
    }
    UE_LOG(LogSCVehicles, Display, TEXT("VEHICLE_REPLACE environment=%s placed=%d"), Viewer.GetEnvironment() ? *Viewer.GetEnvironment()->GetName() : TEXT("none"), Placed);
    Changed();
}

// ---------------------------------------------------------------- console

namespace
{
FSCVehicleInstances* InstanceSet(UWorld* World, USCViewerSubsystem*& OutViewer)
{
    OutViewer = USCViewerSubsystem::Get(World);
    return OutViewer ? OutViewer->GetVehicleInstanceSet() : nullptr;
}
const TCHAR* StateName(ESCVehicleInstanceState S)
{
    switch (S)
    {
    case ESCVehicleInstanceState::Queued: return TEXT("queued");
    case ESCVehicleInstanceState::Exporting: return TEXT("exporting");
    case ESCVehicleInstanceState::Importing: return TEXT("importing");
    case ESCVehicleInstanceState::Ready: return TEXT("ready");
    case ESCVehicleInstanceState::Failed: return TEXT("failed");
    }
    return TEXT("?");
}
/** Clip of V by exact id, else the first whose id contains Part (both case-insensitive); bound clips only. */
int32 FindClip(const ASCVehicle* V, const FString& Part)
{
    if (!V || !V->ClipCatalog) return INDEX_NONE;
    const TArray<FSCImportedClip>& Clips = V->ClipCatalog->Clips;
    const int32 Exact = Clips.IndexOfByPredicate([&](const FSCImportedClip& C) { return !C.Bindings.IsEmpty() && C.Id.ToString().Equals(Part, ESearchCase::IgnoreCase); });
    return Exact != INDEX_NONE ? Exact
                               : Clips.IndexOfByPredicate([&](const FSCImportedClip& C) { return !C.Bindings.IsEmpty() && C.Id.ToString().Contains(Part); });
}

FAutoConsoleCommandWithWorldAndArgs GSCVehicleSpawn(
    TEXT("sc.Vehicle.Spawn"), TEXT("sc.Vehicle.Spawn [entity|loaded] [count]: more instances of an entity on free floor beside the loaded vehicle"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSubsystem* Viewer;
        if (!InstanceSet(World, Viewer)) return;
        const FString Entity = Args.IsValidIndex(0) ? Args[0] : FString();
        const int32 Count = Args.IsValidIndex(1) ? FMath::Max(1, FCString::Atoi(*Args[1])) : 1;
        for (int32 K = 0; K < Count; ++K) Viewer->SpawnVehicleInstance(Entity);
    }));
FAutoConsoleCommandWithWorldAndArgs GSCVehicleSpawnAt(
    TEXT("sc.Vehicle.SpawnAt"),
    TEXT("sc.Vehicle.SpawnAt <entity|loaded> <x m> <y m> [floor z m] [yaw deg]: an instance standing there (z: the loaded vehicle's floor)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSubsystem* Viewer;
        if (!InstanceSet(World, Viewer) || Args.Num() < 3) return;
        double Z = 0;
        if (Args.IsValidIndex(3)) Z = FCString::Atod(*Args[3]) * 100;
        else if (const ASCVehicle* P = Viewer->GetVehicle()) Z = P->LowestVisibleZ();
        const FRotator Yaw(0, Args.IsValidIndex(4) ? FCString::Atod(*Args[4]) : 0, 0);
        Viewer->SpawnVehicleInstance(Args[0], FTransform(Yaw, FVector(FCString::Atod(*Args[1]) * 100, FCString::Atod(*Args[2]) * 100, Z)));
    }));
FAutoConsoleCommandWithWorldAndArgs GSCVehicleList(
    TEXT("sc.Vehicle.List"), TEXT("sc.Vehicle.List: the vehicle instances (0 = the loaded vehicle)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World) {
        USCViewerSubsystem* Viewer;
        if (!InstanceSet(World, Viewer)) return;
        for (const FSCVehicleInstanceInfo& I : Viewer->GetVehicleInstances())
        {
            const ASCVehicle* V = I.Actor.Get();
            UE_LOG(LogSCVehicles, Display, TEXT("VEHICLE_LIST idx=%d entity=%s state=%s auto=%d shared=%d seconds=%.2f at=%s yaw=%.0f actor=%s error=\"%s\""), I.Id,
                   *I.Entity, StateName(I.State), I.bAutoPlaced, I.bShared, I.Seconds, V ? *Metres(V->GetActorLocation()) : TEXT("-"),
                   V ? V->GetActorRotation().Yaw : 0.0, V ? *V->GetName() : TEXT("-"), *I.Error);
        }
        UE_LOG(LogSCVehicles, Display, TEXT("VEHICLE_LIST revision=%d vehicles=%d spawning=%d"), Viewer->GetVehiclesRevision(), Viewer->GetVehicles().Num(),
               Viewer->IsSpawningVehicles());
    }));
FAutoConsoleCommandWithWorldAndArgs GSCVehicleDestroy(
    TEXT("sc.Vehicle.Destroy"), TEXT("sc.Vehicle.Destroy <index>: remove an extra vehicle instance (sc.Vehicle.List)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSubsystem* Viewer;
        if (InstanceSet(World, Viewer) && Args.IsValidIndex(0) && !Viewer->DestroyVehicleInstance(FCString::Atoi(*Args[0])))
            UE_LOG(LogSCVehicles, Warning, TEXT("VEHICLE_DESTROY idx=%s: no such extra instance"), *Args[0]);
    }));
FAutoConsoleCommandWithWorldAndArgs GSCVehicleClips(
    TEXT("sc.Vehicle.Clips"), TEXT("sc.Vehicle.Clips <index>: the bound clips of a vehicle instance"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSubsystem* Viewer;
        FSCVehicleInstances* Set = InstanceSet(World, Viewer);
        const ASCVehicle* V = Set && Args.IsValidIndex(0) ? Set->Find(FCString::Atoi(*Args[0])) : nullptr;
        if (!V || !V->ClipCatalog) return;
        for (int32 Index = 0; Index < V->ClipCatalog->Clips.Num(); ++Index)
            if (const FSCImportedClip& C = V->ClipCatalog->Clips[Index]; !C.Bindings.IsEmpty())
                UE_LOG(LogSCVehicles, Display, TEXT("VEHICLE_CLIP idx=%s clip=%d id=%s owner=%s frames=%d bindings=%d"), *Args[0], Index, *C.Id.ToString(), *C.Owner,
                       C.LastFrame, C.Bindings.Num());
    }));
FAutoConsoleCommandWithWorldAndArgs GSCVehicleClip(
    TEXT("sc.Vehicle.Clip"),
    TEXT("sc.Vehicle.Clip <index> <clip id|id part> [speed]: play a clip on one vehicle instance (every rig it binds; speed < 0 plays it back)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSubsystem* Viewer;
        FSCVehicleInstances* Set = InstanceSet(World, Viewer);
        ASCVehicle* V = Set && Args.Num() >= 2 ? Set->Find(FCString::Atoi(*Args[0])) : nullptr;
        const int32 Clip = FindClip(V, Args.IsValidIndex(1) ? Args[1] : FString());
        const float Speed = Args.IsValidIndex(2) ? FCString::Atof(*Args[2]) : 1.f;
        const bool bOK = V && Clip != INDEX_NONE && V->PlayClipAt(Clip, nullptr, Speed);
        UE_LOG(LogSCVehicles, Display, TEXT("VEHICLE_CLIP_PLAY idx=%s clip=%s resolved=%s speed=%.2f ok=%d"), Args.IsValidIndex(0) ? *Args[0] : TEXT("?"),
               Args.IsValidIndex(1) ? *Args[1] : TEXT("?"), V && Clip != INDEX_NONE ? *V->ClipCatalog->Clips[Clip].Id.ToString() : TEXT("-"), Speed, bOK);
    }));
FAutoConsoleCommandWithWorldAndArgs GSCVehicleFocus(
    TEXT("sc.Vehicle.Focus"), TEXT("sc.Vehicle.Focus <index|all>: frame one vehicle instance, or all of them, with the orbit camera"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSubsystem* Viewer;
        FSCVehicleInstances* Set = InstanceSet(World, Viewer);
        ASCViewerPawn* Pawn = Viewer ? Viewer->GetViewerPawn() : nullptr;
        if (!Set || !Pawn) return;
        FBox Box(ForceInit);
        if (!Args.IsValidIndex(0) || Args[0].Equals(TEXT("all"), ESearchCase::IgnoreCase))
            for (const ASCVehicle* V : Set->GetVehicles()) Box += FSCEnvironment::SolidBounds(V);
        else
            Box = FSCEnvironment::SolidBounds(Set->Find(FCString::Atoi(*Args[0])));
        if (Box.IsValid) Pawn->FocusBounds(Box);
    }));
} // namespace
