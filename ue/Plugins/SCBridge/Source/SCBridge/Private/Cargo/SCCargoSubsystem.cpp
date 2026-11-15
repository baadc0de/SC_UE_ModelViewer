// USCCargoSubsystem core: lifetime, tick order, the UI's read-only views, and preview / submit / cancel of transfers.
// Discovery, scenario, jobs and console live in their own files (SCCargoDiscovery, SCCargoScenarioApply, SCCargoJobs,
// SCCargoCommands).
#include "Cargo/SCCargoSubsystem.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoGridComponent.h"
#include "SCViewerSettings.h"
#include "SCViewerSubsystem.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"

DEFINE_LOG_CATEGORY(LogSCCargo);

ASCCargoDrone* FindDrone(const FSCCargoState& State, int32 DroneId)
{
    for (const TWeakObjectPtr<ASCCargoDrone>& D : State.Drones)
        if (D.IsValid() && D->DroneId == DroneId) return D.Get();
    return nullptr;
}

USCCargoSubsystem* USCCargoSubsystem::Get(const UObject* WorldContext)
{
    const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
    return World ? World->GetSubsystem<USCCargoSubsystem>() : nullptr;
}

void USCCargoSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    State = MakeShared<FSCCargoState>();
}

void USCCargoSubsystem::Deinitialize()
{
    if (State) State->Library.Reset();
    State.Reset();
    Super::Deinitialize();
}

bool USCCargoSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId USCCargoSubsystem::GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(USCCargoSubsystem, STATGROUP_Tickables); }

void USCCargoSubsystem::Tick(float DeltaSeconds)
{
    if (!State || !GetWorld()) return;
    Discover();
    if (USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld())) State->Library.Tick(Viewer, GetWorld());
    TryScenario();
    const double Now = GetWorld()->GetTimeSeconds();
    for (const auto& Pair : State->Grids)
        if (USCCargoGridComponent* Grid = Pair.Value.Get()) UpdateAccess(Grid, Now);
    TickJobs(DeltaSeconds);
    if (State->bDebug) DebugDraw();
}

float USCCargoSubsystem::GetNearbyRadiusM() const { return USCViewerSettings::Get()->CargoNearbyRadiusM; }

bool USCCargoSubsystem::InAtmosphere() const
{
    // The scenario's say (no data signal); without a scenario, an environment (a hangar) has air.
    if (State->Scenario.IsSet()) return State->Scenario.bAtmosphere;
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld());
    return Viewer && Viewer->GetEnvironment();
}

double USCCargoSubsystem::SwayEnvelopeCm() const
{
    // Peak wander of an empty drone at full speed (the speed term at most doubles it).
    const FSCCargoDroneDefinition& D = State->Scenario.Drone;
    return InAtmosphere() ? D.SwayAmplitudeCm * (1.0 + 0.05 * D.MaxSpeedMS) : 0.0;
}

double USCCargoSubsystem::Gravity() const
{
    // The world's gravity (m/s^2): the same acceleration the carried boxes fall with.
    return GetWorld() ? FMath::Abs(GetWorld()->GetGravityZ()) / 100.0 : 0.0;
}

USCCargoGridComponent* USCCargoSubsystem::FindGrid(int32 GridId) const
{
    const TWeakObjectPtr<USCCargoGridComponent>* Grid = State ? State->Grids.Find(GridId) : nullptr;
    return Grid ? Grid->Get() : nullptr;
}

void USCCargoSubsystem::SetDebugDraw(bool bOn) { if (State) State->bDebug = bOn; }
bool USCCargoSubsystem::IsDebugDraw() const { return State && State->bDebug; }

// ------------------------------------------------------------------------------------------------------------ views

namespace
{
FSCCargoBoxView BoxView(const FSCCargoBox& B)
{
    FSCCargoBoxView V;
    V.BoxId = B.Id;
    V.Commodity = B.Commodity;
    V.CommodityLabel = B.CommodityLabel;
    V.SCU = B.Occupancy.StandardCargoUnits;
    V.SizeM = B.Occupancy.Dimensions;
    V.MassKg = B.Occupancy.Mass;
    V.State = B.State;
    V.JobId = B.JobId;
    const bool bOnGrid = B.State == ESCCargoBoxState::Stored || B.State == ESCCargoBoxState::Reserved;
    V.GridId = bOnGrid ? B.GridId : INDEX_NONE;
    V.Origin = B.Slot.Origin;
    V.Cells = B.Slot.Cells;
    if (const ASCCargoContainer* A = B.Actor.Get())
    {
        V.WorldBounds = A->Bounds();
        V.WorldCenter = V.WorldBounds.GetCenter();
    }
    return V;
}
} // namespace

