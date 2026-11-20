// USCCargoSubsystem live spawning (developer console only: sc.Cargo.Spawn / Clear / Catalog, RPC /console): the game's
// box records read on demand (the entity search CargoBoxSearch, each hit's DataCore record, its commodity's record),
// spawns planned on grids with the planner's packing (SCCargoSpawn.h), queued until their box packages are exported
// and put down as scenario stock is (SpawnBox), and clears of the boxes no transfer holds. A session without a scenario
// takes on the default drone fleet (CargoFleet) with its first spawn, so spawned cargo can be moved.
#include "Cargo/SCCargoSubsystem.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoGridComponent.h"
#include "SCBridgeClient.h"
#include "SCViewerSettings.h"
#include "SCViewerSubsystem.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"

#define LOCTEXT_NAMESPACE "SCCargoSpawn"

namespace
{
/** Finished spawns kept for their status (GetSpawnStatus). */
constexpr int32 KeptSpawns = 32;

FString GridsText(const TArray<int32>& Ids) { return Ids.IsEmpty() ? FString(TEXT("-")) : FString::JoinBy(Ids, TEXT("+"), [](int32 I) { return FString::FromInt(I); }); }
FString GridName(const USCCargoGridComponent* G)
{
    const FString Group = G->GroupLabel.ToString(), Grid = G->GridLabel.ToString();
    return Grid.IsEmpty() ? Group : Group.IsEmpty() ? Grid : Group + TEXT(" · ") + Grid;
}
FText CatalogNote(const FSCCargoState& S)
{
    switch (S.CatalogState)
    {
    case ESCCargoCatalogState::Ready:
        return FText::Format(LOCTEXT("CatalogReady", "{0} box records"), S.Catalog ? S.Catalog->Num() : 0);
    case ESCCargoCatalogState::Failed:
        return FText::Format(LOCTEXT("CatalogFailed", "The game's box records could not be read: {0}"), FText::FromString(S.CatalogError));
    case ESCCargoCatalogState::Loading:
        if (S.CatalogProgress && S.CatalogProgress->Total > 0)
            return FText::Format(LOCTEXT("CatalogProgress", "Reading the game's box records ({0} of {1})"), int32(S.CatalogProgress->Done), int32(S.CatalogProgress->Total));
        return LOCTEXT("CatalogWaiting", "Reading the game's box records");
    default: return LOCTEXT("CatalogNone", "The game's box records are not read yet");
    }
}
FSCCargoCatalogResult ReadCatalog(const TSharedPtr<FSCBridgeClient, ESPMode::ThreadSafe>& Client, const FString& Term, const TSharedPtr<FSCCargoCatalogProgress>& Progress)
{
    FSCCargoCatalogResult R;
    const double Start = FPlatformTime::Seconds();
    auto Cancelled = [&] {
        if (Progress->bCancel) R.Error = TEXT("cancelled");
        return bool(Progress->bCancel);
    };
    if (Cancelled()) return R;
    // Candidates by name in the entity index (an empty term: every entity); each is kept or dropped by its record alone.
    const FSCSearchResult Found = Client->Search(Term, 1000000).GetResult();
    if (!Found.Error.IsEmpty()) { R.Error = Found.Error.ToString(); return R; }
    R.Hits = Found.Hits.Num();
    Progress->Total = R.Hits;
    struct FPending { FSCCargoBoxRecord Record; FString Ref; };
    TArray<FPending> Pending;
    for (const FSCEntityHit& Hit : Found.Hits)
    {
        if (Cancelled()) return R;
        const FSCTextResult Record = Client->DataCoreRecord(Hit.Guid.IsEmpty() ? TEXT("EntityClassDefinition.") + Hit.Name : Hit.Guid).GetResult();
        ++Progress->Done;
        // A panic closed the handle: nothing more can be read, and a partial catalogue is no catalogue.
        if (Record.bPanicked) { R.Error = Record.Error.ToString(); return R; }
        if (!Record.bOK) { ++R.Failed; continue; }
        FPending P;
        bool bGroup = false;
        FString Why;
        if (!SCCargoSpawn::ParseBoxRecord(Record.Json, P.Record, P.Ref, bGroup, Why)) { ++R.Skipped; continue; }
        P.Record.Entity = Hit.Name;
        P.Record.DisplayName = Hit.DisplayName;
        Pending.Add(MoveTemp(P));
    }
    // Each commodity by its record's name (a reference is a GUID; one lookup per distinct one). A box whose commodity
    // cannot be named is left out: under its GUID it would be another commodity than the same resource's other boxes.
    TMap<FString, FString> Names;
    R.Catalog = MakeShared<FSCCargoBoxCatalog>();
    for (FPending& P : Pending)
    {
        const FString* Name = Names.Find(P.Ref);
        if (!Name)
        {
            if (Cancelled()) { R.Catalog.Reset(); return R; }
            FString Short;
            FGuid Guid;
            if (FGuid::Parse(P.Ref, Guid))
            {
                const FSCTextResult Ref = Client->DataCoreRecord(P.Ref).GetResult();
                if (Ref.bPanicked) { R.Error = Ref.Error.ToString(); R.Catalog.Reset(); return R; }
                if (Ref.bOK) Short = SCCargoSpawn::RecordShortName(Ref.Json);
            }
            else
            {
                FString Type;
                if (!P.Ref.Split(TEXT("."), &Type, &Short)) Short = P.Ref;
            }
            Name = &Names.Add(P.Ref, Short);
        }
        if (Name->IsEmpty()) { ++R.Unresolved; continue; }
        P.Record.Commodity = FName(**Name);
        R.Catalog->Add(P.Record);
    }
    if (Cancelled()) { R.Catalog.Reset(); return R; }
    R.Catalog->Finish();
    R.Commodities = R.Catalog->Commodities().Num();
    R.Seconds = FPlatformTime::Seconds() - Start;
    return R;
}
} // namespace

