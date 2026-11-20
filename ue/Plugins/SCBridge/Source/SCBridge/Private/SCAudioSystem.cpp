// WP-A1: the audio player of SCAudioSystem.h. Every value comes from the package's audio manifest; the only names here
// are the engine-driven listener-relative RTPCs (kAzimuth, kListenerCone), which the game's engine sets by name too.
#include "SCAudioSystem.h"
#include "SCPackageSource.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "SCLightImporter.h"
#include "SCImporter.h"
#include "scbridge.h"
#include "AudioDevice.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Components/AudioComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EffectConvolutionReverb.h"
#include "SubmixEffects/AudioMixerSubmixEffectDynamicsProcessor.h"
#include "ISubmixBufferListener.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundSubmix.h"
#include "SubmixEffects/SubmixEffectConvolutionReverb.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCAudio, Log, All);

namespace
{
/** The engine's listener-relative RTPCs: the game engine sets them per emitter from the listener (the banks' curves
 *  on them are the data; these two names are the binding the engine itself uses). */
const FString kAzimuth = TEXT("Azimuth");
const FString kListenerCone = TEXT("Listener_Cone");
constexpr double kForever = 1e18;

/** A/B switch for measuring the playback fix against the behaviour before it (bit 1: a looping media replays the
 *  whole file from its start instead of its loop region; bit 2: a voice ends at its game-clock end, not when its
 *  audio has played out; bit 4: a stopped voice is cut, not faded). 0 in normal use. */
int32 GLegacy = 0;
FAutoConsoleVariableRef CVarLegacy(TEXT("sc.Audio.Legacy"), GLegacy,
    TEXT("A/B measurement only (bits): 1 = whole-file loops, 2 = game-clock voice ends, 4 = stops without a fade; 7 = the pre-fix behaviour. 0 = normal."));

/** A pull later than the audio last handed out lasts by more than this is a gap (two render periods of slack). */
constexpr double kGapSlackS = 0.05;

double CyclesToSeconds(uint64 C) { return FPlatformTime::ToSeconds64(C); }
double WallNow() { return CyclesToSeconds(FPlatformTime::Cycles64()); }
void AtomicMax(std::atomic<int32>& A, int32 V)
{
    for (int32 Cur = A.load(std::memory_order_relaxed); V > Cur && !A.compare_exchange_weak(Cur, V, std::memory_order_relaxed);) {}
}

using FObj = TSharedPtr<FJsonObject>;
double Num(const FObj& O, const TCHAR* K, double D = 0)
{
    double V = D;
    if (O) O->TryGetNumberField(K, V);
    return V;
}
bool Flag(const FObj& O, const TCHAR* K, bool Default = false)
{
    bool V = Default;
    if (O && !O->TryGetBoolField(K, V))
    {
        double N;
        if (O->TryGetNumberField(K, N)) V = N != 0;
    }
    return V;
}
FString Str(const FObj& O, const TCHAR* K)
{
    FString V;
    if (O) O->TryGetStringField(K, V);
    return V;
}
FVector Vec(const TSharedPtr<FJsonValue>& V)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    return V && V->TryGetArray(A) && A->Num() >= 3 ? FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber()) : FVector::ZeroVector;
}
FString ToJson(const FObj& O)
{
    FString Out;
    FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
    return Out;
}

/** Even-odd test of a point against a polygon in the XY plane. */
bool InPolygon(const TArray<FVector>& Poly, double X, double Y)
{
    bool bIn = false;
    for (int32 I = 0, J = Poly.Num() - 1; I < Poly.Num(); J = I++)
        if (((Poly[I].Y > Y) != (Poly[J].Y > Y)) && X < (Poly[J].X - Poly[I].X) * (Y - Poly[I].Y) / (Poly[J].Y - Poly[I].Y) + Poly[I].X)
            bIn = !bIn;
    return bIn;
}
/** Distance (m) from a container point to an area shape, and the nearest point of the shape (container metres). */
double ShapeDistance(const FSCRoomVolume& V, const FVector& P, FVector& Nearest)
{
    const FVector S = V.FromContainer.TransformPosition(P);
    FVector N = S;
    if (V.Shape == FSCRoomVolume::EShape::Prism && V.Points.Num() >= 3)
    {
        double ZMin = TNumericLimits<double>::Max();
        for (const FVector& Pt : V.Points) ZMin = FMath::Min(ZMin, Pt.Z);
        N.Z = FMath::Clamp(S.Z, ZMin, ZMin + V.Height);
        if (!InPolygon(V.Points, S.X, S.Y))
        {
            double Best = TNumericLimits<double>::Max();
            for (int32 I = 0, J = V.Points.Num() - 1; I < V.Points.Num(); J = I++)
            {
                const FVector2D A(V.Points[J].X, V.Points[J].Y), B(V.Points[I].X, V.Points[I].Y), Q(S.X, S.Y);
                const FVector2D AB = B - A;
                const double T = AB.SizeSquared() > 0 ? FMath::Clamp(FVector2D::DotProduct(Q - A, AB) / AB.SizeSquared(), 0., 1.) : 0.;
                const FVector2D C = A + AB * T;
                if (const double D = FVector2D::DistSquared(Q, C); D < Best) { Best = D; N.X = C.X; N.Y = C.Y; }
            }
        }
    }
    else
    {
        const FBox B = V.SegmentBounds();
        N = FVector(FMath::Clamp(S.X, B.Min.X, B.Max.X), FMath::Clamp(S.Y, B.Min.Y, B.Max.Y), FMath::Clamp(S.Z, B.Min.Z, B.Max.Z));
    }
    Nearest = V.FromContainer.Inverse().TransformPosition(N);
    return FVector::Dist(S, N);
}
} // namespace

// ============================================================================================================ wave
bool USCAudioWave::Init(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Ogg, int32 Loops, bool bWholeFileLoops)
{
    if (!Ogg || Ogg->IsEmpty()) return false;
    uint32 Ch = 0, R = 0;
    bWholeFile = bWholeFileLoops;
    Bytes = Ogg;
    // The whole-file mode opens one pass at a time and reopens at the end (the pre-fix behaviour); otherwise the
    // stream plays every pass itself, over the media's loop region.
    const uint32 Passes = bWholeFile ? 1u : uint32(FMath::Max(Loops, 0));
    if (sc_audio_open(Ogg->GetData(), Ogg->Num(), Passes, &Stream, &Ch, &R) != SC_STATUS_OK || !Stream) return false;
    sc_audio_loop_region(Stream, &LoopStart, &LoopEnd, &MediaFrames);
    Channels = int32(Ch);
    Rate = int32(R);
    NumChannels = Channels;
    SetSampleRate(Rate);
    Duration = INDEFINITELY_LOOPING_DURATION;
    bLooping = false;
    LoopsLeft = Loops == 0 ? INDEX_NONE : Loops - 1;
    // Stopping fades the source out over the mixer's stop fade instead of cutting it (UE stops a procedural source
    // without a fade unless asked).
    bRequiresStopFade = (GLegacy & 4) == 0;
    if (Loops > 0 && Rate > 0)
        PlaySeconds = bWholeFile ? double(MediaFrames) * Loops / Rate : double(MediaFrames + uint64(Loops - 1) * (LoopEnd - LoopStart)) / Rate;
    return true;
}

void USCAudioWave::OnBeginGenerate()
{
    Super::OnBeginGenerate();
    // A new mixer source: the pull timing starts over (a source change is counted as such, not as a gap). For a
    // source after the first, the silence between the old source's last pull running dry and the new one's start.
    if (Health.Sources.fetch_add(1, std::memory_order_relaxed) > 0 && Rate > 0)
        if (const uint64 Last = Health.LastCycles.load(std::memory_order_relaxed))
        {
            const double Gap = CyclesToSeconds(FPlatformTime::Cycles64() - Last) - double(Health.LastFrames.load(std::memory_order_relaxed)) / Rate / FMath::Max(0.01f, Pitch);
            AtomicMax(Health.MaxResourceGapUs, int32(FMath::Clamp(Gap * 1e6, 0.0, 2e9)));
        }
    Health.LastCycles.store(0, std::memory_order_relaxed);
}

int32 USCAudioWave::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
    const uint64 Now = FPlatformTime::Cycles64();
    FScopeLock Guard(&Lock);
    const int32 Ch = FMath::Max(1, Channels);
    const int32 Frames = NumSamples / Ch;
    // Health: was this pull later than the audio the last one returned lasts?
    if (const uint64 Last = Health.LastCycles.load(std::memory_order_relaxed); Last && Rate > 0)
    {
        const double Late = CyclesToSeconds(Now - Last) - double(Health.LastFrames.load(std::memory_order_relaxed)) / Rate / FMath::Max(0.01f, Pitch);
        if (Late > kGapSlackS)
        {
            Health.Gaps.fetch_add(1, std::memory_order_relaxed);
            AtomicMax(Health.MaxGapUs, int32(FMath::Min(Late * 1e6, 2e9)));
        }
    }
    OutAudio.SetNumZeroed(Frames * Ch * int32(sizeof(float)));
    float* Out = reinterpret_cast<float*>(OutAudio.GetData());
    const bool bWasEnded = bEnded;
    int32 Done = 0;
    bool bReopened = false; // whole-file mode: a stream that yields nothing right after reopening has no audio
    while (Stream && !bEnded && Done < Frames)
    {
        const int64 Got = sc_audio_read(Stream, Out + Done * Ch, size_t(Frames - Done));
        if (Got > 0) { Done += int32(Got); bReopened = false; continue; }
        if (Got == 0 && bWholeFile && !bReopened && LoopsLeft != 0)
        {
            sc_audio_close(Stream);
            Stream = nullptr;
            uint32 C = 0, R = 0;
            if (sc_audio_open(Bytes->GetData(), Bytes->Num(), 1, &Stream, &C, &R) == SC_STATUS_OK && Stream)
            {
                if (LoopsLeft > 0) --LoopsLeft;
                bReopened = true;
                continue;
            }
        }
        bEnded = true;
    }
    // A pull that finds the stream already ended means the mixer has consumed all of its audio: the voice is done.
    if (bWasEnded) Health.bFinished.store(true, std::memory_order_relaxed);
    const uint64 After = FPlatformTime::Cycles64();
    const int64 Ns = int64(CyclesToSeconds(After - Now) * 1e9);
    Health.DecodeNs.fetch_add(Ns, std::memory_order_relaxed);
    AtomicMax(Health.MaxDecodeUs, int32(Ns / 1000));
    Health.Calls.fetch_add(1, std::memory_order_relaxed);
    Health.Frames.fetch_add(Done, std::memory_order_relaxed);
    Health.LastFrames.store(Frames, std::memory_order_relaxed);
    uint64 Zero = 0;
    Health.FirstCycles.compare_exchange_strong(Zero, Now, std::memory_order_relaxed);
    Health.LastCycles.store(Now, std::memory_order_relaxed);
    return Frames * Ch; // silence past the end
}

void USCAudioWave::BeginDestroy()
{
    {
        FScopeLock Guard(&Lock);
        sc_audio_close(Stream);
        Stream = nullptr;
    }
    Super::BeginDestroy();
}

// ============================================================================================================ scene
TSharedPtr<FSCAudioScene> FSCAudioScene::Load(const FSCPackageSource& Source, const FString& ScenePath, FString& Error)
{
    const FString Path = FPaths::GetPath(ScenePath) / TEXT("audio.json");
    if (!Source.Exists(Path))
    {
        Error = TEXT("no audio.json (a package from before cache format 36)");
        return nullptr;
    }
    TArray<uint8> Bytes;
    if (!Source.ReadBytes(Path, Bytes, Error)) return nullptr;
    TSharedPtr<FSCAudioScene> Scene = MakeShared<FSCAudioScene>();
    Scene->Name = FPaths::GetCleanFilename(FPaths::GetPath(ScenePath));
    const FString Text(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()));
    if (!FSCAudioManifest::Parse(Text, Scene->Manifest, Error)) return nullptr;
    for (const auto& [Id, Media] : Scene->Manifest.Media)
    {
        if (Media.File.IsEmpty()) continue;
        TArray<uint8> Ogg;
        FString E;
        if (Source.ReadBytes(Media.File, Ogg, E))
            Scene->MediaBytes.Add(Id, MakeShared<const TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(Ogg)));
        else
            ++Scene->MediaMissing;
    }
    for (const auto& [Name, Env] : Scene->Manifest.Environments)
    {
        TArray<uint8> Raw;
        FString E;
        if (Env.IrFile.IsEmpty() || !Source.ReadBytes(Env.IrFile, Raw, E) || Raw.Num() % 4) continue;
        TArray<float>& Samples = Scene->Ir.Add(Name);
        Samples.SetNumUninitialized(Raw.Num() / 4);
        FMemory::Memcpy(Samples.GetData(), Raw.GetData(), Raw.Num());
    }
    return Scene;
}

