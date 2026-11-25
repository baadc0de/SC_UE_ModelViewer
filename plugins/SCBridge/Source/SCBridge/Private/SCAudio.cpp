// WP-A1: audio manifest parsing and the playback rules of SCAudio.h (curves, container selection, gain composition).
#include "SCAudio.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
using FObj = TSharedPtr<FJsonObject>;
FString Str(const FObj& O, const TCHAR* K)
{
    FString V;
    if (O) O->TryGetStringField(K, V);
    return V;
}
double Num(const FObj& O, const TCHAR* K, double Default = 0)
{
    double V = Default;
    if (O) O->TryGetNumberField(K, V);
    return V;
}
bool Bool(const FObj& O, const TCHAR* K)
{
    bool V = false;
    if (O) O->TryGetBoolField(K, V);
    return V;
}
const TArray<TSharedPtr<FJsonValue>>& Arr(const FObj& O, const TCHAR* K)
{
    static const TArray<TSharedPtr<FJsonValue>> None;
    const TArray<TSharedPtr<FJsonValue>>* A;
    return O && O->TryGetArrayField(K, A) ? *A : None;
}
FObj Obj(const FObj& O, const TCHAR* K)
{
    const FObj* V;
    return O && O->TryGetObjectField(K, V) ? *V : nullptr;
}
uint32 Id(const FString& S) { return uint32(FCString::Strtoui64(*S, nullptr, 10)); }
uint32 IdField(const FObj& O, const TCHAR* K)
{
    double V = 0;
    return O && O->TryGetNumberField(K, V) ? uint32(V) : 0;
}
FSCAudioCurve Curve(const FObj& O)
{
    FSCAudioCurve C;
    if (!O) return C;
    C.Scaling = uint8(Num(O, TEXT("scaling")));
    for (const auto& P : Arr(O, TEXT("points")))
    {
        const auto& A = P->AsArray();
        if (A.Num() >= 3) C.Points.Add(FVector3f(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber()));
    }
    return C;
}
FSCAudioRtpcCurve RtpcCurve(const FObj& CO)
{
    FSCAudioRtpcCurve R;
    R.Rtpc = Str(CO, TEXT("rtpc"));
    R.Type = uint8(Num(CO, TEXT("rtpc_type")));
    R.Param = Str(CO, TEXT("param"));
    R.Accum = uint8(Num(CO, TEXT("accum")));
    if (R.Param.StartsWith(TEXT("fx_param_"))) R.FxParam = FCString::Atoi(*R.Param.RightChop(9));
    R.Curve = Curve(CO);
    return R;
}
} // namespace

bool FSCAudioBus::IsVoiceGain(const FString& P) { return P == TEXT("volume"); }
bool FSCAudioBus::IsOutputGain(const FString& P) { return P == TEXT("bus_volume") || P == TEXT("output_bus_volume") || P == TEXT("make_up_gain"); }

float FSCAudioRtpcCurve::ValueAt(float X) const
{
    const float Y = Curve.Eval(X);
    switch (Curve.Scaling)
    {
    case 2: return FSCAudioCurve::ToDb(2, Y);
    case 3: return FMath::Pow(10.f, Y);
    case 4: return FMath::Pow(10.f, Y / 20.f);
    default: return Y;
    }
}

float FSCAudioRtpcCurve::Apply(float Base, float X) const
{
    const float V = ValueAt(X);
    switch (Accum)
    {
    case 2: return Base + V;
    case 3: return Base * V;
    case 4: return V >= 0.5f ? 1.f : 0.f;
    case 5: return FMath::Max(Base, V);
    default: return V; // none / exclusive: the curve replaces the value
    }
}

