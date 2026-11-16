// USCCargoSubsystem for the RPC harness and state log (WP-H1): activity (what the cargo is still doing), a JSON state
// of grids, boxes, drones, docks and jobs, and scene edits: boxes and docks added to grids, boxes removed.
#include "Cargo/SCCargoSubsystem.h"
#include "SCCargoState.h"
#include "SCRpcJson.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoGridComponent.h"
#include "SCViewerSubsystem.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"

using namespace SCRpcJson;

namespace
{
template <typename E> FString EnumName(E Value) { return StaticEnum<E>()->GetNameStringByValue(int64(Value)); }
const TCHAR* PackageState(FSCCargoBoxLibrary::EState S)
{
    switch (S)
    {
    case FSCCargoBoxLibrary::EState::Ready: return TEXT("ready");
    case FSCCargoBoxLibrary::EState::Failed: return TEXT("failed");
    default: return TEXT("loading");
    }
}
} // namespace

TArray<FString> USCCargoSubsystem::ActivityReasons() const
{
    TArray<FString> Out;
    if (!State) return Out;
    if (!State->Library.IsSettled()) Out.Add(TEXT("cargo_packages"));
    if (State->Scenario.IsSet() && (!State->bScenarioPlaced || (!State->Scenario.Loose.IsEmpty() && !State->bLoosePlaced))) Out.Add(TEXT("cargo_scenario"));
    if (State->Jobs.ContainsByPredicate([](const FSCCargoJob& J) { return J.IsActive(); })) Out.Add(TEXT("cargo_jobs"));
    for (const auto& Pair : State->Grids)
        if (const USCCargoGridComponent* G = Pair.Value.Get();
            G && (G->Access == ESCCargoGridAccess::Opening || G->Access == ESCCargoGridAccess::Closing))
        {
            Out.Add(TEXT("cargo_grid_access"));
            break;
        }
    for (const TWeakObjectPtr<ASCCargoDrone>& D : State->Drones)
        if (D.IsValid() && D->State != ESCCargoDroneState::Docked)
        {
            Out.Add(TEXT("cargo_drones"));
            break;
        }
    bool bMoving = false, bSettling = false;
    for (const auto& Pair : State->Boxes)
    {
        const FSCCargoBox& B = Pair.Value;
        if (B.State == ESCCargoBoxState::Loose)
            bSettling |= B.BeaconId == INDEX_NONE || (B.Actor.IsValid() && B.Actor->Body->IsSimulatingPhysics()); // falling, coming to rest
        else if (B.State != ESCCargoBoxState::Stored && B.State != ESCCargoBoxState::Reserved)
            bMoving = true;
    }
    if (bMoving) Out.Add(TEXT("cargo_boxes_moving"));
    if (bSettling) Out.Add(TEXT("cargo_boxes_settling"));
    return Out;
}

double USCCargoSubsystem::MaxSpeedCmS() const
{
    double Max = 0;
    if (!State) return Max;
    for (const auto& Pair : State->Boxes)
        if (const ASCCargoContainer* A = Pair.Value.Actor.Get(); A && A->Body && A->Body->IsSimulatingPhysics())
            Max = FMath::Max(Max, A->Body->GetPhysicsLinearVelocity().Size());
    for (const TWeakObjectPtr<ASCCargoDrone>& D : State->Drones)
        if (D.IsValid() && D->State != ESCCargoDroneState::Docked) Max = FMath::Max(Max, D->GetVelocity().Size());
    return Max;
}