void FSCAudioScene::LoadCooked(ASCVehicle* V)
{
    const FString Dir = FPaths::ProjectContentDir() / V->CookedAudio;
    FSCFolderPackageSource Source(Dir);
    FString Error;
    TSharedPtr<FSCAudioScene> Scene = Load(Source, V->CookedAudioScene, Error);
    TArray<uint8> Bytes;
    FObj Sidecar;
    if (Scene && Source.ReadBytes(FPaths::GetPath(V->CookedAudioScene) / TEXT("audio_interiors.json"), Bytes, Error))
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()))), Sidecar);
    if (!Scene || !Sidecar)
    {
        UE_LOG(LogSCAudio, Warning, TEXT("AUDIO_COOKED %s: %s"), *Dir, Error.IsEmpty() ? TEXT("no audio_interiors.json") : *Error);
        return;
    }
    Scene->Vehicle = V;
    const bool bRooms = V->LightAreas.IsEmpty(); // a cooked actor keeps its anchors, not its room volumes
    TArray<FString> Warnings;
    const TArray<TSharedPtr<FJsonValue>>* Interiors;
    if (Sidecar->TryGetArrayField(TEXT("interiors"), Interiors))
        for (int32 K = 0; K < V->InteriorAnchors.Num(); ++K)
        {
            // Anchors map to the scene's interiors by their recorded index (interiors without a parent have none).
            const int32 I = V->InteriorSceneIndex.IsValidIndex(K) ? V->InteriorSceneIndex[K] : K;
            if (!Interiors->IsValidIndex(I)) continue;
            const FObj O = (*Interiors)[I]->AsObject();
            auto Texts = [&](const TCHAR* Key) {
                TArray<FString> Out;
                const TArray<TSharedPtr<FJsonValue>>* A;
                if (O && O->TryGetArrayField(Key, A))
                    for (const auto& Item : *A) Out.Add(ToJson(Item->AsObject()));
                return Out;
            };
            const bool bHelper = V->InteriorHelperFrames.IsValidIndex(K) && V->InteriorHelperFrames[K];
            Scene->AddInterior(I, V->InteriorAnchors[K], bHelper, Texts(TEXT("audio_spots")), Texts(TEXT("audio_areas")), Warnings);
            if (bRooms) FSCLightAreas::Register(V, V->InteriorAnchors[K], Texts(TEXT("room_volumes")), bHelper, Warnings);
        }
    for (const FString& W : Warnings) UE_LOG(LogSCAudio, Warning, TEXT("AUDIO_COOKED %s"), *W);
    UE_LOG(LogSCAudio, Display, TEXT("AUDIO_COOKED %s scene=%s media=%d rooms_registered=%d"), *Dir, *V->CookedAudioScene, Scene->MediaBytes.Num(), bRooms);
    if (USCAudioSubsystem* A = USCAudioSubsystem::Get(V)) A->AddScene(Scene.ToSharedRef());
}

void FSCAudioScene::AddInterior(int32 Interior, USceneComponent* Anchor, bool bHelperFrame, const TArray<FString>& SpotJson,
                                const TArray<FString>& AreaJson, TArray<FString>& Warnings)
{
    if (InteriorAnchors.Num() <= Interior)
    {
        InteriorAnchors.SetNum(Interior + 1);
        AnchorToContainer.SetNum(Interior + 1);
    }
    InteriorAnchors[Interior] = Anchor;
    // The container -> anchor mapping lights and room volumes use (metres -> cm).
    const FVector X = FSCLightImporter::ContainerBasis(FVector(1, 0, 0), bHelperFrame) * 100;
    const FVector Y = FSCLightImporter::ContainerBasis(FVector(0, 1, 0), bHelperFrame) * 100;
    const FVector Z = FSCLightImporter::ContainerBasis(FVector(0, 0, 1), bHelperFrame) * 100;
    AnchorToContainer[Interior] = FMatrix(FPlane(X, 0), FPlane(Y, 0), FPlane(Z, 0), FPlane(0, 0, 0, 1)).Inverse();
    auto Parse = [&](const FString& Text, FSCAudioPlaced& P) {
        FObj O;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O) || !O) return false;
        P.EntityId = Str(O, TEXT("entity_id")).ToLower();
        P.Name = Str(O, TEXT("name"));
        P.PlayTrigger = Str(O, TEXT("play_trigger"));
        P.StopTrigger = Str(O, TEXT("stop_trigger"));
        P.Room = Str(O, TEXT("room")).ToLower();
        P.Interior = Interior;
        P.Positions.Add(Vec(O->TryGetField(TEXT("position"))));
        const TArray<TSharedPtr<FJsonValue>>* A;
        if (O->TryGetArrayField(TEXT("extra_positions"), A))
            for (const auto& V : *A) P.Positions.Add(Vec(V));
        if (O->TryGetArrayField(TEXT("links"), A))
            for (const auto& V : *A) P.Links.Add(V->AsString().ToLower());
        const FObj* Params;
        P.Params = O->TryGetObjectField(TEXT("params"), Params) ? *Params : MakeShared<FJsonObject>();
        if (O->TryGetArrayField(TEXT("shapes"), A))
            for (const auto& V : *A)
            {
                FSCRoomVolume Shape;
                FString E;
                if (FSCRoomVolume::Parse(ToJson(V->AsObject()), Shape, E)) P.Shapes.Add(MoveTemp(Shape));
                else Warnings.Add(P.Name + TEXT(": area shape: ") + E);
            }
        return true;
    };
    const int32 FirstArea = Areas.Num();
    for (const FString& Text : AreaJson)
    {
        FSCAudioPlaced A;
        if (Parse(Text, A)) Areas.Add(MoveTemp(A));
    }
    for (const FString& Text : SpotJson)
    {
        FSCAudioPlaced S;
        if (!Parse(Text, S)) continue;
        for (int32 I = FirstArea; I < Areas.Num(); ++I)
            if (Areas[I].Links.Contains(S.EntityId)) { S.Area = I; break; }
        Spots.Add(MoveTemp(S));
    }
}

FVector FSCAudioScene::ContainerToWorld(int32 Interior, const FVector& M) const
{
    const USceneComponent* A = InteriorAnchors.IsValidIndex(Interior) ? InteriorAnchors[Interior].Get() : nullptr;
    if (!A) return FVector::ZeroVector;
    return A->GetComponentTransform().TransformPosition(AnchorToContainer[Interior].Inverse().TransformPosition(M));
}

FVector FSCAudioScene::WorldToContainer(int32 Interior, const FVector& World) const
{
    const USceneComponent* A = InteriorAnchors.IsValidIndex(Interior) ? InteriorAnchors[Interior].Get() : nullptr;
    if (!A) return FVector::ZeroVector;
    return AnchorToContainer[Interior].TransformPosition(A->GetComponentTransform().InverseTransformPosition(World));
}

// ============================================================================================================ subsystem
USCAudioSubsystem* USCAudioSubsystem::Get(const UObject* WorldContext)
{
    const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
    return World ? World->GetSubsystem<USCAudioSubsystem>() : nullptr;
}

TStatId USCAudioSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(USCAudioSubsystem, STATGROUP_Tickables);
}

// A bus submix's output level (audio render thread -> game thread), for sc.Audio.Meter.
class FSCBusMeter : public ISubmixBufferListener
{
public:
    FCriticalSection Lock;
    double SumSq = 0, Samples = 0;
    float Peak = 0;
    virtual void OnNewSubmixBuffer(const USoundSubmix*, float* Data, int32 NumSamples, int32, const int32, double) override
    {
        double S = 0;
        float P = 0;
        for (int32 I = 0; I < NumSamples; ++I)
        {
            S += double(Data[I]) * Data[I];
            P = FMath::Max(P, FMath::Abs(Data[I]));
        }
        FScopeLock Guard(&Lock);
        SumSq += S;
        Samples += NumSamples;
        Peak = FMath::Max(Peak, P);
    }
    virtual const FString& GetListenerName() const override
    {
        static const FString Name(TEXT("SCBusMeter"));
        return Name;
    }
};

// The master output trace: taps on the master bus (the game mix after its limiter) and on the engine's main submix
// (what reaches the device, where anything over full scale clips), per 10 ms block. The main submix renders after
// the master bus in the same render callback, so its buffer belongs to the blocks the master tap just closed.
class FSCLimiterTrace
{
public:
    struct FBlock { float PostRms = 0, PostPeak = 0, MainPeak = 0; int32 MainOver = 0; };
    FCriticalSection Lock;
    TArray<FBlock> Blocks;
    int32 BlockFrames = 480;
    void AddPost(const float* Data, int32 NumSamples, int32 InChannels, int32 Rate)
    {
        FScopeLock G(&Lock);
        BlockFrames = FMath::Max(1, Rate / 100);
        const int32 Ch = FMath::Max(1, InChannels);
        for (int32 F0 = 0; F0 < NumSamples / Ch; F0 += BlockFrames)
        {
            FBlock B;
            double S = 0;
            const int32 F1 = FMath::Min(NumSamples / Ch, F0 + BlockFrames);
            for (int32 I = F0 * Ch; I < F1 * Ch; ++I)
            {
                S += double(Data[I]) * Data[I];
                B.PostPeak = FMath::Max(B.PostPeak, FMath::Abs(Data[I]));
            }
            B.PostRms = float(FMath::Sqrt(S / FMath::Max(1, (F1 - F0) * Ch)));
            Blocks.Add(B);
        }
    }
    void AddMain(const float* Data, int32 NumSamples, int32 InChannels)
    {
        FScopeLock G(&Lock);
        const int32 Ch = FMath::Max(1, InChannels);
        const int32 PerCallback = FMath::Max(1, NumSamples / Ch / BlockFrames);
        for (int32 K = 0; K < PerCallback; ++K)
        {
            const int32 Bi = Blocks.Num() - PerCallback + K;
            if (!Blocks.IsValidIndex(Bi)) continue;
            for (int32 I = K * BlockFrames * Ch; I < FMath::Min(NumSamples, (K + 1) * BlockFrames * Ch); ++I)
            {
                const float A = FMath::Abs(Data[I]);
                Blocks[Bi].MainPeak = FMath::Max(Blocks[Bi].MainPeak, A);
                Blocks[Bi].MainOver += A > 1.f;
            }
        }
    }
};

class FSCLimiterTap : public ISubmixBufferListener
{
public:
    FSCLimiterTap(TSharedRef<FSCLimiterTrace, ESPMode::ThreadSafe> InTrace, bool bInMain) : Trace(InTrace), bMain(bInMain) {}
    TSharedRef<FSCLimiterTrace, ESPMode::ThreadSafe> Trace;
    bool bMain;
    virtual void OnNewSubmixBuffer(const USoundSubmix*, float* Data, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double) override
    {
        if (bMain) Trace->AddMain(Data, NumSamples, NumChannels);
        else Trace->AddPost(Data, NumSamples, NumChannels, SampleRate);
    }
    virtual const FString& GetListenerName() const override
    {
        static const FString Name(TEXT("SCLimiterTap"));
        return Name;
    }
};

void USCAudioSubsystem::StartLimiterTrace()
{
    StopLimiterTrace(FString());
    FAudioDeviceHandle Device = GetWorld() ? GetWorld()->GetAudioDevice() : FAudioDeviceHandle();
    if (!Device) return;
    LimiterTrace = MakeShared<FSCLimiterTrace, ESPMode::ThreadSafe>();
    const TSharedRef<FSCLimiterTrace, ESPMode::ThreadSafe> T = LimiterTrace.ToSharedRef();
    auto Tap = [&](USoundSubmix* Submix, bool bMain) {
        TSharedRef<FSCLimiterTap, ESPMode::ThreadSafe> L = MakeShared<FSCLimiterTap, ESPMode::ThreadSafe>(T, bMain);
        Device->RegisterSubmixBufferListener(L, *Submix);
        LimiterTaps.Add({Submix, L});
    };
    for (const auto& [Bus, Preset] : Limiters)
        if (TObjectPtr<USoundSubmix>* Master = BusSubmixes.Find(Bus); Master && *Master)
        {
            Tap(*Master, false);
            break; // one master
        }
    Tap(&Device->GetMainSubmixObject(), true);
    UE_LOG(LogSCAudio, Display, TEXT("AUDIO_LIMITER_TRACE start taps=%d"), LimiterTaps.Num());
}

void USCAudioSubsystem::StopLimiterTrace(const FString& CsvPath)
{
    if (FAudioDeviceHandle Device = GetWorld() ? GetWorld()->GetAudioDevice() : FAudioDeviceHandle())
        for (const auto& [Submix, L] : LimiterTaps)
            if (USoundSubmix* S = Submix.Get()) Device->UnregisterSubmixBufferListener(L, *S);
    LimiterTaps.Reset();
    if (!LimiterTrace) return;
    TArray<FSCLimiterTrace::FBlock> Blocks;
    { FScopeLock G(&LimiterTrace->Lock); Blocks = LimiterTrace->Blocks; }
    LimiterTrace.Reset();
    auto Db = [](float X) { return X > 1e-9f ? 20.f * FMath::LogX(10.f, X) : -180.f; };
    float PostPeakMax = 0, MainPeakMax = 0, Threshold = 1.f;
    int32 MainOver = 0, MainOverBlocks = 0, PostOverThreshold = 0;
    for (const auto& [Bus, Preset] : Limiters)
        if (Preset) Threshold = FMath::Pow(10.f, Preset->Settings.ThresholdDb / 20.f);
    FString Csv = TEXT("t_s,master_rms_db,master_peak_db,main_peak_db,main_samples_over_0dbfs\n");
    for (int32 I = 0; I < Blocks.Num(); ++I)
    {
        const FSCLimiterTrace::FBlock& B = Blocks[I];
        PostPeakMax = FMath::Max(PostPeakMax, B.PostPeak);
        MainPeakMax = FMath::Max(MainPeakMax, B.MainPeak);
        PostOverThreshold += B.PostPeak > Threshold * 1.01f;
        MainOver += B.MainOver;
        MainOverBlocks += B.MainOver > 0;
        Csv += FString::Printf(TEXT("%.2f,%.2f,%.2f,%.2f,%d\n"), I * 0.01, Db(B.PostRms), Db(B.PostPeak), Db(B.MainPeak), B.MainOver);
    }
    // The limiter holds its ceiling when no block of its output peaks over the threshold; anything over 0 dBFS at the
    // main output is clipped by the device.
    UE_LOG(LogSCAudio, Display,
           TEXT("AUDIO_LIMITER_TRACE blocks=%d master_peak_db=%.1f master_blocks_over_threshold=%d main_peak_db=%.1f main_samples_over_0dbfs=%d main_blocks_over_0dbfs=%d"),
           Blocks.Num(), Db(PostPeakMax), PostOverThreshold, Db(MainPeakMax), MainOver, MainOverBlocks);
    if (!CsvPath.IsEmpty()) FFileHelper::SaveStringToFile(Csv, *CsvPath);
}

