// USCCargoSubsystem grid discovery: grids on the loaded vehicle and environment (one per inventory-container anchor),
// their labels, and their mechanism: the clips that move the grid's attach chain (a cargo plate's deploy, a spindle's
// extension). A grid with such clips is usable only extended (the clips played to their end), retracts back to the rest
// pose only when nothing is on any grid the same clips carry, and is extended by a job before drones use it.
#include "Cargo/SCCargoSubsystem.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoGridComponent.h"
#include "Cargo/SCCargoRecords.h"
#include "SCClipCatalog.h"
#include "SCEnvironmentAccess.h"
#include "SCShipLinkSubsystem.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "SCViewerSubsystem.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"

namespace
{
// "Hangar_FreightElevator_CargoGrid_Large" -> "Hangar Freight Elevator Cargo Grid Large"; DataCore prefixes dropped.
FString Humanize(FString Name)
{
    int32 Dot;
    if (Name.FindLastChar(TEXT('.'), Dot)) Name.RightChopInline(Dot + 1);
    FString Out;
    for (int32 I = 0; I < Name.Len(); ++I)
    {
        const TCHAR C = Name[I];
        if (C == TEXT('_')) { Out += TEXT(' '); continue; }
        if (I > 0 && FChar::IsUpper(C) && FChar::IsLower(Name[I - 1])) Out += TEXT(' ');
        Out += C;
    }
    return Out.TrimStartAndEnd();
}

} // namespace

void USCCargoSubsystem::Discover()
{
    USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld());
    ASCVehicle* Environment = Viewer ? Viewer->GetEnvironment() : nullptr;
    // Every vehicle in the hangar (the loaded one and the extra instances), keyed by instance id: grids of instances that
    // are gone or replaced are forgotten, new ones discovered.
    if (Viewer && Viewer->GetVehiclesRevision() != State->SeenVehiclesRevision)
    {
        State->SeenVehiclesRevision = Viewer->GetVehiclesRevision();
        TMap<int32, ASCVehicle*> Now;
        for (ASCVehicle* V : Viewer->GetVehicles())
            if (V) Now.Add(Viewer->GetVehicleInstanceId(V), V);
        for (auto It = State->SeenVehicles.CreateIterator(); It; ++It)
            if (Now.FindRef(It->Key) != It->Value.Get())
            {
                ForgetGrids(ESCCargoGridKind::Vehicle, It->Value.Get());
                if (!It->Value.IsValid()) ForgetGrids(ESCCargoGridKind::Vehicle, nullptr); // a destroyed actor: whatever grids are orphaned
                It.RemoveCurrent();
            }
        for (const auto& [Id, V] : Now)
            if (!State->SeenVehicles.Contains(Id))
            {
                State->SeenVehicles.Add(Id, V);
                DiscoverOn(V, ESCCargoGridKind::Vehicle);
            }
    }
    if (Environment != State->SeenEnvironment.Get() || (!Environment && State->SeenEnvironment.IsStale()))
    {
        ForgetGrids(ESCCargoGridKind::Environment);
        State->SeenEnvironment = Environment;
        if (Environment) DiscoverOn(Environment, ESCCargoGridKind::Environment);
    }
}

