// SCBridge.Materials.* automation tests: family -> master mapping, parameters and palette routing from the
// checked-in synthetic fixtures (Tests/Fixtures/Materials), master assets, and PNG texture decoding flags.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCMaterialBuilder.h"
#include "SCImportMaterials.h"
#include "SCPackageReader.h"
#include "SCTextureImporter.h"
#include "SCStarEngine.h"
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace
{
// The game's glow rule from the extracted fixture: quadratic for an authored Glow, one scene unit for a placeholder.
float GlowNits(double Glow) { return float(FSCStarEngine::Get().GlowNits(Glow)); }
float UnitGlowNits() { return float(FSCStarEngine::Get().SceneUnitNits); }
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
        TestEqual(TEXT("derived roughness paired by source_path"), X->Textures[TEXT("SmoothnessMap")].Path, FString(TEXT("tex/illum_ddna_smoothness.dds")));
        TestEqual(TEXT("roughness linear"), int32(X->Textures[TEXT("SmoothnessMap")].Role), int32(ESCTextureRole::Linear));
        TestEqual(TEXT("spec map colour"), int32(X->Textures[TEXT("SpecularMap")].Role), int32(ESCTextureRole::Color));
        TestEqual(TEXT("uses roughness map"), X->Scalars[TEXT("UseSmoothnessMap")], 1.f);
        TestEqual(TEXT("gloss = shininess / 255"), X->Scalars[TEXT("GlossMult")], 0.8f);
        TestEqual(TEXT("no glow"), X->Scalars[TEXT("EmissiveStrength")], 0.f);
    }
    if (auto* X = Check(TEXT("illum_masked_glow"), ESCMaster::Illum, ESCBlend::Masked, false, false))
    {
        TestEqual(TEXT("alpha test"), X->Scalars[TEXT("AlphaTest")], 0.25f);
        TestTrue(TEXT("emissive colour"), Near(X->Vectors[TEXT("EmissiveColor")], FLinearColor(2.f, 1.f, 0.5f)));
        TestEqual(TEXT("glow: MaterialGlowToLinearGlow in cd/m2"), X->Scalars[TEXT("EmissiveStrength")], GlowNits(3.f));
        TestEqual(TEXT("authored Shininess=255 used as authored (no placeholder substitution)"), X->Scalars[TEXT("Smoothness")], 1.f);
        TestEqual(TEXT("masked parent"), FSCMaterialBuilder::ParentPath(X->Master, X->Blend, X->bTwoSided),
            FString(TEXT("/SCBridge/Materials/MI_SC_Illum_Masked.MI_SC_Illum_Masked")));
    }
    if (auto* X = Check(TEXT("illum_decal_sheet"), ESCMaster::MeshDecal, ESCBlend::Translucent, true, true))
    {
        // Illum.cfx %VERTCOLORS on an Illum decal: the Illum rule, not MeshDecal's vertex wear (WP-U2c).
        TestEqual(TEXT("Illum decal: vertex colour multiplies albedo"), X->Scalars.FindRef(TEXT("VertexColorMode"), -1.f), 1.f);
        TestEqual(TEXT("Illum decal: no MeshDecal vertex wear"), X->Scalars.FindRef(TEXT("UseVertexColors"), -1.f), 0.f);
    }
    // Illum.cfx %VERTCOLORS (WP-U2c): albedo and glow x vertex rgb (mode 1); under %USE_DAMAGE_MAP x vertex blue, the
    // glow by green (mode 2); blending opacity x vertex alpha unless %BLENDLAYER. No token, no vertex colour.
    if (const FSCMaterialDesc* X = Find(D, TEXT("illum_spec")))
    {
        TestEqual(TEXT("Illum: vertex rgb"), X->Scalars.FindRef(TEXT("VertexColorMode"), -1.f), 1.f);
        TestEqual(TEXT("Illum: vertex alpha scales opacity"), X->Scalars.FindRef(TEXT("UseVertexAlpha"), -1.f), 1.f);
    }
    if (const FSCMaterialDesc* X = Find(D, TEXT("emissives_like")))
        TestEqual(TEXT("Illum with a damage map: vertex blue is AO"), X->Scalars.FindRef(TEXT("VertexColorMode"), -1.f), 2.f);
    if (const FSCMaterialDesc* X = Find(D, TEXT("illum_masked_glow")))
        TestEqual(TEXT("Illum without %VERTCOLORS: none"), X->Scalars.FindRef(TEXT("VertexColorMode"), -1.f), 0.f);
    if (auto* X = Check(TEXT("hard_primary"), ESCMaster::HardSurface, ESCBlend::Opaque, false, false))
    {
        // snapshot diffuse x layer tint x palette secondary.
        TestTrue(TEXT("layer tint x palette"), Near(X->Vectors[TEXT("Layer0Tint")], FLinearColor(0.25f, 0.25f, 0.125f)));
        TestEqual(TEXT("layer tiling"), X->Scalars[TEXT("Layer0Tiling")], 2.f);
        // paletteLib ApplyPaletteParams, TintMode 0 (diffuse and specular): gloss x palette gloss, F0 = palette specular.
        TestEqual(TEXT("layer gloss x palette gloss"), X->Scalars[TEXT("Layer0GlossMult")], 0.5f * 0.8f);
        TestTrue(TEXT("layer F0 = palette specular"), Near(X->Vectors[TEXT("Layer0Specular")], FLinearColor(0.1f, 0.2f, 0.3f)));
        TestFalse(TEXT("wear layer is not a base layer"), X->Scalars.Contains(TEXT("Layer1Tiling")));
        TestEqual(TEXT("layer roughness"), X->Textures[TEXT("Layer0SmoothnessMap")].Path, FString(TEXT("tex/Primary_ddna_smoothness.dds")));
        // HardSurface.cfx macro surface: %MACRO_NORMAL_MAP = TexSlot3 (+ gloss in its alpha), %MACRO_AO_MAP = TexSlot1,
        // both at baseTC x MacroTiling.
        TestEqual(TEXT("macro normal"), X->Textures[TEXT("MacroNormalMap")].Path, FString(TEXT("tex/hard_macro_ddna.png")));
        TestEqual(TEXT("macro normal role"), int32(X->Textures[TEXT("MacroNormalMap")].Role), int32(ESCTextureRole::Normal));
        TestEqual(TEXT("macro gloss"), X->Textures[TEXT("MacroSmoothnessMap")].Path, FString(TEXT("tex/hard_macro_ddna_smoothness.dds")));
        TestEqual(TEXT("macro gloss on"), X->Scalars[TEXT("UseMacroGloss")], 1.f);
        TestEqual(TEXT("macro tiling"), X->Scalars[TEXT("MacroTiling")], 3.f);
        TestEqual(TEXT("macro AO is TexSlot1 under %MACRO_AO_MAP"), X->Textures[TEXT("MacroAOMap")].Path, FString(TEXT("tex/hard_macro_ao.png")));
        TestEqual(TEXT("macro AO is data, not colour"), int32(X->Textures[TEXT("MacroAOMap")].Role), int32(ESCTextureRole::Linear));
        TestFalse(TEXT("HardSurface has no HAL map"), X->Textures.Contains(TEXT("HalMap")));
        // Static wear (HardSurface.cfx CalcAlphas_BlendBased + BlendLayers_SeparatePrimaryAndWear under %WEAR_LAYER).
        TestEqual(TEXT("%VERTCOLORS: vertex glow / AO / wear are read"), X->Scalars[TEXT("UseVertexColors")], 1.f);
        TestEqual(TEXT("wear amount base"), X->Scalars[TEXT("WearBlendBase")], 0.25f);
        TestEqual(TEXT("blend map = Primary layer TexSlot8"), X->Textures[TEXT("WearBlendMap")].Path, FString(TEXT("tex/Primary_blend.png")));
        TestEqual(TEXT("blend map is data"), int32(X->Textures[TEXT("WearBlendMap")].Role), int32(ESCTextureRole::Linear));
        TestEqual(TEXT("blend softness = Primary WearDirtBlendFalloff"), X->Scalars[TEXT("WearFalloff")], 0.25f);
        TestEqual(TEXT("Primary ShadingModel 2 = paint"), X->Scalars[TEXT("WearPaint")], 1.f);
        TestEqual(TEXT("not cloth"), X->Scalars[TEXT("WearCloth")], 0.f);
        TestTrue(TEXT("complement hue is reported"), X->Warnings.ContainsByPredicate([](const FString& W) { return W.Contains(TEXT("complement hue")); }));
        TestEqual(TEXT("wear layer diffuse"), X->Textures[TEXT("WearDiffuseMap")].Path, FString(TEXT("tex/Wear_diff.png")));
        TestEqual(TEXT("wear layer gloss"), X->Textures[TEXT("WearSmoothnessMap")].Path, FString(TEXT("tex/Wear_ddna_smoothness.dds")));
        TestTrue(TEXT("wear layer tint x palette"), Near(X->Vectors[TEXT("WearTint")], FLinearColor(0.5f, 0.5f, 0.25f)));
        TestTrue(TEXT("wear layer F0 = palette specular"), Near(X->Vectors[TEXT("WearSpecular")], FLinearColor(0.1f, 0.2f, 0.3f)));
        TestFalse(TEXT("no exporter metallic: the energy split is in the master"), X->Scalars.Contains(TEXT("WearMetallic")) || X->Scalars.Contains(TEXT("Layer0Metallic")));
        TestEqual(TEXT("wear layer tiling"), X->Scalars[TEXT("WearTiling")], 1.f);
        TestEqual(TEXT("wear layer gloss x palette gloss"), X->Scalars[TEXT("WearGlossMult")], 0.5f * 0.8f);
        TestEqual(TEXT("%USE_SPECULAR_MAPS: Primary TexSlot6"), X->Textures[TEXT("Layer0SpecularMap")].Path, FString(TEXT("tex/Primary_spec.png")));
        TestEqual(TEXT("%USE_SPECULAR_MAPS: Wear TexSlot6"), X->Textures[TEXT("WearSpecularMap")].Path, FString(TEXT("tex/Wear_spec.png")));
        TestEqual(TEXT("spec map is colour"), int32(X->Textures[TEXT("WearSpecularMap")].Role), int32(ESCTextureRole::Color));
        TestTrue(TEXT("sampled spec maps are not folded into F0"), X->VectorMeans.IsEmpty());
    }
    if (auto* X = Check(TEXT("hard_wear_tinted"), ESCMaster::HardSurface, ESCBlend::Opaque, false, false))
    {
        // No %WEAR_LAYER: BlendLayers_TintedPrimaryWear = the Primary maps with LayerAttribs_ApplyWearTints.
        TestEqual(TEXT("no %VERTCOLORS"), X->Scalars[TEXT("UseVertexColors")], 0.f);
        TestEqual(TEXT("tinted wear base"), X->Scalars[TEXT("WearBlendBase")], 0.4f);
        TestEqual(TEXT("tinted wear diffuse = Primary diffuse"), X->Textures[TEXT("WearDiffuseMap")].Path, X->Textures[TEXT("Layer0DiffuseMap")].Path);
        TestEqual(TEXT("tinted wear normal = Primary normal"), X->Textures[TEXT("WearNormalGlossMap")].Path, X->Textures[TEXT("Layer0NormalGlossMap")].Path);
        TestEqual(TEXT("tinted wear gloss map = Primary gloss map"), X->Textures[TEXT("WearSmoothnessMap")].Path, X->Textures[TEXT("Layer0SmoothnessMap")].Path);
        TestTrue(TEXT("WearDiffuseColor x WearTint"), Near(X->Vectors[TEXT("WearTint")], FLinearColor(0.25f, 0.5f, 0.5f)));
        TestTrue(TEXT("F0 = WearSpecularColor"), Near(X->Vectors[TEXT("WearSpecular")], FLinearColor(0.2f, 0.2f, 0.2f)));
        TestFalse(TEXT("tinted wear has no exporter metallic"), X->Scalars.Contains(TEXT("WearMetallic")));
        TestEqual(TEXT("tinted wear tiling = Primary tiling"), X->Scalars[TEXT("WearTiling")], 3.f);
        TestEqual(TEXT("WearGlossiness x WearGloss"), X->Scalars[TEXT("WearGlossMult")], 0.25f);
        TestEqual(TEXT("tinted wear uses the Primary gloss map"), X->Scalars[TEXT("WearUseSmoothnessMap")], 1.f);
        TestEqual(TEXT("ShadingModel 0: no paint remap"), X->Scalars[TEXT("WearPaint")], 0.f);
        TestFalse(TEXT("no %USE_SPECULAR_MAPS: TexSlot6 is not sampled"), X->Textures.Contains(TEXT("Layer0SpecularMap")) || X->Textures.Contains(TEXT("WearSpecularMap")));
        // UseLayerSpecMap = false: the spec map's mean colour scales the layer F0 instead (and the tinted wear F0,
        // whose surface is the Primary layer's textures).
        const FSCTextureKey* Fold = X->VectorMeans.Find(TEXT("Layer0Specular"));
        const FSCTextureKey* WearFold = X->VectorMeans.Find(TEXT("WearSpecular"));
        TestTrue(TEXT("unsampled spec map: mean x Primary F0"), Fold && Fold->Path == TEXT("tex/tinted_spec.png") && Fold->Role == ESCTextureRole::Color);
        TestTrue(TEXT("unsampled spec map: mean x tinted wear F0"), WearFold && *WearFold == *Fold);
        TestEqual(TEXT("nothing else is folded"), X->VectorMeans.Num(), 2);
    }
    for (const TCHAR* Name : { TEXT("hard_wear_no_blend_map"), TEXT("hard_wear_no_tints") })
        if (auto* X = Check(Name, ESCMaster::HardSurface, ESCBlend::Opaque, false, false))
        {
            // A missing input is reported and leaves wear off; no shader default is guessed.
            TestEqual(FString(Name) + TEXT(": wear off"), X->Scalars[TEXT("WearBlendBase")], 0.f);
            TestTrue(FString(Name) + TEXT(": reported"), X->Warnings.ContainsByPredicate([](const FString& W) { return W.Contains(TEXT("WearBlendBase")); }));
            TestFalse(FString(Name) + TEXT(": no wear parameters"), X->Textures.Contains(TEXT("WearBlendMap")) || X->Vectors.Contains(TEXT("WearTint")));
        }
    if (auto* X = Check(TEXT("blend4"), ESCMaster::Layered, ESCBlend::Opaque, false, false))
    {
        // Layer TintMode 1 (diffuse only): palette diffuse and gloss, the layer keeps its own specular.
        TestTrue(TEXT("layer 0 palette primary"), Near(X->Vectors[TEXT("Layer0Tint")], FLinearColor(0.2f, 0.4f, 0.6f)));
        TestTrue(TEXT("layer 0 diffuse-only keeps the layer F0"), Near(X->Vectors[TEXT("Layer0Specular")], FLinearColor(0.04f, 0.04f, 0.04f)));
        TestEqual(TEXT("layer 0 gloss x palette gloss"), X->Scalars[TEXT("Layer0GlossMult")], 0.5f);
        TestTrue(TEXT("layer 1 unrouted"), Near(X->Vectors[TEXT("Layer1Tint")], FLinearColor::White));
        TestTrue(TEXT("layer 1 F0"), Near(X->Vectors[TEXT("Layer1Specular")], FLinearColor(0.9f, 0.9f, 0.9f)));
        TestEqual(TEXT("layer 1 gloss unscaled"), X->Scalars[TEXT("Layer1GlossMult")], 1.f);
        // Layer TintMode 2 (specular only): no palette diffuse, F0 = palette specular.
        TestTrue(TEXT("layer 2 specular-only keeps its diffuse"), Near(X->Vectors[TEXT("Layer2Tint")], FLinearColor::White));
        TestTrue(TEXT("layer 2 F0 = palette tertiary specular"), Near(X->Vectors[TEXT("Layer2Specular")], FLinearColor(0.6f, 0.5f, 0.4f)));
        // A palette value the export does not carry is reported and leaves the layer's own value.
        TestEqual(TEXT("layer 2 gloss not scaled without a palette gloss"), X->Scalars[TEXT("Layer2GlossMult")], 1.f);
        TestTrue(TEXT("missing palette gloss reported"), X->Warnings.ContainsByPredicate([](const FString& W) { return W.Contains(TEXT("tertiary has no gloss")); }));
        TestFalse(TEXT("no exporter metallic"), X->Scalars.Contains(TEXT("Layer1Metallic")));
        TestFalse(TEXT("layer without textures has no maps"), X->Textures.Contains(TEXT("Layer3DiffuseMap")));
        TestEqual(TEXT("blend mask"), X->Textures[TEXT("BlendMaskMap")].Path, FString(TEXT("tex/blend.png")));
        TestEqual(TEXT("blend mask on"), X->Scalars[TEXT("UseBlendMask")], 1.f);
        TestEqual(TEXT("blend map tiling"), X->Scalars[TEXT("BlendMaskTiling")], 3.f);
        // LayerBlend_V2.cfx: %NORMAL_MAP = material-level TexSlot3 over all layers (TexSlot2 is the POM decal normal),
        // untiled; TexSlot13 = hue / AO / luminance control, sampled unconditionally.
        TestEqual(TEXT("macro normal is TexSlot3, not the lowest normal slot"), X->Textures[TEXT("MacroNormalMap")].Path, FString(TEXT("tex/blend_macro_ddna.png")));
        TestEqual(TEXT("macro gloss"), X->Textures[TEXT("MacroSmoothnessMap")].Path, FString(TEXT("tex/blend_macro_ddna_smoothness.dds")));
        TestEqual(TEXT("macro gloss on"), X->Scalars[TEXT("UseMacroGloss")], 1.f);
        TestEqual(TEXT("LayerBlend macro maps are untiled"), X->Scalars[TEXT("MacroTiling")], 1.f);
        TestEqual(TEXT("HAL map"), X->Textures[TEXT("HalMap")].Path, FString(TEXT("tex/blend_hal.png")));
        TestEqual(TEXT("HAL is data, not colour"), int32(X->Textures[TEXT("HalMap")].Role), int32(ESCTextureRole::Linear));
        TestEqual(TEXT("HAL on"), X->Scalars[TEXT("UseHalMap")], 1.f);
        TestFalse(TEXT("LayerBlend has no macro AO map"), X->Textures.Contains(TEXT("MacroAOMap")));
        // LayerBlend_V2.cfx: WearMap (TexSlot11, R = wear/curvature) is the dirt blend map; %WEAR_DIRT_AO_VERTEX adds
        // vertex wear (R -> dirt) and vertex AO (B) (WP-U2b).
        TestEqual(TEXT("wear map is TexSlot11"), X->Textures.Contains(TEXT("WearMap")) ? X->Textures[TEXT("WearMap")].Path : FString(), FString(TEXT("tex/wear.png")));
        TestEqual(TEXT("wear map is data"), X->Textures.Contains(TEXT("WearMap")) ? int32(X->Textures[TEXT("WearMap")].Role) : -1, int32(ESCTextureRole::Linear));
        TestEqual(TEXT("vertex wear/dirt/AO on with the token"), X->Scalars.FindRef(TEXT("UseWearDirtAOVertex"), -1.f), 1.f);
        // GetAlphaBlendDetailMaps: the engine detail map from the render fixture, at baseUV x detailScale.
        TestEqual(TEXT("dirt detail map from the fixture"), X->Textures.Contains(TEXT("DirtDetailMap")) ? X->Textures[TEXT("DirtDetailMap")].Path : FString(),
                  FSCStarEngine::Get().WearDirtFrostDetailMap);
        TestEqual(TEXT("dirt detail scale from the fixture"), X->Scalars.FindRef(TEXT("DirtDetailScale"), -1.f), float(FSCStarEngine::Get().DirtDetailScale));
        TestEqual(TEXT("dirt details on"), X->Scalars.FindRef(TEXT("UseDirtDetail"), -1.f), 1.f);
    }
    {
        // A palette without `finish` (an export older than the field): both halves are reported, layer values stay.
        FSCPalette Bare = *Palette;
        TSharedPtr<FJsonObject> Json; TestTrue(TEXT("palette json"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Bare.Json), Json) && Json);
        if (Json) { Json->RemoveField(TEXT("finish")); Bare.Json.Reset(); FJsonSerializer::Serialize(Json.ToSharedRef(), TJsonWriterFactory<>::Create(&Bare.Json)); }
        const FSCSubmaterial* Sub = M.Materials[0].Submaterials.FindByPredicate([](const FSCSubmaterial& S) { return S.Name == TEXT("hard_primary"); });
        if (TestNotNull(TEXT("hard_primary submaterial"), Sub))
        {
            const FSCMaterialDesc X = FSCMaterialBuilder::Describe(*Sub, &Bare);
            TestTrue(TEXT("no finish: palette diffuse still applies"), Near(X.Vectors[TEXT("Layer0Tint")], FLinearColor(0.25f, 0.25f, 0.125f)));
            TestTrue(TEXT("no finish: layer F0 kept"), Near(X.Vectors[TEXT("Layer0Specular")], FLinearColor(0.04f, 0.04f, 0.04f)));
            TestEqual(TEXT("no finish: gloss unscaled"), X.Scalars[TEXT("Layer0GlossMult")], 0.5f);
            TestTrue(TEXT("no finish: specular reported"), X.Warnings.ContainsByPredicate([](const FString& W) { return W.Contains(TEXT("no specular tint")); }));
            TestTrue(TEXT("no finish: gloss reported"), X.Warnings.ContainsByPredicate([](const FString& W) { return W.Contains(TEXT("no gloss")); }));
        }
    }
    if (auto* X = Check(TEXT("blend1"), ESCMaster::Layered, ESCBlend::Opaque, false, false))
    {
        TestEqual(TEXT("unused slots repeat the last base layer"), X->Scalars[TEXT("Layer3Tiling")], 8.f);
        TestEqual(TEXT("repeated texture"), X->Textures[TEXT("Layer2DiffuseMap")].Path, FString(TEXT("tex/BaseLayer1_diff.png")));
        // LayerBlend_V2 has no layer spec sampler: a layer's TexSlot6 is never bound, its mean scales the layer F0.
        const FSCTextureKey* Fold = X->VectorMeans.Find(TEXT("Layer0Specular"));
        TestTrue(TEXT("LayerBlend spec map: mean x layer F0"), Fold && Fold->Path == TEXT("tex/BaseLayer1_spec.png"));
        TestTrue(TEXT("repeated layers repeat the fold"), X->VectorMeans.Contains(TEXT("Layer3Specular")));
        TestFalse(TEXT("LayerBlend spec map is not a texture parameter"), X->Textures.Contains(TEXT("Layer0SpecularMap")));
        TestEqual(TEXT("no mask -> layer 0"), X->Scalars[TEXT("UseBlendMask")], 0.f);
        TestFalse(TEXT("no macro normal"), X->Textures.Contains(TEXT("MacroNormalMap")));
        TestEqual(TEXT("no macro gloss"), X->Scalars[TEXT("UseMacroGloss")], 0.f);
        TestEqual(TEXT("no HAL"), X->Scalars[TEXT("UseHalMap")], 0.f);
        TestFalse(TEXT("no wear map"), X->Textures.Contains(TEXT("WearMap")));
        TestEqual(TEXT("no vertex wear/dirt/AO without the token"), X->Scalars.FindRef(TEXT("UseWearDirtAOVertex"), -1.f), 0.f);
    }
    if (auto* X = Check(TEXT("blend_palette_fold"), ESCMaster::Layered, ESCBlend::Opaque, false, false))
    {
        // paletteLib ApplyPaletteParams: `Smoothness *= layerGloss * gloss` is unconditional, so the palette gloss
        // (0.8) and the layer GlossMult (0.5) also scale a layer without an authored Shininess or a gloss map.
        TestEqual(TEXT("no gloss map"), X->Scalars[TEXT("Layer0UseSmoothnessMap")], 0.f);
        TestEqual(TEXT("no Shininess: default smoothness x layer gloss x palette gloss"), X->Scalars[TEXT("Layer0Smoothness")], 0.5f * 0.5f * 0.8f);
        // The sampling shader multiplies the spec map (LayerAttribs_ReadSpecular) before ApplyTintsAndPalette picks
        // the palette or the layer specular, so the map scales whichever F0 the palette rule selects: the mean
        // stands in for the sample and folds into the palette specular too.
        TestTrue(TEXT("TintMode 0: F0 = palette specular"), Near(X->Vectors[TEXT("Layer0Specular")], FLinearColor(0.1f, 0.2f, 0.3f)));
        const FSCTextureKey* Fold = X->VectorMeans.Find(TEXT("Layer0Specular"));
        TestTrue(TEXT("unsampled spec map: mean x palette-selected F0"), Fold && Fold->Path == TEXT("tex/BaseLayer1_spec.png"));
        if (Fold)
        {
            FSCMaterialDesc Folded = *X;
            FSCMaterialBuilder::ApplyTextureMeans(Folded, {{*Fold, FLinearColor(0.5f, 0.5f, 0.25f)}});
            TestTrue(TEXT("palette specular x spec map mean"), Near(Folded.Vectors[TEXT("Layer0Specular")], FLinearColor(0.05f, 0.1f, 0.075f)));
        }
    }
    // GlassPBR (glassLib.cfib GlassShared): the transmissive colour is MatDifColor, moved to the palette glass entry
    // by GlassTintStrength only under %USE_TINT_PALETTE; reflectance is MatSpecColor.r; the authored Opacity is never
    // read. Thickness (layer Height) is the vertex blue channel under %PER_VERTEX_CURVATURE times the %HEIGHT_MAP.
    const FSCStarEngine& Star = FSCStarEngine::Get();
    auto GlassRule = [&](const FSCMaterialDesc& X, bool bVertexHeight, bool bVertexTint, bool bHeightMap, bool bTintMap, bool bThinFilm)
    {
        // Without %THIN_FILM the game's Fresnel is lerp(Reflectance, 1, Schlick): the master must bypass UE's thin-film
        // node, which is not an identity at zero thickness (it adds a film interface and raises F0 several-fold).
        TestEqual(X.Name + TEXT(" %THIN_FILM switches the film"), X.Scalars.FindRef(TEXT("UseThinFilm"), -1.f), bThinFilm ? 1.f : 0.f);
        TestEqual(X.Name + TEXT(" density scale is the game's"), X.Scalars[TEXT("GlassDensityScale")], float(Star.GlassDensityScale));
        TestEqual(X.Name + TEXT(" colour floor is the game's"), X.Scalars[TEXT("GlassColorFloor")], float(Star.GlassColourFloor));
        TestEqual(X.Name + TEXT(" vertex height bias is the game's"), X.Scalars[TEXT("GlassVertexHeightBias")], float(Star.GlassVertexHeightBias));
        TestEqual(X.Name + TEXT(" vertex height scale is the game's"), X.Scalars[TEXT("GlassVertexHeightScale")], float(Star.GlassVertexHeightScale));
        TestEqual(X.Name + TEXT(" height clip is the game's"), X.Scalars[TEXT("GlassHeightClip")], float(Star.GlassHeightClip));
        TestEqual(X.Name + TEXT(" %PER_VERTEX_CURVATURE"), X.Scalars[TEXT("UseVertexHeight")], bVertexHeight ? 1.f : 0.f);
        TestEqual(X.Name + TEXT(" %PER_VERTEX_TINT"), X.Scalars[TEXT("UseVertexTint")], bVertexTint ? 1.f : 0.f);
        TestEqual(X.Name + TEXT(" %HEIGHT_MAP"), X.Scalars[TEXT("UseHeightMap")], bHeightMap ? 1.f : 0.f);
        TestEqual(X.Name + TEXT(" %TINT_MAP"), X.Scalars[TEXT("UseTintMap")], bTintMap ? 1.f : 0.f);
        TestFalse(X.Name + TEXT(" authored Opacity is not glass coverage"), X.Scalars.Contains(TEXT("Opacity")));
        TestFalse(X.Name + TEXT(" no tint lerp left to the master"), X.Scalars.Contains(TEXT("GlassTintStrength")) || X.Vectors.Contains(TEXT("GlassTint")));
        // glassUV = baseUV * GlassTiling, then v * GlassVStretch: the glass maps never use the generic Tiling.
        TestTrue(X.Name + TEXT(" glass UV parameters"), X.Scalars.Contains(TEXT("GlassTiling")) && X.Scalars.Contains(TEXT("GlassVStretch")) && !X.Scalars.Contains(TEXT("Tiling")));
    };
    auto Warned = [](const FSCMaterialDesc& X, const TCHAR* What) { return X.Warnings.ContainsByPredicate([What](const FString& W) { return W.Contains(What); }); };
    if (auto* X = Check(TEXT("glass_ext"), ESCMaster::Glass, ESCBlend::Translucent, false, true))
    {
        GlassRule(*X, false, false, false, false, true);
        TestTrue(TEXT("%USE_TINT_PALETTE: Diffuse moved to the palette glass colour by GlassTintStrength"),
                 Near(X->Vectors[TEXT("GlassColor")], FLinearColor(0.45f, 0.55f, 0.275f)));
        TestTrue(TEXT("glass reflectance = Specular.r"), Near(X->Vectors[TEXT("SpecularColor")], FLinearColor(0.05f, 0.05f, 0.05f)));
        TestEqual(TEXT("thin film"), X->Scalars[TEXT("ThinFilmThickness")], 0.06f);
        TestEqual(TEXT("unset GlassTiling is the shader default"), X->Scalars[TEXT("GlassTiling")], float(Star.GlassTilingDefault));
        TestEqual(TEXT("unset GlassVStretch is the shader default"), X->Scalars[TEXT("GlassVStretch")], float(Star.GlassVStretchDefault));
        TestEqual(TEXT("complete glass: nothing reported"), X->Warnings.Num(), 0);
        TestEqual(TEXT("glass roughness"), X->Textures[TEXT("SmoothnessMap")].Path, FString(TEXT("tex/glass_ddna_smoothness.dds")));
        // The game leaves the colour alone when the instance carries no palette (paletteIndex == 0).
        const FSCMaterialDesc Bare = FSCMaterialBuilder::Describe(M.Materials[0].Submaterials[int32(X - D.GetData())], nullptr);
        TestTrue(TEXT("no palette: the authored Diffuse"), Near(Bare.Vectors[TEXT("GlassColor")], FLinearColor(0.8f, 0.5f, 0.25f)));
    }
    if (auto* X = Check(TEXT("glass_thickness"), ESCMaster::Glass, ESCBlend::Translucent, false, true))
    {
        GlassRule(*X, true, false, true, false, false);
        TestTrue(TEXT("no %USE_TINT_PALETTE: a routed palette channel does not tint"), Near(X->Vectors[TEXT("GlassColor")], FLinearColor(0.2f, 0.4f, 0.6f)));
        TestEqual(TEXT("height map is TexSlot8"), X->Textures[TEXT("HeightMap")].Path, FString(TEXT("tex/glass_height.png")));
        TestEqual(TEXT("height map is linear"), int32(X->Textures[TEXT("HeightMap")].Role), int32(ESCTextureRole::Linear));
        TestEqual(TEXT("authored GlassTiling"), X->Scalars[TEXT("GlassTiling")], 2.f);
        TestEqual(TEXT("authored GlassVStretch"), X->Scalars[TEXT("GlassVStretch")], 3.f);
        TestEqual(TEXT("no %THIN_FILM: no film"), X->Scalars[TEXT("ThinFilmThickness")], 0.f);
    }
    if (auto* X = Check(TEXT("glass_vertex_tint"), ESCMaster::Glass, ESCBlend::Translucent, false, true))
    {
        GlassRule(*X, false, true, false, true, false);
        TestEqual(TEXT("tint map is TexSlot4"), X->Textures[TEXT("TintMap")].Path, FString(TEXT("tex/glass_tint.png")));
        TestEqual(TEXT("tint map is colour"), int32(X->Textures[TEXT("TintMap")].Role), int32(ESCTextureRole::Color));
    }
    if (auto* X = Check(TEXT("glass_bare"), ESCMaster::Glass, ESCBlend::Translucent, false, true))
    {
        // What the material leaves out is the shader's own default, and what the export does not carry is reported:
        // the feature would otherwise go missing without a trace.
        GlassRule(*X, false, false, false, false, true);
        TestEqual(TEXT("%THIN_FILM without ThinFilmThickness: the shader default"), X->Scalars[TEXT("ThinFilmThickness")], float(Star.GlassThinFilmThicknessDefault));
        TestTrue(TEXT("%HEIGHT_MAP without an exported TexSlot8 is reported"), Warned(*X, TEXT("TexSlot8")));
        TestTrue(TEXT("%TINT_MAP without an exported TexSlot4 is reported"), Warned(*X, TEXT("TexSlot4")));
        TestTrue(TEXT("glass without Specular is reported"), Warned(*X, TEXT("Specular")));
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
    {
        TestEqual(TEXT("colour decal parent"), FSCMaterialBuilder::ParentPath(X->Master, X->Blend, X->bTwoSided),
            FString(TEXT("/SCBridge/Materials/MI_SC_MeshDecal_TwoSided.MI_SC_MeshDecal_TwoSided")));
        // MeshDecal.cfx dirt (WP-U2b): BlendTexture (the exported breakup / wear mask) at baseTC x WearTiling, vertex
        // wear under %VERTCOLORS (CalculateVertexDirt), AO 1.
        TestEqual(TEXT("decal dirt blend map"), X->Textures.Contains(TEXT("DirtBlendMap")) ? X->Textures[TEXT("DirtBlendMap")].Path : FString(), FString(TEXT("tex/decal_breakup.png")));
        TestEqual(TEXT("decal dirt blend map on"), X->Scalars.FindRef(TEXT("UseDirtBlendMap"), -1.f), 1.f);
        TestEqual(TEXT("decal dirt blend tiling = WearTiling"), X->Scalars.FindRef(TEXT("DirtBlendTiling"), -1.f), 4.f);
        TestEqual(TEXT("decal vertex wear on with %VERTCOLORS"), X->Scalars.FindRef(TEXT("UseVertexColors"), -1.f), 1.f);
    }
    if (auto* X = Check(TEXT("stencil_palette"), ESCMaster::MeshDecal, ESCBlend::Translucent, true, true))
    {
        TestEqual(TEXT("no blend texture: the neutral mask"), X->Scalars.FindRef(TEXT("UseDirtBlendMap"), -1.f), 0.f);
        TestEqual(TEXT("no WearTiling: 1"), X->Scalars.FindRef(TEXT("DirtBlendTiling"), -1.f), 1.f);
    }
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
        TestEqual(TEXT("card strength = unit instance glow"), X->Scalars[TEXT("EmissiveStrength")], UnitGlowNits());
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
        // Tint applied once: unit instance glow, Emissive only as colour.
        TestEqual(TEXT("tinted card strength = unit instance glow"), X->Scalars[TEXT("EmissiveStrength")], UnitGlowNits());
        TestTrue(TEXT("tinted card colour = Emissive"), Near(X->Vectors[TEXT("EmissiveColor")], FLinearColor(0.2f, 0.4f, 0.9f)));
    }
    if (auto* X = Check(TEXT("v2_object"), ESCMaster::Illum, ESCBlend::Masked, true, false))
    {
        // v2 objects win over attributes: strength, tint, cutoff, alpha_test, two_sided.
        TestTrue(TEXT("described from v2"), X->bV2);
        TestEqual(TEXT("v2 strength"), X->Scalars[TEXT("EmissiveStrength")], GlowNits(2.f));
        TestTrue(TEXT("v2 tint"), Near(X->Vectors[TEXT("EmissiveColor")], FLinearColor(0.1f, 0.2f, 0.3f)));
        TestEqual(TEXT("v2 glow cutoff"), X->Scalars[TEXT("AlphaGlowCutoff")], 0.4f);
        TestEqual(TEXT("v2 alpha_test"), X->Scalars[TEXT("AlphaTest")], 0.2f);
        const FSCMaterialDesc V1 = FSCMaterialBuilder::Describe(*M.Materials[0].Submaterials.FindByPredicate([](const FSCSubmaterial& S) { return S.Name == TEXT("v2_object"); }), Palette, false);
        TestEqual(TEXT("v1 attribute path for comparison emits nothing"), V1.Scalars.FindRef(TEXT("EmissiveStrength")), 0.f);
    }
    if (auto* X = Check(TEXT("emissives_like"), ESCMaster::EmissiveUnlit, ESCBlend::Translucent, false, true))
    {
        // Emission colour = Emissive x Diffuse (x texture in the master): blue ordering, strength = the glow rule of Glow.
        const FLinearColor Glow = X->Vectors[TEXT("EmissiveColor")] * X->Vectors[TEXT("DiffuseColor")];
        TestTrue(TEXT("emissives glow blue (b > g > r)"), Glow.B > Glow.G && Glow.G > Glow.R && Glow.R > 0.f);
        TestEqual(TEXT("emissives strength"), X->Scalars[TEXT("EmissiveStrength")], GlowNits(0.45f));
    }
    if (auto* X = Check(TEXT("glows_like"), ESCMaster::Illum, ESCBlend::Opaque, false, false))
    {
        // Glow > 0 with black Emissive: no tint, the orange surface colour emits at Glow strength.
        const FLinearColor Glow = X->Vectors[TEXT("EmissiveColor")] * X->Vectors[TEXT("DiffuseColor")];
        TestTrue(TEXT("glows emit orange (r > g > b)"), Glow.R > Glow.G && Glow.G > Glow.B);
        TestEqual(TEXT("glows strength"), X->Scalars[TEXT("EmissiveStrength")], GlowNits(0.35f));
    }
    if (auto* X = Check(TEXT("emittance_noglow"), ESCMaster::Illum, ESCBlend::Opaque, false, false))
    {
        TestEqual(TEXT("emittance map without Glow -> unit instance glow"), X->Scalars[TEXT("EmissiveStrength")], UnitGlowNits());
        TestTrue(TEXT("Emissive tints the emittance"), Near(X->Vectors[TEXT("EmissiveColor")], FLinearColor(0.3f, 0.6f, 0.9f)));
    }
    if (auto* X = Check(TEXT("layerblend_like"), ESCMaster::Layered, ESCBlend::Opaque, false, false))
    {
        TestEqual(TEXT("LayerBlend Emissive 1,1,1 without Glow / forward flag emits nothing"), X->Scalars[TEXT("EmissiveStrength")], 0.f);
        TestFalse(TEXT("TexSlot3 without %NORMAL_MAP is not sampled"), X->Textures.Contains(TEXT("MacroNormalMap")));
        TestEqual(TEXT("no token -> no macro gloss"), X->Scalars[TEXT("UseMacroGloss")], 0.f);
    }
    if (auto* X = Check(TEXT("glow_zero"), ESCMaster::Illum, ESCBlend::Opaque, false, false))
    {
        TestEqual(TEXT("Glow = 0 with Emissive = 1,1,1 emits nothing"), X->Scalars[TEXT("EmissiveStrength")], 0.f);
        // Masters: Roughness = 1 - smoothness (game GlossToRoughness), so the Substrate default roughness 0.5 is smoothness 0.5.
        TestEqual(TEXT("no Shininess -> Substrate default roughness"), X->Scalars[TEXT("Smoothness")], 0.5f);
    }
    if (auto* X = Check(TEXT("glow_emittance"), ESCMaster::Illum, ESCBlend::Opaque, false, false))
    {
        TestEqual(TEXT("AlphaGlowCutoff from public_params"), X->Scalars[TEXT("AlphaGlowCutoff")], 0.25f);
        TestEqual(TEXT("%EMITTANCE_MAP routes the base-colour slot"), X->Textures[TEXT("EmissiveMap")].Path, FString(TEXT("tex/emit.png")));
        TestEqual(TEXT("emittance map on"), X->Scalars[TEXT("UseEmissiveMap")], 1.f);
        TestEqual(TEXT("emittance strength"), X->Scalars[TEXT("EmissiveStrength")], GlowNits(1.5f));
    }
    // HardSurface %STENCIL_MAP (stencilLib.cfib, non-sticker): TexSlot7 at (UV0 | UV1 by UseUV2ForStencil) x
    // StencilTiling tints the Primary layer; colours by StencilTintOverride (paletteLib GetPaletteStencilColors):
    // 0 authored StencilDiffuseColor{,2,3}, 1 the palette's primary/secondary/tertiary, 2 its decal red/green/blue.
    {
        struct FCase { const TCHAR* Name; float UV2; FLinearColor C1, C2, C3; };
        const FCase Cases[] = {
            {TEXT("hard_stencil_authored"), 0.f, FLinearColor(0.9f, 0.8f, 0.7f), FLinearColor(0.6f, 0.5f, 0.4f), FLinearColor(0.3f, 0.2f, 0.1f)},
            {TEXT("hard_stencil_main_tints"), 1.f, FLinearColor(0.2f, 0.4f, 0.6f), FLinearColor(0.5f, 0.5f, 0.25f), FLinearColor(0.9f, 0.1f, 0.1f)},
            {TEXT("hard_stencil_decal_colours"), 0.f, FLinearColor(0.7f, 0.2f, 0.1f), FLinearColor(0.15f, 0.65f, 0.25f), FLinearColor(0.05f, 0.1f, 0.55f)},
        };
        for (const FCase& C : Cases)
            if (auto* X = Check(C.Name, ESCMaster::HardSurface, ESCBlend::Opaque, false, false))
            {
                const FString N(C.Name);
                TestEqual(N + TEXT(": stencil on"), X->Scalars.FindRef(TEXT("UseStencil")), 1.f);
                TestEqual(N + TEXT(": stencil = TexSlot7"), X->Textures.FindRef(TEXT("StencilMap")).Path, FString(TEXT("tex/camo_stencil.png")));
                TestEqual(N + TEXT(": stencil is data"), int32(X->Textures.FindRef(TEXT("StencilMap")).Role), int32(ESCTextureRole::Linear));
                TestEqual(N + TEXT(": StencilTiling"), X->Scalars.FindRef(TEXT("StencilTiling")), 0.75f);
                TestEqual(N + TEXT(": StencilOpacity"), X->Scalars.FindRef(TEXT("StencilOpacity")), 0.8f);
                TestEqual(N + TEXT(": UseUV2ForStencil"), X->Scalars.FindRef(TEXT("StencilUV2"), -1.f), C.UV2);
                TestTrue(N + TEXT(": colour 1"), Near(X->Vectors.FindRef(TEXT("StencilColor1")), C.C1));
                TestTrue(N + TEXT(": colour 2"), Near(X->Vectors.FindRef(TEXT("StencilColor2")), C.C2));
                TestTrue(N + TEXT(": colour 3"), Near(X->Vectors.FindRef(TEXT("StencilColor3")), C.C3));
            }
        if (const FSCMaterialDesc* X = Find(D, TEXT("hard_primary")))
            TestEqual(TEXT("HardSurface without %STENCIL_MAP: no stencil"), X->Scalars.FindRef(TEXT("UseStencil"), -1.f), 0.f);
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
    // Character families (WP-P2 round 2): plain Illum mapping, no fallback warning.
    if (auto* X = Check(TEXT("skin"), ESCMaster::Illum, ESCBlend::Opaque, false, false))
    {
        TestEqual(TEXT("skin: no fallback warning"), X->Warnings.Num(), 0);
        TestTrue(TEXT("skin: base colour map"), X->Textures.Contains(TEXT("DiffuseMap")));
        TestTrue(TEXT("skin: authored diffuse"), Near(X->Vectors[TEXT("DiffuseColor")], FLinearColor(0.6f, 0.5f, 0.4f)));
    }
    if (auto* X = Check(TEXT("hair_cards"), ESCMaster::Illum, ESCBlend::Masked, false, false))
    {
        TestEqual(TEXT("hair: opacity map is linear coverage"), int32(X->Textures[TEXT("OpacityMap")].Role), int32(ESCTextureRole::Linear));
        // Melanin 0.5, no redness, no dye: UE GetHairColorFromMelanin -> exp(-sqrt(ln2 * sigma_eu) * D).
        const float Dc = 5.969f - 0.215f * 0.3f + 2.532f * 0.09f - 10.73f * 0.027f + 5.574f * 0.0081f + 0.245f * 0.00243f;
        const FLinearColor Expected(FMath::Exp(-FMath::Sqrt(FMath::Loge(2.f) * 0.506f) * Dc),
            FMath::Exp(-FMath::Sqrt(FMath::Loge(2.f) * 0.841f) * Dc), FMath::Exp(-FMath::Sqrt(FMath::Loge(2.f) * 1.653f) * Dc));
        TestTrue(TEXT("hair: colour from melanin"), X->Vectors[TEXT("DiffuseColor")].Equals(Expected, 1e-3f));
        TestTrue(TEXT("hair: brown (R > G > B)"), Expected.R > Expected.G && Expected.G > Expected.B);
        TestEqual(TEXT("hair: UE default clip"), X->Scalars[TEXT("AlphaTest")], GetDefault<UMaterial>()->OpacityMaskClipValue);
    }
    if (auto* X = Check(TEXT("hair_cap"), ESCMaster::Illum, ESCBlend::Opaque, false, false))
        TestTrue(TEXT("hair cap without melanin keeps the authored diffuse"), Near(X->Vectors[TEXT("DiffuseColor")], FLinearColor::Black));
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
        // An effect-system Glow is the per-instance glow term (linear scene units), not an authored material Glow.
        TestEqual(TEXT("effect Glow is linear instance glow"), FSCMaterialBuilder::EngineGlowToEmissive(Glow, 50.f), 50.f * UnitGlowNits());
        TestEqual(TEXT("clamped to the control minimum"), FSCMaterialBuilder::EngineGlowToEmissive(Glow, 1.f), 10.f * UnitGlowNits());
        TestEqual(TEXT("clamped to the control maximum"), FSCMaterialBuilder::EngineGlowToEmissive(Glow, 5000.f), 1000.f * UnitGlowNits());
    }
    {
        // Texture means: RGB x mean, alpha untouched; an unavailable mean is reported and leaves the value alone.
        FSCMaterialDesc Fold;
        const FSCTextureKey Have{TEXT("have.png"), ESCTextureRole::Color}, Lost{TEXT("lost.png"), ESCTextureRole::Color};
        Fold.Vectors.Add(TEXT("A"), FLinearColor(0.5f, 1.f, 1.f, 1.f)); Fold.Vectors.Add(TEXT("B"), FLinearColor(1.f, 1.f, 1.f, 1.f));
        Fold.VectorMeans.Add(TEXT("A"), Have); Fold.VectorMeans.Add(TEXT("B"), Lost);
        FSCMaterialBuilder::ApplyTextureMeans(Fold, {{Have, FLinearColor(0.1f, 0.2f, 0.3f, 0.f)}});
        TestTrue(TEXT("vector x texture mean"), Near(Fold.Vectors[TEXT("A")], FLinearColor(0.05f, 0.2f, 0.3f)) && Fold.Vectors[TEXT("A")].A == 1.f);
        TestTrue(TEXT("missing mean keeps the value"), Near(Fold.Vectors[TEXT("B")], FLinearColor::White));
        TestTrue(TEXT("missing mean is reported"), Fold.Warnings.Num() == 1 && Fold.Warnings[0].Contains(TEXT("lost.png")));
        TestTrue(TEXT("means are applied once"), Fold.VectorMeans.IsEmpty());
    }
    FSCImportMaterials Phase; Phase.Describe(M);
    FSCFolderPackageSource Empty(FixtureDir()); // fixtures carry no PNGs: every decode fails, counts stay exact
    FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    Phase.Decode(Empty);
    // Unique (path, role) pairs across all submaterials; tex/illum_ddna.png etc. are shared and deduplicated. Static
    // wear adds 7: the hard_primary Wear layer's three maps, its two layer spec maps and blend map, and the
    // hard_wear_tinted blend map (its wear surface reuses the Primary maps). Two more are read for their mean
    // colour only: the unsampled spec maps of hard_wear_tinted and blend1. GlassPBR adds 2: the glass_thickness
    // height map and the glass_vertex_tint tint map.
    TestEqual(TEXT("deduplicated texture requests"), Phase.TextureStats.Requested, 48); // + Layered WearMap, the decal DirtBlendMap, the engine detail map (WP-U2b), the HardSurface stencil
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
            // Schema v2 emittance maps can emit without a Glow attribute (e.g. MACRO_EMISSIVE_MAP).
            // Check their authored strength directly; the legacy no-Glow invariant does not apply.
            TSharedPtr<FJsonObject> Json;
            const TSharedPtr<FJsonObject>* Emissive;
            FString Emittance;
            double AuthoredStrength = 0;
            if (D.bV2 && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Sub.Json), Json)
                && Json->TryGetObjectField(TEXT("emissive"), Emissive)
                && (*Emissive)->TryGetStringField(TEXT("emittance_texture"), Emittance) && !Emittance.IsEmpty()
                && (*Emissive)->TryGetNumberField(TEXT("strength"), AuthoredStrength))
            {
                double AuthoredGlow = 0; // absent = 0 (decomposed-export-contract.md)
                (*Emissive)->TryGetNumberField(TEXT("glow"), AuthoredGlow);
                TestEqual(Where + TEXT(" emittance map uses the authored v2 glow, or unit instance glow without one"),
                    D.Scalars.FindRef(TEXT("EmissiveStrength")),
                    D.bHidden ? 0.f : AuthoredGlow > 0 ? GlowNits(AuthoredGlow) : float(AuthoredStrength) * UnitGlowNits());
                Glowing += AuthoredStrength > 0;
                continue;
            }
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
                // A black palette/base colour may legitimately suppress emission. Validate the
                // authored tint instead of requiring its product with every palette to be nonblack.
                const FLinearColor Tint = D.Vectors.FindRef(TEXT("EmissiveColor"));
                const TArray<TSharedPtr<FJsonValue>>* Color;
                if (D.bV2 && Json && Json->TryGetObjectField(TEXT("emissive"), Emissive)
                    && (*Emissive)->TryGetArrayField(TEXT("color"), Color) && Color->Num() == 3)
                {
                    const FLinearColor Expected((*Color)[0]->AsNumber(), (*Color)[1]->AsNumber(), (*Color)[2]->AsNumber());
                    TestTrue(Where + TEXT(" emission tint follows the authored v2 colour"), Tint.Equals(Expected, 1.e-4f));
                }
                else TestTrue(Where + TEXT(" emitting legacy material has a tint"), Tint.R + Tint.G + Tint.B > 0.f);
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
                // Saved flag, not CheckMaterialUsage: a -game session cannot set it and would draw WorldGrid.
                TestTrue(Path + TEXT(" usable on skeletal meshes"), Parent->GetMaterial()->GetUsageByFlag(MATUSAGE_SkeletalMesh));
                if (Master == ESCMaster::Glass || Master == ESCMaster::Translucent || Master == ESCMaster::EmissiveUnlit)
                    TestTrue(Path + TEXT(" is translucent"), IsTranslucentBlendMode(Parent->GetBlendMode()));
