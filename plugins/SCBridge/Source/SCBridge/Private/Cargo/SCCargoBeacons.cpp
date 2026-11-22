// Freestyle cargo beacons (WP-K2): the pure rules (FSCCargoBeacons) and the subsystem's loose cargo. A box let go of is
// let down where it is; at rest it is frozen there and joins or founds a beacon, a virtual location transfers take as a
// source. A load let go of in flight first tries the nearest free slot it can still reach (RetargetLoad). Loose boxes
// can also be put down directly (AddLooseBoxes: the RPC harness, tests).
#include "Cargo/SCCargoBeacons.h"
#include "Cargo/SCCargoSubsystem.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoGridComponent.h"
#include "Cargo/SCCargoPlanner.h"
#include "SCEnvironment.h"
#include "SCVehicle.h"
#include "SCViewerSubsystem.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"

// ------------------------------------------------------------------------------------------------------------ rules

double FSCCargoBeacons::NearCm(const FBox& A, const FBox& B)
{
    return FMath::Max(A.GetSize().GetMax(), B.GetSize().GetMax());
}

int32 FSCCargoBeacons::Join(const TArray<FSCCargoBeaconModel>& Beacons, const TMap<int32, FBox>& MemberBounds, const FBox& Bounds, int32 OriginId, FName Owner)
{
    int32 Best = INDEX_NONE;
    double BestCm = TNumericLimits<double>::Max();
    for (const FSCCargoBeaconModel& B : Beacons)
    {
        if (B.OriginId != OriginId || B.Owner != Owner) continue;
        for (const int32 Id : B.Boxes)
            if (const FBox* M = MemberBounds.Find(Id))
            {
                const double Gap = FMath::Sqrt(Bounds.ComputeSquaredDistanceToBox(*M));
                if (Gap <= NearCm(Bounds, *M) && Gap < BestCm) { BestCm = Gap; Best = B.Id; }
            }
    }
    return Best;
}

FString FSCCargoBeacons::Name(TFunctionRef<int32(int32 Max)> Rand, const TSet<FString>& Taken)
{
    static const TCHAR* Words[] = {TEXT("Kestrel"), TEXT("Heron"),  TEXT("Lynx"),  TEXT("Osprey"), TEXT("Marten"), TEXT("Ibex"),
                                   TEXT("Falcon"),  TEXT("Otter"),  TEXT("Wren"),  TEXT("Puma"),   TEXT("Bison"),  TEXT("Gecko"),
                                   TEXT("Raven"),   TEXT("Mako"),   TEXT("Orca"),  TEXT("Tern"),   TEXT("Fox"),    TEXT("Hare"),
                                   TEXT("Badger"),  TEXT("Crane"),  TEXT("Egret"), TEXT("Stoat"),  TEXT("Condor"), TEXT("Jackal")};
    FString Name;
    for (int32 Try = 0; Try < 20; ++Try)
    {
        Name = FString::Printf(TEXT("Loose cargo %s-%02d"), Words[Rand(UE_ARRAY_COUNT(Words))], Rand(100));
        if (!Taken.Contains(Name)) return Name;
    }
    for (int32 N = 2;; ++N)
        if (const FString Numbered = FString::Printf(TEXT("%s (%d)"), *Name, N); !Taken.Contains(Numbered)) return Numbered;
}

TArray<int32> FSCCargoBeacons::NearestFirst(const TArray<TPair<int32, FBox>>& Candidates, const FVector& P, const TSet<int32>& Tried)
{
    TArray<TPair<int32, FBox>> Left = Candidates.FilterByPredicate([&](const TPair<int32, FBox>& C) { return !Tried.Contains(C.Key); });
    Left.StableSort([&](const TPair<int32, FBox>& A, const TPair<int32, FBox>& B) {
        return A.Value.ComputeSquaredDistanceToPoint(P) < B.Value.ComputeSquaredDistanceToPoint(P);
    });
    TArray<int32> Out;
    for (const TPair<int32, FBox>& C : Left) Out.Add(C.Key);
    return Out;
}

// ------------------------------------------------------------------------------------------------------------ views

bool USCCargoSubsystem::IsBeacon(int32 LocationId) const { return State && State->Beacons.Contains(LocationId); }

