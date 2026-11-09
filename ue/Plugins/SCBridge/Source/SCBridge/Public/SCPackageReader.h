#pragma once
#include "CoreMinimal.h"
#include "SCPackageSource.h"
#include "SCPackageReader.generated.h"

USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCSceneInstance
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString EntityName;
    UPROPERTY(BlueprintReadOnly) FString MeshAsset;
    UPROPERTY(BlueprintReadOnly) FString MaterialSidecar;
    UPROPERTY(BlueprintReadOnly) FString PaletteId;
    UPROPERTY(BlueprintReadOnly) FString ParentEntityName;
    UPROPERTY(BlueprintReadOnly) FString ParentNodeName;
    UPROPERTY(BlueprintReadOnly) FString ItemType;
    UPROPERTY(BlueprintReadOnly) FString PortFlags;
    UPROPERTY(BlueprintReadOnly) FString SourceTransformBasis;
    UPROPERTY(BlueprintReadOnly) bool bInvisible = false;
    UPROPERTY(BlueprintReadOnly) TArray<FString> LightJson; // lossless state/style/attachment metadata
    FString ImportWarning; // Invalid optional instance: retain identity to suppress descendants.
    FMatrix LocalMatrix = FMatrix::Identity;
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCInterior
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FSCSceneInstance Container;
    UPROPERTY(BlueprintReadOnly) TArray<FSCSceneInstance> Placements;
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCSubmaterial
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 Index = 0; // Array ordinal / GLB primitive slot per contract.
    UPROPERTY(BlueprintReadOnly) int32 SourceIndex = 0; // Authored material index can be sparse.
    UPROPERTY(BlueprintReadOnly) FString Name;
    UPROPERTY(BlueprintReadOnly) FString ShaderFamily;
    UPROPERTY(BlueprintReadOnly) bool bBlended = false;
    // Lossless metadata for WP-U2, including texture roles and authored parameters.
    UPROPERTY(BlueprintReadOnly) FString Json;
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCMaterialSet
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString Path;
    UPROPERTY(BlueprintReadOnly) TArray<FSCSubmaterial> Submaterials;
    UPROPERTY(BlueprintReadOnly) FString ContentHash;
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCPalette
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString Id;
    UPROPERTY(BlueprintReadOnly) FString Json;
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCClipRecord
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString Name;
    UPROPERTY(BlueprintReadOnly) FString Sidecar;
    UPROPERTY(BlueprintReadOnly) FString Json;
};
/** controls.engine_glow target: main-thruster glow submaterial by sidecar + authored material index. */
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCEngineGlowTarget
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString MaterialSidecar;
    UPROPERTY(BlueprintReadOnly) int32 SourceMaterialIndex = -1;
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCEngineGlow
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) float MinStrength = 0, MaxStrength = 0, DefaultStrength = 0;
    /** `material_property_glow` (CryEngine Glow value) or legacy `emission_strength`. */
    UPROPERTY(BlueprintReadOnly) FString Units;
    UPROPERTY(BlueprintReadOnly) TArray<FSCEngineGlowTarget> Targets;
};
USTRUCT(BlueprintType)
struct SCBRIDGE_API FSCSceneManifest
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) FString ScenePath;
    UPROPERTY(BlueprintReadOnly) FSCSceneInstance Root;
    UPROPERTY(BlueprintReadOnly) TArray<FSCSceneInstance> Children;
    UPROPERTY(BlueprintReadOnly) TArray<FSCInterior> Interiors;
    UPROPERTY(BlueprintReadOnly) TArray<FSCMaterialSet> Materials;
    UPROPERTY(BlueprintReadOnly) TArray<FSCPalette> Palettes;
    UPROPERTY(BlueprintReadOnly) TArray<FSCClipRecord> Clips;
    UPROPERTY(BlueprintReadOnly) FSCEngineGlow EngineGlow;
    UPROPERTY(BlueprintReadOnly) FString ContentHash;
    TArray<FString> MeshAssets() const;
};
class SCBRIDGE_API FSCPackageReader
{
public:
    // Empty ScenePath selects the sole manifest; shared roots with multiple scenes require an explicit path.
    static bool Read(const FSCPackageSource& Source, FSCSceneManifest& Out, FString& Error, const FString& ScenePath = TEXT(""), TArray<FString>* Warnings = nullptr);
    static FString Hash(const TArray<uint8>& Bytes);
};
