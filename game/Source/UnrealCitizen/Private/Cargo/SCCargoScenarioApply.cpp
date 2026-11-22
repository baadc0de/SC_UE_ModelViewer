// USCCargoSubsystem scenario: loads the scenario file, requests its box packages, (optionally) loads its ship and
// environment, and once their grids and the packages are ready puts the stock and the drone docks on the grids (a dock
// takes as many lattice cells as its mesh spans, turned to clear bay exits). Once the tractor record is read it starts
// resolving the game's effects the drones and boxes play (FSCCargoFx).
#include "Cargo/SCCargoSubsystem.h"
#include "UCSettings.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoGridComponent.h"
#include "Cargo/SCCargoRecords.h"
#include "SCCargoFx.h"
#include "SCViewerSettings.h"
#include "SCViewerSubsystem.h"
#include "SCShipLinkSubsystem.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

USCCargoGridComponent* USCCargoSubsystem::SelectGrid(const FSCCargoGridSelector& Selector) const
{
    TArray<USCCargoGridComponent*> OfKind;
    // A vehicle grid of the scenario's vehicle Selector.Vehicle (its instance id once spawned).
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld());
    const int32 Instance = State->ScenarioVehicleIds.IsValidIndex(Selector.Vehicle) ? State->ScenarioVehicleIds[Selector.Vehicle]
                                                                                     : (Selector.Vehicle == 0 ? 0 : INDEX_NONE);
    for (const auto& Pair : State->Grids)
        if (USCCargoGridComponent* G = Pair.Value.Get(); G && G->Kind == Selector.Kind &&
            (G->Kind != ESCCargoGridKind::Vehicle || (Viewer && Viewer->GetVehicleInstanceId(G->OwnerVehicle.Get()) == Instance)))
            OfKind.Add(G);
    OfKind.Sort([](const USCCargoGridComponent& A, const USCCargoGridComponent& B) { return A.GridId < B.GridId; }); // export order
    if (!Selector.Match.IsEmpty())
    {
        for (USCCargoGridComponent* G : OfKind)
            if (SCCargoRecords::PortKey(G->Record.PortPath).Contains(Selector.Match) || G->Record.EntityName.Contains(Selector.Match) ||
                G->Record.EntityClass.Contains(Selector.Match))
                return G;
        return nullptr;
    }
    return OfKind.IsValidIndex(Selector.Index) ? OfKind[Selector.Index] : nullptr;
}