// ------------------------------------------------------------------------------------------------------------ records

ESCCargoCatalogState USCCargoSubsystem::RequestSpawnCatalog(FText* OutNote)
{
    if (!State) return ESCCargoCatalogState::Failed;
    State->bCatalogWanted = true;
    // Not read yet, or failed before (the archive may be open now): read when the archive allows.
    if (State->CatalogState == ESCCargoCatalogState::None || (State->CatalogState == ESCCargoCatalogState::Failed && !State->CatalogTask.IsValid()))
    {
        State->CatalogState = ESCCargoCatalogState::Loading;
        State->CatalogError.Reset();
    }
    if (OutNote) *OutNote = CatalogNote(*State);
    return State->CatalogState;
}

TArray<FSCCargoSpawnCommodity> USCCargoSubsystem::GetSpawnCommodities() const
{
    return State && State->Catalog && State->CatalogState == ESCCargoCatalogState::Ready ? State->Catalog->Commodities() : TArray<FSCCargoSpawnCommodity>();
}

void USCCargoSubsystem::ListSpawnCatalog(const FString& Text)
{
    FText Note;
    if (RequestSpawnCatalog(&Note) != ESCCargoCatalogState::Ready)
    {
        State->CatalogListing = Text; // listed once the records are read
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_CATALOG %s"), *Note.ToString());
        return;
    }
    State->CatalogListing.Reset();
    int32 Shown = 0;
    for (const FSCCargoSpawnCommodity& C : GetSpawnCommodities())
    {
        if (!Text.IsEmpty() && !C.Commodity.ToString().Contains(Text) && !C.Label.ToString().Contains(Text)) continue;
        FString Sizes;
        for (int32 I = 0; I < C.SizesSCU.Num(); ++I) Sizes += FString::Printf(TEXT(" %d(%gx%gx%g)"), C.SizesSCU[I], C.SizesM[I].X, C.SizesM[I].Y, C.SizesM[I].Z);
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_CATALOG_COMMODITY key=%s label=\"%s\" sizes=[%s ]"), *C.Commodity.ToString(), *C.Label.ToString(), *Sizes);
        ++Shown;
    }
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_CATALOG %s shown=%d"), *Note.ToString(), Shown);
}

