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
        TestEqual(TEXT("placeholder Shininess=255 without map -> roughness 0.5"), X->Scalars[TEXT("Smoothness")], 0.75f);
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
            FString(TEXT("/SCBridge/Materials/M_SC_MeshDecal_Relief.M_SC_MeshDecal_Relief")));
        TestEqual(TEXT("coverage texture"), X->Textures[TEXT("DiffuseMap")].Path, FString(TEXT("tex/decal_diff.png")));
    }
    if (auto* X = Check(TEXT("decal_colour"), ESCMaster::MeshDecal, ESCBlend::Translucent, true, true))
        TestEqual(TEXT("colour decal parent"), FSCMaterialBuilder::ParentPath(X->Master, X->Blend, X->bTwoSided),
            FString(TEXT("/SCBridge/Materials/M_SC_MeshDecal.M_SC_MeshDecal")));
    if (auto* X = Check(TEXT("forward_glow"), ESCMaster::Translucent, ESCBlend::Translucent, false, true))
    {
        // MTL_FLAG_REQUIRE_FORWARD_RENDERING: diffuse alpha is coverage; glow colour = diffuse x emissive (sidecar data).
        TestTrue(TEXT("glow keeps authored diffuse colour"), Near(X->Vectors[TEXT("DiffuseColor")], FLinearColor(0.2f, 0.6f, 0.9f)));
        TestEqual(TEXT("Emissive=1,1,1 without Glow emits nothing"), X->Scalars[TEXT("EmissiveStrength")], 0.f);
    }
    if (auto* X = Check(TEXT("emissives_like"), ESCMaster::Translucent, ESCBlend::Translucent, false, true))
    {
        // Emission colour = Emissive x Diffuse (x texture in the master): blue, strength = Glow x host scale.
        const FLinearColor Glow = X->Vectors[TEXT("EmissiveColor")] * X->Vectors[TEXT("DiffuseColor")];
        TestTrue(TEXT("emissives glow blue"), Glow.B > Glow.G && Glow.G > Glow.R && Glow.R > 0.f);
        TestEqual(TEXT("emissives strength"), X->Scalars[TEXT("EmissiveStrength")], 0.3f * FSCMaterialBuilder::GlowToEmissive);
    }
    for (const FSCSubmaterial& Sub : M.Materials[0].Submaterials)
    {
        // Rule: no Glow (absent or 0) -> no emission, for every lit family (engine-glow targets excepted).
        const FSCMaterialDesc* X = Find(D, *Sub.Name);
        if (!X || X->Master == ESCMaster::EmissiveUnlit || Sub.Json.Contains(TEXT("\"Glow\""))) continue;
        TestEqual(Sub.Name + TEXT(": no Glow, no emission"), X->Scalars.FindRef(TEXT("EmissiveStrength")), 0.f);
        // Rule: a MeshDecal primitive never resolves to an opaque master.
        if (Sub.ShaderFamily == TEXT("MeshDecal"))
            TestTrue(Sub.Name + TEXT(": MeshDecal is a decal/hidden, never opaque"), X->Blend == ESCBlend::Translucent && X->bNonNanite);
    }
    if (auto* X = Check(TEXT("screen"), ESCMaster::EmissiveUnlit, ESCBlend::Translucent, false, true))
        TestEqual(TEXT("RTT screen dark until ui_bindings"), X->Scalars[TEXT("EmissiveStrength")], 0.f);
    if (auto* X = Check(TEXT("monitor"), ESCMaster::EmissiveUnlit, ESCBlend::Translucent, false, true))
        TestEqual(TEXT("image screen glows"), X->Scalars[TEXT("EmissiveStrength")], 1.f);
    if (auto* X = Check(TEXT("inactive"), ESCMaster::EmissiveUnlit, ESCBlend::Translucent, true, true)) TestTrue(TEXT("hidden"), X->bHidden);
    if (auto* X = Check(TEXT("skin"), ESCMaster::Fallback, ESCBlend::Opaque, false, false))
        TestTrue(TEXT("fallback warning names the shader"), X->Warnings.Num() == 1 && X->Warnings[0].Contains(TEXT("HumanSkin_V2")));
    for (const FSCMaterialDesc& X : D)
        TestEqual(X.Name + TEXT(": RequiresNonNanite matches Describe"), FSCMaterialBuilder::RequiresNonNanite(
            *M.Materials[0].Submaterials.FindByPredicate([&](const FSCSubmaterial& S) { return S.Name == X.Name; })), X.bNonNanite);
    // Engine glow: scene controls target (sidecar, authored index 13) gets the default strength.
    TestEqual(TEXT("engine glow targets"), M.EngineGlow.Targets.Num(), 1);
    TestEqual(TEXT("engine glow default"), M.EngineGlow.DefaultStrength, 7.f);
    FSCImportMaterials Phase; Phase.Describe(M);
    FSCFolderPackageSource Empty(FixtureDir()); // fixtures carry no PNGs: every decode fails, counts stay exact
    FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    Phase.Decode(Empty);
    // Unique (path, role) pairs across all submaterials; tex/illum_ddna.png etc. are shared and deduplicated.
    TestEqual(TEXT("deduplicated texture requests"), Phase.TextureStats.Requested, 24);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCMaterialsPackageRules, "SCBridge.Materials.PackageRules", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCMaterialsPackageRules::RunTest(const FString&)
{
    // Real-package invariants (-SCPackage=<export root>): every referenced submaterial, no per-asset expectations.
    FString Folder;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SCPackage="), Folder)) { AddError(TEXT("PackageRules requires -SCPackage")); return false; }
    FSCFolderPackageSource Source(Folder); FSCSceneManifest M; FString Error;
    if (!TestTrue(TEXT("package reads: ") + Error, FSCPackageReader::Read(Source, M, Error))) return false;
    int32 Decals = 0, Glowing = 0, Silent = 0;
    for (const FSCMaterialSet& Set : M.Materials)
        for (const FSCSubmaterial& Sub : Set.Submaterials)
        {
            const FSCMaterialDesc D = FSCMaterialBuilder::Describe(Sub, M.Palettes.Num() ? &M.Palettes[0] : nullptr);
            const FString Where = Set.Path + TEXT(":") + Sub.Name;
            if (Sub.ShaderFamily == TEXT("MeshDecal"))
            {
                ++Decals;
                TestTrue(Where + TEXT(" MeshDecal never opaque"), D.Blend == ESCBlend::Translucent && D.bNonNanite
                    && (D.Master == ESCMaster::MeshDecal || D.Master == ESCMaster::MeshDecalRelief || D.bHidden));
            }
            if (D.Master == ESCMaster::EmissiveUnlit) continue;
            const bool bGlow = Sub.Json.Contains(TEXT("\"Glow\""));
            const float Strength = D.Scalars.FindRef(TEXT("EmissiveStrength"));
            if (!bGlow) { ++Silent; TestEqual(Where + TEXT(" no Glow -> no emission"), Strength, 0.f); }
            else if (Strength > 0.f)
            {
                ++Glowing;
                const FLinearColor C = D.Vectors.FindRef(TEXT("EmissiveColor")) * D.Vectors.FindRef(TEXT("DiffuseColor"));
                TestTrue(Where + TEXT(" glow colour comes from Emissive x Diffuse"), C.R + C.G + C.B > 0.f);
            }
        }
    AddInfo(FString::Printf(TEXT("PACKAGE_RULES decals=%d glowing=%d no_glow=%d"), Decals, Glowing, Silent));
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
                if (bVariant) TestEqual(Path + TEXT(" blend override"), int32(Parent->GetBlendMode()), int32(Blend == ESCBlend::Masked ? BLEND_Masked : BLEND_Opaque));
                if (bVariant) TestEqual(Path + TEXT(" two-sided override"), Parent->IsTwoSided(), bTwoSided);
                if (Master == ESCMaster::MeshDecal || Master == ESCMaster::MeshDecalRelief)
                    TestEqual(Path + TEXT(" is a DBuffer decal"), int32(Parent->GetMaterial()->MaterialDomain), int32(MD_DeferredDecal));
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
        TestEqual(TEXT("1x1 mip averages"), int32(Color.Mips[2][2]), 35);
    }
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
