// WP-A1: unit tests of the audio rules (SCAudio.h) on synthetic manifests: container logic, falloff evaluation, manifest
// parsing, bus gain composition and the room -> environment -> convolution reverb mapping. No game data.
#include "SCAudio.h"
#include "Misc/AutomationTest.h"

namespace
{
/** A synthetic audio.json: a random container over two sounds, an attenuation, a bus chain with a Convolution
 *  Reverb aux bus, an environment, a room and an entity class with a trigger and a tag effect. */
const TCHAR* Manifest = TEXT(R"json({
  "version": 1,
  "emitters": { "spots": 3, "areas": 1 },
  "triggers": {
    "Play_Synthetic": { "status": "playable", "duration_type": "Infinite", "radius_max": 10,
      "play": [ { "root": 0, "delay_s": 0.25, "fade_in_s": 0, "context": { "volume_db": -8, "game_aux_send_db": -6,
        "rtpc": [ { "rtpc": "Listener_Volume", "rtpc_type": 0, "param": "volume", "scaling": 2, "points": [[0, -1, 4], [1, 0, 4]] } ] } } ] },
    "Stop_Synthetic": { "status": "no_media", "reason": "only stops", "other": [ { "action_type": 258, "target": 100, "delay_s": 0, "fade_s": 0.5 } ] }
  },
  "nodes": [
    { "id": 100, "kind": "random", "children": [1, 2], "weights": [50, 25], "avoid_repeat": 1, "shuffle": false, "continuous": true,
      "loop_count": 0, "transition": "crossfade_amp", "transition_s": 1.0, "volume_db": 2, "random": [["pitch", -100, 100]],
      "bus": 7, "spatial": true, "attenuation": 9, "use_game_aux": true },
    { "id": 101, "kind": "sound", "media": 500, "loop_count": 1, "volume_db": -3, "bus": 7, "spatial": true, "attenuation": 9, "use_game_aux": true },
    { "id": 102, "kind": "sound", "media": 501, "loop_count": 0, "bus": 7, "spatial": true, "attenuation": 9, "use_game_aux": true },
    { "id": 103, "kind": "blend", "children": [1, 2], "global": true, "transition_random_s": [-0.5, 0.5],
      "layers": [ { "rtpc": "Listener_Volume", "children": [[1, [[0, 1, 4], [1, 0, 4]]]] } ] }
  ],
  "attenuations": { "9": { "max_distance": 10, "volume_dry": { "scaling": 2, "points": [[0, 0, 9], [1, 0, 4], [10, -1, 4]] },
                          "volume_aux_game": { "scaling": 2, "points": [[0, -0.2920542, 4], [10, -1, 4]] } } },
  "buses": {
    "7": { "parent": 8, "aux": false, "voice_volume_db": -1, "bus_volume_db": 0, "rtpc_default_db": -2,
           "effects": [ { "slot": 0, "id": 1, "plugin": "0x008b0003", "kind": "gain", "gain_db": 1.5 } ] },
    "8": { "aux": false, "voice_volume_db": -0.5, "bus_volume_db": -3,
           "effects": [ { "slot": 1, "id": 3, "plugin": "0x006e0003", "kind": "peak_limiter",
                          "limiter": { "threshold_db": -0.1, "ratio": 40, "look_ahead_s": 0.01, "release_s": 0.05, "output_level_db": 0,
                                       "process_lfe": true, "channel_link": true } } ] },
    "20": { "name": "VERB_Synthetic", "parent": 8, "aux": true, "voice_volume_db": 0, "bus_volume_db": -10,
            "effects": [ { "slot": 0, "id": 2, "plugin": "0x007f0003", "kind": "convolution_reverb", "ir_media": 900,
                           "convolution": { "pre_delay_ms": 0, "wet_level_db": 0, "dry_level_db": -96 } } ] }
  },
  "media": { "500": { "file": "Audio/500.ogg", "codec": "vorbis", "channels": 2, "rate": 48000, "seconds": 4.0 },
             "501": { "file": "Audio/501.ogg", "codec": "vorbis", "channels": 2, "rate": 48000, "seconds": 3.0 } },
  "rtpcs": { "Listener_Volume": { "wwise_name": "Listener_Volume", "min": 0, "max": 1, "default": 1 } },
  "environments": { "VERB_Synthetic": { "aux_bus": 20, "record": "AudioEnvironment.Synthetic", "size": [6, 10], "interiority": [0.9, 1],
                    "impulse_response": { "media": 900, "file": "Audio/ir/900.f32", "channels": 2, "rate": 48000, "frames": 4800, "t20_s": 1.4, "t30_s": 2.0 } } },
  "rooms": { "ABC-Room": { "name": "Room", "volume_m3": 400, "size_m": 7.37, "interiority": 1, "environment": "VERB_Synthetic", "rule": "r" } },
  "entity_classes": { "CLASS-1": { "name": "Lift", "effects": [
      { "on": "trigger", "tag": "RampUp", "tag_path": "A/Elevators/RampUp", "trigger": "Play_Synthetic", "bone": "b", "offset": [3, -3, 2] },
      { "on": "tag", "tag": "InTransit", "tag_path": "A/Elevators/InTransit", "trigger": "Play_Synthetic" } ], "actions": [] } },
  "root_entity_class": "class-0"
})json");

