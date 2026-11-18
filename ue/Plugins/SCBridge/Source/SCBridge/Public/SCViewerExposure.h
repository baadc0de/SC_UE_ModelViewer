// The game's exposure and display chain (FSCStarEngine) expressed as UE post-process settings.
#pragma once
#include "CoreMinimal.h"

class APostProcessVolume;
class UWorld;
struct FPostProcessSettings;
struct FSCStarEngine;

/** The host engine's eye-adaptation conventions the mapping depends on (console variables). */
struct SCBRIDGE_API FSCEngineExposure
{
    double LuminanceMax = 1;                   // LuminanceMaxFromLensAttenuation()
    bool bExtendedRange = false;               // r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange
    double ExponentialTransitionDistance = 0;  // r.EyeAdaptation.ExponentialTransitionDistance, stops
    static FSCEngineExposure FromConsole();
};

/**
 * UE histogram auto-exposure settings that reproduce the game's reflected-light exposure (ue/verification/WP-U3b.md §13):
 * the EV-compensation curve as the exposure-compensation curve, the multiplier clamp as the brightness bounds,
 * the histogram discard ratios as the percentile band and the pupil half-life as the adaptation speed.
 */
struct SCBRIDGE_API FSCExposureSettings
{
    /** The middle grey UE's eye adaptation exposes the metered luminance to (EyeAdaptationCommon, kMiddleGrey). */
    static constexpr double EngineMiddleGrey = 0.18;

    float MinBrightness = 0, MaxBrightness = 0;     // AutoExposureMin/MaxBrightness
    float LowPercent = 0, HighPercent = 0;          // AutoExposureLow/HighPercent
    float SpeedUp = 0, SpeedDown = 0;               // stops per second
    float HistogramLogMin = 0, HistogramLogMax = 0;
    TArray<FVector2D> BiasCurve;                    // linear keys (curve input EV100, stops), constant outside
    /**
     * UE bakes the bias curve into a LUT of evenly spaced samples (r.EyeAdaptation.ExposureCompensationCurveLUT.*)
     * and holds its end samples outside. The domain is the key range, so the clamp ends are sample points, and the
     * sample spacing is at most CurveLutStep stops: a knot between two samples is then off by no more than
     * (slope change) * step / 4 stops. A host precision choice, not a game value.
     */
    static constexpr double CurveLutStep = 1.0 / 64;
    float CurveLutMinX = 0, CurveLutMaxX = 0;
    int32 CurveLutSamples = 0;

    static FSCExposureSettings From(const FSCStarEngine& Game, const FSCEngineExposure& Host);
};

/**
 * The game's SDR display mapping (DisplayMapping in HDRPostProcess.cfxb: HuePreservingShoulder, saturate, sRGB encode) as the
 * engine's tone curve (fork feature SCVIEWER_ENGINE_TONE_CURVE: ETonemappingMethod::HuePreservingShoulder in the tonemapping LUT;
 * UE encodes the output). r_HDRTonemappingCurve 1 selects it; the SDR branch calls it with MaxDisplayNitsLDR and the curve toe
 * (ue/verification/WP-U3b.md 13.5).
 */
struct SCBRIDGE_API FSCDisplayCurve
{
    bool bSupported = false;  // the game's curve has an engine implementation (r_HDRTonemappingCurve 1)
    int32 GameCurve = 0;      // r_HDRTonemappingCurve
    float MaxDisplayNits = 0; // MaxDisplayNitsLDR
    /**
     * hdrRefWhiteScale. The SDR branch's argument is a renderer constant the fixture does not carry; SDR output has no separate
     * paper white, so it is the curve's neutral 1 (no reference white scaling), as the replaced material had it.
     */
    float RefWhiteScale = 1;
    float ToeScale = 0;       // r_HDRTonemappingCurveToe

    static FSCDisplayCurve From(const FSCStarEngine& Game);

    /**
     * The viewer volume's display settings. bGame (and a supported curve): the curve, and the fidelity baseline around it: neutral
     * colour grading, no UE lens vignette (the game's vignetting map is not modelled), local exposure off. Otherwise every one of
     * these overrides is cleared, so UE's tonemapper renders as the project and the map set it.
     */
    void Apply(FPostProcessSettings& Settings, bool bGame) const;
};

namespace SCViewerExposure
{
/** Actor tag of the volume Install spawns (sc.Look.Display finds it by this). */
SCBRIDGE_API extern const FName VolumeTag;
/** Spawn the unbound volume that applies the game's exposure and display mapping above every volume of the map. */
SCBRIDGE_API APostProcessVolume* Install(UWorld& World);
}