void USCCargoSubsystem::TickCatalog()
{
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld());
    if (!Viewer) return;
    // Another archive opened: the records read from the old one are gone (read again when asked).
    if (State->CatalogGeneration != INDEX_NONE && State->CatalogGeneration != Viewer->GetArchiveGeneration())
    {
        if (State->CatalogProgress) State->CatalogProgress->bCancel = true;
        State->Catalog.Reset();
        State->CatalogGeneration = INDEX_NONE;
        if (!State->CatalogTask.IsValid() || State->CatalogTask.IsCompleted())
        {
            State->CatalogTask = {};
            State->CatalogState = State->bCatalogWanted ? ESCCargoCatalogState::Loading : ESCCargoCatalogState::None;
        }
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_CATALOG reset: another archive (generation %d)"), Viewer->GetArchiveGeneration());
        ++Revision;
    }
    if (State->CatalogTask.IsValid())
    {
        if (!State->CatalogTask.IsCompleted()) return;
        FSCCargoCatalogResult R = State->CatalogTask.GetResult();
        State->CatalogTask = {};
        if (State->CatalogGeneration == INDEX_NONE) // read for an archive no longer open
        {
            State->CatalogState = State->bCatalogWanted ? ESCCargoCatalogState::Loading : ESCCargoCatalogState::None;
            return;
        }
        if (!R.Catalog || R.Catalog->Num() == 0)
        {
            State->CatalogState = ESCCargoCatalogState::Failed;
            State->CatalogGeneration = INDEX_NONE;
            State->CatalogError = R.Error.IsEmpty() ? FString::Printf(TEXT("no box records among %d entities matching \"%s\""), R.Hits, *USCViewerSettings::Get()->CargoBoxSearch) : R.Error;
            UE_LOG(LogSCCargo, Warning, TEXT("CARGO_CATALOG failed hits=%d reason=\"%s\""), R.Hits, *State->CatalogError);
            return;
        }
        State->Catalog = R.Catalog;
        State->CatalogState = ESCCargoCatalogState::Ready;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_CATALOG ready hits=%d records=%d commodities=%d skipped=%d failed=%d unresolved=%d seconds=%.2f"), R.Hits,
               R.Catalog->Num(), R.Commodities, R.Skipped, R.Failed, R.Unresolved, R.Seconds);
        if (State->CatalogListing) { const FString Listing = *State->CatalogListing; ListSpawnCatalog(Listing); } // a copy: listing clears it
        ++Revision;
        return;
    }
    if (!State->bCatalogWanted || State->CatalogState != ESCCargoCatalogState::Loading) return;
    const ESCArchiveState Archive = Viewer->GetArchiveState();
    if (Archive == ESCArchiveState::Opening) return; // still opening
    if (Archive != ESCArchiveState::Ready || !Viewer->GetClient())
    {
        // No archive (failed, or none opened: a package-only session): asked again later, read then.
        State->CatalogState = ESCCargoCatalogState::Failed;
        State->CatalogError = TEXT("the archive is not open (box records are read from Data.p4k)");
        UE_LOG(LogSCCargo, Warning, TEXT("CARGO_CATALOG failed reason=\"%s\""), *State->CatalogError);
        return;
    }
    State->CatalogProgress = MakeShared<FSCCargoCatalogProgress>();
    State->CatalogGeneration = Viewer->GetArchiveGeneration();
    const FString Term = USCViewerSettings::Get()->CargoBoxSearch;
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_CATALOG reading search=\"%s\" generation=%d"), *Term, State->CatalogGeneration);
    State->CatalogTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Client = Viewer->GetClient(), Term, Progress = State->CatalogProgress] {
        return ReadCatalog(Client, Term, Progress);
    });
}

// ------------------------------------------------------------------------------------------------------------ fleet

void USCCargoSubsystem::RequestFleet()
{
    // A scenario brings its own drones; without one, the first spawn brings the default fleet.
    if (!State->Scenario.IsSet() && !State->bFleetRequested) State->bFleetRequested = true;
}

