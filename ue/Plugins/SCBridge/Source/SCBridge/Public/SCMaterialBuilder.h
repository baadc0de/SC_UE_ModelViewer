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
    bool bHostTinted = false;
    bool bGlowCard = false;
    bool bForward = false;     // CryEngine MTL_FLAG_REQUIRE_FORWARD_RENDERING (MtlFlags & 0x8000)
    bool bV2 = false;          // described from the sidecar v2 `emissive` / `alpha_config` objects    // forward-flag Illum glow card: additive, alpha-weighted, non-Nanite  // POM mesh decal: colour follows the host surface's palette channel (HostTint)
    TMap<FName, float> Scalars;
    TMap<FName, FLinearColor> Vectors;
    TMap<FName, FSCTextureKey> Textures;
    /** Vector parameter -> texture whose mean colour multiplies it: a layer map the game shader does not sample
     *  for this material (resolved by ApplyTextureMeans once the texture is decoded; never bound). */
    TMap<FName, FSCTextureKey> VectorMeans;
    TArray<FString> Warnings;
};

class SCBRIDGE_API FSCMaterialBuilder
{
public:
    /** Maps one sidecar submaterial; Palette is the scene instance's palettes.json record (may be null). */
    /** bUseV2Objects=false forces the v1 attribute path (used to cross-check v2 data). */
    static FSCMaterialDesc Describe(const FSCSubmaterial& Sub, const FSCPalette* Palette, bool bUseV2Objects = true);
    /** Palette-independent D6 decision used by the mesh split (same rule as Describe().bNonNanite). */
    static bool RequiresNonNanite(const FSCSubmaterial& Sub, bool bEngineGlowTarget = false);
    /** controls.engine_glow target: Strength is in UE emissive units (cd/m2; see EngineGlowToEmissive) and drives
     *  emission; colour stays the rule's (surface colour x Emissive when non-black). A forward-flag Illum target
     *  becomes a glow card (additive, alpha-weighted, non-Nanite) so it can light up at runtime. */
    static void ApplyEngineGlow(FSCMaterialDesc& Desc, float Strength);
    /** Control value -> UE emissive strength (cd/m2): `material_property_glow` is the output of an entity-effect
     *  Glow link, which the game adds as per-instance glow (FSCStarEngine::InstanceGlowNits; linear, not the
     *  quadratic authored-Glow rule; ue/verification/WP-U3b.md §13); legacy `emission_strength` maps 1:1.
     *  Clamped to the control range. */
    static float EngineGlowToEmissive(const FSCEngineGlow& Control, float Value);
    /** Palette channel of an opaque host-paint submaterial ("" for decals, glass, unrouted). */
    static FString HostChannel(const FSCSubmaterial& Sub);
    /** palettes.json colour of a channel (primary/secondary/tertiary/glass); false when absent. */
    static bool PaletteColor(const FSCPalette* Palette, const FString& Channel, FLinearColor& Out);
    /** Resolves Desc.VectorMeans: each named vector's RGB x the texture's mean colour (FSCTextureImporter::
     *  MeanColor). A mean that is not available leaves the vector as described, with a warning. */
    static void ApplyTextureMeans(FSCMaterialDesc& Desc, const TMap<FSCTextureKey, FLinearColor>& Means);
    static const TCHAR* MasterName(ESCMaster Master);
    /** Object path of the master or its blend/two-sided constant variant. */
    static FString ParentPath(ESCMaster Master, ESCBlend Blend, bool bTwoSided);
    /** Game thread. Missing textures keep the master's neutral default and add a warning. */
    static UMaterialInstanceDynamic* Create(const FSCMaterialDesc& Desc, const TMap<FSCTextureKey, UTexture2D*>& Textures,
        UObject* Outer, TArray<FString>& Warnings);
};