TSharedRef<FJsonObject> USCCargoSubsystem::StateJson() const
{
    FObj Root = MakeShared<FJsonObject>();
    if (!State) return Root;
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld());
    Root->SetNumberField(TEXT("revision"), Revision);

    TArray<TSharedPtr<FJsonValue>> Grids;
    TArray<int32> GridIds;
    State->Grids.GetKeys(GridIds);
    GridIds.Sort();
    for (const int32 Id : GridIds)
    {
        const USCCargoGridComponent* G = FindGrid(Id);
        FSCCargoGridView V;
        if (!G || !GetGrid(Id, V)) continue;
        FObj O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("id"), Id);
        O->SetStringField(TEXT("kind"), G->Kind == ESCCargoGridKind::Vehicle ? TEXT("vehicle") : TEXT("environment"));
        O->SetNumberField(TEXT("vehicle"), G->Kind == ESCCargoGridKind::Vehicle && Viewer ? Viewer->GetVehicleInstanceId(G->OwnerVehicle.Get()) : -1);
        O->SetStringField(TEXT("owner"), G->Owner.IsNone() ? FString() : G->Owner.ToString());
        O->SetStringField(TEXT("group"), V.GroupLabel.ToString());
        O->SetStringField(TEXT("label"), V.GridLabel.ToString());
        O->SetStringField(TEXT("access"), EnumName(V.Access));
        O->SetBoolField(TEXT("movable"), !G->Movers.IsEmpty()); // extends / retracts (cargo/access)
        O->SetNumberField(TEXT("used_scu"), V.UsedSCU);
        O->SetNumberField(TEXT("reserved_scu"), V.ReservedSCU);
        O->SetNumberField(TEXT("capacity_scu"), V.CapacitySCU);
        O->SetNumberField(TEXT("boxes"), V.Boxes.Num());
        O->SetField(TEXT("pos"), Metres(V.WorldBounds.GetCenter()));
        Grids.Add(MakeShared<FJsonValueObject>(O));
    }
    Root->SetArrayField(TEXT("grids"), Grids);

    TArray<TSharedPtr<FJsonValue>> Boxes;
    TArray<int32> BoxIds;
    State->Boxes.GetKeys(BoxIds);
    BoxIds.Sort();
    for (const int32 Id : BoxIds)
    {
        const FSCCargoBox& B = State->Boxes[Id];
        FObj O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("id"), Id);
        O->SetStringField(TEXT("entity"), B.Entity);
        O->SetNumberField(TEXT("scu"), B.Occupancy.StandardCargoUnits);
        O->SetStringField(TEXT("commodity"), B.Commodity.ToString());
        O->SetStringField(TEXT("owner"), B.Owner.IsNone() ? FString() : B.Owner.ToString());
        O->SetStringField(TEXT("state"), EnumName(B.State));
        O->SetNumberField(TEXT("grid"), B.GridId);
        O->SetNumberField(TEXT("beacon"), B.BeaconId); // loose cargo (WP-K2)
        if (B.OriginId != INDEX_NONE) O->SetNumberField(TEXT("origin"), B.OriginId);
        O->SetNumberField(TEXT("job"), B.JobId);
        const ASCCargoContainer* A = B.Actor.Get();
        O->SetBoolField(TEXT("actor"), A != nullptr);
        if (A)
        {
            O->SetField(TEXT("pos"), Metres(A->GetActorLocation()));
            O->SetField(TEXT("rot"), Rotation(A->GetActorRotation()));
            const bool bSim = A->Body && A->Body->IsSimulatingPhysics();
            O->SetBoolField(TEXT("sim"), bSim);
            if (bSim)
            {
                O->SetField(TEXT("vel"), Metres(A->Body->GetPhysicsLinearVelocity()));
                O->SetField(TEXT("ang_vel"), Vector(A->Body->GetPhysicsAngularVelocityInDegrees(), 0.1));
            }
            // A stored box sits on its slot: how far it is from it (it rides its grid when the ship moves).
            if (const USCCargoGridComponent* G = FindGrid(B.GridId); G && B.State == ESCCargoBoxState::Stored)
                O->SetField(TEXT("slot_err_cm"), Number(FVector::Dist(G->SlotToWorld(B.Slot, B.SizeM()).GetLocation(), A->GetActorLocation()), 0.1));
        }
        Boxes.Add(MakeShared<FJsonValueObject>(O));
    }
    Root->SetArrayField(TEXT("boxes"), Boxes);

    TArray<TSharedPtr<FJsonValue>> Drones;
    for (const TWeakObjectPtr<ASCCargoDrone>& Weak : State->Drones)
    {
        const ASCCargoDrone* D = Weak.Get();
        if (!D) continue;
        FObj O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("id"), D->DroneId);
        O->SetStringField(TEXT("state"), EnumName(D->State));
        O->SetStringField(TEXT("owner"), D->Owner.IsNone() ? FString() : D->Owner.ToString());
        O->SetNumberField(TEXT("dock"), D->DockItem);
        O->SetNumberField(TEXT("job"), D->JobId);
        O->SetNumberField(TEXT("move"), D->MoveId);
        O->SetNumberField(TEXT("carrying"), D->CarryingBoxId);
        O->SetField(TEXT("pos"), Metres(D->GetActorLocation()));
        O->SetField(TEXT("vel"), Metres(D->GetVelocity()));
        if (D->CarryingBoxId != INDEX_NONE)
        {
            O->SetField(TEXT("beam_force_n"), Vector(D->BeamForceN, 1));
            O->SetField(TEXT("beam_rest_m"), Number(D->BeamRestM));
            O->SetField(TEXT("beam_len_m"), Number(D->BeamLengthM));
        }
        O->SetBoolField(TEXT("blocked"), D->bBlocked);
        // Out of its bay, a drone belongs to a move that has not ended (flying for it, or home for it).
        bool bInMove = false;
        for (const FSCCargoJob& J : State->Jobs)
            for (const FSCCargoMove& M : J.Moves)
                bInMove |= M.Step != FSCCargoMove::EStep::Done && M.Step != FSCCargoMove::EStep::Failed && M.Drones.Contains(D->DroneId);
        O->SetBoolField(TEXT("in_move"), bInMove);
        Drones.Add(MakeShared<FJsonValueObject>(O));
    }
    Root->SetArrayField(TEXT("drones"), Drones);

    TArray<TSharedPtr<FJsonValue>> Docks;
    for (const FSCCargoDock& D : State->Docks)
    {
        FObj O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("item"), D.ItemId);
        O->SetNumberField(TEXT("grid"), D.GridId);
        O->SetBoolField(TEXT("actor"), D.Actor.IsValid());
        if (D.Actor.IsValid()) O->SetField(TEXT("pos"), Metres(D.Actor->GetActorLocation()));
        Docks.Add(MakeShared<FJsonValueObject>(O));
    }
    Root->SetArrayField(TEXT("docks"), Docks);

    TArray<TSharedPtr<FJsonValue>> Beacons;
    for (const FSCCargoBeaconView& V : GetBeacons(FVector::ZeroVector, 0))
    {
        FObj O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("id"), V.BeaconId);
        O->SetStringField(TEXT("name"), V.Label.ToString());
        O->SetNumberField(TEXT("origin"), V.OriginId);
        O->SetStringField(TEXT("origin_label"), V.OriginLabel.ToString());
        O->SetStringField(TEXT("owner"), State->Beacons[V.BeaconId].Owner.ToString());
        O->SetNumberField(TEXT("scu"), V.TotalSCU);
        TArray<int32> Members;
        for (const FSCCargoBoxView& B : V.Boxes) Members.Add(B.BoxId);
        O->SetField(TEXT("boxes"), Ints(Members));
        O->SetField(TEXT("pos"), Metres(V.WorldCenter));
        O->SetBoolField(TEXT("accepts_drops"), V.bAcceptsDrops);
        Beacons.Add(MakeShared<FJsonValueObject>(O));
    }
    Root->SetArrayField(TEXT("beacons"), Beacons);

    TArray<TSharedPtr<FJsonValue>> Jobs;
    for (const FSCCargoJob& J : State->Jobs)
    {
        FObj O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("id"), J.Id);
        O->SetStringField(TEXT("state"), EnumName(J.State));
        O->SetStringField(TEXT("owner"), RequesterOf(J.Request).ToString());
        int32 Done = 0;
        for (const FSCCargoMove& M : J.Moves) Done += M.Step == FSCCargoMove::EStep::Done;
        O->SetNumberField(TEXT("moves"), J.Moves.Num());
        O->SetNumberField(TEXT("moves_done"), Done);
        O->SetStringField(TEXT("summary"), J.Summary.ToString());
        const FText& Why = J.State == ESCCargoJobState::Queued ? J.QueueReason : J.Failure;
        if (!Why.IsEmpty()) O->SetStringField(TEXT("reason"), Why.ToString());
        O->SetField(TEXT("age_s"), Number(GetWorld()->GetTimeSeconds() - J.Created, 0.1));
        Jobs.Add(MakeShared<FJsonValueObject>(O));
    }
    Root->SetArrayField(TEXT("jobs"), Jobs);
    return Root;
}