FSCCargoBoxView USCCargoSubsystem::OwnedBoxView(const FSCCargoBox& B) const
{
    FSCCargoBoxView V = BoxView(B);
    V.OwnerLabel = OwnerLabel(B.Owner.IsNone() ? State->Player : B.Owner);
    V.bMine = B.Owner.IsNone() || B.Owner == State->Player;
    return V;
}

bool USCCargoSubsystem::GetGrid(int32 GridId, FSCCargoGridView& Out) const
{
    const USCCargoGridComponent* G = FindGrid(GridId);
    if (!G) return false;
    Out = FSCCargoGridView();
    Out.GridId = GridId;
    Out.OwnerLabel = OwnerLabel(G->Owner);
    Out.bMine = G->Owner.IsNone() || G->Owner == State->Player;
    Out.Kind = G->Kind;
    Out.GroupKey = G->GroupKey;
    Out.GroupLabel = G->GroupLabel;
    Out.GridLabel = G->GridLabel;
    Out.Cells = G->Lattice.Cells;
    Out.CellSizeM = G->Lattice.CellM;
    Out.MaxItemSizeM = G->Lattice.MaxItemM;
    const double Interior = G->Lattice.InteriorM.X * G->Lattice.InteriorM.Y * G->Lattice.InteriorM.Z;
    Out.CapacitySCU = State->ScuVolumeM3 > 0 ? FMath::RoundToInt(Interior / State->ScuVolumeM3) : 0;
    Out.Access = G->Access;
    Out.bCanClose = CanClose(G, &Out.CloseBlockedReason);
    Out.LatticeToWorld = G->LatticeToWorld();
    Out.WorldBounds = G->WorldBounds();
    for (const auto& Pair : State->Boxes)
    {
        const FSCCargoBox& B = Pair.Value;
        if (B.GridId == GridId && (B.State == ESCCargoBoxState::Stored || B.State == ESCCargoBoxState::Reserved))
        {
            Out.Boxes.Add(OwnedBoxView(B));
            Out.UsedSCU += B.Occupancy.StandardCargoUnits;
        }
        else if (G->Reserved.Contains(B.Id))
            Out.ReservedSCU += B.Occupancy.StandardCargoUnits;
    }
    // Cells held by what is not a box: drone docks, and a hold's own structure (cells meeting the ship's geometry).
    for (const FSCCargoPlacement& P : G->Placed)
    {
        const bool bDock = State->Docks.ContainsByPredicate([&](const FSCCargoDock& D) { return D.GridId == GridId && D.ItemId == P.ItemId; });
        if (!bDock && !SCCargoIsStructure(P.ItemId)) continue;
        for (int32 X = 0; X < P.Cells.X; ++X)
            for (int32 Y = 0; Y < P.Cells.Y; ++Y)
                for (int32 Z = 0; Z < P.Cells.Z; ++Z) Out.BlockedCells.Add(P.Origin + FIntVector(X, Y, Z));
    }
    return true;
}

TArray<FSCCargoGridView> USCCargoSubsystem::GetGrids(const FVector& Viewer, float RadiusM) const
{
    TArray<FSCCargoGridView> Out;
    if (!State) return Out;
    for (const auto& Pair : State->Grids)
    {
        FSCCargoGridView V;
        if (!GetGrid(Pair.Key, V)) continue;
        V.DistanceM = float(FMath::Sqrt(V.WorldBounds.ComputeSquaredDistanceToPoint(Viewer)) / 100.0);
        if (RadiusM <= 0 || V.DistanceM <= RadiusM) Out.Add(MoveTemp(V));
    }
    Out.Sort([](const FSCCargoGridView& A, const FSCCargoGridView& B) { return A.DistanceM < B.DistanceM; });
    return Out;
}