void USCCargoSubsystem::LoadFleet()
{
    const FString File = USCViewerSettings::Get()->ResolveCargoFleet();
    if (File.IsEmpty())
    {
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_FLEET none (CargoFleet empty): spawned cargo has no drones"));
        return;
    }
    FSCCargoScenario Fleet;
    FString Error;
    if (!FSCCargoScenario::Load(File, Fleet, Error))
    {
        UE_LOG(LogSCCargo, Error, TEXT("CARGO_FLEET failed: %s"), *Error);
        return;
    }
    // Its drone definition and docks only. A dock whose grid is not there yet stays: it goes down when its grid appears
    // (a hangar loaded after the first spawn), as a scenario's does.
    Fleet.Ship.Reset();
    Fleet.Environment.Reset();
    Fleet.Stock.Reset();
    Fleet.Loose.Reset();
    Fleet.Obstacles.Reset();
    Fleet.VehicleOwners.Reset();
    Fleet.VehiclePoses.Reset();
    Fleet.bSpace = false;
    int32 Drones = 0, Here = 0;
    for (const FSCCargoDockSpec& D : Fleet.Docks)
    {
        Drones += D.Drones;
        Here += SelectGrid(D.Grid) != nullptr;
    }
    State->Scenario = MoveTemp(Fleet);
    State->bDefaultFleet = true;
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_FLEET loaded=\"%s\" docks=%d (grids here: %d) drones=%d"), *File, State->Scenario.Docks.Num(), Here, Drones);
}

bool USCCargoSubsystem::FleetPending() const
{
    // Asked for and not loaded yet, or a dock of it whose grid is here and not yet placed (its kind not applied).
    if (!State->bFleetRequested || State->Scenario.IsSet() != State->bDefaultFleet) return false;
    if (!State->bFleetTried) return true;
    if (!State->bDefaultFleet) return false; // no fleet file: nothing to wait for
    for (const FSCCargoDockSpec& D : State->Scenario.Docks)
        if (!(State->AppliedKinds & (1u << uint8(D.Grid.Kind))) && SelectGrid(D.Grid)) return true;
    return false;
}

// ------------------------------------------------------------------------------------------------------------ spawn

FSCCargoGridModel USCCargoSubsystem::SpawnModel(const USCCargoGridComponent* G, TSet<int32>& OutNoSupport) const
{
    // The hold's entrance first: probing it adds the hold's structure cells to the grid's placements.
    const int32 Axis = EntranceAxis(const_cast<USCCargoGridComponent*>(G));
    FSCCargoGridModel M = G->Model();
    M.PreferredLongAxis = Axis;
    // Incoming boxes' reserved slots occupy their cells but bear nothing until the box is down; a box of a transfer
    // that is still here is about to leave (its cover was planned without anything new on it).
    OutNoSupport.Append(G->Reserved);
    for (const FSCCargoPlacement& P : M.Placed)
        if (const FSCCargoBox* B = State->Boxes.Find(P.ItemId))
            if (B->State != ESCCargoBoxState::Stored ||
                (B->JobId != INDEX_NONE && State->Jobs.ContainsByPredicate([&](const FSCCargoJob& J) { return J.Id == B->JobId && J.IsActive(); })))
                OutNoSupport.Add(P.ItemId);
    // A box held aside by a dig-out goes back to its own slot here: kept free (that slot is not reserved on the grid).
    for (const FSCCargoJob& J : State->Jobs)
    {
        if (!J.IsActive()) continue;
        for (const FSCCargoMove& Mv : J.Moves)
            if (Mv.Plan.Kind == ESCCargoMoveKind::Hold && Mv.Plan.ToGrid == G->GridId && Mv.Step != FSCCargoMove::EStep::Done &&
                Mv.Step != FSCCargoMove::EStep::Failed && !M.Find(Mv.Plan.BoxId))
            {
                FSCCargoPlacement Return = SCCargoSlotOf(Mv.Plan);
                M.Placed.Add(Return);
                OutNoSupport.Add(Return.ItemId);
            }
    }
    return M;
}