void USCAudioSubsystem::SetLimiterMode(int32 Mode)
{
    for (const auto& [Bus, Preset] : Limiters)
        if (Preset)
        {
            FSubmixEffectDynamicsProcessorSettings S = Preset->Settings;
            S.bBypass = Mode == 0;
            // 2: a fast attack (a fifth of the look-ahead, digital), for A/B measurements of the port's overshoot.
            S.AttackTimeMsec = Mode == 2 ? FMath::Max(1.f, S.LookAheadMsec / 5.f) : S.LookAheadMsec;
            S.bAnalogMode = Mode != 2;
            Preset->SetSettings(S);
        }
    UE_LOG(LogSCAudio, Display, TEXT("AUDIO_LIMITER mode=%d limiters=%d"), Mode, Limiters.Num());
}

void USCAudioSubsystem::StartMeter()
{
    StopMeter();
    FAudioDeviceHandle Device = GetWorld() ? GetWorld()->GetAudioDevice() : FAudioDeviceHandle();
    if (!Device) return;
    for (const auto& [Bus, Submix] : BusSubmixes)
        if (Submix)
        {
            TSharedRef<FSCBusMeter, ESPMode::ThreadSafe> M = MakeShared<FSCBusMeter, ESPMode::ThreadSafe>();
            Device->RegisterSubmixBufferListener(M, *Submix);
            Meters.Add({Bus, M});
        }
    UE_LOG(LogSCAudio, Display, TEXT("AUDIO_METER start buses=%d"), Meters.Num());
}

void USCAudioSubsystem::StopMeter()
{
    FAudioDeviceHandle Device = GetWorld() ? GetWorld()->GetAudioDevice() : FAudioDeviceHandle();
    for (const auto& [Bus, M] : Meters)
    {
        TObjectPtr<USoundSubmix>* Submix = BusSubmixes.Find(Bus);
        if (Device && Submix && *Submix) Device->UnregisterSubmixBufferListener(M, **Submix);
        FScopeLock Guard(&M->Lock);
        const double Rms = M->Samples > 0 ? FMath::Sqrt(M->SumSq / M->Samples) : 0;
        UE_LOG(LogSCAudio, Display, TEXT("AUDIO_METER bus=%u peak_db=%.1f rms_db=%.1f samples=%.0f"), Bus,
               M->Peak > 0 ? 20.f * FMath::LogX(10.f, M->Peak) : -200.f, Rms > 0 ? 20.0 * FMath::LogX(10.0, Rms) : -200.0, M->Samples);
    }
    Meters.Reset();
}

void USCAudioSubsystem::Deinitialize()
{
    StopMeter();
    // The registered bus submixes leave the device, children before parents (registration roots them).
    if (FAudioDeviceHandle Device = GetWorld() ? GetWorld()->GetAudioDevice() : FAudioDeviceHandle())
    {
        auto Depth = [](const USoundSubmix* X) {
            int32 D = 0;
            for (const USoundSubmix* P = X ? Cast<USoundSubmix>(X->ParentSubmix) : nullptr; P && D < 64; P = Cast<USoundSubmix>(P->ParentSubmix)) ++D;
            return D;
        };
        TArray<USoundSubmix*> Order;
        for (const auto& [Bus, Submix] : BusSubmixes) if (Submix) Order.Add(Submix);
        Order.Sort([&](USoundSubmix& A, USoundSubmix& B) { return Depth(&A) > Depth(&B); });
        for (USoundSubmix* Submix : Order) Device->UnregisterSoundSubmix(Submix, false);
    }
    BusSubmixes.Reset();
    for (auto& [Id, I] : Instances)
        for (FVoice& V : I.Voices)
            if (V.Component) V.Component->Stop();
    Instances.Reset();
    Scenes.Reset();
    Super::Deinitialize();
}

void USCAudioSubsystem::AddScene(const TSharedRef<FSCAudioScene>& Scene)
{
    Scenes.Add(Scene);
    LogScene(*Scene);
}

void USCAudioSubsystem::RemoveScene(const ASCVehicle* Vehicle)
{
    for (auto It = Instances.CreateIterator(); It; ++It)
    {
        const TSharedPtr<FSCAudioScene> S = It->Value.Scene.Pin();
        if (S && S->Vehicle.Get() != Vehicle) continue;
        for (FVoice& V : It->Value.Voices)
            if (V.Component) V.Component->DestroyComponent();
        It.RemoveCurrent();
    }
    Scenes.RemoveAll([&](const TSharedRef<FSCAudioScene>& S) { return S->Vehicle.Get() == Vehicle || !S->Vehicle.IsValid(); });
}

TSharedPtr<FSCAudioScene> USCAudioSubsystem::SceneOf(const ASCVehicle* Vehicle) const
{
    for (const auto& S : Scenes)
        if (S->Vehicle.Get() == Vehicle) return S;
    return nullptr;
}

void USCAudioSubsystem::LogScene(const FSCAudioScene& S) const
{
    int32 Playable = 0, Extra = 0, Governed = 0;
    for (const auto& [N, T] : S.Manifest.Triggers) Playable += T.IsPlayable();
    for (const auto& Spot : S.Spots)
    {
        Extra += Spot.Positions.Num() - 1;
        Governed += Spot.Area != INDEX_NONE;
    }
    UE_LOG(LogSCAudio, Display, TEXT("AUDIO_SCENE %s triggers=%d playable=%d nodes=%d media=%d media_missing=%d buses=%d environments=%d rooms=%d classes=%d"),
           *S.Name, S.Manifest.Triggers.Num(), Playable, S.Manifest.Nodes.Num(), S.MediaBytes.Num(), S.MediaMissing, S.Manifest.Buses.Num(),
           S.Manifest.Environments.Num(), S.Manifest.Rooms.Num(), S.Manifest.Classes.Num());
    TMap<int32, TPair<int32, int32>> PerInterior;
    for (const auto& Spot : S.Spots) ++PerInterior.FindOrAdd(Spot.Interior).Key;
    for (const auto& Area : S.Areas) ++PerInterior.FindOrAdd(Area.Interior).Value;
    UE_LOG(LogSCAudio, Display, TEXT("AUDIO_EMITTERS %s spots=%d extra_positions=%d governed=%d areas=%d entities=%d (manifest spots=%d areas=%d)"),
           *S.Name, S.Spots.Num(), Extra, Governed, S.Areas.Num(), S.Entities.Num(), S.Manifest.SpotCount, S.Manifest.AreaCount);
    for (const auto& [Interior, Count] : PerInterior)
    {
        const USceneComponent* A = S.InteriorAnchors.IsValidIndex(Interior) ? S.InteriorAnchors[Interior].Get() : nullptr;
        int32 Positions = 0;
        for (const auto& Spot : S.Spots)
            if (Spot.Interior == Interior) Positions += Spot.Positions.Num() - 1;
        UE_LOG(LogSCAudio, Display, TEXT("AUDIO_EMITTERS %s interior=%s spots=%d extra_positions=%d areas=%d"), *S.Name,
               A ? *A->GetName() : TEXT("?"), Count.Key, Positions, Count.Value);
    }
    for (const auto& [Name, Env] : S.Manifest.Environments)
        UE_LOG(LogSCAudio, Display, TEXT("AUDIO_ENVIRONMENT %s %s aux_bus=%u ir_frames=%d ir_rate=%d t20=%.3f t30=%.3f peak_db=%.1f size=[%g,%g] loaded=%d"),
               *S.Name, *Name, Env.AuxBus, Env.IrFrames, Env.IrRate, Env.T20, Env.T30, Env.IrPeakDb, Env.Size.X, Env.Size.Y, S.Ir.Contains(Name));
}

// ------------------------------------------------------------------------------------------------ mix: submixes
USoundSubmix* USCAudioSubsystem::BusSubmix(FSCAudioScene& S, uint32 Bus)
{
    if (!Bus) return nullptr;
    if (TObjectPtr<USoundSubmix>* Found = BusSubmixes.Find(Bus)) return *Found;
    const FSCAudioBus* B = S.Manifest.Buses.Find(Bus);
    if (!B) return nullptr;
    USoundSubmix* Parent = BusSubmix(S, B->Parent); // the master's parent is the engine's main submix
    USoundSubmix* Submix = NewObject<USoundSubmix>(this, *FString::Printf(TEXT("SCBus_%s"), B->Name.IsEmpty() ? *FString::Printf(TEXT("%u"), Bus) : *B->Name));
    if (Parent)
    {
        Submix->ParentSubmix = Parent;
        Parent->ChildSubmixes.Add(Submix);
    }
    // The bus's own level as the submix's base output volume (dB; applied once, at the submix's init).
    Submix->OutputVolumeModulation.Value = B->MixDb();
    // No auto-disable: UE wakes a silent submix only through its parent, and a child added under a silent (disabled)
    // parent is linked by a command that parent pumps only when it renders, so a bus created while its ancestors are idle
    // (an event sound on a quiet bus chain) would never be heard. The game's buses are always live.
    Submix->bAutoDisable = false;
    if (B->MixDb() > 0.f) // UE clamps a submix's output volume to 0 dB; no bus of the C2 / hangar data is above it
        UE_LOG(LogSCAudio, Warning, TEXT("AUDIO_BUS bus=%u level %+.2f dB above 0 dB is applied as 0 dB (UE submix volume range)"), Bus, B->MixDb());
    if (B->bConvolution)
    {
        // An environment's aux bus: the game's Convolution Reverb over its impulse response (wet / dry as authored; the
        // IR applied at its stored level, which the plugin header's peak level confirms is not normalised).
        for (const auto& [Name, Env] : S.Manifest.Environments)
        {
            const TArray<float>* Samples = S.Ir.Find(Name);
            if (Env.AuxBus != Bus || !Samples) continue;
            UAudioImpulseResponse* IR = NewObject<UAudioImpulseResponse>(this);
            // The authored pre-delay as leading silence of the IR (UE's convolution reverb has no pre-delay of its own).
            const int32 Lead = FMath::RoundToInt(FMath::Max(0.f, B->PreDelayMs) * 0.001f * Env.IrRate) * FMath::Max(1, Env.IrChannels);
            IR->ImpulseResponse.Reset(Lead + Samples->Num());
            IR->ImpulseResponse.AddZeroed(Lead);
            IR->ImpulseResponse.Append(*Samples);
            IR->NumChannels = Env.IrChannels;
            IR->SampleRate = Env.IrRate;
            IR->NormalizationVolumeDb = 0.f;
            IR->bTrueStereo = false;
            USubmixEffectConvolutionReverbPreset* Preset = NewObject<USubmixEffectConvolutionReverbPreset>(this);
            FSubmixEffectConvolutionReverbSettings Settings = Preset->Settings;
            Settings.WetVolumeDb = FMath::Clamp(B->WetDb, -96.f, 0.f);
            Settings.DryVolumeDb = FMath::Clamp(B->DryDb, -96.f, 0.f);
            Settings.NormalizationVolumeDb = 0.f;
            Preset->SetSettings(Settings);
            Preset->SetImpulseResponse(IR);
            Submix->SubmixEffectChain.Add(Preset);
            Keep.Add(IR);
            Keep.Add(Preset);
            UE_LOG(LogSCAudio, Display, TEXT("AUDIO_REVERB bus=%u env=%s ir_frames=%d channels=%d rate=%d wet_db=%.1f dry_db=%.1f pre_delay_ms=%.1f t30=%.3f bus_mix_db=%.1f"),
                   Bus, *Name, Samples->Num() / FMath::Max(1, Env.IrChannels), Env.IrChannels, Env.IrRate, B->WetDb, B->DryDb, B->PreDelayMs, Env.T30,
                   B->MixDb());
            break;
        }
    }
    if (B->Limiter.bOn)
    {
        // The bus's Wwise Peak Limiter as UE's dynamics processor in limiter mode: the authored threshold, look-ahead
        // (also the attack: the look-ahead is the time the Wwise limiter has to reach its gain), release and output level,
        // peak detection, channels linked as authored, hard knee. UE's limiter has an infinite ratio; the authored ratio is
        // logged (40:1 on the C2's master: above the -0.1 dB threshold the two differ by under 0.1 dB per 4 dB over).
        USubmixEffectDynamicsProcessorPreset* Preset = NewObject<USubmixEffectDynamicsProcessorPreset>(this);
        FSubmixEffectDynamicsProcessorSettings Settings = Preset->Settings;
        Settings.DynamicsProcessorType = ESubmixEffectDynamicsProcessorType::Limiter;
        Settings.PeakMode = ESubmixEffectDynamicsPeakMode::Peak;
        Settings.LinkMode = B->Limiter.bChannelLink ? ESubmixEffectDynamicsChannelLinkMode::Peak : ESubmixEffectDynamicsChannelLinkMode::Disabled;
        Settings.ThresholdDb = B->Limiter.ThresholdDb;
        Settings.KneeBandwidthDb = 0.f;
        Settings.LookAheadMsec = B->Limiter.LookAheadS * 1000.f;
        Settings.AttackTimeMsec = B->Limiter.LookAheadS * 1000.f;
        Settings.ReleaseTimeMsec = B->Limiter.ReleaseS * 1000.f;
        Settings.OutputGainDb = B->Limiter.OutputDb;
        Preset->SetSettings(Settings);
        Submix->SubmixEffectChain.Add(Preset);
        Keep.Add(Preset);
        Limiters.Add(Bus, Preset);
        UE_LOG(LogSCAudio, Display, TEXT("AUDIO_LIMITER bus=%u threshold_db=%.2f ratio=%.1f look_ahead_ms=%.1f release_ms=%.1f output_db=%.2f channel_link=%d"),
               Bus, B->Limiter.ThresholdDb, B->Limiter.Ratio, B->Limiter.LookAheadS * 1000.f, B->Limiter.ReleaseS * 1000.f, B->Limiter.OutputDb,
               B->Limiter.bChannelLink);
    }
    if (UWorld* World = GetWorld())
        if (FAudioDeviceHandle Device = World->GetAudioDevice())
        {
            Device->RegisterSoundSubmix(Submix, true);
        }
    BusSubmixes.Add(Bus, Submix);
    UE_LOG(LogSCAudio, Display, TEXT("AUDIO_BUS bus=%u name=%s parent=%u aux=%d submix_db=%.2f (bus_volume=%.2f make_up=%.2f rtpc_defaults=%.2f gain_fx=%.2f) voice_db=%.2f"),
           Bus, B->Name.IsEmpty() ? TEXT("-") : *B->Name, B->Parent, B->bAux, B->MixDb(), B->BusDb, B->MakeUpDb, B->RtpcDefaultDb, B->GainFxDb, B->VoiceDb);
    return Submix;
}

