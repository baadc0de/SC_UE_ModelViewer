// USCCargoSubsystem transfer jobs: Planned -> OpeningGrids -> Moving -> ClosingGrids -> Done (Cancelled / Failed) and the
// swarm: every job's waiting moves (carry, relocate, hold) claim the nearest free drones when their box can be picked
// (uncovered, lift corridor clear) and their dig-out group can be staffed at once; a box that cannot be picked is swapped
// for another eligible one, and after CargoPickAttempts consecutive failures the job fails. Log: CARGO_JOB / CARGO_PICK /
// CARGO_SLOW (a move's tick over 30 ms: its grip search and its tug apart).
#include "Cargo/SCCargoSubsystem.h"
#include "HAL/PlatformTime.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoGridComponent.h"
#include "Components/BoxComponent.h"
#include "SCVehicle.h"
#include "SCViewerSettings.h"
#include "SCViewerSubsystem.h"
#include "Engine/World.h"

namespace
{
using EStep = FSCCargoMove::EStep;
bool Started(EStep S) { return S != EStep::Waiting; }
bool Ended(EStep S) { return S == EStep::Done || S == EStep::Failed; }
} // namespace

FBox USCCargoSubsystem::EnvironmentBounds() const
{
    if (State->BoundsFrame == GFrameCounter) return State->Bounds;
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld());
    const ASCVehicle* Environment = Viewer ? Viewer->GetEnvironment() : nullptr;
    State->Bounds = Environment ? Environment->GetComponentsBoundingBox(true) : FBox(ForceInit);
    State->BoundsFrame = GFrameCounter;
    return State->Bounds;
}

double USCCargoSubsystem::CeilingZ() const
{
    const FBox Bounds = EnvironmentBounds();
    return Bounds.IsValid ? Bounds.Max.Z : 1e9;
}

void USCCargoSubsystem::SetJobState(FSCCargoJob& Job, ESCCargoJobState NewState, const FText& Reason)
{
    if (Job.State == NewState) return;
    Job.State = NewState;
    Job.StateSince = GetWorld()->GetTimeSeconds();
    if (!Reason.IsEmpty()) Job.Failure = Reason;
    if (!Job.IsActive()) Job.Finished = Job.StateSince;
    int32 Done = 0;
    for (const FSCCargoMove& M : Job.Moves) Done += M.Step == EStep::Done;
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_JOB id=%d state=%s t=%.1f moves=%d done=%d overlaps=%d%s"), Job.Id, *StaticEnum<ESCCargoJobState>()->GetNameStringByValue(int64(NewState)),
           Job.StateSince - Job.Created, Job.Moves.Num(), Done, ASCCargoDrone::Overlaps, Reason.IsEmpty() ? TEXT("") : *(TEXT(" reason=\"") + Reason.ToString() + TEXT("\"")));
    ++Revision;
}

void USCCargoSubsystem::FinishMove(FSCCargoJob& Job, FSCCargoMove& Move, bool bOK)
{
    Move.Step = bOK ? EStep::Done : EStep::Failed;
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_MOVE job=%d move=%d box=%d kind=%s step=%s%s"), Job.Id, Move.MoveId, Move.Plan.BoxId,
           *StaticEnum<ESCCargoMoveKind>()->GetNameStringByValue(int64(Move.Plan.Kind)), bOK ? TEXT("Done") : TEXT("Failed"),
           Move.Failure.IsEmpty() ? TEXT("") : *(TEXT(" reason=\"") + Move.Failure + TEXT("\"")));
    ++Revision;
}

void USCCargoSubsystem::ReleaseMove(FSCCargoMove& M)
{
    if (M.bPlanning)
    {
        // Still planning (drones claimed, nothing flown): the drones are free again.
        for (const int32 Id : M.Drones)
            if (ASCCargoDrone* D = FindDrone(*State, Id))
            {
                D->MoveId = INDEX_NONE;
                if (D->State == ESCCargoDroneState::Docked) D->JobId = INDEX_NONE;
            }
        M.Drones.Reset();
        M.bPlanning = false;
        M.Searches.Empty();
        M.EntranceOut = 0;
        M.bReeledIn = M.bRefine = false;
        M.Tier = M.TierStep = 0; // planned afresh: every way of holding it
    }
    if (USCCargoGridComponent* To = FindGrid(M.Plan.ToGrid); To && To->Reserved.Contains(M.Plan.BoxId))
    {
        To->Placed.RemoveAll([&](const FSCCargoPlacement& P) { return P.ItemId == M.Plan.BoxId && P.Origin == M.Plan.Origin; });
        To->Reserved.Remove(M.Plan.BoxId);
    }
    if (FSCCargoBox* B = State->Boxes.Find(M.Plan.BoxId); B && (B->GridId != INDEX_NONE || B->BeaconId != INDEX_NONE))
    {
        // Not lifted yet: on its grid, or loose in its beacon (WP-K2).
        B->State = B->GridId != INDEX_NONE ? ESCCargoBoxState::Stored : ESCCargoBoxState::Loose;
        B->JobId = INDEX_NONE;
    }
}

