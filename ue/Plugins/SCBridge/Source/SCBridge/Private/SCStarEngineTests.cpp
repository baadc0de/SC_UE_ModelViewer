// SCBridge.StarEngine.*: the extracted render fixture loads completely, and the rules built on it follow their formulas.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCStarEngine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
// Visibly synthetic values (not the game's): round numbers that make every rule checkable by hand.
const TCHAR* SyntheticFixture = TEXT(R"({"schema":"star_engine_render_v1",
 "shader_constants":{"HDR_LUMINANCE_SCALE":{"value":100},"LOG2_LUMINANCE_TO_EV":{"value":2},"sqrGlowFactor":{"value":9},
  "middleGray":{"value":0.5},"MaxDisplayNitsLDR":{"value":50},"LOG_MIN":{"value":-6},"LOG_MAX":{"value":10},
  "glassColourDensityScale":{"value":0.5},"glassColourFloor":{"value":0.125},"glassVertexHeightBias":{"value":0.2},
  "glassVertexHeightScale":{"value":2},"glassHeightClip":{"value":0.05},"glassTintStrengthDefault":{"value":0.75},
  "glassTilingDefault":{"value":4},"glassVStretchDefault":{"value":0.5},"glassThinFilmThicknessDefault":{"value":0.25}},
 "exe_cvars":{"r_ExposureCompensationEV0":{"value":-1},"r_ExposureCompensationEV10":{"value":1},"r_ExposureCompensationEV20":{"value":1},
  "r_ExposureMultiplierMin":{"value":0.01},"r_ExposureMultiplierMax":{"value":8},
  "r_ExposureAdaptTimeDark":{"value":4},"r_ExposureAdaptTimeBright":{"value":2},"r_ExposureAdaptTimePupil":{"value":0.5},
  "r_ExposureAdaptRangePupil":{"value":2},"r_ExposureDiscardRatioDark":{"value":0.05},"r_ExposureDiscardRatioBright":{"value":0.1},
  "r_HDRTonemappingCurve":{"value":1},"r_HDRTonemappingCurveToe":{"value":0.5}}})");
