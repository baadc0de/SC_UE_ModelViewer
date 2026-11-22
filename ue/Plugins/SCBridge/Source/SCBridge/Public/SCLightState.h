// Blueprint-visible authored/default state and fallback-shadow metadata for each imported light.
#pragma once
#include "CoreMinimal.h"
#include "SCLightState.generated.h"
class ULightComponent;
class FJsonObject;

USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCImportedLightState
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") TObjectPtr<ULightComponent> Component;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") FString Name;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") FString OwnerEntity;
    /** Exporter's first nonzero state; may differ from the authored viewer default. */
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") FName ExportActiveState;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") FName ActiveState;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") bool bEnabled = true;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") FString StatesJson;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") double RadiusM = 0;
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") double ShadowThresholdM = 0;
    /** Whether the light casts shadows: the authored `shadow_casting` flag (radius quartile for packages without it). */
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") bool bFallbackShadow = true;
    /** Authored `affects_this_area_only`: the game shades it only inside the clip volume it sits in (WP-U3b §12). */
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") bool bAreaOnly = false;
    /** The light sits inside one of the vehicle's room volumes (set for every light when the vehicle has any). */
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") bool bInsideArea = false;
    /** Room GUID the light sits in (WP-L1); empty outside every room volume. */
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") FString AreaRoom;
    /** The light group entity holding the light (`attachment.group_entity_id`, cache format 39); empty outside a group. */
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") FString GroupEntityId;
    /** The interior (scene `interiors[]` index) whose container the light came from; INDEX_NONE for the root and items. Group
     *  ids are unique within one container only. */
    UPROPERTY(BlueprintReadOnly, Category = "SC|Lights") int32 Interior = INDEX_NONE;
    /** The light record (its states and intensity model): FSCLightImporter::ApplyState puts another of its states on. */
    TSharedPtr<FJsonObject> Json;
    /** Authored photometry before the active preset waveform is applied. */
    double BaseCandela = 0;
    FLinearColor BaseColor = FLinearColor::White;
    int32 LightStyleId = 0;
    double AnimationSpeed = 1;
    double AnimationPhase = 0;
};