FText USCCargoSubsystem::LocationLabel(int32 LocationId) const
{
    if (const FSCCargoBeacon* B = State ? State->Beacons.Find(LocationId) : nullptr) return FText::FromString(B->Name);
    if (const USCCargoGridComponent* G = FindGrid(LocationId))
        return FText::Format(NSLOCTEXT("SCCargo", "GridLocation", "{0} · {1}"), G->GroupLabel, G->GridLabel);
    return NSLOCTEXT("SCCargo", "NoLocation", "put down");
}

FVector USCCargoSubsystem::LocationCentre(int32 LocationId) const
{
    if (const USCCargoGridComponent* G = FindGrid(LocationId)) return G->WorldCenter();
    FBox Box(ForceInit);
    if (const FSCCargoBeacon* B = State ? State->Beacons.Find(LocationId) : nullptr)
        for (const int32 Id : B->Boxes)
            if (const FSCCargoBox* Member = State->Boxes.Find(Id); Member && Member->Actor.IsValid()) Box += Member->Actor->Bounds();
    return Box.IsValid ? Box.GetCenter() : FVector::ZeroVector;
}

bool USCCargoSubsystem::GetBeacon(int32 BeaconId, FSCCargoBeaconView& Out) const
{
    const FSCCargoBeacon* B = State ? State->Beacons.Find(BeaconId) : nullptr;
    if (!B) return false;
    Out = FSCCargoBeaconView();
    Out.BeaconId = B->Id;
    Out.Label = FText::FromString(B->Name);
    Out.OriginId = B->OriginId;
    Out.OriginLabel = B->OriginLabel;
    Out.OwnerLabel = OwnerLabel(B->Owner.IsNone() ? State->Player : B->Owner);
    Out.bMine = B->Owner.IsNone() || B->Owner == State->Player;
    Out.bAcceptsDrops = false; // a source only, for now (WP-K2 "Architecture")
    for (const int32 Id : B->Boxes)
        if (const FSCCargoBox* Member = State->Boxes.Find(Id))
        {
            FSCCargoBoxView V = OwnedBoxView(*Member);
            Out.WorldBounds += V.WorldBounds;
            if (Member->State == ESCCargoBoxState::Loose) Out.TotalSCU += V.SCU; // one a job holds is on its way
            Out.Boxes.Add(MoveTemp(V));
        }
    Out.WorldCenter = Out.WorldBounds.IsValid ? Out.WorldBounds.GetCenter() : FVector::ZeroVector;
    return true;
}

TArray<FSCCargoBeaconView> USCCargoSubsystem::GetBeacons(const FVector& Viewer, float RadiusM) const
{
    TArray<FSCCargoBeaconView> Out;
    if (!State) return Out;
    for (const auto& Pair : State->Beacons)
    {
        FSCCargoBeaconView V;
        if (!GetBeacon(Pair.Key, V)) continue;
        V.DistanceM = V.WorldBounds.IsValid ? float(FMath::Sqrt(V.WorldBounds.ComputeSquaredDistanceToPoint(Viewer)) / 100.0) : 0.f;
        if (RadiusM <= 0 || V.DistanceM <= RadiusM) Out.Add(MoveTemp(V));
    }
    Out.Sort([](const FSCCargoBeaconView& A, const FSCCargoBeaconView& B) { return A.DistanceM < B.DistanceM; });
    return Out;
}

// ------------------------------------------------------------------------------------------------------------ loose boxes

void USCCargoSubsystem::LetDown(FSCCargoBox& Box, int32 OriginId)
{
    LeaveBeacon(Box);
    Box.State = ESCCargoBoxState::Loose;
    Box.GridId = INDEX_NONE;
    Box.JobId = INDEX_NONE;
    Box.OriginId = OriginId;
    Box.StillSince = -1;
    if (ASCCargoContainer* A = Box.Actor.Get()) A->Release();
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_LOOSE box=%d let_down=1 origin=%d at=%s"), Box.Id, OriginId,
           Box.Actor.IsValid() ? *Box.Actor->GetActorLocation().ToCompactString() : TEXT("-"));
    ++Revision;
}

void USCCargoSubsystem::LeaveBeacon(FSCCargoBox& Box)
{
    if (Box.BeaconId == INDEX_NONE) return;
    if (FSCCargoBeacon* B = State->Beacons.Find(Box.BeaconId))
    {
        B->Boxes.Remove(Box.Id);
        if (B->Boxes.IsEmpty())
        {
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_BEACON id=%d name=\"%s\" gone=1"), B->Id, *B->Name);
            State->Beacons.Remove(Box.BeaconId);
        }
    }
    Box.BeaconId = INDEX_NONE;
    ++Revision;
}