bool LoadText(const FString& Text, FSCStarEngine& Out, FString& Error)
{
    const FString Path = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("StarEngine"), TEXT(".json"));
    FFileHelper::SaveStringToFile(Text, *Path);
    const bool bOK = FSCStarEngine::Load(Path, Out, Error);
    IFileManager::Get().Delete(*Path);
    return bOK;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCStarEngineFixture, "SCBridge.StarEngine.Fixture",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCStarEngineFixture::RunTest(const FString&)
{
    FSCStarEngine S;
    FString Error;
    TestTrue(TEXT("the plugin fixture loads: ") + Error, FSCStarEngine::Load(FSCStarEngine::FixturePath(), S, Error));
    TestTrue(TEXT("scene unit, glow factor, middle grey and display peak are positive"),
             S.SceneUnitNits > 0 && S.GlowSquareFactor > 0 && S.MiddleGrey > 0 && S.MaxDisplayNitsLDR > 0);
    TestTrue(TEXT("two or more compensation anchors"), S.Compensation.Num() >= 2);
    for (int32 I = 1; I < S.Compensation.Num(); ++I)
        TestTrue(TEXT("anchors ascend"), S.Compensation[I].X > S.Compensation[I - 1].X);
    TestTrue(TEXT("multiplier clamp is a range"), S.MultiplierMin > 0 && S.MultiplierMax > S.MultiplierMin);
    TestTrue(TEXT("half-lives are positive"), S.AdaptTimeDark > 0 && S.AdaptTimeBright > 0 && S.AdaptTimePupil > 0);
    TestTrue(TEXT("meter range is a range"), S.MeterLogMax > S.MeterLogMin);
    TestTrue(TEXT("discard ratios leave a band"), S.DiscardRatioDark >= 0 && S.DiscardRatioBright >= 0 && S.DiscardRatioDark + S.DiscardRatioBright < 1);
    TestTrue(TEXT("glass absorption constants are positive"), S.GlassDensityScale > 0 && S.GlassColourFloor > 0 && S.GlassVertexHeightScale > 0);
    TestTrue(TEXT("glass parameter defaults are in their domain"), S.GlassTilingDefault > 0 && S.GlassVStretchDefault > 0 && S.GlassThinFilmThicknessDefault >= 0);
    FSCStarEngine Synthetic;
    if (TestTrue(TEXT("the synthetic fixture loads: ") + Error, LoadText(SyntheticFixture, Synthetic, Error)))
        TestTrue(TEXT("glass parameter defaults are read from the fixture"),
                 Synthetic.GlassTilingDefault == 4 && Synthetic.GlassVStretchDefault == 0.5 && Synthetic.GlassThinFilmThicknessDefault == 0.25);

    FSCStarEngine Broken;
    TestFalse(TEXT("a fixture without a constant is refused"),
              LoadText(FString(SyntheticFixture).Replace(TEXT("\"sqrGlowFactor\""), TEXT("\"renamed\"")), Broken, Error));
    TestTrue(TEXT("the error names the missing value: ") + Error, Error.Contains(TEXT("shader_constants.sqrGlowFactor")));
    TestFalse(TEXT("one anchor is not a curve"),
              LoadText(FString(SyntheticFixture).Replace(TEXT("r_ExposureCompensationEV10"), TEXT("x10")).Replace(TEXT("r_ExposureCompensationEV20"), TEXT("x20")), Broken, Error));
    TestFalse(TEXT("another schema is refused"), LoadText(FString(SyntheticFixture).Replace(TEXT("render_v1"), TEXT("render_v0")), Broken, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCStarEngineDomain, "SCBridge.StarEngine.FixtureDomain",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCStarEngineDomain::RunTest(const FString&)
{
    // One edit per case takes a value out of the domain the rules need; the loader refuses it and names the rule.
    struct FCase { const TCHAR* What; const TCHAR* From; const TCHAR* To; const TCHAR* Names; };
    const FCase Cases[] = {
        {TEXT("two anchors at one EV"), TEXT(R"("r_ExposureCompensationEV20":{"value":1})"), TEXT(R"("r_ExposureCompensationEV10.0":{"value":1})"), TEXT("two anchors at EV 10")},
        {TEXT("a segment as steep as the metered EV"), TEXT(R"("r_ExposureCompensationEV10":{"value":1})"), TEXT(R"("r_ExposureCompensationEV10":{"value":9})"), TEXT("slope")},
        {TEXT("no pupil half-life"), TEXT(R"("r_ExposureAdaptTimePupil":{"value":0.5})"), TEXT(R"("r_ExposureAdaptTimePupil":{"value":0})"), TEXT("r_ExposureAdaptTime")},
        {TEXT("negative pupil range"), TEXT(R"("r_ExposureAdaptRangePupil":{"value":2})"), TEXT(R"("r_ExposureAdaptRangePupil":{"value":-2})"), TEXT("r_ExposureAdaptRangePupil")},
        {TEXT("inverted meter range"), TEXT(R"("LOG_MAX":{"value":10})"), TEXT(R"("LOG_MAX":{"value":-7})"), TEXT("LOG_MIN < LOG_MAX")},
        {TEXT("discard ratios leave no band"), TEXT(R"("r_ExposureDiscardRatioBright":{"value":0.1})"), TEXT(R"("r_ExposureDiscardRatioBright":{"value":0.95})"), TEXT("r_ExposureDiscardRatio")},
        {TEXT("no glow factor"), TEXT(R"("sqrGlowFactor":{"value":9})"), TEXT(R"("sqrGlowFactor":{"value":0})"), TEXT("sqrGlowFactor")},
        {TEXT("no display peak"), TEXT(R"("MaxDisplayNitsLDR":{"value":50})"), TEXT(R"("MaxDisplayNitsLDR":{"value":0})"), TEXT("MaxDisplayNitsLDR")},
        {TEXT("inverted multiplier clamp"), TEXT(R"("r_ExposureMultiplierMax":{"value":8})"), TEXT(R"("r_ExposureMultiplierMax":{"value":0.001})"), TEXT("r_ExposureMultiplierM")},
        {TEXT("no glass density"), TEXT(R"("glassColourDensityScale":{"value":0.5})"), TEXT(R"("glassColourDensityScale":{"value":0})"), TEXT("glassColourDensityScale")},
        {TEXT("a glass colour floor that divides by zero"), TEXT(R"("glassColourFloor":{"value":0.125})"), TEXT(R"("glassColourFloor":{"value":0})"), TEXT("glassColourFloor")},
        {TEXT("no glass tiling"), TEXT(R"("glassTilingDefault":{"value":4})"), TEXT(R"("glassTilingDefault":{"value":0})"), TEXT("glassTilingDefault")},
        {TEXT("no glass V stretch"), TEXT(R"("glassVStretchDefault":{"value":0.5})"), TEXT(R"("glassVStretchDefault":{"value":0})"), TEXT("glassVStretchDefault")},
        {TEXT("a negative film thickness"), TEXT(R"("glassThinFilmThicknessDefault":{"value":0.25})"), TEXT(R"("glassThinFilmThicknessDefault":{"value":-1})"), TEXT("glassThinFilmThicknessDefault")},
        {TEXT("no vertex height slope"), TEXT(R"("glassVertexHeightScale":{"value":2})"), TEXT(R"("glassVertexHeightScale":{"value":0})"), TEXT("glassVertexHeightScale")},
        {TEXT("a value that is not finite"), TEXT(R"("r_HDRTonemappingCurveToe":{"value":0.5})"), TEXT(R"("r_HDRTonemappingCurveToe":{"value":1e999})"), TEXT("finite")},
    };
    for (const FCase& C : Cases)
    {
        const FString Text = FString(SyntheticFixture).Replace(C.From, C.To, ESearchCase::CaseSensitive);
        FSCStarEngine Broken;
        FString Error;
        TestTrue(FString::Printf(TEXT("%s: the edit applies"), C.What), Text != SyntheticFixture);
        TestFalse(FString::Printf(TEXT("%s: refused"), C.What), LoadText(Text, Broken, Error));
        TestTrue(FString::Printf(TEXT("%s: the error names the rule (%s)"), C.What, *Error), Error.Contains(C.Names));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCStarEngineRules, "SCBridge.StarEngine.Rules",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCStarEngineRules::RunTest(const FString&)
{
    FSCStarEngine S;
    FString Error;
    if (!TestTrue(TEXT("synthetic fixture loads: ") + Error, LoadText(SyntheticFixture, S, Error))) return false;
    const double Pi = UE_DOUBLE_PI;
    TestEqual(TEXT("glow: unit * factor * Glow^2"), S.GlowNits(2.0), 3600.0, 1e-9);
    TestEqual(TEXT("instance glow: unit * amount, linear"), S.InstanceGlowNits(2.0), S.SceneUnitNits * 2.0, 1e-9);
    TestEqual(TEXT("point: pi * unit * I * (1 + bulb)^2"), S.PointCandela(2, 0.5, 1), Pi * 100 * 2 * 2.25, 1e-9);
    TestEqual(TEXT("point: the tweak divides the bulb"), S.PointCandela(2, 0.5, 0.25), Pi * 100 * 2 * 9.0, 1e-9);
    TestEqual(TEXT("point: no bulb"), S.PointCandela(2, 0, 1), Pi * 100 * 2, 1e-9);
    TestEqual(TEXT("planar: pi * unit * I * area"), S.PlanarCandela(2, 0.5, 0.2), Pi * 100 * 2 * 0.1, 1e-9);
    TestEqual(TEXT("scene EV = log2(cd/m2) + offset"), S.SceneEV(4.0), 4.0, 1e-12);
    TestEqual(TEXT("EV round trip"), S.NitsAtEV(S.SceneEV(37.0)), 37.0, 1e-9);
    TestEqual(TEXT("compensation: inside the first segment"), S.CompensationAt(5), 0.0, 1e-12);
    TestEqual(TEXT("compensation: first segment extended"), S.CompensationAt(-10), -3.0, 1e-12);
    TestEqual(TEXT("compensation: flat last segment"), S.CompensationAt(15), 1.0, 1e-12);
    TestEqual(TEXT("compensation: last segment extended"), S.CompensationAt(30), 1.0, 1e-12);
    // Metered EV 5 is 2^(5-2) = 8 cd/m2 = 0.08 scene units with no compensation: multiplier 0.5 / 0.08.
    TestEqual(TEXT("multiplier = middle grey / compensated luminance"), FMath::Pow(2.0, S.LogMultiplierAt(5)), 0.5 / 0.08, 1e-9);
    TestEqual(TEXT("the multiplier inverts"), S.EVAtMultiplier(FMath::Pow(2.0, S.LogMultiplierAt(7))), 7.0, 1e-6);
    return true;
}
#endif
