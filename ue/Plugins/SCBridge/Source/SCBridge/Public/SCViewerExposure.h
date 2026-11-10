// The game's exposure and display chain (FSCStarEngine) expressed as UE post-process settings.
#pragma once
#include "CoreMinimal.h"

class APostProcessVolume;
class UWorld;
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

namespace SCViewerExposure
{
/** Spawn the unbound volume that applies the game's exposure and display mapping above every volume of the map. */
SCBRIDGE_API APostProcessVolume* Install(UWorld& World);
}
