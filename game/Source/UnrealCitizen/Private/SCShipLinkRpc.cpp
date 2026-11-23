#include "SCShipLinkRpc.h"

#if SC_WITH_RPC
#include "SCFirstPersonCharacter.h"
#include "SCShipLinkSubsystem.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "SCViewerSubsystem.h"
#include "Engine/World.h"
#include "UI/Cargo/SCCargoViewSource.h"

using namespace SCRpcJson;

namespace
{
FString Text(const TSharedPtr<FJsonObject>& Body, const TCHAR* Key)
{
    FString Out;
    if (Body) Body->TryGetStringField(Key, Out);
    return Out;
}

FObj Reply(const FString& Error = {})
{
    FObj Out = MakeShared<FJsonObject>();
    Out->SetBoolField(TEXT("ok"), Error.IsEmpty());
    if (!Error.IsEmpty()) Out->SetStringField(TEXT("error"), Error);
    return Out;
}

bool Id(const TSharedPtr<FJsonObject>& Body, const TCHAR* Key, int32& Out)
{
    double Value = 0;
    if (!Body || !Body->TryGetNumberField(Key, Value) || !FMath::IsFinite(Value) ||
        Value < 0 || Value > MAX_int32 || FMath::TruncToDouble(Value) != Value) return false;
    Out = int32(Value);
    return true;
}
}

FObj SCShipLinkRpc::Snapshot(UWorld* World)
{
    FObj Out = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Pairings, Doors, Lights;
    const USCShipLinkSubsystem* ShipLink = USCShipLinkSubsystem::Get(World);
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World);
    if (ShipLink && Viewer)
        for (ASCVehicle* Ship : Viewer->GetVehicles())
        {
            const int32 ShipId = Viewer->GetVehicleInstanceId(Ship);
            const FName Owner = ShipLink->GetOwner(Ship);
            if (Owner.IsNone()) continue;
            FObj Pair = MakeShared<FJsonObject>();
            Pair->SetNumberField(TEXT("ship"), ShipId);
            Pair->SetStringField(TEXT("entity"), Ship->Manifest.Root.EntityName);
            Pair->SetStringField(TEXT("actor"), Owner.ToString());
            Pair->SetStringField(TEXT("role"), TEXT("Owner"));
            Pair->SetBoolField(TEXT("locating"), ShipLink->IsLocating(Ship));
            Pairings.Add(MakeShared<FJsonValueObject>(Pair));
            for (int32 I = 0; I < Ship->Doors.Num(); ++I)
            {
                const FSCDoorInteraction& Door = Ship->Doors[I];
                if (!Door.Rig || Ship->LiftDrivenRigs.Contains(Door.Rig.Get())) continue;
                const ASCVehicle::FShipDoor* Info = Ship->ShipDoors.IsValidIndex(I) ? &Ship->ShipDoors[I] : nullptr;
                FObj D = MakeShared<FJsonObject>();
                D->SetNumberField(TEXT("ship"), ShipId);
                D->SetNumberField(TEXT("id"), I);
                D->SetStringField(TEXT("label"), Ship->ShipDoorLabel(I));
                D->SetStringField(TEXT("group"), FSCShipDoorPolicy::GroupName(Info ? Info->Group : ESCShipDoorGroup::Other));
                D->SetStringField(TEXT("group_source"), Info && !Info->GroupSource.IsNone() ? Info->GroupSource.ToString() : TEXT("undecidable"));
                D->SetBoolField(TEXT("ramp"), Info && Info->bRamp);
                D->SetBoolField(TEXT("open"), Ship->IsDoorOpen(I));
                D->SetBoolField(TEXT("moving"), Ship->IsDoorPlaying(I));
                D->SetBoolField(TEXT("locked"), Ship->IsDoorLocked(I));
                D->SetBoolField(TEXT("securing"), Info && Info->bSecurePending);
                D->SetBoolField(TEXT("cargo_access"), Info && Info->bCargoRelock);
                TArray<FString> PadStates;
                for (const FSCScreenFace& Face : Ship->ScreenFaces)
                    if (const USCRigComponent* Rig = Ship->DoorForPanel(Face.Panel.Get()); Rig && Rig == Door.Rig.Get())
                        PadStates.AddUnique(Face.Shown);
                D->SetField(TEXT("pads"), Strings(PadStates));
                Doors.Add(MakeShared<FJsonValueObject>(D));
            }
            for (int32 I = 0; I < Ship->LightGroups.Num(); ++I)
            {
                const FSCLightGroup& Group = Ship->LightGroups[I];
                FObj L = MakeShared<FJsonObject>();
                L->SetNumberField(TEXT("ship"), ShipId);
                L->SetNumberField(TEXT("id"), I);
                L->SetStringField(TEXT("label"), Group.Name);
                L->SetStringField(TEXT("state"), Group.LightState);
                int32 Members = 0;
                for (const FSCImportedLightState& State : Ship->LightStates)
                    if (State.GroupEntityId == Group.EntityId && State.Interior == Group.Interior) ++Members;
                L->SetNumberField(TEXT("light_count"), Members);
                TArray<FString> Modes;
                for (const FSCLightGroupInteraction& Mode : Group.Interactions)
                    if (!Mode.LightState.IsEmpty() && !Mode.LinkingTag.IsEmpty()) Modes.AddUnique(Mode.LightState);
                L->SetField(TEXT("modes"), Strings(Modes));
                Lights.Add(MakeShared<FJsonValueObject>(L));
            }
        }
    Out->SetArrayField(TEXT("pairings"), Pairings);
    Out->SetArrayField(TEXT("doors"), Doors);
    Out->SetArrayField(TEXT("lights"), Lights);
    if (World == SCViewerShell::ViewerWorld())
    {
        const FSCMobiGlasRequests& Ui = FSCMobiGlasRequests::Get();
        const FObj State = MakeShared<FJsonObject>();
        State->SetBoolField(TEXT("ready"), Ui.bShipLinkReady);
        State->SetNumberField(TEXT("door_labels"), Ui.ShipLinkLabels);
        Out->SetObjectField(TEXT("ui"), State);
    }
    return Out;
}