FSCAudioBusEffect::FBand FSCAudioBusEffect::BandAt(int32 B, const TFunctionRef<float(const FString&)>& Value) const
{
    FBand Out = Bands[FMath::Clamp(B, 0, 2)];
    for (const FSCAudioRtpcCurve& C : Rtpc)
    {
        if (C.Type != 0 || !C.IsSupported() || C.FxParam < 5 * B || C.FxParam >= 5 * B + 5) continue;
        const float X = Value(C.Rtpc);
        switch (C.FxParam - 5 * B)
        {
        case 1: Out.GainDb = C.Apply(Out.GainDb, X); break;
        case 2: Out.FrequencyHz = C.Apply(Out.FrequencyHz, X); break;
        case 3: Out.Q = C.Apply(Out.Q, X); break;
        case 4: Out.bOn = C.Apply(Out.bOn ? 1.f : 0.f, X) >= 0.5f; break;
        default: break; // the filter type is not driven
        }
    }
    return Out;
}

float FSCAudioBusEffect::OutputAt(const TFunctionRef<float(const FString&)>& Value) const
{
    float Db = OutputDb;
    for (const FSCAudioRtpcCurve& C : Rtpc)
        if (C.Type == 0 && C.IsSupported() && C.FxParam == 15) Db = C.Apply(Db, Value(C.Rtpc));
    return Db;
}

float FSCAudioBusEffect::GainAt(const TFunctionRef<float(const FString&)>& Value) const
{
    float Db = GainDb;
    for (const FSCAudioRtpcCurve& C : Rtpc)
        if (C.Type == 0 && C.IsSupported() && C.FxParam == 0) Db = C.Apply(Db, Value(C.Rtpc));
    return Db;
}

float FSCAudioBus::OutputCurveDeltaDb(const TFunctionRef<float(const FString&)>& Value, const TMap<FString, float>& Defaults) const
{
    float Db = 0.f;
    for (const FSCAudioRtpcCurve& C : Rtpc)
        if (const float* D = Defaults.Find(C.Rtpc); D && C.Type == 0 && IsOutputGain(C.Param))
            Db += C.Curve.EvalDb(Value(C.Rtpc)) - C.Curve.EvalDb(*D);
    return Db;
}

float FSCAudioCurve::ShapeAt(int32 Interp, float T)
{
    switch (Interp)
    {
    // Wwise's AkInterpolation shapes in closed form (the exporter's mix::evaluate; wwiser's renderer as reference).
    case 9: return 0.f;                                                    // constant: hold the left value
    case 0: return 1.f - FMath::Pow(1.f - T, 3.f);                         // log3
    case 1: return FMath::Sin(T * HALF_PI);                                // sine
    case 2: return T * (3.f - T) * 0.5f;                                   // log1
    case 3: return T > 0.5f ? 1.f - FMath::Sin(T * PI) * 0.5f : FMath::Sin(T * PI) * 0.5f; // inverse S-curve
    case 5: return (1.f - FMath::Cos(T * PI)) * 0.5f;                      // S-curve
    case 6: return (T + 1.f) * T * 0.5f;                                   // exp1
    case 7: return 1.f - FMath::Cos(T * HALF_PI);                          // sine reciprocal
    case 8: return T * T * T;                                              // exp3
    default: return T;                                                     // linear (4)
    }
}

float FSCAudioCurve::Eval(float X) const
{
    if (Points.IsEmpty()) return 0;
    if (X <= Points[0].X) return Points[0].Y;
    if (X >= Points.Last().X) return Points.Last().Y;
    for (int32 I = 0; I + 1 < Points.Num(); ++I)
    {
        const FVector3f& A = Points[I];
        const FVector3f& B = Points[I + 1];
        if (X >= A.X && X < B.X)
        {
            const float T = B.X > A.X ? (X - A.X) / (B.X - A.X) : 0.f;
            return A.Y + (B.Y - A.Y) * ShapeAt(int32(A.Z), T);
        }
    }
    return Points.Last().Y;
}

float FSCAudioCurve::ToDb(uint8 Scaling, float Y)
{
    if (Scaling != 2) return Y;
    const float Linear = 1.f + Y;
    return Linear <= 1e-10f ? -200.f : 20.f * FMath::LogX(10.f, Linear);
}