#if WITH_EDITOR
                if (Master == ESCMaster::Glass)
                {
                    // A translucent Substrate slab transmits only as a simple volume: the default diffusion type is
                    // demoted to no sub-surface (Substrate.ush) and the mean free path is then ignored.
                    int32 Slabs = 0;
                    for (const UMaterialExpression* E : Parent->GetMaterial()->GetExpressions())
                        if (const auto* Slab = Cast<UMaterialExpressionSubstrateSlabBSDF>(E))
                        { ++Slabs; TestEqual(Path + TEXT(" slab is a simple volume"), int32(Slab->SubSurfaceType), int32(MSS_SimpleVolume)); }
                    TestEqual(Path + TEXT(" one glass slab"), Slabs, 1);
                    TestEqual(Path + TEXT(" coloured transmittance"), int32(Parent->GetBlendMode()), int32(BLEND_TranslucentColoredTransmittance));
                    TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Ids;
                    Parent->GetAllScalarParameterInfo(Infos, Ids);
                    auto Has = [&Infos](FName Name) { return Infos.ContainsByPredicate([Name](const FMaterialParameterInfo& I) { return I.Name == Name; }); };
                    TestTrue(Path + TEXT(" glass UVs: GlassTiling and GlassVStretch, no generic Tiling"),
                             Has(TEXT("GlassTiling")) && Has(TEXT("GlassVStretch")) && !Has(TEXT("Tiling")));
                    TestTrue(Path + TEXT(" thin film behind its %THIN_FILM switch"), Has(TEXT("UseThinFilm")));
                }