TSharedRef<FJsonObject> USCCargoSubsystem::CatalogJson() const
{
    FObj Root = MakeShared<FJsonObject>();
    if (!State) return Root;
    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FString& Entity : State->Library.Entities())
    {
        const FSCCargoBoxLibrary::FEntry* E = State->Library.Find(Entity);
        FObj O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("entity"), Entity);
        O->SetStringField(TEXT("state"), PackageState(E->State));
        if (!E->Error.IsEmpty()) O->SetStringField(TEXT("error"), E->Error);
        O->SetNumberField(TEXT("scu"), E->Occupancy.StandardCargoUnits);
        O->SetNumberField(TEXT("mass_kg"), E->Occupancy.Mass);
        O->SetField(TEXT("size_m"), Vector(FSCCargoLattice::FromSC(E->Occupancy.Dimensions), 0.001));
        Out.Add(MakeShared<FJsonValueObject>(O));
    }
    Root->SetArrayField(TEXT("packages"), Out);
    TArray<TSharedPtr<FJsonValue>> Commodities;
    for (const FSCCargoCommodityView& C : GetCommodities({}))
    {
        FObj O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("key"), C.Commodity.ToString());
        O->SetStringField(TEXT("label"), C.Label.ToString());
        O->SetNumberField(TEXT("total_scu"), C.TotalSCU);
        Commodities.Add(MakeShared<FJsonValueObject>(O));
    }
    Root->SetArrayField(TEXT("commodities"), Commodities);
    return Root;
}

