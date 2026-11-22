// WP-I1-4 tests: the package fields of the interaction feedback (`hover_images[]`, `usable_interactions[]`) and the light
// panel's choice of the hover render for the row under the crosshair.
#if WITH_DEV_AUTOMATION_TESTS
#include "SCPackageReader.h"
#include "SCPackageSource.h"
#include "SCFloorPanel.h"
#include "SCVehicle.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCReaderInteractionFeedbackTest, "SCBridge.Reader.InteractionFeedbackFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCReaderInteractionFeedbackTest::RunTest(const FString&)
{
    class FSource : public FSCPackageSource
    {
    public:
        FString Text;
        bool Exists(const FString& Path) const override { return Path == TEXT("scene.json"); }
        bool ReadBytes(const FString& Path, TArray<uint8>& Out, FString& Error) const override
        {
            FTCHARToUTF8 Utf8(*Text);
            Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
            return true;
        }
        TArray<FString> ListFiles(const FString&) const override { return {TEXT("scene.json")}; }
    } Source;
    // Synthetic names; the shapes of cache format 43.
    Source.Text = TEXT(R"({"version":2,"export_kind":"Decomposed","root_entity":{"entity_name":"Ship"},
      "interiors":[{"name":"hold","container_transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
        "placements":[
          {"cgf_path":"pad.cgf","entity_id":"pad","transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
           "ui_bindings":[{"binding_kind":"physical","generated_image_path":"Data/UI/pad.png","image_size_px":[100,100],
             "buttons":[{"node":"DoorButton","interaction":"OpenDoor","rect_px":[0,0,100,70]}],
             "state_images":[{"state_type":"DoorOpenState","state":"DoorOpened","generated_image_path":"Data/UI/pad_open.png"}],
             "hover_images":[{"node":"DoorButton","states":[],"generated_image_path":"Data/UI/pad__hover_doorbutton.png"},
               {"node":"DoorButton","states":[{"state_type":"DoorOpenState","state":"DoorOpened"}],"generated_image_path":"Data/UI/pad_open__hover_doorbutton.png"},
               {"node":"","states":[],"generated_image_path":"Data/UI/unnamed.png"}]}]},
          {"cgf_path":"bed.cgf","entity_id":"bed","transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
           "usable_interactions":["Rest","Perch",""],
           "interaction_points":[{"helper_name":"$IP_a","radius":1.6,"interactions":[{"name":"Rest","display_name":"Rest","primary":false},
             {"name":"Leave","display_name":"Leave","primary":false}]}]}]}]})");
    FSCSceneManifest M;
    FString Error;
    if (!TestTrue(TEXT("reads: ") + Error, FSCPackageReader::Read(Source, M, Error)) || !TestEqual(TEXT("interior"), M.Interiors.Num(), 1) ||
        !TestEqual(TEXT("placements"), M.Interiors[0].Placements.Num(), 2))
        return false;
    const FSCSceneInstance& Pad = M.Interiors[0].Placements[0];
    if (TestEqual(TEXT("binding"), Pad.UiBindings.Num(), 1))
    {
        const TArray<FSCUiHoverImage>& H = Pad.UiBindings[0].HoverImages;
        if (TestEqual(TEXT("hover images (one without a node dropped)"), H.Num(), 2))
        {
            TestEqual(TEXT("the default's hover: no state key"), H[0].StateKey(), FString());
            TestEqual(TEXT("a state's hover: its state key"), H[1].StateKey(), FString(TEXT("DoorOpenState.DoorOpened")));
            TestEqual(TEXT("as the state image's key"), H[1].StateKey(), Pad.UiBindings[0].StateImages[0].Key());
            TestEqual(TEXT("face key"), FSCUiHoverImage::FaceKey(H[1].Node, H[1].StateKey()), FString(TEXT("DoorButton|DoorOpenState.DoorOpened")));
        }
    }
    const FSCSceneInstance& Bed = M.Interiors[0].Placements[1];
    TestTrue(TEXT("usable interactions (an empty name dropped)"),
             Bed.UsableInteractions == TArray<FName>{FName(TEXT("Rest")), FName(TEXT("Perch"))});
    TestTrue(TEXT("not a usable"), Pad.UsableInteractions.IsEmpty());
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCButtonPanelHoverImageTest, "SCBridge.Screens.ButtonPanelHoverImage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCButtonPanelHoverImageTest::RunTest(const FString&)
{
    FSCFloorPanel P;
    P.Kind = ESCPanelKind::Buttons;
    P.DefaultImage = TEXT("default.png");
    FSCUiStateImage Alarm;
    Alarm.ImagePath = TEXT("alarm.png");
    Alarm.States = {TEXT("LightState.LightsAlarm")};
    P.StateImages = {Alarm};
    P.Layout.Options = {{INDEX_NONE, TEXT("LightsOn"), TEXT("On"), FBox2D(FVector2D(0, 0), FVector2D(1, 0.5)), TEXT("OnButton")},
                        {INDEX_NONE, TEXT("LightsAlarm"), TEXT("Alarm"), FBox2D(FVector2D(0, 0.5), FVector2D(1, 1)), TEXT("AlarmButton")}};
    auto Hover = [](const TCHAR* Node, std::initializer_list<const TCHAR*> States, const TCHAR* Path) {
        FSCUiHoverImage H;
        H.Node = Node;
        for (const TCHAR* S : States) H.States.Add(S);
        H.ImagePath = Path;
        return H;
    };
    P.HoverImages = {Hover(TEXT("OnButton"), {}, TEXT("default__on.png")), Hover(TEXT("AlarmButton"), {}, TEXT("default__alarm.png")),
                     Hover(TEXT("OnButton"), {TEXT("LightState.LightsAlarm")}, TEXT("alarm__on.png"))};
    TestEqual(TEXT("the default render, its row aimed at"), P.HoverImageFor(TEXT("default.png"), 1), FString(TEXT("default__alarm.png")));
    TestEqual(TEXT("a state's render keeps its state"), P.HoverImageFor(TEXT("alarm.png"), 0), FString(TEXT("alarm__on.png")));
    TestEqual(TEXT("no hover render of that state and row: none (our frame)"), P.HoverImageFor(TEXT("alarm.png"), 1), FString());
    TestEqual(TEXT("no row aimed at"), P.HoverImageFor(TEXT("default.png"), INDEX_NONE), FString());
    TestEqual(TEXT("an image the panel does not list"), P.HoverImageFor(TEXT("other.png"), 0), FString());
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCPadDoorButtonTest, "SCBridge.Screens.PadDoorButton",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCPadDoorButtonTest::RunTest(const FString&)
{
    // A pad's buttons and state machine as the C2 exports them (synthetic names): the lock and light buttons set the pad's
    // own states, the door button's open/close acts on the door.
    auto Button = [](const TCHAR* Node, const TCHAR* I) { return FSCUiButton{Node, FName(I), FBox2D(FVector2D(0, 0), FVector2D(1, 1))}; };
    const TArray<FSCUiButton> Buttons = {Button(TEXT("Shelf"), TEXT("Latch")), Button(TEXT("Big"), TEXT("Swing")), Button(TEXT("Big"), TEXT("Unswing"))};
    FSCUiStateType Lock;
    Lock.Name = TEXT("LockState");
    Lock.Transitions = {{FString(), TEXT("Latch"), TEXT("Latched")}};
    FSCUiStateType Open; // set only by the door's own events
    Open.Name = TEXT("OpenState");
    Open.Transitions = {{TEXT("Closed"), TEXT("SyncOpen"), TEXT("Opening")}};
    TestEqual(TEXT("the button that sets none of the pad's states"), FSCPanelAim::DoorButtonOf(Buttons, {Lock, Open}), FString(TEXT("Big")));
    TestEqual(TEXT("every button sets one: none"), FSCPanelAim::DoorButtonOf({Buttons[0]}, {Lock, Open}), FString());
    return !HasAnyErrors();
}
#endif
