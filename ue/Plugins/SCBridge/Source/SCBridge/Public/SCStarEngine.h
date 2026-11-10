// Star Engine render constants (the extracted fixture Resources/StarEngineRender.json) and the rules built on them.
#pragma once
#include "CoreMinimal.h"

/**
 * Values of the game's renderer that the Viewer cannot read at run time: cvar defaults compiled into the game
 * executable and constants of its tokenised shader sources. They live in the extracted fixture
 * Resources/StarEngineRender.json (provenance: StarEngineRender.notes.md) and are re-checked against the installed
 * game by scripts/verify_star_engine_render.py. There is no in-code fallback: a missing or incomplete fixture is an
 * error. The derivation of every rule is in ue/verification/WP-U3b.md §13.
 */
struct SCBRIDGE_API FSCStarEngine
{
    double SceneUnitNits = 0;     // HDR_LUMINANCE_SCALE: cd/m2 of one unit of scene colour
    double LuminanceToEV = 0;     // LOG2_LUMINANCE_TO_EV: scene EV = log2(cd/m2) + this
    double GlowSquareFactor = 0;  // sqrGlowFactor: linear glow (scene units) = Glow^2 * this
    double MiddleGrey = 0;        // middleGray of CalcExposureAndNoise
    double MaxDisplayNitsLDR = 0; // the SDR display peak the display curve is called with
    TArray<FVector2D> Compensation; // r_ExposureCompensationEV<n>: (metered scene EV n, compensation in stops), ascending
    double MultiplierMin = 0, MultiplierMax = 0;                 // r_ExposureMultiplierMin/Max (per scene unit)
    double AdaptTimeDark = 0, AdaptTimeBright = 0;               // pigment half-lives, seconds
    double AdaptTimePupil = 0, AdaptRangePupil = 0;              // pupil half-life (s) and range (stops)
    double DiscardRatioDark = 0, DiscardRatioBright = 0;         // histogram fractions left out of the meter
    double MeterLogMin = 0, MeterLogMax = 0;                     // LOG_MIN / LOG_MAX: the meter's log2 scene-unit range
    int32 TonemappingCurve = 0;                                  // r_HDRTonemappingCurve
    double TonemappingCurveToe = 0;                              // r_HDRTonemappingCurveToe

    static FString FixturePath();
    static bool Load(const FString& Path, FSCStarEngine& Out, FString& Error);
    /** The plugin's fixture, loaded once; fatal when it is missing or incomplete. */
    static const FSCStarEngine& Get();

    /** UE emissive luminance (cd/m2) of an authored material Glow: MaterialGlowToLinearGlow, in scene units. */
    double GlowNits(double Glow) const { return SceneUnitNits * GlowSquareFactor * Glow * Glow; }
    /** UE emissive luminance (cd/m2) of a per-instance glow amount: CalculateEmissive adds instData.glow to the
     *  linear material glow without a conversion, so it is already in scene units. */
    double InstanceGlowNits(double Amount) const { return SceneUnitNits * Amount; }
    /**
     * Far-field luminous intensity (cd) of a point / projector light. The engine scales the authored intensity so
     * that it is reached one metre from the bulb surface and attenuates with
     * GetPhysicalLightAttenuation = 1 / (tweak * (d - bulb) / bulb + 1)^2; the Lambert pi is folded into the colour.
     */
    double PointCandela(double Intensity, double BulbRadiusM, double AttenuationTweak) const;
    /** Normal luminous intensity (cd) of a planar light: its colour is luminance / pi (PlanarLightDiffuse). */
    double PlanarCandela(double Intensity, double WidthM, double HeightM) const;
    /** Scene EV of a luminance in cd/m2 (LogLumToEV). */
    double SceneEV(double Nits) const { return FMath::Log2(Nits) + LuminanceToEV; }
    double NitsAtEV(double EV) const { return FMath::Pow(2.0, EV - LuminanceToEV); }
    /** CalculateEVCompensation: piecewise linear through the anchors, end segments extended. */
    double CompensationAt(double MeteredEV) const;
    /** log2 of the unclamped exposure multiplier (per scene unit) the game settles on at a metered scene EV. */
    double LogMultiplierAt(double MeteredEV) const;
    /** The metered scene EV at which the unclamped multiplier equals Multiplier (the multiplier falls as EV rises). */
    double EVAtMultiplier(double Multiplier) const;
};