int32 USCCargoSubsystem::RequestBoxPackage(const FString& Entity, FString& OutError)
{
    if (!State || Entity.IsEmpty()) { OutError = TEXT("no entity"); return -1; }
    if (!State->Library.Find(Entity)) State->Library.Request(Entity);
    const FSCCargoBoxLibrary::FEntry* E = State->Library.Find(Entity);
    if (!E) { OutError = TEXT("the package could not be requested"); return -1; }
    if (E->State == FSCCargoBoxLibrary::EState::Failed) { OutError = E->Error; return -1; }
    return E->State == FSCCargoBoxLibrary::EState::Ready ? 1 : 0;
}

TArray<int32> USCCargoSubsystem::AddBoxes(int32 GridId, const FString& Entity, int32 Count, FName Commodity, const FText& Label, FName Owner, FString& OutError)
{
    TArray<int32> Ids;
    USCCargoGridComponent* Grid = FindGrid(GridId);
    if (!Grid) { OutError = FString::Printf(TEXT("no grid %d"), GridId); return Ids; }
    if (Grid->Access != ESCCargoGridAccess::Open) { OutError = FString::Printf(TEXT("grid %d is not extended (%s)"), GridId, *EnumName(Grid->Access)); return Ids; }
    FString Error;
    if (RequestBoxPackage(Entity, Error) != 1) { OutError = Error.IsEmpty() ? TEXT("the package is not ready") : Error; return Ids; }
    for (int32 I = 0; I < Count; ++I)
    {
        const int32 Id = SpawnBox(Grid, Entity, Commodity, Label);
        if (Id == INDEX_NONE)
        {
            OutError = FString::Printf(TEXT("%d of %d placed: no room left on grid %d"), Ids.Num(), Count, GridId);
            break;
        }
        State->Boxes[Id].Owner = !Owner.IsNone() ? Owner : !Grid->Owner.IsNone() ? Grid->Owner : State->Player;
        Ids.Add(Id);
    }
    return Ids;
}

void USCCargoSubsystem::ForgetBox(int32 BoxId)
{
    FSCCargoBox* B = State->Boxes.Find(BoxId);
    if (!B) return;
    LeaveBeacon(*B);
    if (ASCCargoContainer* A = B->Actor.Get()) A->Destroy();
    for (const auto& Pair : State->Grids)
        if (USCCargoGridComponent* G = Pair.Value.Get())
        {
            G->Placed.RemoveAll([&](const FSCCargoPlacement& P) { return P.ItemId == BoxId; });
            G->Reserved.Remove(BoxId);
        }
    State->Boxes.Remove(BoxId);
    ++Revision;
}