void FSCAudioProps::Parse(const FObj& O)
{
    VolumeDb = Num(O, TEXT("volume_db"));
    PitchCents = Num(O, TEXT("pitch_cents"));
    Lpf = Num(O, TEXT("lpf"));
    Hpf = Num(O, TEXT("hpf"));
    MakeUpDb = Num(O, TEXT("make_up_gain_db"));
    GameAuxDb = Num(O, TEXT("game_aux_send_db"));
    OutputBusDb = Num(O, TEXT("output_bus_volume_db"));
    InitialDelayS = Num(O, TEXT("initial_delay_s"));
    for (const auto& R : Arr(O, TEXT("random")))
    {
        const auto& A = R->AsArray();
        if (A.Num() >= 3) Random.Add({A[0]->AsString(), float(A[1]->AsNumber()), float(A[2]->AsNumber())});
    }
    for (const auto& C : Arr(O, TEXT("rtpc"))) Rtpc.Add(RtpcCurve(C->AsObject()));
}

TArray<const FSCAudioEffect*> FSCAudioClass::EffectsFor(const FString& Tag, const FString& On) const
{
    TArray<const FSCAudioEffect*> Out;
    for (const FSCAudioEffect& E : Effects)
        if (E.Tag == Tag && (On.IsEmpty() || E.On == On)) Out.Add(&E);
    return Out;
}
TArray<const FSCAudioDoorSound*> FSCAudioClass::DoorSoundsFor(const FString& Event) const
{
    TArray<const FSCAudioDoorSound*> Out;
    const FSCAudioDoorEvent* E = DoorEvents.Find(Event);
    if (!E || E->Group.IsEmpty()) return Out;
    for (const FSCAudioDoorSound& S : DoorSounds)
        if (S.Group == E->Group) Out.Add(&S);
    return Out;
}

