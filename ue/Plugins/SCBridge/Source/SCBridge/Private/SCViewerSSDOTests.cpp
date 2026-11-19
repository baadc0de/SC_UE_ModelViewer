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
#include "SCViewerEngineFeatures.h"
#include <cmath>
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
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
            {TEXT("DeriveSpecularOcclusion"), TEXT("a382cb5e9e642aa7688017551fba5e9b487db21783c201befc17ba383f63b99c")},
            // E1-7 review: the horizon angles' arc cosines (UE::SSDO::AcosFastGame / AcosEst2Game).
            {TEXT("ACosFast"), TEXT("c9d6eed79c45da942e67035d215e77b104ca67a8b9c5ba0cf76cddc83f93daa5")},
            {TEXT("ACosEst2"), TEXT("311eac5ce581723c7134e16efab450ac28bf3820eccd43e1142edd353674efb7")},
            {TEXT("ASinEst"), TEXT("040a85b518f97fb87ddd4af89cdf079caccfc51a979eef10fd87b7a084dbf193")}};
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
        // E1-7 review: the test restores the developer's setting (which their config may turn on); the shipped default is checked on its
        // own (SCBridge.ViewerExposure.SSDO.ShippedDefault).
        TestEqual(TEXT("the setting is restored"), Settings->bStarEngineSSDO, bSaved);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerSSDOShippedDefault, "SCBridge.ViewerExposure.SSDO.ShippedDefault",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerSSDOShippedDefault::RunTest(const FString&)
{
    // The shipped look keeps the game's SSDO off (D15): the class default, and no shipped config file turns it on. Read in isolation from
    // the user's saved GameUserSettings (a developer may enable it there; E1-7 review).
    TestFalse(TEXT("the class default is off"), USCViewerSettings::bStarEngineSSDOShipped);
    const FString Path = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultGameUserSettings.ini"));
    FConfigFile File;
    File.Read(Path);
    bool bShipped = USCViewerSettings::bStarEngineSSDOShipped;
    File.GetBool(TEXT("/Script/SCBridge.SCViewerSettings"), TEXT("bStarEngineSSDO"), bShipped);
    TestFalse(TEXT("the shipped DefaultGameUserSettings.ini does not turn it on"), bShipped);
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
#if SCVIEWER_ENGINE_SSDO >= 2
namespace
{
// Star Engine's DirOcclusionPS sector bits, transcribed from the game source (DeferredShading.cfxb ACosFast and the visibility-bitmask
// branch; out/e1/E1-7/review/game-ssdo-functions.txt): the reference the engine's port (UE::SSDO::SampleVisibilityBits) is checked
// against.
float GameAcosFast(float X)
{
    const float AbsX = FMath::Abs(X);
    float Res = -0.156583f * AbsX + UE_PI / 2.0f;
    const float OneMinus = 1.0f - AbsX;
    uint32 Bits;
    FMemory::Memcpy(&Bits, &OneMinus, 4);
    Bits = 0x1FBD1DF5u + (Bits >> 1);
    float Sqrt;
    FMemory::Memcpy(&Sqrt, &Bits, 4);
    Res *= Sqrt;
    return X >= 0 ? Res : UE_PI - Res;
}
float GameAcosEst2(float X) // CIGMathLib.cfib: ACosEst2 = 1.570796327 - ASinEst
{
    const float C0 = 1.570796327f, C1 = -0.2125329f, C2 = 0.0747737f, C3 = -0.0188236f;
    const float AbsX = FMath::Abs(X);
    const float Sgn = X / (AbsX + 1e-37f);
    const float Res = (((C3 * AbsX + C2) * AbsX + C1) * AbsX + C0) * FMath::Sqrt(1.0f - AbsX);
    return 1.570796327f - Sgn * (C0 - Res);
}
uint32 GameBits(float FrontCos, float BackCos, float AngleNormal, int32 J)
{
    float X = GameAcosFast(FrontCos), Y = GameAcosFast(BackCos);
    if (!J) { const float NX = -Y, NY = -X; X = NX; Y = NY; }
    const auto S = [AngleNormal](float A) { const float T = FMath::Clamp((A - AngleNormal) / UE_PI + 0.5f, 0.f, 1.f); return T * T * (3.f - 2.f * T); };
    X = S(X); Y = S(Y);
    // HLSL round(): halfway cases to even (review fixes 4).
    const uint32 Start = uint32(X * 32), Span = uint32(FMath::RoundHalfToEven(FMath::Clamp(Y - X, 0.f, 1.f) * 32));
    return ((1u << (Span & 31u)) - 1u) << (Start & 31u);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerSSDOLumenTrace, "SCBridge.ViewerExposure.SSDO.LumenTrace",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerSSDOLumenTrace::RunTest(const FString&)
{
    // E1-7 review (Terra 1-2 / Astra H1-H2): Lumen's short-range trace is produced wherever Lumen runs it unless the SSDO replaces both AO
    // terms and no short-range GI (the same trace) is requested; it is applied during integration only where the SSDO owns no AO term
    // (else the composite applies it to the term the SSDO leaves), so neither term is occluded twice.
    using namespace UE::SSDO;
    FViewSettings Off;
    FViewSettings Ambient; Ambient.bEnabled = true; Ambient.AmbientAmount = 1.f;
    FViewSettings Reflection; Reflection.bEnabled = true; Reflection.ReflectionAmount = 1.f;
    FViewSettings Both = Ambient; Both.ReflectionAmount = 1.f;
    for (const bool bIntegration : {false, true})
    {
        const TCHAR* I = bIntegration ? TEXT("integration on") : TEXT("integration off");
        TestEqual(FString::Printf(TEXT("%s, no SSDO: UE's"), I), AppliesLumenShortRangeAODuringIntegration(Off, bIntegration), bIntegration);
        TestFalse(FString::Printf(TEXT("%s, ambient only: not during integration"), I), AppliesLumenShortRangeAODuringIntegration(Ambient, bIntegration));
        TestFalse(FString::Printf(TEXT("%s, reflection only: not during integration"), I), AppliesLumenShortRangeAODuringIntegration(Reflection, bIntegration));
        TestFalse(FString::Printf(TEXT("%s, both: not during integration"), I), AppliesLumenShortRangeAODuringIntegration(Both, bIntegration));
    }
    for (const bool bGI : {false, true})
    {
        TestTrue(TEXT("no SSDO: the trace runs where Lumen runs it"), RunsLumenShortRangeTrace(Off, true, bGI));
        TestFalse(TEXT("Lumen does not run it: nor does the SSDO"), RunsLumenShortRangeTrace(Ambient, false, bGI));
        TestTrue(TEXT("ambient only: runs (Lumen's reflection AO)"), RunsLumenShortRangeTrace(Ambient, true, bGI));
        TestTrue(TEXT("reflection only: runs (Lumen's diffuse AO)"), RunsLumenShortRangeTrace(Reflection, true, bGI));
        TestEqual(FString::Printf(TEXT("both replaced: runs only for short-range GI (GI %d)"), bGI), RunsLumenShortRangeTrace(Both, true, bGI), bGI);
    }
    // The composite then takes each term once: the SSDO's where it owns it, Lumen's otherwise (ComposeIndirectOcclusion).
    const FIndirectOcclusion Lumen{0.6f, 0.7f};
    const FIndirectOcclusion A = ComposeIndirectOcclusion(Ambient, Lumen, 0.5f, 0.8f, 0.4f);
    TestTrue(TEXT("ambient only: Lumen's specular, once"), A.Specular == Lumen.Specular && A.Diffuse == 0.5f);
    const FIndirectOcclusion R = ComposeIndirectOcclusion(Reflection, Lumen, 0.5f, 0.8f, 0.4f);
    TestTrue(TEXT("reflection only: Lumen's diffuse, once"), R.Diffuse == Lumen.Diffuse);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerSSDOHorizonBits, "SCBridge.ViewerExposure.SSDO.HorizonBits",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerSSDOHorizonBits::RunTest(const FString&)
{
    // E1-7 review (Astra, game parity): the engine port's arc cosines and sector bits are the game's. Astra's case: a front horizon
    // cosine of 0.34 with the projected normal at angle 0 starts at bit 31 (UE's acosFast gave 30).
    using namespace UE::SSDO;
    TestEqual(TEXT("ACosFast(0.34) is the game's"), AcosFastGame(0.34f), GameAcosFast(0.34f), 1e-7f);
    const uint32 Bits = SampleVisibilityBits(0.34f, 0.0f, 0.0f, 1);
    TestEqual(TEXT("front 0.34, normal 0: the game's mask"), Bits, GameBits(0.34f, 0.0f, 0.0f, 1));
    TestEqual(TEXT("front 0.34, normal 0: start bit 31"), Bits ? int32(FMath::CountTrailingZeros(Bits)) : -1, 31);
    // Review fixes 4: a span of exactly 2.5 sectors rounds to even (2 bits), as HLSL round() does; 3.5 rounds to 4.
    TestEqual(TEXT("span 2.5 sectors: 2 bits (ties to even)"), SectorBits(0.0f, 2.5f / 32.0f), 0x3u);
    TestEqual(TEXT("span 3.5 sectors: 4 bits (ties to even)"), SectorBits(0.0f, 3.5f / 32.0f), 0xFu);
    TestEqual(TEXT("span 2.5 from bit 4"), SectorBits(4.0f / 32.0f, 6.5f / 32.0f), 0x30u);
    // A sweep over cosines and normal angles, both sides, at and around the sector boundaries (k / 32 after the smoothstep).
    int32 Mismatches = 0, Cases = 0;
    for (int32 F = -100; F <= 100; ++F)
        for (int32 B = -100; B <= 100; B += 7)
            for (const float N : {-1.2f, -0.6f, 0.0f, 0.35f, 0.9f})
                for (const int32 J : {0, 1})
                {
                    const float FC = F / 100.f, BC = B / 100.f;
                    ++Cases;
                    const uint32 E = SampleVisibilityBits(FC, BC, N, J), G = GameBits(FC, BC, N, J);
                    if (E != G && Mismatches++ < 8)
                        AddInfo(FString::Printf(TEXT("mismatch front %.9g back %.9g normal %g side %d: engine %08x game %08x"), FC, BC, N, J, E, G));
                }
    // Exactly at the boundaries: front cosines whose mapped start is k (bisected per k), and their neighbours one ulp away. With the normal
    // at angle 0 the start runs from 16 (cosine 1) to 32 (cosine -1), so the boundaries inside the domain are k = 17..31.
    for (int32 K = 17; K < 32; ++K)
    {
        float Lo = -1.f, Hi = 1.f; // the mapped start decreases as the cosine grows (j = 1, normal 0)
        for (int32 It = 0; It < 60; ++It)
        {
            const float Mid = 0.5f * (Lo + Hi);
            if (uint32(32 * ([](float A) { const float T = FMath::Clamp(A / UE_PI + 0.5f, 0.f, 1.f); return T * T * (3.f - 2.f * T); })(GameAcosFast(Mid))) >= uint32(K)) Lo = Mid; else Hi = Mid;
        }
        for (const float C : {Lo, Hi, std::nextafter(Lo, -2.f), std::nextafter(Hi, 2.f)})
        {
            if (C < -1.f || C > 1.f) continue; // the game's bit trick is defined on [-1, 1] only
            ++Cases;
            const uint32 E = SampleVisibilityBits(C, -1.f, 0.f, 1), G = GameBits(C, -1.f, 0.f, 1);
            if (E != G && Mismatches++ < 16)
                AddInfo(FString::Printf(TEXT("boundary mismatch front %.9g: engine %08x game %08x"), C, E, G));
        }
    }
    TestEqual(FString::Printf(TEXT("sector bits equal the game's in %d cases"), Cases), Mismatches, 0);
    double WorstEst2 = 0;
    for (int32 I = -1000; I <= 1000; ++I)
        WorstEst2 = FMath::Max(WorstEst2, double(FMath::Abs(AcosEst2Game(I / 1000.f) - GameAcosEst2(I / 1000.f))));
    TestTrue(FString::Printf(TEXT("ACosEst2 is the game's (worst %g)"), WorstEst2), WorstEst2 <= 1e-6);
    return true;
}
#endif
#if SCVIEWER_ENGINE_SSDO >= 3
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCViewerSSDORestrictions, "SCBridge.ViewerExposure.SSDO.Restrictions",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCViewerSSDORestrictions::RunTest(const FString&)
{
    // Review fixes 4 (Astra fixes-3 review 7, the HWRT fallback): a restriction that removes the last amount disables the view (it no longer
    // withholds DFAO); a debug view still runs the pass.
    using namespace UE::SSDO;
    FViewSettings Direct; Direct.bEnabled = true; Direct.DirectAmount = 1.f;
    ApplyRendererRestrictions(Direct, /*bNoDirect*/ true, false);
    TestFalse(TEXT("direct-only under unified toon: disabled"), Direct.bEnabled);
    FViewSettings Debug = FViewSettings(); Debug.bEnabled = true; Debug.DirectAmount = 1.f; Debug.Debug = EDebugView::AmbientOcclusion;
    ApplyRendererRestrictions(Debug, true, false);
    TestTrue(TEXT("a debug view still runs"), Debug.bEnabled && Debug.DirectAmount == 0.f);
    FViewSettings All; All.bEnabled = true; All.DirectAmount = All.AmbientAmount = All.ReflectionAmount = 1.f;
    ApplyRendererRestrictions(All, false, /*bNoIndirect*/ true);
    TestTrue(TEXT("hardware ray traced AO: no indirect replacement, direct kept"), All.bEnabled && All.AmbientAmount == 0.f && All.ReflectionAmount == 0.f && All.DirectAmount == 1.f);
    TestFalse(TEXT("hardware ray traced AO: Lumen's AO not replaced"), All.ReplacesAnyLumenOcclusion());
    FViewSettings Off;
    ApplyRendererRestrictions(Off, true, true);
    TestFalse(TEXT("a disabled view stays disabled"), Off.bEnabled);
    return true;
}
#endif
#endif
