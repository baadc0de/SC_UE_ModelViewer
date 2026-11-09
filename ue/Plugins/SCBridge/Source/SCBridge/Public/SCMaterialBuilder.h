// FSCMaterialBuilder: *.materials.json submaterial (+ palette) -> master choice and parameters (pure, Describe),
// then UMaterialInstanceDynamic on the /SCBridge/Materials masters (game thread, Create).
#pragma once
#include "CoreMinimal.h"
#include "SCPackageReader.h"
#include "SCTextureImporter.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;

/** One master per shader family group; names match ue/scripts/create_materials.py. */
enum class ESCMaster : uint8 { Illum, HardSurface, Layered, Glass, MeshDecal, MeshDecalRelief, Translucent, EmissiveUnlit, Fallback };
enum class ESCBlend : uint8 { Opaque, Masked, Translucent };

/** Pure description of one material instance: safe to build on a worker, unit-testable without UObjects. */
struct SCBRIDGE_API FSCMaterialDesc
{
    FString Name, Family, Shader;
    ESCMaster Master = ESCMaster::Fallback;
    ESCBlend Blend = ESCBlend::Opaque;
    bool bTwoSided = false;
    bool bHidden = false;      // inactive / NoDraw: drawn by nothing (zero-emission additive in the split mesh)
    bool bNonNanite = false;   // D6: translucent, decal, additive or hidden -> separate conventional mesh
    bool bHostTinted = false;  // POM mesh decal: colour follows the host surface's palette channel (HostTint)
    TMap<FName, float> Scalars;
    TMap<FName, FLinearColor> Vectors;
    TMap<FName, FSCTextureKey> Textures;
    TArray<FString> Warnings;
};

class SCBRIDGE_API FSCMaterialBuilder
{
public:
    /** Host constant (fallback register, ue/verification/WP-U2.md): UE emissive units per authored Glow unit
     *  for the Viewer's fixed exposure. Sunset: replace with the engine's Glow luminance unit once known. */
    static constexpr float GlowToEmissive = 10.f;
    /** Maps one sidecar submaterial; Palette is the scene instance's palettes.json record (may be null). */
    static FSCMaterialDesc Describe(const FSCSubmaterial& Sub, const FSCPalette* Palette);
    /** Palette-independent D6 decision used by the mesh split (same rule as Describe().bNonNanite). */
    static bool RequiresNonNanite(const FSCSubmaterial& Sub);
    /** controls.engine_glow target: emission follows the exposed strength instead of authored glow. */
    static void ApplyEngineGlow(FSCMaterialDesc& Desc, float Strength);
    /** Palette channel of an opaque host-paint submaterial ("" for decals, glass, unrouted). */
    static FString HostChannel(const FSCSubmaterial& Sub);
    /** palettes.json colour of a channel (primary/secondary/tertiary/glass); false when absent. */
    static bool PaletteColor(const FSCPalette* Palette, const FString& Channel, FLinearColor& Out);
    static const TCHAR* MasterName(ESCMaster Master);
    /** Object path of the master or its blend/two-sided constant variant. */
    static FString ParentPath(ESCMaster Master, ESCBlend Blend, bool bTwoSided);
    /** Game thread. Missing textures keep the master's neutral default and add a warning. */
    static UMaterialInstanceDynamic* Create(const FSCMaterialDesc& Desc, const TMap<FSCTextureKey, UTexture2D*>& Textures,
        UObject* Outer, TArray<FString>& Warnings);
};