bool USCCargoSubsystem::CanPick(int32 BoxId, FString& Why) const
{
    const FSCCargoBox* Box = State->Boxes.Find(BoxId);
    const USCCargoGridComponent* Grid = Box ? FindGrid(Box->GridId) : nullptr;
    const ASCCargoContainer* A = Box ? Box->Actor.Get() : nullptr;
    const bool bLoose = Box && !Grid && Box->BeaconId != INDEX_NONE; // loose cargo (WP-K2): lifted straight up from where it lies
    if ((!Grid && !bLoose) || !A) { Why = TEXT("the box is neither on a grid nor in loose cargo"); return false; }
    // Its lift corridor along the grid normal, with room for the beam and the drones above it.
    double Lift = 0, Hang = 0;
    FString Blocker;
    if (LiftPlan(A, A->GetActorTransform(), Grid ? Grid->LatticeToWorld().GetRotation().GetUpVector() : FVector::UpVector, Lift, Hang, &Blocker))
        return true;
    Why = FString::Printf(TEXT("%d SCU box on %s: its lift along the grid normal is blocked by %s"), Box->Occupancy.StandardCargoUnits,
                          *(Grid ? Grid->GridLabel : LocationLabel(Box->BeaconId)).ToString(), *Blocker);
    return false;
}

bool USCCargoSubsystem::LiftPlan(const ASCCargoContainer* A, const FTransform& Pose, const FVector& Normal, double& OutLift, double& OutHang, FString* OutWhy) const
{
    const FSCCargoDroneDefinition& Def = State->Scenario.Drone;
    constexpr double MarginCm = 30; // kept between the drones and a ceiling (viewer choice)
    const double Height = A->SizeCm().Z;
    // The drones over their emitters (their parts do not collide: their actor bounds, colliding parts only, are empty).
    const double DroneHeight = DroneHeightCm();
    const double Wanted = Height + Def.NormalClearanceM * 100.0;
    const double MinHang = MinHangCm(A), MaxHang = FMath::Max(MinHang, Def.FullStrengthDistanceM * 100.0);
    const double Reach = Wanted + MaxHang + DroneHeight + MarginCm;
    // Headroom: the box (slightly shrunk: it may touch its neighbours and the floor) swept along the normal.
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCCargoLift), false, A);
    for (const TWeakObjectPtr<ASCCargoDrone>& D : State->Drones) Params.AddIgnoredActor(D.Get());
    // Loads in the air pass (their right of way and the lift's route keep them apart): a box turning over its
    // neighbour on the elevator is no ceiling (seen as one, the neighbour's pick failed for 0.3 m of headroom).
    for (const auto& Pair : State->Boxes)
        if (SCCargoIsFlying(Pair.Value.State) && Pair.Value.Actor.IsValid()) Params.AddIgnoredActor(Pair.Value.Actor.Get());
    FHitResult Hit;
    const FVector From = Pose.GetLocation() + Normal * 8;
    const FVector LiftHalf = A->SizeCm() / 2 - FVector(6);
    const TSharedRef<FSCCargoHullRule> Hulls = HullRule(Params, LiftHalf, Pose.GetRotation());
    const bool bHit = GetWorld()->SweepSingleByChannel(Hit, From, From + Normal * Reach, Pose.GetRotation(), ECC_WorldDynamic,
                                                       FCollisionShape::MakeBox(LiftHalf), Hulls->Along(From, From + Normal * Reach));
    const double Room = bHit ? Hit.Distance : Reach; // how far the box itself can travel
    OutLift = FMath::Min(Wanted, Room - MinHang - DroneHeight - MarginCm);
    OutHang = FMath::Clamp(Room - OutLift - DroneHeight - MarginCm, MinHang, MaxHang);
    if (OutLift >= Def.NormalClearanceM * 100.0 * 0.5 && OutLift > 0) return true;
    if (OutWhy)
        *OutWhy = FString::Printf(TEXT("%s (%.1f m of headroom)"), bHit && Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("geometry"), Room / 100.0);
    return false;
}

