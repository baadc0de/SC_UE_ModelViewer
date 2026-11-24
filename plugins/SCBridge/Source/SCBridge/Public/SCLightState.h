// Blueprint-visible authored/default state and fallback-shadow metadata for each imported light.
#pragma once
#include "SCLights.h"
#include "CoreMinimal.h"
#include "SCLightState.generated.h"
class ULightComponent;
class FJsonObject;

/** One authored state of a light (`states.<key>`): its intensity by the import's model, colour and style. Computed at
 *  import from the light record, saved with a prepared level, so changing a light's state reads no JSON. */
USTRUCT()
struct SCBRIDGE_API FSCLightStateValues
{
    GENERATED_BODY()
    UPROPERTY() double Candela = 0;
    UPROPERTY() FLinearColor Color = FLinearColor::White;
    UPROPERTY() int32 LightStyleId = 0;
};
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
    /** The light record (its states and intensity model), at import only. */
    TSharedPtr<FJsonObject> Json;
    /** Its authored states by key (FSCLightStateValues): FSCLights::ApplyState puts one on. Saved. */
    UPROPERTY() TMap<FName, FSCLightStateValues> AuthoredStates;
    /** Authored photometry before the active preset waveform is applied. */
    UPROPERTY() double BaseCandela = 0;
    UPROPERTY() FLinearColor BaseColor = FLinearColor::White;
    UPROPERTY() int32 LightStyleId = 0;
    UPROPERTY() double AnimationSpeed = 1;
    UPROPERTY() double AnimationPhase = 0;
    double StyleSampleSeconds = 0;
    UPROPERTY() float WallLightScale = 1;
    UPROPERTY() float FloorWashScale = 1;
};
