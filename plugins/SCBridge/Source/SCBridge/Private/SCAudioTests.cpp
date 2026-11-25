// WP-A1: unit tests of the audio rules (SCAudio.h) on synthetic manifests: container logic, falloff evaluation, manifest
// parsing, bus gain composition, the room -> environment -> convolution reverb mapping, and the bus effect chain (EQ at
// RTPC values, meters writing game parameters, side-chain ducking, the Peak Limiter ceiling). No game data.
#include "SCAudio.h"
#include "SCAudioBusFx.h"
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
    "7": { "parent": 8, "aux": false, "voice_volume_db": -1, "bus_volume_db": 0,
           "rtpc": [ { "rtpc": "Listener_Volume", "rtpc_type": 0, "param": "bus_volume", "scaling": 0, "points": [[0, 0, 4], [1, -2, 4]] },
                     { "rtpc": "Listener_Volume", "rtpc_type": 0, "param": "volume", "scaling": 0, "points": [[0, 0, 4], [1, -4, 4]] },
                     { "rtpc": "Unset_Synthetic", "rtpc_type": 0, "param": "bus_volume", "scaling": 0, "points": [[0, -50, 4], [1, -50, 4]] } ],
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
    G.AddBusVoice(Chain, [](const FString&) { return false; }, M.RtpcDefaults); // -1 - 0.5 dB voice volumes, -4 dB voice curve
    TestTrue(TEXT("voice gain"), FMath::IsNearlyEqual(G.VolumeDb, -8.f + 2.f - 3.f - 1.f - 0.5f - 4.f, 1e-4f));
    TestEqual(TEXT("aux send summed like volume"), G.GameAuxDb, -6.f);
    // The output-stage curve at its RTPC default; a curve whose RTPC has no default is inactive; the Gain slot runs in
    // the bus's chain, not in its static level.
    TestTrue(TEXT("bus 7 mix: bus volume + output curve at its default"), FMath::IsNearlyEqual(Chain[0]->MixDb(), 0.f - 2.f));
    TestTrue(TEXT("bus 7 gain slot"), Chain[0]->Effects.Num() == 1 && Chain[0]->Effects[0].Kind == TEXT("gain") && Chain[0]->Effects[0].GainDb == 1.5f);
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

namespace
{
/** Bus effects as A1-2 exports them (synthetic values): a source bus with a Meter writing DM_Synthetic; a target bus
 *  ducked by DM_Synthetic (-10 dB at -30 dB and above, as a dB-scaled curve) with a Parametric EQ whose low pass the
 *  RTPC FX_Synthetic opens (log-scaled frequency, exclusive) and whose gain it raises (additive); a slot a state group
 *  bypasses; a master with a Peak Limiter. */
const TCHAR* BusFxManifest = TEXT(R"json({
  "version": 1, "triggers": {}, "nodes": [], "attenuations": {}, "media": {},
  "rtpcs": { "DM_Synthetic": { "wwise_name": "DM_Synthetic", "default": -60 }, "FX_Synthetic": { "wwise_name": "FX_Synthetic", "default": 1 } },
  "buses": {
    "1": { "parent": 3, "aux": false, "voice_volume_db": 0, "bus_volume_db": 0, "effects": [
      { "slot": 1, "id": 11, "plugin": "0x00810003", "kind": "meter", "meter": { "attack_s": 0.0, "release_s": 0.15, "min_db": -60, "max_db": 0, "hold_s": 0.16,
        "infinite_hold": false, "mode": "peak", "scope": "global", "apply_downstream_volume": false, "game_parameter": "DM_Synthetic", "game_parameter_id": 5 } } ] },
    "2": { "parent": 3, "aux": false, "voice_volume_db": 0, "bus_volume_db": 0,
      "rtpc": [ { "rtpc": "DM_Synthetic", "rtpc_type": 0, "param": "bus_volume", "scaling": 2, "points": [[-60, 0, 1], [-30, -0.6837722, 4]] } ],
      "effects": [
        { "slot": 0, "id": 21, "plugin": "0x00690003", "kind": "parametric_eq",
          "eq": { "bands": [ { "filter": "lowshelf", "gain_db": 0, "frequency_hz": 150, "q": 1, "on": true },
                             { "filter": "peaking", "gain_db": 0, "frequency_hz": 350, "q": 1, "on": false },
                             { "filter": "lowpass", "gain_db": 0, "frequency_hz": 400, "q": 1, "on": true } ], "output_level_db": 0 },
          "rtpc": [ { "rtpc": "FX_Synthetic", "rtpc_type": 0, "accum": 1, "param": "fx_param_12", "scaling": 3, "points": [[0, 3, 4], [1, 4.30103, 4]] },
                    { "rtpc": "FX_Synthetic", "rtpc_type": 0, "accum": 2, "param": "fx_param_1", "scaling": 2, "points": [[0, 0, 4], [1, 0.4125375, 4]] } ] },
        { "slot": 1, "id": 22, "plugin": "0x00690003", "kind": "parametric_eq", "state_bypass": [ { "group": 77, "states": [6] } ],
          "eq": { "bands": [ { "filter": "lowpass", "gain_db": 0, "frequency_hz": 400, "q": 1, "on": true } ], "output_level_db": 0 } } ] },
    "3": { "aux": false, "voice_volume_db": 0, "bus_volume_db": 0, "effects": [
      { "slot": 1, "id": 31, "plugin": "0x006e0003", "kind": "peak_limiter",
        "limiter": { "threshold_db": -6, "ratio": 40, "look_ahead_s": 0.01, "release_s": 0.05, "output_level_db": 0, "process_lfe": true, "channel_link": true } } ] }
  }
})json");
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioBusEffectsTest, "SCBridge.Audio.BusEffects", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioBusEffectsTest::RunTest(const FString&)
{
    FSCAudioManifest M;
    FString Error;
    TestTrue(TEXT("parses"), FSCAudioManifest::Parse(BusFxManifest, M, Error));
    const FSCAudioBus& Source = M.Buses[1];
    const FSCAudioBus& Target = M.Buses[2];
    TestEqual(TEXT("meter slot"), Source.Effects.Num(), 1);
    TestEqual(TEXT("meter writes"), Source.Effects[0].Meter.GameParameter, FString(TEXT("DM_Synthetic")));
    TestTrue(TEXT("meter ballistics"), Source.Effects[0].Meter.HoldS == 0.16f && Source.Effects[0].Meter.ReleaseS == 0.15f && !Source.Effects[0].Meter.bRms);
    TestEqual(TEXT("two EQ slots in slot order"), Target.Effects.Num(), 2);
    TestTrue(TEXT("the second depends on a game state"), Target.Effects[1].IsStateDependent() && !Target.Effects[0].IsStateDependent());
    // The RTPC at its default opens the low pass to 20 kHz (exclusive, log-scaled) and raises the shelf by 3 dB (additive).
    auto AtDefault = [&](const FString& N) { const float* D = M.RtpcDefaults.Find(N); return D ? *D : 0.f; };
    const FSCAudioBusEffect::FBand Lp = Target.Effects[0].BandAt(2, AtDefault);
    TestTrue(TEXT("low pass opened"), FMath::IsNearlyEqual(Lp.FrequencyHz, 20000.f, 1.f) && Lp.Type == 0 && Lp.bOn);
    const FSCAudioBusEffect::FBand Shelf = Target.Effects[0].BandAt(0, AtDefault);
    TestTrue(TEXT("shelf gain added"), FMath::IsNearlyEqual(Shelf.GainDb, 3.f, 0.01f) && Shelf.Type == 4);
    const FSCAudioBusEffect::FBand Closed = Target.Effects[0].BandAt(2, [](const FString&) { return 0.f; });
    TestTrue(TEXT("at 0 the low pass sits at 1 kHz"), FMath::IsNearlyEqual(Closed.FrequencyHz, 1000.f, 0.5f));
    // Side-chain: the ducking curve moves the bus level only away from the default.
    TestTrue(TEXT("no duck at the default"), FMath::IsNearlyZero(Target.OutputCurveDeltaDb(AtDefault, M.RtpcDefaults)));
    TestTrue(TEXT("-10 dB at -30 dB"), FMath::IsNearlyEqual(Target.OutputCurveDeltaDb([](const FString&) { return -30.f; }, M.RtpcDefaults), -10.f, 0.01f));

    // The chains on real buffers: the meter writes the shared game parameter, the target ducks by its curve, the
    // master's limiter holds threshold * (peak / threshold)^(1 / ratio).
    FSCRtpcBank Bank;
    FSCBusDsp SourceDsp(1, Source, M.RtpcDefaults, Bank, 0.f), TargetDsp(2, Target, M.RtpcDefaults, Bank, 0.f), Master(3, M.Buses[3], M.RtpcDefaults, Bank, 0.f);
    TestTrue(TEXT("the meter's parameter is driven"), Bank.Driven().Contains(TEXT("DM_Synthetic")));
    TUniquePtr<FSCBusDspState> SourceState = SourceDsp.MakeState(48000.f, 2), TargetState = TargetDsp.MakeState(48000.f, 2), MasterState = Master.MakeState(48000.f, 2);
    TArray<float> Loud;
    Loud.Init(0.5f, 960 * 2); // -6 dBFS, two 10 ms meter blocks
    SourceDsp.Process(*SourceState, Loud.GetData(), 960);
    TestTrue(TEXT("meter -> game parameter"), FMath::IsNearlyEqual(Bank.Find(TEXT("DM_Synthetic"))->Read(), -6.02f, 0.05f));
    TArray<float> Bed;
    for (int32 K = 0; K < 4; ++K)
    {
        Bed.Init(0.1f, 1024 * 2);
        TargetDsp.Process(*TargetState, Bed.GetData(), 1024); // the gain ramps to its target over the first buffer
    }
    // A constant bed: the low shelf adds its +3 dB at DC, the side-chain takes 10 dB.
    TestTrue(FString::Printf(TEXT("ducked by 10 dB (%.5f)"), Bed.Last()), FMath::IsNearlyEqual(Bed.Last(), 0.1f * FMath::Pow(10.f, (3.f - 10.f) / 20.f), 1e-3f));
    TArray<float> Hot;
    Hot.SetNumUninitialized(4800 * 2);
    for (int32 F = 0; F < 4800; ++F) Hot[2 * F] = Hot[2 * F + 1] = 4.f * FMath::Sin(F * 0.13f);
    float Peak = 0;
    for (int32 B = 0; B < 4800; B += 480)
    {
        Master.Process(*MasterState, Hot.GetData() + B * 2, 480);
        for (int32 I = 0; I < 480 * 2; ++I) Peak = FMath::Max(Peak, FMath::Abs(Hot[B * 2 + I]));
    }
    const float Thr = FMath::Pow(10.f, -6.f / 20.f);
    TestTrue(FString::Printf(TEXT("limiter ceiling (peak %.4f)"), Peak), Peak <= Thr * FMath::Pow(4.f / Thr, 1.f / 40.f) * 1.0001f);

    // A Gain slot before a meter changes what the meter measures (the chain runs in slot order).
    FSCAudioBus Gained = Source;
    FSCAudioBusEffect G;
    G.Kind = TEXT("gain");
    G.Slot = 0;
    G.GainDb = -12.f;
    Gained.Effects.Insert(G, 0);
    FSCRtpcBank Bank2;
    FSCBusDsp GainedDsp(1, Gained, M.RtpcDefaults, Bank2, 0.f);
    TUniquePtr<FSCBusDspState> GainedState = GainedDsp.MakeState(48000.f, 2);
    for (int32 K = 0; K < 3; ++K)
    {
        Loud.Init(0.5f, 960 * 2);
        GainedDsp.Process(*GainedState, Loud.GetData(), 960);
    }
    TestTrue(FString::Printf(TEXT("meter after a -12 dB gain (%.2f)"), Bank2.Find(TEXT("DM_Synthetic"))->Read()), FMath::IsNearlyEqual(Bank2.Find(TEXT("DM_Synthetic"))->Read(), -18.06f, 0.1f));
    // Two instances of one chain keep their own state: the second starts from silence, not from the first's history.
    TUniquePtr<FSCBusDspState> Second = Master.MakeState(48000.f, 2);
    TArray<float> Quiet;
    Quiet.Init(0.f, 480 * 2);
    Master.Process(*Second, Quiet.GetData(), 480);
    TestTrue(TEXT("independent delay lines"), Quiet.ContainsByPredicate([](float X) { return X != 0.f; }) == false);
    return true;
}

/** The submix effect a bus submix creates from its preset runs the preset's chain from its first buffer (a submix
 *  creates effect instances without preset data and without a preset change: the first run had no chain at all). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioBusFxInstanceTest, "SCBridge.Audio.BusFxInstance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioBusFxInstanceTest::RunTest(const FString&)
{
    FSCAudioManifest M;
    FString Error;
    TestTrue(TEXT("parses"), FSCAudioManifest::Parse(BusFxManifest, M, Error));
    FSCRtpcBank Bank;
    USCBusFxPreset* Preset = NewObject<USCBusFxPreset>();
    TSharedPtr<FSCBusDsp, ESPMode::ThreadSafe> Dsp = MakeShared<FSCBusDsp, ESPMode::ThreadSafe>(3, M.Buses[3], M.RtpcDefaults, Bank, 0.f);
    Preset->SetDsp(Dsp);
    FSoundEffectSubmixInitData Init; // as FMixerSubmix creates its chain: no preset data, the preset's unique id
    Init.SampleRate = 48000.f;
    Init.PresetSettings = nullptr;
    Init.ParentPresetUniqueId = Preset->GetUniqueID();
    TSharedPtr<FSoundEffectSubmix, ESPMode::ThreadSafe> Fx = USoundEffectPreset::CreateInstance<FSoundEffectSubmixInitData, FSoundEffectSubmix>(Init, *Preset);
    Fx->SetEnabled(true);
    Audio::FAlignedFloatBuffer In, Out;
    In.Init(0.f, 4800 * 2);
    for (int32 F = 0; F < 4800; ++F) In[2 * F] = In[2 * F + 1] = 4.f * FMath::Sin(F * 0.13f);
    Out.SetNumZeroed(In.Num());
    FSoundEffectSubmixInputData InData;
    InData.NumFrames = 4800;
    InData.NumChannels = 2;
    InData.AudioBuffer = &In;
    FSoundEffectSubmixOutputData OutData;
    OutData.AudioBuffer = &Out;
    OutData.NumChannels = 2;
    Fx->ProcessAudio(InData, OutData);
    float Peak = 0;
    for (float S : Out) Peak = FMath::Max(Peak, FMath::Abs(S));
    const float Thr = FMath::Pow(10.f, -6.f / 20.f);
    TestTrue(FString::Printf(TEXT("the instance limits (peak %.3f)"), Peak), Peak > 0.f && Peak <= Thr * FMath::Pow(4.f / Thr, 1.f / 40.f) * 1.0001f);
    USoundEffectPreset::UnregisterInstance(Fx);
    return true;
}

#include "SCAudioBusKernels.h"
/** WP-O1-4c-2: the bus limiter (FSCBusLimiter, the C++ of scbridge's sc_limiter) with the game's master parameters
 *  (threshold -0.1 dB, ratio 40, look-ahead 10 ms, release 50 ms), the cases its Rust tests covered: a +20 dB step and
 *  single-sample clicks over a loud bed at three buffer sizes never exceed the ceiling; a signal under the threshold passes
 *  as it is, delayed by the look-ahead less one; unlinked channels limit apart; an LFE left out passes at unity; invalid
 *  parameters pass the input. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioLimiterKernelTest, "SCBridge.Audio.LimiterKernel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioLimiterKernelTest::RunTest(const FString&)
{
    constexpr float Rate = 48000.f, ThrDb = -0.1f, Ratio = 40.f;
    const float Thr = FMath::Pow(10.f, ThrDb / 20.f);
    auto Ceiling = [&](float Peak) { return Peak <= Thr ? Peak : Thr * FMath::Pow(Peak / Thr, 1.f / Ratio); };
    auto Run = [&](TArray<float>& Signal, int32 Block, float InPeak, float& OverDb, float& OverShare) {
        FSCBusLimiter L(Rate, 2, ThrDb, Ratio, 0.010f, 0.050f, 0.f, true, INDEX_NONE);
        int32 Blocks = 0, Over = 0;
        float Peak = 0.f;
        for (int32 F = 0; F + Block <= Signal.Num() / 2; F += Block)
        {
            FSCBusLimiter::FStats S;
            L.Process(Signal.GetData() + F * 2, Block, S);
            float BlockPeak = 0.f;
            for (int32 I = 0; I < Block * 2; ++I) BlockPeak = FMath::Max(BlockPeak, FMath::Abs(Signal[F * 2 + I]));
            Peak = FMath::Max(Peak, BlockPeak);
            ++Blocks;
            Over += BlockPeak > Ceiling(InPeak) * 1.0001f;
        }
        OverDb = 20.f * FMath::LogX(10.f, FMath::Max(Peak, 1e-9f) / Ceiling(InPeak));
        OverShare = Blocks ? float(Over) / Blocks : 0.f;
    };
    for (const int32 Block : {64, 480, 1024})
    {
        // A +20 dB step: silence, then a 1 kHz tone at ten times the threshold.
        TArray<float> Step;
        Step.SetNumZeroed(48000 * 2);
        for (int32 F = 24000; F < 48000; ++F) Step[F * 2] = Step[F * 2 + 1] = 10.f * Thr * FMath::Sin(2 * PI * 1000.f * F / Rate);
        float OverDb, Share;
        Run(Step, Block, 10.f * Thr, OverDb, Share);
        AddInfo(FString::Printf(TEXT("step +20 dB, %d-frame blocks: %.3f dB from the ceiling, %.0f %% of blocks over"), Block, OverDb, Share * 100.f));
        TestTrue(FString::Printf(TEXT("step: never over the ceiling (%d)"), Block), Share == 0.f);
        // Clicks: one sample at +12 dB every 1000 frames over a bed at -10 dBFS.
        TArray<float> Clicks;
        Clicks.SetNumUninitialized(48000 * 2);
        for (int32 F = 0; F < 48000; ++F) Clicks[F * 2] = Clicks[F * 2 + 1] = (F % 1000 == 500 ? 4.f : 0.316f * FMath::Sin(2 * PI * 200.f * F / Rate));
        Run(Clicks, Block, 4.f, OverDb, Share);
        AddInfo(FString::Printf(TEXT("clicks +12 dB, %d-frame blocks: %.3f dB from the ceiling, %.0f %% of blocks over"), Block, OverDb, Share * 100.f));
        TestTrue(FString::Printf(TEXT("clicks: never over the ceiling (%d)"), Block), Share == 0.f);
    }
    // Under the threshold the signal passes unchanged, delayed by the look-ahead less one (479 frames).
    TArray<float> Quiet;
    Quiet.SetNumUninitialized(4800 * 2);
    for (int32 F = 0; F < 4800; ++F) Quiet[F * 2] = Quiet[F * 2 + 1] = 0.5f * FMath::Sin(2 * PI * 300.f * F / Rate);
    const TArray<float> Original = Quiet;
    FSCBusLimiter L(Rate, 2, ThrDb, Ratio, 0.010f, 0.050f, 0.f, true, INDEX_NONE);
    TestEqual(TEXT("latency"), L.Latency(), 479);
    FSCBusLimiter::FStats S;
    L.Process(Quiet.GetData(), 4800, S);
    float MaxErr = 0.f;
    for (int32 F = 479; F < 4800; ++F) MaxErr = FMath::Max(MaxErr, FMath::Abs(Quiet[F * 2] - Original[(F - 479) * 2]));
    TestTrue(FString::Printf(TEXT("under the threshold: unchanged, delayed (max error %.6f)"), MaxErr), MaxErr < 1e-6f);
    TestEqual(TEXT("under the threshold: no gain reduction"), S.GainMinDb, 0.f);
    // Channel link: a loud left and a quiet right channel. Linked, the right is reduced with the left; unlinked, not.
    for (const bool bLink : {true, false})
    {
        TArray<float> Two;
        Two.SetNumUninitialized(9600 * 2);
        for (int32 F = 0; F < 9600; ++F)
        {
            Two[F * 2] = 4.f * FMath::Sin(2 * PI * 500.f * F / Rate);
            Two[F * 2 + 1] = 0.1f * FMath::Sin(2 * PI * 700.f * F / Rate);
        }
        FSCBusLimiter Lk(Rate, 2, ThrDb, Ratio, 0.010f, 0.050f, 0.f, bLink, INDEX_NONE);
        Lk.Process(Two.GetData(), 9600, S);
        float Right = 0.f;
        for (int32 F = 4800; F < 9600; ++F) Right = FMath::Max(Right, FMath::Abs(Two[F * 2 + 1]));
        if (bLink) TestTrue(FString::Printf(TEXT("linked: the quiet channel follows the loud one's gain (%.4f)"), Right), Right < 0.05f);
        else TestTrue(FString::Printf(TEXT("unlinked: the quiet channel keeps its level (%.4f)"), Right), FMath::IsNearlyEqual(Right, 0.1f, 1e-3f));
    }
    // 5.1 with a loud LFE (index 3) left out: the other channels keep their level, the LFE passes at unity (delayed).
    TArray<float> Six;
    Six.SetNumUninitialized(4800 * 6);
    for (int32 F = 0; F < 4800; ++F)
        for (int32 C = 0; C < 6; ++C) Six[F * 6 + C] = (C == 3 ? 4.f : 0.1f) * FMath::Sin(2 * PI * 100.f * F / Rate);
    const TArray<float> SixIn = Six;
    FSCBusLimiter L6(Rate, 6, ThrDb, Ratio, 0.010f, 0.050f, 0.f, true, 3);
    L6.Process(Six.GetData(), 4800, S);
    float LfeErr = 0.f, FrontErr = 0.f;
    for (int32 F = 479; F < 4800; ++F)
    {
        LfeErr = FMath::Max(LfeErr, FMath::Abs(Six[F * 6 + 3] - SixIn[(F - 479) * 6 + 3]));
        FrontErr = FMath::Max(FrontErr, FMath::Abs(Six[F * 6] - SixIn[(F - 479) * 6]));
    }
    TestTrue(FString::Printf(TEXT("an LFE left out passes at unity (error %.6f)"), LfeErr), LfeErr < 1e-6f);
    TestTrue(FString::Printf(TEXT("... and does not reduce the others (error %.6f)"), FrontErr), FrontErr < 1e-6f);
    // Invalid parameters (ratio below 1): the input passes.
    TArray<float> Loud;
    Loud.Init(2.f, 480 * 2);
    FSCBusLimiter Bad(Rate, 2, ThrDb, 0.5f, 0.010f, 0.050f, 0.f, true, INDEX_NONE);
    Bad.Process(Loud.GetData(), 480, S);
    TestTrue(TEXT("invalid parameters pass the input"), Loud[0] == 2.f && Loud.Last() == 2.f);
    return true;
}

/** WP-O1-4c-2: the bus meter (FSCBusMeter) measures as sc_meter did: 10 ms blocks by integer division of the rate (220
 *  frames at 22050 Hz), carried across buffers of any size; an attack of 1 s rises 1 - exp(-1) of a step in 1 s. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioMeterKernelTest, "SCBridge.Audio.MeterKernel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioMeterKernelTest::RunTest(const FString&)
{
    {
        FSCBusMeter M(22050.f, 2, 0.f, 0.f, 0.f, -60.f, 0.f, false, false);
        TArray<float> Half;
        Half.Init(0.5f, 300 * 2);
        M.Process(Half.GetData(), 100);
        TestEqual(TEXT("219 frames: no block yet"), M.Process(Half.GetData(), 119), -60.f);
        TestTrue(TEXT("the 220th frame closes the block"), FMath::IsNearlyEqual(M.Process(Half.GetData(), 1), -6.02f, 0.01f));
    }
    {
        FSCBusMeter M(48000.f, 1, 1.f, 1.f, 0.f, -60.f, 0.f, false, false);
        TArray<float> Full;
        Full.Init(1.f, 480);
        float V = 0.f;
        for (int32 B = 0; B < 100; ++B) V = M.Process(Full.GetData(), 480); // 1 s of blocks at 0 dBFS
        const float Expected = -60.f + 60.f * (1.f - FMath::Exp(-1.f));
        TestTrue(FString::Printf(TEXT("1 s attack after 1 s: %.3f (expected %.3f)"), V, Expected), FMath::IsNearlyEqual(V, Expected, 0.01f));
    }
    return true;
}

// A door class's events and the effect-core sounds of their groups (audio.json `door`, WP-A1 door effects).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioDoorEffectsTest, "SCBridge.Audio.DoorEffects", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioDoorEffectsTest::RunTest(const FString&)
{
    FSCAudioManifest M;
    FString Error;
    const TCHAR* Json = TEXT(R"json({ "version": 1, "triggers": {}, "nodes": [], "attenuations": {}, "buses": {}, "media": {},
  "entity_classes": { "DOOR-1": { "name": "Gate", "actions": [],
    "effects": [ { "on": "trigger", "tag": "Open", "tag_path": "Door/Open", "trigger": "Play_Synthetic_DoorTag" },
                 { "on": "trigger", "tag": "Open", "tag_path": "Latch/Open", "trigger": "Play_Synthetic_LatchTag" } ],
    "door": { "events": { "open": { "group": "OpenGroup", "tag": "Open", "tag_path": "Door/Open" },
                          "opening": { "group": "OpeningGroup" }, "opened": { "tag": "Opened" } },
              "sounds": [ { "group": "OpenGroup", "trigger": "Play_Synthetic_Open", "helper": "$slot_sensor", "offset": [0, 1, 2], "looped": false },
                          { "group": "OpeningGroup", "trigger": "Play_Synthetic_Loop", "looped": true },
                          { "group": "UnusedGroup", "trigger": "Play_Synthetic_Other", "looped": false } ] } } } })json");
    if (!TestTrue(TEXT("parses: ") + Error, FSCAudioManifest::Parse(Json, M, Error))) return false;
    const FSCAudioClass* C = M.Classes.Find(TEXT("door-1"));
    if (!TestNotNull(TEXT("class"), C)) return false;
    TestEqual(TEXT("events"), C->DoorEvents.Num(), 3);
    TestEqual(TEXT("the open event's tag"), C->DoorEvents[TEXT("open")].Tag, FString(TEXT("Open")));
    const TArray<const FSCAudioDoorSound*> Open = C->DoorSoundsFor(TEXT("open"));
    if (TestEqual(TEXT("open plays its group's sound"), Open.Num(), 1))
    {
        TestEqual(TEXT("trigger"), Open[0]->Trigger, FString(TEXT("Play_Synthetic_Open")));
        TestEqual(TEXT("helper"), Open[0]->Helper, FString(TEXT("$slot_sensor")));
        TestEqual(TEXT("offset"), Open[0]->Offset, FVector(0, 1, 2));
        TestFalse(TEXT("one-shot"), Open[0]->bLooped);
    }
    const TArray<const FSCAudioDoorSound*> Opening = C->DoorSoundsFor(TEXT("opening"));
    TestTrue(TEXT("opening holds its loop"), Opening.Num() == 1 && Opening[0]->bLooped);
    TestEqual(TEXT("an event with a tag and no group plays no group sound"), C->DoorSoundsFor(TEXT("opened")).Num(), 0);
    TestEqual(TEXT("an event the door does not have plays nothing"), C->DoorSoundsFor(TEXT("locked")).Num(), 0);
    // The event's tag by its full path: the latch's tag of the same leaf name is not the door's.
    TestEqual(TEXT("the open event's tag path"), C->DoorEvents[TEXT("open")].TagPath, FString(TEXT("Door/Open")));
    const TArray<const FSCAudioEffect*> Fired = C->EffectsForDoorEvent(C->DoorEvents[TEXT("open")], TEXT("trigger"));
    TestTrue(TEXT("only the door's own tag effect"), Fired.Num() == 1 && Fired[0]->Trigger == TEXT("Play_Synthetic_DoorTag"));
    return true;
}