bool FSCAudioManifest::Parse(const FString& Json, FSCAudioManifest& Out, FString& Error)
{
    FObj Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root)
    {
        Error = TEXT("audio.json is not a JSON object");
        return false;
    }
    if (Num(Root, TEXT("version")) != 1)
    {
        Error = FString::Printf(TEXT("audio.json version %g (this viewer reads 1)"), Num(Root, TEXT("version")));
        return false;
    }
    Out = FSCAudioManifest();
    for (const auto& V : Arr(Root, TEXT("nodes")))
    {
        const FObj O = V->AsObject();
        FSCAudioNode N;
        N.Id = IdField(O, TEXT("id"));
        const FString Kind = Str(O, TEXT("kind"));
        N.Kind = Kind == TEXT("random")     ? ESCAudioNodeKind::Random
               : Kind == TEXT("sequence") ? ESCAudioNodeKind::Sequence
               : Kind == TEXT("blend")    ? ESCAudioNodeKind::Blend
               : Kind == TEXT("switch")   ? ESCAudioNodeKind::Switch
               : Kind == TEXT("mixer")    ? ESCAudioNodeKind::Mixer
                                          : ESCAudioNodeKind::Sound;
        for (const auto& C : Arr(O, TEXT("children"))) N.Children.Add(int32(C->AsNumber()));
        N.Props.Parse(O);
        N.Bus = IdField(O, TEXT("bus"));
        N.Attenuation = IdField(O, TEXT("attenuation"));
        N.bSpatial = Bool(O, TEXT("spatial"));
        N.bUseGameAux = Bool(O, TEXT("use_game_aux"));
        N.Media = IdField(O, TEXT("media"));
        N.SourcePlugin = Str(O, TEXT("source_plugin"));
        N.LoopCount = int32(Num(O, TEXT("loop_count"), 1));
        for (const auto& W : Arr(O, TEXT("weights"))) N.Weights.Add(W->AsNumber());
        N.AvoidRepeat = int32(Num(O, TEXT("avoid_repeat")));
        N.bShuffle = Bool(O, TEXT("shuffle"));
        N.bContinuous = Bool(O, TEXT("continuous"));
        N.Transition = Str(O, TEXT("transition"));
        N.TransitionS = Num(O, TEXT("transition_s"));
        if (const TArray<TSharedPtr<FJsonValue>> R = Arr(O, TEXT("transition_random_s")); R.Num() == 2)
            N.TransitionRandomS = FVector2f(R[0]->AsNumber(), R[1]->AsNumber());
        N.bGlobal = Bool(O, TEXT("global"));
        for (const auto& L : Arr(O, TEXT("layers")))
        {
            const FObj LO = L->AsObject();
            FSCAudioNode::FLayer Layer;
            Layer.Rtpc = Str(LO, TEXT("rtpc"));
            for (const auto& A : Arr(LO, TEXT("children")))
            {
                const auto& Pair = A->AsArray();
                if (Pair.Num() < 2) continue;
                FSCAudioCurve Gain; // linear gain 0..1 over the RTPC value
                for (const auto& P : Pair[1]->AsArray())
                {
                    const auto& Pt = P->AsArray();
                    if (Pt.Num() >= 3) Gain.Points.Add(FVector3f(Pt[0]->AsNumber(), Pt[1]->AsNumber(), Pt[2]->AsNumber()));
                }
                Layer.Children.Add({int32(Pair[0]->AsNumber()), MoveTemp(Gain)});
            }
            N.Layers.Add(MoveTemp(Layer));
        }
        Out.Nodes.Add(MoveTemp(N));
    }
    const FObj TriggerMap = Obj(Root, TEXT("triggers"));
    if (TriggerMap)
    for (const auto& [Key, V] : TriggerMap->Values)
    {
        const FString Name(*Key);
        const FObj O = V->AsObject();
        FSCAudioTrigger T;
        T.Status = Str(O, TEXT("status"));
        T.Reason = Str(O, TEXT("reason"));
        T.DurationType = Str(O, TEXT("duration_type"));
        T.RadiusMax = Num(O, TEXT("radius_max"));
        for (const auto& P : Arr(O, TEXT("play")))
        {
            const FObj PO = P->AsObject();
            FSCAudioAction A;
            A.Root = int32(Num(PO, TEXT("root"), -1));
            A.DelayS = Num(PO, TEXT("delay_s"));
            A.FadeInS = Num(PO, TEXT("fade_in_s"));
            A.Context.Parse(Obj(PO, TEXT("context")));
            if (Out.Nodes.IsValidIndex(A.Root)) T.Play.Add(MoveTemp(A));
        }
        for (const auto& P : Arr(O, TEXT("other")))
        {
            const FObj PO = P->AsObject();
            T.Other.Add({int32(Num(PO, TEXT("action_type"))), IdField(PO, TEXT("target")), float(Num(PO, TEXT("delay_s"))), float(Num(PO, TEXT("fade_s")))});
        }
        Out.Triggers.Add(Name, MoveTemp(T));
    }
    if (const FObj A = Obj(Root, TEXT("attenuations")))
        for (const auto& [Key, V] : A->Values)
        {
            const FString K(*Key);
            const FObj O = V->AsObject();
            FSCAudioAttenuation At;
            At.MaxDistance = Num(O, TEXT("max_distance"));
            At.VolumeDry = Curve(Obj(O, TEXT("volume_dry")));
            At.VolumeAuxGame = Curve(Obj(O, TEXT("volume_aux_game")));
            Out.Attenuations.Add(Id(K), MoveTemp(At));
        }
    if (const FObj B = Obj(Root, TEXT("buses")))
        for (const auto& [Key, V] : B->Values)
        {
            const FString K(*Key);
            const FObj O = V->AsObject();
            FSCAudioBus Bus;
            Bus.Id = Id(K);
            Bus.Parent = IdField(O, TEXT("parent"));
            Bus.Name = Str(O, TEXT("name"));
            Bus.bAux = Bool(O, TEXT("aux"));
            Bus.VoiceDb = Num(O, TEXT("voice_volume_db"));
            Bus.BusDb = Num(O, TEXT("bus_volume_db"));
            Bus.MakeUpDb = Num(O, TEXT("make_up_gain_db"));
            Bus.PitchCents = Num(O, TEXT("pitch_cents"));
            for (const auto& C : Arr(O, TEXT("rtpc"))) Bus.Rtpc.Add(RtpcCurve(C->AsObject()));
            for (const auto& E : Arr(O, TEXT("effects")))
            {
                const FObj EO = E->AsObject();
                if (Bool(EO, TEXT("bypass"))) continue; // a bypassed slot is not applied
                FSCAudioBusEffect Fx;
                Fx.Kind = Str(EO, TEXT("kind"));
                Fx.Slot = int32(Num(EO, TEXT("slot")));
                Fx.Id = IdField(EO, TEXT("id"));
                for (const auto& G : Arr(EO, TEXT("state_bypass")))
                {
                    const uint32 Group = IdField(G->AsObject(), TEXT("group"));
                    Fx.StateGroups.Add(Group);
                    for (const auto& St : Arr(G->AsObject(), TEXT("states"))) Fx.StateBypass.FindOrAdd(Group).Add(uint32(St->AsNumber()));
                }
                for (const auto& C : Arr(EO, TEXT("rtpc"))) Fx.Rtpc.Add(RtpcCurve(C->AsObject()));
                Fx.GainDb = Num(EO, TEXT("gain_db"));
                if (const FObj Eq = Obj(EO, TEXT("eq")))
                {
                    static const TCHAR* Types[] = {TEXT("lowpass"), TEXT("highpass"), TEXT("bandpass"), TEXT("notch"), TEXT("lowshelf"), TEXT("highshelf"), TEXT("peaking")};
                    const auto& Bands = Arr(Eq, TEXT("bands"));
                    for (int32 Bi = 0; Bi < FMath::Min(3, Bands.Num()); ++Bi)
                    {
                        const FObj BO = Bands[Bi]->AsObject();
                        FSCAudioBusEffect::FBand& Band = Fx.Bands[Bi];
                        const FString Type = Str(BO, TEXT("filter"));
                        Band.Type = INDEX_NONE;
                        for (int32 T = 0; T < UE_ARRAY_COUNT(Types); ++T)
                            if (Type == Types[T]) Band.Type = T;
                        Band.GainDb = Num(BO, TEXT("gain_db"));
                        Band.FrequencyHz = Num(BO, TEXT("frequency_hz"));
                        Band.Q = Num(BO, TEXT("q"), 1);
                        Band.bOn = Bool(BO, TEXT("on")) && Band.Type != INDEX_NONE;
                    }
                    Fx.OutputDb = Num(Eq, TEXT("output_level_db"));
                    bool bLfe = true;
                    Fx.bEqProcessLfe = !Eq->TryGetBoolField(TEXT("process_lfe"), bLfe) || bLfe;
                }
                if (const FObj Mt = Obj(EO, TEXT("meter")))
                {
                    Fx.Meter.AttackS = Num(Mt, TEXT("attack_s"));
                    Fx.Meter.ReleaseS = Num(Mt, TEXT("release_s"));
                    Fx.Meter.HoldS = Num(Mt, TEXT("hold_s"));
                    Fx.Meter.MinDb = Num(Mt, TEXT("min_db"), -60);
                    Fx.Meter.MaxDb = Num(Mt, TEXT("max_db"));
                    Fx.Meter.bRms = Str(Mt, TEXT("mode")) == TEXT("rms");
                    Fx.Meter.bDownstream = Bool(Mt, TEXT("apply_downstream_volume"));
                    Fx.Meter.bInfiniteHold = Bool(Mt, TEXT("infinite_hold"));
                    Fx.Meter.bGlobal = Str(Mt, TEXT("scope")) != TEXT("game_object");
                    Fx.Meter.GameParameter = Str(Mt, TEXT("game_parameter"));
                }
                if (const FObj L = Obj(EO, TEXT("limiter")))
                {
                    Fx.Limiter.bOn = true;
                    Fx.Limiter.ThresholdDb = Num(L, TEXT("threshold_db"));
                    Fx.Limiter.Ratio = Num(L, TEXT("ratio"));
                    Fx.Limiter.LookAheadS = Num(L, TEXT("look_ahead_s"));
                    Fx.Limiter.ReleaseS = Num(L, TEXT("release_s"));
                    Fx.Limiter.OutputDb = Num(L, TEXT("output_level_db"));
                    Fx.Limiter.bChannelLink = Bool(L, TEXT("channel_link"));
                    Fx.Limiter.bProcessLfe = Bool(L, TEXT("process_lfe"));
                    if (!Bus.Limiter.bOn) Bus.Limiter = Fx.Limiter;
                }
                Bus.Effects.Add(MoveTemp(Fx));
                if (const FObj Conv = Obj(EO, TEXT("convolution")))
                {
                    Bus.bConvolution = true;
                    Bus.IrMedia = IdField(EO, TEXT("ir_media"));
                    Bus.WetDb = Num(Conv, TEXT("wet_level_db"));
                    Bus.DryDb = Num(Conv, TEXT("dry_level_db"));
                    Bus.PreDelayMs = Num(Conv, TEXT("pre_delay_ms"));
                }
            }
            Bus.Effects.Sort([](const FSCAudioBusEffect& A, const FSCAudioBusEffect& B) { return A.Slot < B.Slot; });
            Out.Buses.Add(Bus.Id, MoveTemp(Bus));
        }
    if (const FObj M = Obj(Root, TEXT("media")))
        for (const auto& [Key, V] : M->Values)
        {
            const FString K(*Key);
            const FObj O = V->AsObject();
            FSCAudioMedia Media;
            Media.File = Str(O, TEXT("file"));
            Media.Codec = Str(O, TEXT("codec"));
            Media.Error = Str(O, TEXT("error"));
            Media.Channels = int32(Num(O, TEXT("channels")));
            Media.Rate = int32(Num(O, TEXT("rate")));
            Media.Seconds = Num(O, TEXT("seconds"));
            Out.Media.Add(Id(K), MoveTemp(Media));
        }
    if (const FObj R = Obj(Root, TEXT("rtpcs")))
        for (const auto& [Key, V] : R->Values)
        {
            const FString K(*Key);
            double D;
            if (V->AsObject()->TryGetNumberField(TEXT("default"), D) || V->AsObject()->TryGetNumberField(TEXT("wwise_default"), D))
                Out.RtpcDefaults.Add(K, D);
        }
    if (const FObj E = Obj(Root, TEXT("environments")))
        for (const auto& [Key, V] : E->Values)
        {
            const FString K(*Key);
            const FObj O = V->AsObject();
            FSCAudioEnvironment Env;
            Env.Name = K;
            Env.AuxBus = IdField(O, TEXT("aux_bus"));
            Env.Record = Str(O, TEXT("record"));
            Env.Error = Str(O, TEXT("error"));
            const auto& S = Arr(O, TEXT("size"));
            if (S.Num() == 2) Env.Size = FVector2f(S[0]->AsNumber(), S[1]->AsNumber());
            const auto& I = Arr(O, TEXT("interiority"));
            if (I.Num() == 2) Env.Interiority = FVector2f(I[0]->AsNumber(), I[1]->AsNumber());
            if (const FObj IR = Obj(O, TEXT("impulse_response")))
            {
                Env.IrFile = Str(IR, TEXT("file"));
                Env.IrMedia = IdField(IR, TEXT("media"));
                Env.IrChannels = int32(Num(IR, TEXT("channels")));
                Env.IrRate = int32(Num(IR, TEXT("rate")));
                Env.IrFrames = int32(Num(IR, TEXT("frames")));
                Env.IrPeakDb = Num(IR, TEXT("peak_db"));
                Env.T20 = Num(IR, TEXT("t20_s"));
                Env.T30 = Num(IR, TEXT("t30_s"));
            }
            Out.Environments.Add(K, MoveTemp(Env));
        }
    if (const FObj R = Obj(Root, TEXT("rooms")))
        for (const auto& [Key, V] : R->Values)
        {
            const FString K(*Key);
            const FObj O = V->AsObject();
            FSCAudioRoom Room;
            Room.Name = Str(O, TEXT("name"));
            Room.Environment = Str(O, TEXT("environment"));
            Room.Rule = Str(O, TEXT("rule"));
            Room.Selection = Str(O, TEXT("selection"));
            Room.VolumeM3 = Num(O, TEXT("volume_m3"));
            Room.SizeM = Num(O, TEXT("size_m"));
            Room.Interiority = Num(O, TEXT("interiority"));
            Out.Rooms.Add(K.ToLower(), MoveTemp(Room));
        }
    if (const FObj C = Obj(Root, TEXT("entity_classes")))
        for (const auto& [Key, V] : C->Values)
        {
            const FString K(*Key);
            const FObj O = V->AsObject();
            FSCAudioClass Class;
            Class.Name = Str(O, TEXT("name"));
            for (const auto& E : Arr(O, TEXT("effects")))
            {
                const FObj EO = E->AsObject();
                FSCAudioEffect Fx;
                Fx.On = Str(EO, TEXT("on"));
                Fx.Tag = Str(EO, TEXT("tag"));
                Fx.TagPath = Str(EO, TEXT("tag_path"));
                Fx.Label = Str(EO, TEXT("label"));
                Fx.Trigger = Str(EO, TEXT("trigger"));
                Fx.Bone = Str(EO, TEXT("bone"));
                const auto& Off = Arr(EO, TEXT("offset"));
                if (Off.Num() == 3) Fx.Offset = FVector(Off[0]->AsNumber(), Off[1]->AsNumber(), Off[2]->AsNumber());
                Class.Effects.Add(MoveTemp(Fx));
            }
            for (const auto& A : Arr(O, TEXT("actions")))
            {
                const FObj AO = A->AsObject();
                Class.Actions.Add({Str(AO, TEXT("path")), Str(AO, TEXT("trigger"))});
            }
            if (const FObj Door = Obj(O, TEXT("door")))
            {
                if (const FObj Events = Obj(Door, TEXT("events")))
                    for (const auto& [Event, EV] : Events->Values)
                    {
                        const FObj EO = EV->AsObject();
                        Class.DoorEvents.Add(FString(*Event), {Str(EO, TEXT("group")), Str(EO, TEXT("tag"))});
                    }
                for (const auto& S : Arr(Door, TEXT("sounds")))
                {
                    const FObj SO = S->AsObject();
                    FSCAudioDoorSound Sound;
                    Sound.Group = Str(SO, TEXT("group"));
                    Sound.Trigger = Str(SO, TEXT("trigger"));
                    Sound.Helper = Str(SO, TEXT("helper"));
                    Sound.bLooped = Bool(SO, TEXT("looped"));
                    const auto& Off = Arr(SO, TEXT("offset"));
                    if (Off.Num() == 3) Sound.Offset = FVector(Off[0]->AsNumber(), Off[1]->AsNumber(), Off[2]->AsNumber());
                    if (!Sound.Trigger.IsEmpty()) Class.DoorSounds.Add(MoveTemp(Sound));
                }
            }
            Out.Classes.Add(K.ToLower(), MoveTemp(Class));
        }
    Out.RootClass = Str(Root, TEXT("root_entity_class")).ToLower();
    if (const FObj E = Obj(Root, TEXT("emitters")))
    {
        Out.SpotCount = int32(Num(E, TEXT("spots")));
        Out.AreaCount = int32(Num(E, TEXT("areas")));
    }
    // Each bus's output-stage gain curves at their RTPC defaults (one default per RTPC: the manifest's `rtpcs`).
    for (auto& [Id, Bus] : Out.Buses)
        for (const FSCAudioRtpcCurve& C : Bus.Rtpc)
            if (const float* D = Out.RtpcDefaults.Find(C.Rtpc); D && C.Type == 0 && FSCAudioBus::IsOutputGain(C.Param)) Bus.CurveDefaultDb += C.Curve.EvalDb(*D);
    return true;
}

