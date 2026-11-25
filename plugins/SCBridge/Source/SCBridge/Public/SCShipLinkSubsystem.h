// Ship ownership and the shared policy for door orders from the HUD, sensors, mobiGlas and RPC.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "SCShipLinkSubsystem.generated.h"

class ASCVehicle;
class USCViewerSubsystem;

enum class ESCShipRole : uint8 { None, Owner, Crew, Guest };
enum class ESCShipDoorAction : uint8 { Open, Close, Lock, Unlock };
UENUM()
enum class ESCShipDoorGroup : uint8 { Exterior, Interior, Other };

struct SCBRIDGE_API FSCShipDoorPolicy
{
    static ESCShipDoorGroup Classify(bool bHasRooms, const FString& SideA, const FString& SideB);
    static FString Refusal(ESCShipRole Role, ESCShipDoorAction Action, bool bLocked, ESCShipDoorGroup Group, bool bInside);
    static const TCHAR* GroupName(ESCShipDoorGroup Group);
};

struct FSCShipDoorResult
{
    int32 Door = INDEX_NONE;
    FString Refusal;
    bool bPending = false;
};

UCLASS()
class SCBRIDGE_API USCShipLinkSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    static USCShipLinkSubsystem* Get(const UObject* WorldContext);
    virtual void OnWorldBeginPlay(UWorld& InWorld) override;
    virtual void Deinitialize() override;

    FName GetPlayer() const { return Player; }
    void SetScenarioPlayer(FName Actor);
    void SetOwner(int32 VehicleInstance, FName Actor);
    /** The instance is nobody's ship (a cargo obstacle such as an asteroid): no owner, not paired, not the player's by
     *  default. Like an explicit owner, it is dropped when the instance's vehicle is replaced. */
    void SetUnowned(int32 VehicleInstance);
    FName GetOwner(const ASCVehicle* Vehicle) const;
    ESCShipRole RoleOf(FName Actor, const ASCVehicle* Vehicle) const;
    TArray<ASCVehicle*> PairedShips(FName Actor) const;
    ASCVehicle* FindShip(int32 VehicleInstance) const;

    static FString OrderDoor(ASCVehicle* Ship, int32 Door, ESCShipDoorAction Action, FName Actor,
        const TOptional<FVector>& LocalUse = {});
    TArray<FSCShipDoorResult> Secure(ASCVehicle* Ship);
    static FString SetLight(ASCVehicle* Ship, int32 Group, const FString& Mode);
    /** Set the eligible groups together. Empty Mode toggles them based on whether any is on. */
    static TArray<int32> SetAllLights(ASCVehicle* Ship, const FString& Mode);

    bool IsLocating(const ASCVehicle* Ship) const;
    ASCVehicle* GetLocatedShip() const;
    void ToggleLocate(ASCVehicle* Ship);

private:
    struct FPairing
    {
        TWeakObjectPtr<ASCVehicle> Vehicle;
        FName ExplicitOwner;
        bool bUnowned = false;
    };
    void SyncVehicles();
    TWeakObjectPtr<USCViewerSubsystem> Viewer;
    TMap<int32, FPairing> Pairings;
    FName Player = TEXT("you");
    TWeakObjectPtr<ASCVehicle> LocateShip;
};