TArray<FSCCargoCommodityView> USCCargoSubsystem::GetCommodities(const TArray<int32>& Grids) const
{
    TArray<FSCCargoCommodityView> Out;
    if (!State) return Out;
    for (const auto& Pair : State->Boxes)
    {
        const FSCCargoBox& B = Pair.Value;
        if (B.State != ESCCargoBoxState::Stored || (!Grids.IsEmpty() && !Grids.Contains(B.GridId))) continue;
        FSCCargoCommodityView* C = Out.FindByPredicate([&](const FSCCargoCommodityView& V) { return V.Commodity == B.Commodity; });
        if (!C)
        {
            C = &Out.AddDefaulted_GetRef();
            C->Commodity = B.Commodity;
            C->Label = B.CommodityLabel;
        }
        C->TotalSCU += B.Occupancy.StandardCargoUnits;
        C->BoxSizesSCU.AddUnique(B.Occupancy.StandardCargoUnits);
    }
    for (FSCCargoCommodityView& C : Out) C.BoxSizesSCU.Sort();
    Out.Sort([](const FSCCargoCommodityView& A, const FSCCargoCommodityView& B) { return A.Label.CompareTo(B.Label) < 0; });
    return Out;
}

TArray<FSCCargoBoxView> USCCargoSubsystem::GetMovingBoxes() const
{
    TArray<FSCCargoBoxView> Out;
    if (!State) return Out;
    for (const auto& Pair : State->Boxes)
        if (Pair.Value.State == ESCCargoBoxState::Lifting || Pair.Value.State == ESCCargoBoxState::InTransit || Pair.Value.State == ESCCargoBoxState::Placing ||
            Pair.Value.State == ESCCargoBoxState::Held)
            Out.Add(OwnedBoxView(Pair.Value));
    return Out;
}

TArray<FSCCargoDroneView> USCCargoSubsystem::GetDrones() const
{
    TArray<FSCCargoDroneView> Out;
    if (!State) return Out;
    for (const TWeakObjectPtr<ASCCargoDrone>& Weak : State->Drones)
        if (const ASCCargoDrone* D = Weak.Get())
        {
            FSCCargoDroneView V;
            V.DroneId = D->DroneId;
            V.State = D->State;
            V.Label = FText::Format(NSLOCTEXT("SCCargo", "Drone", "Drone {0}"), D->DroneId);
            V.JobId = D->JobId;
            V.CarryingBoxId = D->CarryingBoxId;
            V.WorldLocation = D->GetActorLocation();
            V.WorldBounds = D->GetComponentsBoundingBox(false);
            V.OwnerLabel = OwnerLabel(D->Owner);
            Out.Add(MoveTemp(V));
        }
    return Out;
}

TArray<FSCCargoJobView> USCCargoSubsystem::GetJobs() const
{
    TArray<FSCCargoJobView> Out;
    if (!State) return Out;
    const UEnum* States = StaticEnum<ESCCargoJobState>();
    for (const FSCCargoJob& J : State->Jobs)
    {
        FSCCargoJobView V;
        V.JobId = J.Id;
        V.State = J.State;
        V.Summary = J.Summary;
        V.StateText = States->GetDisplayNameTextByValue(int64(J.State));
        V.FailureReason = J.State == ESCCargoJobState::Queued ? J.QueueReason : J.Failure;
        V.OwnerLabel = OwnerLabel(RequesterOf(J.Request));
        V.bMine = RequesterOf(J.Request) == State->Player;
        V.FromGrids = J.Request.FromGrids;
        V.ToGrids = J.Request.ToGrids;
        V.Moves = J.Moves.Num();
        double Remaining = 0, Speed = FMath::Max(1.0, State->Scenario.Drone.MaxSpeedMS * 100.0);
        float Partial = 0;
        for (const FSCCargoMove& M : J.Moves)
        {
            const bool bCarry = M.Plan.Kind == ESCCargoMoveKind::Carry;
            if (bCarry) V.TotalSCU += M.Plan.SCU;
            const bool bPlaced = M.Step == FSCCargoMove::EStep::Returning || M.Step == FSCCargoMove::EStep::Done;
            const FSCCargoMove::EStep Step = M.Step == FSCCargoMove::EStep::Holding ? FSCCargoMove::EStep::Transit : M.Step;
            if (bPlaced) { ++V.MovesDone; if (bCarry) V.DoneSCU += M.Plan.SCU; }
            else if (M.Step != FSCCargoMove::EStep::Waiting && M.Step != FSCCargoMove::EStep::Failed) Partial += float(Step) / float(FSCCargoMove::EStep::Returning);
            if (!bPlaced && M.Step != FSCCargoMove::EStep::Failed) Remaining += M.DistanceCm / Speed;
            for (const int32 D : M.Drones) if (M.Step != FSCCargoMove::EStep::Done) V.Drones.AddUnique(D);
        }
        V.Progress = J.State == ESCCargoJobState::Done ? 1.f : V.Moves ? (V.MovesDone + Partial) / float(V.Moves) : 0.f;
        V.EtaSeconds = J.IsActive() ? float(Remaining) : 0.f;
        Out.Add(MoveTemp(V));
    }
    Out.Sort([&](const FSCCargoJobView& A, const FSCCargoJobView& B) {
        const bool AA = A.State != ESCCargoJobState::Done && A.State != ESCCargoJobState::Cancelled && A.State != ESCCargoJobState::Failed;
        const bool BA = B.State != ESCCargoJobState::Done && B.State != ESCCargoJobState::Cancelled && B.State != ESCCargoJobState::Failed;
        return AA != BA ? AA : A.JobId > B.JobId;
    });
    constexpr int32 FinishedShown = 8;
    int32 Finished = 0;
    Out.RemoveAll([&](const FSCCargoJobView& V) {
        const bool bFinished = V.State == ESCCargoJobState::Done || V.State == ESCCargoJobState::Cancelled || V.State == ESCCargoJobState::Failed;
        return bFinished && ++Finished > FinishedShown;
    });
    return Out;
}

