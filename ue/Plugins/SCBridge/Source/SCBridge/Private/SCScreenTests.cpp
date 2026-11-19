// SCBridge.Materials.ScreenFamily / SCBridge.Package.UiBindings (WP-I1 slice I1-1): the game's render-to-texture
// DisplayScreen face is an opaque dark glass screen whose emission is the bound panel image x the authored glow; the
// package's ui_bindings name that image, bound to door and lift operating panels only (MFD and HUD screens stay blank).
// Submaterials are synthetic, shaped like the LIVE 4.10.2 sidecars (Starlifter_Int RTT_Screens, template_int_a screen).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCMaterialBuilder.h"
#include "SCStarEngine.h"
#include "SCPackageReader.h"
#include "SCPackageSource.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"

namespace
{
/** A v2 sidecar submaterial: `Family` with authored Specular / Shininess / Glow, `BackColour` public param, the
 *  alpha_config and emissive objects, and optionally the virtual $RenderToTexture slot or an exported base colour. */
FSCSubmaterial ScreenSub(const TCHAR* Name, const TCHAR* Family, bool bRenderToTexture, bool bForward = false,
    const TCHAR* State = TEXT("active"), const TCHAR* BaseColour = nullptr)
{
    FSCSubmaterial S;
    S.Name = Name; S.ShaderFamily = Family;
    const FString Slots = FString(bRenderToTexture
        ? TEXT(R"({"slot":"TexSlot9","role":"render_to_texture","is_virtual":true,"export_path":null,"source_path":"$RenderToTexture"})")
        : TEXT(""))
        + (BaseColour ? FString::Printf(TEXT(R"(%s{"slot":"TexSlot1","role":"base_color","is_virtual":false,"export_path":"%s"})"),
                                        bRenderToTexture ? TEXT(",") : TEXT(""), BaseColour) : FString());
    S.Json = FString::Printf(TEXT(R"({"index":8,"submaterial_name":"%s","shader_family":"%s","shader":"%s",
        "authored_attributes":[{"name":"Shader","value":"%s"},{"name":"Diffuse","value":"1,1,1"},
          {"name":"Specular","value":"0.0194,0.0194,0.0194"},{"name":"Emissive","value":"0,0,0"},{"name":"Opacity","value":"1"},
          {"name":"Shininess","value":"170"},{"name":"Glow","value":"0.08"}],
        "public_params":{"BackColour":"0.0176,0.0176,0.0176","EmissiveContrast":1,"EmissiveSaturation":1},
        "alpha_config":{"alpha_test":0.0,"forward_rendering":%s,"opacity":1.0,"two_sided":false},
        "emissive":{"alpha_glow_cutoff":null,"color":[1.0,1.0,1.0],"emittance_texture":null,"forward_rendering":%s,"glow":0.08,"strength":0.08,"uses_base_texture":true},
        "decoded_feature_flags":{"tokens":["EMISSIVE_MAP","DBA_GLOBAL"]},
        "texture_slots":[%s],"derived_textures":[],"layer_manifest":[],
        "palette_routing":{"layer_channels":[],"material_channel":null},
        "activation_state":{"state":"%s","reason":"synthetic"}})"),
        Name, Family, Family, Family, bForward ? TEXT("true") : TEXT("false"), bForward ? TEXT("true") : TEXT("false"), *Slots, State);
    return S;
}
const FString ScreenMaster = TEXT("/SCBridge/Materials/M_SC_Screen.M_SC_Screen");
const FString UnlitMaster = TEXT("/SCBridge/Materials/M_SC_Emissive_Unlit.M_SC_Emissive_Unlit");
}

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
        TestEqual(Where + TEXT("emission strength = the authored Glow by the game's glow rule"), D.Scalars.FindRef(TEXT("EmissiveStrength")), Nits);
        TestTrue(Where + TEXT("emission tint = the v2 colour"), D.Vectors.FindRef(TEXT("EmissiveColor")).Equals(FLinearColor::White));
        TestFalse(Where + TEXT("no image in the description (bound per instance)"), D.Textures.Contains(TEXT("ScreenImage")));
        TestEqual(Where + TEXT("no warnings"), D.Warnings.Num(), 0);
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
    {
        // The master: opaque Nanite surface with the image, gloss and look parameters the builder sets.
        UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr, *ScreenMaster);
        if (TestNotNull(TEXT("M_SC_Screen exists"), Master))
        {
            TestEqual(TEXT("M_SC_Screen opaque"), int32(Master->GetBlendMode()), int32(BLEND_Opaque));
            TestTrue(TEXT("M_SC_Screen usable on skeletal meshes"), Master->GetMaterial()->GetUsageByFlag(MATUSAGE_SkeletalMesh));
            TestTrue(TEXT("M_SC_Screen usable instanced"), Master->GetMaterial()->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes));
            UTexture* Image = nullptr;
            TestTrue(TEXT("M_SC_Screen has ScreenImage"), Master->GetTextureParameterDefaultValue(FHashedMaterialParameterInfo(FSCMaterialBuilder::ScreenImageParam), Image));
            TestEqual(TEXT("unbound screen image is black (dark glass)"), GetNameSafe(Image), FString(TEXT("T_SC_Black")));
            for (const TCHAR* Name : {TEXT("ScreenBackColor"), TEXT("SpecularColor"), TEXT("EmissiveColor")})
            {
                FLinearColor V;
                TestTrue(FString(TEXT("M_SC_Screen has ")) + Name, Master->GetVectorParameterDefaultValue(FHashedMaterialParameterInfo(FName(Name)), V));
            }
            for (const FName Name : {FName(TEXT("EmissiveStrength")), FName(TEXT("Smoothness")), FName(TEXT("GlossMult")), FName(TEXT("UseSmoothnessMap")),
                                     FSCMaterialBuilder::GlassReflectanceScaleParam, FSCMaterialBuilder::GlassReflectanceFloorParam})
            {
                float V;
                TestTrue(TEXT("M_SC_Screen has ") + Name.ToString(), Master->GetScalarParameterDefaultValue(FHashedMaterialParameterInfo(Name), V));
            }
        }
    }
    return true;
}

