// WP-A1: unit tests of the audio rules (SCAudio.h) on synthetic manifests: container logic, falloff evaluation, manifest
// parsing, bus gain composition, the room -> environment -> convolution reverb mapping, and the bus effect chain (EQ at
// RTPC values, meters writing game parameters, side-chain ducking, the Peak Limiter ceiling). No game data.
#include "SCAudio.h"
#include "SCAudioSystem.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "Engine/World.h"
#include "SCAudioBusFx.h"
#include "SCAudioRayOcclusion.h"
#include "SCAudioStereoDirections.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCWwiseFilterScaleTest, "SCBridge.Audio.Filter.ScaleAndSum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCWwiseFilterScaleTest::RunTest(const FString&)
{
    FSCAudioFilterLevels Filter;
    Filter.Add(TEXT("lpf"), 15.f);
    Filter.Add(TEXT("lpf"), 30.f);
    TestEqual(TEXT("additive source parameters"), Filter.Lpf, 45.f);
    const auto Check = [this](const TCHAR* Name, float Actual, float Expected)
    {
        TestTrue(Name, FMath::Abs(Actual - Expected) <= Expected * 0.03f);
    };
    Check(TEXT("low-pass 0"), FSCAudioFilterLevels::LowPassHz(0.f), 20000.f);
    Check(TEXT("low-pass 30"), FSCAudioFilterLevels::LowPassHz(30.f), 7000.f);
    Check(TEXT("low-pass 45"), FSCAudioFilterLevels::LowPassHz(Filter.Lpf), 1922.f);
    Check(TEXT("low-pass 58"), FSCAudioFilterLevels::LowPassHz(58.f), 627.f);
    Check(TEXT("low-pass 100"), FSCAudioFilterLevels::LowPassHz(100.f), 17.f);
    Check(TEXT("high-pass 20"), FSCAudioFilterLevels::HighPassHz(20.f), 94.f);

    FSCAudioProps Context, Node;
    FSCAudioRtpcCurve& ContextCurve = Context.Rtpc.AddDefaulted_GetRef();
    ContextCurve.Param = TEXT("lpf");
    ContextCurve.Curve.Points = {{0.f, 50.f, 4.f}, {1.f, 0.f, 4.f}};
    Node.Rtpc.Add(ContextCurve);
    Node.Rtpc[0].Curve.Points[0].Y = -50.f;
    FSCAudioGain Gain;
    const auto AtVacuum = [](const FString&) { return 0.f; };
    Gain.Add(Context, AtVacuum, nullptr);
    TestEqual(TEXT("the context's positive curve closes the filter"), Gain.Filters.Lpf, 50.f);
    Gain.Add(Node, AtVacuum, nullptr);
    TestEqual(TEXT("the node's signed correction cancels the context before clamping"), Gain.Filters.Lpf, 0.f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCWwiseRampTest, "SCBridge.Audio.GameSync.Ramps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCWwiseRampTest::RunTest(const FString&)
{
    const FSCAudioRamp Filtering{2.f, 1.f, 2};
    TestTrue(TEXT("filter is within 0.5 percent of a rising target at the authored time"),
        FMath::IsNearlyEqual(Filtering.Advance(0.f, 1.f, 2.f), 0.995f, 1e-5f));
    TestTrue(TEXT("fall follows its own authored time"),
        FMath::IsNearlyEqual(Filtering.Advance(1.f, 0.f, 1.f), 0.005f, 1e-5f));
    const FSCAudioRamp Slew{4.f, 6.f, 1};
    TestEqual(TEXT("slew moves at the authored rate"), Slew.Advance(0.f, 2.f, 0.25f), 1.f);
    TestEqual(TEXT("falling slew stops at target"), Slew.Advance(1.f, 0.f, 1.f), 0.f);

    FSCRtpcBank Bank;
    const FSCLiveRtpcRef Value = Bank.Get(TEXT("synthetic"), 0.f);
    Bank.ConfigureRamp(TEXT("synthetic"), 0.f, Filtering);
    Bank.Set(TEXT("synthetic"), 1.f);
    TestEqual(TEXT("callback sees the old smoothed value before a tick"), Value->Read(), 0.f);
    Bank.Advance(2.f);
    TestTrue(TEXT("callback reads the advanced value"), FMath::IsNearlyEqual(Value->Read(), 0.995f, 1e-5f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCWwiseBusStateTest, "SCBridge.Audio.GameSync.BusSlotState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCWwiseBusStateTest::RunTest(const FString&)
{
    // Synthetic group and state ids. The gain slot becomes active only in the enabled state.
    const uint32 Group = 71, Bypassed = 81, Enabled = 83;
    FSCAudioBus Bus;
    FSCAudioBusEffect& Fx = Bus.Effects.AddDefaulted_GetRef();
    Fx.Kind = TEXT("gain");
    Fx.GainDb = -6.f;
    Fx.StateGroups.Add(Group);
    Fx.StateBypass.Add(Group, TArray<uint32>{Bypassed});

    FSCRtpcBank Bank;
    FSCBusDsp Dsp(101, Bus, {}, Bank, 0.f);
    TUniquePtr<FSCBusDspState> State = Dsp.MakeState(48000.f, 1);
    float Sample = 1.f;
    Dsp.Process(*State, &Sample, 1);
    TestEqual(TEXT("unknown state leaves the stateful slot bypassed"), Sample, 1.f);
    Bank.SetState(Group, Enabled);
    Sample = 1.f;
    Dsp.Process(*State, &Sample, 1);
    TestTrue(TEXT("enabled state applies the gain slot"), FMath::IsNearlyEqual(Sample, 0.501187f, 1e-5f));
    Bank.SetState(Group, Bypassed);
    Sample = 1.f;
    Dsp.Process(*State, &Sample, 1);
    TestEqual(TEXT("authored bypass state skips the slot"), Sample, 1.f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCWwiseStateTransitionTest, "SCBridge.Audio.GameSync.StateTransition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCWwiseStateTransitionTest::RunTest(const FString&)
{
    const FString Json = TEXT(R"({
        "version":1,
        "buses":{"101":{"state_values":[{"group":71,"state":11,"bus_volume_db":-12.0}]}},
        "state_groups":{"71":{"default_transition_ms":1000,"transitions":[{"from":11,"to":12,"duration_ms":200}]}}
    })");
    FSCAudioManifest M;
    FString Error;
    if (!TestTrue(TEXT("fixture parses"), FSCAudioManifest::Parse(Json, M, Error))) return false;
    if (!TestTrue(TEXT("authored group and bus are parsed"), M.StateTimings.Contains(71) && M.Buses.Contains(101))) return false;
    if (!TestEqual(TEXT("authored bus state is parsed"), M.Buses[101].States.Num(), 1)) return false;
    FSCRtpcBank Bank;
    Bank.ConfigureState(71, M.StateTimings[71]);
    FSCBusDsp Dsp(101, M.Buses[101], {}, Bank, 0.f);
    TestTrue(TEXT("state gain alone creates a submix effect"), Dsp.HasWork());
    const FSCAudioWeightedState State = Bank.BindState(M.Buses[101].States[0]);
    Bank.SetState(71, 11);
    Bank.Advance(0.5f);
    TestEqual(TEXT("default transition halfway"), State.Weight->load(), 0.5f);
    TUniquePtr<FSCBusDspState> Stream = Dsp.MakeState(48000.f, 1);
    float Sample = 1.f;
    Dsp.Process(*Stream, &Sample, 1);
    TestTrue(TEXT("the bus applies half of the authored -12 dB offset"), FMath::IsNearlyEqual(Sample, 0.501187f, 1.e-5f));
    Bank.SetState(71, 12);
    TestEqual(TEXT("interruption preserves the mix at the transition boundary"), State.Weight->load(), 0.5f);
    Bank.Advance(0.1f);
    TestEqual(TEXT("the authored pair takes precedence over the default duration"), State.Weight->load(), 0.25f);
    Bank.Advance(0.1f);
    TestEqual(TEXT("state without values fades previous offsets to zero"), State.Weight->load(), 0.f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCWwiseRoomPropagationTest, "SCBridge.Audio.Rooms.ConnectorPropagation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCWwiseRoomPropagationTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ASCVehicle* Vehicle = World->SpawnActor<ASCVehicle>();
    FSCAudioScene S;
    S.Vehicle = Vehicle;
    TArray<FString> Warnings;
    S.AddInterior(0, Vehicle->GetRootComponent(), false, {}, {}, Warnings);
    const FString Transform = TEXT(R"("transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]])");
    const FString Left = FString::Printf(TEXT(R"({"room":"a","kind":"box",%s,"box_min":[-2,-2,-2],"box_max":[-0.1,2,2]})"), *Transform);
    const FString Right = FString::Printf(TEXT(R"({"room":"b","kind":"box",%s,"box_min":[0.1,-2,-2],"box_max":[2,2,2]})"), *Transform);
    FSCLightAreas::Register(Vehicle, Vehicle->GetRootComponent(), {Left, Right}, false, Warnings);
    S.AddConnectors(0, {FString::Printf(TEXT(R"({"entity_id":"connector","door_entity_id":"Door","bounds":[1,2,2],"offset":[0,0,0],%s,"physical_occlusion_when_closed":0.75})"), *Transform )});
    S.BuildRoomGraph();
    const FSCAudioRoomKey A{0,TEXT("a")}, B{0,TEXT("b")};
    TestTrue(TEXT("opposite faces resolve two distinct room volumes"), S.Connectors.Num() == 1 && S.Connectors[0].A == A && S.Connectors[0].B == B);
    S.ListenerRoomKey = B;
    TestEqual(TEXT("same scoped room has no physical occlusion"), S.RoomOcclusion(B).Get(-1.f), 0.f);
    TestFalse(TEXT("reused GUID in another placement is distinct"), S.RoomOcclusion({1,TEXT("b")}).IsSet());
    if (S.Connectors.Num() == 1)
    {
        auto* Rig = NewObject<USCRigComponent>(Vehicle);
        FSCDoorInteraction Door;
        Door.Rig = Rig;
        Door.Fragment = TEXT("door");
        Vehicle->Doors.Add(Door);
        S.Connectors[0].DoorRig = Rig;
        TestEqual(TEXT("closed authored weight / 3"), S.RoomOcclusion(A).Get(-1.f), 0.25f);
        auto Second = S.Connectors[0];
        Second.ClosedOcclusion.Reset();
        S.Connectors.Add(Second);
        TestFalse(TEXT("unknown equally short path remains unknown"), S.RoomOcclusion(A).IsSet());
        S.Connectors.Pop();
        S.Connectors[0].ClosedOcclusion = 0.f;
        TestEqual(TEXT("authored zero remains zero"), S.RoomOcclusion(A).Get(-1.f), 0.f);
        S.Connectors[0].ClosedOcclusion.Reset();
        TestFalse(TEXT("missing authored weight remains unknown"), S.RoomOcclusion(A).IsSet());
    }
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCWwiseAreaOverlapTest, "SCBridge.Audio.Rooms.ExclusiveArea",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCWwiseAreaOverlapTest::RunTest(const FString&)
{
    FSCAudioAreaOverlap Overlap;
    Overlap.LowerDistance = 3.;
    Overlap.HigherDistance = 2.;
    Overlap.HigherFadeDistance = 8.;
    Overlap.bHigherHullInLower = true;
    TestEqual(TEXT("outside both, the closer overlapping area wins"), Overlap.ExclusiveFade().Get(1.f), 0.f);
    Overlap.bLowerInside = true;
    TestFalse(TEXT("inside lower only, the lower still owns the listener"), Overlap.ExclusiveFade().IsSet());
    Overlap.bLowerInside = false;
    Overlap.bHigherInside = true;
    Overlap.HullSeparation = 0.5;
    TestEqual(TEXT("higher only, different hulls do not play lower"), Overlap.ExclusiveFade().Get(1.f), 0.f);
    Overlap.HullSeparation = 0.0;
    TestEqual(TEXT("higher only, touching hulls can cross-fade"), Overlap.ExclusiveFade().Get(0.f), 0.75f);
    Overlap.bLowerInside = true;
    TestEqual(TEXT("inside both, depth into the higher area controls fade"), Overlap.ExclusiveFade().Get(0.f), 0.75f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCWwiseFilterBusTest, "SCBridge.Audio.Filter.OutputBusSpectrum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCWwiseFilterBusTest::RunTest(const FString&)
{
    constexpr int32 Rate = 48000;
    FSCAudioBus Bus;
    Bus.OutputFilters.Lpf = 58.f;
    FSCRtpcBank Bank;
    FSCBusDsp Dsp(19, Bus, {}, Bank, 0.f);
    TestTrue(TEXT("an output filter alone requires a submix effect"), Dsp.HasWork());
    TUniquePtr<FSCBusDspState> State = Dsp.MakeState(Rate, 2);
    TArray<float> Frame;
    Frame.SetNumZeroed(Rate * 2);
    for (int32 I = 0; I < Rate; ++I)
        Frame[I * 2] = FMath::Sin(2. * UE_PI * 627. * I / Rate);

    Dsp.Process(*State, Frame.GetData(), Rate);

    double DryEnergy = 0., WetEnergy = 0., SilentChannelEnergy = 0.;
    for (int32 I = Rate / 4; I < Rate; ++I)
    {
        const double Input = FMath::Sin(2. * UE_PI * 627. * I / Rate);
        DryEnergy += FMath::Square(Input);
        WetEnergy += FMath::Square(double(Frame[I * 2]));
        SilentChannelEnergy += FMath::Square(double(Frame[I * 2 + 1]));
    }
    const double Attenuation = 10. * FMath::LogX(10., WetEnergy / DryEnergy);
    TestTrue(TEXT("the authored cutoff is about minus three decibels"), FMath::Abs(Attenuation + 3.) <= 0.25);
    TestEqual(TEXT("independent filter history per channel"), SilentChannelEnergy, 0.);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioStereoDirectionsTest, "SCBridge.Audio.StereoDirections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioStereoDirectionsTest::RunTest(const FString&)
{
    FSCAudioStereoDirections MonoLeft, StereoPair;
    MonoLeft.Add(-1.f, 0.25f);
    StereoPair.Add(-1.f, 0.25f);
    StereoPair.Add(1.f, 0.25f);
    StereoPair.Add(-1.f, 0.25f);
    TestTrue(TEXT("the left channel matches one position and does not sum duplicate positions"),
        FMath::IsNearlyEqual(StereoPair.PerSpeaker.X, MonoLeft.PerSpeaker.X));
    TestEqual(TEXT("the right channel matches one position"), StereoPair.PerSpeaker.Y, 0.25f);
    MonoLeft.Add(1.f, 0.f);
    TestTrue(TEXT("an inaudible direction does not affect either channel"),
        MonoLeft.RelativeGains().Equals(FVector2f(1.f, 0.f), 1.e-6f));
    FSCAudioStereoDirections Silent;
    TestTrue(TEXT("no audible position gives a silent matrix"), Silent.RelativeGains().IsZero());

    float Pcm[] = {0.25f, 0.5f, 0.75f, 0.f, 0.f, 0.f};
    const FVector2f Equal = StereoPair.RelativeGains();
    FSCAudioStereoDirections::Render(Pcm, 3, 1, FVector2f::ZeroVector, Equal);
    TestTrue(TEXT("expands a single mono block backwards and smooths from silence"),
        FMath::IsNearlyEqual(Pcm[0], 0.25f / 3.f) && FMath::IsNearlyEqual(Pcm[1], 0.25f / 3.f) &&
        FMath::IsNearlyEqual(Pcm[2], 1.f / 3.f) && FMath::IsNearlyEqual(Pcm[3], 1.f / 3.f) &&
        Pcm[4] == 0.75f && Pcm[5] == 0.75f);
    float DistinctStereo[] = {0.25f, 0.75f};
    FSCAudioStereoDirections::Render(DistinctStereo, 1, 2, Equal, Equal);
    TestEqual(TEXT("preserves the source's left signal"), DistinctStereo[0], 0.25f);
    TestEqual(TEXT("preserves the source's right signal"), DistinctStereo[1], 0.75f);
    float LeftImpulse[] = {1.f, 0.f};
    const FVector2f HardRight(0.f, 1.f);
    FSCAudioStereoDirections::Render(LeftImpulse, 1, 2, HardRight, HardRight);
    TestEqual(TEXT("the left source reaches the right output at a hard pan"), LeftImpulse[1], 0.5f);
    TestEqual(TEXT("the opposite output is silent at a hard pan"), LeftImpulse[0], 0.f);
    float RightImpulse[] = {0.f, 1.f};
    const FVector2f HardLeft(1.f, 0.f);
    FSCAudioStereoDirections::Render(RightImpulse, 1, 2, HardLeft, HardLeft);
    TestEqual(TEXT("the right source reaches the left output at a hard pan"), RightImpulse[0], 0.5f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCAudioRayOcclusionTest, "SCBridge.Audio.RayOcclusion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCAudioRayOcclusionTest::RunTest(const FString&)
{
    const FSCAudioRayProfile Profile{0.3f, 500.f, 5.f};
    const float Wall[] = {0.4f, 0.4f, 0.4f, 0.4f, 0.4f};
    const FSCAudioRayLevels Covered = FSCAudioRayLevels::Reduce(Profile, 2.0, Wall);
    TestTrue(TEXT("equal rays behind a wall give its obstruction coefficient as occlusion"),
        FMath::IsNearlyEqual(Covered.Occlusion, 0.4f));
    TestEqual(TEXT("equal rays at close range give zero separate obstruction"), Covered.Obstruction, 0.f);
    const float Centre[] = {0.8f, 0.f, 0.f, 0.f, 0.f};
    TestTrue(TEXT("the central ray's obstruction falls off beyond the full distance"),
        FMath::IsNearlyEqual(FSCAudioRayLevels::Reduce(Profile, 10.0, Centre).Obstruction, 0.4f));
    TestEqual(TEXT("very near ray target is neutral"), FSCAudioRayLevels::Reduce(Profile, 0.25, Wall).Occlusion, 0.f);
    TestEqual(TEXT("out of range ray target is neutral"), FSCAudioRayLevels::Reduce(Profile, 501.0, Wall).Occlusion, 0.f);
    TestEqual(TEXT("absent configuration does not invent a distance profile"),
        FSCAudioRayLevels::Reduce({}, 2.0, Wall).Occlusion, 0.f);

    const FString Json = TEXT(R"({"version":1,"game_parameters":{"71":{"ramp_type":2,"ramp_up":0.3,"ramp_down":0.3,"builtin":6}},
        "surface_obstruction":{"mat_default":0.25,"mat_hollow":0.35}})");
    FSCAudioManifest Data;
    FString Error;
    TestTrue(TEXT("ray fixture manifest parses"), FSCAudioManifest::Parse(Json, Data, Error));
    TestTrue(TEXT("the STMG binding selects per-emitter occlusion"), Data.GameParameters.FindRef(71).IsRayParameter());
    TestEqual(TEXT("the fallback retains the archive coefficient"), Data.SurfaceObstruction.FindRef(FName(TEXT("mat_default"))), 0.25f);
    return true;
}