USoundSubmix* USCAudioSubsystem::EnvironmentSubmix(FSCAudioScene& S, const FString& Environment)
{
    const FSCAudioEnvironment* Env = S.Manifest.Environments.Find(Environment);
    return Env && Env->AuxBus ? BusSubmix(S, Env->AuxBus) : nullptr;
}

// ------------------------------------------------------------------------------------------------ posting
FVector USCAudioSubsystem::PlacePosition(const FInstance& I) const
{
    if (I.Place.bListener) return ListenerPos;
    if (const USceneComponent* A = I.Place.Attach.Get())
    {
        const FTransform T = I.Place.Socket.IsNone() ? A->GetComponentTransform() : A->GetSocketTransform(I.Place.Socket);
        return T.TransformPosition(I.Place.Offset);
    }
    if (I.Place.Interior != INDEX_NONE)
        if (const TSharedPtr<FSCAudioScene> S = I.Scene.Pin()) return S->ContainerToWorld(I.Place.Interior, I.Place.ContainerPos);
    return I.Place.World;
}

FString USCAudioSubsystem::EnvironmentAt(const FSCAudioScene& S, const FInstance& I, const FVector& World) const
{
    FString Room = I.Place.Room;
    if (Room.IsEmpty())
        if (const ASCVehicle* V = S.Vehicle.Get()) Room = V->LightRoomAt(World).ToLower();
    return S.Manifest.RoomEnvironment(Room);
}

float USCAudioSubsystem::RtpcValue(const FSCAudioScene& S, const FInstance& I, const FString& Name, const FVector& Emitter) const
{
    if (!I.Place.AreaFadeRtpc.IsEmpty() && Name == I.Place.AreaFadeRtpc)
    {
        if (S.Areas.IsValidIndex(I.Place.Area)) return S.Areas[I.Place.Area].Fade;
        return 1.f; // an emitter no area governs is faded by none
    }
    if (Name == kAzimuth || Name == kListenerCone)
    {
        const FVector D = (Emitter - ListenerPos).GetSafeNormal();
        if (D.IsNearlyZero()) return 0.f;
        const FVector F = ListenerFwd.GetSafeNormal(), R = ListenerRight.GetSafeNormal();
        if (Name == kListenerCone) // the angle between the listener's forward and the emitter, in 3D
            return float(FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(D, F), -1.0, 1.0))));
        // Azimuth: the signed angle around the listener's up axis (its forward / right plane).
        return float(FMath::RadiansToDegrees(FMath::Atan2(FVector::DotProduct(D, R), FVector::DotProduct(D, F))));
    }
    const float* D = S.Manifest.RtpcDefaults.Find(Name);
    return D ? *D : 0.f;
}

int32 USCAudioSubsystem::Post(FSCAudioScene& S, const FString& Trigger, const FSCAudioPlace& Where)
{
    const FSCAudioTrigger* T = S.Manifest.Triggers.Find(Trigger);
    if (!T)
    {
        if (bLog) UE_LOG(LogSCAudio, Display, TEXT("AUDIO_SKIP trigger=%s reason=\"not in the manifest\" emitter=%s"), *Trigger, *Where.Label);
        return INDEX_NONE;
    }
    // Stop actions stop what their target plays: object-scoped stops (an odd low byte: Stop_E_O, Stop_ALL_O, ...) only
    // on this emitter, the others everywhere in the scene; after the action's delay, over its fade. The target is matched
    // against each playing action's root (a stop aimed at an ancestor or a descendant of that root is not resolved).
    for (const auto& O : T->Other)
        if ((O.ActionType >> 8) == 0x01)
        {
            const bool bObject = (O.ActionType & 0x01) != 0;
            for (auto& [Id, I] : Instances)
                if (I.Scene.Pin().Get() == &S && I.Action && S.Manifest.Nodes.IsValidIndex(I.Action->Root) && S.Manifest.Nodes[I.Action->Root].Id == O.Target &&
                    (!bObject || (I.Place.Attach == Where.Attach && I.Place.Socket == Where.Socket && I.Place.Label == Where.Label)))
                    Stop(Id, O.FadeS, O.DelayS);
        }
    if (!T->IsPlayable())
    {
        if (bLog && T->Other.IsEmpty())
            UE_LOG(LogSCAudio, Display, TEXT("AUDIO_SKIP trigger=%s status=%s reason=\"%s\" emitter=%s"), *Trigger, *T->Status, *T->Reason, *Where.Label);
        return INDEX_NONE;
    }
    int32 Group = INDEX_NONE;
    for (const FSCAudioAction& A : T->Play)
    {
        const int32 Id = NextId++;
        FInstance& I = Instances.Add(Id);
        I.Id = Id;
        if (Group == INDEX_NONE) Group = Id;
        I.Group = Group;
        for (const TSharedRef<FSCAudioScene>& X : Scenes)
            if (&X.Get() == &S) I.Scene = X;
        I.Trigger = Trigger;
        I.Place = Where;
        I.Action = &A;
        I.Random.Initialize(int32(GetTypeHash(Trigger) ^ uint32(Id * 2654435761u)));
        I.Position = PlacePosition(I);
        I.Environment = EnvironmentAt(S, I, I.Position);
        Start(I, S, {A.Root}, Clock + A.DelayS + A.Context.InitialDelayS, A.FadeInS, 0.f);
        UpdateInstance(I, Clock);
        if (bLog)
            for (const FVoice& V : I.Voices)
            {
                const FSCAudioNode& N = S.Manifest.Nodes[V.Leaf];
                UE_LOG(LogSCAudio, Display,
                       TEXT("AUDIO_PLAY trigger=%s media=%u emitter=\"%s\" pos=(%.0f,%.0f,%.0f) dist_m=%.2f gain_db=%.1f static_db=%.1f bus=%u bus_mix_db=%.1f env=%s send_db=%.1f loops=%d spatial=%d"),
                       *Trigger, N.Media, *Where.Label, I.Position.X, I.Position.Y, I.Position.Z, V.LastDistance, V.LastGain, V.StaticDb, V.Bus,
                       S.Manifest.Buses.Contains(V.Bus) ? S.Manifest.Buses[V.Bus].MixDb() : 0.f, I.Environment.IsEmpty() ? TEXT("-") : *I.Environment,
                       V.LastSend, N.LoopCount, V.bSpatial);
            }
    }
    return Group;
}

double USCAudioSubsystem::Start(FInstance& I, FSCAudioScene& S, const TArray<int32>& Path, double T0, float FadeIn, float CrossfadeOut)
{
    const int32 NodeIndex = Path.Last();
    if (!S.Manifest.Nodes.IsValidIndex(NodeIndex) || Path.Num() > 32) return T0;
    const FSCAudioNode& N = S.Manifest.Nodes[NodeIndex];
    T0 += N.Props.InitialDelayS; // the node's own initial delay (its ancestors' are in the action context or added above)
    auto Child = [&](int32 C) {
        TArray<int32> P = Path;
        P.Add(N.Children[C]);
        return P;
    };
    switch (N.Kind)
    {
    case ESCAudioNodeKind::Sound:
        return StartVoice(I, S, Path, T0, FadeIn);
    case ESCAudioNodeKind::Blend:
    case ESCAudioNodeKind::Mixer:
    case ESCAudioNodeKind::Switch:
    {
        double End = T0;
        const FVector Here = PlacePosition(I);
        for (int32 C = 0; C < N.Children.Num(); ++C)
            if (N.Kind != ESCAudioNodeKind::Blend || BlendGainDb(S, I, N, C, Here) > -200.f) // a layer that silences a child
                End = FMath::Max(End, Start(I, S, Child(C), T0, FadeIn, CrossfadeOut));
        return End;
    }
    default:
        break;
    }
    if (!N.bContinuous)
    {
        const int32 C = SCAudioPick(N, ContainerState(S, NodeIndex, I), I.Random);
        return C == INDEX_NONE ? T0 : Start(I, S, Child(C), T0, FadeIn, CrossfadeOut);
    }
    // Continuous: items one after another, `loop_count` passes over the children. A finite run is scheduled whole now
    // (its end is the container's end for an enclosing container); an infinite one item by item as it plays. Each item
    // fades out into a successor only (crossfade transitions); the last one plays out.
    const bool bCrossfade = N.Transition.StartsWith(TEXT("crossfade"));
    const bool bPower = N.Transition == TEXT("crossfade_power");
    auto Item = [&](double At, bool bFirst) -> TPair<double, double> {
        const int32 C = SCAudioPick(N, ContainerState(S, NodeIndex, I), I.Random);
        if (C == INDEX_NONE) return {At, -1.0};
        const int32 Before = I.Voices.Num();
        const double End = Start(I, S, Child(C), At, bFirst ? FadeIn : (bCrossfade ? N.TransitionS : 0.f), 0.f);
        for (int32 V = Before; V < I.Voices.Num(); ++V) I.Voices[V].bPower = bPower;
        return {At, End};
    };
    auto FadeIntoNext = [&](int32 Before, double End, double Next) {
        if (!bCrossfade || End >= kForever) return;
        for (int32 V = Before; V < I.Voices.Num(); ++V)
        {
            I.Voices[V].FadeOutStart = FMath::Min(Next, End);
            I.Voices[V].FadeOutEnd = End;
        }
    };
    if (N.LoopCount > 0)
    {
        double At = T0, Last = T0;
        const int32 Items = N.LoopCount * N.Children.Num();
        for (int32 K = 0; K < Items; ++K)
        {
            const int32 Before = I.Voices.Num();
            const TPair<double, double> R = Item(At, K == 0);
            if (R.Value < 0) break;
            Last = FMath::Max(Last, R.Value);
            if (R.Value >= kForever) return kForever;
            const double Next = NextItemAt(N, I, R.Key, R.Value);
            if (K + 1 < Items) FadeIntoNext(Before, R.Value, Next);
            At = Next;
        }
        return Last;
    }
    const int32 Before = I.Voices.Num();
    const TPair<double, double> R = Item(T0, true);
    if (R.Value < 0) return T0;
    if (R.Value >= kForever) return kForever;
    FRun Run;
    Run.Node = NodeIndex;
    Run.Path = Path;
    Run.ItemsLeft = INDEX_NONE;
    Run.NextAt = NextItemAt(N, I, R.Key, R.Value);
    FadeIntoNext(Before, R.Value, Run.NextAt);
    I.Runs.Add(MoveTemp(Run));
    return kForever;
}