namespace
{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCUiBindings, "SCBridge.Package.UiBindings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCUiBindings::RunTest(const FString&)
{
    // Shapes from the LIVE C2 package; names synthetic. A dashboard child carries an MFD and a mounted HUD projector; a
    // door-button child carries its own door UI; interior placements carry door pads, a radar screen, a screen that is
    // off by default, a two-screen panel with different images, and an unsafe image path.
    auto Binding = [](const TCHAR* Kind, const TCHAR* Source, const TCHAR* Helper, const TCHAR* Image, bool bOff = false, const TCHAR* Aspect = TEXT("null")) {
        return FString::Printf(TEXT(R"({"binding_kind":"%s","source_entity_name":"%s","helper_name":%s,"canvas_record_name":"BuildingBlocks_Canvas.Synthetic",)"
            R"("content_canvas_record_name":null,"default_state_is_off":%s,"generated_image_path":%s,"ui_screen_aspect_w_over_h":%s})"),
            Kind, Source, Helper ? *FString::Printf(TEXT("\"%s\""), Helper) : TEXT("null"), bOff ? TEXT("true") : TEXT("false"),
            Image ? *FString::Printf(TEXT("\"%s\""), Image) : TEXT("null"), Aspect);
    };
    const TCHAR* Door = TEXT("Data/UI/Generated/ship/synthetic/door.png");
    const FString Scene = FString::Printf(TEXT(R"({"version":2,"export_kind":"Decomposed","root_entity":{"entity_name":"Ship"},
        "children":[
          {"entity_name":"Dashboard","parent_entity_name":"Ship","source_transform_basis":"gltf_y_up","ui_bindings":[%s,%s]},
          {"entity_name":"DoorButton","parent_entity_name":"Ship","source_transform_basis":"gltf_y_up","ui_bindings":[%s]}],
        "interiors":[{"name":"Deck","placements":[
          {"cgf_path":"pad.cgf","entity_id":"1","ui_bindings":[%s]},
          {"cgf_path":"radar.cgf","entity_id":"2","ui_bindings":[%s]},
          {"cgf_path":"off.cgf","entity_id":"3","ui_bindings":[%s]},
          {"cgf_path":"two.cgf","entity_id":"4","ui_bindings":[%s,%s]},
          {"cgf_path":"same.cgf","entity_id":"5","ui_bindings":[%s,%s]},
          {"cgf_path":"unsafe.cgf","entity_id":"6","ui_bindings":[%s]},
          {"cgf_path":"none.cgf","entity_id":"7","ui_bindings":[%s]}]}]})"),
        *Binding(TEXT("mfd"), TEXT("Vehicle_Screen_MFD"), TEXT("screen_centre"), TEXT("Data/UI/Generated/ship/synthetic/mfd.png"), false, TEXT("1.7777")),
        *Binding(TEXT("physical"), TEXT("Dashboard_Projector_HUD"), TEXT("hud_plane"), TEXT("Data/UI/Generated/ship/synthetic/hud.png")),
        *Binding(TEXT("physical"), TEXT("DoorButton"), TEXT("Screen"), Door),
        *Binding(TEXT("physical"), TEXT("ControlPanel_Door"), nullptr, Door),
        *Binding(TEXT("radar"), TEXT("Engineering_Screen"), nullptr, TEXT("Data/UI/Generated/ship/synthetic/radar.png")),
        *Binding(TEXT("physical"), TEXT("Off_Panel"), nullptr, TEXT("Data/UI/Generated/ship/synthetic/off.png"), true),
        *Binding(TEXT("physical"), TEXT("Two_Screen"), TEXT("screen_a"), TEXT("Data/UI/Generated/ship/synthetic/a.png")),
        *Binding(TEXT("physical"), TEXT("Two_Screen"), TEXT("screen_b"), TEXT("Data/UI/Generated/ship/synthetic/b.png")),
        *Binding(TEXT("physical"), TEXT("Cabinet"), nullptr, Door),
        *Binding(TEXT("physical"), TEXT("Cabinet"), TEXT("attach"), Door),
        *Binding(TEXT("physical"), TEXT("Unsafe"), nullptr, TEXT("../outside.png")),
        *Binding(TEXT("physical"), TEXT("NoRender"), nullptr, nullptr));
    FScreenMemorySource Source;
    Source.Add(TEXT("scene.json"), Scene);
    FSCSceneManifest M; FString Error;
    if (!TestTrue(TEXT("reads: ") + Error, FSCPackageReader::Read(Source, M, Error))) return false;
    if (!TestEqual(TEXT("children"), M.Children.Num(), 2) || !TestEqual(TEXT("interiors"), M.Interiors.Num(), 1)
        || !TestEqual(TEXT("placements"), M.Interiors[0].Placements.Num(), 7)) return false;
    const FSCSceneInstance& Dash = M.Children[0];
    if (TestEqual(TEXT("dashboard bindings"), Dash.UiBindings.Num(), 2))
    {
        const FSCUiBinding& B = Dash.UiBindings[0];
        TestEqual(TEXT("kind"), B.Kind, FString(TEXT("mfd")));
        TestEqual(TEXT("source entity"), B.SourceEntityName, FString(TEXT("Vehicle_Screen_MFD")));
        TestEqual(TEXT("helper"), B.HelperName, FString(TEXT("screen_centre")));
        TestEqual(TEXT("canvas"), B.CanvasRecordName, FString(TEXT("BuildingBlocks_Canvas.Synthetic")));
        TestTrue(TEXT("no content canvas"), B.ContentCanvasRecordName.IsEmpty());
        TestEqual(TEXT("image path"), B.ImagePath, FString(TEXT("Data/UI/Generated/ship/synthetic/mfd.png")));
        TestEqual(TEXT("aspect"), B.AspectWOverH, 1.7777f);
        TestEqual(TEXT("unmeasured aspect is 0"), Dash.UiBindings[1].AspectWOverH, 0.f);
    }
    const TArray<FSCSceneInstance>& P = M.Interiors[0].Placements;
    TestTrue(TEXT("default_state_is_off read"), P[2].UiBindings.Num() == 1 && P[2].UiBindings[0].bDefaultStateIsOff);
    TestTrue(TEXT("an image outside the package is dropped"), P[5].UiBindings.Num() == 1 && P[5].UiBindings[0].ImagePath.IsEmpty());
    TestTrue(TEXT("a binding without a render has no image"), P[6].UiBindings.Num() == 1 && P[6].UiBindings[0].ImagePath.IsEmpty());