void USCCargoSubsystem::ForgetGrids(ESCCargoGridKind Kind, const ASCVehicle* Only)
{
    // Only: the grids of that vehicle (nullptr with Kind Vehicle and a stale entry: the orphaned ones).
    const bool bOrphans = Kind == ESCCargoGridKind::Vehicle && !Only && !State->SeenVehicles.IsEmpty();
    TSet<int32> Gone;
    for (const auto& Pair : State->Grids)
    {
        USCCargoGridComponent* G = Pair.Value.Get();
        if (!G || (Only ? G->OwnerVehicle.Get() == Only : bOrphans ? !G->OwnerVehicle.IsValid() : G->Kind == Kind)) Gone.Add(Pair.Key);
    }
    if (Gone.IsEmpty()) return;
    // Jobs touching them fail. Every move still open lets go of its box and gives back what it holds: the reservation on a
    // grid that stays, the box's state, its drones (they fly home and the job ends when they are back). Boxes on the lost
    // grids go with them, and so do the lost jobs' boxes in the air; other transfers keep theirs.
    const double Now = GetWorld()->GetTimeSeconds();
    TSet<int32> LostJobs;
    for (FSCCargoJob& J : State->Jobs)
        if (J.IsActive() && J.Moves.ContainsByPredicate([&](const FSCCargoMove& M) { return Gone.Contains(M.Plan.FromGrid) || Gone.Contains(M.Plan.ToGrid); }))
        {
            LostJobs.Add(J.Id);
            const bool bReturning = AbandonJob(J, NSLOCTEXT("SCCargo", "GridGone", "A grid of the transfer was unloaded"), Now);
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_JOB id=%d grid_gone=1 drones_returning=%d"), J.Id, bReturning);
        }
    for (auto It = State->Boxes.CreateIterator(); It; ++It)
        if (Gone.Contains(It->Value.GridId)) // the lost jobs' boxes in the air were let down loose (AbandonJob)
        {
            if (ASCCargoContainer* A = It->Value.Actor.Get()) A->Destroy();
            It.RemoveCurrent();
        }
    TSet<int32> GoneDocks;
    State->Docks.RemoveAll([&](const FSCCargoDock& D) {
        if (!Gone.Contains(D.GridId)) return false;
        GoneDocks.Add(D.ItemId);
        if (ASCCargoDroneDock* A = D.Actor.Get()) A->Destroy();
        return true;
    });
    for (TWeakObjectPtr<ASCCargoDrone>& Weak : State->Drones)
        if (ASCCargoDrone* D = Weak.Get(); D && GoneDocks.Contains(D->DockItem)) D->Destroy();
    State->Drones.RemoveAll([](const TWeakObjectPtr<ASCCargoDrone>& D) { return !D.IsValid() || D->IsActorBeingDestroyed(); });
    for (const int32 Id : Gone)
    {
        if (USCCargoGridComponent* G = FindGrid(Id))
        {
            if (G->bAccessRule && G->OwnerVehicle.IsValid())
                FSCEnvironmentAccess::Release(G->OwnerVehicle.Get(), FSCEnvironmentAccess::GridKey(G));
            G->DestroyComponent();
        }
        State->Grids.Remove(Id);
    }
    State->AppliedKinds &= ~(1u << uint8(Kind));
    if (Kind == ESCCargoGridKind::Vehicle) WakeLoose(nullptr); // what loose cargo rested on may be gone: it settles again
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRIDS_LOST kind=%s grids=%d"), Kind == ESCCargoGridKind::Vehicle ? TEXT("vehicle") : TEXT("environment"), Gone.Num());
    ++Revision;
}

