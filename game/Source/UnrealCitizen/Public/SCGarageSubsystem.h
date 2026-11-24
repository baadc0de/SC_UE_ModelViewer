// WP-M2 fleet lifecycle. Live configuration goes through the viewer and the paired vehicle.
#pragma once

#include "CoreMinimal.h"
#include "SCSceneManifest.h"
#include "SCArchive.h"
#include "Subsystems/WorldSubsystem.h"

#include "SCGarageSubsystem.generated.h"

class ASCVehicle;
class USCViewerSubsystem;
struct FSCUiBinding;

enum class ESCGarageStatus : uint8 { Stored, Retrieving, InHangar, Unavailable };
enum class ESCGaragePlacement : uint8 { Bay, Floor };

/** A ship's Garage details (the fleet table's columns), from its EntityClassDefinition and manufacturer records. */
struct UNREALCITIZEN_API FSCGarageShipDetails
{
    FString Manufacturer, Role, Size;
    /** A ship of the fleet (a scenario's vehicle); false: prepared only as something else (a cargo setup's obstacle, e.g. an
     *  asteroid), so its prepared level is no Garage ship. */
    bool bFleet = true;
    /** The archive build the row was read from (the ship's prepared level's, at prep); empty: unknown. */
    FString Build;
};

struct FSCGarageRow
{
    FString Entity, DisplayName, Manufacturer, Role, Size, Paint, Error;
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
class UNREALCITIZEN_API USCGarageSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    static USCGarageSubsystem* Get(const UObject* WorldContext);
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    void RequestCatalog();
    void RequestDetails(const FString& Entity);
    TArray<FSCGarageRow> Rows(const FString& EntityFilter = {}, bool bIncludeDefaultStored = true) const;
    bool AddToFleet(const FString& Entity);
    const TArray<FSCEntityHit>& GetCatalog() const { return Catalog; }
    FSCGarageOrder Retrieve(const FString& Entity, ESCGaragePlacement At, bool bConfirmStore, int32 ConfirmedOccupant = INDEX_NONE);
    FString Store(int32 Ship);
    FString Paint(int32 Ship, const FString& Palette);
    FString PaintEntity(const FString& Entity, const FString& Palette);

    /** The details in an EntityClassDefinition record (role, size) and the reference of its manufacturer record (empty:
     *  none). False when the record does not read. */
    static bool ReadClassDetails(const FString& ClassJson, FSCGarageShipDetails& Out, FString& OutManufacturerRef);
    /** A manufacturer record's code (empty when it has none or does not read). */
    static FString ReadManufacturerCode(const FString& Json);
    /** WP-O1-4b-3c: the prepared ships' details, read at prep (Content/SCPrepared/Shared/garage_details.json, entity ->
     *  details; beside the records, apart from them: no scenario name can be it):
     *  a ship listed there needs no archive for its row. */
    static FString PreparedDetailsFile();
    static bool LoadPreparedDetails(TMap<FString, FSCGarageShipDetails>& Out, const FString& File = PreparedDetailsFile());
    static bool SavePreparedDetails(const TMap<FString, FSCGarageShipDetails>& Details, FString& Error, const FString& File = PreparedDetailsFile());

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
    using FDetails = FSCGarageShipDetails;
    TMap<FName, FDetails> Details;
    TMap<FString, FSCGarageShipDetails> PreparedDetails; // read once with the prepared ships (RequestCatalog)
    TMap<FString, FString> ManufacturerCodes;
    TArray<FString> DetailsQueue;
    bool bDetailsReading = false;
    void ReadNextDetail();
};
