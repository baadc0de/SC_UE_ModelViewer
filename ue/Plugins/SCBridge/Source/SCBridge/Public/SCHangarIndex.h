// Capacity of the cooked hangars (WP-C1): what ship each one holds, measured at cook time from its own geometry, so the
// viewer can pick a hangar by ship size without loading every hangar.
#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SCHangarIndex.generated.h"

USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCHangarCapacity
{
    GENERATED_BODY()
    /** Archive folder of the object container (e.g. hightech_a) and its base name (e.g. hangar_xltop_001). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SC") FString Style;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SC") FString Name;
    /** Ship elevator platform (FSCEnvironment::FindShipBay), presented pose: footprint X/Y in cm. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SC") FVector2D BayCm = FVector2D::ZeroVector;
    /** Clear height above the platform in cm; negative = open (FSCEnvironment::BayClearance hit nothing). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SC") double ClearanceCm = -1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SC") FString Elevator;
    /** Ships enter through a roof hatch (FSCEnvironment::IsTopLoading), else through front doors. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SC") bool bTopLoading = false;
    /** A ship of this width/length (cm, centred on the bay) and height above the floor fits. */
    bool Holds(const FBox& ShipRelativeToBayCentre) const;
};

UCLASS()
class SCBRIDGE_API USCHangarIndex : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SC") TArray<FSCHangarCapacity> Hangars;
    static constexpr const TCHAR* AssetPath = TEXT("/Game/SC/Hangars/HangarIndex.HangarIndex");
    /** The cooked index, or null when no hangar has been cooked. */
    static USCHangarIndex* Load();
    const FSCHangarCapacity* Find(const FString& Style, const FString& Name) const;
    FSCHangarCapacity& FindOrAdd(const FString& Style, const FString& Name);
};
