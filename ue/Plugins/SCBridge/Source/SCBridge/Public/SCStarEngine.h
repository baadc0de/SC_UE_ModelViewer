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
    // The meter (CalcLogLumMetered, OpticsExposure.cfxb): r_exposureMeterMode 0 reflected, 1 incident, 2 blend, 3 clamped blend of the
    // reflected and the incident meter, in log2 luminance; the blend moves r_exposureIncidentBlendAmt of the way to the incident one,
    // clamped to [reflected - MaxCompensationPerBlend * blend, reflected - MinCompensationPerBlend * blend] (mode 3).
    int32 ExposureMeterMode = 0;                                 // r_exposureMeterMode
    double IncidentBlendAmount = 0;                              // r_exposureIncidentBlendAmt
    double IncidentSmoothingTime = 0;                            // r_exposureIncidentSmoothingTime: half-life (s) of the incident meter
    double MeterMaxCompensationPerBlend = 0;                     // maxCompensation = this * IncidentBlendAmt (stops below reflected)
    double MeterMinCompensationPerBlend = 0;                     // minCompensation = this * IncidentBlendAmt (<= 0: stops above reflected)
    double IncidentMiddleGrey = 0;                               // ExposureAdaptationCS: incident luminance = this * white-diffuse luminance
    int32 TonemappingCurve = 0;                                  // r_HDRTonemappingCurve
    double TonemappingCurveToe = 0;                              // r_HDRTonemappingCurveToe
    // GlassPBR (glassLib.cfib GlassShared). Absorption per unit of layer height:
    //   density = (1 / max(transmissive colour, GlassColourFloor) - 1) * GlassDensityScale,
    //   transmittance = exp(-density * height * refracted path length).
    // Height = saturate((vertex blue - GlassVertexHeightBias) * GlassVertexHeightScale) under %PER_VERTEX_CURVATURE,
    // times the height map under %HEIGHT_MAP, which also clips the surface below GlassHeightClip.
    double GlassDensityScale = 0, GlassColourFloor = 0;
    double GlassVertexHeightBias = 0, GlassVertexHeightScale = 0;
    double GlassHeightClip = 0;
    double GlassTintStrengthDefault = 0; // GlassTintStrength when the material does not set it (%USE_TINT_PALETTE)
    // The glass maps (normal, smoothness, height, tint) are sampled at glassUV = baseUV * GlassTiling with
    // v *= GlassVStretch; under %THIN_FILM the iridescence LUT is read at NdotV * ThinFilmThickness. These are the
    // shader's values for a material that does not set the parameter.
    double GlassTilingDefault = 0, GlassVStretchDefault = 0;
    double GlassThinFilmThicknessDefault = 0;
    // LayerBlend_V2 GetAlphaBlendDetailMaps (WP-U2b): the engine's wear/dirt/frost detail map (shader_textures; scviewer-core
    // copies it into packages with a LayerBlend_V2 material under this path) sampled at baseUV * DirtDetailScale.
    FString WearDirtFrostDetailMap;
    double DirtDetailScale = 0;
    // GetLightForcedDistanceFade: every local light ends at its radius through saturate((1 - d / R) / fraction)^2, full
    // strength until the last `fraction` of the radius. The importer sets it as the light's range fade (engine fork,
    // ULocalLightComponent::RangeFade = SquaredLinearTail with RangeFadeFraction = fraction; UE's is (1 - (d / R)^4)^2).
    double ForcedFadeFraction = 0;
    // Screen-space directional occlusion (DirOcclusionPS, CalculateSSDO, DeriveSpecularOcclusion; the engine fork's SSDO, WP-E1 E1-7):
    // r_ssdo on / off, r_ssdoQuality (0 = 3 slices x 6 samples per side, the quality ported), the search radius (m), the AO exponent and
    // the amounts on direct light (lights that cast no shadow, the sun), ambient light and its reflections.
    bool bSSDO = false;
    int32 SSDOQuality = 0;
    double SSDORadiusM = 0, SSDOStrength = 0;
    double SSDOAmountDirect = 0, SSDOAmountAmbient = 0, SSDOAmountReflection = 0;

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
    /**
     * Distance offset of the same attenuation (m): for every distance it is PointCandela / (max(d, bulb) + offset)^2,
     * offset = bulb (1 - tweak) / tweak. 0 for tweak 1 (plain inverse square beyond the bulb), negative above 1. The
     * importer sets it as the light's bulb offset law (engine fork, ULocalLightComponent::DistanceAttenuation = BulbOffset).
     */
    double AttenuationOffset(double BulbRadiusM, double AttenuationTweak) const;
    /** Normal luminous intensity (cd) of a planar light: its colour is luminance / pi (PlanarLightDiffuse). */
    double PlanarCandela(double Intensity, double WidthM, double HeightM) const;
    /** The game's range window of a local light at a distance (any unit, the radius' unit): GetLightForcedDistanceFade. */
    double LightRangeFade(double Distance, double Radius) const;
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