void USCCargoSubsystem::JoinBeacon(FSCCargoBox& Box)
{
    const ASCCargoContainer* A = Box.Actor.Get();
    if (!A) return;
    const FName Owner = Box.Owner.IsNone() ? State->Player : Box.Owner;
    TArray<FSCCargoBeaconModel> Models;
    TMap<int32, FBox> Bounds;
    TSet<FString> Names;
    for (const auto& [Id, B] : State->Beacons)
    {
        Models.Add({Id, B.OriginId, B.Owner, B.Boxes});
        Names.Add(B.Name);
        for (const int32 M : B.Boxes)
            if (const FSCCargoBox* Member = State->Boxes.Find(M); Member && Member->Actor.IsValid()) Bounds.Add(M, Member->Actor->Bounds());
    }
    int32 Id = FSCCargoBeacons::Join(Models, Bounds, A->Bounds(), Box.OriginId, Owner);
    if (Id == INDEX_NONE)
    {
        FSCCargoBeacon& B = State->Beacons.Add(State->NextGridId);
        B.Id = Id = State->NextGridId++; // a location id: never a grid's
        B.Name = FSCCargoBeacons::Name([](int32 Max) { return FMath::RandHelper(Max); }, Names);
        B.OriginId = Box.OriginId;
        B.OriginLabel = Box.OriginId == INDEX_NONE ? NSLOCTEXT("SCCargo", "PutDown", "put down here")
                                                   : FText::Format(NSLOCTEXT("SCCargo", "FromOrigin", "from {0}"), LocationLabel(Box.OriginId));
        B.Owner = Owner;
        B.Created = GetWorld()->GetTimeSeconds();
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_BEACON id=%d name=\"%s\" founded=1 origin=%d owner=%s at=%s"), B.Id, *B.Name, B.OriginId, *Owner.ToString(),
               *A->GetActorLocation().ToCompactString());
    }
    State->Beacons[Id].Boxes.AddUnique(Box.Id);
    Box.BeaconId = Id;
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_BEACON id=%d name=\"%s\" box=%d members=%d"), Id, *State->Beacons[Id].Name, Box.Id, State->Beacons[Id].Boxes.Num());
    ++Revision;
}

void USCCargoSubsystem::WakeLoose(const FBox* Region)
{
    for (auto& Pair : State->Boxes)
    {
        FSCCargoBox& B = Pair.Value;
        ASCCargoContainer* A = B.Actor.Get();
        if (B.State != ESCCargoBoxState::Loose || !A || A->Body->IsSimulatingPhysics()) continue;
        if (Region && !Region->ExpandBy(A->SizeCm().GetMax()).Intersect(A->Bounds())) continue;
        A->Release();
        B.StillSince = -1;
    }
}

void USCCargoSubsystem::TickLoose(double Now)
{
    constexpr double StillCmS = 1, StillDegS = 1, RestSeconds = 0.5; // at rest: under these for this long (as "settled")
    const double KillZ = GetWorld()->GetWorldSettings() ? GetWorld()->GetWorldSettings()->KillZ : -HALF_WORLD_MAX;
    const bool bZeroG = Gravity() <= 0;
    TArray<int32> Lost;
    for (auto& Pair : State->Boxes)
    {
        FSCCargoBox& B = Pair.Value;
        if (B.State != ESCCargoBoxState::Loose) continue;
        ASCCargoContainer* A = B.Actor.Get();
        if (!A) { Lost.Add(B.Id); continue; }
        if (!A->Body->IsSimulatingPhysics())
        {
            if (B.BeaconId == INDEX_NONE) JoinBeacon(B); // put down frozen
            continue;
        }
        if (A->GetActorLocation().Z < KillZ) { Lost.Add(B.Id); continue; }
        // Zero g (WP-K3): nothing would ever stop a box let go with the load's speed, or nudged; loose cargo holds station
        // where it was let go (viewer choice: no drifting cargo, so every loose box comes to rest in a beacon).
        if (bZeroG)
        {
            A->Body->SetPhysicsLinearVelocity(FVector::ZeroVector);
            A->Body->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
        }
        const bool bStill = A->Body->GetPhysicsLinearVelocity().Size() < StillCmS && A->Body->GetPhysicsAngularVelocityInDegrees().Size() < StillDegS;
        if (!bStill) { B.StillSince = -1; continue; }
        if (B.StillSince < 0) { B.StillSince = Now; continue; }
        if (Now - B.StillSince < RestSeconds) continue;
        // At rest: frozen where it lies (no drift, no physics cost), in a beacon. A box woken in a beacon (its support went)
        // stays in it while still near another member; one that ended up elsewhere leaves it and joins another.
        A->Lock(nullptr, A->GetActorTransform());
        if (B.BeaconId != INDEX_NONE)
        {
            const FSCCargoBeacon& Beacon = State->Beacons[B.BeaconId];
            bool bNear = Beacon.Boxes.Num() == 1;
            for (const int32 M : Beacon.Boxes)
                if (const FSCCargoBox* O = State->Boxes.Find(M); M != B.Id && O && O->Actor.IsValid())
                    bNear |= FMath::Sqrt(A->Bounds().ComputeSquaredDistanceToBox(O->Actor->Bounds())) <= FSCCargoBeacons::NearCm(A->Bounds(), O->Actor->Bounds());
            if (bNear) continue;
            LeaveBeacon(B);
        }
        JoinBeacon(B);
    }
    for (const int32 Id : Lost)
    {
        UE_LOG(LogSCCargo, Warning, TEXT("CARGO_LOOSE box=%d lost=1 (fell out of the world or its actor is gone)"), Id);
        ForgetBox(Id);
    }
}

