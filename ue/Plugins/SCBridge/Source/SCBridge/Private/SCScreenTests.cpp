// SCBridge.Materials.ScreenFamily / SCBridge.Package.UiBindings / SCBridge.Screens.PerFaceAssembly (WP-I1): the game's
// render-to-texture DisplayScreen face is a dark glass screen whose emission is the bound panel image x the authored glow
// (opaque, or lit translucent when its data asks for partial coverage); the package's ui_bindings name that image, bound
// per face to operating panels only: a binding whose owner is a display item (MFDs, HUDs, signage: source_item_type
// `Display`) stays dark, whoever carries it. Submaterials are synthetic, shaped like the LIVE 4.10.2 sidecars
// (Starlifter_Int RTT_Screens, template_int_a screen, the Idris ui_screen_16x9 `Flash` at opacity 0.6).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCImporter.h"
#include "SCMaterialBuilder.h"
#include "SCStarEngine.h"
#include "SCPackageReader.h"
#include "SCPackageSource.h"
#include "SCVehicle.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionEyeAdaptation.h"
#include "Materials/MaterialExpressionEyeAdaptationInverse.h"
#include "Materials/MaterialExpressionEyeAdaptationUnclamped.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshResources.h"

namespace
{
/** A v2 sidecar submaterial JSON: `Family` with authored Specular / Shininess / Glow, `BackColour` public param, the
 *  alpha_config (Opacity) and emissive objects, and optionally the virtual $RenderToTexture slot or a base colour. */
FString ScreenJson(const TCHAR* Name, const TCHAR* Family, bool bRenderToTexture, bool bForward = false,
    const TCHAR* State = TEXT("active"), const TCHAR* BaseColour = nullptr, float Opacity = 1.f)
{
    const FString Slots = FString(bRenderToTexture
        ? TEXT(R"({"slot":"TexSlot9","role":"render_to_texture","is_virtual":true,"export_path":null,"source_path":"$RenderToTexture"})")
        : TEXT(""))
        + (BaseColour ? FString::Printf(TEXT(R"(%s{"slot":"TexSlot1","role":"base_color","is_virtual":false,"export_path":"%s"})"),
                                        bRenderToTexture ? TEXT(",") : TEXT(""), BaseColour) : FString());
    return FString::Printf(TEXT(R"({"index":8,"submaterial_name":"%s","shader_family":"%s","shader":"%s",
        "authored_attributes":[{"name":"Shader","value":"%s"},{"name":"Diffuse","value":"1,1,1"},
          {"name":"Specular","value":"0.0194,0.0194,0.0194"},{"name":"Emissive","value":"0,0,0"},{"name":"Opacity","value":"%g"},
          {"name":"Shininess","value":"170"},{"name":"Glow","value":"0.08"}],
        "public_params":{"BackColour":"0.0176,0.0176,0.0176","EmissiveContrast":1,"EmissiveSaturation":1},
        "alpha_config":{"alpha_test":0.0,"forward_rendering":%s,"opacity":%g,"two_sided":false},
        "emissive":{"alpha_glow_cutoff":null,"color":[1.0,1.0,1.0],"emittance_texture":null,"forward_rendering":%s,"glow":0.08,"strength":0.08,"uses_base_texture":true},
        "decoded_feature_flags":{"tokens":["EMISSIVE_MAP","DBA_GLOBAL"]},
        "texture_slots":[%s],"derived_textures":[],"layer_manifest":[],
        "palette_routing":{"layer_channels":[],"material_channel":null},
        "activation_state":{"state":"%s","reason":"synthetic"}})"),
        Name, Family, Family, Family, Opacity, bForward ? TEXT("true") : TEXT("false"), Opacity, bForward ? TEXT("true") : TEXT("false"), *Slots, State);
}
FSCSubmaterial ScreenSub(const TCHAR* Name, const TCHAR* Family, bool bRenderToTexture, bool bForward = false,
    const TCHAR* State = TEXT("active"), const TCHAR* BaseColour = nullptr, float Opacity = 1.f)
{
    FSCSubmaterial S;
    S.Name = Name; S.ShaderFamily = Family;
    S.Json = ScreenJson(Name, Family, bRenderToTexture, bForward, State, BaseColour, Opacity);
    return S;
}
const FString ScreenMaster = TEXT("/SCBridge/Materials/M_SC_Screen.M_SC_Screen");
const FString TranslucentScreenMaster = TEXT("/SCBridge/Materials/M_SC_Screen_Translucent.M_SC_Screen_Translucent");
const FString UnlitMaster = TEXT("/SCBridge/Materials/M_SC_Emissive_Unlit.M_SC_Emissive_Unlit");

/** One ui_bindings entry; ItemType nullptr writes null, bNoItemType omits the owner keys (an export before
 *  source_item_type), bUnresolved writes an owner the exporter could not resolve. The owner is operated
 *  (source_kiosk_interaction) unless it is a Display item, or when bOperatedDisplay is set (a kiosk). */
FString Binding(const TCHAR* Kind, const TCHAR* ItemType, const TCHAR* Source, const TCHAR* Helper, const TCHAR* Image, bool bOff = false,
    bool bNoItemType = false, bool bUnresolved = false, bool bOperatedDisplay = false)
{
    const bool bOperated = bOperatedDisplay || !(ItemType && FCString::Strcmp(ItemType, TEXT("Display")) == 0);
    auto Quoted = [](const TCHAR* S) { return S ? FString::Printf(TEXT("\"%s\""), S) : FString(TEXT("null")); };
    return FString::Printf(TEXT(R"({"binding_kind":"%s","source_entity_name":"%s","helper_name":%s,"canvas_record_name":"BuildingBlocks_Canvas.Synthetic",)"
        R"("content_canvas_record_name":null,"default_state_is_off":%s,"generated_image_path":%s,"ui_screen_aspect_w_over_h":null%s})"),
        Kind, Source, *Quoted(Helper), bOff ? TEXT("true") : TEXT("false"), *Quoted(Image),
        bNoItemType ? TEXT("") : *(TEXT(",\"source_item_type\":") + Quoted(ItemType) + TEXT(",\"source_owner_resolved\":") +
                                  (bUnresolved ? TEXT("false") : TEXT("true")) + TEXT(",\"source_kiosk_interaction\":") +
                                  (bOperated && !bUnresolved ? TEXT("true") : TEXT("false"))));
}