double USCAudioSubsystem::NextItemAt(const FSCAudioNode& N, FInstance& I, double ItemStart, double ItemEnd) const
{
    // The authored transition time plus a draw from its random range.
    const float Random = N.TransitionRandomS.X != 0.f || N.TransitionRandomS.Y != 0.f ? I.Random.FRandRange(N.TransitionRandomS.X, N.TransitionRandomS.Y) : 0.f;
    const double T = FMath::Max(0.0, double(N.TransitionS + Random));
    if (N.Transition.StartsWith(TEXT("crossfade"))) return FMath::Max(ItemStart, ItemEnd - T);
    if (N.Transition == TEXT("delay")) return ItemEnd + T;
    if (N.Transition == TEXT("trigger_rate")) return ItemStart + T; // the next item starts a fixed time after the last one started
    return ItemEnd;                                                 // disabled, sample accurate
}

float USCAudioSubsystem::BlendGainDb(const FSCAudioScene& S, const FInstance& I, const FSCAudioNode& Blend, int32 ChildPos, const FVector& Emitter) const
{
    float Linear = 1.f;
    for (const FSCAudioNode::FLayer& L : Blend.Layers)
        for (const auto& [Child, Curve] : L.Children)
            if (Child == ChildPos && !Curve.IsEmpty()) Linear *= FMath::Max(0.f, Curve.Eval(RtpcValue(S, I, L.Rtpc, Emitter)));
    return Linear <= 1e-10f ? -200.f : 20.f * FMath::LogX(10.f, Linear);
}

FSCAudioContainerState& USCAudioSubsystem::ContainerState(FSCAudioScene& S, int32 Node, const FInstance& I)
{
    // Wwise keeps a container's sequence / avoid-repeat history per game object unless the container is global.
    uint32 Emitter = 0;
    if (S.Manifest.Nodes.IsValidIndex(Node) && !S.Manifest.Nodes[Node].bGlobal)
        Emitter = HashCombine(GetTypeHash(I.Place.Label), HashCombine(GetTypeHash(I.Place.Attach.Get()), GetTypeHash(I.Place.Socket))) | 1u;
    return S.Containers.FindOrAdd((uint64(uint32(Node)) << 32) | Emitter);
}

void USCAudioSubsystem::SetSpatial(FInstance& I, bool bListener)
{
    // An area bed switches between listener-centred (not panned) and border-positioned (panned as authored).
    if (I.Place.bListener == bListener) return;
    I.Place.bListener = bListener;
    const TSharedPtr<FSCAudioScene> S = I.Scene.Pin();
    if (!S) return;
    for (FVoice& V : I.Voices)
    {
        V.bSpatial = S->Manifest.Nodes[V.Leaf].bSpatial && !bListener;
        if (!V.Component) continue;
        V.Component->bAllowSpatialization = V.bSpatial;
        V.Component->AttenuationOverrides.bSpatialize = V.bSpatial;
        V.Component->AdjustAttenuation(V.Component->AttenuationOverrides);
    }
}

double USCAudioSubsystem::StartVoice(FInstance& I, FSCAudioScene& S, const TArray<int32>& Path, double T0, float FadeIn)
{
    const FSCAudioNode& Leaf = S.Manifest.Nodes[Path.Last()];
    const auto* Bytes = S.MediaBytes.Find(Leaf.Media);
    const FSCAudioMedia* Media = S.Manifest.Media.Find(Leaf.Media);
    if (!Leaf.Media || !Bytes || !Media || Media->Seconds <= 0) return T0;
    FVoice V;
    V.Leaf = Path.Last();
    V.bSpatial = Leaf.bSpatial && !I.Place.bListener;
    V.bAux = Leaf.bUseGameAux;
    V.Bus = Leaf.Bus;
    V.Attenuation = S.Manifest.Attenuations.Find(Leaf.Attenuation);
    // Static gains: the context and every node's own values, their random draws and their curves at fixed RTPCs;
    // curves on the area-fade and listener-relative RTPCs change while playing.
    FSCAudioGain Gain;
    const FVector Here = PlacePosition(I);
    auto IsDynamic = [&](const FString& R) { return R == kAzimuth || R == kListenerCone || (!I.Place.AreaFadeRtpc.IsEmpty() && R == I.Place.AreaFadeRtpc); };
    auto AddProps = [&](const FSCAudioProps& P) {
        FSCAudioProps Static = P;
        Static.Rtpc.RemoveAll([&](const FSCAudioRtpcCurve& C) { return IsDynamic(C.Rtpc); });
        Gain.Add(Static, [&](const FString& R) { return RtpcValue(S, I, R, Here); }, &I.Random);
        for (const FSCAudioRtpcCurve& C : P.Rtpc)
            if (IsDynamic(C.Rtpc) && C.Type == 0) V.Dynamic.Add(&C);
    };
    AddProps(I.Action->Context);
    for (int32 Node : Path) AddProps(S.Manifest.Nodes[Node].Props);
    Gain.AddBusVoice(S.Manifest.BusChain(Leaf.Bus));
    float BlendDb = 0.f; // the blend layers' gains on the path (RTPCs at their values when the voice starts)
    for (int32 K = 0; K + 1 < Path.Num(); ++K)
        if (const FSCAudioNode& P = S.Manifest.Nodes[Path[K]]; P.Kind == ESCAudioNodeKind::Blend)
            BlendDb += BlendGainDb(S, I, P, P.Children.IndexOfByKey(Path[K + 1]), Here);
    float BusCents = 0.f;
    for (const FSCAudioBus* B : S.Manifest.BusChain(Leaf.Bus)) BusCents += B->PitchCents;
    V.StaticDb = Gain.VolumeDb + I.Place.VolumeDb + BlendDb;
    V.StaticAux = Gain.GameAuxDb;
    const float PitchRatio = FMath::Pow(2.f, (Gain.PitchCents + BusCents + I.Place.PitchCents) / 1200.f);
    USCAudioWave* Wave = NewObject<USCAudioWave>(this);
    if (!Wave->Init(*Bytes, Leaf.LoopCount, (GLegacy & 1) != 0)) return T0;
    Wave->Pitch = PitchRatio;
    // The scheduled end (container timing, fades): all passes over the loop region at the voice's pitch. The voice
    // itself plays until its audio has (UpdateInstance).
    const double Seconds = Wave->PlaySeconds >= 0 ? Wave->PlaySeconds : Media->Seconds * Leaf.LoopCount;
    V.Start = T0;
    V.FadeInEnd = T0 + FadeIn;
    V.End = Leaf.LoopCount == 0 ? kForever : T0 + Seconds / FMath::Max(PitchRatio, 0.01f);
    V.FadeOutStart = V.FadeOutEnd = V.End;

    if (!Host)
    {
        FActorSpawnParameters P;
        P.ObjectFlags = RF_Transient;
        Host = GetWorld()->SpawnActor<AActor>(P);
        USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
        Host->SetRootComponent(Root);
        Root->RegisterComponent();
    }
    // Dry and wet are two sends of the voice: the base submix output is off, the bus gets the dry send and the room's
    // environment the wet one, each at its own level relative to the voice volume (UpdateInstance).
    V.DrySubmix = BusSubmix(S, Leaf.Bus);
    Wave->SoundSubmixObject = V.DrySubmix;
    Wave->bEnableBaseSubmix = false;
    Wave->bEnableSubmixSends = true;
    UAudioComponent* C = NewObject<UAudioComponent>(Host);
    C->bAutoActivate = false;
    C->bAutoDestroy = false;
    C->bAllowSpatialization = V.bSpatial;
    C->bOverrideAttenuation = true;
    C->AttenuationOverrides.bAttenuate = false;       // the game's curves are applied as gains
    C->AttenuationOverrides.bEnableReverbSend = false; // UE's own reverb is not the game's
    C->AttenuationOverrides.bSpatialize = V.bSpatial;
    C->SetSound(Wave);
    if (USceneComponent* A = I.Place.Attach.Get(); A && !I.Place.bListener)
    {
        C->SetupAttachment(A, I.Place.Socket);
        C->SetRelativeLocation(I.Place.Offset);
    }
    else
        C->SetWorldLocation(Here);
    C->RegisterComponentWithWorld(GetWorld());
    C->SetPitchMultiplier(PitchRatio);
    C->SetVolumeMultiplier(0.f);
    V.Component = C;
    V.Wave = Wave;
    I.Voices.Add(MoveTemp(V));
    return I.Voices.Last().End;
}

void USCAudioSubsystem::Stop(int32 Id, float FadeS, float DelayS)
{
    if (Id == INDEX_NONE) return;
    const double From = Clock + DelayS;
    for (auto& [Key, I] : Instances)
    {
        if (I.Id != Id && I.Group != Id) continue;
        I.Runs.Reset();
        I.bStopping = true;
        I.StopAt = From + FadeS;
        for (FVoice& V : I.Voices)
        {
            V.FadeOutStart = FMath::Min(V.FadeOutStart, From);
            V.FadeOutEnd = FMath::Min(V.FadeOutEnd, From + FadeS);
            V.End = FMath::Min(V.End, From + FadeS);
            V.bStopped = true;
        }
    }
}

bool USCAudioSubsystem::IsPlaying(int32 Id) const
{
    if (Id == INDEX_NONE) return false;
    for (const auto& [Key, I] : Instances)
        if (I.Id == Id || I.Group == Id) return true;
    return false;
}

int32 USCAudioSubsystem::VoiceCount() const
{
    int32 N = 0;
    for (const auto& [Id, I] : Instances) N += I.Voices.Num();
    return N;
}