TArray<const FSCAudioBus*> FSCAudioManifest::BusChain(uint32 Bus) const
{
    TArray<const FSCAudioBus*> Chain;
    for (uint32 Id = Bus; Id && Chain.Num() < 64;)
    {
        const FSCAudioBus* B = Buses.Find(Id);
        if (!B || Chain.Contains(B)) break;
        Chain.Add(B);
        Id = B->Parent;
    }
    return Chain;
}

FString FSCAudioManifest::RoomEnvironment(const FString& Room) const
{
    const FSCAudioRoom* R = Rooms.Find(Room.ToLower());
    return R ? R->Environment : FString();
}

int32 SCAudioPick(const FSCAudioNode& Node, FSCAudioContainerState& State, FRandomStream& Random)
{
    const int32 Count = Node.Children.Num();
    if (Count == 0) return INDEX_NONE;
    if (Node.Kind == ESCAudioNodeKind::Sequence)
    {
        const int32 Pick = State.Next % Count;
        State.Next = (Pick + 1) % Count;
        return Pick;
    }
    auto Weight = [&](int32 I) { return Node.Weights.IsValidIndex(I) ? FMath::Max(0.f, Node.Weights[I]) : 1.f; };
    TArray<int32> Candidates;
    if (Node.bShuffle)
    {
        if (State.Bag.IsEmpty())
            for (int32 I = 0; I < Count; ++I) State.Bag.Add(I);
        Candidates = State.Bag;
    }
    else
        for (int32 I = 0; I < Count; ++I) Candidates.Add(I);
    // Avoid the last picks (never all of them).
    const int32 Avoid = FMath::Min(Node.AvoidRepeat, Count - 1);
    for (int32 H = State.History.Num() - 1, Left = Avoid; H >= 0 && Left > 0 && Candidates.Num() > 1; --H, --Left)
        Candidates.Remove(State.History[H]);
    float Total = 0;
    for (int32 C : Candidates) Total += Weight(C);
    int32 Pick = Candidates[0];
    if (Total > 0)
    {
        float R = Random.FRand() * Total;
        for (int32 C : Candidates)
        {
            R -= Weight(C);
            if (R <= 0) { Pick = C; break; }
        }
    }
    else
        Pick = Candidates[Random.RandHelper(Candidates.Num())];
    State.History.Add(Pick);
    while (State.History.Num() > FMath::Max(Avoid, 1)) State.History.RemoveAt(0);
    State.Bag.Remove(Pick);
    return Pick;
}