class FScreenMemorySource : public FSCPackageSource
{
public:
    TMap<FString, TArray<uint8>> Files;
    void Add(const FString& Name, const FString& Text)
    { FTCHARToUTF8 Utf8(*Text); TArray<uint8> B; B.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); Files.Add(Name, MoveTemp(B)); }
    bool Exists(const FString& Path) const override { return Files.Contains(Path); }
    bool ReadBytes(const FString& Path, TArray<uint8>& Out, FString& Error) const override
    { if (auto* B = Files.Find(Path)) { Out = *B; return true; } Error = TEXT("Missing fixture"); return false; }
    TArray<FString> ListFiles(const FString& Prefix) const override
    { TArray<FString> R; for (const auto& P : Files) if (Prefix.IsEmpty() || P.Key.StartsWith(Prefix)) R.Add(P.Key); R.Sort(); return R; }
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCScreenFamily, "SCBridge.Materials.ScreenFamily", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCScreenFamily::RunTest(const FString&)
{
    const float Nits = float(FSCStarEngine::Get().GlowNits(0.08));
    // The display face (DisplayScreen with the virtual $RenderToTexture slot): opaque dark glass, not the invisible additive.
    for (const bool bForward : {false, true}) // a forward-flagged screen (console_cargo_elevator) is the same opaque face
    {
        const FSCSubmaterial Sub = ScreenSub(TEXT("RTT_Screens"), TEXT("DisplayScreen"), true, bForward);
        const FSCMaterialDesc D = FSCMaterialBuilder::Describe(Sub, nullptr);
        const FString Where = bForward ? TEXT("forward screen: ") : TEXT("screen: ");
        TestEqual(Where + TEXT("the screen master"), FSCMaterialBuilder::ParentPath(D.Master, D.Blend, D.bTwoSided), ScreenMaster);
        TestEqual(Where + TEXT("opaque"), int32(D.Blend), int32(ESCBlend::Opaque));
        TestFalse(Where + TEXT("Nanite (opaque path, Lumen cards)"), D.bNonNanite);
        TestFalse(Where + TEXT("the mesh split agrees"), FSCMaterialBuilder::RequiresNonNanite(Sub));
        TestFalse(Where + TEXT("visible"), D.bHidden);
        TestTrue(Where + TEXT("base colour = BackColour"), D.Vectors.FindRef(TEXT("ScreenBackColor")).Equals(FLinearColor(0.0176f, 0.0176f, 0.0176f), 1e-5f));
        TestTrue(Where + TEXT("F0 = authored Specular"), D.Vectors.FindRef(TEXT("SpecularColor")).Equals(FLinearColor(0.0194f, 0.0194f, 0.0194f), 1e-5f));
        TestEqual(Where + TEXT("gloss = Shininess / 255"), D.Scalars.FindRef(TEXT("Smoothness")), 170.f / 255.f);
        // A8: the unbound face emits nothing (black image, and an emitter tool reading the strength must agree); the
        // authored glow is kept for the image-bound instance.
        TestEqual(Where + TEXT("unbound: no emission strength"), D.Scalars.FindRef(TEXT("EmissiveStrength"), -1.f), 0.f);
        TestEqual(Where + TEXT("bound glow = the authored Glow by the game's glow rule"), D.ScreenGlow, Nits);
        TestTrue(Where + TEXT("emission tint = the v2 colour"), D.Vectors.FindRef(TEXT("EmissiveColor")).Equals(FLinearColor::White));
        TestFalse(Where + TEXT("no image in the description (bound per face)"), D.Textures.Contains(TEXT("ScreenImage")));
        TestFalse(Where + TEXT("no opacity parameter"), D.Scalars.Contains(TEXT("Opacity")));
        TestEqual(Where + TEXT("no warnings"), D.Warnings.Num(), 0);
        // WP-I1 fix pass: the game's DisplayScreenPS emission (glassLib GlassShared %EMISSIVE_MAP): the image through
        // MonitorEffects_ApplyViewingAngle (authored contrast / saturation / viewing angle, the shader's defaults when
        // absent), x the glow, x %DBA_GLOBAL GetGlobalDBAMultiplier_HDR (the scene's adapted luminance / middle grey,
        // clamped to EV 1..14), x the glass transmittance (white Diffuse: 1).
        const FSCStarEngine& Game = FSCStarEngine::Get();
        TestEqual(Where + TEXT("DBA_GLOBAL on"), D.Scalars.FindRef(TEXT("ScreenDBA"), -1.f), 1.f);
        TestEqual(Where + TEXT("DBA lower bound = NitsAtEV(minAdaptedEV)"), D.Scalars.FindRef(TEXT("DBAMinNits"), -1.f), float(Game.NitsAtEV(Game.DBAMinAdaptedEV)));
        TestEqual(Where + TEXT("DBA upper bound = NitsAtEV(maxAdaptedEV)"), D.Scalars.FindRef(TEXT("DBAMaxNits"), -1.f), float(Game.NitsAtEV(Game.DBAMaxAdaptedEV)));
        TestEqual(Where + TEXT("DBA divisor = DBA middle grey x scene unit"), D.Scalars.FindRef(TEXT("DBANitsPerUnit"), -1.f), float(Game.DBAMiddleGrey * Game.SceneUnitNits));
        TestEqual(Where + TEXT("adapted luminance from the exposure's middle grey"), D.Scalars.FindRef(TEXT("DBAExposureGrey"), -1.f), float(Game.MiddleGrey));
        TestEqual(Where + TEXT("authored saturation"), D.Scalars.FindRef(TEXT("EmissiveSaturation"), -1.f), 1.f);
        TestEqual(Where + TEXT("authored contrast"), D.Scalars.FindRef(TEXT("EmissiveContrast"), -1.f), 1.f);
        TestEqual(Where + TEXT("viewing angle: the shader default"), D.Scalars.FindRef(TEXT("ViewingAngle"), -1.f), float(Game.MonitorViewingAngleDefault));
        TestEqual(Where + TEXT("view angle mode: the shader default"), D.Scalars.FindRef(TEXT("ViewAngleMode"), -1.f), float(Game.MonitorViewAngleModeDefault));
        TestTrue(Where + TEXT("white Diffuse transmits fully"), D.Vectors.FindRef(TEXT("ScreenTransmittance")).Equals(FLinearColor::White, 1e-5f));
    }
    {
        // Without %DBA_GLOBAL the glow is absolute (the game's emissive at scene units).
        FSCSubmaterial Sub = ScreenSub(TEXT("RTT_NoDBA"), TEXT("DisplayScreen"), true);
        Sub.Json = Sub.Json.Replace(TEXT(R"(["EMISSIVE_MAP","DBA_GLOBAL"])"), TEXT(R"(["EMISSIVE_MAP"])"));
        const FSCMaterialDesc D = FSCMaterialBuilder::Describe(Sub, nullptr);
        TestEqual(TEXT("no DBA token: DBA off"), D.Scalars.FindRef(TEXT("ScreenDBA"), -1.f), 0.f);
    }
    // A4: authored partial opacity (the Idris/Ironclad `Flash` overlay, 0.6) or a blended alpha mode is honoured: a lit
    // translucent screen, split into the non-Nanite mesh like every blended surface; the split rule agrees.
    for (const bool bBlendedMode : {false, true})
    {
        FSCSubmaterial Sub = ScreenSub(TEXT("Flash"), TEXT("DisplayScreen"), true, false, TEXT("active"), nullptr, bBlendedMode ? 1.f : 0.6f);
        Sub.bBlended = bBlendedMode;
        const FSCMaterialDesc D = FSCMaterialBuilder::Describe(Sub, nullptr);
        const FString Where = bBlendedMode ? TEXT("blended screen: ") : TEXT("opacity 0.6 screen: ");
        TestEqual(Where + TEXT("screen family"), int32(D.Master), int32(ESCMaster::Screen));
        TestEqual(Where + TEXT("translucent"), int32(D.Blend), int32(ESCBlend::Translucent));
        TestEqual(Where + TEXT("the translucent screen master"), FSCMaterialBuilder::ParentPath(D.Master, D.Blend, D.bTwoSided), TranslucentScreenMaster);
        TestTrue(Where + TEXT("non-Nanite"), D.bNonNanite);
        TestTrue(Where + TEXT("the mesh split agrees"), FSCMaterialBuilder::RequiresNonNanite(Sub));
        TestEqual(Where + TEXT("coverage = authored opacity"), D.Scalars.FindRef(TEXT("Opacity"), -1.f), bBlendedMode ? 1.f : 0.6f);
        TestEqual(Where + TEXT("bound glow kept"), D.ScreenGlow, Nits);
    }
    {
        // UIPlane (the HUD projector glass, RTT_Hud) keeps the invisible additive: the owner's HUDs are our own UI.
        const FSCMaterialDesc D = FSCMaterialBuilder::Describe(ScreenSub(TEXT("RTT_Hud"), TEXT("UIPlane"), true), nullptr);
        TestEqual(TEXT("UIPlane: additive unlit"), FSCMaterialBuilder::ParentPath(D.Master, D.Blend, D.bTwoSided), UnlitMaster);
        TestEqual(TEXT("UIPlane: dark"), D.Scalars.FindRef(TEXT("EmissiveStrength"), -1.f), 0.f);
        TestTrue(TEXT("UIPlane: split mesh"), D.bNonNanite);
    }
    {
        // A DisplayScreen without a render-to-texture slot draws its own exported image as before.
        const FSCMaterialDesc D = FSCMaterialBuilder::Describe(ScreenSub(TEXT("baked"), TEXT("DisplayScreen"), false, false, TEXT("active"), TEXT("tex/baked.png")), nullptr);
        TestEqual(TEXT("baked screen: additive unlit"), FSCMaterialBuilder::ParentPath(D.Master, D.Blend, D.bTwoSided), UnlitMaster);
        TestEqual(TEXT("baked screen: glows by the authored rule"), D.Scalars.FindRef(TEXT("EmissiveStrength")), Nits);
    }
    {
        // An inactive screen stays hidden.
        const FSCMaterialDesc D = FSCMaterialBuilder::Describe(ScreenSub(TEXT("off"), TEXT("DisplayScreen"), true, false, TEXT("inactive")), nullptr);
        TestTrue(TEXT("inactive screen: hidden"), D.bHidden && D.bNonNanite);
        TestEqual(TEXT("inactive screen: dark"), D.Scalars.FindRef(TEXT("EmissiveStrength"), -1.f), 0.f);
    }
    // The masters: opaque Nanite / lit translucent surfaces with the image, gloss and look parameters the builder sets.
    for (const FString& Path : {ScreenMaster, TranslucentScreenMaster})
    {
        UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, *Path);
        if (!TestNotNull(Path + TEXT(" exists"), Master)) continue;
        const bool bTranslucent = Path == TranslucentScreenMaster;
        TestEqual(Path + TEXT(" blend"), int32(Master->GetBlendMode()), int32(bTranslucent ? BLEND_Translucent : BLEND_Opaque));
        TestTrue(Path + TEXT(" is a screen master"), FSCMaterialBuilder::IsScreenMaster(Master));
        TestTrue(Path + TEXT(" usable on skeletal meshes"), Master->GetMaterial()->GetUsageByFlag(MATUSAGE_SkeletalMesh));
        TestTrue(Path + TEXT(" usable instanced"), Master->GetMaterial()->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes));
        UTexture* Image = nullptr;
        TestTrue(Path + TEXT(" has ScreenImage"), Master->GetTextureParameterDefaultValue(FHashedMaterialParameterInfo(FSCMaterialBuilder::ScreenImageParam), Image));
        TestEqual(Path + TEXT(": unbound screen image is black (dark glass)"), GetNameSafe(Image), FString(TEXT("T_SC_Black")));
        // No EmissiveColor: GlassShared scales the image by the scalar glow and the target's alpha (review fix).
        for (const TCHAR* Name : {TEXT("ScreenBackColor"), TEXT("SpecularColor"), TEXT("ScreenTransmittance")})
        {
            FLinearColor V;
            TestTrue(Path + TEXT(" has ") + Name, Master->GetVectorParameterDefaultValue(FHashedMaterialParameterInfo(FName(Name)), V));
        }
        TArray<FName> Scalars = {FName(TEXT("EmissiveStrength")), FName(TEXT("Smoothness")), FName(TEXT("GlossMult")), FName(TEXT("UseSmoothnessMap")),
                                 FSCMaterialBuilder::GlassReflectanceScaleParam, FSCMaterialBuilder::GlassReflectanceFloorParam,
                                 FName(TEXT("ScreenDBA")), FName(TEXT("DBAMinNits")), FName(TEXT("DBAMaxNits")), FName(TEXT("DBANitsPerUnit")),
                                 FName(TEXT("DBAExposureGrey")), FName(TEXT("EmissiveSaturation")), FName(TEXT("EmissiveContrast")),
                                 FName(TEXT("ViewingAngle")), FName(TEXT("ViewAngleMode"))};
        if (bTranslucent) Scalars.Add(TEXT("Opacity"));
        for (const FName Name : Scalars)
        {
            float V;
            TestTrue(Path + TEXT(" has ") + Name.ToString(), Master->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(Name), V));
        }
    }
    TestFalse(TEXT("glass is not a screen master"), FSCMaterialBuilder::IsScreenMaster(
        LoadObject<UMaterialInterface>(nullptr, *FSCMaterialBuilder::ParentPath(ESCMaster::Glass, ESCBlend::Translucent, false))));
    return true;
}