void USCCargoSubsystem::TryScenario()
{
    USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld());
    if (!Viewer) return;
    if (!State->bScenarioTried)
    {
        State->bScenarioTried = true;
        const FString File = UUCSettings::Get()->ResolveCargoScenario();
        if (File.IsEmpty()) return;
        FString Error;
        if (!FSCCargoScenario::Load(File, State->Scenario, Error))
        {
            UE_LOG(LogSCCargo, Error, TEXT("CARGO_SCENARIO failed: %s"), *Error);
            return;
        }
        for (const FString& Entity : State->Scenario.Entities()) State->Library.Request(Entity);
        if (State->Scenario.bSpace)
        {
            if (State->Scenario.SunLux >= 0) Viewer->SetSpaceSunLux(float(State->Scenario.SunLux));
            Viewer->SetSpace(true); // WP-K3: no hangar, no gravity, a sun
        }
        const FSCCargoDroneDefinition& D = State->Scenario.Drone;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_SCENARIO loaded=\"%s\" ship=%s environment=%s stock=%d docks=%d tractor=%s/%s max_force=%.0f full_strength_m=%.1f"),
               *State->Scenario.Name, *State->Scenario.Ship, *State->Scenario.Environment, State->Scenario.Stock.Num(), State->Scenario.Docks.Num(),
               *D.TractorRecord, *D.TractorAction, D.MaxForceN, D.FullStrengthDistanceM);
    }
    // No scenario: the default drone fleet once cargo is spawned live (SCCargoSpawnLive.cpp); from here on it is applied as
    // a scenario of docks only.
    if (!State->Scenario.IsSet() && State->bFleetRequested && !State->bFleetTried)
    {
        State->bFleetTried = true;
        LoadFleet();
    }
    const FSCCargoScenario& S = State->Scenario;
    if (!S.IsSet()) return;
    // The tractor values from the game data (the record the scenario names); the scenario's copy without the archive.
    if (State->TractorLookup == FSCCargoState::ELookup::None)
    {
        const TSharedPtr<FSCBridgeClient, ESPMode::ThreadSafe> Client = Viewer->GetClient();
        const FString Record = !S.Drone.TractorRecord.IsEmpty() ? S.Drone.TractorRecord : S.Drone.TractorRecordGuid;
        if (!Client || Record.IsEmpty())
        {
            State->TractorLookup = FSCCargoState::ELookup::Done;
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_TRACTOR source=scenario reason=\"%s\" max_force=%.0f"), Client ? TEXT("no record named") : TEXT("no archive (FFI)"),
                   S.Drone.MaxForceN);
        }
        else
        {
            State->TractorLookup = FSCCargoState::ELookup::Pending;
            TWeakObjectPtr<USCCargoSubsystem> Weak(this);
            Client->DataCoreRecord(Record, [Weak, Record](const FSCTextResult& R) {
                USCCargoSubsystem* Self = Weak.Get();
                if (!Self || !Self->State) return;
                FSCCargoDroneDefinition& D = Self->State->Scenario.Drone;
                FString Error = R.Error.ToString();
                if (R.bOK && D.ApplyTractorRecord(R.Json, Error))
                {
                    UE_LOG(LogSCCargo, Display, TEXT("CARGO_TRACTOR source=datacore record=%s action=%s max_force=%.0f min_distance_m=%.2f max_distance_m=%.1f full_strength_m=%.1f max_volume=%.1f volume_force_coefficient=%.2f max_angle=%.0f"),
                           *Record, *D.TractorAction, D.MaxForceN, D.MinDistanceM, D.MaxDistanceM, D.FullStrengthDistanceM, D.MaxVolume, D.VolumeForceCoefficient, D.MaxAngleDeg);
                }
                else
                {
                    UE_LOG(LogSCCargo, Warning, TEXT("CARGO_TRACTOR source=scenario record=%s reason=\"%s\" max_force=%.0f"), *Record, *Error, D.MaxForceN);
                }
                Self->State->TractorLookup = FSCCargoState::ELookup::Done;
            });
        }
    }
    if (State->TractorLookup != FSCCargoState::ELookup::Done) return; // stock and drones wait for the lift values
    if (!State->Fx)
    {
        // The tractor's effects (WP-K4c), from the record just read (none without the archive: the beam mesh then).
        State->Fx = MakeShared<FSCCargoFx>();
        if (FParse::Param(FCommandLine::Get(), TEXT("SCNoCargoFx")))
        {
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_FX off (-SCNoCargoFx): the beam mesh"));
        }
        else
        {
            State->Fx->Start(Viewer->GetClient(), S.Drone);
            float MinScale, MaxScale;
            State->Fx->Contact(S.Ship, MinScale, MaxScale); // resolved ahead, so the first knock plays too
        }
    }
    // Nothing loaded and no -SCLoad: the scenario's own ship and environment.
    FString Ignored;
    if (!State->bScenarioLoadRequested && !S.Ship.IsEmpty() && Viewer->GetLoadedEntity().IsEmpty() && Viewer->GetPendingEntity().IsEmpty() &&
        !FParse::Value(FCommandLine::Get(), TEXT("SCLoad="), Ignored) && !Viewer->IsBusy() && Viewer->CanLoad(S.Ship))
    {
        State->bScenarioLoadRequested = true;
        if (!S.Environment.IsEmpty() && Viewer->GetEnvironmentRequest().IsEmpty()) Viewer->SetEnvironment(S.Environment);
        Viewer->LoadEntity(S.Ship, Viewer->GetOptions());
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_SCENARIO loading ship=%s environment=%s"), *S.Ship, *S.Environment);
    }
    // Loose stock (WP-K2): put down once the rest is placed, at its poses or at free reachable spots from the scenario's seed.
    if (State->bScenarioPlaced && !State->bLoosePlaced && !Viewer->IsSpawningVehicles())
    {
        State->bLoosePlaced = true;
        FRandomStream Rng(S.Seed);
        for (const FSCCargoScenario::FLoose& L : S.Loose)
        {
            FString Error;
            const TArray<int32> Ids = AddLooseBoxes(L.Entity, L.Count, L.At.GetPtrOrNull(), &Rng, L.Commodity, L.CommodityLabel, L.Owner, Error);
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_SCENARIO loose=%s count=%d placed=%d%s"), *L.Entity, L.Count, Ids.Num(), Error.IsEmpty() ? TEXT("") : *(TEXT(" reason=\"") + Error + TEXT("\"")));
        }
    }
    if (State->AppliedKinds == 3 || Viewer->IsBusy() || !State->Library.IsSettled()) return;
    // Owners, and the scenario's other vehicles: more instances of its ship, spawned before any stock is placed.
    if (!State->bScenarioVehiclesRequested && Viewer->GetLoadedEntity().Equals(S.Ship, ESearchCase::IgnoreCase))
    {
        State->bScenarioVehiclesRequested = true;
        State->Player = S.Player;
        for (const auto& [Id, Label] : S.Owners) State->OwnerLabels.Add(Id, Label);
        State->ScenarioVehicleIds = {0};
        USCShipLinkSubsystem* ShipLink = USCShipLinkSubsystem::Get(this);
        if (ShipLink)
        {
            ShipLink->SetScenarioPlayer(S.Player);
            if (!S.VehicleOwners.IsEmpty()) ShipLink->SetOwner(0, S.VehicleOwners[0]);
        }
        for (int32 I = 1; I < S.VehicleOwners.Num(); ++I)
        {
            // Placed where the scenario says (in space: floating there), else on free floor beside the first.
            const TOptional<FTransform>& Pose = S.VehiclePoses.IsValidIndex(I) ? S.VehiclePoses[I] : TOptional<FTransform>();
            const int32 Id = Pose ? Viewer->SpawnVehicleInstance(S.Ship, *Pose, /*bRest=*/!S.bSpace) : Viewer->SpawnVehicleInstance(S.Ship);
            State->ScenarioVehicleIds.Add(Id);
            if (ShipLink) ShipLink->SetOwner(Id, S.VehicleOwners[I]);
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_SCENARIO vehicle=%d instance=%d owner=%s"), I, Id, *S.VehicleOwners[I].ToString());
        }
        // Obstacles (asteroids in space): entities placed as they are; drones and loads keep clear of them like any geometry.
        for (const FSCCargoScenario::FObstacle& O : S.Obstacles)
        {
            const int32 Id = Viewer->SpawnVehicleInstance(O.Entity, O.Pose, /*bRest=*/false);
            State->ObstacleIds.Add(Id);
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_SCENARIO obstacle=%s instance=%d at=%s"), *O.Entity, Id, *(O.Pose.GetLocation() / 100).ToCompactString());
        }
        // Grids found before the owners were known take them now.
        for (const auto& Pair : State->Grids)
            if (USCCargoGridComponent* G = Pair.Value.Get(); G && G->Kind == ESCCargoGridKind::Vehicle)
                G->Owner = ShipLink ? ShipLink->GetOwner(G->OwnerVehicle.Get()) : State->Player;
    }
    if (Viewer->IsSpawningVehicles()) return;
    if (!S.Ship.IsEmpty() && !Viewer->GetLoadedEntity().Equals(S.Ship, ESearchCase::IgnoreCase))
    {
        if (GetWorld()->GetTimeSeconds() - State->LastScenarioWait > 30)
        {
            State->LastScenarioWait = GetWorld()->GetTimeSeconds();
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_SCENARIO waiting: loaded ship %s is not the scenario's %s"), *Viewer->GetLoadedEntity(), *S.Ship);
        }
        return;
    }
    ApplyScenario();
}

