// FSCExposureSettings: the game's exposure chain as UE eye-adaptation settings, and the volume that applies it.
#include "SCViewerExposure.h"
#include "SCStarEngine.h"
#include "Curves/CurveFloat.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "SCViewerEngineFeatures.h"
#if !defined(SCVIEWER_ENGINE_TONE_CURVE) || SCVIEWER_ENGINE_TONE_CURVE < 1
#error "SCBridge needs the SCViewer fork of UE 5.9 with the display curve (WP-E1 E1-4, SCVIEWER_ENGINE_TONE_CURVE)"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSCExposure, Log, All);

FSCEngineExposure FSCEngineExposure::FromConsole()
{
    const auto Number = [](const TCHAR* Name) {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        if (!Variable) UE_LOG(LogSCExposure, Fatal, TEXT("console variable %s is missing: the exposure mapping depends on it"), Name);
        return double(Variable->GetFloat());
    };
    FSCEngineExposure Host;
    Host.bExtendedRange = Number(TEXT("r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange")) != 0;
    // LuminanceMaxFromLensAttenuation() (Renderer-private): 0.78 is the ISO 12232 saturation-speed constant q*100.
    Host.LuminanceMax = Host.bExtendedRange ? 0.78 / FMath::Max(Number(TEXT("r.EyeAdaptation.LensAttenuation")), 0.01) : 1.0;
    Host.ExponentialTransitionDistance = Number(TEXT("r.EyeAdaptation.ExponentialTransitionDistance"));
    return Host;
}

FSCExposureSettings FSCExposureSettings::From(const FSCStarEngine& Game, const FSCEngineExposure& Host)
{
    // UE's curve input: EV100 = log2(metered cd/m2 / (EngineMiddleGrey * LuminanceMax)); the game's scene EV is
    // log2(metered cd/m2) + LuminanceToEV.
    const double AxisOffset = Game.LuminanceToEV + FMath::Log2(EngineMiddleGrey * Host.LuminanceMax);
    // UE exposes the metered luminance to EngineMiddleGrey * 2^bias, the game to MiddleGrey * 2^compensation.
    const double GreyStops = FMath::Log2(Game.MiddleGrey / EngineMiddleGrey);
    // The multiplier clamp: outside [EVLow, EVHigh] the game's exposure stops following the metered luminance.
    const double EVLow = Game.EVAtMultiplier(Game.MultiplierMax);
    const double EVHigh = Game.EVAtMultiplier(Game.MultiplierMin);
    // Brightness bounds: EV100 with the extended luminance range, white-point luminance (average / grey) without.
    const auto Brightness = [&](double EV) { return Host.bExtendedRange ? EV - AxisOffset : Game.NitsAtEV(EV) / EngineMiddleGrey; };
    const double HistogramShift = FMath::Log2(Game.SceneUnitNits) - (Host.bExtendedRange ? FMath::Log2(Host.LuminanceMax) : 0.0);

    FSCExposureSettings Out;
    Out.MinBrightness = float(Brightness(EVLow));
    Out.MaxBrightness = float(Brightness(EVHigh));
    Out.LowPercent = float(100.0 * Game.DiscardRatioDark);
    Out.HighPercent = float(100.0 * (1.0 - Game.DiscardRatioBright));
    // UE adapts exponentially inside the transition distance D with time constant D / speed (linearly at `speed`
    // stops per second beyond it); the game's pupil decays with half-life AdaptTimePupil = ln2 * time constant.
    Out.SpeedUp = Out.SpeedDown = float(FMath::Loge(2.0) * Host.ExponentialTransitionDistance / Game.AdaptTimePupil);
    Out.HistogramLogMin = float(Game.MeterLogMin + HistogramShift);
    Out.HistogramLogMax = float(Game.MeterLogMax + HistogramShift);
    // The compensation curve between the clamp ends; UE holds the end keys outside, which with the brightness
    // bounds pins the exposure exactly as the multiplier clamp does.
    const auto Key = [&](double EV) { Out.BiasCurve.Add(FVector2D(EV - AxisOffset, Game.CompensationAt(EV) + GreyStops)); };
    Key(EVLow);
    for (const FVector2D& Anchor : Game.Compensation)
        if (Anchor.X > EVLow && Anchor.X < EVHigh) Key(Anchor.X);
    Key(EVHigh);
    // UE bakes the curve into evenly spaced samples and holds the end samples outside: the domain is the key range.
    Out.CurveLutMinX = float(Out.BiasCurve[0].X);
    Out.CurveLutMaxX = float(Out.BiasCurve.Last().X);
    Out.CurveLutSamples = FMath::CeilToInt32((double(Out.CurveLutMaxX) - Out.CurveLutMinX) / CurveLutStep) + 1;
    return Out;
}