// SCBridge.Materials.ScreenDBA (WP-E1 E1-9): the screen's %DBA_GLOBAL term against the game's chain over the adapted luminance,
// with the fixture's constants. The game (OpticsExposure.cfxb CalcExposureAndNoise, OpticsExposure.cfib GetGlobalDBAMultiplier_HDR):
// adaptedLuminance = 2^(pigment + pupil) is stored before the exposure clamp, unclampedExposure = middleGray / adaptedLuminance,
// exposure = clamp(unclampedExposure, r_ExposureMultiplierMin, r_ExposureMultiplierMax), DBA = clamp(adaptedLuminance, EV 1, EV 14) /
// 0.18; the screen shows glow x image x DBA x exposure. The viewer: EmissiveStrength = the glow in cd/m2, x ScreenDBA(the engine's
// unclamped scale), exposed by the clamped scale (the brightness bounds are the game's multiplier clamp / SceneUnitNits). The
// display brightness must agree in every regime: below the DBA floor, below the exposure clamp (the dark room), inside it (constant
// display brightness), above the DBA ceiling, above the exposure clamp.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCScreenDBA, "SCBridge.Materials.ScreenDBA", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCScreenDBA::RunTest(const FString&)
{
    const FSCStarEngine& G = FSCStarEngine::Get();
    // Scene units (the game's) and cd/m2 (UE's): one scene unit is SceneUnitNits cd/m2.
    const double DBAFloorSU = G.NitsAtEV(G.DBAMinAdaptedEV) / G.SceneUnitNits;
    const double DBACeilingSU = G.NitsAtEV(G.DBAMaxAdaptedEV) / G.SceneUnitNits;
    const double ClampDarkSU = G.MiddleGrey / G.MultiplierMax;   // darker than this: the exposure is held at its maximum
    const double ClampBrightSU = G.MiddleGrey / G.MultiplierMin; // brighter than this: held at its minimum
    // The fixture's order of the four bounds, which the regimes below assume (the game's: 0.25 < 9 < 2048 < 1.8e6 cd/m2).
    if (!TestTrue(TEXT("DBA floor < exposure clamp (dark) < DBA ceiling < exposure clamp (bright)"),
                  DBAFloorSU < ClampDarkSU && ClampDarkSU < DBACeilingSU && DBACeilingSU < ClampBrightSU))
        return false;

    // Displayed (pre-tonemap) value per unit of glow x image.
    auto Game = [&](double AdaptedSU) {
        const double DBA = FMath::Clamp(AdaptedSU, DBAFloorSU, DBACeilingSU) / G.DBAMiddleGrey;
        return DBA * FMath::Clamp(G.MiddleGrey / AdaptedSU, G.MultiplierMin, G.MultiplierMax);
    };
    auto Viewer = [&](double AdaptedSU, bool bClampedRule) {
        const double Unclamped = G.MiddleGrey / (AdaptedSU * G.SceneUnitNits); // UE's scale, per cd/m2
        const double Clamped = FMath::Clamp(Unclamped, G.MultiplierMin / G.SceneUnitNits, G.MultiplierMax / G.SceneUnitNits);
        // glow x image in cd/m2 (EmissiveStrength = SceneUnitNits x the linear glow) x DBA, exposed by the clamped scale
        return G.SceneUnitNits * G.ScreenDBA(bClampedRule ? Clamped : Unclamped) * Clamped;
    };
    struct FCase { const TCHAR* Name; double AdaptedSU; };
    const FCase Cases[] = {
        {TEXT("below the DBA floor"), DBAFloorSU * 0.25},
        {TEXT("below the exposure clamp (dark room)"), FMath::Sqrt(DBAFloorSU * ClampDarkSU)},
        {TEXT("just below the exposure clamp"), ClampDarkSU * 0.9},
        {TEXT("at the exposure clamp"), ClampDarkSU},
        {TEXT("inside the exposure clamp"), FMath::Sqrt(ClampDarkSU * DBACeilingSU)},
        {TEXT("above the DBA ceiling"), FMath::Sqrt(DBACeilingSU * ClampBrightSU)},
        {TEXT("above the exposure clamp"), ClampBrightSU * 4.0}};
    for (const FCase& C : Cases)
    {
        const double Expected = Game(C.AdaptedSU), Got = Viewer(C.AdaptedSU, false);
        TestTrue(FString::Printf(TEXT("%s (%.4g cd/m2): viewer %.6g = game %.6g"), C.Name, C.AdaptedSU * G.SceneUnitNits, Got, Expected),
                 FMath::IsNearlyEqual(Got, Expected, 1e-9 * Expected));
    }
    // The rule itself: constant display brightness (glow x image) inside the clamps; below the exposure clamp the screen dims with
    // the adapted luminance (adapted / clamp luminance), which the clamped exposure (the WP-I1 rule, EyeAdaptationInverse) misses.
    const double Inside = FMath::Sqrt(ClampDarkSU * DBACeilingSU), Dark = FMath::Sqrt(DBAFloorSU * ClampDarkSU);
    TestTrue(TEXT("inside: displayed = glow x image"), FMath::IsNearlyEqual(Game(Inside), 1.0, 1e-12));
    TestTrue(TEXT("dark room: displayed = adapted / clamp luminance"), FMath::IsNearlyEqual(Game(Dark), Dark / ClampDarkSU, 1e-12));
    TestTrue(TEXT("dark room: the clamped exposure would show it at full brightness"), FMath::IsNearlyEqual(Viewer(Dark, true), 1.0, 1e-9));
    TestTrue(TEXT("inside and above the exposure clamp both rules agree"),
             FMath::IsNearlyEqual(Viewer(Inside, true), Viewer(Inside, false), 1e-12) &&
             FMath::IsNearlyEqual(Viewer(ClampBrightSU * 4.0, true), Viewer(ClampBrightSU * 4.0, false), 1e-12));
    TestEqual(TEXT("a non-positive scale (disabled, or flushed by half precision) reads as the DBA ceiling"), G.ScreenDBA(0.0),
              G.ScreenDBA(G.MiddleGrey / G.NitsAtEV(G.DBAMaxAdaptedEV) * 1e-3));
    return true;
}