void USCCargoSubsystem::ApplyScenario()
{
    const FSCCargoScenario& S = State->Scenario;
    for (const ESCCargoGridKind Kind : {ESCCargoGridKind::Vehicle, ESCCargoGridKind::Environment})
    {
        const uint8 Bit = 1u << uint8(Kind);
        if (State->AppliedKinds & Bit) continue;
        // Wait until the kind's grids exist (its actor is loaded); a scenario without entries of a kind is done for it.
        bool bAny = false, bReady = true;
        for (const FSCCargoDockSpec& D : S.Docks) if (D.Grid.Kind == Kind) { bAny = true; bReady &= SelectGrid(D.Grid) != nullptr; }
        for (const FSCCargoStock& St : S.Stock) if (St.Grid.Kind == Kind) { bAny = true; bReady &= SelectGrid(St.Grid) != nullptr; }
        if (bAny && !bReady) continue;
        // Stock and docks go only onto usable grids: a grid that extends is extended first (cargo rides only extended
        // grids, and a dock on a retracted plate would fold into the hull with its drones). The scenario stays
        // applying (activity cargo_scenario) until they are out.
        bool bExtended = true;
        for (const FSCCargoStock& St : S.Stock)
            if (USCCargoGridComponent* G = St.Grid.Kind == Kind ? SelectGrid(St.Grid) : nullptr; G && (!G->Movers.IsEmpty() || G->bAccessRule))
                bExtended &= RequestAccess(G, true, GetWorld()->GetTimeSeconds());
        // A dock grid with nothing to extend is usable once it is ready (Opening until then; a ship's hold is opened first):
        // waited for as well.
        for (const FSCCargoDockSpec& D : S.Docks)
            if (USCCargoGridComponent* G = D.Grid.Kind == Kind ? SelectGrid(D.Grid) : nullptr)
                bExtended &= G->Movers.IsEmpty() && !G->bAccessRule ? G->Access == ESCCargoGridAccess::Open : RequestAccess(G, true, GetWorld()->GetTimeSeconds());
        if (!bExtended) continue;
        State->AppliedKinds |= Bit;
        int32 Boxes = 0, Docks = 0;
        for (const FSCCargoDockSpec& D : S.Docks)
            if (D.Grid.Kind == Kind) { SpawnDock(SelectGrid(D.Grid), D.Drones); ++Docks; }
        for (const FSCCargoStock& St : S.Stock)
        {
            if (St.Grid.Kind != Kind) continue;
            USCCargoGridComponent* Grid = SelectGrid(St.Grid);
            // Largest boxes first: the best-fit packing the planner uses.
            TArray<const FSCCargoStockEntry*> Entries;
            for (const FSCCargoStockEntry& E : St.Boxes) Entries.Add(&E);
            auto Volume = [&](const FSCCargoStockEntry* E) {
                const FSCCargoBoxLibrary::FEntry* L = State->Library.Find(E->Entity);
                return L ? L->Occupancy.Dimensions.X * L->Occupancy.Dimensions.Y * L->Occupancy.Dimensions.Z : 0.0;
            };
            // Placed cells first (in file order: a column bottom up), then the rest largest first.
            Entries.StableSort([&](const FSCCargoStockEntry& A, const FSCCargoStockEntry& B) {
                return A.Origin.IsSet() != B.Origin.IsSet() ? A.Origin.IsSet() : !A.Origin.IsSet() && Volume(&A) > Volume(&B);
            });
            for (const FSCCargoStockEntry* E : Entries)
                for (int32 I = 0; I < E->Count; ++I)
                {
                    const int32 Id = SpawnBox(Grid, E->Entity, E->Commodity, E->CommodityLabel, E->Origin, E->bRotated);
                    if (Id == INDEX_NONE) continue;
                    ++Boxes;
                    State->Boxes[Id].Owner = !E->Owner.IsNone() ? E->Owner : !Grid->Owner.IsNone() ? Grid->Owner : State->Player;
                }
        }
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_SCENARIO applied kind=%s boxes=%d docks=%d drones=%d"),
               Kind == ESCCargoGridKind::Vehicle ? TEXT("vehicle") : TEXT("environment"), Boxes, Docks, State->Drones.Num());
    }
    State->bScenarioPlaced |= State->AppliedKinds == 3;
}

