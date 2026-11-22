// WP-O1-2: UnrealCitizen's part of the RPC harness (SCRpcExtension.h): the cargo routes, state, activity, anomalies,
// catalogue and job waits (WP-H1, WP-K1/K2), and the Ship Link (WP-M1) and Garage (WP-M2) routes and state.
#include "UCRpcRoutes.h"
#include "SCRpcExtension.h"

#if SC_WITH_RPC
#include "SCRpcJson.h"
#include "SCShipLinkRpc.h"
#include "SCGarageRpc.h"
#include "Cargo/SCCargoSubsystem.h"
#include "Engine/World.h"

using namespace SCRpcJson;
using namespace SCRpcArgs;

namespace
{
USCCargoSubsystem* Cargo(UWorld* World) { return USCCargoSubsystem::Get(World); }

int32 ResolveGrid(const USCCargoSubsystem* C, const TSharedPtr<FJsonObject>& B, const TCHAR* Key, FString& OutError)
{
    const FString Sel = Str(B, Key);
    if (!C || Sel.IsEmpty()) { OutError = FString::Printf(TEXT("%s: a grid id or selector is needed"), Key); return INDEX_NONE; }
    const TArray<int32> Ids = C->ResolveGrids(Sel, FName(*Str(B, TEXT("owner"))));
    if (Ids.IsEmpty()) { OutError = FString::Printf(TEXT("no grid matches \"%s\""), *Sel); return INDEX_NONE; }
    return Ids[0];
}

void CargoPackage(const TSharedPtr<FJsonObject>& B, ISCRpcHost& Host, const ISCRpcHost::FReply& Reply, bool bSpawn)
{
    USCCargoSubsystem* C = Cargo(Host.World());
    const FString Entity = Str(B, TEXT("entity"));
    if (!C || Entity.IsEmpty()) return Reply(RpcError(TEXT("entity is needed")));
    FString Err;
    int32 Grid = INDEX_NONE;
    // Loose (WP-K2): at a point (metres, yaw) or at random free reachable floor spots (seed); else onto a grid.
    FVector At;
    const bool bAt = Vec(B, TEXT("at"), At);
    const bool bLoose = bAt || Flag(B, TEXT("random"), false);
    const TOptional<FTransform> Pose = bAt ? TOptional<FTransform>(FTransform(FRotator(0, Num(B, TEXT("yaw"), 0), 0), At)) : TOptional<FTransform>();
    const TSharedRef<FRandomStream> Rng = MakeShared<FRandomStream>(int32(Num(B, TEXT("seed"), FMath::Rand())));
    if (bSpawn && !bLoose && (Grid = ResolveGrid(C, B, TEXT("grid"), Err)) == INDEX_NONE) return Reply(RpcError(Err));
    const int32 Count = FMath::Clamp(int32(Num(B, TEXT("count"), 1)), 1, 500);
    const FString Commodity = Str(B, TEXT("commodity"), TEXT("Test"));
    const FText Label = FText::FromString(Str(B, TEXT("label"), Commodity));
    const FName Owner(*Str(B, TEXT("owner")));
    if (C->RequestBoxPackage(Entity, Err) < 0) return Reply(RpcError(Err));
    // The package exports and imports once (it may take a while the first time): answer when it is ready.
    TWeakObjectPtr<UWorld> World = Host.World();
    Host.Defer(bSpawn ? TEXT("/cargo/spawn") : TEXT("/cargo/package"), Num(B, TEXT("timeout_s"), 300),
               [World, Entity, Grid, Count, Commodity, Label, Owner, bSpawn, bLoose, Pose, Rng](bool) -> TSharedPtr<FJsonObject> {
                   USCCargoSubsystem* Cg = Cargo(World.Get());
                   if (!Cg) return RpcError(TEXT("no cargo subsystem"));
                   FString E;
                   const int32 State = Cg->RequestBoxPackage(Entity, E);
                   if (State < 0) return RpcError(E);
                   if (State == 0) return nullptr;
                   FObj O = RpcOk();
                   if (!bSpawn) return O;
                   const TArray<int32> Ids = bLoose ? Cg->AddLooseBoxes(Entity, Count, Pose.GetPtrOrNull(), &Rng.Get(), FName(*Commodity), Label, Owner, E)
                                                    : Cg->AddBoxes(Grid, Entity, Count, FName(*Commodity), Label, Owner, E);
                   O->SetField(TEXT("boxes"), Ints(Ids));
                   if (!E.IsEmpty()) O->SetStringField(TEXT("warning"), E);
                   O->SetBoolField(TEXT("ok"), !Ids.IsEmpty());
                   if (Ids.IsEmpty()) O->SetStringField(TEXT("error"), E);
                   return O;
               });
}

void CargoRemove(const TSharedPtr<FJsonObject>& B, ISCRpcHost& Host, const ISCRpcHost::FReply& Reply)
{
    USCCargoSubsystem* C = Cargo(Host.World());
    if (!C) return Reply(RpcError(TEXT("no cargo subsystem")));
    const bool bForce = Flag(B, TEXT("force"), false);
    // box: one id; grid: every box on the grids that match; all: every box.
    TArray<int32> Targets;
    const TSharedRef<FJsonObject> S = C->StateJson();
    TArray<int32> Grids;
    if (B->HasField(TEXT("grid")))
    {
        Grids = C->ResolveGrids(Str(B, TEXT("grid")), FName(*Str(B, TEXT("owner"))));
        if (Grids.IsEmpty()) return Reply(RpcError(TEXT("no grid matches")));
    }
    if (B->HasField(TEXT("box"))) Targets.Add(int32(Num(B, TEXT("box"), -1)));
    else
        for (const TSharedPtr<FJsonValue>& V : S->GetArrayField(TEXT("boxes")))
        {
            const TSharedPtr<FJsonObject> Box = V->AsObject();
            if (Flag(B, TEXT("all"), false) || Grids.Contains(int32(Num(Box, TEXT("grid"), -1))) || Grids.Contains(int32(Num(Box, TEXT("beacon"), -1))))
                Targets.Add(int32(Num(Box, TEXT("id"), -1)));
        }
    if (Targets.IsEmpty()) return Reply(RpcError(TEXT("nothing to remove (box, grid or all)")));
    TArray<int32> Removed;
    TArray<FString> Errors;
    for (const int32 Id : Targets)
    {
        if (Removed.Contains(Id)) continue;
        FString Err;
        if (!C->RemoveBox(Id, bForce, Removed, Err)) Errors.Add(Err);
    }
    FObj O = RpcOk();
    O->SetField(TEXT("removed"), Ints(Removed));
    O->SetField(TEXT("refused"), Strings(Errors));
    O->SetBoolField(TEXT("ok"), !Removed.IsEmpty());
    if (Removed.IsEmpty()) O->SetStringField(TEXT("error"), Errors.IsEmpty() ? TEXT("nothing removed") : Errors[0]);
    Reply(O);
}

void Transfer(const TSharedPtr<FJsonObject>& B, ISCRpcHost& Host, const ISCRpcHost::FReply& Reply)
{
    USCCargoSubsystem* C = Cargo(Host.World());
    if (!C) return Reply(RpcError(TEXT("no cargo subsystem")));
    FSCCargoTransferRequest R;
    R.Requester = FName(*Str(B, TEXT("owner")));
    R.FromGrids = C->ResolveGrids(Str(B, TEXT("from")), R.Requester);
    R.ToGrids = C->ResolveGrids(Str(B, TEXT("to")), R.Requester);
    R.Commodity = C->ResolveCommodity(Str(B, TEXT("commodity")));
    R.AmountSCU = int32(Num(B, TEXT("scu"), 0));
    // Each missing part named (a beacon emptied since the caller looked is the usual one).
    TArray<FString> Missing;
    if (R.FromGrids.IsEmpty()) Missing.Add(FString::Printf(TEXT("from: no grid or beacon matches '%s'"), *Str(B, TEXT("from"))));
    if (R.ToGrids.IsEmpty()) Missing.Add(FString::Printf(TEXT("to: no grid matches '%s'"), *Str(B, TEXT("to"))));
    if (R.Commodity.IsNone()) Missing.Add(FString::Printf(TEXT("commodity: '%s' is on no grid or beacon"), *Str(B, TEXT("commodity"))));
    if (R.AmountSCU <= 0) Missing.Add(TEXT("scu > 0 is needed"));
    if (!Missing.IsEmpty()) return Reply(RpcError(FString::Join(Missing, TEXT("; "))));
    const FSCCargoTransferPreview P = C->Preview(R);
    FObj O = RpcOk();
    O->SetNumberField(TEXT("planned_scu"), P.PlannedSCU);
    O->SetNumberField(TEXT("moves"), P.Moves.Num());
    TArray<FString> Refusals;
    for (const FSCCargoRefusal& F : P.Refusals) Refusals.Add(F.Reason.ToString());
    O->SetField(TEXT("refusals"), Strings(Refusals));
    if (Flag(B, TEXT("preview"), false)) return Reply(O);
    FText Err;
    const int32 Job = C->Submit(R, Err);
    O->SetNumberField(TEXT("job"), Job);
    if (Job == INDEX_NONE)
    {
        O->SetBoolField(TEXT("ok"), false);
        O->SetStringField(TEXT("error"), Err.ToString());
    }
    Reply(O);
}

/** Cargo (WP-H1 for WP-K1/K2): /cargo routes, the "cargo" state section, drones and jobs as activity, box and drone
 *  anomalies, the box catalogue and /wait until=job. */
class FCargoRpc final : public ISCRpcExtension
{
public:
    void Routes(TMap<FString, FRoute>& Out) override
    {
        Out.Add(TEXT("/cargo/package"), [](auto& B, ISCRpcHost& H, auto& Reply) { CargoPackage(B, H, Reply, false); });
        Out.Add(TEXT("/cargo/spawn"), [](auto& B, ISCRpcHost& H, auto& Reply) { CargoPackage(B, H, Reply, true); });
        Out.Add(TEXT("/cargo/remove"), [](auto& B, ISCRpcHost& H, auto& Reply) { CargoRemove(B, H, Reply); });
        Out.Add(TEXT("/cargo/dock"), [](auto& B, ISCRpcHost& H, auto& Reply) {
            USCCargoSubsystem* C = Cargo(H.World());
            FString Err;
            const int32 Grid = ResolveGrid(C, B, TEXT("grid"), Err);
            if (Grid == INDEX_NONE) return Reply(RpcError(Err));
            if (!C->AddDock(Grid, int32(Num(B, TEXT("drones"), 2)), Err)) return Reply(RpcError(Err));
            Reply(RpcOk());
        });
        Out.Add(TEXT("/cargo/transfer"), [](auto& B, ISCRpcHost& H, auto& Reply) { Transfer(B, H, Reply); });
        Out.Add(TEXT("/cargo/cancel"), [](auto& B, ISCRpcHost& H, auto& Reply) {
            USCCargoSubsystem* C = Cargo(H.World());
            if (!C || !C->Cancel(int32(Num(B, TEXT("job"), -1)))) return Reply(RpcError(TEXT("no active job with that id")));
            Reply(RpcOk());
        });
        Out.Add(TEXT("/cargo/access"), [](auto& B, ISCRpcHost& H, auto& Reply) {
            USCCargoSubsystem* C = Cargo(H.World());
            FString Err;
            const int32 Grid = ResolveGrid(C, B, TEXT("grid"), Err);
            if (Grid == INDEX_NONE) return Reply(RpcError(Err));
            FText Why;
            if (!C->RequestGridAccess(Grid, Flag(B, TEXT("open"), true), Why)) return Reply(RpcError(Why.ToString()));
            Reply(RpcOk());
        });
    }
    void Snapshot(UWorld* World, FJsonObject& State) const override
    {
        if (const USCCargoSubsystem* C = Cargo(World)) State.SetObjectField(TEXT("cargo"), C->StateJson());
    }
    void Activity(UWorld* World, TArray<FString>& Reasons, double& MaxSpeedCmS) const override
    {
        if (const USCCargoSubsystem* C = Cargo(World))
        {
            Reasons.Append(C->ActivityReasons());
            MaxSpeedCmS = FMath::Max(MaxSpeedCmS, double(C->MaxSpeedCmS()));
        }
    }
    void Scan(UWorld* World, const FJsonObject& S, TSet<FString>& Found) const override
    {
        // Stored boxes off their slots, boxes without actors or simulated while stored, drones left hovering; positions
        // that are not finite or far below the world.
        auto Check = [&](const TCHAR* Kind, const TSharedPtr<FJsonObject>& O) {
            const FString Id = Str(O, TEXT("id"));
            const TArray<TSharedPtr<FJsonValue>>* P;
            if (!O->TryGetArrayField(TEXT("pos"), P) || P->Num() < 3) return;
            for (const TSharedPtr<FJsonValue>& V : *P)
                if (V->Type != EJson::Number) { Found.Add(FString::Printf(TEXT("nan:%s:%s"), Kind, *Id)); return; }
            if ((*P)[2]->AsNumber() < -1000) Found.Add(FString::Printf(TEXT("fell:%s:%s"), Kind, *Id));
        };
        const TSharedPtr<FJsonObject>* CargoObj;
        if (!S.TryGetObjectField(TEXT("cargo"), CargoObj)) return;
        const TSharedPtr<FJsonObject>& C = *CargoObj;
        const TArray<TSharedPtr<FJsonValue>>* Arr;
        if (C->TryGetArrayField(TEXT("boxes"), Arr))
            for (const TSharedPtr<FJsonValue>& V : *Arr)
            {
                const TSharedPtr<FJsonObject> B = V->AsObject();
                Check(TEXT("box"), B);
                const FString Id = Str(B, TEXT("id")), State = Str(B, TEXT("state"));
                if (!Flag(B, TEXT("actor"), true)) Found.Add(TEXT("box_no_actor:") + Id);
                if (Num(B, TEXT("slot_err_cm"), 0) > 5) Found.Add(TEXT("box_off_slot:") + Id);
                const bool bOnGrid = Num(B, TEXT("grid"), -1) >= 0;
                if (bOnGrid && State == TEXT("Stored") && Flag(B, TEXT("sim"), false)) Found.Add(TEXT("box_simulated_on_grid:") + Id);
                // A stored box has a grid; a loose one at rest (frozen) a beacon (WP-K2).
                const bool bInBeacon = Num(B, TEXT("beacon"), -1) >= 0;
                if (!bOnGrid && State == TEXT("Stored")) Found.Add(TEXT("box_stored_without_grid:") + Id);
                if (!bOnGrid && !bInBeacon && State == TEXT("Reserved")) Found.Add(TEXT("box_reserved_nowhere:") + Id);
                if (State == TEXT("Loose") && !bInBeacon && !Flag(B, TEXT("sim"), false)) Found.Add(TEXT("box_unbeaconed:") + Id);
            }
        if (C->TryGetArrayField(TEXT("drones"), Arr))
            for (const TSharedPtr<FJsonValue>& V : *Arr)
            {
                const TSharedPtr<FJsonObject> D = V->AsObject();
                Check(TEXT("drone"), D);
                // Out of its bay with no move to fly it: it would hover there for good.
                if (Str(D, TEXT("state")) != TEXT("Docked") && !Flag(D, TEXT("in_move"), true)) Found.Add(TEXT("drone_orphaned:") + Str(D, TEXT("id")));
            }
    }
    void Catalog(UWorld* World, FJsonObject& Out) const override
    {
        if (const USCCargoSubsystem* C = Cargo(World))
            for (const auto& [Key, Value] : C->CatalogJson()->Values) Out.SetField(Key, Value);
    }
    bool Wait(UWorld* World, const FString& Until, const TSharedPtr<FJsonObject>& B, bool& bReached, FJsonObject& Out, FString& OutError) const override
    {
        if (Until != TEXT("job")) return false;
        const int32 Job = int32(Num(B, TEXT("job"), -1));
        ESCCargoJobState State;
        FText Reason;
        const USCCargoSubsystem* C = Cargo(World);
        if (!C || !C->FindJobState(Job, State, Reason))
        {
            OutError = FString::Printf(TEXT("no job %d"), Job);
            return true;
        }
        bReached = State == ESCCargoJobState::Done || State == ESCCargoJobState::Failed || State == ESCCargoJobState::Cancelled;
        Out.SetStringField(TEXT("job_state"), StaticEnum<ESCCargoJobState>()->GetNameStringByValue(int64(State)));
        if (!Reason.IsEmpty()) Out.SetStringField(TEXT("job_reason"), Reason.ToString());
        return true;
    }
};

/** Ship Link (WP-M1): /ship/state|door|secure|lights|locate, the "ship" state section and its anomalies. */
class FShipLinkRpc final : public ISCRpcExtension
{
public:
    void Routes(TMap<FString, FRoute>& Out) override
    {
        for (const TCHAR* Route : {TEXT("state"), TEXT("door"), TEXT("secure"), TEXT("lights"), TEXT("locate")})
            Out.Add(FString(TEXT("/ship/")) + Route, [Name = FString(Route)](auto& B, ISCRpcHost& H, auto& Reply) {
                Reply(SCShipLinkRpc::Command(H.World(), Name, B));
            });
    }
    void Snapshot(UWorld* World, FJsonObject& State) const override { State.SetObjectField(TEXT("ship"), SCShipLinkRpc::Snapshot(World)); }
    void Scan(UWorld* World, const FJsonObject&, TSet<FString>& Found) const override { SCShipLinkRpc::Scan(World, Found); }
};

/** Garage (WP-M2): /garage/fleet|retrieve|store|paint and the "garage" state section. */
class FGarageRpc final : public ISCRpcExtension
{
public:
    void Routes(TMap<FString, FRoute>& Out) override
    {
        for (const TCHAR* Route : {TEXT("fleet"), TEXT("retrieve"), TEXT("store"), TEXT("paint")})
            Out.Add(FString(TEXT("/garage/")) + Route, [Name = FString(Route)](auto& B, ISCRpcHost& H, auto& Reply) {
                Reply(SCGarageRpc::Command(H.World(), Name, B));
            });
    }
    void Snapshot(UWorld* World, FJsonObject& State) const override { State.SetObjectField(TEXT("garage"), SCGarageRpc::Snapshot(World)); }
};
} // namespace

#endif // SC_WITH_RPC

namespace UCRpcRoutes
{
void Register()
{
#if SC_WITH_RPC
    SCRpcExtensions::Add(TEXT("Cargo"), MakeShared<FCargoRpc>());
    SCRpcExtensions::Add(TEXT("ShipLink"), MakeShared<FShipLinkRpc>());
    SCRpcExtensions::Add(TEXT("Garage"), MakeShared<FGarageRpc>());
#endif
}
void Unregister()
{
    for (const TCHAR* Name : {TEXT("Cargo"), TEXT("ShipLink"), TEXT("Garage")}) SCRpcExtensions::Remove(Name);
}
} // namespace UCRpcRoutes
