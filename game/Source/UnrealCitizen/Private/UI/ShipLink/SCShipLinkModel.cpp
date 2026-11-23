#include "SCShipLinkModel.h"
#include "../Cargo/SCCargoViewSource.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "Engine/World.h"

ASCVehicle* FSCShipLinkModel::GetShip() const { return Ship.Get(); }

USCShipLinkSubsystem* FSCShipLinkModel::GetLink() const
{
    return USCShipLinkSubsystem::Get(SCViewerShell::ViewerWorld());
}

void FSCShipLinkModel::Refresh()
{
    const USCShipLinkSubsystem* Link = GetLink();
    TArray<TWeakObjectPtr<ASCVehicle>> Available;
    if (Link)
        for (ASCVehicle* Value : Link->PairedShips(Link->GetPlayer())) Available.Add(Value);
    if (Available == Fleet) return;
    Fleet = MoveTemp(Available);
    if (!Ship.IsValid() || !Fleet.Contains(Ship)) SelectShip(Fleet.IsEmpty() ? nullptr : Fleet[0].Get());
    ++Revision;
}

void FSCShipLinkModel::SelectShip(ASCVehicle* Value)
{
    if (Value && !Fleet.Contains(TWeakObjectPtr<ASCVehicle>(Value))) return;
    Ship = Value;
    SelectedDoor = INDEX_NONE;
    Refusals.Reset();
    Notice.Reset();
    ++Revision;
}

void FSCShipLinkModel::SelectDoor(int32 Door)
{
    if (Listed(GetShip(), Door)) SelectedDoor = Door;
}

bool FSCShipLinkModel::SelectCallout(const FVector2D& Absolute)
{
    for (const auto& [Door, Rect] : CalloutHits)
        if (Rect.IsInsideOrOn(Absolute)) { SelectDoor(Door); return true; }
    return false;
}

bool FSCShipLinkModel::Listed(const ASCVehicle* Value, int32 Door)
{
    return Value && Value->Doors.IsValidIndex(Door) && Value->Doors[Door].Rig &&
        !Value->IsLiftDriven(Value->Doors[Door].Rig.Get());
}

FString FSCShipLinkModel::ShipLabel(const ASCVehicle* Value)
{
    if (!Value) return TEXT("No paired ship");
    const FSCSceneInstance& Root = Value->Manifest.Root;
    if (!Root.DisplayName.IsEmpty()) return Root.DisplayName;
    FString Entity = Root.EntityName;
    FString Prefix, Suffix;
    if (Entity.Split(TEXT("."), &Prefix, &Suffix, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) Entity = MoveTemp(Suffix);
    return Entity.Replace(TEXT("_"), TEXT(" "));
}

FString FSCShipLinkModel::DoorStatus(const ASCVehicle* Value, int32 Door)
{
    if (!Listed(Value, Door)) return {};
    if (Value->IsDoorPlaying(Door)) return Value->IsDoorOpen(Door) ? TEXT("Opening…") : TEXT("Closing…");
    if (Value->IsDoorLocked(Door)) return Value->IsDoorOpen(Door) ? TEXT("Open · Locked") : TEXT("Locked");
    return Value->IsDoorOpen(Door) ? TEXT("Open") : TEXT("Closed");
}

void FSCShipLinkModel::Order(int32 Door, ESCShipDoorAction Action)
{
    ASCVehicle* Value = GetShip();
    if (!Listed(Value, Door)) return;
    const FString Why = Value->OrderShipDoor(Door, Action, Value->ShipLinkPlayer());
    if (Why.IsEmpty()) Refusals.Remove(Door);
    else Refusals.Add(Door, Why);
}

void FSCShipLinkModel::OrderGroup(ESCShipDoorGroup Group, ESCShipDoorAction Action)
{
    if (ASCVehicle* Value = GetShip())
        for (int32 Door = 0; Door < Value->ShipDoors.Num(); ++Door)
            if (Value->ShipDoors[Door].Group == Group) Order(Door, Action);
}

void FSCShipLinkModel::Secure()
{
    if (USCShipLinkSubsystem* Link = GetLink())
    {
        const TArray<FSCShipDoorResult> Results = Link->Secure(GetShip());
        Notice = Results.IsEmpty() ? TEXT("No exterior doors identified") : FString();
        for (const FSCShipDoorResult& Result : Results)
            if (Result.Refusal.IsEmpty()) Refusals.Remove(Result.Door);
            else Refusals.Add(Result.Door, Result.Refusal);
    }
}

void FSCShipLinkModel::Ramp()
{
    ASCVehicle* Value = GetShip();
    if (!Value) return;
    TArray<int32> Ramps;
    for (int32 Id = 0; Id < Value->ShipDoors.Num(); ++Id)
        if (Value->ShipDoors[Id].bRamp && Value->ShipDoors[Id].Group == ESCShipDoorGroup::Exterior && Listed(Value, Id))
            Ramps.Add(Id);
    const bool bClose = Ramps.ContainsByPredicate([&](int32 Door) { return Value->IsDoorOpen(Door); });
    for (int32 Door : Ramps) Order(Door, bClose ? ESCShipDoorAction::Close : ESCShipDoorAction::Open);
}

void FSCShipLinkModel::Lights()
{
    const int32 Skipped = USCShipLinkSubsystem::SetAllLights(GetShip(), FString()).Num();
    Notice = Skipped ? FString::Printf(TEXT("%d light groups have no on/off pair"), Skipped) : FString();
}

void FSCShipLinkModel::SetLight(int32 Group, const FString& Mode)
{
    Notice = USCShipLinkSubsystem::SetLight(GetShip(), Group, Mode);
}

void FSCShipLinkModel::SetThrusterPreview(ESCThrusterPreviewMode Mode)
{
    if (ASCVehicle* Vehicle = GetShip())
        Notice = Vehicle->SetThrusterPreview(Mode) ? FString() : TEXT("Some exhaust assets need a local build.");
}

void FSCShipLinkModel::ToggleLocate()
{
    if (USCShipLinkSubsystem* Link = GetLink()) Link->ToggleLocate(GetShip());
}
