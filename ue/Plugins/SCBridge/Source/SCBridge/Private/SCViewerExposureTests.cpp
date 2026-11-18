// SCBridge.ViewerExposure.*: the game's exposure chain expressed as UE eye-adaptation settings.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCViewerExposure.h"
#include "SCStarEngine.h"
#include "Curves/CurveFloat.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Math/Float16.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SceneView.h"
#include "ShowFlags.h"
#include "Engine/Texture2D.h"
#include "AutoExposureConversion.h"

namespace
{
// Visibly synthetic renderer (not the game's): round numbers that make the mapping checkable by hand.
FSCStarEngine SyntheticGame()
{
    FSCStarEngine G;
    G.SceneUnitNits = 100; G.LuminanceToEV = 2; G.GlowSquareFactor = 9; G.MiddleGrey = 0.5; G.MaxDisplayNitsLDR = 50;
    G.Compensation = {FVector2D(0, -1), FVector2D(10, 1), FVector2D(20, 1)};
    G.MultiplierMin = 0.01; G.MultiplierMax = 8;
    G.AdaptTimeDark = 4; G.AdaptTimeBright = 2; G.AdaptTimePupil = 0.5; G.AdaptRangePupil = 2;
    G.DiscardRatioDark = 0.05; G.DiscardRatioBright = 0.1;
    G.MeterLogMin = -6; G.MeterLogMax = 10;
    G.ExposureMeterMode = 3; G.IncidentBlendAmount = 0.25; G.IncidentSmoothingTime = 0.75;
    G.MeterMaxCompensationPerBlend = 4; G.MeterMinCompensationPerBlend = -1; G.IncidentMiddleGrey = 0.18;
    G.TonemappingCurve = 1; G.TonemappingCurveToe = 0.5;
    return G;
}
// The bias the keys describe: linear between them, the end keys held outside.
double KeyCurve(const FSCExposureSettings& E, double X)
{
    const TArray<FVector2D>& K = E.BiasCurve;
    if (X <= K[0].X) return K[0].Y;
    for (int32 I = 1; I < K.Num(); ++I)
        if (X <= K[I].X) return FMath::Lerp(K[I - 1].Y, K[I].Y, (X - K[I - 1].X) / (K[I].X - K[I - 1].X));
    return K.Last().Y;
}
UCurveFloat* MakeCurve(const FSCExposureSettings& E)
{
    UCurveFloat* Curve = NewObject<UCurveFloat>(GetTransientPackage());
    for (const FVector2D& K : E.BiasCurve)
        Curve->FloatCurve.SetKeyInterpMode(Curve->FloatCurve.AddKey(float(K.X), float(K.Y)), RCIM_Linear);
    return Curve;
}
// The bias UE applies: CreateCurveLUT (PostProcessEyeAdaptation.cpp) stores 2^curve as half floats at Samples evenly spaced inputs
// across the domain the renderer picks (UE::AutoExposure::GetCompensationCurveLUTDomain); the shader reads it with linear filtering
// and clamped addressing.
double BakedBias(const FSCExposureSettings& E, const UE::AutoExposure::FCompensationCurveLUTDomain& D, double X)
{
    const int32 N = D.NumSamples;
    const double Delta = (double(D.MaxX) - D.MinX) / (N - 1);
    const auto Texel = [&](int32 I) { return double(FFloat16(float(FMath::Pow(2.0, KeyCurve(E, D.MinX + Delta * I)))).GetFloat()); };
    const double At = FMath::Clamp((X - D.MinX) / Delta, 0.0, double(N - 1));
    const int32 Low = FMath::Min(FMath::FloorToInt32(At), N - 1), High = FMath::Min(Low + 1, N - 1);
    return FMath::Log2(FMath::Lerp(Texel(Low), Texel(High), At - Low));
}
// Sets a console variable for the scope of a test and restores it.
struct FScopedConsoleValue
{
    IConsoleVariable* Variable;
    FString Saved;
    FScopedConsoleValue(const TCHAR* Name, const TCHAR* Value) : Variable(IConsoleManager::Get().FindConsoleVariable(Name))
    {
        if (Variable) { Saved = Variable->GetString(); Variable->Set(Value, ECVF_SetByCode); }
    }
    ~FScopedConsoleValue() { if (Variable) Variable->Set(*Saved, ECVF_SetByCode); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerExposureLut, "SCBridge.ViewerExposure.CompensationLut",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerExposureLut::RunTest(const FString&)
{
    // The project bakes the curve over its key range at 1/64 stop (DefaultEngine.ini, fork policy WP-E1 E1-5): the renderer then
    // follows the keys everywhere, the clamp ends included, with no cvar set before the curve exists.
    FSCEngineExposure Host;
    Host.LuminanceMax = 2; Host.bExtendedRange = true; Host.ExponentialTransitionDistance = 1.5;
    const IConsoleVariable* Domain = IConsoleManager::Get().FindConsoleVariable(TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.Domain"));
    const IConsoleVariable* MaxStep = IConsoleManager::Get().FindConsoleVariable(TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.MaxStep"));
    if (!TestNotNull(TEXT("the fork's LUT domain policy"), Domain) || !TestNotNull(TEXT("the fork's LUT sample step"), MaxStep)) return false;
    TestEqual(TEXT("the project bakes over the key range"), Domain->GetInt(), 1);
    TestEqual(TEXT("the project's sample step: 1/64 stop"), double(MaxStep->GetFloat()), 1.0 / 64, 1e-9);
    // A multiplier clamp that ends past curve input 20, the upper end of UE's stock LUT domain [-10, 20].
    FSCStarEngine Wide = SyntheticGame();
    Wide.MultiplierMin = 1e-5;
    TestTrue(TEXT("the wide clamp ends beyond UE's stock LUT domain"), FSCExposureSettings::From(Wide, Host).BiasCurve.Last().X > 20);
    const FSCStarEngine Games[] = {SyntheticGame(), Wide, FSCStarEngine::Get()};
    for (const FSCStarEngine& G : Games)
    {
        const FSCExposureSettings E = FSCExposureSettings::From(G, Host);
        const UE::AutoExposure::FCompensationCurveLUTDomain D = UE::AutoExposure::GetCompensationCurveLUTDomain(MakeCurve(E));
        if (!TestTrue(TEXT("the LUT has samples"), D.NumSamples >= 2)) return false;
        TestEqual(TEXT("the LUT starts at the first key"), double(D.MinX), E.BiasCurve[0].X, 1e-5);
        TestEqual(TEXT("the LUT ends at the last key"), double(D.MaxX), E.BiasCurve.Last().X, 1e-5);
        TestTrue(TEXT("sample spacing within MaxStep"), (double(D.MaxX) - D.MinX) / (D.NumSamples - 1) <= MaxStep->GetFloat() + 1e-9);
        double Worst = 0;
        for (double X = E.BiasCurve[0].X - 5; X <= E.BiasCurve.Last().X + 5; X += 0.01)
            Worst = FMath::Max(Worst, FMath::Abs(BakedBias(E, D, X) - KeyCurve(E, X)));
        TestTrue(FString::Printf(TEXT("the baked LUT follows the keys everywhere (worst %.5f stops)"), Worst), Worst <= 0.005);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerExposureLutPolicy, "SCBridge.ViewerExposure.CompensationLutPolicy",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerExposureLutPolicy::RunTest(const FString&)
{
    // The fork's domain policy (UE::AutoExposure::GetCompensationCurveLUTDomain): UE's [MinX, MaxX] by default, the key range only
    // where the curve is constant beyond its keys (UE's extrapolation semantics preserved).
    FScopedConsoleValue MinX(TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.MinX"), TEXT("-10"));
    FScopedConsoleValue MaxX(TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.MaxX"), TEXT("20"));
    FScopedConsoleValue Samples(TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.NumSamples"), TEXT("64"));
    FScopedConsoleValue Step(TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.MaxStep"), TEXT("0.5"));
    UCurveFloat* Curve = NewObject<UCurveFloat>(GetTransientPackage());
    Curve->FloatCurve.AddKey(2.f, -1.f);
    Curve->FloatCurve.AddKey(40.f, 3.f);
    {
        FScopedConsoleValue Policy(TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.Domain"), TEXT("0"));
        const UE::AutoExposure::FCompensationCurveLUTDomain D = UE::AutoExposure::GetCompensationCurveLUTDomain(Curve);
        TestTrue(TEXT("UE's policy: the cvar domain and sample count"), D.MinX == -10.f && D.MaxX == 20.f && D.NumSamples == 64);
    }
    FScopedConsoleValue Policy(TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.Domain"), TEXT("1"));
    {
        const UE::AutoExposure::FCompensationCurveLUTDomain D = UE::AutoExposure::GetCompensationCurveLUTDomain(Curve);
        TestTrue(TEXT("key range: both ends constant"), D.MinX == 2.f && D.MaxX == 40.f);
        TestEqual(TEXT("key range: ceil(38 / 0.5) + 1 samples"), D.NumSamples, 77);
    }
    Curve->FloatCurve.PostInfinityExtrap = RCCE_Linear;
    {
        const UE::AutoExposure::FCompensationCurveLUTDomain D = UE::AutoExposure::GetCompensationCurveLUTDomain(Curve);
        TestTrue(TEXT("linear extrapolation keeps UE's upper end"), D.MinX == 2.f && D.MaxX == 20.f);
    }
    Curve->FloatCurve.PostInfinityExtrap = RCCE_Constant;
    Curve->FloatCurve.Reset();
    Curve->FloatCurve.AddKey(5.f, 1.f);
    {
        const UE::AutoExposure::FCompensationCurveLUTDomain D = UE::AutoExposure::GetCompensationCurveLUTDomain(Curve);
        TestTrue(TEXT("one key: UE's domain"), D.MinX == -10.f && D.MaxX == 20.f && D.NumSamples == 64);
    }
    // The engine's conversions are the ones the mapping derives from.
    const FSCEngineExposure Host = FSCEngineExposure::FromEngine();
    TestEqual(TEXT("middle grey"), Host.MiddleGrey, double(UE::AutoExposure::MiddleGrey));
    TestEqual(TEXT("curve input of the white point's luminance"), double(UE::AutoExposure::GetCompensationCurveInput(float(Host.MiddleGrey * Host.LuminanceMax))), 0.0, 1e-5);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerExposureInstall, "SCBridge.ViewerExposure.Install",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerExposureInstall::RunTest(const FString&)
{
    // Installing touches no console variable (the LUT domain is the project's policy, not set before the curve exists).
    const TCHAR* Names[] = {TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.MinX"), TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.MaxX"),
                            TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.NumSamples")};
    FString Before[3];
    for (int32 I = 0; I < 3; ++I)
    {
        const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Names[I]);
        if (!TestNotNull(Names[I], Var)) return false;
        Before[I] = Var->GetString();
    }
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    const APostProcessVolume* Volume = SCViewerExposure::Install(*World);
    const FSCExposureSettings E = FSCExposureSettings::From(FSCStarEngine::Get(), FSCEngineExposure::FromEngine());
    if (TestNotNull(TEXT("the volume spawns"), Volume))
    {
        const FPostProcessSettings& S = Volume->Settings;
        TestTrue(TEXT("unbound and enabled"), Volume->bUnbound && Volume->bEnabled);
        const UCurveFloat* Curve = S.AutoExposureBiasCurve;
        TestEqual(TEXT("one curve key per bias key"), Curve ? Curve->FloatCurve.GetNumKeys() : -1, E.BiasCurve.Num());
        for (int32 I = 0; I < 3; ++I)
            TestEqual(FString::Printf(TEXT("%s untouched"), Names[I]), IConsoleManager::Get().FindConsoleVariable(Names[I])->GetString(), Before[I]);
        // The game's meter and adaptation (fork WP-E1 E1-5).
        TestTrue(TEXT("the game's meter"), S.bOverride_AutoExposureMeter && S.AutoExposureMeter == E.Meter);
        TestTrue(TEXT("its blend and clamps"), S.bOverride_AutoExposureIncidentBlend && S.AutoExposureIncidentBlend == E.IncidentBlend &&
                 S.AutoExposureIncidentClampDown == E.IncidentClampDown && S.AutoExposureIncidentClampUp == E.IncidentClampUp);
        TestTrue(TEXT("two-stage adaptation"), S.bOverride_AutoExposureAdaptation && S.AutoExposureAdaptation == EAutoExposureAdaptation::TwoStage);
        TestTrue(TEXT("its half-lives and range"), S.AutoExposureSlowAdaptationHalfLifeBrighter == E.SlowHalfLifeBrighter &&
                 S.AutoExposureSlowAdaptationHalfLifeDarker == E.SlowHalfLifeDarker && S.AutoExposureFastAdaptationHalfLife == E.FastHalfLife &&
                 S.AutoExposureFastAdaptationRange == E.FastRange);
        // The display curve is the engine's tone curve on this volume (no replacing-tonemapper blendable), found by its tag.
        TestTrue(TEXT("tagged for sc.Look.Display"), Volume->Tags.Contains(SCViewerExposure::VolumeTag));
        TestEqual(TEXT("no post-process material"), S.WeightedBlendables.Array.Num(), 0);
        TestTrue(TEXT("the game's display curve"), S.bOverride_TonemappingMethod && S.TonemappingMethod == ETonemappingMethod::HuePreservingShoulder);
        TestEqual(TEXT("its peak from the fixture"), double(S.HuePreservingShoulderMaxDisplayNits), FSCStarEngine::Get().MaxDisplayNitsLDR, 1e-5);
        // sc.Look.Exposure ue: UE's reflected meter and one-stage adaptation at the pupil's speed (the mapping before E1-5).
        FPostProcessSettings U = S;
        E.Apply(U, false, false);
        const FPostProcessSettings& UE = FPostProcessSettings::GetDefault();
        TestTrue(TEXT("ue: no meter override, UE's reflected meter"), !U.bOverride_AutoExposureMeter && U.AutoExposureMeter == EAutoExposureMeter::Reflected);
        TestTrue(TEXT("ue: no adaptation override"), !U.bOverride_AutoExposureAdaptation && U.AutoExposureAdaptation == UE.AutoExposureAdaptation);
        TestTrue(TEXT("ue: the pupil speed"), U.bOverride_AutoExposureSpeedUp && U.AutoExposureSpeedUp == E.SpeedUp);
        TestTrue(TEXT("ue: the same curve and bounds"), U.AutoExposureBiasCurve == S.AutoExposureBiasCurve && U.AutoExposureMinBrightness == S.AutoExposureMinBrightness);
    }
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerExposureSettings, "SCBridge.ViewerExposure.Settings",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerExposureSettings::RunTest(const FString&)
{
    const FSCStarEngine G = SyntheticGame();
    // The unclamped multiplier is 2^(6.643856 - 0.8 EV) below EV 10 and 2^(8.643856 - EV) above: it meets the
    // clamp [0.01, 8] at EV 4.554820 and EV 15.287712.
    const double EVLow = 4.554820, EVHigh = 15.287712;
    FSCEngineExposure Host;
    Host.LuminanceMax = 2; Host.bExtendedRange = true; Host.ExponentialTransitionDistance = 1.5; Host.MiddleGrey = 0.18;
    const FSCExposureSettings E = FSCExposureSettings::From(G, Host);
    // UE curve input X = log2(metered cd/m2 / (0.18 * LuminanceMax)) = scene EV - 2 - log2(0.36).
    const double Offset = 2 + FMath::Log2(0.36);
    TestEqual(TEXT("min brightness: the EV100 where the multiplier reaches its maximum"), double(E.MinBrightness), EVLow - Offset, 1e-4);
    TestEqual(TEXT("max brightness: the EV100 where the multiplier reaches its minimum"), double(E.MaxBrightness), EVHigh - Offset, 1e-4);
    TestEqual(TEXT("low percent = dark discard ratio"), double(E.LowPercent), 5.0, 1e-5);
    TestEqual(TEXT("high percent = 1 - bright discard ratio"), double(E.HighPercent), 90.0, 1e-5);
    TestEqual(TEXT("speed: ln2 * transition distance / pupil half-life"), double(E.SpeedUp), FMath::Loge(2.0) * 1.5 / 0.5, 1e-5);
    TestEqual(TEXT("one speed both ways (the pupil has one half-life)"), double(E.SpeedDown), double(E.SpeedUp), 1e-9);
    TestEqual(TEXT("histogram min: the meter's log2 range in cd/m2, as EV100"), double(E.HistogramLogMin), -6 + FMath::Log2(100.0) - 1, 1e-4);
    TestEqual(TEXT("histogram max"), double(E.HistogramLogMax), 10 + FMath::Log2(100.0) - 1, 1e-4);

    // Bias keys: the clamp ends plus the anchors between them; stops over UE's 0.18 target.
    const double GreyStops = FMath::Log2(0.5 / 0.18);
    if (!TestEqual(TEXT("three bias keys (anchors outside the clamp are unreachable)"), E.BiasCurve.Num(), 3)) return false;
    TestEqual(TEXT("key 0 at the low clamp"), E.BiasCurve[0].X, EVLow - Offset, 1e-4);
    TestEqual(TEXT("key 0: compensation at the low clamp"), E.BiasCurve[0].Y, (-1 + 0.2 * EVLow) + GreyStops, 1e-4);
    TestEqual(TEXT("key 1 at the interior anchor"), E.BiasCurve[1].X, 10 - Offset, 1e-9);
    TestEqual(TEXT("key 1: the anchor's compensation"), E.BiasCurve[1].Y, 1 + GreyStops, 1e-9);
    TestEqual(TEXT("key 2 at the high clamp"), E.BiasCurve[2].X, EVHigh - Offset, 1e-4);
    TestEqual(TEXT("key 2: compensation at the high clamp"), E.BiasCurve[2].Y, 1 + GreyStops, 1e-4);
    // UE's exposure scale at the low clamp, 2^bias * 0.18 / metered cd/m2, is the game's maximum multiplier per scene unit.
    TestEqual(TEXT("the low clamp reproduces MultiplierMax"),
              FMath::Pow(2.0, E.BiasCurve[0].Y) * 0.18 / G.NitsAtEV(EVLow) * G.SceneUnitNits, 8.0, 1e-3);
    TestEqual(TEXT("the high clamp reproduces MultiplierMin"),
              FMath::Pow(2.0, E.BiasCurve[2].Y) * 0.18 / G.NitsAtEV(EVHigh) * G.SceneUnitNits, 0.01, 1e-5);

    // The meter (mode 3, clamped blend): blend 0.25, at most 4 * 0.25 = 1 stop below and 1 * 0.25 above the reflected meter, the
    // incident meter smoothed with its half-life.
    TestTrue(TEXT("mode 3: clamped blend"), E.Meter == EAutoExposureMeter::ClampedBlend);
    TestEqual(TEXT("blend: r_exposureIncidentBlendAmt"), double(E.IncidentBlend), 0.25, 1e-6);
    TestEqual(TEXT("clamp down: maxCompensation per blend * blend"), double(E.IncidentClampDown), 1.0, 1e-6);
    TestEqual(TEXT("clamp up: -minCompensation per blend * blend"), double(E.IncidentClampUp), 0.25, 1e-6);
    TestEqual(TEXT("incident smoothing: r_exposureIncidentSmoothingTime"), double(E.IncidentSmoothingHalfLife), 0.75, 1e-6);
    // The adaptation: pigment = slow (bright / dark half-lives), pupil = fast (half-life, range).
    TestEqual(TEXT("slow brighter: r_ExposureAdaptTimeBright"), double(E.SlowHalfLifeBrighter), 2.0, 1e-6);
    TestEqual(TEXT("slow darker: r_ExposureAdaptTimeDark"), double(E.SlowHalfLifeDarker), 4.0, 1e-6);
    TestEqual(TEXT("fast: r_ExposureAdaptTimePupil"), double(E.FastHalfLife), 0.5, 1e-6);
    TestEqual(TEXT("fast range: r_ExposureAdaptRangePupil"), double(E.FastRange), 2.0, 1e-6);
    // The other meter modes.
    FSCStarEngine M = G;
    M.ExposureMeterMode = 0; TestTrue(TEXT("mode 0: reflected"), FSCExposureSettings::From(M, Host).Meter == EAutoExposureMeter::Reflected);
    M.ExposureMeterMode = 1; TestTrue(TEXT("mode 1: incident"), FSCExposureSettings::From(M, Host).Meter == EAutoExposureMeter::Incident);
    M.ExposureMeterMode = 2;
    const FSCExposureSettings Blend = FSCExposureSettings::From(M, Host);
    TestTrue(TEXT("mode 2: an unclamped blend"), Blend.Meter == EAutoExposureMeter::ClampedBlend &&
             Blend.IncidentClampDown >= FSCExposureSettings::UnclampedBlendStops && Blend.IncidentClampUp >= FSCExposureSettings::UnclampedBlendStops);

    // Without the extended luminance range UE reads the brightness bounds as white-point luminances and the
    // histogram range as log2 cd/m2; LuminanceMax is 1 there.
    Host.LuminanceMax = 1; Host.bExtendedRange = false;
    const FSCExposureSettings L = FSCExposureSettings::From(G, Host);
    TestEqual(TEXT("legacy min brightness: metered cd/m2 / 0.18"), double(L.MinBrightness), G.NitsAtEV(EVLow) / 0.18, 1e-2);
    TestEqual(TEXT("legacy max brightness"), double(L.MaxBrightness) / (G.NitsAtEV(EVHigh) / 0.18), 1.0, 1e-5);
    TestEqual(TEXT("legacy histogram min: log2 cd/m2"), double(L.HistogramLogMin), -6 + FMath::Log2(100.0), 1e-4);
    TestEqual(TEXT("legacy bias keys keep the EV100 axis"), L.BiasCurve[1].X, 10 - 2 - FMath::Log2(0.18), 1e-9);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerExposureMeter, "SCBridge.ViewerExposure.Meter",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerExposureMeter::RunTest(const FString&)
{
    // The engine's meter rule M = R + clamp(b (I - R), -down, +up) with the derived settings is the game's CalcLogLumMetered,
    // clamp(lerp(R, I, b), R - max b, R - min b), for every mode, over reflected / incident pairs a wide range apart.
    FSCEngineExposure Host;
    Host.LuminanceMax = 1; Host.bExtendedRange = true; Host.ExponentialTransitionDistance = 1.5;
    FSCStarEngine Modes[] = {SyntheticGame(), SyntheticGame(), SyntheticGame(), SyntheticGame(), FSCStarEngine::Get()};
    for (int32 Mode = 0; Mode < 4; ++Mode) Modes[Mode].ExposureMeterMode = Mode;
    for (const FSCStarEngine& G : Modes)
    {
        const FSCExposureSettings E = FSCExposureSettings::From(G, Host);
        double Worst = 0;
        for (double R = -12; R <= 12; R += 0.75)
            for (double I = -12; I <= 12; I += 0.25)
                Worst = FMath::Max(Worst, FMath::Abs(E.EngineMeteredLog(R, I) - FSCExposureSettings::GameMeteredLog(G, R, I)));
        TestTrue(FString::Printf(TEXT("mode %d: the engine's meter is the game's (worst %.2e stops)"), G.ExposureMeterMode, Worst), Worst < 1e-5);
    }
    // The fixture's own clamp in numbers: r_exposureMeterMode 3 with blend 0.5 is M = R + clamp(0.5 (I - R), -1.5, +1).
    const FSCStarEngine& Game = FSCStarEngine::Get();
    const FSCExposureSettings Fixture = FSCExposureSettings::From(Game, Host);
    if (Game.ExposureMeterMode == 3)
    {
        TestEqual(TEXT("fixture: 2 stops brighter incident light lifts the meter one stop (clamp up)"), Fixture.EngineMeteredLog(0, 4), double(Fixture.IncidentClampUp), 1e-6);
        TestEqual(TEXT("fixture: darker incident light lowers it by at most the clamp down"), Fixture.EngineMeteredLog(0, -10), -double(Fixture.IncidentClampDown), 1e-6);
    }
    // The engine calibrates the incident meter to its middle grey, the game to incidentMiddleGray: the blend is only the game's when
    // they agree (a game patch that changes one fails here).
    TestEqual(TEXT("the incident meter's middle grey is the engine's"), Game.IncidentMiddleGrey, double(UE::AutoExposure::MiddleGrey), 1e-6);
    // The engine ports (the meter's blend and smoothing, the two-stage adaptation) were written from these token hashes of the game's
    // functions (scripts/verify_star_engine_render.py re-derives the fixture's from the installed game). A changed hash fails here:
    // re-read the function and update the port in the engine (PostProcessEyeAdaptation.usf) and the mapping.
    FString Text;
    TSharedPtr<FJsonObject> Root;
    const TSharedPtr<FJsonObject>* Functions = nullptr;
    if (TestTrue(TEXT("fixture has shader functions"), FFileHelper::LoadFileToString(Text, *FSCStarEngine::FixturePath()) &&
                 FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) && Root && Root->TryGetObjectField(TEXT("shader_functions"), Functions)))
    {
        const TPair<const TCHAR*, const TCHAR*> Ported[] = {
            {TEXT("CalcLogLumMetered"), TEXT("64c1a2f44cce6bea06bc3c49d4cbaf9e9cfc0973d71b9290f1c0c1de9a5b7ad1")},
            {TEXT("CalcUpdatedFrameState"), TEXT("4f563d3dc44fe11d044df4d53c97dca4dd4c1f86542ed4ff0ca22cf27701bb0f")},
            {TEXT("ApplyEyeAdaptation"), TEXT("2c7f8bd7f1908e23dd39654b4042b3de4e42c2e65bbc97ec189d67051d1c9f63")},
            {TEXT("ExposureAdaptationCS"), TEXT("503485836a216f3a3d3ad92c0bc1bb2a39636e1eaff049a729d4a3dcabe52909")},
            {TEXT("PupilAdaptation"), TEXT("91ee9b5af4aa1a8a734a6a2b4ddb803a075c10880de66820cdd4332b00f808f3")},
            {TEXT("PigmentAdaptation"), TEXT("fb72f6fe6c46bb12b61baf1218a9bb5270674142f2237d669dcde204dcf720c8")},
            {TEXT("ExponentialDecay"), TEXT("a04c557b5b33dd9a25a2dc30b066ac10eeb338a47928dfcfffed0dee06a91faf")},
            {TEXT("CalcExposureAndNoise"), TEXT("d1ecc2be00cb2543c422b9c13cd64f539527cc963c45e7494cbee569ac82615a")}};
        for (const auto& [Name, Hash] : Ported)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            FString FixtureHash;
            if ((*Functions)->TryGetObjectField(Name, Entry)) (*Entry)->TryGetStringField(TEXT("sha256"), FixtureHash);
            TestEqual(FString::Printf(TEXT("the engine port matches the game's %s"), Name), FixtureHash, FString(Hash));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerExposureDisplayCurve, "SCBridge.ViewerExposure.DisplayCurve",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerExposureDisplayCurve::RunTest(const FString&)
{
    // Parameters: DisplayMapping's SDR branch calls curve 1 with MaxDisplayNitsLDR and the curve toe; reference white is neutral.
    const FSCDisplayCurve D = FSCDisplayCurve::From(SyntheticGame());
    TestTrue(TEXT("curve 1 is the engine's hue preserving shoulder"), D.bSupported);
    TestEqual(TEXT("peak: MaxDisplayNitsLDR"), D.MaxDisplayNits, 50.f);
    TestEqual(TEXT("reference white: neutral in SDR"), D.RefWhiteScale, 1.f);
    TestEqual(TEXT("toe: r_HDRTonemappingCurveToe"), D.ToeScale, 0.5f);
    const FSCStarEngine& Game = FSCStarEngine::Get();
    const FSCDisplayCurve Fixture = FSCDisplayCurve::From(Game);
    TestEqual(TEXT("fixture: supported iff curve 1"), Fixture.bSupported, Game.TonemappingCurve == 1);
    TestEqual(TEXT("fixture peak"), double(Fixture.MaxDisplayNits), Game.MaxDisplayNitsLDR, 1e-5);
    TestEqual(TEXT("fixture toe"), double(Fixture.ToeScale), Game.TonemappingCurveToe, 1e-6);
    FSCStarEngine Curve2 = SyntheticGame();
    Curve2.TonemappingCurve = 2; // HuePreservingShoulder2023: no engine curve
    const FSCDisplayCurve Unsupported = FSCDisplayCurve::From(Curve2);
    TestFalse(TEXT("curve 2 has no engine curve"), Unsupported.bSupported);

    // Game: the curve, no lens vignette, neutral grading over a graded volume, local exposure off.
    const FPostProcessSettings& UE = FPostProcessSettings::GetDefault();
    FPostProcessSettings S;
    S.bOverride_ColorSaturation = true; S.ColorSaturation = FVector4(0.5, 0.5, 0.5, 1);
    S.bOverride_WhiteTemp = true; S.WhiteTemp = 4000;
    S.bOverride_LocalExposureHighlightContrastScale = true; S.LocalExposureHighlightContrastScale = 0.6f;
    S.bOverride_ToneCurveAmount = true; S.ToneCurveAmount = 0.5f;
    D.Apply(S, true);
    TestTrue(TEXT("game: hue preserving shoulder"), S.bOverride_TonemappingMethod && S.TonemappingMethod == ETonemappingMethod::HuePreservingShoulder);
    TestTrue(TEXT("game: peak"), S.bOverride_HuePreservingShoulderMaxDisplayNits && S.HuePreservingShoulderMaxDisplayNits == 50.f);
    TestTrue(TEXT("game: reference white"), S.bOverride_HuePreservingShoulderRefWhiteScale && S.HuePreservingShoulderRefWhiteScale == 1.f);
    TestTrue(TEXT("game: toe"), S.bOverride_HuePreservingShoulderToeScale && S.HuePreservingShoulderToeScale == 0.5f);
    TestTrue(TEXT("game: no UE lens vignette"), S.bOverride_VignetteIntensity && S.VignetteIntensity == 0.f);
    TestTrue(TEXT("game: neutral saturation"), S.bOverride_ColorSaturation && S.ColorSaturation == UE.ColorSaturation);
    TestTrue(TEXT("game: neutral white balance"), S.bOverride_WhiteTemp && S.WhiteTemp == UE.WhiteTemp);
    TestTrue(TEXT("game: the whole curve"), S.bOverride_ToneCurveAmount && S.ToneCurveAmount == 1.f);
    TestTrue(TEXT("game: local exposure off"), S.bOverride_LocalExposureHighlightContrastScale && S.LocalExposureHighlightContrastScale == 1.f &&
             S.bOverride_LocalExposureShadowContrastScale && S.LocalExposureShadowContrastScale == 1.f &&
             S.bOverride_LocalExposureDetailStrength && S.LocalExposureDetailStrength == 1.f && !S.LocalExposureHighlightContrastCurve);
    // UE: every display override cleared, values back to UE's defaults (Project Default method, UE's vignette).
    D.Apply(S, false);
    TestFalse(TEXT("ue: no method override"), bool(S.bOverride_TonemappingMethod));
    TestTrue(TEXT("ue: project default method"), S.TonemappingMethod == UE.TonemappingMethod);
    TestFalse(TEXT("ue: no peak override"), bool(S.bOverride_HuePreservingShoulderMaxDisplayNits));
    TestTrue(TEXT("ue: UE's vignette"), !S.bOverride_VignetteIntensity && S.VignetteIntensity == UE.VignetteIntensity);
    TestFalse(TEXT("ue: grading not overridden"), S.bOverride_ColorSaturation || S.bOverride_WhiteTemp || S.bOverride_ToneCurveAmount);
    TestFalse(TEXT("ue: local exposure not overridden"), S.bOverride_LocalExposureHighlightContrastScale || S.bOverride_LocalExposureShadowContrastScale);
    // The engine's HuePreservingShoulder (PostProcessCombineLUTs.usf, WP-E1 E1-4) is a statement-for-statement port of the game's
    // function with these token hashes (scripts/verify_star_engine_render.py re-derives the fixture's from the installed game; UE's
    // LinearToSrgb is ToSRGB's encode). A game patch that changes them fails here: port the new function into the engine.
    {
        FString Text;
        TSharedPtr<FJsonObject> Root;
        const bool bRead = FFileHelper::LoadFileToString(Text, *FSCStarEngine::FixturePath()) &&
                           FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) && Root;
        const TSharedPtr<FJsonObject>* Functions = nullptr;
        if (TestTrue(TEXT("fixture has shader functions"), bRead && Root->TryGetObjectField(TEXT("shader_functions"), Functions)))
        {
            const TPair<const TCHAR*, const TCHAR*> Ported[] = {
                {TEXT("HuePreservingShoulder"), TEXT("765ad7fe9ff17ee77f7ff1f13107cf9fe9d4cf8e656393e11debae841dbd345b")},
                {TEXT("GetLuminance"), TEXT("eab8d913434ab6867181ae9c532ac58f21003e7f8c6b4556281300f07bd6220f")},
                {TEXT("ToSRGB"), TEXT("8c9a59f93c51fd030960b1cfff84ba735e9faa86b35993a420140f081ccfde59")}};
            for (const auto& [Name, Hash] : Ported)
            {
                const TSharedPtr<FJsonObject>* Entry = nullptr;
                FString FixtureHash;
                if ((*Functions)->TryGetObjectField(Name, Entry)) (*Entry)->TryGetStringField(TEXT("sha256"), FixtureHash);
                TestEqual(FString::Printf(TEXT("the engine port matches the game's %s"), Name), FixtureHash, FString(Hash));
            }
        }
    }
    // An unsupported curve leaves UE's tonemapper in place even when the game's display is asked for.
    FPostProcessSettings U;
    Unsupported.Apply(U, true);
    TestFalse(TEXT("unsupported curve: UE's tonemapper"), U.bOverride_TonemappingMethod || U.bOverride_VignetteIntensity);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerExposureBaselineBlend, "SCBridge.ViewerExposure.BaselineBlend",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerExposureBaselineBlend::RunTest(const FString&)
{
    // The final view, not only the volume: a lower-priority map volume grades with a LUT and turns local exposure on with both
    // contrast curves; the viewer volume (game display) blends over it. UE ignores an overridden None asset, so the fork's
    // FPostProcessSettings::bOverriddenNoneAssetsClear (WP-E1 E1-4 review) is what clears them.
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    APostProcessVolume* Volume = SCViewerExposure::Install(*World);
    if (!TestNotNull(TEXT("the volume spawns"), Volume)) { World->DestroyWorld(false); return false; }
    UTexture2D* Lut = NewObject<UTexture2D>(GetTransientPackage());
    UCurveFloat* Highlight = NewObject<UCurveFloat>(GetTransientPackage());
    UCurveFloat* Shadow = NewObject<UCurveFloat>(GetTransientPackage());
    FPostProcessSettings Map;
    Map.bOverride_ColorGradingLUT = true; Map.ColorGradingLUT = Lut;
    Map.bOverride_ColorGradingIntensity = true; Map.ColorGradingIntensity = 1.f;
    Map.bOverride_LocalExposureHighlightContrastCurve = true; Map.LocalExposureHighlightContrastCurve = Highlight;
    Map.bOverride_LocalExposureShadowContrastCurve = true; Map.LocalExposureShadowContrastCurve = Shadow;

    FSceneViewFamily Family(FSceneViewFamily::ConstructionValues(nullptr, nullptr, FEngineShowFlags(ESFIM_Game)).SetTime(FGameTime()));
    FSceneViewInitOptions Init;
    Init.ViewFamily = &Family;
    Init.SetViewRectangle(FIntRect(0, 0, 64, 64));
    Init.ViewOrigin = FVector::ZeroVector;
    Init.ViewRotationMatrix = FMatrix::Identity;
    Init.ProjectionMatrix = FReversedZPerspectiveMatrix(UE_HALF_PI / 2, 1.f, 1.f, 10.f);
    const auto Blend = [&](const FPostProcessSettings& Viewer, float ViewerWeight) {
        FSceneView View(Init);
        View.StartFinalPostprocessSettings(FVector::ZeroVector);
        View.OverridePostProcessSettings(Map, 1.f);
        View.OverridePostProcessSettings(Viewer, ViewerWeight);
        return View.FinalPostProcessSettings;
    };
    const auto LutWeight = [](const FFinalPostProcessSettings& F, const UTexture* Texture) {
        float Weight = 0;
        for (const FFinalPostProcessSettings::FLUTBlenderEntry& E : F.ContributingLUTs) if (E.LUTTexture == Texture) Weight += E.Weight;
        return Weight;
    };
    {
        const FFinalPostProcessSettings F = Blend(Volume->Settings, 1.f);
        TestEqual(TEXT("game display: the map's LUT is gone"), LutWeight(F, Lut), 0.f);
        TestEqual(TEXT("game display: identity LUT at full weight"), LutWeight(F, nullptr), 1.f, 1e-6f);
        TestTrue(TEXT("game display: no highlight contrast curve (local exposure off)"), F.LocalExposureHighlightContrastCurve == nullptr);
        TestTrue(TEXT("game display: no shadow contrast curve"), F.LocalExposureShadowContrastCurve == nullptr);
        TestTrue(TEXT("game display: the exposure compensation curve stays the viewer's"), F.AutoExposureBiasCurve == Volume->Settings.AutoExposureBiasCurve);
    }
    {
        // A partly blended viewer volume blends the map's LUT towards the identity by its weight.
        const FFinalPostProcessSettings F = Blend(Volume->Settings, 0.5f);
        TestEqual(TEXT("half weight: map LUT 0.5"), LutWeight(F, Lut), 0.5f, 1e-5f);
        TestEqual(TEXT("half weight: identity 0.5"), LutWeight(F, nullptr), 0.5f, 1e-5f);
    }
    {
        // UE's tonemapper (sc.Look.Display ue) clears the viewer's display overrides: the map's grading and local exposure are back.
        FPostProcessSettings UEDisplay = Volume->Settings;
        FSCDisplayCurve::From(FSCStarEngine::Get()).Apply(UEDisplay, false);
        TestFalse(TEXT("ue display: the flag is off"), bool(UEDisplay.bOverriddenNoneAssetsClear));
        const FFinalPostProcessSettings F = Blend(UEDisplay, 1.f);
        TestEqual(TEXT("ue display: the map's LUT"), LutWeight(F, Lut), 1.f, 1e-6f);
        TestTrue(TEXT("ue display: the map's curves"), F.LocalExposureHighlightContrastCurve == Highlight && F.LocalExposureShadowContrastCurve == Shadow);
        // UE's own behaviour, unchanged without the flag: an overridden None is ignored.
        UEDisplay.bOverride_ColorGradingLUT = true; UEDisplay.ColorGradingLUT = nullptr;
        UEDisplay.bOverride_LocalExposureHighlightContrastCurve = true; UEDisplay.LocalExposureHighlightContrastCurve = nullptr;
        const FFinalPostProcessSettings G = Blend(UEDisplay, 1.f);
        TestEqual(TEXT("UE default: an overridden None LUT is ignored"), LutWeight(G, Lut), 1.f, 1e-6f);
        TestTrue(TEXT("UE default: an overridden None curve is ignored"), G.LocalExposureHighlightContrastCurve == Highlight);
    }
    World->DestroyWorld(false);
    return true;
}

namespace
{
// The engine's HuePreservingShoulder (PostProcessCombineLUTs.usf, WP-E1 E1-4), statement for statement, in double precision.
FVector3d EngineHuePreservingShoulder(const FVector3d& Col, double MaxDisplayNits, double RefWhiteScale, double ToeScale)
{
    const double RolloffRange = 0.5, ToeRange = 0.15, ToeStrength = 1.0 + 0.1 * ToeScale;
    const double Y = FMath::Clamp(Col.X * 0.2126 + Col.Y * 0.7152 + Col.Z * 0.0722, 0.0, 1.0);
    const double AdjustedRefWhiteScale = FMath::Lerp(1.0, RefWhiteScale, FMath::Pow(Y, 0.25));
    const double MaxDisplayLum = (MaxDisplayNits / AdjustedRefWhiteScale) / 80.0;
    const double ColMax = FMath::Max(FMath::Max3(Col.X, Col.Y, Col.Z), 1e-6);
    FVector3d Ratios = Col / ColMax;
    const double Knee = MaxDisplayLum - RolloffRange, X = ColMax, T = (X - Knee) / (MaxDisplayLum - Knee);
    const double S = FMath::Clamp(X / ToeRange, 0.0, 1.0), Smooth = S * S * (3 - 2 * S);
    const double Toe = FMath::Lerp(FMath::Pow(X, ToeStrength), X, Smooth);
    const double Compressed = X < Knee ? Toe : Knee + (MaxDisplayLum - Knee) * (1.0 - FMath::Exp(-T));
    double ExpR = 3, ExpG = 3;
    const double ExpB = 3;
    double A = FMath::Clamp(FMath::Max(Ratios.X, Ratios.Y) - Ratios.Z * 3.0, 0.0, 1.0);
    ExpR = FMath::Lerp(ExpR, 2.0, A); ExpG = FMath::Lerp(ExpG, 2.2, A);
    A = FMath::Clamp(Ratios.Z - FMath::Max(Ratios.X * 3.0, Ratios.Y), 0.0, 1.0);
    ExpR = FMath::Lerp(ExpR, 5.0, A); ExpG = FMath::Lerp(ExpG, 2.0, A);
    const double Avg = (Col.X + Col.Y + Col.Z) * 0.3333, Base = Avg / ((MaxDisplayLum - 0.2) + Avg);
    Ratios = FVector3d(FMath::Lerp(Ratios.X, 1.0, FMath::Pow(Base, ExpR)), FMath::Lerp(Ratios.Y, 1.0, FMath::Pow(Base, ExpG)),
                       FMath::Lerp(Ratios.Z, 1.0, FMath::Pow(Base, ExpB)));
    return Compressed * Ratios * AdjustedRefWhiteScale;
}
bool IsFiniteColor(const FVector3d& C) { return FMath::IsFinite(C.X) && FMath::IsFinite(C.Y) && FMath::IsFinite(C.Z); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerExposureToneCurveDomain, "SCBridge.ViewerExposure.ToneCurveDomain",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerExposureToneCurveDomain::RunTest(const FString&)
{
    // The renderer raises Max Display Nits to the curve's valid domain (fork, E1-4 review): 52 max(1, RefWhiteScale).
    TestEqual(TEXT("domain floor at reference white 1"), FPostProcessSettings::GetHuePreservingShoulderMinMaxDisplayNits(1.f), 52.f, 1e-4f);
    TestEqual(TEXT("domain floor below reference white 1"), FPostProcessSettings::GetHuePreservingShoulderMinMaxDisplayNits(0.5f), 52.f, 1e-4f);
    TestEqual(TEXT("domain floor at reference white 4"), FPostProcessSettings::GetHuePreservingShoulderMinMaxDisplayNits(4.f), 208.f, 1e-3f);
    TestTrue(TEXT("the game's peak lies in the domain"),
             FSCStarEngine::Get().MaxDisplayNitsLDR >= FPostProcessSettings::GetHuePreservingShoulderMinMaxDisplayNits(1.f));
    // What the old clamp (1 nit) allowed: 8 nits gives a peak of 0.1, a negative denominator for dark colours and a NaN.
    TestFalse(TEXT("outside the domain the curve is not finite (8 nits)"), IsFiniteColor(EngineHuePreservingShoulder(FVector3d(0.02, 0.01, 0.001), 8, 1, 1)));
    // At the domain's edge, for reference whites around 1 and toes from linear to steep, the curve is finite over neutral ramps and
    // saturated colours, and continuous: no step between neighbouring inputs beyond the curve's own slope.
    for (const double RefWhite : {0.5, 1.0, 2.0})
        for (const double Toe : {0.0, 1.0, 10.0})
        {
            const double Nits = FPostProcessSettings::GetHuePreservingShoulderMinMaxDisplayNits(float(RefWhite));
            const FVector3d Hues[] = {FVector3d(1, 1, 1), FVector3d(1, 0.2, 0.05), FVector3d(0.05, 0.2, 1), FVector3d(0.1, 1, 0.1)};
            bool bFinite = true;
            double WorstSlope = 0;
            for (const FVector3d& Hue : Hues)
            {
                const double Step = 1.0 / 4096;
                FVector3d Prev = EngineHuePreservingShoulder(Hue * Step, Nits, RefWhite, Toe);
                for (double X = 2 * Step; X <= 4.0; X += Step)
                {
                    const FVector3d Cur = EngineHuePreservingShoulder(Hue * X, Nits, RefWhite, Toe);
                    bFinite &= IsFiniteColor(Cur);
                    WorstSlope = FMath::Max(WorstSlope, (Cur - Prev).GetAbsMax() / Step);
                    Prev = Cur;
                }
            }
            const FString Case = FString::Printf(TEXT("ref white %.1f toe %.0f (%.1f nits)"), RefWhite, Toe, Nits);
            TestTrue(FString::Printf(TEXT("finite: %s"), *Case), bFinite);
            TestTrue(FString::Printf(TEXT("continuous: %s (worst slope %.3f)"), *Case, WorstSlope), WorstSlope < 4.0);
        }
    return true;
}
#endif