// ------------------------------------------------------------------------------------------------------------ plans

FSCCargoPlanInput USCCargoSubsystem::PlanInput(const FSCCargoTransferRequest& Request, bool bIgnoreOtherJobs) const
{
    // Ownership: the requester moves only their boxes, between grids they may use (their ships, the shared hangar), with
    // their ships' drones and the hangar's shared pool. bIgnoreOtherJobs: as if no other transfer held boxes or cells
    // (whether a request could run once they are done: it queues instead of failing).
    const FName Who = RequesterOf(Request);
    auto Usable = [&](int32 GridId) { const USCCargoGridComponent* G = FindGrid(GridId); return G && (G->Owner.IsNone() || G->Owner == Who); };
    FSCCargoPlanInput In;
    for (const auto& Pair : State->Grids)
        if (const USCCargoGridComponent* G = Pair.Value.Get())
        {
            FSCCargoGridModel Model = G->Model();
            Model.PreferredLongAxis = EntranceAxis(const_cast<USCCargoGridComponent*>(G));
            if (bIgnoreOtherJobs) Model.Placed.RemoveAll([&](const FSCCargoPlacement& P) { return G->Reserved.Contains(P.ItemId); });
            In.Grids.Add(MoveTemp(Model));
        }
    for (const auto& Pair : State->Boxes)
    {
        const FSCCargoBox& B = Pair.Value;
        FSCCargoItem I;
        I.Id = B.Id;
        I.SizeM = B.SizeM();
        I.SCU = B.Occupancy.StandardCargoUnits;
        I.MassKg = B.Occupancy.Mass;
        I.SubType = B.Occupancy.ItemSubType;
        I.Commodity = B.Commodity;
        I.Volume = B.VolumeSCU();
        In.Items.Add(B.Id, I);
        const bool bTheirs = !B.Owner.IsNone() && B.Owner != Who;
        if (bTheirs || (B.State != ESCCargoBoxState::Stored && !(bIgnoreOtherJobs && B.State == ESCCargoBoxState::Reserved))) In.Busy.Add(B.Id);
        if (B.Commodity == Request.Commodity) In.CommodityLabel = B.CommodityLabel.ToString();
    }
    for (const FSCCargoDock& D : State->Docks)
    {
        FSCCargoItem I;
        I.Id = D.ItemId;
        I.bMovable = false;
        In.Items.Add(D.ItemId, I);
    }
    In.FromGrids = Request.FromGrids.FilterByPredicate(Usable);
    In.ToGrids = Request.ToGrids.FilterByPredicate(Usable);
    In.Commodity = Request.Commodity;
    In.AmountSCU = Request.AmountSCU;
    In.LiftForceN = State->Scenario.Drone.MaxForceN;
    In.DroneMassKg = State->Scenario.Drone.DroneMassKg;
    In.MinLoadAccelMS2 = State->Scenario.Drone.MinLoadAccelMS2;
    In.GravityMS2 = Gravity();
    In.BeamMaxVolume = State->Scenario.Drone.MaxVolume;
    In.DronesAvailable = 0;
    for (const TWeakObjectPtr<ASCCargoDrone>& D : State->Drones)
        if (D.IsValid() && (D->Owner.IsNone() || D->Owner == Who)) ++In.DronesAvailable;
    return In;
}

