// The game's exposure and display chain (FSCStarEngine) expressed as UE post-process settings.
#pragma once
#include "CoreMinimal.h"
#include "Engine/Scene.h"

class APostProcessVolume;
class UWorld;
struct FPostProcessSettings;
struct FSCStarEngine;

/** The host engine's eye-adaptation conventions the mapping depends on (the fork's public UE::AutoExposure API, WP-E1 E1-5). */
struct SCBRIDGE_API FSCEngineExposure
{
    double LuminanceMax = 1;                   // UE::AutoExposure::GetLuminanceMax()
    bool bExtendedRange = false;               // r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange
    double ExponentialTransitionDistance = 0;  // r.EyeAdaptation.ExponentialTransitionDistance, stops
    double MiddleGrey = 0.18;                  // UE::AutoExposure::MiddleGrey: what the metered luminance is exposed to
    static FSCEngineExposure FromEngine();
};

/**
 * UE histogram auto-exposure settings that reproduce the game's exposure chain (ue/verification/WP-U3b.md 13; fork features WP-E1
 * E1-5): the EV-compensation curve as the exposure-compensation curve, the multiplier clamp as the brightness bounds, the histogram
 * discard ratios as the percentile band, the game's meter (r_exposureMeterMode: the reflected and the incident meter, blended and
 * clamped) as the engine's AutoExposureMeter, and its pupil / pigment adaptation as the engine's two-stage adaptation.
 */
struct SCBRIDGE_API FSCExposureSettings
{
    float MinBrightness = 0, MaxBrightness = 0;     // AutoExposureMin/MaxBrightness
    float LowPercent = 0, HighPercent = 0;          // AutoExposureLow/HighPercent
    float HistogramLogMin = 0, HistogramLogMax = 0;
    /**
     * Linear keys (curve input EV100, stops), constant outside. The renderer bakes the curve over its key range
     * (r.EyeAdaptation.ExposureCompensationCurveLUT.Domain 1 and MaxStep in DefaultEngine.ini), so the clamp ends are sample points.
     */
    TArray<FVector2D> BiasCurve;

    // UE's one-stage adaptation (the "ue" adaptation): UE adapts exponentially inside the transition distance D with time constant
    // D / speed; the game's pupil decays with half-life AdaptTimePupil = ln2 * time constant. Stops per second.
    float SpeedUp = 0, SpeedDown = 0;

    // The game's meter (CalcLogLumMetered) as the engine's AutoExposureMeter.
    EAutoExposureMeter Meter = EAutoExposureMeter::Reflected;
    float IncidentBlend = 0;              // r_exposureIncidentBlendAmt
    float IncidentClampDown = 0;          // maxCompensation = MeterMaxCompensationPerBlend * blend (stops below the reflected meter)
    float IncidentClampUp = 0;            // -minCompensation (stops above it)
    float IncidentSmoothingHalfLife = 0;  // r_exposureIncidentSmoothingTime
    /** Clamp of the unclamped blend (mode 2), stops: far beyond any scene's meter difference. */
    static constexpr float UnclampedBlendStops = 1.0e6f;

    // The game's PigmentAdaptation (slow) and PupilAdaptation (fast) as the engine's two-stage adaptation.
    float SlowHalfLifeBrighter = 0;       // r_ExposureAdaptTimeBright
    float SlowHalfLifeDarker = 0;         // r_ExposureAdaptTimeDark
    float FastHalfLife = 0;               // r_ExposureAdaptTimePupil
    float FastRange = 0;                  // r_ExposureAdaptRangePupil

    static FSCExposureSettings From(const FSCStarEngine& Game, const FSCEngineExposure& Host);

    /** The game's metered log2 luminance (CalcLogLumMetered) for its meter mode, reflected and incident log2 luminances. */
    static double GameMeteredLog(const FSCStarEngine& Game, double LogReflected, double LogIncident);
    /** The engine's meter (ComputeMeteredLuminance, PostProcessEyeAdaptation.usf) with these settings, in log2 luminance. */
    double EngineMeteredLog(double LogReflected, double LogIncident) const;

    /**
     * The viewer volume's exposure settings. Always: the histogram method, brightness bounds, percentiles, histogram range, compensation
     * (bias 0; the curve is set by Install). bGameMeter: the game's meter; else UE's reflected meter (no override). bGameAdaptation: the
     * two-stage adaptation; else UE's adaptation at the pupil speed.
     */
    void Apply(FPostProcessSettings& Settings, bool bGameMeter, bool bGameAdaptation) const;
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
