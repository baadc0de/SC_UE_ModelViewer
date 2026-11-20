// Slate's Ship Link selection. Live state and orders stay on ASCVehicle / USCShipLinkSubsystem.
#pragma once

#include "CoreMinimal.h"
#include "SCShipLinkSubsystem.h"

class FSCShipLinkModel
{
public:
    void Refresh();
    ASCVehicle* GetShip() const;
    USCShipLinkSubsystem* GetLink() const;
    const TArray<TWeakObjectPtr<ASCVehicle>>& GetFleet() const { return Fleet; }
    void SelectShip(ASCVehicle* Value);
    void SelectDoor(int32 Door);
    int32 GetDoor() const { return SelectedDoor; }
    int32 GetRevision() const { return Revision; }
    const FString& GetNotice() const { return Notice; }
    FString DoorRefusal(int32 Door) const { return Refusals.FindRef(Door); }
    void Order(int32 Door, ESCShipDoorAction Action);
    void OrderGroup(ESCShipDoorGroup Group, ESCShipDoorAction Action);
    void Secure();
    void Ramp();
    void Lights();
    void SetLight(int32 Group, const FString& Mode);
    void ToggleLocate();
    TMap<int32, FBox2D> CalloutHits;
    bool SelectCallout(const FVector2D& Absolute);
    static FString ShipLabel(const ASCVehicle* Value);
    static FString DoorStatus(const ASCVehicle* Value, int32 Door);
    static bool Listed(const ASCVehicle* Value, int32 Door);

private:
    TWeakObjectPtr<ASCVehicle> Ship;
    TArray<TWeakObjectPtr<ASCVehicle>> Fleet;
    TMap<int32, FString> Refusals;
    FString Notice;
    int32 SelectedDoor = INDEX_NONE;
    int32 Revision = 1;
};