void USCCargoSubsystem::DiscoverOn(ASCVehicle* Actor, ESCCargoGridKind Kind)
{
    const FSCSceneManifest& M = Actor->Manifest;
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld());
    const int32 Instance = Kind == ESCCargoGridKind::Vehicle && Viewer ? Viewer->GetVehicleInstanceId(Actor) : INDEX_NONE;
    const FString Owner = Kind == ESCCargoGridKind::Vehicle && Viewer && Instance == 0 && !Viewer->GetLoadedEntity().IsEmpty() ? Viewer->GetLoadedEntity()
                                                                                                                             : M.Root.EntityName;
    // A vehicle belongs to whom the scenario says (the player by default); the hangar's grids are shared.
    const USCShipLinkSubsystem* ShipLink = USCShipLinkSubsystem::Get(this);
    const FName GridOwner = Kind == ESCCargoGridKind::Vehicle
        ? (ShipLink ? ShipLink->GetOwner(Actor) : State->Player) : NAME_None;
    const bool bSeveral = Viewer && Viewer->GetVehicles().Num() + int32(Viewer->IsSpawningVehicles()) > 1;
    int32 Found = 0;
    for (int32 C = 0; C < M.InventoryContainers.Num(); ++C)
    {
        const FSCInventoryContainerRecord& R = M.InventoryContainers[C];
        const FString Name = R.EntityName.IsEmpty() ? R.EntityClass : R.EntityName;
        USceneComponent* Anchor = Actor->InventoryAnchors.IsValidIndex(C) ? Actor->InventoryAnchors[C].Get() : nullptr;
        FSCCargoLattice Lattice;
        FString Error;
        if (!Anchor || !FSCCargoLattice::FromContainer(R.Container, Lattice, Error))
        {
            // Containers whose inventory type authors no item sizes (closed, capacity-only containers) are not grids.
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRID skipped entity=%s port=%s type=%s reason=\"%s\""), *Name, *SCCargoRecords::PortKey(R.PortPath),
                   *R.Container.InventoryType, Anchor ? *Error : TEXT("no anchor (unresolved parent item or helper)"));
            continue;
        }
        auto* G = NewObject<USCCargoGridComponent>(Actor, NAME_None, RF_Transient);
        G->SetupAttachment(Anchor);
        Actor->AddInstanceComponent(G);
        G->RegisterComponent();
        G->GridId = State->NextGridId++;
        G->Kind = Kind;
        G->Record = R;
        G->Lattice = Lattice;
        G->OwnerVehicle = Actor;
        G->Owner = GridOwner;
        ++Found;
        if (Kind == ESCCargoGridKind::Vehicle)
        {
            // One group per ship; the grid named after the item port it hangs from.
            // One group per ship instance ("MISC Hull A · 2" when the hangar holds several).
            G->GroupKey = FName(*FString::Printf(TEXT("vehicle:%s:%d"), *Owner, FMath::Max(Instance, 0)));
            G->GroupLabel = FText::FromString(bSeveral || Instance > 0 ? FString::Printf(TEXT("%s · %d"), *Humanize(Owner), FMath::Max(Instance, 0) + 1)
                                                                        : Humanize(Owner));
            const FString Port = R.ParentPortPath.IsEmpty() ? SCCargoRecords::PortKey(R.PortPath) : R.ParentPortPath.Last();
            G->GridLabel = FText::FromString(FString::Printf(TEXT("Grid %d · %s"), Found, *Humanize(Port)));
        }
        else
        {
            // One group per container entity (an elevator platform).
            G->GroupKey = FName(*FString::Printf(TEXT("environment:%d:%s"), R.InteriorIndex, *R.EntityId));
            G->GroupLabel = FText::FromString(Humanize(R.EntityClass.IsEmpty() ? Name : R.EntityClass));
            G->GridLabel = FText::FromString(FString::Printf(TEXT("Grid %d"), Found));
        }
        // A grid with a mechanism starts at its rest pose: retracted (the clips have not been played).
        G->Movers = SCCargoFindMovers(Actor, Anchor, G->WorldCenter());
        const bool bOpen = G->Movers.IsEmpty();
        G->Access = bOpen ? ESCCargoGridAccess::Open : ESCCargoGridAccess::Closed;
        State->Grids.Add(G->GridId, G);
        // The way into the grid is opened by the geometric access rule: in the environment (a gate, the platform raised)
        // and in a ship's interior hold (a ramp, cargo doors: whatever poses clear the grid's access corridors to the
        // outside). External grids carry themselves out (their mechanism, above) or are always open.
        G->bAccessRule = Kind == ESCCargoGridKind::Environment || (!R.Container.bExternalContainer && G->Movers.IsEmpty());
        // The environment's way in is opened now (its state is the environment's); a ship's hold stays as its doors stand
        // in the data (SCItemDoorParams.IsOpened: closed on the C2) until a transfer asks for it (RequestAccess).
        if (G->bAccessRule && Kind == ESCCargoGridKind::Environment)
        {
            const FSCAccessResult Opened = FSCEnvironmentAccess::OpenGrid(G);
            G->ReadyAt = GetWorld()->GetTimeSeconds() + Opened.Seconds;
            G->bAccessOpened = true;
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRID_ENV_ACCESS id=%d rigs_tested=%d rigs_moved=%d seconds=%.1f"), G->GridId, Opened.RigsTested,
                   Opened.Moves.Num(), Opened.Seconds);
        }
        else if (G->bAccessRule) G->Access = ESCCargoGridAccess::Closed;
        const FTransform W = G->LatticeToWorld();
        UE_LOG(LogSCCargo, Display,
               TEXT("CARGO_GRID id=%d kind=%s group=\"%s\" label=\"%s\" entity=%s port=%s parent_node=%s cells=%s cell_m=%s max_item_m=%s interior_m=%s "
                    "offset_m=%s access=%s movers=%d external=%d access_rule=%d entity_world=%s lattice_min_world=%s"),
               G->GridId, Kind == ESCCargoGridKind::Vehicle ? TEXT("vehicle") : TEXT("environment"), *G->GroupLabel.ToString(), *G->GridLabel.ToString(), *Name,
               *SCCargoRecords::PortKey(R.PortPath), *R.ParentNodeName, *Lattice.Cells.ToString(), *Lattice.CellM.ToString(), *Lattice.MaxItemM.ToString(),
               *Lattice.InteriorM.ToString(), *R.Container.GridPosOffset.ToString(), G->Access == ESCCargoGridAccess::Closed ? TEXT("closed") : TEXT("open"), G->Movers.Num(), R.Container.bExternalContainer, G->bAccessRule,
               *G->GetComponentLocation().ToString(), *W.GetLocation().ToString());
    }
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRIDS kind=%s actor=%s containers=%d grids=%d"), Kind == ESCCargoGridKind::Vehicle ? TEXT("vehicle") : TEXT("environment"),
           *Owner, M.InventoryContainers.Num(), Found);
    ++Revision;
}