bool USCCargoSubsystem::FindLooseSpot(const FVector& SizeCm, FRandomStream& Rng, FTransform& Out) const
{
    // On the floor the ships stand on (the loaded vehicle's lowest point), inside the environment, or in space (WP-K3)
    // anywhere around the ship within its own size of it, with nothing in the way
    // and room above for the lift and the drones (as a pick needs: LiftPlan's reach), off every grid.
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld());
    const ASCVehicle* Environment = Viewer ? Viewer->GetEnvironment() : nullptr;
    const ASCVehicle* Ship = Viewer ? Viewer->GetVehicle() : nullptr;
    const bool bSpace = Viewer && Viewer->IsSpace();
    if ((!Environment && !bSpace) || !Ship) return false;
    // Within reach of the ship (the hangar's solid bounds take in the structure around the room), inside the environment.
    const FBox ShipBox = FSCEnvironment::SolidBounds(Ship);
    const FBox Room = bSpace ? ShipBox.ExpandBy(ShipBox.GetSize().GetMax())
                             : FSCEnvironment::SolidBounds(Environment).Overlap(ShipBox.ExpandBy(FVector(ShipBox.GetSize().GetMax(), ShipBox.GetSize().GetMax(), 0)));
    const double FloorZ = Ship->LowestVisibleZ();
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    // The least a pick needs above the box (as LiftPlan accepts): half the normal clearance to lift, the beam's shortest hang, the drone.
    const double Reach = Def.NormalClearanceM * 50.0 + Def.MinDistanceM * 100.0 + DroneHeightCm() + 30;
    int32 NoFloor = 0, Blocked = 0, NoRoom = 0, OnGrid = 0, Enclosed = 0;
    // The room's open air: over a drone dock (its drones fly from there), else over the ship.
    FVector Air(ShipBox.GetCenter().X, ShipBox.GetCenter().Y, ShipBox.Max.Z + 200);
    for (const FSCCargoDock& D : State->Docks)
        if (const ASCCargoDroneDock* Dock = D.Actor.Get()) { Air = Dock->GetActorLocation() + FVector(0, 0, 300); break; }
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCCargoLooseSpot), true);
    for (const TWeakObjectPtr<ASCCargoDrone>& D : State->Drones) Params.AddIgnoredActor(D.Get());
    const FVector Half = SizeCm / 2;
    const double Pad = Half.Size2D();
    constexpr int32 Tries = 400;
    for (int32 Try = 0; Try < Tries; ++Try)
    {
        const FVector XY(Rng.FRandRange(Room.Min.X + Pad, Room.Max.X - Pad), Rng.FRandRange(Room.Min.Y + Pad, Room.Max.Y - Pad), Rng.FRandRange(Room.Min.Z, Room.Max.Z));
        FHitResult Floor;
        // What a box collides with (blocks physics bodies): not the volumes that only overlap.
        if (!bSpace && (!GetWorld()->LineTraceSingleByChannel(Floor, FVector(XY.X, XY.Y, FloorZ + 300), FVector(XY.X, XY.Y, FloorZ - 100), ECC_PhysicsBody, Params) ||
            Floor.ImpactNormal.Z < 0.95))
        { ++NoFloor; continue; }
        const FQuat Q(FVector::UpVector, Rng.FRandRange(0, UE_TWO_PI));
        const FVector At = bSpace ? XY : Floor.ImpactPoint + FVector(0, 0, Half.Z + 2); // in space: where it floats
        if (GetWorld()->OverlapBlockingTestByChannel(At, Q, ECC_PhysicsBody, FCollisionShape::MakeBox(Half), Params)) { ++Blocked; continue; }
        if (GetWorld()->SweepTestByChannel(At, At + FVector(0, 0, Half.Z + Reach), Q, ECC_PhysicsBody, FCollisionShape::MakeBox(Half - FVector(6)), Params)) { ++NoRoom; continue; }
        // In the room the drones fly in: a clear line from the air above the ship to the top of the spot's lift corridor.
        if (GetWorld()->LineTraceTestByChannel(Air, At + FVector(0, 0, Half.Z + Reach), ECC_PhysicsBody, Params)) { ++Enclosed; continue; }
        bool bOnGrid = false;
        for (const auto& Pair : State->Grids)
            if (const USCCargoGridComponent* G = Pair.Value.Get(); G && (bSpace ? G->WorldBounds().ExpandBy(Pad).IsInside(At) : G->WorldBounds().ExpandBy(Pad).IsInsideXY(At))) bOnGrid = true;
        if (bOnGrid) { ++OnGrid; continue; }
        Out = FTransform(Q, At);
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_LOOSE_SPOT found=1 tries=%d at=%s"), Try + 1, *At.ToCompactString());
        return true;
    }
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_LOOSE_SPOT found=0 tries=%d no_floor=%d blocked=%d no_room=%d on_grid=%d enclosed=%d floor_z=%.0f room=%s"), Tries, NoFloor, Blocked, NoRoom, OnGrid, Enclosed, FloorZ,
           *Room.ToString());
    return false;
}