FSCCargoSpawnPreview USCCargoSubsystem::PreviewSpawn(const FSCCargoSpawnRequest& Request) const
{
    FSCCargoSpawnPreview P;
    auto Refuse = [&P](const TCHAR* Key, const FText& Why) {
        P.bValid = false;
        P.Refused = Key;
        P.Reason = Why;
        return P;
    };
    if (!State) return Refuse(TEXT("grid"), LOCTEXT("NoCargo", "No cargo system in this scene"));
    if (Request.Grids.IsEmpty()) return Refuse(TEXT("grid"), LOCTEXT("NoGrid", "No grid matches"));
    TArray<const USCCargoGridComponent*> Open;
    FText Closed;
    for (const int32 Id : Request.Grids)
    {
        if (IsBeacon(Id))
            return Refuse(TEXT("grid"), FText::Format(LOCTEXT("Beacon", "{0} is loose cargo, not a grid: boxes are spawned onto grids"), LocationLabel(Id)));
        const USCCargoGridComponent* G = FindGrid(Id);
        if (!G) return Refuse(TEXT("grid"), FText::Format(LOCTEXT("NoGridN", "No grid {0}"), Id));
        if (G->Access == ESCCargoGridAccess::Open) { Open.Add(G); continue; }
        if (!Closed.IsEmpty()) continue;
        // Cargo rides only on an extended grid (a hold: an open one).
        const FText Name = FText::FromString(GridName(G));
        Closed = G->Access == ESCCargoGridAccess::Opening ? FText::Format(LOCTEXT("Extending", "{0} is still extending"), Name)
               : G->Access == ESCCargoGridAccess::Closing ? FText::Format(LOCTEXT("Retracting", "{0} is retracting"), Name)
               : G->Movers.IsEmpty() && G->bAccessRule    ? FText::Format(LOCTEXT("ClosedHold", "{0} is closed: open it first"), Name)
                                                          : FText::Format(LOCTEXT("Retracted", "{0} is retracted: extend it first"), Name);
    }
    if (Open.IsEmpty()) return Refuse(TEXT("retracted"), Closed);
    if (State->CatalogState != ESCCargoCatalogState::Ready || !State->Catalog) return Refuse(TEXT("catalog"), CatalogNote(*State));
    TArray<const FSCCargoBoxRecord*> Boxes;
    if (const FSCCargoBoxRecord* Entity = State->Catalog->FindEntity(Request.What.TrimStartAndEnd())) Boxes.Add(Entity);
    else
    {
        FString Why;
        const FName Commodity = State->Catalog->FindCommodity(Request.What, Why);
        if (Commodity.IsNone()) return Refuse(TEXT("commodity"), FText::FromString(Why));
        Boxes = State->Catalog->BoxesOf(Commodity);
    }
    TArray<FSCCargoGridModel> Models;
    TSet<int32> NoSupport;
    for (const USCCargoGridComponent* G : Open) Models.Add(SpawnModel(G, NoSupport));
    return SCCargoSpawn::Plan(Models, Boxes, Request, State->NextItemId + 1000000, NoSupport);
}

namespace
{
TArray<FString> EntitiesOf(const FSCCargoSpawnPreview& P)
{
    TArray<FString> Out;
    for (const FSCCargoSpawnBoxPlan& B : P.Boxes) Out.AddUnique(B.Entity);
    return Out;
}
void LogRefused(const FSCCargoSpawnRequest& R, const FSCCargoSpawnPreview& P, int32 SpawnId)
{
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_SPAWN refused=%s grid=%s what=\"%s\" reason=\"%s\"%s"), *P.Refused.ToString(), *GridsText(R.Grids), *R.What, *P.Reason.ToString(),
           SpawnId == INDEX_NONE ? TEXT("") : *FString::Printf(TEXT(" id=%d"), SpawnId));
}
} // namespace