#endif
            }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCMaterialsMasterParameters, "SCBridge.Materials.MasterParameters", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCMaterialsMasterParameters::RunTest(const FString&)
{
    // Every parameter the describe code writes must exist in the chosen master: a MID silently drops a name its
    // graph lacks, so the authored value would never reach the shader. (Presence only; graph output is GPU-verified.)
    FSCSceneManifest M; if (!ReadFixture(*this, M)) return false;
    int32 Checked = 0, Dropped = 0, Layered = 0;
    for (const auto& Sub : M.Materials[0].Submaterials)
    {
        const FSCMaterialDesc D = FSCMaterialBuilder::Describe(Sub, &M.Palettes[0]);
        const FString Path = FSCMaterialBuilder::ParentPath(D.Master, D.Blend, D.bTwoSided);
        UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *Path);
        if (!TestNotNull(D.Name + TEXT(" parent ") + Path, Parent)) continue;
        auto Names = [&](auto Getter)
        {
            TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Ids; (Parent->*Getter)(Infos, Ids);
            TSet<FName> Out; for (const FMaterialParameterInfo& I : Infos) Out.Add(I.Name); return Out;
        };
        const TSet<FName> Scalars = Names(&UMaterialInterface::GetAllScalarParameterInfo);
        const TSet<FName> Vectors = Names(&UMaterialInterface::GetAllVectorParameterInfo);
        const TSet<FName> Textures = Names(&UMaterialInterface::GetAllTextureParameterInfo);
        // Strict for the layered family; the other families share describe code that writes a common parameter set,
        // so their drops are only counted (ue/verification/WP-U2.md, open issues).
        const bool bStrict = D.Master == ESCMaster::HardSurface || D.Master == ESCMaster::Layered;
        auto Check = [&](const TCHAR* Kind, FName Key, const TSet<FName>& Have)
        {
            if (Have.Contains(Key)) { ++Checked; return; }
            if (bStrict) AddError(FString::Printf(TEXT("%s %s %s is missing from %s"), *D.Name, Kind, *Key.ToString(), *Path));
            else ++Dropped;
        };
        for (const auto& KV : D.Scalars) Check(TEXT("scalar"), KV.Key, Scalars);
        for (const auto& KV : D.Vectors) Check(TEXT("vector"), KV.Key, Vectors);
        for (const auto& KV : D.Textures) Check(TEXT("texture"), KV.Key, Textures);
        Layered += bStrict;
    }
    AddInfo(FString::Printf(TEXT("MASTER_PARAMETERS present=%d layered_family=%d dropped_other_families=%d"), Checked, Layered, Dropped));
    return TestTrue(TEXT("fixture has layered-family materials"), Layered > 0);
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
    {
        // Mean colour of the top mip in linear light: black + white is 0.5 when sRGB-encoded, 127.5/255 as data.
        FLinearColor SrgbMean, LinearMean, None;
        TestTrue(TEXT("sRGB mean"), FSCTextureImporter::MeanColor(BWColor, SrgbMean) && FMath::IsNearlyEqual(SrgbMean.R, 0.5f, 1e-4f) && SrgbMean.R == SrgbMean.B);
        TestTrue(TEXT("linear mean"), FSCTextureImporter::MeanColor(BWLinear, LinearMean) && FMath::IsNearlyEqual(LinearMean.G, 0.5f, 1e-4f));
        TestFalse(TEXT("no texels, no mean"), FSCTextureImporter::MeanColor(FSCDecodedTexture(), None));
    }
    // Colour encoding: the PNG states it (the exporter copies the DDS's sRGB state); the role is only the fallback.
    auto Tagged = [&](const TCHAR* Name, const ANSICHAR* Type, TArray<uint8> Data)
    {
        TArray<uint8> Chunk; const uint32 Len = Data.Num();
        Chunk.Append({uint8(Len >> 24), uint8(Len >> 16), uint8(Len >> 8), uint8(Len)});
        TArray<uint8> Body; Body.Append(reinterpret_cast<const uint8*>(Type), 4); Body.Append(Data);
        const uint32 Crc = FCrc::MemCrc32(Body.GetData(), Body.Num());
        Chunk.Append(Body); Chunk.Append({uint8(Crc >> 24), uint8(Crc >> 16), uint8(Crc >> 8), uint8(Crc)});
        TArray<uint8> File(BWBytes.GetData(), int32(BWBytes.Num()));
        File.Insert(Chunk, 8 + 25); // after the signature and IHDR
        FFileHelper::SaveArrayToFile(File, *(Dir / Name));
    };
    Tagged(TEXT("bw_srgb.png"), "sRGB", {0});
    Tagged(TEXT("bw_linear.png"), "gAMA", {0x00, 0x01, 0x86, 0xA0}); // 100000 = gamma 1.0
    FSCDecodedTexture SrgbMask, LinearColor, SrgbNormal;
    if (FSCTextureImporter::Decode(Source, {TEXT("bw_srgb.png"), ESCTextureRole::Linear}, SrgbMask) && SrgbMask.Mips.Num() == 2)
    {
        TestTrue(TEXT("sRGB chunk overrides the Linear role"), SrgbMask.bSRGB);
        TestEqual(TEXT("sRGB-tagged mask mip in linear light"), int32(SrgbMask.Mips[1][2]), 188);
    }
    else AddError(TEXT("tagged sRGB decode: ") + SrgbMask.Error);
    if (FSCTextureImporter::Decode(Source, {TEXT("bw_linear.png"), ESCTextureRole::Color}, LinearColor) && LinearColor.Mips.Num() == 2)
    {
        TestFalse(TEXT("gAMA 1.0 overrides the Color role"), LinearColor.bSRGB);
        TestEqual(TEXT("linear-tagged colour mip as stored"), int32(LinearColor.Mips[1][2]), 128);
    }
    else AddError(TEXT("tagged linear decode: ") + LinearColor.Error);
    TestTrue(TEXT("normal stays linear whatever the PNG says"),
        FSCTextureImporter::Decode(Source, {TEXT("bw_srgb.png"), ESCTextureRole::Normal}, SrgbNormal) && !SrgbNormal.bSRGB);
    TestTrue(TEXT("untagged PNG falls back to the role"), BWColor.bSRGB && !BWLinear.bSRGB);
    UTexture2D* SrgbMaskTexture = FSCTextureImporter::Create(MoveTemp(SrgbMask), GetTransientPackage());
    UTexture2D* LinearColorTexture = FSCTextureImporter::Create(MoveTemp(LinearColor), GetTransientPackage());
    TestTrue(TEXT("created textures carry the resolved encoding"), SrgbMaskTexture && SrgbMaskTexture->SRGB && LinearColorTexture && !LinearColorTexture->SRGB);
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCMaterialsSmoothnessPng, "SCBridge.Materials.SmoothnessMapIsTheGameSmoothness", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCMaterialsSmoothnessPng::RunTest(const FString&)
{
    // WP-T1: the masters sample the game's DDNA smoothness. The DDS map stores it as is (linear data); the exporter's
    // PNG fallback stores derived roughness r = sqrt(1 - s), which the importer decodes back to s = 1 - r^2.
    FSCSceneManifest M; if (!ReadFixture(*this, M)) return false;
    const FSCSubmaterial* Sub = M.Materials[0].Submaterials.FindByPredicate([](const FSCSubmaterial& S) { return S.Name == TEXT("illum_spec"); });
    if (!TestNotNull(TEXT("illum_spec fixture"), Sub)) return false;
    FSCSubmaterial Png = *Sub;
    Png.Json = Png.Json.Replace(TEXT("tex/illum_ddna_smoothness.dds"), TEXT("tex/illum_ddna_roughness.png"));
    TestNotEqual(TEXT("fixture carries the smoothness ref"), Png.Json, Sub->Json);
    const FSCMaterialDesc Dds = FSCMaterialBuilder::Describe(*Sub, &M.Palettes[0]);
    const FSCMaterialDesc Fallback = FSCMaterialBuilder::Describe(Png, &M.Palettes[0]);
    TestEqual(TEXT("DDS smoothness: linear data"), int32(Dds.Textures.FindRef(TEXT("SmoothnessMap")).Role), int32(ESCTextureRole::Linear));
    TestEqual(TEXT("PNG fallback bound as derived roughness"), int32(Fallback.Textures.FindRef(TEXT("SmoothnessMap")).Role), int32(ESCTextureRole::DerivedRoughness));
    TestEqual(TEXT("PNG fallback still maps gloss"), Fallback.Scalars.FindRef(TEXT("UseSmoothnessMap")), 1.f);

    // Decode: roughness 0 / 128 / 255 -> smoothness 255 / 191 / 0; mips filtered in smoothness space.
    IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    TSharedPtr<IImageWrapper> Writer = Module.CreateImageWrapper(EImageFormat::PNG);
    TArray<uint8> Rgba = {0, 0, 0, 255, 128, 128, 128, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    Writer->SetRaw(Rgba.GetData(), Rgba.Num(), 4, 1, ERGBFormat::RGBA, 8);
    const TArray64<uint8>& Bytes = Writer->GetCompressed();
    const FString Dir = FPaths::ProjectSavedDir() / TEXT("Import/MaterialTests");
    FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Bytes.GetData(), int32(Bytes.Num())), *(Dir / TEXT("rough.png")));
    FSCFolderPackageSource Source(Dir);
    FSCDecodedTexture Smooth;
    if (TestTrue(TEXT("decode derived roughness"), FSCTextureImporter::Decode(Source, {TEXT("rough.png"), ESCTextureRole::DerivedRoughness}, Smooth)) && Smooth.Mips.Num() == 3)
    {
        TestEqual(TEXT("r 0 -> s 255"), int32(Smooth.Mips[0][2]), 255);
        TestEqual(TEXT("r 128 -> s 191"), int32(Smooth.Mips[0][4 + 2]), 191);
        TestEqual(TEXT("r 255 -> s 0"), int32(Smooth.Mips[0][8 + 2]), 0);
        TestEqual(TEXT("grey: green = red"), int32(Smooth.Mips[0][4 + 1]), 191);
        TestEqual(TEXT("mip averages smoothness"), int32(Smooth.Mips[1][2]), 223); // (255 + 191) / 2
        TestFalse(TEXT("smoothness is data"), Smooth.bSRGB);
    }
    else AddError(TEXT("derived roughness decode"));
    return true;
}
namespace
{
/** A package DDS as WP-T1 specifies it: "DDS ", the 124-byte header, a DX10 header, then the mips. */
TArray<uint8> MakeDds(uint32 Dxgi, uint32 W, uint32 H, uint32 Mips, uint32 TopBytes, const TArray<uint8>& Data)
{
    TArray<uint32> Header; Header.SetNumZeroed(31 + 5);
    Header[0] = 124; Header[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000; Header[2] = H; Header[3] = W;
    Header[4] = TopBytes; Header[6] = Mips; Header[18] = 32; Header[19] = 0x4; Header[20] = 0x30315844; // pixel format: FourCC "DX10"
    Header[26] = 0x1000 | 0x400000 | 0x8;
    Header[31] = Dxgi; Header[32] = 3; Header[34] = 1; // DX10: 2D, one slice
    TArray<uint8> File; File.Append(reinterpret_cast<const uint8*>("DDS "), 4);
    File.Append(reinterpret_cast<const uint8*>(Header.GetData()), Header.Num() * 4);
    File.Append(Data);
    return File;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCMaterialsDdsTextures, "SCBridge.Materials.DdsBlocksPassThrough", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCMaterialsDdsTextures::RunTest(const FString&)
{
    // WP-T1: a package DDS reaches the GPU as the game's blocks in the matching pixel format, its mips as stored.
    const FString Dir = FPaths::ProjectSavedDir() / TEXT("Import/MaterialTests");
    // BC5 SNORM normal map 8x8 + 4x4: every block red (100, -50), green (60, -20), indices 0 (all texels = endpoint 0).
    TArray<uint8> Block = {100, uint8(int8(-50)), 0, 0, 0, 0, 0, 0, 60, uint8(int8(-20)), 0, 0, 0, 0, 0, 0};
    TArray<uint8> Bc5; for (int32 I = 0; I < 5; ++I) Bc5.Append(Block);
    FFileHelper::SaveArrayToFile(MakeDds(84, 8, 8, 2, 64, Bc5), *(Dir / TEXT("n.dds")));
    // BC1 sRGB 4x4, one mip: colour 0 = colour 1 = RGB565 (16, 33, 16) -> (132, 134, 132), indices 0.
    FFileHelper::SaveArrayToFile(MakeDds(72, 4, 4, 1, 8, {0x30, 0x84, 0x30, 0x84, 0, 0, 0, 0}), *(Dir / TEXT("c.dds")));
    FFileHelper::SaveArrayToFile(MakeDds(71, 4, 4, 1, 8, {0x30, 0x84, 0x30, 0x84, 0, 0, 0, 0}), *(Dir / TEXT("l.dds")));
    FFileHelper::SaveArrayToFile(MakeDds(84, 8, 8, 2, 64, TArray<uint8>(Bc5.GetData(), 64)), *(Dir / TEXT("short.dds")));
    FSCFolderPackageSource Source(Dir);

    FSCDecodedTexture Normal, Color, Linear, Short, AsColor;
    TestTrue(TEXT("decode BC5 normal"), FSCTextureImporter::Decode(Source, {TEXT("n.dds"), ESCTextureRole::Normal}, Normal));
    if (!Normal.Error.IsEmpty()) AddError(Normal.Error);
    TestEqual(TEXT("BC5 normal: PF_BC5"), int32(Normal.PixelFormat), int32(PF_BC5));
    TestEqual(TEXT("mips as stored"), Normal.MipCount, 2);
    TestEqual(TEXT("uploads the blocks"), Normal.UploadBytes(), int64(80));
    TestFalse(TEXT("normal linear"), Normal.bSRGB);
    if (Normal.IsBlocks() && Normal.Dds.Num() == Normal.DataOffset + 80)
    {
        const uint8* B = Normal.Dds.GetData() + Normal.DataOffset;
        // SNORM -> UNORM: red endpoints re-biased (order kept: 8-value mode), indices untouched.
        TestEqual(TEXT("red e0 re-biased"), int32(B[0]), 100 + 128);
        TestEqual(TEXT("red e1 re-biased"), int32(B[1]), -50 + 128);
        TestEqual(TEXT("red indices untouched"), int32(B[2]), 0);
        // Green negated (glTF +Y -> UE -Y) in 8-value mode: endpoints (-g1, -g0) re-biased, index 0 -> 1.
        TestEqual(TEXT("green e0 = -g1"), int32(B[8]), 20 + 128);
        TestEqual(TEXT("green e1 = -g0"), int32(B[9]), -60 + 128);
        TestEqual(TEXT("green index 0 -> 1"), int32(B[10]), 0x49);
    }
    else AddError(TEXT("BC5 blocks"));
    TestTrue(TEXT("decode BC5 as colour"), FSCTextureImporter::Decode(Source, {TEXT("n.dds"), ESCTextureRole::Color}, AsColor));
    if (AsColor.IsBlocks()) TestEqual(TEXT("colour role: green not flipped"), int32(AsColor.Dds[AsColor.DataOffset + 8]), 60 + 128);

    TestTrue(TEXT("decode BC1 sRGB"), FSCTextureImporter::Decode(Source, {TEXT("c.dds"), ESCTextureRole::Linear}, Color));
    TestEqual(TEXT("BC1: PF_DXT1"), int32(Color.PixelFormat), int32(PF_DXT1));
    TestTrue(TEXT("encoding from the DXGI format, not the role"), Color.bSRGB);
    TestTrue(TEXT("decode BC1 linear"), FSCTextureImporter::Decode(Source, {TEXT("l.dds"), ESCTextureRole::Color}, Linear));
    TestFalse(TEXT("linear BC1 under a colour role"), Linear.bSRGB);
    FLinearColor SrgbMean, LinearMean;
    TestTrue(TEXT("sRGB BC1 mean linearized"), FSCTextureImporter::MeanColor(Color, SrgbMean) && FMath::IsNearlyEqual(SrgbMean.R, FLinearColor::sRGBToLinearTable[132], 1e-4f) && FMath::IsNearlyEqual(SrgbMean.G, FLinearColor::sRGBToLinearTable[134], 1e-4f));
    TestTrue(TEXT("linear BC1 mean as stored"), FSCTextureImporter::MeanColor(Linear, LinearMean) && FMath::IsNearlyEqual(LinearMean.R, 132.f / 255.f, 1e-4f));

    TestFalse(TEXT("a mip short of its blocks is rejected"), FSCTextureImporter::Decode(Source, {TEXT("short.dds"), ESCTextureRole::Normal}, Short));
    TestFalse(TEXT("rejection explains"), Short.Error.IsEmpty());

    UTexture2D* T = FSCTextureImporter::Create(MoveTemp(Normal), GetTransientPackage());
    if (TestNotNull(TEXT("BC5 texture"), T))
    {
        TestEqual(TEXT("texture BC5"), int32(T->GetPixelFormat()), int32(PF_BC5));
        TestEqual(TEXT("texture mips"), T->GetNumMips(), 2);
        TestFalse(TEXT("texture linear"), T->SRGB);
    }
    UTexture2D* C = FSCTextureImporter::Create(MoveTemp(Color), GetTransientPackage());
    if (TestNotNull(TEXT("BC1 texture"), C)) { TestEqual(TEXT("texture DXT1"), int32(C->GetPixelFormat()), int32(PF_DXT1)); TestTrue(TEXT("texture sRGB"), C->SRGB); }
    return true;
}

#endif