bool USCCargoSubsystem::AbandonJob(FSCCargoJob& J, const FText& Why, double Now)
{
    using EStep = FSCCargoMove::EStep;
    J.Failure = Why;
    bool bReturning = false;
    for (FSCCargoMove& M : J.Moves)
    {
        if (M.Step == EStep::Done || M.Step == EStep::Failed) continue;
        if (M.Failure.IsEmpty()) M.Failure = Why.ToString();
        for (const int32 D : M.Drones)
            if (ASCCargoDrone* Drone = FindDrone(*State, D)) Drone->Drop();
        // A box in the air is let down loose where it is (WP-K2); one not lifted yet keeps its place.
        FSCCargoBox* Box = State->Boxes.Find(M.Plan.BoxId);
        const bool bLifted = Box && Box->GridId == INDEX_NONE && Box->BeaconId == INDEX_NONE && Box->State != ESCCargoBoxState::Loose && Box->State != ESCCargoBoxState::Reserved;
        ReleaseMove(M);
        if (bLifted) LetDown(*Box, M.Plan.FromGrid);
        if (M.Step == EStep::Waiting) { FinishMove(J, M, false); continue; }
        if (M.Step != EStep::Returning) SendHome(M, Now);
        bReturning = true;
    }
    SetJobState(J, bReturning ? ESCCargoJobState::Moving : ESCCargoJobState::ClosingGrids);
    return bReturning;
}

bool USCCargoSubsystem::RemoveBox(int32 BoxId, bool bForce, TArray<int32>& OutRemoved, FString& OutError)
{
    const FSCCargoBox* B = State ? State->Boxes.Find(BoxId) : nullptr;
    if (!B) { OutError = FString::Printf(TEXT("no box %d"), BoxId); return false; }
    FSCCargoJob* Job = B->JobId == INDEX_NONE ? nullptr : State->Jobs.FindByPredicate([&](const FSCCargoJob& J) { return J.Id == B->JobId && J.IsActive(); });
    if (Job || (B->State != ESCCargoBoxState::Stored && B->State != ESCCargoBoxState::Loose)) // resting on a grid or loose: free to go
    {
        if (!bForce)
        {
            OutError = FString::Printf(TEXT("box %d is %s in job %d (force removes it and abandons the job)"), BoxId, *EnumName(B->State), B->JobId);
            return false;
        }
        if (Job)
        {
            const int32 JobId = Job->Id;
            const bool bReturning = AbandonJob(*Job, NSLOCTEXT("SCCargo", "BoxRemoved", "A box of the transfer was removed"), GetWorld()->GetTimeSeconds());
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_JOB id=%d box_removed=%d drones_returning=%d"), JobId, BoxId, bReturning);
            // Its other boxes in the air are let down loose (AbandonJob).
        }
    }
    ForgetBox(BoxId);
    OutRemoved.Add(BoxId);
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_BOX_REMOVED ids=%s force=%d"), *FString::JoinBy(OutRemoved, TEXT(","), [](int32 I) { return FString::FromInt(I); }), bForce);
    return true;
}

bool USCCargoSubsystem::AddDock(int32 GridId, int32 Drones, FString& OutError)
{
    USCCargoGridComponent* Grid = FindGrid(GridId);
    if (!Grid) { OutError = FString::Printf(TEXT("no grid %d"), GridId); return false; }
    if (Grid->Access != ESCCargoGridAccess::Open) { OutError = FString::Printf(TEXT("grid %d is not extended"), GridId); return false; }
    const int32 Before = State->Docks.Num();
    SpawnDock(Grid, FMath::Clamp(Drones, 0, 16));
    if (State->Docks.Num() == Before) { OutError = FString::Printf(TEXT("no room for a dock on grid %d"), GridId); return false; }
    return true;
}

bool USCCargoSubsystem::FindJobState(int32 JobId, ESCCargoJobState& OutState, FText& OutReason) const
{
    const FSCCargoJob* J = State ? State->Jobs.FindByPredicate([&](const FSCCargoJob& X) { return X.Id == JobId; }) : nullptr;
    if (!J) return false;
    OutState = J->State;
    OutReason = J->State == ESCCargoJobState::Queued ? J->QueueReason : J->Failure;
    return true;
}