int32 USCCargoSubsystem::SpawnBox(USCCargoGridComponent* Grid, const FString& Entity, FName Commodity, const FText& Label, const TOptional<FIntVector>& Origin, bool bRotated,
                                  const TSet<int32>* NoSupport)
{
    const FSCCargoBoxLibrary::FEntry* Package = State->Library.Find(Entity);
    if (!Grid || !Package || Package->State != FSCCargoBoxLibrary::EState::Ready)
    {
        UE_LOG(LogSCCargo, Warning, TEXT("CARGO_SCENARIO box %s not placed: %s"), *Entity,
               !Grid ? TEXT("no grid") : Package ? *Package->Error : TEXT("package not requested"));
        return INDEX_NONE;
    }
    FSCCargoBox B;
    B.Id = State->NextItemId++;
    B.Entity = Entity;
    B.Commodity = Commodity;
    B.CommodityLabel = Label;
    B.Occupancy = Package->Occupancy;
    if (State->ScuVolumeM3 <= 0 && B.Occupancy.StandardCargoUnits > 0)
        State->ScuVolumeM3 = B.Occupancy.Dimensions.X * B.Occupancy.Dimensions.Y * B.Occupancy.Dimensions.Z / B.Occupancy.StandardCargoUnits;
    FSCCargoItem Item;
    Item.Id = B.Id;
    Item.SizeM = B.SizeM();
    Item.SCU = B.Occupancy.StandardCargoUnits;
    Item.SubType = B.Occupancy.ItemSubType;
    FSCCargoPlacement Slot;
    Slot.ItemId = Item.Id;
    bool bFits = false;
    if (Origin)
    {
        // The scenario's own cell (a stacked column): it must be free, supported and allowed like any other.
        bool bTurn = false;
        Slot.Origin = *Origin;
        Slot.bRotated = bRotated;
        Slot.Cells = Grid->Lattice.CellsFor(bRotated ? FVector(Item.SizeM.Y, Item.SizeM.X, Item.SizeM.Z) : Item.SizeM);
        const FSCCargoGridModel Model = Grid->Model();
        bFits = FSCCargoPlanner::FitsMaxItem(Grid->Lattice, Item.SizeM, bTurn) && Model.IsFree(Slot.Origin, Slot.Cells) &&
                Model.IsSupported(Slot.Origin, Slot.Cells, INDEX_NONE, NoSupport);
    }
    else bFits = FSCCargoPlanner::FindSlot(Grid->Model(), Item, Slot);
    if (!bFits)
    {
        UE_LOG(LogSCCargo, Warning, TEXT("CARGO_SCENARIO box %s does not fit grid %d (%s)%s"), *Entity, Grid->GridId, *Grid->GroupLabel.ToString(),
               Origin ? *FString::Printf(TEXT(" at %s"), *Origin->ToString()) : TEXT(""));
        return INDEX_NONE;
    }
    const FTransform World = Grid->SlotToWorld(Slot, Item.SizeM);
    ASCCargoContainer* A = SCCargoSpawnBoxActor(GetWorld(), *Package, Entity, World);
    if (!A) return INDEX_NONE;
    A->BoxId = B.Id;
    A->Lock(Grid, World);
    B.Actor = A;
    B.GridId = Grid->GridId;
    B.Slot = Slot;
    Grid->Placed.Add(Slot);
    State->Boxes.Add(B.Id, B);
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_BOX id=%d entity=%s scu=%d grid=%d origin=%s cells=%s rotated=%d world=%s"), B.Id, *Entity,
           Item.SCU, Grid->GridId, *Slot.Origin.ToString(), *Slot.Cells.ToString(), Slot.bRotated, *World.GetLocation().ToString());
    ++Revision;
    return B.Id;
}