int32 USCCargoSubsystem::Spawn(const FSCCargoSpawnRequest& Request, FSCCargoSpawnPreview& OutPreview)
{
    if (!State) return INDEX_NONE;
    RequestSpawnCatalog();
    OutPreview = PreviewSpawn(Request);
    // Still reading the box records: it waits for them (and is planned then); any other refusal is final.
    const bool bWaiting = !OutPreview.bValid && OutPreview.Refused == TEXT("catalog") && State->CatalogState != ESCCargoCatalogState::Failed;
    if (!OutPreview.bValid && !bWaiting)
    {
        LogRefused(Request, OutPreview, INDEX_NONE);
        return INDEX_NONE;
    }
    FSCCargoPendingSpawn& S = State->Spawns.AddDefaulted_GetRef();
    S.Request = Request;
    S.Since = GetWorld()->GetTimeSeconds();
    S.Status.SpawnId = State->NextSpawnId++;
    S.Status.Grids = Request.Grids;
    S.Status.Text = bWaiting ? CatalogNote(*State) : LOCTEXT("Queued", "Exporting the box packages");
    if (OutPreview.bValid)
    {
        // A valid plan brings the default fleet (no scenario) and starts its packages exporting while it waits.
        RequestFleet();
        S.Entities = EntitiesOf(OutPreview);
        FString Ignored;
        for (const FString& E : S.Entities) RequestBoxPackage(E, Ignored);
    }
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_SPAWN_QUEUED id=%d grid=%s what=\"%s\" commodity=%s boxes=%d scu=%d%s"), S.Status.SpawnId, *GridsText(Request.Grids), *Request.What,
           *OutPreview.Commodity.ToString(), OutPreview.Boxes.Num(), OutPreview.TotalSCU, bWaiting ? TEXT(" waiting=catalog") : TEXT(""));
    const int32 Id = S.Status.SpawnId;
    // Older finished spawns go (their boxes stay).
    while (State->Spawns.Num() > KeptSpawns)
    {
        const int32 Old = State->Spawns.IndexOfByPredicate([](const FSCCargoPendingSpawn& X) { return X.Status.State != ESCCargoSpawnState::Waiting; });
        if (Old == INDEX_NONE) break;
        State->Spawns.RemoveAt(Old);
    }
    ++Revision;
    return Id;
}

bool USCCargoSubsystem::GetSpawnStatus(int32 SpawnId, FSCCargoSpawnStatus& Out) const
{
    const FSCCargoPendingSpawn* S = State ? State->Spawns.FindByPredicate([&](const FSCCargoPendingSpawn& X) { return X.Status.SpawnId == SpawnId; }) : nullptr;
    if (!S) return false;
    Out = S->Status;
    return true;
}