void USCAudioSubsystem::UpdateInstance(FInstance& I, double Now)
{
    const TSharedPtr<FSCAudioScene> SP = I.Scene.Pin();
    if (!SP) { I.Voices.Reset(); I.Runs.Reset(); return; }
    FSCAudioScene& S = *SP;
    // Continuous containers: start the next item when its time comes.
    for (int32 R = 0; R < I.Runs.Num(); ++R)
    {
        FRun& Run = I.Runs[R];
        if (Now < Run.NextAt || Run.ItemsLeft == 0) continue;
        const int32 NodeIndex = Run.Node;
        const FSCAudioNode& N = S.Manifest.Nodes[NodeIndex];
        const int32 C = SCAudioPick(N, ContainerState(S, NodeIndex, I), I.Random);
        if (C == INDEX_NONE) { Run.ItemsLeft = 0; continue; }
        const bool bCrossfade = N.Transition.StartsWith(TEXT("crossfade"));
        TArray<int32> P = Run.Path;
        P.Add(N.Children[C]);
        const double At = Run.NextAt;
        const int32 Before = I.Voices.Num();
        const double End = Start(I, S, P, At, bCrossfade ? N.TransitionS : 0.f, 0.f);
        const double Next = End >= kForever ? kForever : NextItemAt(N, I, At, End);
        for (int32 V = Before; V < I.Voices.Num(); ++V)
        {
            I.Voices[V].bPower = N.Transition == TEXT("crossfade_power");
            if (bCrossfade && End < kForever)
            {
                I.Voices[V].FadeOutStart = FMath::Min(Next, End);
                I.Voices[V].FadeOutEnd = End;
            }
        }
        if (I.Runs.IsValidIndex(R)) I.Runs[R].NextAt = Next;
    }
    I.Runs.RemoveAll([](const FRun& R) { return R.ItemsLeft == 0 || R.NextAt >= kForever; });
    I.Position = PlacePosition(I);
    if (const FString Env = EnvironmentAt(S, I, I.Position); Env != I.Environment)
    {
        for (FVoice& V : I.Voices)
            if (V.Component && V.bSent)
                if (USoundSubmix* Old = EnvironmentSubmix(S, I.Environment)) V.Component->SetSubmixSend(Old, 0.f);
        I.Environment = Env;
    }
    USoundSubmix* EnvSubmix = I.Environment.IsEmpty() ? nullptr : EnvironmentSubmix(S, I.Environment);
    float AreaOffsetDb = 0.f;
    if (S.Spots.IsValidIndex(I.Place.Spot) && S.Areas.IsValidIndex(I.Place.Area))
    {
        const FSCAudioPlaced& Area = S.Areas[I.Place.Area];
        AreaOffsetDb = float(Num(Area.Params, TEXT("atsVolumeOffset"))) * (Area.bInside ? 1.f : 0.f);
    }
    const double Wall = WallNow();
    for (int32 Vi = I.Voices.Num() - 1; Vi >= 0; --Vi)
    {
        FVoice& V = I.Voices[Vi];
        bool bDone = Now >= V.End;
        if (const USCAudioWave* W = V.Wave.Get(); W && V.End < kForever && !V.bStopped && (GLegacy & 2) == 0)
        {
            // A finite voice ends when its audio has played out, not at its game-clock end: its audio starts a little
            // after Play() (later still after a hitch), so the clock end would cut its last part.
            const FSCAudioWaveHealth& H = W->Health;
            const uint64 Last = H.LastCycles.load(std::memory_order_relaxed);
            const bool bStalled = Last ? Wall - CyclesToSeconds(Last) > 0.5 : H.FirstCycles.load(std::memory_order_relaxed) == 0;
            // Done: played out; or past its end with no mixer source for a while (silent: culled, never started).
            bDone = H.bFinished.load(std::memory_order_relaxed) || (Now >= V.End + 0.5 && bStalled);
        }
        if (bDone)
        {
            RetireVoice(I, V, Now);
            if (V.Component) V.Component->DestroyComponent();
            I.Voices.RemoveAt(Vi);
            continue;
        }
        if (Now < V.Start) continue;
        if (V.Component && !V.Component->IsPlaying())
        {
            if (V.Plays > 0)
            {
                ++Totals.Restarted;
                UE_LOG(LogSCAudio, Display, TEXT("AUDIO_VOICE_RESTART trigger=%s media=%u emitter=\"%s\" plays=%d t=%.2f"), *I.Trigger,
                       S.Manifest.Nodes[V.Leaf].Media, *I.Place.Label, V.Plays + 1, Now);
            }
            else
                V.PlayWall = Wall;
            ++V.Plays;
            V.Component->Play();
        }
        CheckVoice(I, V);
        if (V.Component && !I.Place.Attach.IsValid()) V.Component->SetWorldLocation(I.Position);
        const FVector Emitter = V.Component ? V.Component->GetComponentLocation() : I.Position;
        float Db = V.StaticDb + AreaOffsetDb;
        float AuxDb = V.StaticAux;
        for (const FSCAudioRtpcCurve* C : V.Dynamic)
        {
            const float X = RtpcValue(S, I, C->Rtpc, Emitter);
            if (C->Param == TEXT("game_aux_send_volume")) AuxDb += C->Curve.EvalDb(X);
            else if (C->Param == TEXT("volume") || C->Param == TEXT("make_up_gain") || C->Param == TEXT("output_bus_volume")) Db += C->Curve.EvalDb(X);
        }
        // Distance attenuation applies whenever the node has an attenuation (panned or not), except to a listener bed.
        const bool bDistance = V.Attenuation && !I.Place.bListener;
        const double DistanceM = bDistance ? FVector::Dist(Emitter, ListenerPos) / 100.0 : 0.0;
        const double Scaled = DistanceM / FMath::Max(0.01f, I.Place.AttenuationScale);
        float DryDb = 0.f; // the dry path's own curve
        if (bDistance)
        {
            if (!V.Attenuation->VolumeDry.IsEmpty()) DryDb += V.Attenuation->VolumeDry.EvalDb(Scaled);
            if (!V.Attenuation->VolumeAuxGame.IsEmpty()) AuxDb += V.Attenuation->VolumeAuxGame.EvalDb(Scaled);
        }
        float Fade = 1.f;
        if (V.FadeInEnd > V.Start && Now < V.FadeInEnd)
        {
            const float T = float((Now - V.Start) / (V.FadeInEnd - V.Start));
            Fade *= V.bPower ? FMath::Sin(T * HALF_PI) : T;
        }
        if (Now > V.FadeOutStart && V.FadeOutEnd > V.FadeOutStart)
        {
            const float T = FMath::Clamp(float((Now - V.FadeOutStart) / (V.FadeOutEnd - V.FadeOutStart)), 0.f, 1.f);
            Fade *= V.bPower ? FMath::Cos(T * HALF_PI) : 1.f - T;
        }
        // Shared gain (Db) and the fade go into the voice volume once; the dry curve and the aux gain (relative to the
        // voice, as in Wwise) are the two sends' levels. UE clamps a send to 1, so the voice volume carries the louder
        // of the two and each send its difference to it.
        const bool bWet = V.bAux && EnvSubmix && bSends;
        const float Common = bWet ? FMath::Max(DryDb, AuxDb) : DryDb;
        V.LastGain = Db + DryDb;
        V.LastDistance = float(DistanceM);
        const bool bSceneMuted = MutedScenes.ContainsByPredicate([&](const FString& M) { return S.Name.Contains(M); });
        const float Linear = bMuted || bSceneMuted ? 0.f : FSCAudioGain::Linear(Db + Common) * Fade;
        V.MinLinear = FMath::Min(V.MinLinear, Linear);
        V.LastLinear = Linear;
        if (V.Component)
        {
            V.Component->SetVolumeMultiplier(Linear);
            if (V.DrySubmix) V.Component->SetSubmixSend(V.DrySubmix, FSCAudioGain::Linear(DryDb - Common));
            if (bWet || (V.bSent && EnvSubmix))
            {
                // With the sends off (sc.Audio.Sends 0) a voice that never sent gets no send at all (a clean dry path).
                V.LastSend = bWet ? Db + AuxDb : 0.f;
                V.bSent |= bWet;
                V.Component->SetSubmixSend(EnvSubmix, bWet ? FSCAudioGain::Linear(AuxDb - Common) : 0.f);
            }
        }
    }
}

void USCAudioSubsystem::UpdateAreasAndSpots(FSCAudioScene& S, double Now)
{
    // Areas: inside -> fade 1, the bed at the listener; outside within fadeDistance -> fade falls to 0 and the bed sits
    // at the nearest point of the area (the CryEngine area-ambience behaviour the brief describes; the bank's curve on
    // the area-fade RTPC shapes the level).
    for (int32 A = 0; A < S.Areas.Num(); ++A)
    {
        FSCAudioPlaced& Area = S.Areas[A];
        const FVector L = S.WorldToContainer(Area.Interior, ListenerPos);
        double Best = TNumericLimits<double>::Max();
        FVector Nearest = L;
        bool bInside = false;
        for (const FSCRoomVolume& Shape : Area.Shapes)
        {
            if (Shape.Contains(L)) { bInside = true; Best = 0; Nearest = L; break; }
            FVector N;
            if (const double D = ShapeDistance(Shape, L, N); D < Best) { Best = D; Nearest = N; }
        }
        const double FadeDistance = Num(Area.Params, TEXT("fadeDistance")); // the class default when the instance has none
        Area.bInside = bInside;
        Area.Fade = bInside ? 1.f : FadeDistance > 0 ? float(FMath::Clamp(1.0 - Best / FadeDistance, 0.0, 1.0)) : 0.f;
        if (bLog && (FMath::Abs(Area.Fade - Area.LoggedFade) >= 0.1f || (Area.Fade == 0.f) != (Area.LoggedFade == 0.f) ||
                     (Area.Fade == 1.f) != (Area.LoggedFade == 1.f)))
        {
            Area.LoggedFade = Area.Fade;
            UE_LOG(LogSCAudio, Display, TEXT("AUDIO_AREA name=%s trigger=%s fade=%.2f inside=%d dist_m=%.2f fade_distance_m=%.1f t=%.2f"), *Area.Name,
                   *Area.PlayTrigger, Area.Fade, bInside, bInside ? 0.0 : Best, FadeDistance, Now);
        }
        const bool bWant = bAmbience && Area.Fade > 0.f && !Area.PlayTrigger.IsEmpty();
        if (bWant && !IsPlaying(Area.Instance))
        {
            FSCAudioPlace P;
            P.bListener = bInside;
            P.Interior = Area.Interior;
            P.ContainerPos = Nearest;
            P.VolumeDb = float(Num(Area.Params, TEXT("volume")));
            P.PitchCents = float(Num(Area.Params, TEXT("pitch")));
            P.Area = A;
            P.AreaFadeRtpc = Str(Area.Params, TEXT("areaFadeRTPC.rtpc"));
            P.Label = Area.Name;
            Area.Instance = Post(S, Area.PlayTrigger, P);
        }
        else if (!bWant && IsPlaying(Area.Instance))
        {
            Stop(Area.Instance, 0.1f);
            Area.Instance = INDEX_NONE;
        }
        for (auto& [Key, I] : Instances)
            if (Area.Instance != INDEX_NONE && I.Group == Area.Instance)
            {
                SetSpatial(I, bInside);
                I.Place.ContainerPos = Nearest;
            }
    }
    // Spots: voiced while the listener is within reach of their nearest position (and their area, if any, is heard).
    for (int32 Si = 0; Si < S.Spots.Num(); ++Si)
    {
        FSCAudioPlaced& Spot = S.Spots[Si];
        const FSCAudioTrigger* T = S.Manifest.Triggers.Find(Spot.PlayTrigger);
        if (!T || !T->IsPlayable()) continue;
        const FVector L = S.WorldToContainer(Spot.Interior, ListenerPos);
        int32 Best = 0;
        for (int32 P = 1; P < Spot.Positions.Num(); ++P)
            if (FVector::DistSquared(Spot.Positions[P], L) < FVector::DistSquared(Spot.Positions[Best], L)) Best = P;
        const float Scale = float(Num(Spot.Params, TEXT("attenuationScalingFactor"), 1.0));
        double Reach = T->RadiusMax;
        for (const FSCAudioAction& A : T->Play)
            if (const FSCAudioAttenuation* At = S.Manifest.Attenuations.Find(S.Manifest.Nodes[A.Root].Attenuation)) Reach = FMath::Max(Reach, double(At->MaxDistance));
        const bool bAreaHeard = !S.Areas.IsValidIndex(Spot.Area) || S.Areas[Spot.Area].Fade > 0.f;
        const bool bNear = FVector::Dist(Spot.Positions[Best], L) <= Reach * Scale * 1.05 && bAreaHeard && bAmbience && Flag(Spot.Params, TEXT("enabled"), true);
        const bool bRandom = Flag(Spot.Params, TEXT("playRandom"));
        auto Place = [&](FVector At) {
            FSCAudioPlace P;
            if (bRandom)
            {
                // playRandom spots play each time within radiusRandom along the enabled axes.
                const float R = float(Num(Spot.Params, TEXT("radiusRandom")));
                At += FVector(Flag(Spot.Params, TEXT("playOnX")) ? FMath::FRandRange(-R, R) : 0.f, Flag(Spot.Params, TEXT("playOnY")) ? FMath::FRandRange(-R, R) : 0.f,
                              Flag(Spot.Params, TEXT("playOnZ")) ? FMath::FRandRange(-R, R) : 0.f);
            }
            P.Interior = Spot.Interior;
            P.ContainerPos = At;
            P.Room = Spot.Room;
            const float VR = float(Num(Spot.Params, TEXT("volumeRandomRange"))), PR = float(Num(Spot.Params, TEXT("pitchRandomRange")));
            P.VolumeDb = float(Num(Spot.Params, TEXT("volume"))) + (VR > 0 ? FMath::FRandRange(-VR, VR) : 0.f);
            P.PitchCents = float(Num(Spot.Params, TEXT("pitch"))) + (PR > 0 ? FMath::FRandRange(-PR, PR) : 0.f);
            P.AttenuationScale = Scale;
            P.Spot = Si;
            P.Area = Spot.Area;
            P.AreaFadeRtpc = Str(Spot.Params, TEXT("audioRTPCAreaFadeRtpc.rtpc"));
            P.Label = Spot.Name;
            return P;
        };
        if (bRandom)
        {
            if (bNear && Now >= Spot.NextPlay)
            {
                Spot.Instance = Post(S, Spot.PlayTrigger, Place(Spot.Positions[Best]));
                const double Delay = FMath::FRandRange(Num(Spot.Params, TEXT("minDelay")), Num(Spot.Params, TEXT("maxDelay")));
                double Length = 0;
                for (const auto& [Key, I] : Instances)
                    if (Spot.Instance != INDEX_NONE && I.Group == Spot.Instance)
                        for (const FVoice& V : I.Voices) Length = FMath::Max(Length, FMath::Min(V.End, kForever) - Now);
                Spot.NextPlay = Now + Delay + (Flag(Spot.Params, TEXT("delayIsFromEnd")) ? Length : 0.0);
            }
            continue;
        }
        // A looping spot plays while in reach; a one-shot spot plays once each time the listener comes within reach.
        const bool bInfinite = T->DurationType == TEXT("Infinite");
        if (bNear && !IsPlaying(Spot.Instance) && !Spot.bFired)
        {
            Spot.Instance = Post(S, Spot.PlayTrigger, Place(Spot.Positions[Best]));
            Spot.bFired = !bInfinite;
        }
        else if (!bNear && IsPlaying(Spot.Instance) && bInfinite)
        {
            if (!Spot.StopTrigger.IsEmpty()) Post(S, Spot.StopTrigger, Place(Spot.Positions[Best]));
            Stop(Spot.Instance, 0.2f);
            Spot.Instance = INDEX_NONE;
        }
        if (!bNear) Spot.bFired = false;
        // Single-source multi-position spots follow the listener's nearest position.
        if (Spot.Positions.Num() > 1)
            for (auto& [Key, I] : Instances)
                if (Spot.Instance != INDEX_NONE && I.Group == Spot.Instance) I.Place.ContainerPos = Spot.Positions[Best];
    }
}