TArray<int32> USCCargoSubsystem::AddLooseBoxes(const FString& Entity, int32 Count, const FTransform* At, FRandomStream* Rng, FName Commodity, const FText& Label,
                                               FName Owner, FString& OutError)
{
    TArray<int32> Ids;
    FString Error;
    if (RequestBoxPackage(Entity, Error) != 1) { OutError = Error.IsEmpty() ? TEXT("the package is not ready") : Error; return Ids; }
    const FSCCargoBoxLibrary::FEntry* Package = State->Library.Find(Entity);
    const FVector SizeM = FSCCargoLattice::FromSC(Package->Occupancy.Dimensions);
    for (int32 I = 0; I < Count; ++I)
    {
        FTransform Pose;
        if (At)
        {
            Pose = *At;
            Pose.AddToTranslation(At->GetRotation().GetRightVector() * I * (SizeM.Y * 100.0 + 50)); // side by side
        }
        else if (!Rng || !FindLooseSpot(SizeM * 100.0, *Rng, Pose))
        {
            OutError = FString::Printf(TEXT("%d of %d put down: no free reachable spot found"), Ids.Num(), Count);
            break;
        }
        ASCCargoContainer* A = SCCargoSpawnBoxActor(GetWorld(), *Package, Entity, Pose);
        if (!A) continue;
        FSCCargoBox B;
        B.Id = State->NextItemId++;
        B.Entity = Entity;
        B.Commodity = Commodity;
        B.CommodityLabel = Label;
        B.Occupancy = Package->Occupancy;
        B.Owner = Owner.IsNone() ? State->Player : Owner;
        B.Actor = A;
        B.State = ESCCargoBoxState::Loose;
        A->BoxId = B.Id;
        A->Release(); // falls onto whatever is below, comes to rest, joins a beacon
        State->Boxes.Add(B.Id, B);
        Ids.Add(B.Id);
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_BOX id=%d entity=%s scu=%d loose=1 world=%s"), B.Id, *Entity, B.Occupancy.StandardCargoUnits, *Pose.GetLocation().ToString());
        ++Revision;
    }
    return Ids;
}

// ------------------------------------------------------------------------------------------------------------ retarget