FObj SCShipLinkRpc::Command(UWorld* World, const FString& Route, const TSharedPtr<FJsonObject>& Body)
{
    if (Route == TEXT("state"))
    {
        FObj Out = Reply();
        Out->SetObjectField(TEXT("ship"), Snapshot(World));
        return Out;
    }
    USCShipLinkSubsystem* Link = USCShipLinkSubsystem::Get(World);
    int32 Instance = 0;
    if (Body && Body->HasField(TEXT("ship")) && !Id(Body, TEXT("ship"), Instance)) return Reply(TEXT("ship must be an instance id"));
    ASCVehicle* Ship = Link ? Link->FindShip(Instance) : nullptr;
    if (!Ship) return Reply(TEXT("No such ship"));
    const FString As = Text(Body, TEXT("as"));
    const FName Actor = As.IsEmpty() ? Link->GetPlayer() : FName(*As);
    if (Route == TEXT("lights") || Route == TEXT("locate"))
    {
        if (Link->RoleOf(Actor, Ship) != ESCShipRole::Owner) return Reply(TEXT("Owner access required"));
        if (Route == TEXT("locate"))
        {
            Link->ToggleLocate(Ship);
            FObj Out = Reply();
            Out->SetBoolField(TEXT("locating"), Link->IsLocating(Ship));
            return Out;
        }
        bool bAll = false;
        if (Body) Body->TryGetBoolField(TEXT("all"), bAll);
        const FString Mode = Text(Body, TEXT("mode"));
        if (bAll)
        {
            FObj Out = Reply();
            Out->SetField(TEXT("skipped"), Ints(Link->SetAllLights(Ship, Mode == TEXT("toggle") ? FString() : Mode)));
            return Out;
        }
        int32 Group = INDEX_NONE;
        if (!Id(Body, TEXT("group"), Group)) return Reply(TEXT("group or all is required"));
        return Reply(Link->SetLight(Ship, Group, Mode));
    }
    TArray<FSCShipDoorResult> Results;
    if (Route == TEXT("secure")) Results = Ship->SecureShip(Actor);
    else
    {
        const FString Verb = Text(Body, TEXT("action")).ToLower();
        ESCShipDoorAction Action;
        if (Verb == TEXT("open")) Action = ESCShipDoorAction::Open;
        else if (Verb == TEXT("close")) Action = ESCShipDoorAction::Close;
        else if (Verb == TEXT("lock")) Action = ESCShipDoorAction::Lock;
        else if (Verb == TEXT("unlock")) Action = ESCShipDoorAction::Unlock;
        else return Reply(TEXT("Unknown door action"));
        bool bLocal = false;
        if (Body) Body->TryGetBoolField(TEXT("local"), bLocal);
        TOptional<FVector> LocalUse;
        if (bLocal && Actor == Link->GetPlayer())
            if (const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World))
                if (const ASCFirstPersonCharacter* Player = Viewer->GetFirstPerson()) LocalUse = Player->GetActorLocation();
        auto Order = [&](int32 I) { Results.Add({I, Link->OrderDoor(Ship, I, Action, Actor, LocalUse), Ship->IsDoorPlaying(I)}); };
        int32 Door = INDEX_NONE;
        if (Id(Body, TEXT("door"), Door)) Order(Door);
        else
        {
            const FString Group = Text(Body, TEXT("group"));
            for (int32 I = 0; I < Ship->ShipDoors.Num(); ++I)
                if (Group.Equals(FSCShipDoorPolicy::GroupName(Ship->ShipDoors[I].Group), ESearchCase::IgnoreCase) &&
                    !Ship->LiftDrivenRigs.Contains(Ship->Doors[I].Rig.Get())) Order(I);
            if (Results.IsEmpty()) return Reply(TEXT("No matching door or group"));
        }
    }
    const FSCShipDoorResult* Refused = Results.FindByPredicate([](const FSCShipDoorResult& D) { return !D.Refusal.IsEmpty(); });
    // One door failure is an error; group commands return every outcome even when a member refuses.
    FObj Out = Reply(Refused && Results.Num() == 1 ? Refused->Refusal : FString());
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FSCShipDoorResult& Door : Results)
    {
        FObj D = MakeShared<FJsonObject>();
        D->SetNumberField(TEXT("door"), Door.Door);
        D->SetBoolField(TEXT("pending"), Door.bPending);
        if (!Door.Refusal.IsEmpty()) D->SetStringField(TEXT("error"), Door.Refusal);
        Rows.Add(MakeShared<FJsonValueObject>(D));
    }
    Out->SetArrayField(TEXT("doors"), Rows);
    return Out;
}