void USCCargoSubsystem::SpawnDock(USCCargoGridComponent* Grid, int32 Drones)
{
    if (!Grid) return;
    // Only on an extended grid (a grid with nothing to extend is always so): retracted, the plate is folded into the
    // hull, and the dock and its drones would be inside it. Once placed, the grid cannot retract (CanClose).
    if (Grid->Access != ESCCargoGridAccess::Open)
    {
        UE_LOG(LogSCCargo, Warning, TEXT("CARGO_SCENARIO dock not placed on grid %d: the grid is not extended (access=%s)"), Grid->GridId,
               *StaticEnum<ESCCargoGridAccess>()->GetNameStringByValue(int64(Grid->Access)));
        return;
    }
    // A module of whole cells: it occupies them like any box (the lattice step is the minimum permitted item size); as
    // many per axis as its mesh spans (the two-bay dock: two cells long). A turned slot turns it (SlotToWorld), so its
    // long axis lies along the slot's.
    const FVector CellCm = Grid->Lattice.CellM * 100.0;
    const FIntVector Cells = ASCCargoDroneDock::MeshCells(CellCm);
    FSCCargoItem Item;
    Item.Id = State->NextItemId++;
    Item.SizeM = FVector(Cells) * Grid->Lattice.CellM;
    Item.bMovable = false;
    FSCCargoPlacement Slot;
    if (!FSCCargoPlanner::FindSlot(Grid->Model(), Item, Slot))
    {
        UE_LOG(LogSCCargo, Warning, TEXT("CARGO_SCENARIO dock does not fit grid %d"), Grid->GridId);
        return;
    }
    FTransform World = Grid->SlotToWorld(Slot, Item.SizeM);
    World.AddToTranslation(-World.GetRotation().GetUpVector() * Item.SizeM.Z * 50.0); // the cell floor
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ASCCargoDroneDock* Dock = GetWorld()->SpawnActor<ASCCargoDroneDock>(ASCCargoDroneDock::StaticClass(), World, Params);
    if (!Dock) return;
    Dock->ItemId = Item.Id;
    Dock->GridId = Grid->GridId;
    Dock->SetFootprint(Cells, CellCm);
    // Its bays open to one side: turned about the grid normal to face open space, the first turn whose bay exits are
    // all clear of the ship and the hangar (FSCCargoDockFootprint::Turns: quarter turns for a square footprint, half
    // turns for a long one, which keeps its cells).
    {
        FCollisionQueryParams ExitParams(SCENE_QUERY_STAT(SCCargoDockExit), true, Dock);
        FCollisionObjectQueryParams Types;
        for (const ECollisionChannel C : {ECC_WorldStatic, ECC_WorldDynamic, ECC_PhysicsBody, ECC_Vehicle, ECC_Destructible}) Types.AddObjectTypesToQuery(C);
        // The probe at each exit: the cell in front of the bay, in the dock's frame, from just above the dock's floor
        // (a sphere of the cell's size there reached into the floor and found no turn clear).
        const FQuat Base = World.GetRotation();
        bool bClear = false;
        FString Tried;
        for (const int32 Turn : FSCCargoDockFootprint::Turns(Cells))
        {
            Dock->SetActorRotation(Base * FQuat(FVector::UpVector, Turn * UE_HALF_PI));
            bClear = true;
            for (int32 Bay = 0; Bay < FMath::Max(1, Dock->NumBays()); ++Bay)
            {
                const FVector Exit = Dock->BayExit(Bay);
                const double Above = FVector::DotProduct(Exit - Dock->GetActorLocation(), Dock->GetActorUpVector());
                const FVector Half(CellCm.X / 2 - 2, CellCm.Y / 2 - 2, FMath::Max(1.0, FMath::Min(CellCm.Z / 2, Above) - 2));
                const bool bFree = !GetWorld()->OverlapAnyTestByObjectType(Exit, Dock->GetActorQuat(), Types, FCollisionShape::MakeBox(Half), ExitParams);
                Tried += FString::Printf(TEXT(" turn%d/bay%d=%s"), Turn, Bay, bFree ? TEXT("clear") : TEXT("blocked"));
                bClear &= bFree;
            }
            if (bClear) break;
        }
        if (!bClear) Dock->SetActorRotation(Base);
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_DOCK_EXITS grid=%d%s -> %s"), Grid->GridId, *Tried, bClear ? TEXT("turned") : TEXT("none clear: base turn"));
    }
    Dock->AttachToComponent(Grid, FAttachmentTransformRules::KeepWorldTransform);
    Slot.KeepClear = 1; // its bays open to the side and the drones rise out of it: no box against it
    Grid->Placed.Add(Slot);
    State->Docks.Add({Item.Id, Grid->GridId, Dock});
    const FSCCargoDroneDefinition& D = State->Scenario.Drone;
    for (int32 I = 0; I < Drones; ++I)
    {
        ASCCargoDrone* Drone = GetWorld()->SpawnActor<ASCCargoDrone>(ASCCargoDrone::StaticClass(), Dock->BayTransform(I), Params);
        if (Drone) Drone->Owner = Grid->Owner; // the dock's owner: a ship's own, the hangar's shared
        if (!Drone) continue;
        Drone->DroneId = State->NextDroneId++;
        Drone->DockItem = Item.Id;
        Drone->Bay = I;
        Drone->MaxSpeed = D.MaxSpeedMS * 100.f;
        Drone->MaxAccel = D.MaxAccelMS2 * 100.f;
        Drone->Place(Dock->BayTransform(I));
        Drone->AttachToActor(Dock, FAttachmentTransformRules::KeepWorldTransform);
        State->Drones.Add(Drone);
    }
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_DOCK item=%d grid=%d origin=%s cells=%s rotated=%d yaw=%.0f bays=%d drones=%d world=%s"), Item.Id, Grid->GridId,
           *Slot.Origin.ToString(), *Slot.Cells.ToString(), Slot.bRotated, Dock->GetActorRotation().Yaw, Dock->NumBays(), Drones, *World.GetLocation().ToString());
    ++Revision;
}