void USCAudioSubsystem::CheckVoice(const FInstance& I, FVoice& V)
{
    const USCAudioWave* W = V.Wave.Get();
    if (!W) return;
    const FSCAudioWaveHealth& H = W->Health;
    const TSharedPtr<FSCAudioScene> S = I.Scene.Pin();
    const uint32 Media = S && S->Manifest.Nodes.IsValidIndex(V.Leaf) ? S->Manifest.Nodes[V.Leaf].Media : 0;
    if (const int32 Gaps = H.Gaps.load(std::memory_order_relaxed); Gaps > V.LoggedGaps)
    {
        V.LoggedGaps = Gaps;
        UE_LOG(LogSCAudio, Display, TEXT("AUDIO_VOICE_GAP trigger=%s media=%u emitter=\"%s\" gaps=%d max_gap_ms=%.1f t=%.2f"), *I.Trigger, Media, *I.Place.Label,
               Gaps, H.MaxGapUs.load(std::memory_order_relaxed) / 1000.0, Clock);
    }
    if (const int32 Sources = H.Sources.load(std::memory_order_relaxed); Sources > V.LoggedSources)
    {
        V.LoggedSources = Sources;
        // The volume this voice was given since its previous source: a source dropped while the volume stayed well
        // above the engine's audibility floor was taken by the engine, not culled for silence.
        auto Db = [](float L) { return L > 1e-10f ? 20.f * FMath::LogX(10.f, L) : -200.f; };
        UE_LOG(LogSCAudio, Display, TEXT("AUDIO_VOICE_RESOURCED trigger=%s media=%u emitter=\"%s\" sources=%d silence_ms=%.1f gain_db=%.1f volume_db=%.1f min_volume_db=%.1f wave=%s t=%.2f"),
               *I.Trigger, Media, *I.Place.Label, Sources, H.MaxResourceGapUs.load(std::memory_order_relaxed) / 1000.0, V.LastGain, Db(V.LastLinear),
               Db(V.MinLinear), *W->GetName(), Clock);
        V.MinLinear = 1e9f;
    }
}

void USCAudioSubsystem::RetireVoice(const FInstance& I, FVoice& V, double Now)
{
    CheckVoice(I, V);
    const USCAudioWave* W = V.Wave.Get();
    ++Totals.Voices;
    if (!W) return;
    const FSCAudioWaveHealth& H = W->Health;
    Totals.Calls += H.Calls.load(std::memory_order_relaxed);
    Totals.Frames += H.Frames.load(std::memory_order_relaxed);
    Totals.DecodeNs += H.DecodeNs.load(std::memory_order_relaxed);
    Totals.Gaps += H.Gaps.load(std::memory_order_relaxed);
    Totals.MaxGapUs = FMath::Max(Totals.MaxGapUs, H.MaxGapUs.load(std::memory_order_relaxed));
    Totals.MaxDecodeUs = FMath::Max(Totals.MaxDecodeUs, H.MaxDecodeUs.load(std::memory_order_relaxed));
    Totals.Resourced += FMath::Max(0, H.Sources.load(std::memory_order_relaxed) - 1);
    const uint64 First = H.FirstCycles.load(std::memory_order_relaxed);
    if (!First)
    {
        ++Totals.Unrendered;
        return;
    }
    if (V.PlayWall > 0)
    {
        const double Latency = CyclesToSeconds(First) - V.PlayWall;
        Totals.SumStartLatency += Latency;
        Totals.MaxStartLatency = FMath::Max(Totals.MaxStartLatency, Latency);
        ++Totals.StartLatencies;
    }
    // A finite voice removed before its audio played out (not by a stop) lost its last part.
    if (!V.bStopped && W->PlaySeconds >= 0 && !H.bFinished.load(std::memory_order_relaxed) && W->Rate > 0)
    {
        const double Left = W->PlaySeconds - double(H.Frames.load(std::memory_order_relaxed)) / W->Rate;
        if (Left > 0.001)
        {
            // Audible when it ended: its volume above the engine's floor for a voice (UE_KINDA_SMALL_NUMBER).
            const bool bAudible = V.LastLinear > UE_KINDA_SMALL_NUMBER;
            ++Totals.Cut;
            Totals.CutAudible += bAudible;
            Totals.CutSeconds += Left;
            const TSharedPtr<FSCAudioScene> S = I.Scene.Pin();
            UE_LOG(LogSCAudio, Display, TEXT("AUDIO_VOICE_CUT trigger=%s media=%u emitter=\"%s\" left_ms=%.1f gain_db=%.1f audible=%d t=%.2f"), *I.Trigger,
                   S && S->Manifest.Nodes.IsValidIndex(V.Leaf) ? S->Manifest.Nodes[V.Leaf].Media : 0u, *I.Place.Label, Left * 1000.0, V.LastGain, bAudible, Now);
        }
    }
}

void USCAudioSubsystem::LogHealth(bool bReset)
{
    // The ended voices' totals plus the voices playing now.
    FHealthTotals T = Totals;
    int32 Live = 0;
    for (const auto& [Id, I] : Instances)
        for (const FVoice& V : I.Voices)
            if (const USCAudioWave* W = V.Wave.Get())
            {
                ++Live;
                const FSCAudioWaveHealth& H = W->Health;
                T.Calls += H.Calls.load(std::memory_order_relaxed);
                T.Frames += H.Frames.load(std::memory_order_relaxed);
                T.DecodeNs += H.DecodeNs.load(std::memory_order_relaxed);
                T.Gaps += H.Gaps.load(std::memory_order_relaxed);
                T.MaxGapUs = FMath::Max(T.MaxGapUs, H.MaxGapUs.load(std::memory_order_relaxed));
                T.MaxDecodeUs = FMath::Max(T.MaxDecodeUs, H.MaxDecodeUs.load(std::memory_order_relaxed));
                T.Resourced += FMath::Max(0, H.Sources.load(std::memory_order_relaxed) - 1);
            }
    const double Window = FMath::Max(1e-3, WallNow() - Totals.Since);
    // The engine's voice limit (the audio quality level's max channels unless overridden): voices past it are culled.
    const FAudioDeviceHandle Device = GetWorld() ? GetWorld()->GetAudioDevice() : FAudioDeviceHandle();
    const int32 MaxChannels = Device ? Device->GetMaxChannels() : 0;
    // voices_rendering: the realtime-equivalent voices the mixer pulled (frames at 48 kHz over the window).
    UE_LOG(LogSCAudio, Display,
           TEXT("AUDIO_HEALTH window_s=%.1f legacy=%d voices_ended=%lld voices_live=%d pulls=%lld audio_frames=%lld gaps=%d max_gap_ms=%.1f resourced=%d restarted=%d cut=%d cut_audible=%d cut_s=%.2f unrendered=%d start_latency_ms_avg=%.1f start_latency_ms_max=%.1f decode_ms_per_s=%.2f decode_max_us=%d max_channels=%d voices_rendering=%.1f"),
           Window, GLegacy, T.Voices, Live, T.Calls, T.Frames, T.Gaps, T.MaxGapUs / 1000.0, T.Resourced, T.Restarted, T.Cut, T.CutAudible, T.CutSeconds, T.Unrendered,
           T.StartLatencies ? T.SumStartLatency / T.StartLatencies * 1000.0 : 0.0, T.MaxStartLatency * 1000.0, T.DecodeNs / 1e6 / Window, T.MaxDecodeUs, MaxChannels,
           T.Frames / 48000.0 / Window);
    if (bReset)
    {
        Totals = FHealthTotals();
        Totals.Since = WallNow();
        // Live voices start the new window from zero counters (their source count from one).
        for (auto& [Id, I] : Instances)
            for (FVoice& V : I.Voices)
                if (USCAudioWave* W = V.Wave.Get())
                {
                    FSCAudioWaveHealth& H = W->Health;
                    H.Calls = 0;
                    H.Frames = 0;
                    H.DecodeNs = 0;
                    H.Gaps = 0;
                    H.MaxGapUs = 0;
                    H.MaxDecodeUs = 0;
                    V.LoggedGaps = 0;
                    H.Sources = FMath::Min(H.Sources.load(), 1);
                    V.LoggedSources = FMath::Max(1, H.Sources.load());
                }
    }
}

void USCAudioSubsystem::ScheduleHitches(float Ms, int32 Count, float EveryS)
{
    HitchMs = FMath::Clamp(Ms, 0.f, 5000.f);
    HitchesLeft = FMath::Max(0, Count);
    HitchEveryS = FMath::Max(0.f, EveryS);
    NextHitch = WallNow();
    UE_LOG(LogSCAudio, Display, TEXT("AUDIO_HITCH scheduled ms=%.0f count=%d every_s=%.2f"), HitchMs, HitchesLeft, HitchEveryS);
}

void USCAudioSubsystem::Tick(float DeltaTime)
{
    if (HitchesLeft > 0 && WallNow() >= NextHitch)
    {
        // A measured game-thread stall (the frame-time spike a heavy frame or a load causes).
        FPlatformProcess::SleepNoStats(HitchMs / 1000.f);
        --HitchesLeft;
        NextHitch = WallNow() + HitchEveryS;
    }
    if (Totals.Since == 0) Totals.Since = WallNow();
    Clock += DeltaTime;
    UWorld* World = GetWorld();
    if (!World || Scenes.IsEmpty()) return;
    if (Scenes.ContainsByPredicate([](const TSharedRef<FSCAudioScene>& S) { return !S->Vehicle.IsValid(); })) RemoveScene(nullptr);
    if (APlayerController* PC = World->GetFirstPlayerController())
        PC->GetAudioListenerPosition(ListenerPos, ListenerFwd, ListenerRight);
    for (const TSharedRef<FSCAudioScene>& S : TArray<TSharedRef<FSCAudioScene>>(Scenes))
    {
        if (!S->Vehicle.IsValid()) continue;
        UpdateAreasAndSpots(*S, Clock);
        if (const ASCVehicle* V = S->Vehicle.Get(); V && V->IsInsideLightArea(ListenerPos))
        {
            ListenerRoom = V->LightRoomAt(ListenerPos).ToLower();
            ListenerEnvironment = S->Manifest.RoomEnvironment(ListenerRoom);
            if (ListenerRoom != LastLoggedRoom)
            {
                LastLoggedRoom = ListenerRoom;
                const FSCAudioRoom* R = S->Manifest.Rooms.Find(ListenerRoom);
                const FSCAudioEnvironment* E = S->Manifest.Environments.Find(ListenerEnvironment);
                const FSCAudioBus* B = E ? S->Manifest.Buses.Find(E->AuxBus) : nullptr;
                UE_LOG(LogSCAudio, Display, TEXT("AUDIO_ROOM room=%s name=%s volume_m3=%.0f size_m=%.2f env=%s ir_t20=%.3f ir_t30=%.3f ir_peak_db=%.1f wet_db=%.1f dry_db=%.1f aux_bus_mix_db=%.1f rule=\"%s\""),
                       *ListenerRoom, R ? *R->Name : TEXT("-"), R ? R->VolumeM3 : 0.f, R ? R->SizeM : 0.f, ListenerEnvironment.IsEmpty() ? TEXT("-") : *ListenerEnvironment,
                       E ? E->T20 : 0.f, E ? E->T30 : 0.f, E ? E->IrPeakDb : 0.f, B ? B->WetDb : 0.f, B ? B->DryDb : 0.f, B ? B->MixDb() : 0.f, R ? *R->Rule : TEXT(""));
            }
        }
    }
    for (auto It = Instances.CreateIterator(); It; ++It)
    {
        UpdateInstance(It->Value, Clock);
        if (It->Value.Voices.IsEmpty() && It->Value.Runs.IsEmpty() && (It->Value.bStopping || Clock > It->Value.StopAt)) It.RemoveCurrent();
    }
}

// ------------------------------------------------------------------------------------------------ viewer events
int32 USCAudioSubsystem::FireEntityTag(ASCVehicle* Vehicle, int32 Interior, const FString& EntityId, const FString& Tag)
{
    const TSharedPtr<FSCAudioScene> S = SceneOf(Vehicle);
    if (!S) return 0;
    const FSCAudioEntity* E = S->Entities.Find(FString::Printf(TEXT("%d/%s"), Interior, *EntityId.ToLower()));
    const FSCAudioClass* C = E ? S->Manifest.Classes.Find(E->ClassGuid) : nullptr;
    if (!C) return 0;
    int32 Played = 0;
    for (const FSCAudioEffect* Fx : C->EffectsFor(Tag, TEXT("trigger")))
    {
        FSCAudioPlace P;
        P.Attach = E->Rig.IsValid() ? static_cast<USceneComponent*>(E->Rig.Get()) : E->Anchor.Get();
        if (USCRigComponent* Rig = E->Rig.Get(); Rig && !Fx->Bone.IsEmpty() && Rig->GetBoneIndex(FName(*Fx->Bone)) != INDEX_NONE) P.Socket = FName(*Fx->Bone);
        P.Offset = FSCLightImporter::ContainerBasis(Fx->Offset, false) * 100;
        P.Label = C->Name + TEXT(":") + Tag;
        Played += Post(*S, Fx->Trigger, P) != INDEX_NONE;
    }
    return Played;
}

void USCAudioSubsystem::SetEntityTag(ASCVehicle* Vehicle, int32 Interior, const FString& EntityId, const FString& Tag, bool bSet)
{
    const TSharedPtr<FSCAudioScene> S = SceneOf(Vehicle);
    if (!S) return;
    FSCAudioEntity* E = S->Entities.Find(FString::Printf(TEXT("%d/%s"), Interior, *EntityId.ToLower()));
    const FSCAudioClass* C = E ? S->Manifest.Classes.Find(E->ClassGuid) : nullptr;
    if (!C) return;
    if (!bSet)
    {
        if (const TArray<int32>* Ids = E->TagInstances.Find(Tag))
            for (const int32 Id : *Ids) Stop(Id, 0.1f);
        E->TagInstances.Remove(Tag);
        return;
    }
    if (const TArray<int32>* Ids = E->TagInstances.Find(Tag); Ids && Ids->ContainsByPredicate([&](int32 Id) { return IsPlaying(Id); })) return;
    E->TagInstances.Remove(Tag);
    for (const FSCAudioEffect* Fx : C->EffectsFor(Tag, TEXT("tag")))
    {
        FSCAudioPlace P;
        P.Attach = E->Rig.IsValid() ? static_cast<USceneComponent*>(E->Rig.Get()) : E->Anchor.Get();
        if (USCRigComponent* Rig = E->Rig.Get(); Rig && !Fx->Bone.IsEmpty() && Rig->GetBoneIndex(FName(*Fx->Bone)) != INDEX_NONE) P.Socket = FName(*Fx->Bone);
        P.Offset = FSCLightImporter::ContainerBasis(Fx->Offset, false) * 100;
        P.Label = C->Name + TEXT(":") + Tag;
        const int32 Id = Post(*S, Fx->Trigger, P);
        if (Id != INDEX_NONE) E->TagInstances.FindOrAdd(Tag).Add(Id);
    }
}