FSCAudioManifest Parsed(FAutomationTestBase& Test)
{
    FSCAudioManifest M;
    FString Error;
    Test.TestTrue(TEXT("manifest parses"), FSCAudioManifest::Parse(Manifest, M, Error));
    Test.TestEqual(TEXT("parse error"), Error, FString());
    return M;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioManifestParseTest, "SCBridge.Audio.ManifestParse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioManifestParseTest::RunTest(const FString&)
{
    const FSCAudioManifest M = Parsed(*this);
    TestEqual(TEXT("nodes"), M.Nodes.Num(), 4);
    TestEqual(TEXT("blend layer"), M.Nodes[3].Layers.Num(), 1);
    TestEqual(TEXT("blend layer child"), M.Nodes[3].Layers[0].Children[0].Key, 1);
    TestTrue(TEXT("blend layer gain at 0.25"), FMath::IsNearlyEqual(M.Nodes[3].Layers[0].Children[0].Value.Eval(0.25f), 0.75f));
    TestTrue(TEXT("global container"), M.Nodes[3].bGlobal && !M.Nodes[0].bGlobal);
    TestTrue(TEXT("transition random range"), M.Nodes[3].TransitionRandomS == FVector2f(-0.5f, 0.5f));
    TestEqual(TEXT("root kind"), int32(M.Nodes[0].Kind), int32(ESCAudioNodeKind::Random));
    TestTrue(TEXT("continuous"), M.Nodes[0].bContinuous);
    TestEqual(TEXT("loop forever"), M.Nodes[0].LoopCount, 0);
    TestEqual(TEXT("children"), M.Nodes[0].Children, TArray<int32>{1, 2});
    const FSCAudioTrigger& T = M.Triggers[TEXT("Play_Synthetic")];
    TestTrue(TEXT("playable"), T.IsPlayable());
    TestEqual(TEXT("delay"), T.Play[0].DelayS, 0.25f);
    TestEqual(TEXT("context volume"), T.Play[0].Context.VolumeDb, -8.f);
    TestEqual(TEXT("stop action"), M.Triggers[TEXT("Stop_Synthetic")].Other[0].Target, 100u);
    TestEqual(TEXT("media seconds"), M.Media[500].Seconds, 4.f);
    TestTrue(TEXT("convolution bus"), M.Buses[20].bConvolution);
    TestEqual(TEXT("IR media"), M.Buses[20].IrMedia, 900u);
    TestTrue(TEXT("peak limiter"), M.Buses[8].Limiter.bOn && M.Buses[8].Limiter.bChannelLink);
    TestEqual(TEXT("limiter threshold"), M.Buses[8].Limiter.ThresholdDb, -0.1f);
    TestEqual(TEXT("limiter look-ahead"), M.Buses[8].Limiter.LookAheadS, 0.01f);
    TestFalse(TEXT("no limiter on a bus without one"), M.Buses[7].Limiter.bOn);
    TestEqual(TEXT("room key lower case"), M.RoomEnvironment(TEXT("abc-room")), FString(TEXT("VERB_Synthetic")));
    const FSCAudioClass& C = M.Classes[TEXT("class-1")];
    TestEqual(TEXT("trigger effects by tag"), C.EffectsFor(TEXT("RampUp"), TEXT("trigger")).Num(), 1);
    TestEqual(TEXT("no trigger effect for a state tag"), C.EffectsFor(TEXT("InTransit"), TEXT("trigger")).Num(), 0);
    TestEqual(TEXT("effect offset"), C.Effects[0].Offset, FVector(3, -3, 2));
    TestEqual(TEXT("emitters"), M.SpotCount, 3);
    FSCAudioManifest Bad;
    FString Error;
    TestFalse(TEXT("other versions are refused"), FSCAudioManifest::Parse(TEXT("{\"version\": 2}"), Bad, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioCurveTest, "SCBridge.Audio.Falloff", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioCurveTest::RunTest(const FString&)
{
    const FSCAudioManifest M = Parsed(*this);
    const FSCAudioAttenuation& A = M.Attenuations[9];
    // Flat to 1 m (constant), then linear in the stored (linear - 1) domain to silence at 10 m.
    TestEqual(TEXT("0 dB inside 1 m"), A.VolumeDry.EvalDb(0.5f), 0.f);
    TestTrue(TEXT("half way (5.5 m) is -6 dB"), FMath::IsNearlyEqual(A.VolumeDry.EvalDb(5.5f), -6.0206f, 1e-3f));
    TestEqual(TEXT("silent at the max distance"), A.VolumeDry.EvalDb(10.f), -200.f);
    TestEqual(TEXT("silent beyond"), A.VolumeDry.EvalDb(11.f), -200.f);
    TestTrue(TEXT("aux curve starts at -3 dB"), FMath::IsNearlyEqual(A.VolumeAuxGame.EvalDb(0.f), -3.f, 1e-3f));
    TestEqual(TEXT("max distance"), A.MaxDistance, 10.f);
    TestTrue(TEXT("unscaled curves are dB"), FSCAudioCurve::ToDb(0, -12.f) == -12.f);
    TestTrue(TEXT("exp3 shape"), FMath::IsNearlyEqual(FSCAudioCurve::ShapeAt(8, 0.5f), 0.125f));
    // Wwise's AkInterpolation forms (the exporter's mix::evaluate golden values).
    TestTrue(TEXT("exp1 (t + 1) t / 2"), FMath::IsNearlyEqual(FSCAudioCurve::ShapeAt(6, 0.25f), 0.15625f, 1e-5f));
    TestTrue(TEXT("log1 t (3 - t) / 2"), FMath::IsNearlyEqual(FSCAudioCurve::ShapeAt(2, 0.25f), 0.34375f, 1e-5f));
    TestTrue(TEXT("S-curve"), FMath::IsNearlyEqual(FSCAudioCurve::ShapeAt(5, 0.25f), 0.146447f, 1e-4f));
    TestTrue(TEXT("inverse S-curve"), FMath::IsNearlyEqual(FSCAudioCurve::ShapeAt(3, 0.75f), 0.646447f, 1e-4f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioContainerTest, "SCBridge.Audio.ContainerLogic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioContainerTest::RunTest(const FString&)
{
    FSCAudioNode Random;
    Random.Kind = ESCAudioNodeKind::Random;
    Random.Children = {10, 11, 12};
    Random.Weights = {50, 50, 50};
    Random.AvoidRepeat = 2;
    FSCAudioContainerState State;
    FRandomStream Stream(7);
    int32 Previous[2] = {INDEX_NONE, INDEX_NONE};
    TArray<int32> Counts{0, 0, 0};
    for (int32 I = 0; I < 300; ++I)
    {
        const int32 Pick = SCAudioPick(Random, State, Stream);
        TestTrue(TEXT("avoid-repeat 2 of 3: never one of the last two"), Pick != Previous[0] && Pick != Previous[1]);
        Previous[1] = Previous[0];
        Previous[0] = Pick;
        ++Counts[Pick];
    }
    TestTrue(TEXT("every child plays"), Counts[0] > 0 && Counts[1] > 0 && Counts[2] > 0);
    // Weights: a zero-weight child never plays (no avoid-repeat).
    FSCAudioNode Weighted = Random;
    Weighted.AvoidRepeat = 0;
    Weighted.Weights = {0, 100, 0};
    FSCAudioContainerState W;
    for (int32 I = 0; I < 20; ++I) TestEqual(TEXT("the only weighted child"), SCAudioPick(Weighted, W, Stream), 1);
    // Shuffle: each pass plays every child once.
    FSCAudioNode Shuffle = Random;
    Shuffle.AvoidRepeat = 0;
    Shuffle.bShuffle = true;
    FSCAudioContainerState S;
    TSet<int32> Pass;
    for (int32 I = 0; I < 3; ++I) Pass.Add(SCAudioPick(Shuffle, S, Stream));
    TestEqual(TEXT("a shuffle pass covers all"), Pass.Num(), 3);
    // Sequence: in order, wrapping.
    FSCAudioNode Sequence;
    Sequence.Kind = ESCAudioNodeKind::Sequence;
    Sequence.Children = {1, 2};
    FSCAudioContainerState Q;
    TestEqual(TEXT("first"), SCAudioPick(Sequence, Q, Stream), 0);
    TestEqual(TEXT("second"), SCAudioPick(Sequence, Q, Stream), 1);
    TestEqual(TEXT("wraps"), SCAudioPick(Sequence, Q, Stream), 0);
    // A single child is picked even when avoid-repeat asks for more.
    FSCAudioNode One = Random;
    One.Children = {5};
    One.Weights = {50};
    FSCAudioContainerState O;
    TestEqual(TEXT("single"), SCAudioPick(One, O, Stream), 0);
    TestEqual(TEXT("single again"), SCAudioPick(One, O, Stream), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioGainTest, "SCBridge.Audio.GainComposition", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioGainTest::RunTest(const FString&)
{
    const FSCAudioManifest M = Parsed(*this);
    const FSCAudioAction& A = M.Triggers[TEXT("Play_Synthetic")].Play[0];
    auto Rtpc = [&](const FString& Name) { const float* D = M.RtpcDefaults.Find(Name); return D ? *D : 0.f; };
    FSCAudioGain G;
    G.Add(A.Context, Rtpc, nullptr);           // -8 dB, Listener_Volume at its default 1 -> 0 dB
    G.Add(M.Nodes[0].Props, Rtpc, nullptr);    // +2 dB (random ranges not drawn here)
    G.Add(M.Nodes[1].Props, Rtpc, nullptr);    // -3 dB
    const TArray<const FSCAudioBus*> Chain = M.BusChain(M.Nodes[1].Bus);
    TestEqual(TEXT("bus chain"), Chain.Num(), 2);
    G.AddBusVoice(Chain);                      // -1 - 0.5 dB voice volumes
    TestTrue(TEXT("voice gain"), FMath::IsNearlyEqual(G.VolumeDb, -8.f + 2.f - 3.f - 1.f - 0.5f, 1e-4f));
    TestEqual(TEXT("aux send summed like volume"), G.GameAuxDb, -6.f);
    TestTrue(TEXT("bus 7 mix: bus volume + RTPC default + Gain effect"), FMath::IsNearlyEqual(Chain[0]->MixDb(), 0.f - 2.f + 1.5f));
    TestEqual(TEXT("bus 8 mix"), Chain[1]->MixDb(), -3.f);
    // With the listener-volume RTPC at 0 the dB-scaled curve silences the voice.
    FSCAudioGain Silent;
    Silent.Add(A.Context, [](const FString&) { return 0.f; }, nullptr);
    TestTrue(TEXT("silenced by the curve"), Silent.VolumeDb < -150.f);
    // Random draws stay within the authored range.
    FRandomStream Stream(3);
    for (int32 I = 0; I < 50; ++I)
    {
        FSCAudioGain R;
        R.Add(M.Nodes[0].Props, Rtpc, &Stream);
        TestTrue(TEXT("pitch draw in range"), R.PitchCents >= -100.f && R.PitchCents <= 100.f);
    }
    TestEqual(TEXT("linear of 0 dB"), FSCAudioGain::Linear(0.f), 1.f);
    TestEqual(TEXT("silence is zero"), FSCAudioGain::Linear(-200.f), 0.f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioReverbMappingTest, "SCBridge.Audio.ReverbMapping", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioReverbMappingTest::RunTest(const FString&)
{
    const FSCAudioManifest M = Parsed(*this);
    // Room -> environment -> its aux bus -> the Convolution Reverb slot and its impulse response.
    const FString Env = M.RoomEnvironment(TEXT("ABC-Room"));
    TestEqual(TEXT("room environment"), Env, FString(TEXT("VERB_Synthetic")));
    const FSCAudioEnvironment& E = M.Environments[Env];
    const FSCAudioBus& Aux = M.Buses[E.AuxBus];
    TestTrue(TEXT("aux bus runs a convolution"), Aux.bConvolution && Aux.bAux);
    TestEqual(TEXT("the slot's IR is the environment's"), Aux.IrMedia, E.IrMedia);
    TestEqual(TEXT("wet as authored"), Aux.WetDb, 0.f);
    TestEqual(TEXT("dry as authored (fully wet)"), Aux.DryDb, -96.f);
    TestEqual(TEXT("reverb return level"), Aux.MixDb(), -10.f);
    TestEqual(TEXT("its parent chain"), M.BusChain(E.AuxBus).Num(), 2);
    TestEqual(TEXT("IR frames"), E.IrFrames, 4800);
    TestEqual(TEXT("T30"), E.T30, 2.f);
    TestEqual(TEXT("an unknown room has none"), M.RoomEnvironment(TEXT("nowhere")), FString());
    return true;
}