ASCCargoContainer* SCCargoSpawnBoxActor(UWorld* World, const FSCCargoBoxLibrary::FEntry& Package, const FString& Entity, const FTransform& Pose)
{
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ASCCargoContainer* A = World ? World->SpawnActor<ASCCargoContainer>(ASCCargoContainer::StaticClass(), Pose, Params) : nullptr;
    if (!A) return nullptr;
    const FSCItemOccupancy& O = Package.Occupancy;
    const FVector SizeM = FSCCargoLattice::FromSC(O.Dimensions);
    A->SetOccupancy(SizeM * 100.0, O.Mass);
    // The item's frame relative to the occupancy box centre: minus the local bounds' centre (SC axes -> lattice axes, cm).
    const FVector EntityOrigin = O.LocalBoundsMax != O.LocalBoundsMin ? -FSCCargoLattice::FromSC((O.LocalBoundsMin + O.LocalBoundsMax) / 2) * 100.0
                                                                      : FVector(0, 0, -SizeM.Z * 50.0);
    if (O.LocalBoundsMax == O.LocalBoundsMin)
        UE_LOG(LogSCCargo, Warning, TEXT("CARGO_BOX %s has no occupancy local bounds: its pivot is taken as the bottom centre"), *Entity);
    A->SetVisuals(Package.Template.Get(), EntityOrigin);
    return A;
}