void USCCargoSubsystem::TickSpawns()
{
    for (FSCCargoPendingSpawn& S : State->Spawns)
    {
        FSCCargoSpawnStatus& St = S.Status;
        if (St.State != ESCCargoSpawnState::Waiting) continue;
        auto Refuse = [&](const FSCCargoSpawnPreview& P) {
            St.State = ESCCargoSpawnState::Refused;
            St.Text = P.Reason;
            LogRefused(S.Request, P, St.SpawnId);
            ++Revision;
        };
        if (State->CatalogState != ESCCargoCatalogState::Ready)
        {
            St.Text = CatalogNote(*State);
            if (State->CatalogState == ESCCargoCatalogState::Failed)
            {
                FSCCargoSpawnPreview P;
                P.Refused = TEXT("catalog");
                P.Reason = St.Text;
                Refuse(P);
            }
            continue;
        }
        // Planned once the records are read (a spawn that waited for them): refused now, or its packages requested and
        // the default fleet asked for.
        if (S.Entities.IsEmpty())
        {
            const FSCCargoSpawnPreview P = PreviewSpawn(S.Request);
            if (!P.bValid) { Refuse(P); continue; }
            S.Entities = EntitiesOf(P);
            FString Ignored;
            for (const FString& E : S.Entities) RequestBoxPackage(E, Ignored);
            RequestFleet();
        }
        // Without a scenario the default fleet's docks go down first: they take cells too.
        if (FleetPending())
        {
            St.Text = LOCTEXT("WaitFleet", "Waiting for the drone docks to be put down");
            continue;
        }
        // Its packages (not planned again every frame while they export).
        bool bReady = true;
        FString Error;
        for (const FString& E : S.Entities)
        {
            const int32 Package = RequestBoxPackage(E, Error);
            if (Package < 0) { Error = FString::Printf(TEXT("the box package %s failed: %s"), *E, *Error); break; }
            Error.Reset();
            bReady &= Package == 1;
        }
        if (!Error.IsEmpty())
        {
            FSCCargoSpawnPreview P;
            P.Refused = TEXT("package");
            P.Reason = FText::FromString(Error);
            Refuse(P);
            continue;
        }
        if (!bReady)
        {
            St.Text = LOCTEXT("WaitPackages", "Exporting the box packages");
            continue;
        }
        // Planned again now (the grids may have changed); a plan that needs another box waits for its package too.
        FSCCargoSpawnPreview P = PreviewSpawn(S.Request);
        if (!P.bValid) { Refuse(P); continue; }
        const TArray<FString> Need = EntitiesOf(P);
        if (Need.ContainsByPredicate([&](const FString& E) { return !S.Entities.Contains(E); }))
        {
            for (const FString& E : Need) S.Entities.AddUnique(E);
            continue;
        }
        // Bottom up, each onto its planned cell (SpawnBox checks it is free and supported, as for scenario stock). All or
        // nothing: a box that cannot go down takes the request's others with it.
        TSet<int32> NoSupport;
        for (const int32 GridId : S.Request.Grids)
            if (const USCCargoGridComponent* G = FindGrid(GridId)) SpawnModel(G, NoSupport);
        TArray<int32> Ids;
        TArray<FName> Owners;
        int32 Scu = 0;
        FString Failed;
        for (const FSCCargoSpawnBoxPlan& B : P.Boxes)
        {
            USCCargoGridComponent* Grid = FindGrid(B.GridId);
            const int32 Id = Grid ? SpawnBox(Grid, B.Entity, P.Commodity, P.CommodityLabel, B.Origin, B.bRotated, &NoSupport) : INDEX_NONE;
            if (Id == INDEX_NONE)
            {
                Failed = FString::Printf(TEXT("the %d SCU box %s could not be put on its planned cell %s of grid %d"), B.SCU, *B.Entity, *B.Origin.ToString(), B.GridId);
                break;
            }
            FSCCargoBox& Box = State->Boxes[Id];
            Box.Owner = !S.Request.Owner.IsNone() ? S.Request.Owner : !Grid->Owner.IsNone() ? Grid->Owner : State->Player;
            Owners.AddUnique(Box.Owner);
            Ids.Add(Id);
            Scu += Box.Occupancy.StandardCargoUnits;
        }
        if (!Failed.IsEmpty())
        {
            for (const int32 Id : Ids) ForgetBox(Id);
            P.Refused = TEXT("full");
            P.Reason = FText::FromString(Failed);
            Refuse(P);
            continue;
        }
        St.State = ESCCargoSpawnState::Done;
        St.BoxIds = Ids;
        St.SCU = Scu;
        St.Text = FText::Format(LOCTEXT("Added", "Added {0} {0}|plural(one=box,other=boxes) · {1} SCU of {2}"), Ids.Num(), Scu, P.CommodityLabel);
        TArray<int32> Grids;
        for (const FSCCargoSpawnBoxPlan& B : P.Boxes) Grids.AddUnique(B.GridId);
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_SPAWN grid=%s commodity=%s boxes=%d scu=%d owner=%s id=%d ids=%s"), *GridsText(Grids), *P.Commodity.ToString(), Ids.Num(), Scu,
               *FString::JoinBy(Owners, TEXT("+"), [](FName N) { return N.ToString(); }), St.SpawnId, *GridsText(Ids));
        ++Revision;
    }
}

// ------------------------------------------------------------------------------------------------------------ clear

