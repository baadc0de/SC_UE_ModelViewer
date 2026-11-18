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
        // The display curve is the engine's tone curve on this volume (no replacing-tonemapper blendable), found by its tag.
        TestTrue(TEXT("tagged for sc.Look.Display"), Volume->Tags.Contains(SCViewerExposure::VolumeTag));
        TestEqual(TEXT("no post-process material"), Volume->Settings.WeightedBlendables.Array.Num(), 0);
        TestTrue(TEXT("the game's display curve"), Volume->Settings.bOverride_TonemappingMethod &&
                 Volume->Settings.TonemappingMethod == ETonemappingMethod::HuePreservingShoulder);
        TestEqual(TEXT("its peak from the fixture"), double(Volume->Settings.HuePreservingShoulderMaxDisplayNits), FSCStarEngine::Get().MaxDisplayNitsLDR, 1e-5);
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
#endif