FSCCargoTransferPreview USCCargoSubsystem::Preview(const FSCCargoTransferRequest& Request) const
{
    if (!State) return FSCCargoTransferPreview();
    FSCCargoTransferPreview P = FSCCargoPlanner::Plan(PlanInput(Request));
    OwnerRefusals(Request, P);
    // ETA: each move flies out, carries and returns at cruise speed (grid to grid, plus the climbs).
    const double Speed = FMath::Max(1.0, State->Scenario.Drone.MaxSpeedMS * 100.0);
    for (const FSCCargoMovePlan& M : P.Moves)
    {
        const USCCargoGridComponent *From = FindGrid(M.FromGrid), *To = FindGrid(M.ToGrid);
        if (!From || !To) continue;
        const double Legs = FVector::Dist(From->WorldCenter(), To->WorldCenter()) * 2 + 4 * State->Scenario.Drone.ClearanceMarginM * 100.0;
        P.EstimatedSeconds += float(Legs / Speed);
    }
    return P;
}

int32 USCCargoSubsystem::Submit(const FSCCargoTransferRequest& Request, FText& OutError)
{
    if (!State) return INDEX_NONE;
    const FSCCargoTransferPreview P = Preview(Request);
    if (!P.bValid)
    {
        // Only other transfers in the way (their boxes or cells reserved, the shared lift busy): it waits its turn.
        if (FSCCargoPlanner::Plan(PlanInput(Request, true)).bValid)
        {
            FSCCargoJob& Q = State->Jobs.AddDefaulted_GetRef();
            Q.Id = State->NextJobId++;
            Q.Request = Request;
            Q.Preview = P;
            Q.State = ESCCargoJobState::Queued;
            Q.Created = Q.StateSince = GetWorld()->GetTimeSeconds();
            Q.NextQueueTry = Q.Created + 2;
            Q.QueueReason = NSLOCTEXT("SCCargo", "QueuedReason", "Waiting for the cargo lift: boxes or cells are held by another transfer");
            Q.Summary = FText::Format(NSLOCTEXT("SCCargo", "QueuedSummary", "{0} SCU · queued"), Request.AmountSCU);
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_JOB id=%d state=Queued owner=%s reason=\"%s\""), Q.Id, *RequesterOf(Request).ToString(), *Q.QueueReason.ToString());
            ++Revision;
            return Q.Id;
        }
        OutError = SCCargoMainReason(P);
        return INDEX_NONE;
    }
    FSCCargoJob& J = State->Jobs.AddDefaulted_GetRef();
    J.Id = State->NextJobId++;
    J.Request = Request;
    J.Created = J.StateSince = GetWorld()->GetTimeSeconds();
    Materialize(J, P);
    return J.Id;
}

void USCCargoSubsystem::Materialize(FSCCargoJob& J, const FSCCargoTransferPreview& P)
{
    const FSCCargoTransferRequest& Request = J.Request;
    J.Preview = P;
    FText CommodityLabel = FText::FromName(Request.Commodity), FromLabel, ToLabel;
    for (const FSCCargoMovePlan& M : P.Moves)
    {
        FSCCargoMove& Move = J.Moves.AddDefaulted_GetRef();
        Move.Plan = M;
        Move.MoveId = State->NextMoveId++;
        FSCCargoBox& B = State->Boxes[M.BoxId];
        B.State = ESCCargoBoxState::Reserved;
        B.JobId = J.Id;
        if (M.Kind == ESCCargoMoveKind::Carry) CommodityLabel = B.CommodityLabel;
        // A hold's cell is taken back when it returns (the dug-out box frees it); carries and relocations reserve theirs.
        if (USCCargoGridComponent* To = M.Kind == ESCCargoMoveKind::Hold ? nullptr : FindGrid(M.ToGrid))
        {
            FSCCargoPlacement Slot;
            Slot.ItemId = M.BoxId;
            Slot.Origin = M.Origin;
            Slot.Cells = M.Cells;
            Slot.bRotated = M.bRotated;
            To->Placed.Add(Slot);
            To->Reserved.Add(M.BoxId);
            if (M.Kind == ESCCargoMoveKind::Carry) ToLabel = To->GroupLabel;
        }
        if (const USCCargoGridComponent* From = FindGrid(M.FromGrid); From && M.Kind == ESCCargoMoveKind::Carry) FromLabel = From->GroupLabel;
        Move.DistanceCm = FindGrid(M.FromGrid) && FindGrid(M.ToGrid) ? FVector::Dist(FindGrid(M.FromGrid)->WorldCenter(), FindGrid(M.ToGrid)->WorldCenter()) * 2 : 0;
    }
    J.Summary = FText::Format(NSLOCTEXT("SCCargo", "JobSummary", "{0} · {1} SCU · {2} → {3}"), CommodityLabel, P.PlannedSCU, FromLabel, ToLabel);
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_JOB id=%d state=Planned owner=%s moves=%d scu=%d requested=%d refusals=%d summary=\"%s\""), J.Id,
           *RequesterOf(Request).ToString(), J.Moves.Num(), P.PlannedSCU, P.RequestedSCU, P.Refusals.Num(), *J.Summary.ToString());
    for (const FSCCargoRefusal& R : P.Refusals)
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_REFUSAL job=%d box=%d scu=%d reason=\"%s\""), J.Id, R.BoxId, R.SCU, *R.Reason.ToString());
    ++Revision;
}