bool USCCargoSubsystem::StartMove(FSCCargoJob& Job, FSCCargoMove& M, double Now)
{
    FSCCargoBox* Box = State->Boxes.Find(M.Plan.BoxId);
    USCCargoGridComponent* From = Box ? FindGrid(Box->GridId) : nullptr;
    const bool bLoose = Box && !From && Box->BeaconId != INDEX_NONE; // loose cargo (WP-K2): nothing covers it
    if (!Box || (!From && !bLoose) || !Box->Actor.IsValid())
    {
        M.Failure = TEXT("the box is gone");
        ReleaseMove(M);
        FinishMove(Job, M, false);
        return false;
    }
    // Covered: wait while what rests on it is a task's (it comes off first); otherwise the box cannot be picked. Only
    // what is physically on the grid counts (not the cells reserved for boxes on their way).
    TArray<int32> Covering;
    if (From)
    {
        FSCCargoGridModel Physical = From->Model();
        Physical.Placed.RemoveAll([&](const FSCCargoPlacement& P) {
            const FSCCargoBox* B = State->Boxes.Find(P.ItemId);
            return B && (B->GridId != From->GridId || B->Slot.Origin != P.Origin);
        });
        Covering = Physical.ItemsOn(Box->Id);
    }
    FString Why;
    bool bLift = true; // a failure below is the lift's (covered, no headroom) unless the routes failed
    bool bPick = true;
    for (const int32 Id : Covering)
    {
        const bool bTask = State->Jobs.ContainsByPredicate([&](const FSCCargoJob& J) {
            return J.IsActive() && J.Moves.ContainsByPredicate([&](const FSCCargoMove& O) { return O.Plan.BoxId == Id && !Ended(O.Step); });
        });
        if (bTask) return false;
        bPick = false;
        Why = FString::Printf(TEXT("%d SCU box on %s is covered by another box"), Box->Occupancy.StandardCargoUnits, *From->GridLabel.ToString());
    }
    // Dug out: the boxes lifted off it are still over it until they hover at their holding point (held) or are placed
    // elsewhere (relocated).
    for (const FSCCargoMove& O : Job.Moves)
        if (&O != &M && O.Plan.ForBoxId == Box->Id && O.Plan.Kind != ESCCargoMoveKind::Carry && Started(O.Step) && !Ended(O.Step) &&
            O.Step != EStep::Holding && O.Step != EStep::Returning)
            return false;
    // Drones: free ones (docked, or flying home unclaimed); a dig-out group starts only when it can be staffed at once.
    // Planning (route searches spread over frames): the drones claimed when it began.
    TArray<ASCCargoDrone*> Free;
    if (M.bPlanning)
    {
        for (const int32 Id : M.Drones)
            if (ASCCargoDrone* D = FindDrone(*State, Id)) Free.Add(D);
    }
    else
    {
        // Only the requester's ships' drones and the hangar's shared pool serve this job.
        const FName Who = RequesterOf(Job.Request);
        for (const TWeakObjectPtr<ASCCargoDrone>& W : State->Drones)
            // ... that can fly: a docked drone whose way out of its bay is not free stays where it is.
            if (ASCCargoDrone* D = W.Get(); D && D->MoveId == INDEX_NONE && (D->Owner.IsNone() || D->Owner == Who) && (D->State != ESCCargoDroneState::Docked || CanLeaveBay(D))) Free.Add(D);
        int32 Need = M.Plan.DronesNeeded;
        if (M.Plan.Kind == ESCCargoMoveKind::Hold)
            for (const FSCCargoMove& O : Job.Moves)
                if (&O != &M && O.Plan.ForBoxId == M.Plan.ForBoxId && !Started(O.Step) && O.Plan.Kind != ESCCargoMoveKind::Relocate) Need += O.Plan.DronesNeeded;
        if (Free.Num() < Need) return false;
        if (bPick) bPick = CanPick(Box->Id, Why);
    }
    if (bPick && !M.bPlanning)
    {
        // The nearest free drones, then every route planned and swept before anything flies.
        const FVector At = Box->Actor->GetActorLocation();
        Free.Sort([&](const ASCCargoDrone& A, const ASCCargoDrone& B) { return FVector::DistSquared(A.GetActorLocation(), At) < FVector::DistSquared(B.GetActorLocation(), At); });
        Free.SetNum(M.Plan.DronesNeeded);
        M.Drones.Reset();
        for (ASCCargoDrone* D : Free)
        {
            D->MoveId = M.MoveId;
            D->JobId = Job.Id;
            M.Drones.Add(D->DroneId);
        }
    }
    if (bPick)
    {
        if (!PrepareMove(Job, M, Why))
        {
            if (M.bRoutePending)
            {
                if (!M.bPlanning)
                    UE_LOG(LogSCCargo, Display, TEXT("CARGO_MOVE job=%d move=%d box=%d step=Planning drones=%d"), Job.Id, M.MoveId, Box->Id, M.Drones.Num());
                M.bPlanning = true;
                return false;
            }
            M.bPlanning = false;
            M.Searches.Empty();
            M.EntranceOut = 0;
            M.bReeledIn = M.bRefine = false;
            M.Tier = M.TierStep = 0; // planned afresh: every way of holding it
            bPick = false;
            bLift = false; // it could be lifted; there is no way to its place
            Job.Unroutable.Add(Box->Id);
            for (ASCCargoDrone* D : Free)
            {
                D->MoveId = INDEX_NONE;
                if (D->State == ESCCargoDroneState::Docked) D->JobId = INDEX_NONE;
            }
            M.Drones.Reset();
        }
    }
    const USCViewerSettings* Settings = USCViewerSettings::Get();
    if (!bPick)
    {
        ++Job.PickFailures;
        (bLift ? Job.bLiftFailures : Job.bRouteFailures) = true;
        Job.PickReasons.Add(Why);
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_PICK job=%d move=%d box=%d ok=0 reason=\"%s\" failures=%d"), Job.Id, M.MoveId, Box->Id, *Why, Job.PickFailures);
        if (M.Plan.Kind == ESCCargoMoveKind::Carry && Job.PickFailures < Settings->CargoPickAttempts)
        {
            // Another box of the transfer: the same commodity from the same sources that fits the destination now.
            FSCCargoPlanInput In = PlanInput(Job.Request);
            TSet<int32> Excluded;
            for (const FSCCargoMove& O : Job.Moves) Excluded.Add(O.Plan.BoxId);
            Excluded.Append(Job.Unroutable); // searched already: no way to its place as things stand
            for (FSCCargoGridModel& G : In.Grids)
                if (G.GridId == M.Plan.ToGrid) G.Placed.RemoveAll([&](const FSCCargoPlacement& P) { return P.ItemId == M.Plan.BoxId; });
            FSCCargoMovePlan Swap;
            TArray<FString> Reasons;
            if (FSCCargoPlanner::Substitute(In, M.Plan, Excluded, [&](int32 Id, FString& W) { return CanPick(Id, W); }, Swap, Reasons))
            {
                ReleaseMove(M);
                M.Plan = Swap;
                M.Plan.ForBoxId = Swap.BoxId;
                FSCCargoBox& New = State->Boxes[Swap.BoxId];
                New.State = ESCCargoBoxState::Reserved;
                New.JobId = Job.Id;
                if (USCCargoGridComponent* To = FindGrid(Swap.ToGrid))
                {
                    FSCCargoPlacement Slot;
                    Slot.ItemId = Swap.BoxId;
                    Slot.Origin = Swap.Origin;
                    Slot.Cells = Swap.Cells;
                    Slot.bRotated = Swap.bRotated;
                    To->Placed.Add(Slot);
                    To->Reserved.Add(Swap.BoxId);
                }
                UE_LOG(LogSCCargo, Display, TEXT("CARGO_PICK job=%d move=%d substitute box=%d scu=%d to=%d"), Job.Id, M.MoveId, Swap.BoxId, Swap.SCU, Swap.ToGrid);
                ++Revision;
                return false;
            }
            Job.PickReasons.Append(Reasons);
        }
        M.Failure = Why;
        ReleaseMove(M);
        FinishMove(Job, M, false);
        if (Job.PickFailures >= Settings->CargoPickAttempts)
        {
            // Nothing could be moved: the rest is dropped; boxes already moved stay where they landed. A move whose routes
            // are still being searched goes on (it may yet fly; if it fails too, this is reached again).
            bool bOpen = false;
            for (FSCCargoMove& O : Job.Moves)
                if (O.Step == EStep::Waiting && !O.bPlanning)
                {
                    O.Failure = TEXT("dropped: no box could be moved");
                    ReleaseMove(O);
                    FinishMove(Job, O, false);
                }
                else bOpen |= !Ended(O.Step);
            if (!bOpen)
            {
                TArray<FString> Last = Job.PickReasons;
                if (Last.Num() > 3) Last.RemoveAt(0, Last.Num() - 3);
                // What stopped it: boxes that cannot be drawn off their grid, or boxes with no way to their place.
                const TCHAR* What = Job.bLiftFailures && Job.bRouteFailures ? TEXT("No box can be moved: ")
                                    : Job.bRouteFailures                    ? TEXT("No box can be carried to its place: ")
                                                                            : TEXT("No box can be lifted: ");
                Job.Failure = FText::FromString(What + FString::Join(Last, TEXT("; ")));
            }
        }
        return false;
    }
    Job.PickFailures = 0;
    Job.bLiftFailures = Job.bRouteFailures = false;
    // A move that took shared drones: its owner was served now (the shared pool goes to the longest waiting owner next).
    for (const int32 Id : M.Drones)
        if (const ASCCargoDrone* D = FindDrone(*State, Id); D && D->Owner.IsNone())
        {
            State->SharedServedAt.Add(RequesterOf(Job.Request), Now);
            break;
        }
    {
        const FSCCargoPlanInput In = PlanInput(Job.Request);
        int32 ByForce = 0, ByVolume = 0;
        if (const FSCCargoItem* Item = In.Items.Find(Box->Id))
            FSCCargoPlanner::TeamSize(In, *Item, In.DronesAvailable, &ByForce, &ByVolume);
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_TEAM job=%d move=%d box=%d scu=%d volume_scu=%.1f mass=%.0f byForce=%d byVolume=%d team=%d beam_max_volume=%.1f"), Job.Id,
               M.MoveId, Box->Id, Box->Occupancy.StandardCargoUnits, Box->VolumeSCU(), Box->Occupancy.Mass, ByForce, ByVolume, M.Drones.Num(),
               State->Scenario.Drone.MaxVolume);
    }
    M.bPlanning = false;
    M.Searches.Empty();
    M.EntranceOut = 0;
    M.bReeledIn = M.bRefine = false;
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_PICK job=%d move=%d box=%d ok=1 kind=%s drones=%d"), Job.Id, M.MoveId, Box->Id,
           *StaticEnum<ESCCargoMoveKind>()->GetNameStringByValue(int64(M.Plan.Kind)), M.Drones.Num());
    (void)Now;
    return true;
}

