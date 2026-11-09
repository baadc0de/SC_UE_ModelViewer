// View options for one entity load (LOD, mip, content toggles, collision). A UObject so UI widgets, Blueprint and
// Python bind to the same fields; Config so the last used options persist (USCViewerSettings::Save).
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SCViewOptions.generated.h"

UENUM(BlueprintType)
enum class ESCMaterialMode : uint8
{
    Textures,
    Colors,
    None
};

UCLASS(BlueprintType, Config = GameUserSettings)
class SCBRIDGE_API USCViewOptions : public UObject
{
    GENERATED_BODY()
public:
    USCViewOptions();
    /** Geometry LOD exported by the core (0 = full detail). */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Options", meta = (ClampMin = 0, ClampMax = 3)) int32 Lod = 0;
    /** Texture mip level dropped at export (0 = full resolution). */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Options", meta = (ClampMin = 0, ClampMax = 4)) int32 Mip = 0;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Options") ESCMaterialMode MaterialMode = ESCMaterialMode::Textures;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Options") bool bAttachments = true;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Options") bool bInterior = true;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Options") bool bLights = true;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Options") bool bShields = false;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Options") bool bAnimations = true;
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Options") bool bDefaultPose = false;
    /** UE-side: build per-mesh complex collision (the FFI never builds collision). */
    UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category = "SC|Options") bool bCollision = true;

    /** Core defaults (sc_default_options) plus collision on. */
    UFUNCTION(BlueprintCallable, Category = "SC|Options") void ResetToDefaults();
    UFUNCTION(BlueprintCallable, Category = "SC|Options") void CopyFrom(const USCViewOptions* Other);
    /** Stable hash of every field; with the entity name it keys object reuse. */
    UFUNCTION(BlueprintPure, Category = "SC|Options") FString GetHash() const;
    /** Short human summary, e.g. "LOD 1 · mip 2 · textures · interior · lights · anim · collision". */
    UFUNCTION(BlueprintPure, Category = "SC|Options") FString Describe() const;
    /** Fill the FFI struct (collision_reserved stays 0). */
    void ToFfi(struct sc_options& Out) const;
};
