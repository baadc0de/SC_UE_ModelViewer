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
// The bias UE applies: CreateCurveLUT (PostProcessEyeAdaptation.cpp) stores 2^curve as half floats at Samples evenly
// spaced inputs across [MinX, MaxX]; the shader reads it with linear filtering and clamped addressing.
double BakedBias(const FSCExposureSettings& E, double X)
{
    const int32 N = E.CurveLutSamples;
    const double Delta = (double(E.CurveLutMaxX) - E.CurveLutMinX) / (N - 1);
    const auto Texel = [&](int32 I) { return double(FFloat16(float(FMath::Pow(2.0, KeyCurve(E, E.CurveLutMinX + Delta * I)))).GetFloat()); };
    const double At = FMath::Clamp((X - E.CurveLutMinX) / Delta, 0.0, double(N - 1));
    const int32 Low = FMath::Min(FMath::FloorToInt32(At), N - 1), High = FMath::Min(Low + 1, N - 1);
    return FMath::Log2(FMath::Lerp(Texel(Low), Texel(High), At - Low));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerExposureLut, "SCBridge.ViewerExposure.CompensationLut",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerExposureLut::RunTest(const FString&)
{
    FSCEngineExposure Host;
    Host.LuminanceMax = 2; Host.bExtendedRange = true; Host.ExponentialTransitionDistance = 1.5;
    // A multiplier clamp that ends past curve input 20, the upper end of UE's stock LUT domain [-10, 20].
    FSCStarEngine Wide = SyntheticGame();
    Wide.MultiplierMin = 1e-5;
    TestTrue(TEXT("the wide clamp ends beyond UE's stock LUT domain"), FSCExposureSettings::From(Wide, Host).BiasCurve.Last().X > 20);
    const FSCStarEngine Games[] = {SyntheticGame(), Wide, FSCStarEngine::Get()};
    for (const FSCStarEngine& G : Games)
    {
        const FSCExposureSettings E = FSCExposureSettings::From(G, Host);
        if (!TestTrue(TEXT("the LUT has samples"), E.CurveLutSamples >= 2)) return false;
        TestEqual(TEXT("the LUT starts at the first key"), double(E.CurveLutMinX), E.BiasCurve[0].X, 1e-5);
        TestEqual(TEXT("the LUT ends at the last key"), double(E.CurveLutMaxX), E.BiasCurve.Last().X, 1e-5);
        TestTrue(TEXT("sample spacing within CurveLutStep"),
                 (double(E.CurveLutMaxX) - E.CurveLutMinX) / (E.CurveLutSamples - 1) <= FSCExposureSettings::CurveLutStep + 1e-9);
        double Worst = 0;
        for (double X = E.BiasCurve[0].X - 5; X <= E.BiasCurve.Last().X + 5; X += 0.01)
            Worst = FMath::Max(Worst, FMath::Abs(BakedBias(E, X) - KeyCurve(E, X)));
        TestTrue(FString::Printf(TEXT("the baked LUT follows the keys everywhere (worst %.5f stops)"), Worst), Worst <= 0.005);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerExposureInstall, "SCBridge.ViewerExposure.Install",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerExposureInstall::RunTest(const FString&)
{
    const TCHAR* Names[] = {TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.MinX"), TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.MaxX"),
                            TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.NumSamples")};
    IConsoleVariable* Vars[3];
    FString Saved[3];
    for (int32 I = 0; I < 3; ++I)
    {
        Vars[I] = IConsoleManager::Get().FindConsoleVariable(Names[I]);
        if (!TestNotNull(Names[I], Vars[I])) return false;
        Saved[I] = Vars[I]->GetString();
    }
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    const APostProcessVolume* Volume = SCViewerExposure::Install(*World);
    const FSCExposureSettings E = FSCExposureSettings::From(FSCStarEngine::Get(), FSCEngineExposure::FromConsole());
    if (TestNotNull(TEXT("the volume spawns"), Volume))
    {
        TestTrue(TEXT("unbound and enabled"), Volume->bUnbound && Volume->bEnabled);
        const UCurveFloat* Curve = Volume->Settings.AutoExposureBiasCurve;
        TestEqual(TEXT("one curve key per bias key"), Curve ? Curve->FloatCurve.GetNumKeys() : -1, E.BiasCurve.Num());
        // The renderer bakes the curve with these three; they must describe the installed curve, not UE's defaults.
        TestTrue(TEXT("the settings carry a LUT domain"), E.CurveLutSamples >= 2 && E.CurveLutMaxX > E.CurveLutMinX);
        TestEqual(TEXT("LUT MinX"), Vars[0]->GetFloat(), E.CurveLutMinX);
        TestEqual(TEXT("LUT MaxX"), Vars[1]->GetFloat(), E.CurveLutMaxX);
        TestEqual(TEXT("LUT NumSamples"), Vars[2]->GetInt(), E.CurveLutSamples);
    }
    for (int32 I = 0; I < 3; ++I) Vars[I]->Set(*Saved[I], ECVF_SetByCode);
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
    Host.LuminanceMax = 2; Host.bExtendedRange = true; Host.ExponentialTransitionDistance = 1.5;
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
#endif
