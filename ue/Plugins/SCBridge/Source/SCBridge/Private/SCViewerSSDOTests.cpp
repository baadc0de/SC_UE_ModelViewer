// SCBridge.ViewerExposure.SSDO.*: the game's screen-space directional occlusion as the engine fork's SSDO settings (WP-E1 E1-7).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCViewerExposure.h"
#include "SCViewerSettings.h"
#include "SCStarEngine.h"
#include "Dom/JsonObject.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "SceneView.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ShowFlags.h"
#include "ScreenSpaceDirectionalOcclusionSettings.h"
#include <limits>

namespace
{
// Visibly synthetic values (not the game's).
FSCStarEngine SyntheticSSDOGame()
{
    FSCStarEngine G;
    G.bSSDO = true; G.SSDOQuality = 0; G.SSDORadiusM = 2.5; G.SSDOStrength = 3;
    G.SSDOAmountDirect = 0.25; G.SSDOAmountAmbient = 0.5; G.SSDOAmountReflection = 0.75;
    return G;
}
// The final post-process settings of a view with a map volume (weight 1) and the viewer volume (ViewerWeight) blended over it.
FFinalPostProcessSettings BlendSSDO(const FPostProcessSettings& Map, const FPostProcessSettings& Viewer, float ViewerWeight)
{
    FSceneViewFamily Family(FSceneViewFamily::ConstructionValues(nullptr, nullptr, FEngineShowFlags(ESFIM_Game)).SetTime(FGameTime()));
    FSceneViewInitOptions Init;
    Init.ViewFamily = &Family;
    Init.SetViewRectangle(FIntRect(0, 0, 64, 64));
    Init.ViewOrigin = FVector::ZeroVector;
    Init.ViewRotationMatrix = FMatrix::Identity;
    Init.ProjectionMatrix = FReversedZPerspectiveMatrix(UE_HALF_PI / 2, 1.f, 1.f, 10.f);
    FSceneView View(Init);
    View.StartFinalPostprocessSettings(FVector::ZeroVector);
    View.OverridePostProcessSettings(Map, 1.f);
    View.OverridePostProcessSettings(Viewer, ViewerWeight);
    return View.FinalPostProcessSettings;
}
// Star Engine's CalculateSSDO (ShadeLib.cfi; the WP-L2 port's SCCalculateSSDO), written out for the engine rule to be compared with.
double GameCalculateSSDO(double LightDist, double CameraDist, double CosLightToBent, double Occlusion, double Amount)
{
    const double CosConeAperture = FMath::Sqrt(Occlusion);
    const double LightFadeout = FMath::Clamp(LightDist / CameraDist * 5.0, 0.0, 1.0);
    return 1.0 - FMath::Clamp((CosConeAperture - CosLightToBent) * Amount * LightFadeout, 0.0, 1.0);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerSSDOMapping, "SCBridge.ViewerExposure.SSDO.Mapping",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerSSDOMapping::RunTest(const FString&)
{
    // Fixture -> settings: metres to centimetres, the exponent and the amounts as they are; r_ssdo 0 zeroes the amounts.
    const FSCSSDOSettings S = FSCSSDOSettings::From(SyntheticSSDOGame());
    TestTrue(TEXT("quality 0 is the engine's search"), S.bSupported);
    TestEqual(TEXT("radius in cm"), S.Radius, 250.f);
    TestEqual(TEXT("strength"), S.Strength, 3.f);
    TestTrue(TEXT("amounts"), S.DirectAmount == 0.25f && S.AmbientAmount == 0.5f && S.ReflectionAmount == 0.75f);
    FSCStarEngine Off = SyntheticSSDOGame();
    Off.bSSDO = false;
    const FSCSSDOSettings O = FSCSSDOSettings::From(Off);
    TestTrue(TEXT("r_ssdo 0: no amounts"), O.DirectAmount == 0.f && O.AmbientAmount == 0.f && O.ReflectionAmount == 0.f);
    FSCStarEngine High = SyntheticSSDOGame();
    High.SSDOQuality = 1;
    const FSCSSDOSettings H = FSCSSDOSettings::From(High);
    TestFalse(TEXT("another game quality is not ported"), H.bSupported);

    // The fixture's own values.
    const FSCStarEngine& Game = FSCStarEngine::Get();
    const FSCSSDOSettings F = FSCSSDOSettings::From(Game);
    TestEqual(TEXT("fixture: supported iff r_ssdoQuality 0"), F.bSupported, Game.SSDOQuality == 0);
    TestEqual(TEXT("fixture radius"), double(F.Radius), Game.SSDORadiusM * 100.0, 1e-4);
    TestEqual(TEXT("fixture strength"), double(F.Strength), Game.SSDOStrength, 1e-6);
    TestEqual(TEXT("fixture direct amount"), double(F.DirectAmount), Game.bSSDO ? Game.SSDOAmountDirect : 0.0, 1e-6);
    TestEqual(TEXT("fixture ambient amount"), double(F.AmbientAmount), Game.bSSDO ? Game.SSDOAmountAmbient : 0.0, 1e-6);
    TestEqual(TEXT("fixture reflection amount"), double(F.ReflectionAmount), Game.bSSDO ? Game.SSDOAmountReflection : 0.0, 1e-6);

    // Apply: game = every field overridden with the game's; off = the amounts overridden to 0, radius / strength UE's, not overridden.
    const FPostProcessSettings& UE = FPostProcessSettings::GetDefault();
    FPostProcessSettings P;
    S.Apply(P, true);
    TestTrue(TEXT("game: radius"), P.bOverride_SSDORadius && P.SSDORadius == 250.f);
    TestTrue(TEXT("game: strength"), P.bOverride_SSDOStrength && P.SSDOStrength == 3.f);
    TestTrue(TEXT("game: amounts"), P.bOverride_SSDODirectAmount && P.SSDODirectAmount == 0.25f && P.bOverride_SSDOAmbientAmount &&
             P.SSDOAmbientAmount == 0.5f && P.bOverride_SSDOReflectionAmount && P.SSDOReflectionAmount == 0.75f);
    S.Apply(P, false);
    TestTrue(TEXT("off: amounts overridden to 0"), P.bOverride_SSDODirectAmount && P.SSDODirectAmount == 0.f && P.bOverride_SSDOAmbientAmount &&
             P.SSDOAmbientAmount == 0.f && P.bOverride_SSDOReflectionAmount && P.SSDOReflectionAmount == 0.f);
    TestTrue(TEXT("off: radius and strength UE's"), !P.bOverride_SSDORadius && P.SSDORadius == UE.SSDORadius && !P.bOverride_SSDOStrength &&
             P.SSDOStrength == UE.SSDOStrength);
    FPostProcessSettings Q;
    H.Apply(Q, true);
    TestTrue(TEXT("unsupported quality: off even when the game's is asked for"), Q.SSDODirectAmount == 0.f && Q.SSDOAmbientAmount == 0.f &&
             Q.SSDOReflectionAmount == 0.f && !Q.bOverride_SSDORadius);

    // UE's default is no SSDO: the engine resolves default settings to a disabled view.
    const UE::SSDO::FViewSettings Default = UE::SSDO::ResolveViewSettings(UE, UE::SSDO::FConsoleSettings(), true);
    TestFalse(TEXT("UE default: no SSDO pass"), Default.bEnabled);
    TestTrue(TEXT("UE default: zero amounts"), UE.SSDODirectAmount == 0.f && UE.SSDOAmbientAmount == 0.f && UE.SSDOReflectionAmount == 0.f);

    // The engine's SSDO (ScreenSpaceDirectionalOcclusion.usf / Common.ush) is a port of these functions with these token hashes
    // (scripts/verify_star_engine_render.py re-derives the fixture's from the installed game). A game patch that changes one fails here:
    // port the new function into the engine.
    FString Text;
    TSharedPtr<FJsonObject> Root;
    const bool bRead = FFileHelper::LoadFileToString(Text, *FSCStarEngine::FixturePath()) &&
                       FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) && Root;
    const TSharedPtr<FJsonObject>* Functions = nullptr;
    if (TestTrue(TEXT("fixture has shader functions"), bRead && Root->TryGetObjectField(TEXT("shader_functions"), Functions)))
    {
        const TPair<const TCHAR*, const TCHAR*> Ported[] = {
            {TEXT("DirOcclusionPS"), TEXT("6d0b80fa581928697ce816a3e809f43d7399f8227ebdecbaef36999a744b2f86")},
            {TEXT("GetSSDORadius"), TEXT("e9f5b1cde58cc2688ae88488e068364a81b2f1076202d211287ab7bc05c8a504")},
            {TEXT("CalculateSSDO"), TEXT("588ccce3f95a3918dd43960be6e62ecb6ae0db83065791c6b0712f85f80a2552")},
            {TEXT("DeriveSpecularOcclusion"), TEXT("a382cb5e9e642aa7688017551fba5e9b487db21783c201befc17ba383f63b99c")}};
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerSSDOBlend, "SCBridge.ViewerExposure.SSDO.Blend",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerSSDOBlend::RunTest(const FString&)
{
    const FSCSSDOSettings S = FSCSSDOSettings::From(SyntheticSSDOGame());
    const UE::SSDO::FConsoleSettings Console;
    FPostProcessSettings Empty;
    FPostProcessSettings OptIn; // a map volume that asks for SSDO itself
    OptIn.bOverride_SSDODirectAmount = true; OptIn.SSDODirectAmount = 1.f;
    OptIn.bOverride_SSDOAmbientAmount = true; OptIn.SSDOAmbientAmount = 1.f;
    {
        // The look (off) over that map: the viewer volume's 0 wins.
        FPostProcessSettings Viewer;
        S.Apply(Viewer, false);
        const FFinalPostProcessSettings F = BlendSSDO(OptIn, Viewer, 1.f);
        TestTrue(TEXT("off over an opt-in map: no amounts"), F.SSDODirectAmount == 0.f && F.SSDOAmbientAmount == 0.f);
        TestFalse(TEXT("off over an opt-in map: no pass"), UE::SSDO::ResolveViewSettings(F, Console, true).bEnabled);
        // UE's blending without the viewer volume: the map's opt-in reaches the view.
        const FFinalPostProcessSettings M = BlendSSDO(OptIn, Empty, 1.f);
        TestTrue(TEXT("a map volume alone turns it on"), UE::SSDO::ResolveViewSettings(M, Console, true).bEnabled && M.SSDOAmbientAmount == 1.f);
    }
    {
        // The game's SSDO at full weight, then at half weight over a map without SSDO: amounts and radius interpolate from UE's defaults.
        FPostProcessSettings Viewer;
        S.Apply(Viewer, true);
        const UE::SSDO::FViewSettings V = UE::SSDO::ResolveViewSettings(BlendSSDO(Empty, Viewer, 1.f), Console, true);
        TestTrue(TEXT("game: enabled with the game's values"), V.bEnabled && V.Radius == 250.f && V.Strength == 3.f && V.DirectAmount == 0.25f &&
                 V.AmbientAmount == 0.5f && V.ReflectionAmount == 0.75f);
        const FPostProcessSettings& UE = FPostProcessSettings::GetDefault();
        const FFinalPostProcessSettings Half = BlendSSDO(Empty, Viewer, 0.5f);
        TestEqual(TEXT("half weight: radius lerps"), Half.SSDORadius, 0.5f * (UE.SSDORadius + 250.f), 1e-3f);
        TestEqual(TEXT("half weight: strength lerps"), Half.SSDOStrength, 0.5f * (UE.SSDOStrength + 3.f), 1e-5f);
        TestEqual(TEXT("half weight: ambient amount lerps from 0"), Half.SSDOAmbientAmount, 0.25f, 1e-6f);
        TestEqual(TEXT("half weight: reflection amount lerps from 0"), Half.SSDOReflectionAmount, 0.375f, 1e-6f);
    }
    {
        // The installed viewer volume follows the setting: off by default (the look, D15), the game's with bStarEngineSSDO.
        USCViewerSettings* Settings = USCViewerSettings::Get();
        const bool bSaved = Settings->bStarEngineSSDO;
        for (const bool bOn : {false, true})
        {
            Settings->bStarEngineSSDO = bOn;
            UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
            const APostProcessVolume* Volume = SCViewerExposure::Install(*World);
            if (TestNotNull(TEXT("the volume spawns"), Volume))
            {
                const FSCSSDOSettings G = FSCSSDOSettings::From(FSCStarEngine::Get());
                const FPostProcessSettings& P = Volume->Settings;
                const bool bExpected = bOn && G.bSupported;
                TestTrue(FString::Printf(TEXT("install %d: amounts overridden"), bOn), P.bOverride_SSDODirectAmount && P.bOverride_SSDOAmbientAmount && P.bOverride_SSDOReflectionAmount);
                TestEqual(FString::Printf(TEXT("install %d: ambient amount"), bOn), P.SSDOAmbientAmount, bExpected ? G.AmbientAmount : 0.f);
                TestEqual(FString::Printf(TEXT("install %d: radius overridden"), bOn), bool(P.bOverride_SSDORadius), bExpected);
            }
            World->DestroyWorld(false);
        }
        Settings->bStarEngineSSDO = bSaved;
        TestFalse(TEXT("the shipped look keeps the game's SSDO off"), GetDefault<USCViewerSettings>()->bStarEngineSSDO);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerSSDORules, "SCBridge.ViewerExposure.SSDO.Rules",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerSSDORules::RunTest(const FString&)
{
    using namespace UE::SSDO;
    FPostProcessSettings P;
    P.SSDOAmbientAmount = 0.5f;
    FConsoleSettings C;
    TestTrue(TEXT("an amount runs the pass"), ResolveViewSettings(P, C, true).bEnabled);
    TestFalse(TEXT("unsupported view (mobile, forward, path tracer, reflection capture): never"), ResolveViewSettings(P, C, false).bEnabled);
    C.Mode = EMode::Off;
    const FViewSettings Off = ResolveViewSettings(P, C, true);
    TestTrue(TEXT("r.SSDO 0: off, amounts 0"), !Off.bEnabled && Off.AmbientAmount == 0.f);
    C.Mode = EMode::Force;
    const FViewSettings Forced = ResolveViewSettings(FPostProcessSettings(), C, true);
    TestTrue(TEXT("r.SSDO 2: on with amounts 1"), Forced.bEnabled && Forced.DirectAmount == 1.f && Forced.AmbientAmount == 1.f && Forced.ReflectionAmount == 1.f);
    TestEqual(TEXT("r.SSDO 2 keeps a set amount"), ResolveViewSettings(P, C, true).AmbientAmount, 0.5f);
    C.Mode = EMode::PostProcess;
    C.Debug = EDebugView::AmbientOcclusion;
    const FViewSettings Debug = ResolveViewSettings(FPostProcessSettings(), C, true);
    TestTrue(TEXT("debug view runs the pass without amounts"), Debug.bEnabled && Debug.DirectAmount == 0.f && Debug.AmbientAmount == 0.f);
    TestFalse(TEXT("a debug view replaces nothing"), Debug.ReplacesLumenDiffuseOcclusion() || Debug.ReplacesLumenSpecularOcclusion());
    C.Mode = EMode::Off;
    TestFalse(TEXT("r.SSDO 0 wins over the debug view"), ResolveViewSettings(FPostProcessSettings(), C, true).bEnabled);
    C = FConsoleSettings();
    C.Quality = 7;
    FPostProcessSettings Bad;
    Bad.SSDODirectAmount = -1.f; Bad.SSDOAmbientAmount = std::numeric_limits<float>::quiet_NaN(); Bad.SSDOReflectionAmount = 0.25f; Bad.SSDORadius = 0.f; Bad.SSDOStrength = -2.f;
    const FViewSettings B = ResolveViewSettings(Bad, C, true);
    TestTrue(TEXT("negative / non-finite amounts count as 0"), B.DirectAmount == 0.f && B.AmbientAmount == 0.f && B.ReflectionAmount == 0.25f);
    TestEqual(TEXT("radius clamped"), B.Radius, MinRadius);
    TestEqual(TEXT("strength clamped"), B.Strength, 0.f);
    TestEqual(TEXT("quality clamped"), B.Quality, MaxQuality);
    TestTrue(TEXT("quality table"), QualitySlices[DefaultQuality] == 3 && QualitySamplesPerSide[DefaultQuality] == 6);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerSSDOLumenInterplay, "SCBridge.ViewerExposure.SSDO.LumenInterplay",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerSSDOLumenInterplay::RunTest(const FString&)
{
    using namespace UE::SSDO;
    const FIndirectOcclusion Lumen{0.6f, 0.7f};
    const float W = 0.5f, NoV = 0.8f, Roughness = 0.4f;
    FViewSettings V;
    V.bEnabled = true;
    // No ambient / reflection amount: Lumen's short-range AO as UE has it, computed.
    {
        const FIndirectOcclusion O = ComposeIndirectOcclusion(V, Lumen, W, NoV, Roughness);
        TestTrue(TEXT("no amounts: Lumen's terms"), O.Diffuse == Lumen.Diffuse && O.Specular == Lumen.Specular);
        TestEqual(TEXT("no amounts: Lumen's AO computed"), GetLumenShortRangeAOIntensity(V, 0.9f), 0.9f);
    }
    // The game's amounts (1, 1): both terms are the SSDO's alone (no double darkening: never the product with Lumen's), and Lumen's
    // short-range AO is not computed.
    V.AmbientAmount = 1.f; V.ReflectionAmount = 1.f;
    {
        const FIndirectOcclusion O = ComposeIndirectOcclusion(V, Lumen, W, NoV, Roughness);
        TestEqual(TEXT("diffuse: 1 - w"), O.Diffuse, 0.5f, 1e-6f);
        TestEqual(TEXT("specular: derived from 1 - w"), O.Specular, DeriveSpecularOcclusion(NoV, 0.5f, 1.f - Roughness), 1e-6f);
        TestTrue(TEXT("not Lumen's times the SSDO's"), FMath::Abs(O.Diffuse - Lumen.Diffuse * 0.5f) > 0.1f);
        const FIndirectOcclusion Unoccluded = ComposeIndirectOcclusion(V, FIndirectOcclusion{0.2f, 0.2f}, 0.f, NoV, Roughness);
        TestEqual(TEXT("Lumen's darkening does not reach through"), Unoccluded.Diffuse, 1.f);
        TestEqual(TEXT("Lumen's AO skipped"), GetLumenShortRangeAOIntensity(V, 0.9f), 0.f);
    }
    // One term: that term is the SSDO's, the other Lumen's, and Lumen's AO is still computed for it.
    V.ReflectionAmount = 0.f;
    {
        const FIndirectOcclusion O = ComposeIndirectOcclusion(V, Lumen, W, NoV, Roughness);
        TestTrue(TEXT("ambient only: SSDO diffuse, Lumen specular"), O.Diffuse == 0.5f && O.Specular == Lumen.Specular);
        TestEqual(TEXT("ambient only: Lumen's AO computed"), GetLumenShortRangeAOIntensity(V, 0.9f), 0.9f);
    }
    V.bEnabled = false;
    TestTrue(TEXT("disabled view: Lumen's"), ComposeIndirectOcclusion(V, Lumen, W, NoV, Roughness).Diffuse == Lumen.Diffuse);
    TestEqual(TEXT("disabled view: Lumen's AO"), GetLumenShortRangeAOIntensity(V, 0.9f), 0.9f);

    // Specular occlusion: full AO leaves specular whole; the curve is the game's (DeriveSpecularOcclusion, TiledShading.cfxb).
    TestEqual(TEXT("AO 1: no specular occlusion"), DeriveSpecularOcclusion(0.3f, 1.f, 0.2f), 1.f, 1e-6f);
    const float Expected = FMath::Clamp(FMath::Pow(0.3f + 0.4f, FMath::Exp2(-16.f * (1.f - 0.6f) - 1.f)) - 1.f + 0.4f, 0.f, 1.f);
    TestEqual(TEXT("the game's curve"), DeriveSpecularOcclusion(0.3f, 0.4f, 0.6f), Expected, 1e-6f);

    // Direct light: the engine's factor is the game's CalculateSSDO over a sweep of occlusions, angles and distances.
    double Worst = 0;
    for (const double Occ : {0.0, 0.1, 0.5, 0.9, 1.0})
        for (const double Cos : {-1.0, -0.3, 0.0, 0.4, 0.95, 1.0})
            for (const double LightDist : {1.0, 50.0, 1e8})
                for (const double Amount : {0.0, 0.5, 1.0, 2.0})
                    Worst = FMath::Max(Worst, FMath::Abs(DirectLightFactor(float(Occ), float(Cos), float(LightDist), 200.f, float(Amount)) -
                                                         GameCalculateSSDO(LightDist, 200.0, Cos, Occ, Amount)));
    TestTrue(FString::Printf(TEXT("direct light factor = CalculateSSDO (worst %.2g)"), Worst), Worst < 1e-5);
    TestEqual(TEXT("amount 0: unoccluded"), DirectLightFactor(1.f, -1.f, 1e8f, 100.f, 0.f), 1.f);
    return true;
}
#endif