bool USCCargoSubsystem::ClearLocation(int32 LocationId, TArray<int32>& OutRemoved, int32& OutKept, FString& OutError)
{
    OutRemoved.Reset();
    OutKept = 0;
    if (!State) { OutError = TEXT("no cargo system"); return false; }
    auto InJob = [&](const FSCCargoBox& B) {
        return B.JobId != INDEX_NONE && State->Jobs.ContainsByPredicate([&](const FSCCargoJob& J) { return J.Id == B.JobId && J.IsActive(); });
    };
    TSet<int32> Candidates;
    int32 There = 0;
    FBox Region(ForceInit);
    if (const FSCCargoBeacon* Beacon = State->Beacons.Find(LocationId))
    {
        // Loose cargo at rest: every box no transfer holds.
        for (const int32 Id : Beacon->Boxes)
            if (const FSCCargoBox* B = State->Boxes.Find(Id))
            {
                ++There;
                if (B->State == ESCCargoBoxState::Loose && !InJob(*B)) Candidates.Add(Id);
                if (const ASCCargoContainer* A = B->Actor.Get()) Region += A->Bounds();
            }
        OutRemoved = Candidates.Array();
    }
    else if (const USCCargoGridComponent* G = FindGrid(LocationId))
    {
        // Boxes at rest on the grid that no transfer holds, except those beneath what stays (by the lattice).
        for (const auto& [Id, B] : State->Boxes)
            if (B.GridId == LocationId && (B.State == ESCCargoBoxState::Stored || B.State == ESCCargoBoxState::Reserved))
            {
                ++There;
                if (B.State == ESCCargoBoxState::Stored && !InJob(B)) Candidates.Add(Id);
            }
        OutRemoved = SCCargoSpawn::Clearable(G->Model(), Candidates);
        FSCCargoGridView V;
        if (GetGrid(LocationId, V)) Region = V.WorldBounds;
    }
    else
    {
        OutError = FString::Printf(TEXT("no grid or beacon %d"), LocationId);
        return false;
    }
    // Off the lattice nothing says what rests on what: a box off every grid that a transfer holds (frozen where it lies,
    // a loose box reserved for pickup) keeps whatever is right under it, however deep, by their bounds.
    {
        TArray<FBox> Kept;
        for (const auto& [Id, B] : State->Boxes)
            if (B.GridId == INDEX_NONE && (InJob(B) || (B.State != ESCCargoBoxState::Loose && B.State != ESCCargoBoxState::Stored)))
                if (const ASCCargoContainer* A = B.Actor.Get()) Kept.Add(A->Bounds());
        auto Under = [](const FBox& Below, const FBox& Above) {
            const double OverlapX = FMath::Min(Below.Max.X, Above.Max.X) - FMath::Max(Below.Min.X, Above.Min.X);
            const double OverlapY = FMath::Min(Below.Max.Y, Above.Max.Y) - FMath::Max(Below.Min.Y, Above.Min.Y);
            return OverlapX > 1 && OverlapY > 1 && FMath::Abs(Above.Min.Z - Below.Max.Z) < 10;
        };
        for (bool bChanged = !Kept.IsEmpty(); bChanged;)
        {
            bChanged = false;
            for (int32 I = OutRemoved.Num() - 1; I >= 0; --I)
            {
                const ASCCargoContainer* A = State->Boxes[OutRemoved[I]].Actor.Get();
                if (!A) continue;
                const FBox Bounds = A->Bounds();
                if (!Kept.ContainsByPredicate([&](const FBox& K) { return Under(Bounds, K); })) continue;
                Kept.Add(Bounds);
                OutRemoved.RemoveAt(I);
                bChanged = true;
            }
        }
    }
    int32 Scu = 0;
    for (const int32 Id : OutRemoved)
    {
        Scu += State->Boxes[Id].Occupancy.StandardCargoUnits;
        ForgetBox(Id);
    }
    OutKept = There - OutRemoved.Num();
    // Loose boxes that may have rested on what went settle again.
    if (!OutRemoved.IsEmpty() && Region.IsValid)
    {
        const FBox Around = Region.ExpandBy(300);
        WakeLoose(&Around);
    }
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_CLEAR location=%d removed=%d scu=%d kept=%d ids=%s"), LocationId, OutRemoved.Num(), Scu, OutKept, *GridsText(OutRemoved));
    return true;
}

#undef LOCTEXT_NAMESPACE