#if WITH_EDITOR
namespace
{
/** The material's generated HLSL (Material.ush) for the running shader platform: translation only, nothing is compiled. */
FString TranslatedMaterialSource(UMaterial* Material)
{
    FMaterialResource* Resource = Material->AllocateResource();
    Resource->SetMaterial(Material, nullptr, GMaxRHIShaderPlatform);
    FString Source;
    if (!Resource->GetMaterialExpressionSource(Source)) Source.Reset();
    delete Resource;
    return Source;
}
/** A transient surface material whose emissive is one expression of class Class. */
UMaterial* EmissiveOf(UClass* Class)
{
    UMaterial* M = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
    UMaterialExpression* E = NewObject<UMaterialExpression>(M, Class, NAME_None, RF_Transient);
    E->Material = M;
    M->GetExpressionCollection().AddExpression(E);
    M->GetEditorOnlyData()->EmissiveColor.Connect(0, E);
    M->PostEditChange();
    return M;
}
} // namespace

// SCBridge.Materials.ScreenExposureNode (WP-E1 E1-9): the screen masters read the engine's unclamped exposure (EyeAdaptationUnclamped)
// in their DBA term and not the clamped one; materials without the node translate without the unclamped lookup (the engine change
// adds nothing to them), the EyeAdaptation node still emits UE's lookup, and the node emits the fork's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCScreenExposureNode, "SCBridge.Materials.ScreenExposureNode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCScreenExposureNode::RunTest(const FString&)
{
    const TCHAR* Unclamped = TEXT("EyeAdaptationUnclampedLookup()");
    for (const FString& Path : {ScreenMaster, TranslucentScreenMaster})
    {
        UMaterial* Master = LoadObject<UMaterial>(nullptr, *Path);
        if (!TestNotNull(Path, Master)) continue;
        int32 Nodes = 0, Inverse = 0;
        const UMaterialExpressionCustom* DBA = nullptr;
        for (UMaterialExpression* E : Master->GetExpressions())
        {
            Nodes += E && E->IsA<UMaterialExpressionEyeAdaptationUnclamped>();
            Inverse += E && E->IsA<UMaterialExpressionEyeAdaptationInverse>();
            if (const UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(E))
                if (C->Inputs.ContainsByPredicate([](const FCustomInput& I) { return I.InputName == TEXT("UseDBA"); })) DBA = C;
        }
        TestEqual(Path + TEXT(": one EyeAdaptationUnclamped node"), Nodes, 1);
        TestEqual(Path + TEXT(": no EyeAdaptationInverse (the clamped exposure)"), Inverse, 0);
        if (TestNotNull(Path + TEXT(": the DBA term"), DBA))
        {
            const FCustomInput* In = DBA->Inputs.FindByPredicate([](const FCustomInput& I) { return I.InputName == TEXT("Unclamped"); });
            TestTrue(Path + TEXT(": the DBA term reads the unclamped exposure"),
                     In && In->Input.Expression && In->Input.Expression->IsA<UMaterialExpressionEyeAdaptationUnclamped>());
            TestTrue(Path + TEXT(": adapted luminance = middle grey / the unclamped scale"), DBA->Code.Contains(TEXT("Unclamped > 0.0 ? ExposureGrey / Unclamped : MaxNits")));
        }
        TestTrue(Path + TEXT(": the generated HLSL reads the unclamped lookup"), TranslatedMaterialSource(Master).Contains(Unclamped));
    }
    for (ESCMaster Family : {ESCMaster::Illum, ESCMaster::HardSurface, ESCMaster::Layered, ESCMaster::Glass, ESCMaster::MeshDecal,
                             ESCMaster::Translucent, ESCMaster::EmissiveUnlit, ESCMaster::Fallback})
    {
        UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *FSCMaterialBuilder::ParentPath(Family, ESCBlend::Opaque, false));
        UMaterial* Base = Parent ? Parent->GetMaterial() : nullptr;
        if (!TestNotNull(TEXT("master"), Base)) continue;
        const FString Source = TranslatedMaterialSource(Base);
        TestFalse(Base->GetName() + TEXT(": translates"), Source.IsEmpty());
        TestFalse(Base->GetName() + TEXT(": no unclamped lookup"), Source.Contains(Unclamped));
    }
    const FString WithEyeAdaptation = TranslatedMaterialSource(EmissiveOf(UMaterialExpressionEyeAdaptation::StaticClass()));
    TestTrue(TEXT("EyeAdaptation: UE's lookup"), WithEyeAdaptation.Contains(TEXT("EyeAdaptationLookup()")));
    TestFalse(TEXT("EyeAdaptation: no unclamped lookup"), WithEyeAdaptation.Contains(Unclamped));
    const FString WithNode = TranslatedMaterialSource(EmissiveOf(UMaterialExpressionEyeAdaptationUnclamped::StaticClass()));
    TestTrue(TEXT("EyeAdaptationUnclamped: the fork's lookup"), WithNode.Contains(Unclamped));
    return true;
}
#endif // WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCUiBindings, "SCBridge.Package.UiBindings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCUiBindings::RunTest(const FString&)
{
    // Shapes from the LIVE C2 and Ironclad packages; names synthetic.
    const TCHAR* Door = TEXT("Data/UI/Generated/ship/synthetic/door.png");
    const TCHAR* A = TEXT("Data/UI/Generated/ship/synthetic/a.png");
    const TCHAR* B = TEXT("Data/UI/Generated/ship/synthetic/b.png");
    const FString Scene = FString::Printf(TEXT(R"({"version":2,"export_kind":"Decomposed","root_entity":{"entity_name":"Ship"},
        "children":[
          {"entity_name":"Dashboard","parent_entity_name":"Ship","source_transform_basis":"gltf_y_up","ui_bindings":[%s,%s]},
          {"entity_name":"DoorRamp","parent_entity_name":"Ship","source_transform_basis":"gltf_y_up","ui_bindings":[%s]},
          {"entity_name":"Cockpit_Screen","parent_entity_name":"Ship","source_transform_basis":"gltf_y_up","ui_bindings":[%s]},
          {"entity_name":"Console","parent_entity_name":"Ship","source_transform_basis":"gltf_y_up","ui_bindings":[%s,%s,%s,%s]}],
        "interiors":[{"name":"Deck","placements":[
          {"cgf_path":"pad.cgf","entity_id":"1","ui_bindings":[%s]},
          {"cgf_path":"hud.cgf","entity_id":"2","ui_bindings":[%s]},
          {"cgf_path":"lift.cgf","entity_id":"3","ui_bindings":[%s]},
          {"cgf_path":"off.cgf","entity_id":"4","ui_bindings":[%s]},
          {"cgf_path":"old.cgf","entity_id":"5","ui_bindings":[%s]},
          {"cgf_path":"unsafe.cgf","entity_id":"6","ui_bindings":[%s]},
          {"cgf_path":"cabinet.cgf","entity_id":"7","ui_bindings":[%s,%s]},
          {"cgf_path":"radar.cgf","entity_id":"8","ui_bindings":[%s]},
          {"cgf_path":"inline.cgf","entity_id":"9","ui_bindings":[%s]},
          {"cgf_path":"kiosk.cgf","entity_id":"10","ui_bindings":[%s]}]}]})"),
        // Dashboard: an MFD, and a self-owned physical cockpit screen (Vehicle_Screen_Physical, item type Display).
        *Binding(TEXT("mfd"), TEXT("Display"), TEXT("Vehicle_Screen_MFD"), TEXT("screen_centre"), TEXT("Data/UI/Generated/ship/synthetic/mfd.png")),
        *Binding(TEXT("physical"), TEXT("Display"), TEXT("Vehicle_Screen_Physical"), TEXT("screen_countermeasures"), TEXT("Data/UI/Generated/ship/synthetic/cm.png")),
        // DoorRamp: a geometry-less door-control child's binding, propagated onto the ramp at the ramp's node.
        *Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("ControlPanel_Screen_DoorControl"), TEXT("panel_slot"), Door),
        // A cockpit screen child whose binding is its own (source == host), still a display item.
        *Binding(TEXT("physical"), TEXT("Display"), TEXT("Cockpit_Screen"), nullptr, TEXT("Data/UI/Generated/ship/synthetic/hud.png")),
        // Console: two distinct panel images on two helpers, an MFD, and a panel binding that is off by default.
        *Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("Console"), TEXT("screen_a"), A),
        *Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("Console"), TEXT("screen_b"), B),
        *Binding(TEXT("mfd"), TEXT("Display"), TEXT("Vehicle_Screen_MFD"), TEXT("screen_mfd"), TEXT("Data/UI/Generated/ship/synthetic/mfd.png")),
        *Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("Console"), TEXT("screen_off"), A, true),
        *Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("ControlPanel_Door"), nullptr, Door),
        // The Ironclad command-module wall screen: an SCItemDisplayScreen showing the Cutlass countermeasures HUD canvas.
        *Binding(TEXT("physical"), TEXT("Display"), TEXT("ui_screen_RTT_4x3_a"), nullptr, TEXT("Data/UI/Generated/ship/synthetic/countermeasures.png")),
        // A transit panel: no attachable component, so a null item type (still a panel).
        *Binding(TEXT("physical"), nullptr, TEXT("console_elevator"), nullptr, TEXT("Data/UI/Generated/ship/synthetic/lift.png")),
        *Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("Off_Panel"), nullptr, TEXT("Data/UI/Generated/ship/synthetic/off.png"), true),
        // An export before source_item_type: cannot be classified, left dark.
        *Binding(TEXT("physical"), nullptr, TEXT("Old_Panel"), nullptr, Door, false, true),
        *Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("Unsafe"), nullptr, TEXT("../outside.png")),
        // A loadout-expanded cabinet: the helper is the parent's attachment point, not a node of this mesh.
        *Binding(TEXT("physical"), TEXT("Usable"), TEXT("Cabinet"), nullptr, Door),
        *Binding(TEXT("physical"), TEXT("Usable"), TEXT("Cabinet"), TEXT("attach"), Door),
        *Binding(TEXT("radar"), TEXT("Display"), TEXT("Engineering_Screen"), nullptr, TEXT("Data/UI/Generated/ship/synthetic/radar.png")),
        // An inline canvas on an entity without a class: the owner is unknown, so its item type is too (left dark).
        *Binding(TEXT("physical"), nullptr, TEXT("ui_screen_inline"), nullptr, TEXT("Data/UI/Generated/ship/synthetic/inline_hud.png"), false, false, true),
        // The hangar ship-selector kiosk: a Display item the player operates (KioskInteractionEntityComponentParams).
        *Binding(TEXT("physical"), TEXT("Display"), TEXT("ShipSelectorKiosk"), nullptr, TEXT("Data/UI/Generated/ship/synthetic/kiosk.png"), false, false, false, true));
    FScreenMemorySource Source;
    Source.Add(TEXT("scene.json"), Scene);
    FSCSceneManifest M; FString Error;
    if (!TestTrue(TEXT("reads: ") + Error, FSCPackageReader::Read(Source, M, Error))) return false;
    if (!TestEqual(TEXT("children"), M.Children.Num(), 4) || !TestEqual(TEXT("interiors"), M.Interiors.Num(), 1)
        || !TestEqual(TEXT("placements"), M.Interiors[0].Placements.Num(), 10)) return false;
    const FSCSceneInstance& Dash = M.Children[0];
    const FSCSceneInstance& Ramp = M.Children[1];
    const FSCSceneInstance& Cockpit = M.Children[2];
    const FSCSceneInstance& Console = M.Children[3];
    const TArray<FSCSceneInstance>& P = M.Interiors[0].Placements;
    if (TestEqual(TEXT("dashboard bindings"), Dash.UiBindings.Num(), 2))
    {
        const FSCUiBinding& Mfd = Dash.UiBindings[0];
        TestEqual(TEXT("kind"), Mfd.Kind, FString(TEXT("mfd")));
        TestEqual(TEXT("source entity"), Mfd.SourceEntityName, FString(TEXT("Vehicle_Screen_MFD")));
        TestEqual(TEXT("helper"), Mfd.HelperName, FString(TEXT("screen_centre")));
        TestEqual(TEXT("item type"), Mfd.SourceItemType, FString(TEXT("Display")));
        TestTrue(TEXT("classified"), Mfd.bClassified);
    }
    TestTrue(TEXT("null item type: classified, empty"), P[2].UiBindings[0].bClassified && P[2].UiBindings[0].SourceItemType.IsEmpty());
    TestFalse(TEXT("no source_item_type key: unclassified"), P[4].UiBindings[0].bClassified);
    TestTrue(TEXT("an image outside the package is dropped"), P[5].UiBindings[0].ImagePath.IsEmpty());

    // T1: the role comes from the owner's item type, not from who carries the binding.
    TestTrue(TEXT("door pad: panel"), P[0].UiBindings[0].IsOperatingPanel());
    TestTrue(TEXT("propagated door control on its parent: panel"), Ramp.UiBindings[0].IsOperatingPanel());
    TestFalse(TEXT("self-owned cockpit screen: not a panel"), Cockpit.UiBindings[0].IsOperatingPanel());
    TestFalse(TEXT("Ironclad countermeasures HUD on a wall display: not a panel"), P[1].UiBindings[0].IsOperatingPanel());
    TestFalse(TEXT("dashboard physical screen: not a panel"), Dash.UiBindings[1].IsOperatingPanel());
    TestTrue(TEXT("transit panel (no item type): panel"), P[2].UiBindings[0].IsOperatingPanel());
    TestFalse(TEXT("radar: not a panel"), P[7].UiBindings[0].IsOperatingPanel());
    TestFalse(TEXT("unclassified: not bound"), P[4].UiBindings[0].IsOperatingPanel());
    TestFalse(TEXT("unresolved owner (null item type, unknown): not bound"), P[8].UiBindings[0].IsOperatingPanel());
    TestTrue(TEXT("an operated display item (ship-selector kiosk): panel"), P[9].UiBindings[0].IsOperatingPanel());
    TestTrue(TEXT("a child's own binding is not propagated"), !Console.UiBindings[0].bPropagated && !Cockpit.UiBindings[0].bPropagated);
    TestTrue(TEXT("another entity's binding on a child is propagated"), Ramp.UiBindings[0].bPropagated && Console.UiBindings[2].bPropagated);
    TestFalse(TEXT("placements are their own entity"), P[0].UiBindings[0].bPropagated);

    // Decoding: every image a face may show; display renders counted as excluded, old exports as unclassified.
    {
        TSet<FString> Images; int32 Excluded = 0, Unclassified = 0;
        for (const FSCSceneInstance* I : {&Dash, &Ramp, &Cockpit, &Console}) I->PanelImages(Images, &Excluded, &Unclassified);
        for (const FSCSceneInstance& I : P) I.PanelImages(Images, &Excluded, &Unclassified);
        TestEqual(TEXT("images: door, a, b, lift, kiosk"), Images.Num(), 5);
        TestTrue(TEXT("the off-by-default render is not decoded"), !Images.Contains(TEXT("Data/UI/Generated/ship/synthetic/off.png")));
        TestEqual(TEXT("excluded: 2 dashboard, cockpit, console MFD, countermeasures, radar"), Excluded, 6);
        TestEqual(TEXT("unclassified: the old export and the unresolved owner"), Unclassified, 2);
    }

    // T2 / T3: per face. A helper that names a node of the mesh owns that node's subtree.
    auto Nodes = [](std::initializer_list<const TCHAR*> Names) { TSet<FString> S; for (const TCHAR* N : Names) S.Add(FString(N).ToLower()); return S; };
    auto Chain = [](std::initializer_list<const TCHAR*> Names) { TArray<FString> C; for (const TCHAR* N : Names) C.Add(FString(N).ToLower()); return C; };
    const TSet<FString> RampNodes = Nodes({TEXT("Ramp_Root"), TEXT("panel_slot"), TEXT("Screen_Quad"), TEXT("Hull")});
    bool bAmbiguous = false;
    TestEqual(TEXT("attached door-panel child: the ramp's face under the helper shows the door UI"),
              Ramp.ScreenSlotImage(RampNodes, {Chain({TEXT("Screen_Quad"), TEXT("panel_slot"), TEXT("Ramp_Root")})}), FString(Door));
    TestTrue(TEXT("a ramp screen face no binding names stays dark"), Ramp.ScreenSlotImage(RampNodes, {Chain({TEXT("Hull"), TEXT("Ramp_Root")})}).IsEmpty());
    TestTrue(TEXT("a propagated binding whose helper is not on the host claims no face"),
             Ramp.ScreenSlotImage(Nodes({TEXT("Ramp_Root"), TEXT("Hull")}), {Chain({TEXT("Hull"), TEXT("Ramp_Root")})}).IsEmpty());
    TestTrue(TEXT("unresolved owner placement: dark"), P[8].ScreenSlotImage(Nodes({TEXT("x")}), {Chain({TEXT("x")})}).IsEmpty());
    TestTrue(TEXT("self-owned cockpit screen: dark"), Cockpit.ScreenSlotImage(Nodes({TEXT("screen")}), {Chain({TEXT("screen")})}).IsEmpty());
    TestTrue(TEXT("Ironclad countermeasures HUD placement: dark"), P[1].ScreenSlotImage(Nodes({TEXT("screen")}), {Chain({TEXT("screen")})}).IsEmpty());
    const TSet<FString> ConsoleNodes = Nodes({TEXT("root"), TEXT("screen_a"), TEXT("screen_b"), TEXT("screen_mfd"), TEXT("screen_off")});
    TestEqual(TEXT("two images: face A"), Console.ScreenSlotImage(ConsoleNodes, {Chain({TEXT("screen_a"), TEXT("root")})}), FString(A));
    TestEqual(TEXT("two images: face B"), Console.ScreenSlotImage(ConsoleNodes, {Chain({TEXT("screen_b"), TEXT("root")})}), FString(B));
    TestTrue(TEXT("mixed: the MFD face beside a panel stays dark"), Console.ScreenSlotImage(ConsoleNodes, {Chain({TEXT("screen_mfd"), TEXT("root")})}).IsEmpty());
    TestTrue(TEXT("a panel screen off by default stays dark"), Console.ScreenSlotImage(ConsoleNodes, {Chain({TEXT("screen_off"), TEXT("root")})}).IsEmpty());
    TestTrue(TEXT("A and B sharing a slot: dark"), Console.ScreenSlotImage(ConsoleNodes,
        {Chain({TEXT("screen_a"), TEXT("root")}), Chain({TEXT("screen_b"), TEXT("root")})}, &bAmbiguous).IsEmpty());
    TestTrue(TEXT("A and B sharing a slot: ambiguous"), bAmbiguous);
    TestTrue(TEXT("panel and MFD sharing a slot: dark"), Console.ScreenSlotImage(ConsoleNodes,
        {Chain({TEXT("screen_a"), TEXT("root")}), Chain({TEXT("screen_mfd"), TEXT("root")})}, &bAmbiguous).IsEmpty() && bAmbiguous);
    TestTrue(TEXT("a face under no helper with no own binding: dark, not ambiguous"),
             Console.ScreenSlotImage(ConsoleNodes, {Chain({TEXT("root")})}, &bAmbiguous).IsEmpty() && !bAmbiguous);
    // Entity-level bindings (no helper on this mesh) own every face.
    TestEqual(TEXT("door pad placement"), P[0].ScreenSlotImage(Nodes({TEXT("pad")}), {Chain({TEXT("pad")})}), FString(Door));
    TestEqual(TEXT("lift panel placement"), P[2].ScreenSlotImage(Nodes({TEXT("panel")}), {Chain({TEXT("panel")})}),
              FString(TEXT("Data/UI/Generated/ship/synthetic/lift.png")));
    TestTrue(TEXT("off by default: dark"), P[3].ScreenSlotImage(Nodes({TEXT("x")}), {Chain({TEXT("x")})}).IsEmpty());
    TestTrue(TEXT("unclassified: dark"), P[4].ScreenSlotImage(Nodes({TEXT("x")}), {Chain({TEXT("x")})}).IsEmpty());
    TestTrue(TEXT("unsafe path: dark"), P[5].ScreenSlotImage(Nodes({TEXT("x")}), {Chain({TEXT("x")})}).IsEmpty());
    TestEqual(TEXT("loadout cabinet: the parent's helper is not this mesh's, one image"),
              P[6].ScreenSlotImage(Nodes({TEXT("cabinet")}), {Chain({TEXT("cabinet")})}, &bAmbiguous), FString(Door));
    TestFalse(TEXT("one image: not ambiguous"), bAmbiguous);
    TestTrue(TEXT("no owners: dark"), P[0].ScreenSlotImage(Nodes({TEXT("pad")}), {}).IsEmpty());
    return true;
}