void USCCargoSubsystem::OwnerRefusals(const FSCCargoTransferRequest& Request, FSCCargoTransferPreview& P) const
{
    const FName Who = RequesterOf(Request);
    TSet<int32> Grids;
    for (const int32 Id : Request.FromGrids) Grids.Add(Id);
    for (const int32 Id : Request.ToGrids) Grids.Add(Id);
    for (const int32 Id : Grids)
        if (const USCCargoGridComponent* G = FindGrid(Id); G && !G->Owner.IsNone() && G->Owner != Who)
        {
            FSCCargoRefusal& R = P.Refusals.AddDefaulted_GetRef();
            R.Reason = FText::Format(NSLOCTEXT("SCCargo", "GridOfOther", "{0} of {1} belongs to {2}"), G->GridLabel, G->GroupLabel, OwnerLabel(G->Owner));
        }
    TMap<FString, int32> Theirs; // usable source group -> boxes of the commodity there that belong to others
    for (const auto& Pair : State->Boxes)
    {
        const FSCCargoBox& B = Pair.Value;
        const USCCargoGridComponent* G = FindGrid(B.GridId);
        if (!G || !Request.FromGrids.Contains(B.GridId) || B.Commodity != Request.Commodity || B.Owner.IsNone() || B.Owner == Who) continue;
        if (!G->Owner.IsNone() && G->Owner != Who) continue; // the grid itself was refused
        ++Theirs.FindOrAdd(G->GroupLabel.ToString());
    }
    for (const auto& [Group, N] : Theirs)
    {
        FSCCargoRefusal& R = P.Refusals.AddDefaulted_GetRef();
        R.Reason = FText::FromString(FString::Printf(TEXT("%d box%s on the %s belong%s to another owner"), N, N == 1 ? TEXT("") : TEXT("es"), *Group, N == 1 ? TEXT("s") : TEXT("")));
    }
}

bool USCCargoSubsystem::Cancel(int32 JobId)
{
    FSCCargoJob* J = State ? State->Jobs.FindByPredicate([&](const FSCCargoJob& Job) { return Job.Id == JobId; }) : nullptr;
    if (!J || !J->IsActive()) return false;
    // Moves not started are dropped (their reservations released); boxes already in the air finish their move.
    for (FSCCargoMove& M : J->Moves)
        if (M.Step == FSCCargoMove::EStep::Waiting)
        {
            M.Failure = TEXT("cancelled");
            ReleaseMove(M);
            FinishMove(*J, M, false);
        }
    J->bCancelRequested = true;
    if (J->State == ESCCargoJobState::Queued) SetJobState(*J, ESCCargoJobState::Cancelled, NSLOCTEXT("SCCargo", "CancelledReason", "Cancelled"));
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_JOB id=%d cancel=1 state=%s"), J->Id, *StaticEnum<ESCCargoJobState>()->GetNameStringByValue(int64(J->State)));
    if (J->State == ESCCargoJobState::Planned || J->State == ESCCargoJobState::OpeningGrids) SetJobState(*J, ESCCargoJobState::ClosingGrids);
    ++Revision;
    return true;
}
