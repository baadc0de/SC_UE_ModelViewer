// WP-M2 fleet lifecycle. Live configuration goes through the viewer and the paired vehicle.
#pragma once

#include "CoreMinimal.h"
#include "SCBridgeClient.h"
#include "Subsystems/WorldSubsystem.h"

#include "SCGarageSubsystem.generated.h"

class ASCVehicle;
class USCViewerSubsystem;
struct FSCUiBinding;

enum class ESCGarageStatus : uint8 { Stored, Retrieving, InHangar, Unavailable };
enum class ESCGaragePlacement : uint8 { Bay, Floor };

struct FSCGarageRow
{
    FString Entity, DisplayName, Role, Size, Paint, Error;
    TArray<FString> PaintOptions;
    ESCGarageStatus Status = ESCGarageStatus::Stored;
    int32 Ship = INDEX_NONE;
    float Progress = 0;
    double DistanceM = 0;
};

struct FSCGarageOrder
{
    int32 Ship = INDEX_NONE;
    int32 Occupant = INDEX_NONE;
    FString Refusal;
    bool bConfirmStore = false;
};

struct FSCGaragePolicy
{
    static FString StoreRefusal(bool bOwned, bool bAboard, bool bTransfer);
    static FString BayRefusal(bool bOwned, bool bConfirmed, bool& bNeedsConfirmation);
    static bool IsFleetCandidate(const FSCEntityHit& Entity);
    static bool IsVehicleKiosk(const FSCUiBinding& Binding);
    static const TCHAR* StatusName(ESCGarageStatus Status);
};

UCLASS()
class SCBRIDGE_API USCGarageSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    static USCGarageSubsystem* Get(const UObject* WorldContext);
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    void RequestCatalog();
    TArray<FSCGarageRow> Rows(const FString& EntityFilter = {}, bool bIncludeDefaultStored = true) const;
    bool AddToFleet(const FString& Entity);
    const TArray<FSCEntityHit>& GetCatalog() const { return Catalog; }
    FSCGarageOrder Retrieve(const FString& Entity, ESCGaragePlacement At, bool bConfirmStore, int32 ConfirmedOccupant = INDEX_NONE);
    FString Store(int32 Ship);
    FString Paint(int32 Ship, const FString& Palette);
    FString PaintEntity(const FString& Entity, const FString& Palette);

private:
    USCViewerSubsystem* Viewer() const;
    static FString Canonical(FString Entity);
    bool Available(const FString& Entity) const;
    void FinishRetrieve(const FString& Error, ASCVehicle* Ship);
    struct FPending
    {
        FString Entity;
        int32 Id = INDEX_NONE;
    };
    TOptional<FPending> Pending;
    TArray<FSCEntityHit> Catalog;
    TArray<FString> Cooked;
    TMap<FString, FString> Failures;
    int32 RequestedGeneration = INDEX_NONE;
    bool bScannedCooked = false;
};