int32 USCAudioSubsystem::PlayItemAction(ASCVehicle* Kit, const FString& ItemEntity, const FString& Field, USceneComponent* Attach)
{
    const TSharedPtr<FSCAudioScene> S = SceneOf(Kit);
    if (!S || ItemEntity.IsEmpty()) return INDEX_NONE;
    const FString Qualified = TEXT("EntityClassDefinition.") + ItemEntity;
    for (const auto& [Guid, C] : S->Manifest.Classes)
    {
        if (C.Name != ItemEntity && C.Name != Qualified) continue;
        for (const auto& [Path, Trigger] : C.Actions)
            if (Path == Field || Path.EndsWith(TEXT(".") + Field))
            {
                FSCAudioPlace P;
                P.Attach = Attach;
                P.Label = ItemEntity + TEXT(":") + Field;
                return Post(*S, Trigger, P);
            }
    }
    if (bLog) UE_LOG(LogSCAudio, Display, TEXT("AUDIO_SKIP item=%s field=%s reason=\"the item class has no such audio field\""), *ItemEntity, *Field);
    return INDEX_NONE;
}

int32 USCAudioSubsystem::PlayClipEvent(ASCVehicle* Vehicle, USCRigComponent* Rig, const FString& Trigger, const FString& Bone, const FVector& OffsetCry)
{
    const TSharedPtr<FSCAudioScene> S = SceneOf(Vehicle);
    if (!S || !Rig) return INDEX_NONE;
    FSCAudioPlace P;
    P.Attach = Rig;
    if (!Bone.IsEmpty() && Rig->GetBoneIndex(FName(*Bone)) != INDEX_NONE) P.Socket = FName(*Bone);
    P.Offset = FSCLightImporter::ContainerBasis(OffsetCry, false) * 100;
    P.Label = Rig->OwnerEntity + TEXT(":") + Bone;
    return Post(*S, Trigger, P);
}

void USCAudioSubsystem::StopAll()
{
    TArray<int32> Ids;
    Instances.GetKeys(Ids);
    for (int32 Id : Ids) Stop(Id, 0.05f);
}

void USCAudioSubsystem::LogRooms() const
{
    for (const auto& S : Scenes)
        for (const auto& [Guid, R] : S->Manifest.Rooms)
        {
            const FSCAudioEnvironment* E = S->Manifest.Environments.Find(R.Environment);
            const FSCAudioBus* B = E ? S->Manifest.Buses.Find(E->AuxBus) : nullptr;
            UE_LOG(LogSCAudio, Display, TEXT("AUDIO_ROOMS %s room=%s name=%s volume_m3=%.0f size_m=%.2f interiority=%.2f env=%s ir_t20=%.3f ir_t30=%.3f wet_db=%.1f dry_db=%.1f aux_bus_mix_db=%.1f"),
                   *S->Name, *Guid, *R.Name, R.VolumeM3, R.SizeM, R.Interiority, R.Environment.IsEmpty() ? TEXT("-") : *R.Environment, E ? E->T20 : 0.f,
                   E ? E->T30 : 0.f, B ? B->WetDb : 0.f, B ? B->DryDb : 0.f, B ? B->MixDb() : 0.f);
        }
}

FString USCAudioSubsystem::Describe() const
{
    FString Out = FString::Printf(TEXT("scenes=%d instances=%d voices=%d muted=%d listener_room=%s env=%s\n"), Scenes.Num(), Instances.Num(), VoiceCount(), bMuted,
                                  *ListenerRoom, *ListenerEnvironment);
    for (const auto& [Id, I] : Instances)
        for (const FVoice& V : I.Voices)
            Out += FString::Printf(TEXT("  #%d %s \"%s\" dist_m=%.1f gain_db=%.1f send_db=%.1f env=%s bus=%u\n"), Id, *I.Trigger, *I.Place.Label, V.LastDistance, V.LastGain,
                                   V.LastSend, *I.Environment, V.Bus);
    return Out;
}

// ------------------------------------------------------------------------------------------------ console
namespace
{
USCAudioSubsystem* ConsoleTarget(UWorld* World)
{
    return World ? World->GetSubsystem<USCAudioSubsystem>() : nullptr;
}
FAutoConsoleCommandWithWorldAndArgs GAudioList(TEXT("sc.Audio.List"), TEXT("List playing audio voices (trigger, emitter, distance, gain, reverb send, environment, bus)."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World) {
        if (USCAudioSubsystem* A = ConsoleTarget(World)) UE_LOG(LogSCAudio, Display, TEXT("AUDIO_LIST %s"), *A->Describe());
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioMute(TEXT("sc.Audio.Mute"), TEXT("sc.Audio.Mute 0|1: silence every SC voice (the logic keeps running)."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCAudioSubsystem* A = ConsoleTarget(World)) A->bMuted = Args.IsEmpty() ? !A->bMuted : Args[0] != TEXT("0");
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioAmbience(TEXT("sc.Audio.Ambience"), TEXT("sc.Audio.Ambience 0|1: placed spots and areas on or off (event sounds still play)."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCAudioSubsystem* A = ConsoleTarget(World)) A->bAmbience = Args.IsEmpty() ? !A->bAmbience : Args[0] != TEXT("0");
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioSends(TEXT("sc.Audio.Sends"), TEXT("sc.Audio.Sends 0|1: aux (reverb) sends on or off (dry / wet measurements)."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCAudioSubsystem* A = ConsoleTarget(World))
        {
            A->bSends = Args.IsEmpty() ? !A->bSends : Args[0] != TEXT("0");
            UE_LOG(LogSCAudio, Display, TEXT("AUDIO_SENDS on=%d"), A->bSends);
        }
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioMeter(TEXT("sc.Audio.Meter"),
    TEXT("sc.Audio.Meter start | stop: measure every bus submix's output; stop logs AUDIO_METER (peak, RMS) per bus."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCAudioSubsystem* A = ConsoleTarget(World))
        {
            if (!Args.IsEmpty() && Args[0] == TEXT("stop")) A->StopMeter();
            else A->StartMeter();
        }
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioUnfocused(TEXT("sc.Audio.Unfocused"),
    TEXT("sc.Audio.Unfocused <0..1>: the volume while the window has no focus (UE mutes then by default; off-screen runs need 1)."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld*) {
        FApp::SetUnfocusedVolumeMultiplier(Args.IsEmpty() ? 1.f : FCString::Atof(*Args[0]));
        UE_LOG(LogSCAudio, Display, TEXT("AUDIO_UNFOCUSED volume=%.2f"), FApp::GetUnfocusedVolumeMultiplier());
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioLog(TEXT("sc.Audio.Log"), TEXT("sc.Audio.Log 0|1: AUDIO_PLAY lines on or off."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCAudioSubsystem* A = ConsoleTarget(World)) A->bLog = Args.IsEmpty() ? !A->bLog : Args[0] != TEXT("0");
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioPlay(TEXT("sc.Audio.Play"), TEXT("sc.Audio.Play <trigger> [area]: post a trigger of the first scene that has it, 2 m in front of the listener; `area`: as the bed of the area the listener stands in."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCAudioSubsystem* A = ConsoleTarget(World);
        if (!A || Args.IsEmpty()) return;
        for (TActorIterator<ASCVehicle> It(World); It; ++It)
            if (TSharedPtr<FSCAudioScene> S = A->SceneOf(*It); S && S->Manifest.Triggers.Contains(Args[0]))
            {
                FSCAudioPlace P;
                P.World = A->ListenerPos + A->ListenerFwd * 200;
                P.Label = TEXT("console");
                // `area`: as the bed of the area the listener stands in (its area-fade RTPC, volume and pitch), so an
                // area bed can be heard alone (sc.Audio.Ambience 0).
                if (Args.Num() > 1 && Args[1] == TEXT("area"))
                    for (int32 Ai = 0; Ai < S->Areas.Num(); ++Ai)
                        if (const FSCAudioPlaced& Area = S->Areas[Ai]; Area.bInside)
                        {
                            P.bListener = true;
                            P.Interior = Area.Interior;
                            P.Area = Ai;
                            P.AreaFadeRtpc = Str(Area.Params, TEXT("areaFadeRTPC.rtpc"));
                            P.VolumeDb = float(Num(Area.Params, TEXT("volume")));
                            P.PitchCents = float(Num(Area.Params, TEXT("pitch")));
                            P.Label = TEXT("console ") + Area.Name;
                            break;
                        }
                A->Post(*S, Args[0], P);
                return;
            }
        UE_LOG(LogSCAudio, Warning, TEXT("sc.Audio.Play: no scene has trigger %s"), *Args[0]);
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioStop(TEXT("sc.Audio.Stop"), TEXT("sc.Audio.Stop: stop every playing instance (spots and areas restart when in reach)."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World) {
        if (USCAudioSubsystem* A = ConsoleTarget(World)) A->StopAll();
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioHealth(TEXT("sc.Audio.Health"),
    TEXT("sc.Audio.Health [reset]: log AUDIO_HEALTH (render-side voice health since the last reset: gaps, source changes, restarts, cut voices, start latency, decode load); reset starts a new window."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCAudioSubsystem* A = ConsoleTarget(World)) A->LogHealth(!Args.IsEmpty() && Args[0] == TEXT("reset"));
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioHitch(TEXT("sc.Audio.Hitch"),
    TEXT("sc.Audio.Hitch <ms> [count=1] [every_s=1]: stall the game thread (measurements of playback under frame-time spikes)."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCAudioSubsystem* A = ConsoleTarget(World); A && !Args.IsEmpty())
            A->ScheduleHitches(FCString::Atof(*Args[0]), Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 1, Args.Num() > 2 ? FCString::Atof(*Args[2]) : 1.f);
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioLimiterTrace(TEXT("sc.Audio.LimiterTrace"),
    TEXT("sc.Audio.LimiterTrace start | stop [csv path]: trace the master bus output (after its limiter) and the main output per 10 ms (peaks, samples over full scale); stop logs AUDIO_LIMITER_TRACE."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCAudioSubsystem* A = ConsoleTarget(World))
        {
            if (!Args.IsEmpty() && Args[0] == TEXT("stop")) A->StopLimiterTrace(Args.Num() > 1 ? Args[1] : FString());
            else A->StartLimiterTrace();
        }
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioLimiter(TEXT("sc.Audio.Limiter"), TEXT("sc.Audio.Limiter 0|1|2: the bus limiters off (bypassed), on, or on with a fast attack (A/B measurements)."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (USCAudioSubsystem* A = ConsoleTarget(World)) A->SetLimiterMode(Args.IsEmpty() ? 1 : FCString::Atoi(*Args[0]));
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioMuteScene(TEXT("sc.Audio.MuteScene"),
    TEXT("sc.Audio.MuteScene <name part> 0|1: silence (1) or restore (0) the voices of the scenes whose name contains it (A/B measurements)."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCAudioSubsystem* A = ConsoleTarget(World);
        if (!A || Args.IsEmpty()) return;
        if (Args.Num() > 1 && Args[1] == TEXT("0")) A->MutedScenes.Remove(Args[0]);
        else A->MutedScenes.AddUnique(Args[0]);
        UE_LOG(LogSCAudio, Display, TEXT("AUDIO_MUTE_SCENE muted=%s"), *FString::Join(A->MutedScenes, TEXT(",")));
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioRooms(TEXT("sc.Audio.Rooms"), TEXT("Log every room's audio environment and its reverb parameters."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World) {
        if (USCAudioSubsystem* A = ConsoleTarget(World)) A->LogRooms();
    }));
FAutoConsoleCommandWithWorldAndArgs GAudioRecord(TEXT("sc.Audio.Record"),
    TEXT("sc.Audio.Record start [seconds] | stop <absolute path .wav>: record the main submix output (evidence captures)."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        if (!World || Args.IsEmpty()) return;
        if (Args[0] == TEXT("start"))
            UAudioMixerBlueprintLibrary::StartRecordingOutput(World, Args.Num() > 1 ? FCString::Atof(*Args[1]) : 60.f);
        else if (Args[0] == TEXT("stop") && Args.Num() > 1)
        {
            const FString File = Args[1];
            UAudioMixerBlueprintLibrary::StopRecordingOutputToAnyPath(World, EAudioRecordingExportType::WavFile, FPaths::GetBaseFilename(File), FPaths::GetPath(File));
            UE_LOG(LogSCAudio, Display, TEXT("AUDIO_RECORD written=%s"), *File);
        }
    }));
} // namespace