    // The rule: own physical panel screens are bound; MFD / radar bindings and screens mounted on an item are not.
    int32 Skipped = 0; bool bAmbiguous = false;
    TestTrue(TEXT("dashboard: MFD and mounted HUD stay unbound"), Dash.PanelImage(false, &Skipped).IsEmpty());
    TestEqual(TEXT("dashboard: both counted as vehicle screens"), Skipped, 2);
    TestEqual(TEXT("door button child: its own door UI"), M.Children[1].PanelImage(false), FString(Door));
    TestEqual(TEXT("door pad placement"), P[0].PanelImage(true), FString(Door));
    Skipped = 0;
    TestTrue(TEXT("radar placement unbound"), P[1].PanelImage(true, &Skipped).IsEmpty());
    TestEqual(TEXT("radar counted"), Skipped, 1);
    TestTrue(TEXT("off by default: dark"), P[2].PanelImage(true).IsEmpty());
    TestTrue(TEXT("two different images: none (no face match)"), P[3].PanelImage(true, nullptr, &bAmbiguous).IsEmpty());
    TestTrue(TEXT("two different images: reported"), bAmbiguous);
    TestEqual(TEXT("two bindings, one image"), P[4].PanelImage(true, nullptr, &bAmbiguous), FString(Door));
    TestFalse(TEXT("one image: not ambiguous"), bAmbiguous);
    TestTrue(TEXT("unsafe path: no image"), P[5].PanelImage(true).IsEmpty());
    TestTrue(TEXT("no render: no image"), P[6].PanelImage(true).IsEmpty());
    TestEqual(TEXT("no bindings: no image"), M.Root.PanelImage(false), FString());
    return true;
}
#endif