void FSCAudioGain::Add(const FSCAudioProps& P, const TFunctionRef<float(const FString&)>& Rtpc, FRandomStream* Random)
{
    VolumeDb += P.VolumeDb + P.MakeUpDb + P.OutputBusDb;
    PitchCents += P.PitchCents;
    GameAuxDb += P.GameAuxDb;
    if (Random)
        for (const auto& R : P.Random)
        {
            const float Draw = FMath::Lerp(R.Min, R.Max, Random->FRand());
            if (R.Param == TEXT("volume") || R.Param == TEXT("make_up_gain")) VolumeDb += Draw;
            else if (R.Param == TEXT("pitch")) PitchCents += Draw;
        }
    for (const FSCAudioRtpcCurve& C : P.Rtpc)
    {
        if (C.Type != 0) continue; // modulators (LFO) and MIDI are not driven (open item)
        const float X = Rtpc(C.Rtpc);
        if (C.Param == TEXT("volume") || C.Param == TEXT("make_up_gain") || C.Param == TEXT("output_bus_volume"))
            VolumeDb += C.Curve.EvalDb(X);
        else if (C.Param == TEXT("pitch"))
            PitchCents += C.Curve.Eval(X);
        else if (C.Param == TEXT("game_aux_send_volume"))
            GameAuxDb += C.Curve.EvalDb(X);
    }
}

void FSCAudioGain::AddBusVoice(const TArray<const FSCAudioBus*>& Chain, const TFunctionRef<bool(const FString&)>& Skip, const TMap<FString, float>& Defaults)
{
    for (const FSCAudioBus* B : Chain)
    {
        VolumeDb += B->VoiceDb;
        for (const FSCAudioRtpcCurve& C : B->Rtpc)
            if (const float* D = Defaults.Find(C.Rtpc); D && C.Type == 0 && FSCAudioBus::IsVoiceGain(C.Param) && !Skip(C.Rtpc)) VolumeDb += C.Curve.EvalDb(*D);
    }
}