void USCCargoSubsystem::UpdateAccess(USCCargoGridComponent* Grid, double Now)
{
    ESCCargoGridAccess Access = ESCCargoGridAccess::Open;
    bool bMoving = false, bTargetOpen = true, bAllOpen = true;
    for (const FSCCargoMover& M : Grid->Movers)
    {
        const FSCCargoMoverState S = State->Movers.FindRef({TObjectKey<USCRigComponent>(M.Rig.Get()), M.Clip});
        bAllOpen &= S.bOpen;
        if (Now < S.Until) { bMoving = true; bTargetOpen &= S.bOpen; }
    }
    if (Grid->Movers.IsEmpty() && Now < Grid->ReadyAt) Access = ESCCargoGridAccess::Opening;
    if (!Grid->Movers.IsEmpty())
        Access = bMoving ? (bTargetOpen ? ESCCargoGridAccess::Opening : ESCCargoGridAccess::Closing) : bAllOpen ? ESCCargoGridAccess::Open : ESCCargoGridAccess::Closed;
    // The access rule's part (C2 fixes): not asked for yet, or a door it opened closed again (its pad, a reset): Closed; a door
    // it opened still playing: Opening. Its own mechanism (Movers) decides only once the way is open.
    if (Grid->bAccessRule)
    {
        const ASCVehicle* Vehicle = Grid->OwnerVehicle.Get();
        bool bPlaying = false, bShut = false;
        for (const int32 Door : Grid->AccessDoors)
            if (Vehicle && Vehicle->Doors.IsValidIndex(Door))
            {
                if (Vehicle->IsDoorPlaying(Door)) bPlaying = true;
                else if (!Vehicle->IsDoorOpen(Door)) bShut = true;
            }
        if (bShut) Grid->bAccessOpened = false; // the next request opens it again
        if (!Grid->bAccessOpened) Access = ESCCargoGridAccess::Closed;
        else if (bPlaying) Access = ESCCargoGridAccess::Opening;
    }
    if (Access == Grid->Access) return;
    Grid->Access = Access;
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRID_ACCESS id=%d access=%s"), Grid->GridId, *StaticEnum<ESCCargoGridAccess>()->GetNameStringByValue(int64(Access)));
    ++Revision;
}