FSCDisplayCurve FSCDisplayCurve::From(const FSCStarEngine& Game)
{
    FSCDisplayCurve Out;
    Out.GameCurve = Game.TonemappingCurve;
    Out.bSupported = Game.TonemappingCurve == 1; // HuePreservingShoulder; 2 (HuePreservingShoulder2023) has no engine curve
    Out.MaxDisplayNits = float(Game.MaxDisplayNitsLDR);
    Out.ToeScale = float(Game.TonemappingCurveToe);
    return Out;
}

void FSCDisplayCurve::Apply(FPostProcessSettings& S, bool bGame) const
{
    const bool bOn = bGame && bSupported;
    const FPostProcessSettings& UE = FPostProcessSettings::GetDefault();
    // A field and its override: the value when on, UE's default and no override when off.
#define SC_DISPLAY_SET(Field, Value) S.bOverride_##Field = bOn; S.Field = bOn ? (Value) : UE.Field
#define SC_DISPLAY_NEUTRAL(Field) SC_DISPLAY_SET(Field, UE.Field)
    SC_DISPLAY_SET(TonemappingMethod, ETonemappingMethod::HuePreservingShoulder);
    SC_DISPLAY_SET(HuePreservingShoulderMaxDisplayNits, MaxDisplayNits);
    SC_DISPLAY_SET(HuePreservingShoulderRefWhiteScale, RefWhiteScale);
    SC_DISPLAY_SET(HuePreservingShoulderToeScale, ToeScale);
    SC_DISPLAY_SET(VignetteIntensity, 0.f);
    // Neutral grading: UE's defaults over whatever a map volume grades (ToneCurveAmount scales the curve itself).
    SC_DISPLAY_NEUTRAL(ToneCurveAmount);
    SC_DISPLAY_NEUTRAL(TemperatureType); SC_DISPLAY_NEUTRAL(WhiteTemp); SC_DISPLAY_NEUTRAL(WhiteTint);
    SC_DISPLAY_NEUTRAL(ColorSaturation); SC_DISPLAY_NEUTRAL(ColorContrast); SC_DISPLAY_NEUTRAL(ColorGamma); SC_DISPLAY_NEUTRAL(ColorGain); SC_DISPLAY_NEUTRAL(ColorOffset);
    SC_DISPLAY_NEUTRAL(ColorSaturationShadows); SC_DISPLAY_NEUTRAL(ColorContrastShadows); SC_DISPLAY_NEUTRAL(ColorGammaShadows); SC_DISPLAY_NEUTRAL(ColorGainShadows); SC_DISPLAY_NEUTRAL(ColorOffsetShadows);
    SC_DISPLAY_NEUTRAL(ColorSaturationMidtones); SC_DISPLAY_NEUTRAL(ColorContrastMidtones); SC_DISPLAY_NEUTRAL(ColorGammaMidtones); SC_DISPLAY_NEUTRAL(ColorGainMidtones); SC_DISPLAY_NEUTRAL(ColorOffsetMidtones);
    SC_DISPLAY_NEUTRAL(ColorSaturationHighlights); SC_DISPLAY_NEUTRAL(ColorContrastHighlights); SC_DISPLAY_NEUTRAL(ColorGammaHighlights); SC_DISPLAY_NEUTRAL(ColorGainHighlights); SC_DISPLAY_NEUTRAL(ColorOffsetHighlights);
    SC_DISPLAY_NEUTRAL(ColorCorrectionShadowsMax); SC_DISPLAY_NEUTRAL(ColorCorrectionHighlightsMin); SC_DISPLAY_NEUTRAL(ColorCorrectionHighlightsMax);
    SC_DISPLAY_NEUTRAL(SceneColorTint); SC_DISPLAY_NEUTRAL(ColorGradingIntensity); SC_DISPLAY_NEUTRAL(ColorGradingLUT);
    // Local exposure off: the renderer skips it when both contrast scales and the detail strength are 1 and no curve is set.
    SC_DISPLAY_NEUTRAL(LocalExposureHighlightContrastScale); SC_DISPLAY_NEUTRAL(LocalExposureShadowContrastScale);
    SC_DISPLAY_NEUTRAL(LocalExposureDetailStrength);
    SC_DISPLAY_NEUTRAL(LocalExposureHighlightContrastCurve); SC_DISPLAY_NEUTRAL(LocalExposureShadowContrastCurve);
#undef SC_DISPLAY_NEUTRAL
#undef SC_DISPLAY_SET
}

