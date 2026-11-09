// SCBridge.Materials.* automation tests: family -> master mapping, parameters and palette routing from the
// checked-in synthetic fixtures (Tests/Fixtures/Materials), master assets, and PNG texture decoding flags.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCMaterialBuilder.h"
#include "SCImportMaterials.h"
#include "SCPackageReader.h"
#include "SCTextureImporter.h"
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "UObject/Package.h"

namespace
{
FString FixtureDir() { return IPluginManager::Get().FindPlugin(TEXT("SCBridge"))->GetBaseDir() / TEXT("Tests/Fixtures/Materials"); }
bool ReadFixture(FAutomationTestBase& Test, FSCSceneManifest& Manifest)
{
    FSCFolderPackageSource Source(FixtureDir()); FString Error; TArray<FString> Warnings;
    const bool bOK = FSCPackageReader::Read(Source, Manifest, Error, TEXT("scene.json"), &Warnings);
    Test.TestTrue(TEXT("fixture manifest reads: ") + Error, bOK && Manifest.Materials.Num() == 1 && Manifest.Palettes.Num() == 1);
    return bOK && Manifest.Materials.Num() == 1 && Manifest.Palettes.Num() == 1;
}
const FSCMaterialDesc* Find(const TArray<FSCMaterialDesc>& Descs, const TCHAR* Name)
{ return Descs.FindByPredicate([&](const FSCMaterialDesc& D) { return D.Name == Name; }); }
bool Near(const FLinearColor& A, const FLinearColor& B) { return A.Equals(B, 1e-4f); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCMaterialsFamilies, "SCBridge.Materials.FamilyMapping", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCMaterialsFamilies::RunTest(const FString&)
{
    FSCSceneManifest M; if (!ReadFixture(*this, M)) return false;
    const FSCPalette* Palette = &M.Palettes[0];
    TArray<FSCMaterialDesc> D; for (const auto& Sub : M.Materials[0].Submaterials) D.Add(FSCMaterialBuilder::Describe(Sub, Palette));
    auto Check = [&](const TCHAR* Name, ESCMaster Master, ESCBlend Blend, bool bTwoSided, bool bNonNanite) -> const FSCMaterialDesc*
    {
        const FSCMaterialDesc* X = Find(D, Name);
        if (!TestNotNull(FString(Name), X)) return nullptr;
        TestEqual(FString(Name) + TEXT(" master"), int32(X->Master), int32(Master));
        TestEqual(FString(Name) + TEXT(" blend"), int32(X->Blend), int32(Blend));
        TestEqual(FString(Name) + TEXT(" two-sided"), X->bTwoSided, bTwoSided);
        TestEqual(FString(Name) + TEXT(" non-Nanite (D6)"), X->bNonNanite, bNonNanite);
        return X;
    };
    if (auto* X = Check(TEXT("illum_spec"), ESCMaster::Illum, ESCBlend::Opaque, false, false))
    {
        TestTrue(TEXT("diffuse colour"), Near(X->Vectors[TEXT("DiffuseColor")], FLinearColor(0.5f, 0.25f, 1.f)));
        TestTrue(TEXT("F0 = authored specular colour"), Near(X->Vectors[TEXT("SpecularColor")], FLinearColor(0.9f, 0.6f, 0.3f)));
        TestEqual(TEXT("diffuse sRGB"), int32(X->Textures[TEXT("DiffuseMap")].Role), int32(ESCTextureRole::Color));
        TestEqual(TEXT("DDNA normal"), int32(X->Textures[TEXT("NormalGlossMap")].Role), int32(ESCTextureRole::Normal));
        TestEqual(TEXT("derived roughness paired by source_path"), X->Textures[TEXT("RoughnessMap")].Path, FString(TEXT("tex/illum_ddna_roughness.png")));
        TestEqual(TEXT("roughness linear"), int32(X->Textures[TEXT("RoughnessMap")].Role), int32(ESCTextureRole::Linear));
        TestEqual(TEXT("spec map colour"), int32(X->Textures[TEXT("SpecularMap")].Role), int32(ESCTextureRole::Color));
        TestEqual(TEXT("uses roughness map"), X->Scalars[TEXT("UseRoughnessMap")], 1.f);
        TestEqual(TEXT("gloss = shininess / 255"), X->Scalars[TEXT("GlossMult")], 0.8f);
        TestEqual(TEXT("no glow"), X->Scalars[TEXT("EmissiveStrength")], 0.f);
    }
    if (auto* X = Check(TEXT("illum_masked_glow"), ESCMaster::Illum, ESCBlend::Masked, false, false))
    {
        TestEqual(TEXT("alpha test"), X->Scalars[TEXT("AlphaTest")], 0.25f);
        TestTrue(TEXT("emissive colour"), Near(X->Vectors[TEXT("EmissiveColor")], FLinearColor(2.f, 1.f, 0.5f)));
        TestEqual(TEXT("glow x host scale"), X->Scalars[TEXT("EmissiveStrength")], 3.f * FSCMaterialBuilder::GlowToEmissive);
        TestEqual(TEXT("authored Shininess=255 used as authored (no placeholder substitution)"), X->Scalars[TEXT("Smoothness")], 1.f);
        TestEqual(TEXT("masked parent"), FSCMaterialBuilder::ParentPath(X->Master, X->Blend, X->bTwoSided),
            FString(TEXT("/SCBridge/Materials/MI_SC_Illum_Masked.MI_SC_Illum_Masked")));
    }
    Check(TEXT("illum_decal_sheet"), ESCMaster::MeshDecal, ESCBlend::Translucent, true, true);
    if (auto* X = Check(TEXT("hard_primary"), ESCMaster::HardSurface, ESCBlend::Opaque, false, false))
    {
        // snapshot diffuse x layer tint x palette secondary.
        TestTrue(TEXT("layer tint x palette"), Near(X->Vectors[TEXT("Layer0Tint")], FLinearColor(0.25f, 0.25f, 0.125f)));
        TestEqual(TEXT("layer tiling"), X->Scalars[TEXT("Layer0Tiling")], 2.f);
        TestEqual(TEXT("layer gloss"), X->Scalars[TEXT("Layer0GlossMult")], 0.5f);
        TestFalse(TEXT("wear layer is not a base layer"), X->Scalars.Contains(TEXT("Layer1Tiling")));
        TestEqual(TEXT("layer roughness"), X->Textures[TEXT("Layer0RoughnessMap")].Path, FString(TEXT("tex/Primary_ddna_roughness.png")));
    }
    if (auto* X = Check(TEXT("blend4"), ESCMaster::Layered, ESCBlend::Opaque, false, false))
    {
        TestTrue(TEXT("layer 0 palette primary"), Near(X->Vectors[TEXT("Layer0Tint")], FLinearColor(0.2f, 0.4f, 0.6f)));
        TestTrue(TEXT("layer 1 unrouted"), Near(X->Vectors[TEXT("Layer1Tint")], FLinearColor::White));
        TestTrue(TEXT("layer 2 palette tertiary"), Near(X->Vectors[TEXT("Layer2Tint")], FLinearColor(0.9f, 0.1f, 0.1f)));
        TestEqual(TEXT("layer 1 metallic from snapshot"), X->Scalars[TEXT("Layer1Metallic")], 1.f);
        TestTrue(TEXT("layer 1 F0"), Near(X->Vectors[TEXT("Layer1Specular")], FLinearColor(0.9f, 0.9f, 0.9f)));
        TestFalse(TEXT("layer without textures has no maps"), X->Textures.Contains(TEXT("Layer3DiffuseMap")));
        TestEqual(TEXT("blend mask"), X->Textures[TEXT("BlendMaskMap")].Path, FString(TEXT("tex/blend.png")));
        TestEqual(TEXT("blend mask on"), X->Scalars[TEXT("UseBlendMask")], 1.f);
        TestEqual(TEXT("blend map tiling"), X->Scalars[TEXT("BlendMaskTiling")], 3.f);
    }
    if (auto* X = Check(TEXT("blend1"), ESCMaster::Layered, ESCBlend::Opaque, false, false))
    {
        TestEqual(TEXT("unused slots repeat the last base layer"), X->Scalars[TEXT("Layer3Tiling")], 8.f);
        TestEqual(TEXT("repeated texture"), X->Textures[TEXT("Layer2DiffuseMap")].Path, FString(TEXT("tex/BaseLayer1_diff.png")));
        TestEqual(TEXT("no mask -> layer 0"), X->Scalars[TEXT("UseBlendMask")], 0.f);
    }
    if (auto* X = Check(TEXT("glass_ext"), ESCMaster::Glass, ESCBlend::Translucent, false, true))
    {
        TestTrue(TEXT("palette glass tint"), Near(X->Vectors[TEXT("GlassTint")], FLinearColor(0.1f, 0.6f, 0.3f)));
        TestEqual(TEXT("tint strength"), X->Scalars[TEXT("GlassTintStrength")], 0.5f);
        TestEqual(TEXT("thin film"), X->Scalars[TEXT("ThinFilmThickness")], 0.06f);
        TestEqual(TEXT("glass roughness"), X->Textures[TEXT("RoughnessMap")].Path, FString(TEXT("tex/glass_ddna_roughness.png")));
    }
    if (auto* X = Check(TEXT("decal_pom"), ESCMaster::MeshDecalRelief, ESCBlend::Translucent, true, true))
    {
        TestEqual(TEXT("POM displacement"), X->Scalars[TEXT("PomDisplacement")], 0.03f);
        TestEqual(TEXT("height linear"), int32(X->Textures[TEXT("HeightMap")].Role), int32(ESCTextureRole::Linear));
        TestEqual(TEXT("decal alpha multiplier"), X->Scalars[TEXT("Opacity")], 0.5f);
        TestEqual(TEXT("control-only POM: normal/roughness decal, coverage from TexSlot1"), FSCMaterialBuilder::ParentPath(X->Master, X->Blend, X->bTwoSided),
            FString(TEXT("/SCBridge/Materials/MI_SC_MeshDecal_Relief_TwoSided.MI_SC_MeshDecal_Relief_TwoSided")));
        TestEqual(TEXT("coverage texture"), X->Textures[TEXT("DiffuseMap")].Path, FString(TEXT("tex/decal_diff.png")));
    }
    if (auto* X = Check(TEXT("decal_colour"), ESCMaster::MeshDecal, ESCBlend::Translucent, true, true))
        TestEqual(TEXT("colour decal parent"), FSCMaterialBuilder::ParentPath(X->Master, X->Blend, X->bTwoSided),
            FString(TEXT("/SCBridge/Materials/MI_SC_MeshDecal_TwoSided.MI_SC_MeshDecal_TwoSided")));
    const float DefaultClip = GetDefault<UMaterial>()->OpacityMaskClipValue;
    if (auto* X = Check(TEXT("forward_trim"), ESCMaster::Illum, ESCBlend::Masked, false, false))
        TestEqual(TEXT("non-emitting forward surface stays masked at the engine default clip"), X->Scalars[TEXT("AlphaTest")], DefaultClip);
    if (const FSCMaterialDesc* X = Find(D, TEXT("forward_trim")))
    {
        // controls.engine_glow on a forward Illum target: glow card driven by the control, rule colour (no tint).
        FSCMaterialDesc Driven = *X; FSCMaterialBuilder::ApplyEngineGlow(Driven, 50.f);
        TestTrue(TEXT("driven forward target is a non-Nanite glow card"), Driven.bGlowCard && Driven.bNonNanite && Driven.Master == ESCMaster::EmissiveUnlit);
        TestEqual(TEXT("control strength"), Driven.Scalars[TEXT("EmissiveStrength")], 50.f);
        TestTrue(TEXT("black Emissive = no tint"), Near(Driven.Vectors[TEXT("EmissiveColor")], FLinearColor::White));
        TestTrue(TEXT("split decision knows engine-glow targets"), FSCMaterialBuilder::RequiresNonNanite(
            *M.Materials[0].Submaterials.FindByPredicate([](const FSCSubmaterial& S) { return S.Name == TEXT("forward_trim"); }), true));
    }
    if (const FSCMaterialDesc* X = Find(D, TEXT("illum_spec")))
    {
        FSCMaterialDesc Driven = *X; FSCMaterialBuilder::ApplyEngineGlow(Driven, 50.f);
        TestTrue(TEXT("driven non-forward target stays a lit Nanite surface"), Driven.Master == ESCMaster::Illum && !Driven.bNonNanite);
    }
    if (auto* X = Check(TEXT("forward_glow"), ESCMaster::EmissiveUnlit, ESCBlend::Translucent, false, true))
    {
        // Headlight_Glow-like: forward flag + Emissive 1,1,1 + no Glow -> glow card, strength mean(Emissive) = 1,
        // colour from Diffuse x texture, additive and alpha-weighted (no clip), split from Nanite.
        TestTrue(TEXT("glow card"), X->bGlowCard);
        TestEqual(TEXT("card strength = 1 x k"), X->Scalars[TEXT("EmissiveStrength")], FSCMaterialBuilder::GlowToEmissive);
        TestEqual(TEXT("colour from the texture"), X->Textures[TEXT("DiffuseMap")].Path, FString(TEXT("tex/illum_diff.png")));
        TestEqual(TEXT("alpha-weighted"), X->Scalars[TEXT("AlphaWeight")], 1.f);
    }
    if (auto* X = Check(TEXT("decal_glow_overlay"), ESCMaster::EmissiveUnlit, ESCBlend::Translucent, true, true))
    {
        // Forward Illum + %DECAL + DecalDiffuseOpacity 0: glow-only overlay -> glow card (never a DBuffer decal),
        // coverage = texture alpha only, decal opacities never scale emission or coverage.
        TestTrue(TEXT("overlay is a glow card"), X->bGlowCard);
        TestFalse(TEXT("no decal opacity on the card"), X->Scalars.Contains(TEXT("Opacity")) && X->Scalars[TEXT("Opacity")] == 0.f);
        FSCMaterialDesc Driven = *X; FSCMaterialBuilder::ApplyEngineGlow(Driven, 100.f);
        TestEqual(TEXT("engine glow drives the overlay card"), Driven.Scalars[TEXT("EmissiveStrength")], 100.f);
    }
    if (auto* X = Check(TEXT("card_tinted"), ESCMaster::EmissiveUnlit, ESCBlend::Translucent, false, true))
    {
        // Tint applied once: strength 1 x k, Emissive only as colour.
        TestEqual(TEXT("tinted card strength = 1 x k"), X->Scalars[TEXT("EmissiveStrength")], FSCMaterialBuilder::GlowToEmissive);
        TestTrue(TEXT("tinted card colour = Emissive"), Near(X->Vectors[TEXT("EmissiveColor")], FLinearColor(0.2f, 0.4f, 0.9f)));
    }
    if (auto* X = Check(TEXT("v2_object"), ESCMaster::Illum, ESCBlend::Masked, true, false))
    {
        // v2 objects win over attributes: strength, tint, cutoff, alpha_test, two_sided.
        TestTrue(TEXT("described from v2"), X->bV2);
        TestEqual(TEXT("v2 strength"), X->Scalars[TEXT("EmissiveStrength")], 2.f * FSCMaterialBuilder::GlowToEmissive);
        TestTrue(TEXT("v2 tint"), Near(X->Vectors[TEXT("EmissiveColor")], FLinearColor(0.1f, 0.2f, 0.3f)));
        TestEqual(TEXT("v2 glow cutoff"), X->Scalars[TEXT("AlphaGlowCutoff")], 0.4f);
        TestEqual(TEXT("v2 alpha_test"), X->Scalars[TEXT("AlphaTest")], 0.2f);
        const FSCMaterialDesc V1 = FSCMaterialBuilder::Describe(*M.Materials[0].Submaterials.FindByPredicate([](const FSCSubmaterial& S) { return S.Name == TEXT("v2_object"); }), Palette, false);
        TestEqual(TEXT("v1 attribute path for comparison emits nothing"), V1.Scalars.FindRef(TEXT("EmissiveStrength")), 0.f);
    }
    if (auto* X = Check(TEXT("emissives_like"), ESCMaster::EmissiveUnlit, ESCBlend::Translucent, false, true))
    {
        // Emission colour = Emissive x Diffuse (x texture in the master): blue ordering, strength = Glow x host scale.
        const FLinearColor Glow = X->Vectors[TEXT("EmissiveColor")] * X->Vectors[TEXT("DiffuseColor")];
        TestTrue(TEXT("emissives glow blue (b > g > r)"), Glow.B > Glow.G && Glow.G > Glow.R && Glow.R > 0.f);
        TestEqual(TEXT("emissives strength"), X->Scalars[TEXT("EmissiveStrength")], 0.45f * FSCMaterialBuilder::GlowToEmissive);
    }
    if (auto* X = Check(TEXT("glows_like"), ESCMaster::Illum, ESCBlend::Opaque, false, false))
    {
        // Glow > 0 with black Emissive: no tint, the orange surface colour emits at Glow strength.
        const FLinearColor Glow = X->Vectors[TEXT("EmissiveColor")] * X->Vectors[TEXT("DiffuseColor")];
        TestTrue(TEXT("glows emit orange (r > g > b)"), Glow.R > Glow.G && Glow.G > Glow.B);
        TestEqual(TEXT("glows strength"), X->Scalars[TEXT("EmissiveStrength")], 0.35f * FSCMaterialBuilder::GlowToEmissive);
    }
    if (auto* X = Check(TEXT("emittance_noglow"), ESCMaster::Illum, ESCBlend::Opaque, false, false))
    {
        TestEqual(TEXT("emittance map without Glow -> strength 1"), X->Scalars[TEXT("EmissiveStrength")], FSCMaterialBuilder::GlowToEmissive);
        TestTrue(TEXT("Emissive tints the emittance"), Near(X->Vectors[TEXT("EmissiveColor")], FLinearColor(0.3f, 0.6f, 0.9f)));
    }
    if (auto* X = Check(TEXT("layerblend_like"), ESCMaster::Layered, ESCBlend::Opaque, false, false))
        TestEqual(TEXT("LayerBlend Emissive 1,1,1 without Glow / forward flag emits nothing"), X->Scalars[TEXT("EmissiveStrength")], 0.f);
    if (auto* X = Check(TEXT("glow_zero"), ESCMaster::Illum, ESCBlend::Opaque, false, false))
        TestEqual(TEXT("Glow = 0 with Emissive = 1,1,1 emits nothing"), X->Scalars[TEXT("EmissiveStrength")], 0.f);
    if (auto* X = Check(TEXT("glow_emittance"), ESCMaster::Illum, ESCBlend::Opaque, false, false))
    {
        TestEqual(TEXT("AlphaGlowCutoff from public_params"), X->Scalars[TEXT("AlphaGlowCutoff")], 0.25f);
        TestEqual(TEXT("%EMITTANCE_MAP routes the base-colour slot"), X->Textures[TEXT("EmissiveMap")].Path, FString(TEXT("tex/emit.png")));
        TestEqual(TEXT("emittance map on"), X->Scalars[TEXT("UseEmissiveMap")], 1.f);
        TestEqual(TEXT("emittance strength"), X->Scalars[TEXT("EmissiveStrength")], 1.5f * FSCMaterialBuilder::GlowToEmissive);
    }
    if (auto* X = Check(TEXT("stencil_palette"), ESCMaster::MeshDecal, ESCBlend::Translucent, true, true))
    {
        // $TintPaletteDecal: stencil texture from palettes.json decal.export_path, colours from decal red/green/blue.
        TestFalse(TEXT("palette supplies the missing colour source -> visible"), X->bHidden);
        TestEqual(TEXT("stencil texture"), X->Textures[TEXT("StencilMap")].Path, FString(TEXT("tex/palette_decal.png")));
        TestEqual(TEXT("stencil linear"), int32(X->Textures[TEXT("StencilMap")].Role), int32(ESCTextureRole::Linear));
        TestTrue(TEXT("decal red"), Near(X->Vectors[TEXT("DecalRed")], FLinearColor(0.7f, 0.2f, 0.1f)));
        TestTrue(TEXT("decal blue"), Near(X->Vectors[TEXT("DecalBlue")], FLinearColor(0.05f, 0.1f, 0.55f)));
        TestTrue(TEXT("StencilDiffuseColor2"), Near(X->Vectors[TEXT("StencilTint2")], FLinearColor(0.6f, 0.5f, 0.4f)));
        TestEqual(TEXT("StencilOpacity"), X->Scalars[TEXT("StencilOpacity")], 0.75f);
        TestEqual(TEXT("stencil on"), X->Scalars[TEXT("UseStencil")], 1.f);
        TestEqual(TEXT("two-sided decal parent"), FSCMaterialBuilder::ParentPath(X->Master, X->Blend, X->bTwoSided),
            FString(TEXT("/SCBridge/Materials/MI_SC_MeshDecal_TwoSided.MI_SC_MeshDecal_TwoSided")));
    }
    for (const FSCSubmaterial& Sub : M.Materials[0].Submaterials)
    {
        // Rule: no Glow (absent or 0) -> no emission, for every lit family (engine-glow targets excepted).
        const FSCMaterialDesc* X = Find(D, *Sub.Name);
        if (!X || X->bV2 || X->Master == ESCMaster::EmissiveUnlit || X->bGlowCard || Sub.Json.Contains(TEXT("\"Glow\""), ESearchCase::CaseSensitive) || Sub.Json.Contains(TEXT("EMITTANCE_MAP"), ESearchCase::CaseSensitive)) continue;
        TestEqual(Sub.Name + TEXT(": no Glow, no emission"), X->Scalars.FindRef(TEXT("EmissiveStrength")), 0.f);
        // Rule: a MeshDecal primitive never resolves to an opaque master.
        if (Sub.ShaderFamily == TEXT("MeshDecal"))
            TestTrue(Sub.Name + TEXT(": MeshDecal is a decal/hidden, never opaque"), X->Blend == ESCBlend::Translucent && X->bNonNanite);
    }
    if (auto* X = Check(TEXT("screen"), ESCMaster::EmissiveUnlit, ESCBlend::Translucent, false, true))
        TestEqual(TEXT("RTT screen dark until ui_bindings"), X->Scalars[TEXT("EmissiveStrength")], 0.f);
    if (auto* X = Check(TEXT("monitor"), ESCMaster::EmissiveUnlit, ESCBlend::Translucent, false, true))
        TestEqual(TEXT("screen image without Glow emits nothing"), X->Scalars[TEXT("EmissiveStrength")], 0.f);
    if (auto* X = Check(TEXT("inactive"), ESCMaster::EmissiveUnlit, ESCBlend::Translucent, true, true)) TestTrue(TEXT("hidden"), X->bHidden);
    if (auto* X = Check(TEXT("skin"), ESCMaster::Fallback, ESCBlend::Opaque, false, false))
        TestTrue(TEXT("fallback warning names the shader"), X->Warnings.Num() == 1 && X->Warnings[0].Contains(TEXT("HumanSkin_V2")));
    for (const FSCMaterialDesc& X : D)
        TestEqual(X.Name + TEXT(": RequiresNonNanite matches Describe"), FSCMaterialBuilder::RequiresNonNanite(
            *M.Materials[0].Submaterials.FindByPredicate([&](const FSCSubmaterial& S) { return S.Name == X.Name; })), X.bNonNanite);
    // Engine glow: scene controls target (sidecar, authored index 13) gets the default strength.
    TestEqual(TEXT("engine glow targets"), M.EngineGlow.Targets.Num(), 1);
    TestEqual(TEXT("engine glow default"), M.EngineGlow.DefaultStrength, 7.f);
    {
        FSCEngineGlow Glow = M.EngineGlow; // legacy units: 1:1
        TestEqual(TEXT("emission_strength 1:1"), FSCMaterialBuilder::EngineGlowToEmissive(Glow, 7.f), 7.f);
        Glow.Units = TEXT("material_property_glow"); Glow.MinStrength = 10.f; Glow.MaxStrength = 1000.f;
        TestEqual(TEXT("Glow units x GlowToEmissive"), FSCMaterialBuilder::EngineGlowToEmissive(Glow, 50.f), 50.f * FSCMaterialBuilder::GlowToEmissive);
        TestEqual(TEXT("clamped to the control minimum"), FSCMaterialBuilder::EngineGlowToEmissive(Glow, 1.f), 10.f * FSCMaterialBuilder::GlowToEmissive);
    }
    FSCImportMaterials Phase; Phase.Describe(M);
    FSCFolderPackageSource Empty(FixtureDir()); // fixtures carry no PNGs: every decode fails, counts stay exact
    FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    Phase.Decode(Empty);
    // Unique (path, role) pairs across all submaterials; tex/illum_ddna.png etc. are shared and deduplicated.
    TestEqual(TEXT("deduplicated texture requests"), Phase.TextureStats.Requested, 26);
    // Scene schema v2 (WP-R5, additive) reads; unknown versions are rejected.
    {
        FSCFolderPackageSource Source(FixtureDir()); FSCSceneManifest V2, V3; FString Error;
        TestTrue(TEXT("scene v2 accepted: ") + Error, FSCPackageReader::Read(Source, V2, Error, TEXT("scene_v2.json")) && V2.Materials.Num() == 1);
        TestFalse(TEXT("scene v3 rejected"), FSCPackageReader::Read(Source, V3, Error, TEXT("scene_v3.json")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCMaterialsPackageRules, "SCBridge.Materials.PackageRules", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCMaterialsPackageRules::RunTest(const FString&)
{
    // Real-package invariants (-SCPackage=<export root>): every referenced submaterial, no per-asset expectations.
    FString Folder;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SCPackage="), Folder) || !FPaths::DirectoryExists(Folder))
    { AddInfo(TEXT("PACKAGE_RULES skipped: no -SCPackage export root")); return true; }
    FSCFolderPackageSource Source(Folder); FSCSceneManifest M; FString Error;
    if (!TestTrue(TEXT("package reads: ") + Error, FSCPackageReader::Read(Source, M, Error))) return false;
    int32 Decals = 0, Glowing = 0, Silent = 0, Cards = 0, V2 = 0, Disagree = 0;
    for (const FSCMaterialSet& Set : M.Materials)
        for (const FSCSubmaterial& Sub : Set.Submaterials)
        {
            const FSCMaterialDesc D = FSCMaterialBuilder::Describe(Sub, M.Palettes.Num() ? &M.Palettes[0] : nullptr);
            const FString Where = Set.Path + TEXT(":") + Sub.Name;
            if (D.bV2)
            {
                // Cross-check the sidecar v2 objects against the v1 attribute evaluation (informational).
                ++V2;
                const FSCMaterialDesc A = FSCMaterialBuilder::Describe(Sub, M.Palettes.Num() ? &M.Palettes[0] : nullptr, false);
                TArray<FString> Diff;
                if (!FMath::IsNearlyEqual(A.Scalars.FindRef(TEXT("EmissiveStrength")), D.Scalars.FindRef(TEXT("EmissiveStrength")), 1e-3f))
                    Diff.Add(FString::Printf(TEXT("strength attr=%g v2=%g"), A.Scalars.FindRef(TEXT("EmissiveStrength")), D.Scalars.FindRef(TEXT("EmissiveStrength"))));
                if (!A.Vectors.FindRef(TEXT("EmissiveColor")).Equals(D.Vectors.FindRef(TEXT("EmissiveColor")), 1e-3f))
                    Diff.Add(TEXT("tint attr=") + A.Vectors.FindRef(TEXT("EmissiveColor")).ToString() + TEXT(" v2=") + D.Vectors.FindRef(TEXT("EmissiveColor")).ToString());
                if (A.bForward != D.bForward) Diff.Add(TEXT("forward"));
                if (A.bTwoSided != D.bTwoSided) Diff.Add(FString::Printf(TEXT("two_sided attr=%d v2=%d"), A.bTwoSided, D.bTwoSided));
                if (A.Blend != D.Blend || A.Master != D.Master) Diff.Add(FString::Printf(TEXT("blend/master attr=%d/%d v2=%d/%d"), int32(A.Blend), int32(A.Master), int32(D.Blend), int32(D.Master)));
                if (Diff.Num()) { ++Disagree; AddInfo(TEXT("V2_DIFF ") + Where + TEXT(" ") + FString::Join(Diff, TEXT("; "))); }
            }
            if (Sub.ShaderFamily == TEXT("MeshDecal"))
            {
                ++Decals;
                TestTrue(Where + TEXT(" MeshDecal never opaque"), D.Blend == ESCBlend::Translucent && D.bNonNanite
                    && (D.Master == ESCMaster::MeshDecal || D.Master == ESCMaster::MeshDecalRelief || D.bHidden));
            }
            if (D.bGlowCard)
            {
                ++Cards;
                // Emitting, or a %DECAL glow-only overlay at strength 0 (runtime-driven by controls.engine_glow).
                TestTrue(Where + TEXT(" glow card is additive, alpha-weighted and non-Nanite"), D.bNonNanite
                    && D.Scalars.FindRef(TEXT("AlphaWeight")) == 1.f && (D.Scalars.FindRef(TEXT("EmissiveStrength")) > 0.f || Sub.Json.Contains(TEXT("%DECAL"))));
                continue;
            }
            if (D.Master == ESCMaster::EmissiveUnlit) continue;
            const bool bGlow = Sub.Json.Contains(TEXT("\"Glow\""), ESearchCase::CaseSensitive);
            const float Strength = D.Scalars.FindRef(TEXT("EmissiveStrength"));
            if (Sub.Name == TEXT("emissives")) // the Gladius wing-tip / under-wing strips when this package is the Gladius
            {
                const FLinearColor C = D.Vectors.FindRef(TEXT("EmissiveColor")) * D.Vectors.FindRef(TEXT("DiffuseColor"));
                TestTrue(Where + TEXT(" emits blue (b > g > r)"), Strength > 0.f && C.B > C.G && C.G > C.R);
            }
            if (!bGlow && !Sub.Json.Contains(TEXT("EMITTANCE_MAP"), ESearchCase::CaseSensitive)) { ++Silent; TestEqual(Where + TEXT(" no Glow -> no emission"), Strength, 0.f); }
            else if (Strength > 0.f)
            {
                ++Glowing;
                const FLinearColor C = D.Vectors.FindRef(TEXT("EmissiveColor")) * D.Vectors.FindRef(TEXT("DiffuseColor"));
                TestTrue(Where + TEXT(" glow colour comes from Emissive x Diffuse"), C.R + C.G + C.B > 0.f);
            }
        }
    AddInfo(FString::Printf(TEXT("PACKAGE_RULES decals=%d glow_cards=%d glowing=%d no_glow=%d v2=%d v2_disagree=%d"), Decals, Cards, Glowing, Silent, V2, Disagree));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCMaterialsMasters, "SCBridge.Materials.MasterAssets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCMaterialsMasters::RunTest(const FString&)
{
    for (ESCMaster Master : {ESCMaster::Illum, ESCMaster::HardSurface, ESCMaster::Layered, ESCMaster::Glass, ESCMaster::MeshDecal,
        ESCMaster::MeshDecalRelief, ESCMaster::Translucent, ESCMaster::EmissiveUnlit, ESCMaster::Fallback})
        for (ESCBlend Blend : {ESCBlend::Opaque, ESCBlend::Masked})
            for (bool bTwoSided : {false, true})
            {
                const FString Path = FSCMaterialBuilder::ParentPath(Master, Blend, bTwoSided);
                UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *Path);
                if (!TestNotNull(Path, Parent)) continue;
                const bool bVariant = Path.Contains(TEXT("/MI_"));
                const bool bOpaqueFamily = Master == ESCMaster::Illum || Master == ESCMaster::HardSurface || Master == ESCMaster::Layered || Master == ESCMaster::Fallback;
                if (bVariant && bOpaqueFamily) TestEqual(Path + TEXT(" blend override"), int32(Parent->GetBlendMode()), int32(Blend == ESCBlend::Masked ? BLEND_Masked : BLEND_Opaque));
                if (bVariant && bOpaqueFamily) TestEqual(Path + TEXT(" two-sided override"), Parent->IsTwoSided(), bTwoSided);
                if (Master == ESCMaster::MeshDecal || Master == ESCMaster::MeshDecalRelief)
                    TestEqual(Path + TEXT(" is a DBuffer decal"), int32(Parent->GetMaterial()->MaterialDomain), int32(MD_DeferredDecal));
                if (Master == ESCMaster::MeshDecal || Master == ESCMaster::MeshDecalRelief || Master == ESCMaster::Translucent)
                    TestEqual(Path + TEXT(" sidedness follows is_double_sided"), Parent->IsTwoSided(), bTwoSided);
                TestEqual(Path + TEXT(" explicit mask clip 0.5"), Parent->GetMaterial()->OpacityMaskClipValue, 0.5f);
                if (Master == ESCMaster::Glass || Master == ESCMaster::Translucent || Master == ESCMaster::EmissiveUnlit)
                    TestTrue(Path + TEXT(" is translucent"), IsTranslucentBlendMode(Parent->GetBlendMode()));
            }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCMaterialsTextures, "SCBridge.Materials.TextureDecode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCMaterialsTextures::RunTest(const FString&)
{
    // Synthetic 4x2 RGBA PNG written to Saved, decoded per role.
    IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    TSharedPtr<IImageWrapper> Png = Module.CreateImageWrapper(EImageFormat::PNG);
    TArray<uint8> Rgba; for (int32 I = 0; I < 8; ++I) Rgba.Append({uint8(10 * I), uint8(200 - I), uint8(30), uint8(255 - I)});
    Png->SetRaw(Rgba.GetData(), Rgba.Num(), 4, 2, ERGBFormat::RGBA, 8);
    const TArray64<uint8>& Bytes = Png->GetCompressed();
    const FString Dir = FPaths::ProjectSavedDir() / TEXT("Import/MaterialTests");
    FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Bytes.GetData(), int32(Bytes.Num())), *(Dir / TEXT("t.png")));
    FSCFolderPackageSource Source(Dir);
    FSCDecodedTexture Color, Normal;
    TestTrue(TEXT("decode colour"), FSCTextureImporter::Decode(Source, {TEXT("t.png"), ESCTextureRole::Color}, Color));
    TestTrue(TEXT("decode normal"), FSCTextureImporter::Decode(Source, {TEXT("t.png"), ESCTextureRole::Normal}, Normal));
    TestEqual(TEXT("size"), Color.Width * 10 + Color.Height, 42);
    TestEqual(TEXT("mip chain 4x2 -> 2x1 -> 1x1"), Color.Mips.Num(), 3);
    if (Color.Mips.Num() && Normal.Mips.Num())
    {
        TestEqual(TEXT("BGRA red"), int32(Color.Mips[0][2]), 0);
        TestEqual(TEXT("colour green untouched"), int32(Color.Mips[0][1]), 200);
        TestEqual(TEXT("normal green flipped to UE -Y"), int32(Normal.Mips[0][1]), 55);
        TestEqual(TEXT("normal alpha (smoothness) untouched"), int32(Normal.Mips[0][3]), 255);
    }
    // Mip filtering: sRGB colour averages in linear light (black/white -> 188), masks/normals as stored (128).
    TArray<uint8> BW = {0, 0, 0, 255, 255, 255, 255, 255};
    Png->SetRaw(BW.GetData(), BW.Num(), 2, 1, ERGBFormat::RGBA, 8);
    const TArray64<uint8>& BWBytes = Png->GetCompressed();
    FFileHelper::SaveArrayToFile(TArrayView<const uint8>(BWBytes.GetData(), int32(BWBytes.Num())), *(Dir / TEXT("bw.png")));
    FSCDecodedTexture BWColor, BWLinear;
    if (FSCTextureImporter::Decode(Source, {TEXT("bw.png"), ESCTextureRole::Color}, BWColor) && BWColor.Mips.Num() == 2)
        TestEqual(TEXT("sRGB mip in linear light"), int32(BWColor.Mips[1][2]), 188);
    else AddError(TEXT("bw colour decode"));
    if (FSCTextureImporter::Decode(Source, {TEXT("bw.png"), ESCTextureRole::Linear}, BWLinear) && BWLinear.Mips.Num() == 2)
        TestEqual(TEXT("linear mip"), int32(BWLinear.Mips[1][2]), 128);
    else AddError(TEXT("bw linear decode"));
    // Limits: dimension / per-texture / aggregate budget are enforced before decompression.
    FSCTextureLimits Small; Small.MaxDimension = 2;
    FSCDecodedTexture Rejected;
    TestFalse(TEXT("over the dimension limit"), FSCTextureImporter::Decode(Source, {TEXT("t.png"), ESCTextureRole::Color}, Rejected, Small));
    TestTrue(TEXT("rejected before decode"), Rejected.Mips.IsEmpty() && Rejected.Error.Contains(TEXT("RHI limit")));
    FSCTextureLimits Budget; Budget.MaxDimension = 64; Budget.BudgetBytes = FSCTextureImporter::DecodedBytes(2, 1);
    const TArray<FSCDecodedTexture> Batch = FSCTextureImporter::DecodeAll(Source, {{TEXT("bw.png"), ESCTextureRole::Linear}, {TEXT("t.png"), ESCTextureRole::Color}}, Budget);
    TestTrue(TEXT("first texture within budget"), Batch.Num() == 2 && Batch[0].Error.IsEmpty() && Batch[0].Mips.Num() == 2);
    TestTrue(TEXT("second texture over the aggregate budget"), Batch.Num() == 2 && Batch[1].Mips.IsEmpty() && Batch[1].Error.Contains(TEXT("budget")));
    FSCDecodedTexture Missing;
    TestFalse(TEXT("missing file reports"), FSCTextureImporter::Decode(Source, {TEXT("none.png"), ESCTextureRole::Linear}, Missing));
    UPackage* Outer = GetTransientPackage();
    UTexture2D* C = FSCTextureImporter::Create(MoveTemp(Color), Outer);
    UTexture2D* N = FSCTextureImporter::Create(MoveTemp(Normal), Outer);
    if (TestNotNull(TEXT("colour texture"), C) && TestNotNull(TEXT("normal texture"), N))
    {
        TestTrue(TEXT("colour sRGB"), C->SRGB);
        TestFalse(TEXT("normal linear"), N->SRGB);
        TestEqual(TEXT("normal compression"), int32(N->CompressionSettings), int32(TC_Normalmap));
        TestEqual(TEXT("mips uploaded"), C->GetNumMips(), 3);
    }
    return true;
}
#endif