bool USCCargoSubsystem::RequestAccess(USCCargoGridComponent* Grid, bool bOpen, double Now, FName Requester, FString* OutRefusal)
{
    // A grid of the access rule is opened on request: whatever poses clear its corridors (a ship's doors through their own
    // interaction, FSCEnvironmentAccess), again whenever a door it opened was closed since (UpdateAccess). It does not close
    // (the rule has no closing pose); a mechanism of its own (Movers) is driven below either way.
    if (Grid->Kind == ESCCargoGridKind::Vehicle && Requester.IsNone()) Requester = State->Player;
    ASCVehicle* Vehicle = Grid->OwnerVehicle.Get();
    const auto* ShipLink = USCShipLinkSubsystem::Get(this);
    if (bOpen && Vehicle && Grid->AccessDoors.ContainsByPredicate([&](int32 Door) {
        return Vehicle->ShipDoors.IsValidIndex(Door) &&
            (Vehicle->ShipDoors[Door].bLocked || Vehicle->ShipDoors[Door].bCargoRelock);
    }) && (!ShipLink || ShipLink->RoleOf(Requester, Vehicle) != ESCShipRole::Owner))
    {
        if (OutRefusal) *OutRefusal = TEXT("Hold locked by owner");
        return false;
    }
    if (Grid->bAccessRule && bOpen && !Grid->bAccessOpened)
    {
        const FSCAccessResult Opened = FSCEnvironmentAccess::OpenGrid(Grid, Requester);
        if (!Opened.Refusal.IsEmpty())
        {
            if (OutRefusal) *OutRefusal = Opened.Refusal;
            return false;
        }
        Grid->ReadyAt = Now;
        Grid->AccessDoors.Reset();
        for (const FSCAccessMove& Move : Opened.Moves)
            if (Move.bDoor) Grid->AccessDoors.AddUnique(Move.Door);
            else Grid->ReadyAt = FMath::Max(Grid->ReadyAt, Now + Move.Seconds);
        Grid->bAccessOpened = true;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRID_ENV_ACCESS id=%d rigs_tested=%d rigs_moved=%d doors=%d on_request=1"), Grid->GridId, Opened.RigsTested,
               Opened.Moves.Num(), Grid->AccessDoors.Num());
    }
    int32 Played = 0;
    float Seconds = 0;
    for (const FSCCargoMover& M : Grid->Movers)
    {
        FSCCargoMoverState& S = State->Movers.FindOrAdd({TObjectKey<USCRigComponent>(M.Rig.Get()), M.Clip});
        if (S.bOpen == bOpen || !Vehicle || !M.Rig.IsValid()) continue;
        // Towards the clip's open end (Classify): forwards when its last frame is open, backwards otherwise.
        if (Vehicle->PlayClipAt(M.Clip, M.Rig.Get(), bOpen == M.bForwardOpens ? 1.f : -1.f))
        {
            S.bOpen = bOpen;
            S.Until = Now + M.Seconds;
            Seconds = FMath::Max(Seconds, M.Seconds);
            ++Played;
        }
    }
    if (Played)
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_GRID_ACCESS id=%d request=%s clips_played=%d seconds=%.1f"), Grid->GridId, bOpen ? TEXT("extend") : TEXT("retract"),
               Played, Seconds);
    UpdateAccess(Grid, Now);
    return Grid->Access == (bOpen ? ESCCargoGridAccess::Open : ESCCargoGridAccess::Closed);
}

bool USCCargoSubsystem::IsAccessDoorBusy(const ASCVehicle* Ship, int32 Door) const
{
    if (!State) return false;
    for (const auto& Pair : State->Grids)
        if (const USCCargoGridComponent* Grid = Pair.Value.Get();
            Grid && Grid->OwnerVehicle.Get() == Ship && Grid->bAccessRule &&
            (Grid->AccessDoors.Contains(Door) || (Ship->ShipDoors.IsValidIndex(Door) && Ship->ShipDoors[Door].bCargoRelock)) &&
            State->Jobs.ContainsByPredicate([&](const FSCCargoJob& Job) {
                return Job.IsActive() && Job.Moves.ContainsByPredicate([&](const FSCCargoMove& Move) {
                    return Move.Plan.FromGrid == Grid->GridId || Move.Plan.ToGrid == Grid->GridId;
                });
            })) return true;
    return false;
}