bool USCCargoSubsystem::RetargetLoad(FSCCargoJob& Job, FSCCargoMove& M, FSCCargoBox* Box, ASCCargoContainer* A, const FString& Why, double Now)
{
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    M.TargetsTried.Add(M.Plan.ToGrid);
    const FName Who = RequesterOf(Job.Request);
    FSCCargoItem Item;
    Item.Id = Box->Id;
    Item.SizeM = Box->SizeM();
    Item.SCU = Box->Occupancy.StandardCargoUnits;
    Item.SubType = Box->Occupancy.ItemSubType;
    // Open grids the requester may use that take the box at all, nearest first.
    TArray<TPair<int32, FBox>> Candidates;
    for (const auto& Pair : State->Grids)
    {
        const USCCargoGridComponent* G = Pair.Value.Get();
        bool bRotate = false;
        if (!G || G->Access != ESCCargoGridAccess::Open || (!G->Owner.IsNone() && G->Owner != Who) || !FSCCargoPlanner::FitsMaxItem(G->Lattice, Item.SizeM, bRotate) ||
            G->Lattice.ExcludedSubTypes.ContainsByPredicate([&](const FString& S) { return S.Equals(Item.SubType, ESearchCase::IgnoreCase); }))
            continue;
        Candidates.Add({Pair.Key, G->WorldBounds()});
    }
    if (USCCargoGridComponent* Old = FindGrid(M.Plan.ToGrid))
    {
        Old->Placed.RemoveAll([&](const FSCCargoPlacement& P) { return P.ItemId == Box->Id; });
        Old->Reserved.Remove(Box->Id);
    }
    for (const int32 Id : FSCCargoBeacons::NearestFirst(Candidates, A->GetActorLocation(), M.TargetsTried))
    {
        USCCargoGridComponent* G = FindGrid(Id);
        FSCCargoPlacement Slot;
        if (!FSCCargoPlanner::FindSlot(G->Model(), Item, Slot)) { M.TargetsTried.Add(Id); continue; }
        Slot.ItemId = Box->Id;
        const FTransform SlotWorld = G->SlotToWorld(Slot, Item.SizeM);
        const FVector Normal = G->LatticeToWorld().GetRotation().GetUpVector();
        FVector Goal, Approach;
        if (!SlotApproach(M, A, SlotWorld, Normal, A->GetActorQuat(), Def.NormalClearanceM * 100.0, Goal, Approach)) { M.TargetsTried.Add(Id); continue; }
        const int32 OldTo = M.Plan.ToGrid;
        G->Placed.Add(Slot);
        G->Reserved.Add(Box->Id);
        M.Plan.ToGrid = Id;
        M.Plan.Origin = Slot.Origin;
        M.Plan.Cells = Slot.Cells;
        M.Plan.bRotated = Slot.bRotated;
        M.ToNormal = Normal;
        M.ToRotation = SlotWorld.GetRotation();
        M.Goal = Goal;
        M.Approach = Approach;
        M.SecondPath.Reset();
        // Held from its ends (WP-K4 round 4), it goes down onto the new slot so: its approach was found so.
        M.OverLeg.Reset();
        M.bEndsToSlot = M.Grip == FSCCargoMove::EGrip::Ends;
        M.TurnPoints.Empty();
        M.EndsFinishes.Empty();
        M.Entrances.Empty();
        M.Replans = 0; // a fresh budget towards the new place
        M.bStalled = false;
        M.NextRetry = Now;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_RETARGET job=%d move=%d box=%d from_to=%d to=%d origin=%s reason=\"%s\" tried=%d"), Job.Id, M.MoveId, Box->Id, OldTo, Id,
               *Slot.Origin.ToString(), *Why, M.TargetsTried.Num());
        if (!ReplanLoad(Job, M, Box, A, G, A->GetActorLocation(), TEXT("retarget"), Now) && !M.bRoutePending) M.bStalled = true;
        ++Revision;
        return true;
    }
    return false;
}

bool USCCargoSubsystem::InterruptLoad(int32 JobId)
{
    for (FSCCargoJob& J : State->Jobs)
        if (J.IsActive() && (JobId == INDEX_NONE || J.Id == JobId))
            for (FSCCargoMove& M : J.Moves)
                if (M.Step == FSCCargoMove::EStep::Transit)
                {
                    M.bInterrupt = true;
                    UE_LOG(LogSCCargo, Display, TEXT("CARGO_INTERRUPT job=%d move=%d box=%d"), J.Id, M.MoveId, M.Plan.BoxId);
                    return true;
                }
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_INTERRUPT none in flight%s"), JobId == INDEX_NONE ? TEXT("") : *FString::Printf(TEXT(" for job %d"), JobId));
    return false;
}