void USCCargoSubsystem::TickJobs(float DeltaSeconds)
{
    const double Now = GetWorld()->GetTimeSeconds();
    // Grids of a job: its sources, destinations and the docks its drones fly from (drones launch only when extended).
    auto Involved = [&](const FSCCargoJob& J) {
        TSet<int32> Out;
        for (const FSCCargoMove& M : J.Moves) { Out.Add(M.Plan.FromGrid); Out.Add(M.Plan.ToGrid); }
        for (const FSCCargoDock& D : State->Docks) Out.Add(D.GridId);
        return Out;
    };
    // Fair turns at the shared resources: the jobs whose owner was served longest ago start their moves first (FIFO by
    // request time within an owner), so no owner starves while another keeps submitting.
    TArray<int32> Order;
    for (int32 I = 0; I < State->Jobs.Num(); ++I) Order.Add(I);
    Order.StableSort([&](int32 A, int32 B) {
        const FSCCargoJob &JA = State->Jobs[A], &JB = State->Jobs[B];
        const double SA = State->SharedServedAt.FindRef(RequesterOf(JA.Request), -1), SB = State->SharedServedAt.FindRef(RequesterOf(JB.Request), -1);
        return SA != SB ? SA < SB : JA.Created < JB.Created;
    });
    for (const int32 JobIndex : Order)
    {
        FSCCargoJob& J = State->Jobs[JobIndex];
        if (!J.IsActive()) continue;
        if (J.State == ESCCargoJobState::Queued)
        {
            // Planned again until the boxes and cells it needs are free (all or nothing: a queued job holds nothing, so
            // two jobs never wait on each other).
            if (J.bCancelRequested) { SetJobState(J, ESCCargoJobState::Cancelled, NSLOCTEXT("SCCargo", "CancelledReason", "Cancelled")); continue; }
            if (Now < J.NextQueueTry) continue;
            J.NextQueueTry = Now + 2;
            const FSCCargoTransferPreview P = Preview(J.Request);
            if (P.bValid)
            {
                Materialize(J, P);
                SetJobState(J, ESCCargoJobState::Planned);
            }
            else if (!FSCCargoPlanner::Plan(PlanInput(J.Request, true)).bValid)
                SetJobState(J, ESCCargoJobState::Failed, SCCargoMainReason(P));
            continue;
        }
        if (J.State == ESCCargoJobState::Planned)
        {
            for (const int32 Id : Involved(J))
                if (USCCargoGridComponent* G = FindGrid(Id); G && !G->Movers.IsEmpty() && G->Access != ESCCargoGridAccess::Open)
                {
                    RequestAccess(G, true, Now);
                    J.OpenedGrids.Add(Id);
                }
            // A ship's hold is opened for each transfer as its doors stand now (one closed since the last transfer reopens).
            for (const int32 Id : Involved(J))
                if (USCCargoGridComponent* G = FindGrid(Id); G && G->bAccessRule && G->Kind == ESCCargoGridKind::Vehicle) G->bAccessOpened = false;
            SetJobState(J, ESCCargoJobState::OpeningGrids);
        }
        if (J.State == ESCCargoJobState::OpeningGrids)
        {
            // Vehicle grids extend by their mechanism; environment grids are made reachable by the environment (doors, lights).
            bool bOpen = true;
            for (const int32 Id : Involved(J))
                if (USCCargoGridComponent* G = FindGrid(Id))
                    bOpen &= G->bAccessRule ? EnvironmentAccess(G, true) : RequestAccess(G, true, Now);
            if (bOpen) SetJobState(J, ESCCargoJobState::Moving);
        }
        if (J.State == ESCCargoJobState::Moving)
        {
            bool bAllEnded = true;
            for (FSCCargoMove& M : J.Moves)
            {
                if (M.Step == EStep::Waiting && J.bCancelRequested)
                {
                    M.Failure = TEXT("cancelled");
                    ReleaseMove(M);
                    FinishMove(J, M, false);
                }
                else
                {
                    // A tick of a move that takes long is said (CARGO_SLOW: its grip search and its tug apart).
                    const double T0 = FPlatformTime::Seconds();
                    M.GripMs = M.TugMs = 0;
                    if (M.Step == EStep::Waiting) { if (StartMove(J, M, Now)) TickMove(J, M, Now, DeltaSeconds); }
                    else if (!Ended(M.Step)) TickMove(J, M, Now, DeltaSeconds);
                    if (const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0; Ms > 30.0)
                        UE_LOG(LogSCCargo, Display, TEXT("CARGO_SLOW move=%d box=%d step=%d ms=%.1f grip_ms=%.1f tug_ms=%.1f"), M.MoveId, M.Plan.BoxId, int32(M.Step), Ms, M.GripMs, M.TugMs);
                }
                bAllEnded &= Ended(M.Step);
            }
            if (bAllEnded) SetJobState(J, ESCCargoJobState::ClosingGrids);
        }
        if (J.State == ESCCargoJobState::ClosingGrids)
        {
            // Retract what this job extended once nothing is on it (a loaded grid stays extended; it retracts after the job
            // that empties it). Another transfer using it, or cargo on a grid the same mechanism carries, keeps it out.
            bool bClosed = true;
            for (const int32 Id : J.OpenedGrids)
                if (USCCargoGridComponent* G = FindGrid(Id); G && CanClose(G, nullptr, J.Id)) bClosed &= RequestAccess(G, false, Now);
            if (!bClosed) continue;
            const FSCCargoMove* Failed = J.Moves.FindByPredicate([](const FSCCargoMove& M) { return M.Step == EStep::Failed && !M.Failure.IsEmpty(); });
            const bool bCarried = J.Moves.ContainsByPredicate([](const FSCCargoMove& M) { return M.Step == EStep::Done && M.Plan.Kind == ESCCargoMoveKind::Carry; });
            if (J.bCancelRequested) SetJobState(J, ESCCargoJobState::Cancelled, NSLOCTEXT("SCCargo", "CancelledReason", "Cancelled"));
            else if (!J.Failure.IsEmpty() || (Failed && !bCarried))
                SetJobState(J, ESCCargoJobState::Failed, J.Failure.IsEmpty() ? FText::FromString(Failed->Failure) : J.Failure);
            else SetJobState(J, ESCCargoJobState::Done, Failed ? FText::FromString(TEXT("Partly done: ") + Failed->Failure) : FText());
        }
    }
}