namespace
{
/** A tiny solid-colour PNG (a panel image the importer decodes like the exporter's renders). */
TArray<uint8> SolidPng(FColor Colour)
{
    IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    TSharedPtr<IImageWrapper> Png = Module.CreateImageWrapper(EImageFormat::PNG);
    TArray<FColor> Pixels; Pixels.Init(Colour, 16);
    Png->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), 4, 4, ERGBFormat::BGRA, 8);
    const TArray64<uint8> Bytes = Png->GetCompressed();
    return TArray<uint8>(Bytes.GetData(), int32(Bytes.Num()));
}
void Append32(TArray<uint8>& B, uint32 V) { B.Append(reinterpret_cast<uint8*>(&V), 4); }
/** screens.glb: one triangle per primitive; node `panel_screen` draws slot 0 (PanelScreen) and slot 4 (Flash, opacity
 *  0.6), `mfd_screen` slot 1, `panel_b` and `mfd_b` share slot 2, `housing` slot 5 (Illum); slot 3 (UnusedScreen) is
 *  declared and drawn by nothing. */
TArray<uint8> ScreensGlb()
{
    const float Values[] = {0,0,0, 0.2f,0,0, 0,0.2f,0,  0,0,1, 0,0,1, 0,0,1,  0,0, 1,0, 0,1};
    TArray<uint8> Bin; Bin.Append(reinterpret_cast<const uint8*>(Values), sizeof(Values));
    const FString Json = TEXT(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":96}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0.2,0.2,0]},)"
        R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"}],)"
        R"("materials":[{"name":"PanelScreen"},{"name":"MfdScreen"},{"name":"SharedScreen"},{"name":"UnusedScreen"},{"name":"Flash"},{"name":"Housing"}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"material":0},{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"material":4}]},)"
        R"({"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"material":1}]},)"
        R"({"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"material":2}]},)"
        R"({"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"material":5}]}],)"
        R"("nodes":[{"name":"console_root","children":[1,2,3,4,5]},{"name":"panel_screen","mesh":0,"translation":[0,0,0]},)"
        R"({"name":"mfd_screen","mesh":1,"translation":[0.3,0,0]},{"name":"panel_b","mesh":2,"translation":[0.6,0,0]},)"
        R"({"name":"mfd_b","mesh":2,"translation":[0.9,0,0]},{"name":"housing","mesh":3,"translation":[0,0.3,0]}],)"
        R"("scenes":[{"nodes":[0]}],"scene":0})");
    FTCHARToUTF8 Utf8(*Json); TArray<uint8> J; J.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    while (J.Num() % 4) J.Add(' ');
    TArray<uint8> B; Append32(B, 0x46546c67); Append32(B, 2); Append32(B, 28 + J.Num() + Bin.Num());
    Append32(B, J.Num()); Append32(B, 0x4e4f534a); B.Append(J); Append32(B, Bin.Num()); Append32(B, 0x004e4942); B.Append(Bin);
    return B;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCScreensPerFace, "SCBridge.Screens.PerFaceAssembly", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCScreensPerFace::RunTest(const FString&)
{
    // Through import and assembly: each screen face shows its own binding's image, nothing else does.
    FScreenMemorySource Source;
    Source.Files.Add(TEXT("screens.glb"), ScreensGlb());
    const TCHAR* A = TEXT("Data/UI/Generated/ship/synthetic/panel_a.png");
    const TCHAR* B = TEXT("Data/UI/Generated/ship/synthetic/panel_b.png");
    const TCHAR* Door = TEXT("Data/UI/Generated/ship/synthetic/door.png");
    Source.Files.Add(A, SolidPng(FColor(200, 40, 40)));
    Source.Files.Add(B, SolidPng(FColor(40, 200, 40)));
    Source.Files.Add(Door, SolidPng(FColor(40, 40, 200)));
    Source.Files.Add(TEXT("Data/UI/Generated/ship/synthetic/mfd.png"), SolidPng(FColor(200, 200, 200)));
    Source.Files.Add(TEXT("Data/UI/Generated/ship/synthetic/hud.png"), SolidPng(FColor(200, 200, 40)));
    Source.Add(TEXT("screens.materials.json"), FString::Printf(TEXT(R"({"version":2,"submaterials":[%s,%s,%s,%s,%s,{"submaterial_name":"Housing","shader_family":"Illum"}]})"),
        *ScreenJson(TEXT("PanelScreen"), TEXT("DisplayScreen"), true), *ScreenJson(TEXT("MfdScreen"), TEXT("DisplayScreen"), true),
        *ScreenJson(TEXT("SharedScreen"), TEXT("DisplayScreen"), true), *ScreenJson(TEXT("UnusedScreen"), TEXT("DisplayScreen"), true),
        *ScreenJson(TEXT("Flash"), TEXT("DisplayScreen"), true, false, TEXT("active"), nullptr, 0.6f)));
    auto Child = [](const TCHAR* Name, const FString& Bindings) {
        return FString::Printf(TEXT(R"({"entity_name":"%s","parent_entity_name":"Root","mesh_asset":"screens.glb","material_sidecar":"screens.materials.json",)"
                                    R"("source_transform_basis":"gltf_y_up","ui_bindings":[%s]})"), Name, *Bindings);
    };
    const FString Mfd = TEXT("Data/UI/Generated/ship/synthetic/mfd.png");
    Source.Add(TEXT("scene.json"), FString::Printf(TEXT(R"({"version":2,"export_kind":"Decomposed","root_entity":{"entity_name":"Root"},"children":[%s,%s,%s,%s,%s]})"),
        // Mixed panel / MFD child: a panel on panel_screen and panel_b, MFDs on mfd_screen and mfd_b (panel_b and mfd_b share slot 2).
        *Child(TEXT("Console"), Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("Console_Panel"), TEXT("panel_screen"), A) + TEXT(",")
            + Binding(TEXT("mfd"), TEXT("Display"), TEXT("Vehicle_Screen_MFD"), TEXT("mfd_screen"), *Mfd) + TEXT(",")
            + Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("Console_Panel"), TEXT("panel_b"), A) + TEXT(",")
            + Binding(TEXT("mfd"), TEXT("Display"), TEXT("Vehicle_Screen_MFD"), TEXT("mfd_b"), *Mfd)),
        // An attached door-panel child's binding carried by its host at the host's node.
        *Child(TEXT("Ramp"), Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("Ramp_DoorControl"), TEXT("mfd_screen"), Door)),
        // A self-owned cockpit screen (display item).
        *Child(TEXT("Cockpit"), Binding(TEXT("physical"), TEXT("Display"), TEXT("Cockpit"), nullptr, TEXT("Data/UI/Generated/ship/synthetic/hud.png"))),
        // Two distinct panel images on one entity.
        *Child(TEXT("Twin"), Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("Twin"), TEXT("panel_screen"), A) + TEXT(",")
            + Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("Twin"), TEXT("mfd_screen"), B)),
        // A propagated door control whose helper is not a node of the host: it has no face here, and lights none.
        *Child(TEXT("Host"), Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("Host_DoorControl"), TEXT("missing_helper"), Door))));
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FSCImportOptions Options; Options.World = World; Options.bBuildCollision = false;
    FSCImportResult Result;
    TestTrue(TEXT("imports"), FSCImporter::Import(Source, Options, Result));
    if (Result.Vehicle.IsValid())
    {
        // What every drawn screen section shows: entity/slot -> image file name, "dark", or "missing".
        TMap<FString, FString> Shown;
        TArray<UStaticMeshComponent*> Components;
        Result.Vehicle->GetComponents(Components);
        for (UStaticMeshComponent* C : Components)
        {
            const UStaticMesh* Mesh = C->GetStaticMesh();
            const USceneComponent* Anchor = C->GetAttachParent();
            if (!Mesh || !Anchor || !Mesh->GetRenderData() || C->ComponentHasTag(TEXT("SCGameCollision"))) continue;
            const FString Entity = Anchor->GetName().Left(Anchor->GetName().Find(TEXT("_"))).IsEmpty() ? Anchor->GetName()
                                 : Anchor->GetName().Left(Anchor->GetName().Find(TEXT("_")));
            for (const FStaticMeshSection& Section : Mesh->GetRenderData()->LODResources[0].Sections)
            {
                if (!Section.NumTriangles) continue;
                const UMaterialInterface* Material = C->GetMaterial(Section.MaterialIndex);
                if (!FSCMaterialBuilder::IsScreenMaster(Material)) continue;
                UTexture* Image = nullptr;
                Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FSCMaterialBuilder::ScreenImageParam), Image);
                const FString* Path = Image ? Result.Vehicle->SourcePaths.Find(Image) : nullptr;
                float Strength = -1.f;
                Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("EmissiveStrength"))), Strength);
                Shown.Add(FString::Printf(TEXT("%s/%d"), *Entity, Section.MaterialIndex),
                          Path ? FPaths::GetBaseFilename(*Path) + (Strength > 0.f ? TEXT("") : TEXT(" (no glow)"))
                               : Strength == 0.f ? FString(TEXT("dark")) : FString::Printf(TEXT("dark (glow %g)"), Strength));
            }
        }
        auto Expect = [&](const TCHAR* Key, const TCHAR* Value) { TestEqual(Key, Shown.FindRef(Key), FString(Value)); };
        Expect(TEXT("Console/0"), TEXT("panel_a"));  // the panel's own face
        Expect(TEXT("Console/4"), TEXT("panel_a"));  // its 0.6 overlay, in the translucent split, same node
        Expect(TEXT("Console/1"), TEXT("dark"));     // the MFD beside it: dark, emitting nothing (A8)
        Expect(TEXT("Console/2"), TEXT("dark"));     // panel_b and mfd_b share the slot: cannot be told apart
        Expect(TEXT("Ramp/1"), TEXT("door"));        // the propagated door control at the host's node
        Expect(TEXT("Ramp/0"), TEXT("dark"));        // the host's other screens
        Expect(TEXT("Ramp/2"), TEXT("dark"));
        Expect(TEXT("Cockpit/0"), TEXT("dark"));     // a self-owned display item
        Expect(TEXT("Cockpit/1"), TEXT("dark"));
        Expect(TEXT("Twin/0"), TEXT("panel_a"));     // two distinct images on one entity, each on its face
        Expect(TEXT("Twin/1"), TEXT("panel_b"));
        Expect(TEXT("Twin/4"), TEXT("panel_a"));
        Expect(TEXT("Twin/2"), TEXT("dark"));
        for (const TCHAR* Key : {TEXT("Host/0"), TEXT("Host/1"), TEXT("Host/2"), TEXT("Host/4")}) Expect(Key, TEXT("dark"));
        for (const TCHAR* Entity : {TEXT("Console"), TEXT("Ramp"), TEXT("Cockpit"), TEXT("Twin"), TEXT("Host")})
            TestFalse(FString(Entity) + TEXT(": the unused screen slot draws nothing"), Shown.Contains(FString(Entity) + TEXT("/3")));
        // The counters SCREEN_BIND reports: only drawn screen sections count (slot 3 never does).
        TestEqual(TEXT("bound instances: Console, Ramp, Twin"), Result.ScreenBoundInstances, 3);
        TestEqual(TEXT("bound slots: Console 0 and 4, Ramp 1, Twin 0, 1 and 4"), Result.ScreenBoundSlots, 6);
        TestEqual(TEXT("dark slots: Console 1 and 2, Ramp 0, 2 and 4, Cockpit 0, 1, 2 and 4, Twin 2, Host 0, 1, 2 and 4"), Result.ScreenDarkSlots, 14);
        TestEqual(TEXT("ambiguous: Console slot 2"), Result.ScreenAmbiguousSlots, 1);
    }
    FSCImporter::Unload(World); World->DestroyWorld(false);
    return true;
}
// SCBridge.Screens.StateImages (WP-I1 I1-3): an operating panel's `state_images[]` are read with the binding (an image
// outside the package, or an entry without its state, is dropped) and decoded with its default image; a display item's
// are not. A pad shows its door's DoorOpenState: opening / closing while the door moves, opened / closed at rest.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCScreenStateImages, "SCBridge.Screens.StateImages", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCScreenStateImages::RunTest(const FString&)
{
    const TCHAR* States = TEXT(R"(,"state_images":[
        {"state_type":"DoorOpenState","state":"DoorOpened","generated_image_path":"Data/UI/Generated/ship/synthetic/door__open.png"},
        {"state_type":"DoorLockState","state":"DoorLocked","generated_image_path":"../outside.png"},
        {"state_type":"","state":"DoorClosing","generated_image_path":"Data/UI/Generated/ship/synthetic/door__x.png"}]})");
    auto WithStates = [&](FString B) { B.RemoveFromEnd(TEXT("}")); return B + States; };
    const FString Scene = FString::Printf(TEXT(R"({"version":2,"export_kind":"Decomposed","root_entity":{"entity_name":"Ship"},
        "interiors":[{"name":"Deck","placements":[
          {"cgf_path":"pad.cgf","entity_id":"1","ui_bindings":[%s]},
          {"cgf_path":"hud.cgf","entity_id":"2","ui_bindings":[%s]}]}]})"),
        *WithStates(Binding(TEXT("physical"), TEXT("ControlPanel"), TEXT("Pad"), nullptr, TEXT("Data/UI/Generated/ship/synthetic/door.png"))),
        *WithStates(Binding(TEXT("mfd"), TEXT("Display"), TEXT("Vehicle_Screen_MFD"), nullptr, TEXT("Data/UI/Generated/ship/synthetic/mfd.png"))));
    FScreenMemorySource Source;
    Source.Add(TEXT("scene.json"), Scene);
    FSCSceneManifest M; FString Error;
    if (!TestTrue(TEXT("reads: ") + Error, FSCPackageReader::Read(Source, M, Error))) return false;
    const FSCSceneInstance& Pad = M.Interiors[0].Placements[0];
    const FSCSceneInstance& Hud = M.Interiors[0].Placements[1];
    if (!TestEqual(TEXT("pad state images (unsafe and nameless dropped)"), Pad.UiBindings[0].StateImages.Num(), 1)) return false;
    TestEqual(TEXT("key"), Pad.UiBindings[0].StateImages[0].Key(), FString(TEXT("DoorOpenState.DoorOpened")));
    TestTrue(TEXT("panel binding of the default image"), Pad.PanelBinding(Pad.UiBindings[0].PanelImage()) == &Pad.UiBindings[0]);
    TSet<FString> Images;
    Pad.PanelImages(Images);
    TestTrue(TEXT("the state image is decoded with the panel"), Images.Contains(TEXT("Data/UI/Generated/ship/synthetic/door__open.png")));
    TestEqual(TEXT("an MFD's state images are not shown"), Hud.UiBindings[0].PanelStateImages().Num(), 0);
    TestEqual(TEXT("closed"), ASCVehicle::DoorOpenStateKey(false, false), FString(TEXT("DoorOpenState.DoorClosed")));
    TestEqual(TEXT("opened"), ASCVehicle::DoorOpenStateKey(true, false), FString(TEXT("DoorOpenState.DoorOpened")));
    TestEqual(TEXT("opening"), ASCVehicle::DoorOpenStateKey(true, true), FString(TEXT("DoorOpenState.DoorOpening")));
    TestEqual(TEXT("closing"), ASCVehicle::DoorOpenStateKey(false, true), FString(TEXT("DoorOpenState.DoorClosing")));
    return true;
}
#endif