void SCShipLinkRpc::Scan(UWorld* World, TSet<FString>& Found)
{
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World);
    if (!Viewer) return;
    for (const ASCVehicle* Ship : Viewer->GetVehicles())
    {
        for (int32 Door = 0; Door < Ship->ShipDoors.Num(); ++Door)
            if (Ship->ShipDoors[Door].bActorOpenedWithoutAccess)
                Found.Add(FString::Printf(TEXT("door_open_while_locked_by_actor:%d:%d"), Viewer->GetVehicleInstanceId(Ship), Door));
        for (const FSCScreenFace& Face : Ship->ScreenFaces)
        {
            if (!Face.Pinned.IsEmpty() || Face.Custom.IsValid() || !Face.Component.IsValid()) continue;
            const USCRigComponent* Rig = Ship->DoorForPanel(Face.Panel.Get());
            if (!Rig) continue;
            const int32 Door = Ship->DoorForRig(Rig);
            const FString Locked = TEXT("DoorLockState.DoorLocked");
            if (Face.States.Contains(Locked) && Face.Shown.Contains(Locked) != Ship->IsDoorLocked(Door))
                Found.Add(FString::Printf(TEXT("pad_lock_mismatch:%d:%d"), Viewer->GetVehicleInstanceId(Ship), Door));
        }
    }
}
#endif