const FName SCViewerExposure::VolumeTag(TEXT("SCViewerExposure"));

APostProcessVolume* SCViewerExposure::Install(UWorld& World)
{
    const FSCStarEngine& Game = FSCStarEngine::Get();
    const FSCExposureSettings E = FSCExposureSettings::From(Game, FSCEngineExposure::FromConsole());
    // The renderer rebakes the curve LUT when the curve or the sample count changes, not when the domain alone
    // does: the domain is in place before the curve exists.
    const auto Console = [](const TCHAR* Name) {
        IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        if (!Variable) UE_LOG(LogSCExposure, Fatal, TEXT("console variable %s is missing: the exposure curve depends on it"), Name);
        return Variable;
    };
    Console(TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.MinX"))->Set(E.CurveLutMinX, ECVF_SetByCode);
    Console(TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.MaxX"))->Set(E.CurveLutMaxX, ECVF_SetByCode);
    Console(TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT.NumSamples"))->Set(E.CurveLutSamples, ECVF_SetByCode);
    if (!Console(TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT"))->GetBool())
        UE_LOG(LogSCExposure, Warning, TEXT("r.EyeAdaptation.ExposureCompensationCurveLUT is off: the curve is evaluated on the CPU, the LUT domain is unused"));
    float Priority = 0; // above every volume the map carries
    for (TActorIterator<APostProcessVolume> It(&World); It; ++It) Priority = FMath::Max(Priority, It->Priority + 1.f);

    FActorSpawnParameters Spawn;
    Spawn.ObjectFlags |= RF_Transient;
    APostProcessVolume* Volume = World.SpawnActor<APostProcessVolume>(Spawn);
    if (!Volume) { UE_LOG(LogSCExposure, Error, TEXT("VIEWER_GAME_EXPOSURE failed: cannot spawn the volume")); return nullptr; }
    Volume->bUnbound = true; Volume->bEnabled = true; Volume->BlendWeight = 1.f; Volume->Priority = Priority;
    Volume->Tags.Add(VolumeTag);

    FPostProcessSettings& S = Volume->Settings;
    S.bOverride_AutoExposureMethod = true; S.AutoExposureMethod = AEM_Histogram;
    S.bOverride_AutoExposureMinBrightness = true; S.AutoExposureMinBrightness = E.MinBrightness;
    S.bOverride_AutoExposureMaxBrightness = true; S.AutoExposureMaxBrightness = E.MaxBrightness;
    S.bOverride_AutoExposureLowPercent = true; S.AutoExposureLowPercent = E.LowPercent;
    S.bOverride_AutoExposureHighPercent = true; S.AutoExposureHighPercent = E.HighPercent;
    S.bOverride_AutoExposureSpeedUp = true; S.AutoExposureSpeedUp = E.SpeedUp;
    S.bOverride_AutoExposureSpeedDown = true; S.AutoExposureSpeedDown = E.SpeedDown;
    S.bOverride_HistogramLogMin = true; S.HistogramLogMin = E.HistogramLogMin;
    S.bOverride_HistogramLogMax = true; S.HistogramLogMax = E.HistogramLogMax;
    S.bOverride_AutoExposureBias = true; S.AutoExposureBias = 0.f; // the whole compensation is the curve
    S.bOverride_AutoExposureApplyPhysicalCameraExposure = true; S.AutoExposureApplyPhysicalCameraExposure = false;
    UCurveFloat* Curve = NewObject<UCurveFloat>(Volume, NAME_None, RF_Transient);
    for (const FVector2D& K : E.BiasCurve)
        Curve->FloatCurve.SetKeyInterpMode(Curve->FloatCurve.AddKey(float(K.X), float(K.Y)), RCIM_Linear);
    S.bOverride_AutoExposureBiasCurve = true; S.AutoExposureBiasCurve = Curve;

    // Look (2026-11-12, plan D15, adjudicated; ue/verification/LOOK-1.md, LOOK-2.md): Lumen bounces 1.15x the surface's
    // diffuse colour, a restrained lift of indirect light (1.3 in LOOK-1 partly offset the since-dropped room
    // confinement; 1.0..1.3 moves the views 1..4 %). Not game data.
    S.bOverride_LumenDiffuseColorBoost = true; S.LumenDiffuseColorBoost = 1.15f;

    // Display mapping: the game's display curve as the engine's tone curve (WP-E1 E1-4), with the fidelity baseline around it.
    const FSCDisplayCurve Display = FSCDisplayCurve::From(Game);
    if (!Display.bSupported)
        UE_LOG(LogSCExposure, Error, TEXT("r_HDRTonemappingCurve=%d has no engine tone curve: UE's tonemapper stays"), Display.GameCurve);
    Display.Apply(S, true);
    const bool bDisplay = Display.bSupported;

    // The volume registered itself at its default priority when it spawned; the world keeps its volumes sorted.
    World.RemovePostProcessVolume(Volume);
    World.AddPostProcessVolume(TWeakInterfacePtr<IInterface_PostProcessVolume>(Volume));
    UE_LOG(LogSCExposure, Display, TEXT("VIEWER_GAME_EXPOSURE priority=%.0f brightness=%.3f..%.3f percent=%.1f..%.1f speed=%.3f histogram=%.3f..%.3f keys=%d first=(%.3f,%.3f) last=(%.3f,%.3f) lut=%.3f..%.3f/%d display_curve=%d tone_curve=%s max_nits=%.1f ref_white=%.2f toe=%.2f"),
           Priority, E.MinBrightness, E.MaxBrightness, E.LowPercent, E.HighPercent, E.SpeedUp, E.HistogramLogMin, E.HistogramLogMax,
           E.BiasCurve.Num(), E.BiasCurve[0].X, E.BiasCurve[0].Y, E.BiasCurve.Last().X, E.BiasCurve.Last().Y, E.CurveLutMinX, E.CurveLutMaxX, E.CurveLutSamples, bDisplay ? Game.TonemappingCurve : 0,
           bDisplay ? TEXT("HuePreservingShoulder") : TEXT("project"), S.HuePreservingShoulderMaxDisplayNits, S.HuePreservingShoulderRefWhiteScale, S.HuePreservingShoulderToeScale);
    return Volume;
}