bool USCCargoSubsystem::CanClose(const USCCargoGridComponent* Grid, FText* OutWhy, int32 IgnoreJob) const
{
    auto Why = [&](const FText& T) { if (OutWhy) *OutWhy = T; return false; };
    if (Grid->Movers.IsEmpty()) return Why(FText()); // fixed: nothing to retract (the UI reads an empty reason so)
    // Every grid the same clips carry must be empty: a retracting mechanism takes all of them along.
    for (const auto& Pair : State->Grids)
    {
        const USCCargoGridComponent* G = Pair.Value.Get();
        const bool bShares = G && G->Movers.ContainsByPredicate([&](const FSCCargoMover& A) {
            return Grid->Movers.ContainsByPredicate([&](const FSCCargoMover& B) { return A.Rig == B.Rig && A.Clip == B.Clip; });
        });
        if (!bShares) continue;
        for (const FSCCargoJob& J : State->Jobs)
            if (J.Id != IgnoreJob && J.IsActive() &&
                J.Moves.ContainsByPredicate([&](const FSCCargoMove& M) { return M.Plan.FromGrid == G->GridId || M.Plan.ToGrid == G->GridId; }))
                return Why(FText::Format(NSLOCTEXT("SCCargo", "CloseBusy", "{0} is in use by a transfer"), G->GridLabel));
        if (G->Placed.IsEmpty()) continue;
        int32 SCU = 0;
        for (const auto& B : State->Boxes)
            if (B.Value.GridId == G->GridId) SCU += B.Value.Occupancy.StandardCargoUnits;
        const FText Who = G == Grid ? NSLOCTEXT("SCCargo", "ThisGrid", "Grid") : G->GridLabel;
        return Why(SCU > 0 ? FText::Format(NSLOCTEXT("SCCargo", "CloseLoaded", "{0} holds {1} SCU; unload it before retracting"), Who, SCU)
                           : FText::Format(NSLOCTEXT("SCCargo", "CloseDock", "{0} holds the drone dock and cannot retract"), Who));
    }
    return true;
}

bool USCCargoSubsystem::RequestGridAccess(int32 GridId, bool bOpen, FText& OutError)
{
    USCCargoGridComponent* Grid = FindGrid(GridId);
    if (!Grid)
    {
        OutError = NSLOCTEXT("SCCargo", "NoGrid", "No such grid");
        return false;
    }
    if (Grid->bAccessRule && bOpen)
    {
        // The way in is opened (a ship's hold: its doors), with the grid's own mechanism if it has one.
        RequestAccess(Grid, true, GetWorld()->GetTimeSeconds());
        return true;
    }
    if (Grid->Movers.IsEmpty())
    {
        OutError = NSLOCTEXT("SCCargo", "FixedGrid", "This grid does not extend or retract");
        return bOpen;
    }
    if (!bOpen && !CanClose(Grid, &OutError)) return false;
    RequestAccess(Grid, bOpen, GetWorld()->GetTimeSeconds());
    return true;
}

FText USCCargoSubsystem::OwnerLabel(FName Owner) const
{
    if (Owner.IsNone()) return NSLOCTEXT("SCCargo", "OwnerShared", "Shared");
    if (const FText* L = State->OwnerLabels.Find(Owner)) return *L;
    return Owner == State->Player ? NSLOCTEXT("SCCargo", "OwnerYou", "You") : FText::FromName(Owner);
}

FName USCCargoSubsystem::RequesterOf(const FSCCargoTransferRequest& Request) const
{
    return Request.Requester.IsNone() ? State->Player : Request.Requester;
}
