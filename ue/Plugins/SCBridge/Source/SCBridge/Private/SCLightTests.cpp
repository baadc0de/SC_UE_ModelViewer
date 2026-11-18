// Regression fixtures for instance-local shadow budgets, authored default light states and the game's light model.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCLightImporter.h"
#include "SCViewerSettings.h"
#include "SCVehicle.h"
#include "Engine/World.h"
#include "Components/LightComponent.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "SCLightAreas.h"
#include "SCStarEngine.h"
#include "Algo/AllOf.h"
#include "LightDefinitions.h"
#include "LightSceneProxy.h"
#include "Misc/ScopeExit.h"
#include "SceneManagement.h"
#include "SpotLightSceneProxyDesc.h"
#include "SCViewerEngineObjectVersion.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "Misc/Parse.h"

namespace
{
// The tests check the game's authored rules (shadows, WP-L1 room confinement); the look settings (D15) are pinned meanwhile.
struct FAuthoredShadowRule
{
    bool bSaved = USCViewerSettings::Get()->bShadowAllLights, bSavedConfine = USCViewerSettings::Get()->bConfineLightsToRooms;
    FAuthoredShadowRule() { USCViewerSettings::Get()->bShadowAllLights = false; USCViewerSettings::Get()->bConfineLightsToRooms = true; }
    ~FAuthoredShadowRule() { USCViewerSettings::Get()->bShadowAllLights = bSaved; USCViewerSettings::Get()->bConfineLightsToRooms = bSavedConfine; }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightDefaults, "SCBridge.Lights.DefaultsAndShadowBudget",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightDefaults::RunTest(const FString&)
{
    const FAuthoredShadowRule AuthoredShadows;
    auto* MegaLights = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MegaLights.EnableForProject"));
    const int32 Previous = MegaLights->GetInt();
    MegaLights->Set(0, ECVF_SetByCode);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    auto* Vehicle = World->SpawnActor<ASCVehicle>();
    FSCFolderPackageSource Source(TEXT("/tmp"));
    TArray<FString> Warnings;
    const auto Helper = [](const FString&) -> USceneComponent* { return nullptr; };
    FSCSceneInstance Instance;
    for (int32 Radius = 1; Radius <= 4; ++Radius)
        Instance.LightJson.Add(FString::Printf(
            TEXT(
                R"({"name":"Radius%d","semantic_light_kind":"point","transform_basis":"cryengine_z_up","radius_m":%d,"intensity_candela_proxy":25})"),
            Radius, Radius));
    Instance.LightJson.Add(TEXT(R"({"name":"Ambient","semantic_light_kind":"ambient_proxy","radius_m":100})"));
    FSCLightImporter::Build(Source, Instance, Vehicle, Vehicle->GetRootComponent(), Helper, Warnings);
    TestEqual(TEXT("four local components"), Vehicle->Lights.Num(), 4);
    for (int32 I = 0; I < 4; ++I)
        TestEqual(TEXT("upper quartile selects only radius four"), bool(Vehicle->Lights[I]->CastShadows), I == 3);
    FSCSceneInstance Other;
    Other.LightJson.Add(TEXT(
        R"({"name":"OtherInstance","semantic_light_kind":"point","transform_basis":"cryengine_z_up","radius_m":0.5,"intensity_candela_proxy":25})"));
    FSCLightImporter::Build(Source, Other, Vehicle, Vehicle->GetRootComponent(), Helper, Warnings);
    TestTrue(TEXT("threshold is per instance, not per ship"), Vehicle->Lights.Last()->CastShadows);
    FSCSceneInstance States;
    for (
        const TCHAR* State :
        {TEXT(R"("active_state":"offState","states":{"offState":{"intensity_candela_proxy":99}})"),
         TEXT(
             R"("active_state":"auxiliaryState","states":{"defaultState":{"intensity_candela_proxy":0},"auxiliaryState":{"intensity_candela_proxy":25}})"),
         TEXT(R"("states":{"defaultState":{"intensity_candela_proxy":0}})"),
         TEXT(R"("active_state":"defaultState","states":{"defaultState":{"intensity_candela_proxy":25}})")})
    {
        States.LightJson.Add(FString::Printf(
            TEXT(
                R"({"name":"StateFixture","semantic_light_kind":"point","transform_basis":"cryengine_z_up","radius_m":2,"intensity_candela_proxy":25,%s})"),
            State));
    }
    const int32 Begin = Vehicle->Lights.Num();
    FSCLightImporter::Build(Source, States, Vehicle, Vehicle->GetRootComponent(), Helper, Warnings);
    for (int32 I = 0; I < 4; ++I)
        TestEqual(TEXT("off and zero-default states import disabled"), Vehicle->Lights[Begin + I]->IsVisible(), I == 3);
    TestEqual(TEXT("one state property per component"), Vehicle->LightStates.Num(), Vehicle->Lights.Num());
    TestEqual(TEXT("export fallback preserved for UI"), Vehicle->LightStates[Begin + 1].ExportActiveState, FName(TEXT("auxiliaryState")));
    TestEqual(TEXT("viewer uses authored default"), Vehicle->LightStates[Begin + 1].ActiveState, FName(TEXT("defaultState")));
    TestTrue(TEXT("full state map preserved for UI"), Vehicle->LightStates[Begin + 1].StatesJson.Contains(TEXT("auxiliaryState")));
    // Null-RHI cannot provide MegaLights even when the project preference is on.
    MegaLights->Set(1, ECVF_SetByCode);
    Vehicle->RefreshLightShadowPolicy();
    if (FParse::Param(FCommandLine::Get(), TEXT("nullrhi")))
    {
        TestFalse(TEXT("null-RHI does not masquerade as active MegaLights"), Vehicle->bMegaLightsActive);
        TestFalse(TEXT("runtime refresh retains fallback on null-RHI"), Vehicle->Lights[0]->CastShadows);
    }
    TestEqual(TEXT("valid fixtures have no warnings"), Warnings.Num(), 0);
    Vehicle->Destroy();
    World->DestroyWorld(false);
    MegaLights->Set(Previous, ECVF_SetByCode);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightAuthoredModel, "SCBridge.Lights.AuthoredCryEngineModel",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightAuthoredModel::RunTest(const FString&)
{
    const FAuthoredShadowRule AuthoredShadows;
    // Synthetic lights (not game data). Point / spot: pi * unit * I * (1 + bulb / tweak)^2; planar: pi * unit * I * w * h,
    // unit = the scene unit in cd/m2 from the extracted fixture.
    const double Unit = UE_DOUBLE_PI * FSCStarEngine::Get().SceneUnitNits;
    auto* MegaLights = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MegaLights.EnableForProject"));
    const int32 Previous = MegaLights->GetInt();
    MegaLights->Set(0, ECVF_SetByCode);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    auto* Vehicle = World->SpawnActor<ASCVehicle>();
    FSCFolderPackageSource Source(TEXT("/tmp"));
    TArray<FString> Warnings;
    const auto Helper = [](const FString&) -> USceneComponent* { return nullptr; };
    FSCSceneInstance Instance;
    const TCHAR* Common = TEXT(R"("transform_basis":"cryengine_z_up","radius_m":3,"intensity_candela_proxy":1500,"plane_width_m":0.5,"plane_height_m":0.2)");
    Instance.LightJson.Add(FString::Printf(TEXT(R"({"name":"Bulb","semantic_light_kind":"point",%s,"intensity_raw":2,"bulb_radius_m":0.1,"shadow_casting":true,)"
                                                R"("active_state":"defaultState","states":{"defaultState":{"intensity_raw":2,"intensity_candela_proxy":3000}}})"), Common));
    Instance.LightJson.Add(FString::Printf(TEXT(R"({"name":"Tiny","semantic_light_kind":"spot",%s,"intensity_raw":2,"bulb_radius_m":0.01,"shadow_casting":false,"outer_angle":40,"inner_angle":30})"), Common));
    Instance.LightJson.Add(FString::Printf(TEXT(R"({"name":"Panel","semantic_light_kind":"area",%s,"intensity_raw":2,"bulb_radius_m":0,"shadow_casting":false})"), Common));
    Instance.LightJson.Add(TEXT(R"({"name":"Legacy","semantic_light_kind":"point","transform_basis":"cryengine_z_up","radius_m":3,"intensity_candela_proxy":25})"));
    Instance.LightJson.Add(FString::Printf(TEXT(R"({"name":"Tweaked","semantic_light_kind":"spot",%s,"intensity_raw":2,"bulb_radius_m":0.1,"attenuation_tweak":0.25,"shadow_casting":false,"outer_angle":40,"inner_angle":30})"), Common));
    Instance.LightJson.Add(FString::Printf(TEXT(R"({"name":"NoBulb","semantic_light_kind":"point",%s,"intensity_raw":2,"bulb_radius_m":0,"shadow_casting":false})"), Common));
    FSCLightImporter::Build(Source, Instance, Vehicle, Vehicle->GetRootComponent(), Helper, Warnings);
    TestEqual(TEXT("six components"), Vehicle->Lights.Num(), 6);
    const auto Candela = [&](int32 Index) { return double(Vehicle->Lights[Index]->Intensity); };
    TestEqual(TEXT("point: reached 1 m from the bulb surface"), Candela(0), Unit * 2.0 * FMath::Square(1.1), Unit * 1e-5);
    TestEqual(TEXT("point: small bulb"), Candela(1), Unit * 2.0 * FMath::Square(1.01), Unit * 1e-5);
    TestEqual(TEXT("planar: luminance pi * I over the plane"), Candela(2), Unit * 2.0 * 0.5 * 0.2, Unit * 1e-5);
    TestEqual(TEXT("legacy packages keep the proxy"), Candela(3), 25.0, 0.001);
    TestEqual(TEXT("attenuation tweak scales the bulb term"), Candela(4), Unit * 2.0 * FMath::Square(1.0 + 0.1 / 0.25), Unit * 1e-5);
    TestEqual(TEXT("no bulb: the authored intensity at 1 m"), Candela(5), Unit * 2.0, Unit * 1e-5);
    TestEqual(TEXT("bulb is the source radius (cm)"), double(Cast<UPointLightComponent>(Vehicle->Lights[0])->SourceRadius), 10.0, 0.001);
    TestEqual(TEXT("plane is the rect source (cm)"), double(Cast<URectLightComponent>(Vehicle->Lights[2])->SourceWidth), 50.0, 0.001);
    // GetPhysicalLightAttenuation is the engine fork's bulb offset law (WP-E1 E1-2): candela / (max(d, b) + c)^2 with the
    // bulb b and the tweak's distance offset c = b (1 - tweak) / tweak, both in cm, on authored point and spot lights.
    const auto* Bulb = Cast<ULocalLightComponent>(Vehicle->Lights[0]);
    const auto* Tweaked = Cast<ULocalLightComponent>(Vehicle->Lights[4]);
    TestEqual(TEXT("authored point: bulb offset law"), Bulb->DistanceAttenuation, ELightDistanceAttenuation::BulbOffset);
    TestEqual(TEXT("authored point: the bulb (cm)"), double(Bulb->AttenuationBulbRadius), 10.0, 0.001);
    TestEqual(TEXT("tweak 1: no offset"), double(Bulb->AttenuationDistanceOffset), 0.0, 1e-9);
    TestEqual(TEXT("authored spot: bulb offset law"), Cast<ULocalLightComponent>(Vehicle->Lights[1])->DistanceAttenuation, ELightDistanceAttenuation::BulbOffset);
    TestEqual(TEXT("tweaked spot: bulb offset law"), Tweaked->DistanceAttenuation, ELightDistanceAttenuation::BulbOffset);
    TestEqual(TEXT("tweak offset is carried (cm)"), double(Tweaked->AttenuationDistanceOffset), 30.0, 0.001);
    TestEqual(TEXT("planar: UE's area law"), Cast<ULocalLightComponent>(Vehicle->Lights[2])->DistanceAttenuation, ELightDistanceAttenuation::Default);
    TestEqual(TEXT("legacy packages: UE's law"), Cast<ULocalLightComponent>(Vehicle->Lights[3])->DistanceAttenuation, ELightDistanceAttenuation::Default);
    TestEqual(TEXT("no bulb: no bulb offset law"), Cast<ULocalLightComponent>(Vehicle->Lights[5])->DistanceAttenuation, ELightDistanceAttenuation::Default);
    // SoftSourceRadius keeps its UE meaning (the soft specular radius); the importer leaves it alone.
    TestEqual(TEXT("soft source radius untouched"), double(Cast<UPointLightComponent>(Vehicle->Lights[4])->SoftSourceRadius), 0.0, 1e-9);
    // GetLightForcedDistanceFade is every local light's range fade: the squared linear tail over the fixture's fraction.
    for (int32 I = 0; I < 6; ++I)
    {
        const auto* Local = Cast<ULocalLightComponent>(Vehicle->Lights[I]);
        TestEqual(TEXT("forced fade: squared linear tail"), Local->RangeFade, ELightRangeFade::SquaredLinearTail);
        TestEqual(TEXT("forced fade: the fixture's fraction"), double(Local->RangeFadeFraction), FSCStarEngine::Get().ForcedFadeFraction, 1e-6);
    }
    TestTrue(TEXT("fallback shadows follow shadow_casting (on)"), bool(Vehicle->Lights[0]->CastShadows));
    TestFalse(TEXT("fallback shadows follow shadow_casting (off)"), bool(Vehicle->Lights[1]->CastShadows));
    TestEqual(TEXT("no warnings"), Warnings.Num(), 0);
    Vehicle->Destroy();
    World->DestroyWorld(false);
    MegaLights->Set(Previous, ECVF_SetByCode);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightProjectorShape, "SCBridge.Lights.ProjectorShape",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightProjectorShape::RunTest(const FString&)
{
    // The game's projector is its texture over the square frustum of half-angle FOV / 2 (border sampler, no cone falloff). The
    // engine fork's SquareFrustum spot light (WP-E1 E1-3) is that frustum for OuterConeAngle = FOV / 2: the light function's UVs
    // span the square (0 and 1 at tan(FOV / 2)), culling / bounds / shadows use the cone through its corners, and the shaders get
    // the square through the packed spot angles and the light model word (LightDefinitions.h).
    const double HalfFov = 50.0, Tan = FMath::Tan(FMath::DegreesToRadians(HalfFov));
    const double Corner = FMath::Atan(UE_DOUBLE_SQRT_2 * Tan);
    // The proxies need a scene: a registered component in a test world.
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    USpotLightComponent* Spot = NewObject<USpotLightComponent>(World);
    Spot->InnerConeAngle = 20.f;
    Spot->OuterConeAngle = float(HalfFov);
    Spot->RegisterComponentWithWorld(World);
    ON_SCOPE_EXIT { Spot->UnregisterComponent(); World->DestroyWorld(false); };
    TestTrue(TEXT("UE default: cone"), Spot->Shape == ESpotLightShape::Cone);
    TestTrue(TEXT("UE default: wrapping light function"), Spot->LightFunctionAddress == ELightFunctionAddress::Wrap);
    TestEqual(TEXT("UE default: no near clip"), Spot->ProjectorNearClip, 0.f);
    TestEqual(TEXT("UE default: bounding cone is the clamped outer cone"), Spot->GetHalfConeAngle(), Spot->GetClampedConeAngles().Y);
    const auto Packed = [](const USpotLightComponent* S, FLightRenderParameters& P, float& LightFunctionTan, float& BoundingCone)
    {
        FLightSceneProxy* Proxy = S->CreateSceneProxy();
        if (!Proxy) return false;
        Proxy->GetLightShaderParameters(P);
        LightFunctionTan = Proxy->GetLightFunctionTanOuterAngle();
        BoundingCone = Proxy->GetOuterConeAngle();
        delete Proxy;
        return true;
    };
    FLightRenderParameters P;
    float LightFunctionTan = 0, BoundingCone = 0;
    if (!TestTrue(TEXT("cone proxy"), Packed(Spot, P, LightFunctionTan, BoundingCone))) return false;
    const FVector2f Clamped = Spot->GetClampedConeAngles();
    TestEqual(TEXT("cone: UE's spot angles"), double(P.SpotAngles.Y), 1.0 / (FMath::Cos(Clamped.X) - FMath::Cos(Clamped.Y)), 1e-3);
    TestEqual(TEXT("cone: the model word is UE's (all zero)"), FMath::AsUInt(P.GetPackedDistanceFalloff().Y) & 0xFFFFFFF0u, 0u);
    TestEqual(TEXT("cone: light function spans the outer cone"), double(LightFunctionTan), FMath::Tan(double(Clamped.Y)), 1e-6);

    Spot->Shape = ESpotLightShape::SquareFrustum;
    Spot->LightFunctionAddress = ELightFunctionAddress::Border;
    Spot->ProjectorNearClip = 25.f;
    TestEqual(TEXT("square: bounding cone through the corners"), double(Spot->GetHalfConeAngle()), Corner, 1e-5);
    if (!TestTrue(TEXT("square proxy"), Packed(Spot, P, LightFunctionTan, BoundingCone))) return false;
    TestEqual(TEXT("square: proxy culls with the corner cone"), double(BoundingCone), Corner, 1e-5);
    TestEqual(TEXT("square: light function UVs span the square (edge at tan(FOV / 2))"), double(LightFunctionTan), Tan, 1e-5);
    TestEqual(TEXT("square: SpotAngles.x is the corner cone"), double(P.SpotAngles.X), FMath::Cos(Corner), 1e-5);
    TestEqual(TEXT("square: SpotAngles.y is tan(FOV / 2)"), double(P.SpotAngles.Y), Tan, 1e-5);
    const uint32 Word = FMath::AsUInt(P.GetPackedDistanceFalloff().Y);
    TestEqual(TEXT("square: shape in the model word"), (Word >> LIGHT_MODEL_WORD_BIT_OFFSET_SPOT_SHAPE) & LIGHT_MODEL_WORD_MASK_SPOT_SHAPE, uint32(LIGHT_SPOT_SHAPE_SQUARE_FRUSTUM));
    TestEqual(TEXT("square: near clip in the model word (half, cm)"),
              uint32((Word >> LIGHT_MODEL_WORD_BIT_OFFSET_PROJECTOR_NEAR_CLIP) & LIGHT_MODEL_WORD_MASK_PROJECTOR_NEAR_CLIP), uint32(FFloat16(25.f).Encoded));
    // The near clip is packed as a half (E1-3 review): the setter and the proxy descriptor clamp to the half's range (65504 cm) with
    // a warning, so the property never promises a depth the packed light cannot hold.
    AddExpectedMessage(TEXT("above the packed range"), EAutomationExpectedMessageFlags::Contains, 0);
    Spot->SetMobility(EComponentMobility::Movable);
    Spot->SetProjectorNearClip(100000.f);
    TestEqual(TEXT("near clip: the setter clamps to the packed range"), Spot->ProjectorNearClip, 65504.f);
    Spot->ProjectorNearClip = 100000.f; // as a serialized value would arrive
    TestEqual(TEXT("near clip: the proxy descriptor clamps to the packed range"), USpotLightComponent::BuildSceneProxyDesc(*Spot).ProjectorNearClip, 65504.f);
    Spot->ProjectorNearClip = 25.f;
    // The square's corner direction lies on the bounding cone: a probe just inside it is inside the bounds, a probe outside is not.
    Spot->SetWorldLocation(FVector::ZeroVector);
    const FVector CornerDir = FVector(1, Tan, Tan).GetSafeNormal();
    const FVector Lateral = FVector(0, 1, 1).GetSafeNormal();
    const double R = Spot->AttenuationRadius;
    TestTrue(TEXT("square: the corner is inside the bounds"), Spot->AffectsBounds(FBoxSphereBounds(CornerDir * 0.5 * R - Lateral * 2.0, FVector(1), 1)));
    TestFalse(TEXT("square: past the corner is outside the bounds"), Spot->AffectsBounds(FBoxSphereBounds(CornerDir * 0.5 * R + Lateral * 0.05 * R, FVector(1), 1)));
    // UE's limit stays: a 90-degree half-FOV is an infinite frustum.
    const float Wide = USpotLightComponent::GetClampedSquareFrustumHalfAngle(90.f);
    TestEqual(TEXT("limit: half-FOV clamped like UE's outer cone"), double(Wide), FMath::DegreesToRadians(88.9) + 0.001, 1e-6);
    TestTrue(TEXT("limit: the corner cone stays below 90 degrees"), USpotLightComponent::GetSquareFrustumEnclosingConeAngle(Wide) < UE_HALF_PI);
    TestEqual(TEXT("solid angle of a 90-degree (half 45) square frustum: 4 asin(1/2) = 2 pi / 3"),
              double(USpotLightComponent::GetSquareFrustumSolidAngle(UE_PI / 4)), 2.0 * UE_DOUBLE_PI / 3.0, 1e-5);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightProxyDescArchive, "SCBridge.Lights.ProxyDescArchive",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightProxyDescArchive::RunTest(const FString&)
{
    // FastGeo writes light descriptors without property tags. The fork's fields (E1-2 falloff models, E1-3 shape, near clip and
    // light function address) are versioned (FSCViewerEngineObjectVersion): an archive written with UE's layout or E1-2's loads
    // with UE's defaults for what it lacks and still reads every field after them.
    FSpotLightSceneProxyDesc Out;
    Out.LightFunctionAddress = LIGHT_FUNCTION_ADDRESS_BORDER;
    Out.AttenuationRadius = 500.f;
    Out.RangeFadeFraction = 0.2f;
    Out.bBulbOffsetAttenuation = true;
    Out.AttenuationBulbRadius = 3.f;
    Out.AttenuationDistanceOffset = -1.f;
    Out.SourceRadius = 7.f;
    Out.bUseInverseSquaredFalloff = true;
    Out.InnerConeAngle = 12.f;
    Out.OuterConeAngle = 34.f;
    Out.Shape = uint8(ESpotLightShape::SquareFrustum);
    Out.ProjectorNearClip = 25.f;
    const auto RoundTrip = [&Out](int32 WriterVersion, FSpotLightSceneProxyDesc& In, bool& bConsumedAll)
    {
        TArray<uint8> Bytes;
        FObjectWriter Writer(Bytes);
        if (WriterVersion >= 0)
        {
            Writer.SetCustomVersion(FSCViewerEngineObjectVersion::GUID, WriterVersion, TEXT("SCViewerEngineObjectVer"));
            Writer.SetShouldSkipUpdateCustomVersion(true);
        }
        Out.Serialize(Writer);
        FObjectReader Reader(Bytes);
        Reader.SetCustomVersions(Writer.GetCustomVersions());
        In.Serialize(Reader);
        bConsumedAll = Reader.Tell() == Reader.TotalSize() && !Reader.IsError();
    };
    bool bAll = false;
    FSpotLightSceneProxyDesc Latest;
    RoundTrip(-1, Latest, bAll);
    TestTrue(TEXT("latest: every byte read"), bAll);
    TestEqual(TEXT("latest: light function address"), int32(Latest.LightFunctionAddress), int32(LIGHT_FUNCTION_ADDRESS_BORDER));
    TestEqual(TEXT("latest: range fade"), Latest.RangeFadeFraction, 0.2f);
    TestTrue(TEXT("latest: bulb offset"), Latest.bBulbOffsetAttenuation && Latest.AttenuationDistanceOffset == -1.f);
    TestEqual(TEXT("latest: shape"), int32(Latest.Shape), int32(ESpotLightShape::SquareFrustum));
    TestEqual(TEXT("latest: near clip"), Latest.ProjectorNearClip, 25.f);

    FSpotLightSceneProxyDesc UE;
    RoundTrip(FSCViewerEngineObjectVersion::BeforeCustomVersionWasAdded, UE, bAll);
    TestTrue(TEXT("UE layout: every byte read"), bAll);
    TestEqual(TEXT("UE layout: Wrap"), int32(UE.LightFunctionAddress), int32(LIGHT_FUNCTION_ADDRESS_WRAP));
    TestEqual(TEXT("UE layout: UE's range fade"), UE.RangeFadeFraction, 0.f);
    TestFalse(TEXT("UE layout: UE's distance law"), UE.bBulbOffsetAttenuation);
    TestEqual(TEXT("UE layout: cone"), int32(UE.Shape), int32(ESpotLightShape::Cone));
    TestEqual(TEXT("UE layout: no near clip"), UE.ProjectorNearClip, 0.f);
    TestEqual(TEXT("UE layout: fields after the fork's still read (attenuation radius)"), UE.AttenuationRadius, 500.f);
    TestEqual(TEXT("UE layout: fields after the fork's still read (source radius)"), UE.SourceRadius, 7.f);
    TestEqual(TEXT("UE layout: fields after the fork's still read (cone angles)"), UE.OuterConeAngle, 34.f);

    FSpotLightSceneProxyDesc E12;
    RoundTrip(FSCViewerEngineObjectVersion::LocalLightFalloffModels, E12, bAll);
    TestTrue(TEXT("E1-2 layout: every byte read"), bAll);
    TestEqual(TEXT("E1-2 layout: falloff read"), E12.RangeFadeFraction, 0.2f);
    TestEqual(TEXT("E1-2 layout: cone (no shape yet)"), int32(E12.Shape), int32(ESpotLightShape::Cone));
    TestEqual(TEXT("E1-2 layout: Wrap (no address yet)"), int32(E12.LightFunctionAddress), int32(LIGHT_FUNCTION_ADDRESS_WRAP));
    TestEqual(TEXT("E1-2 layout: cone angles"), E12.InnerConeAngle, 12.f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightVerifier, "SCBridge.Lights.VerifierFollowsImporter",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightVerifier::RunTest(const FString&)
{
    const FAuthoredShadowRule AuthoredShadows;
    auto* MegaLights = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MegaLights.EnableForProject"));
    const int32 Previous = MegaLights->GetInt();
    MegaLights->Set(0, ECVF_SetByCode);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    auto* Vehicle = World->SpawnActor<ASCVehicle>();
    FSCFolderPackageSource Source(TEXT("/tmp"));
    TArray<FString> Warnings;
    const auto Helper = [](const FString&) -> USceneComponent* { return nullptr; };
    // Records the importer builds nothing for sit between the lights and carry the largest radii: they may neither
    // shift the verifier's component index nor move its shadow quartile.
    FSCSceneInstance Instance;
    const auto Record = [&Instance](const TCHAR* Name, const TCHAR* Kind, const TCHAR* Basis, int32 Radius) {
        Instance.LightJson.Add(FString::Printf(
            TEXT(R"({"name":"%s","semantic_light_kind":"%s","transform_basis":"%s","radius_m":%d,"intensity_candela_proxy":25})"), Name, Kind, Basis, Radius));
    };
    Record(TEXT("Radius1"), TEXT("point"), TEXT("cryengine_z_up"), 1);
    Record(TEXT("Ambient"), TEXT("ambient_proxy"), TEXT("cryengine_z_up"), 50);
    Record(TEXT("Radius2"), TEXT("point"), TEXT("cryengine_z_up"), 2);
    Record(TEXT("UnknownKind"), TEXT("synthetic_unknown_kind"), TEXT("cryengine_z_up"), 60);
    Record(TEXT("Radius3"), TEXT("point"), TEXT("cryengine_z_up"), 3);
    Record(TEXT("ForeignBasis"), TEXT("point"), TEXT("synthetic_basis"), 70);
    Record(TEXT("Radius4"), TEXT("point"), TEXT("cryengine_z_up"), 4);
    Vehicle->Manifest.Root = Instance;
    FSCLightImporter::Build(Source, Instance, Vehicle, Vehicle->GetRootComponent(), Helper, Warnings);
    TestEqual(TEXT("four lights are built"), Vehicle->Lights.Num(), 4);
    TestEqual(TEXT("the unknown kind and the foreign basis warn"), Warnings.Num(), 2);
    TestTrue(TEXT("the verifier accepts what the importer built"), FSCLightImporter::Verify(Vehicle));
    if (Vehicle->Lights.Num() == 4)
    {
        auto* Light = Cast<ULocalLightComponent>(Vehicle->Lights[0]);
        const float Built = Light->Intensity;
        Light->SetIntensity(Built * 2);
        TestFalse(TEXT("a light at twice its rule intensity is refused"), FSCLightImporter::Verify(Vehicle));
        Light->SetIntensity(Built);
        Light->IntensityUnits = ELightUnits::Lumens;
        TestFalse(TEXT("a light in another unit is refused"), FSCLightImporter::Verify(Vehicle));
        Light->IntensityUnits = ELightUnits::Candelas;
        TestTrue(TEXT("restored, it passes again"), FSCLightImporter::Verify(Vehicle));
    }
    Vehicle->Destroy();
    World->DestroyWorld(false);
    MegaLights->Set(Previous, ECVF_SetByCode);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightRoomVolumes, "SCBridge.Lights.RoomVolumes",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightRoomVolumes::RunTest(const FString&)
{
    const FAuthoredShadowRule AuthoredShadows;
    // Synthetic shapes (not game data). `transform` is column-major segment -> container, metres, CryEngine axes.
    FString Error;
    FSCRoomVolume Box;
    TestTrue(TEXT("box parses"), FSCRoomVolume::Parse(TEXT(R"({"room":"a","room_name":"Room","source":"Shape","kind":"box",)"
        R"("transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[10,0,0,1]],"box_min":[-1,-2,-3],"box_max":[1,2,3]})"), Box, Error));
    TestEqual(TEXT("labels kept"), Box.RoomName + TEXT("/") + Box.Source + TEXT("/") + Box.Room, FString(TEXT("Room/Shape/a")));
    TestTrue(TEXT("box: translated centre"), Box.Contains(FVector(10, 0, 0)));
    TestTrue(TEXT("box: near a corner"), Box.Contains(FVector(10.9, 1.9, 2.9)));
    TestFalse(TEXT("box: container origin is outside"), Box.Contains(FVector(0, 0, 0)));
    TestFalse(TEXT("box: past +x"), Box.Contains(FVector(11.5, 0, 0)));
    // Quarter turn about z: segment x -> container y, segment y -> container -x.
    FSCRoomVolume Turned;
    TestTrue(TEXT("rotated box parses"), FSCRoomVolume::Parse(TEXT(R"({"room":"a","room_name":null,"source":"s","kind":"box",)"
        R"("transform":[[0,1,0,0],[-1,0,0,0],[0,0,1,0],[0,0,0,1]],"box_min":[0,0,0],"box_max":[4,1,1]})"), Turned, Error));
    TestTrue(TEXT("rotated box: segment (3,.5,.5) is container (-.5,3,.5)"), Turned.Contains(FVector(-0.5, 3, 0.5)));
    TestFalse(TEXT("rotated box: the unrotated point is outside"), Turned.Contains(FVector(3, 0.5, 0.5)));
    // L-shaped polygon extruded from its lowest point (z 1) by 3.
    FSCRoomVolume Prism;
    TestTrue(TEXT("prism parses"), FSCRoomVolume::Parse(TEXT(R"({"room":"a","source":"s","kind":"prism","height":3,)"
        R"("transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],"points":[[0,0,1],[4,0,1],[4,2,1],[2,2,1],[2,4,1],[0,4,1]]})"), Prism, Error));
    TestTrue(TEXT("prism: first arm"), Prism.Contains(FVector(3, 1, 2)));
    TestTrue(TEXT("prism: second arm"), Prism.Contains(FVector(1, 3, 2)));
    TestFalse(TEXT("prism: the notch"), Prism.Contains(FVector(3, 3, 2)));
    TestFalse(TEXT("prism: below the base"), Prism.Contains(FVector(1, 1, 0.5)));
    TestFalse(TEXT("prism: above base + height"), Prism.Contains(FVector(1, 1, 4.5)));
    // Closed tetrahedron, three vertices per face.
    FSCRoomVolume Mesh;
    TestTrue(TEXT("trimesh parses"), FSCRoomVolume::Parse(TEXT(R"({"room":"a","source":"s","kind":"trimesh",)"
        R"("transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],"vertices":[[0,0,0],[4,0,0],[0,4,0],[0,0,0],[0,4,0],[0,0,4],)"
        R"([0,0,0],[0,0,4],[4,0,0],[4,0,0],[0,0,4],[0,4,0]]})"), Mesh, Error));
    TestTrue(TEXT("trimesh: inside"), Mesh.Contains(FVector(0.5, 0.5, 0.5)));
    TestFalse(TEXT("trimesh: beyond the slanted face"), Mesh.Contains(FVector(3, 3, 3)));
    TestFalse(TEXT("trimesh: behind a face"), Mesh.Contains(FVector(-1, 0.5, 0.5)));
    // The +Z ray from here enters and leaves the solid: two crossings are still outside.
    TestFalse(TEXT("trimesh: outside with the solid ahead"), Mesh.Contains(FVector(0.5, 0.5, -0.7)));
    FSCRoomVolume Bad;
    const TCHAR* Identity = TEXT(R"("transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]])");
    // Closed unit cube, top split along a diagonal or fanned around its centre (the exporter's room_containment
    // tests): a ray through a shared edge or vertex counts once, a wall seen edge-on never.
    auto Cube = [&](bool bFan)
    {
        FString V;
        auto Tri = [&V](FVector A, FVector B, FVector C)
        { for (const FVector& Q : {A, B, C}) V += FString::Printf(TEXT("%s[%g,%g,%g]"), V.IsEmpty() ? TEXT("") : TEXT(","), Q.X, Q.Y, Q.Z); };
        auto Quad = [&Tri](FVector A, FVector B, FVector C, FVector D) { Tri(A, B, C); Tri(A, C, D); };
        Quad({0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}); Quad({0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1});
        Quad({1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}); Quad({0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1});
        Quad({0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1});
        const FVector Rim[] = {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
        if (bFan) for (int32 I = 0; I < 4; ++I) Tri(Rim[I], Rim[(I + 1) % 4], FVector(0.5, 0.5, 1));
        else Quad(Rim[0], Rim[1], Rim[2], Rim[3]);
        FSCRoomVolume Solid;
        TestTrue(TEXT("cube parses"), FSCRoomVolume::Parse(FString::Printf(TEXT(R"({"room":"a","source":"s","kind":"trimesh",%s,"vertices":[%s]})"), Identity, *V), Solid, Error));
        return Solid;
    };
    for (const bool bFan : {false, true})
    {
        const FSCRoomVolume Solid = Cube(bFan);
        int32 Wrong = 0;
        for (int32 X = 1; X < 8; ++X) for (int32 Y = 1; Y < 8; ++Y) for (int32 Z = 1; Z < 8; ++Z) Wrong += !Solid.Contains(FVector(X, Y, Z) / 8.0);
        TestEqual(bFan ? TEXT("trimesh: interior lattice points left out of the fanned cube") : TEXT("trimesh: interior lattice points left out of the split cube"), Wrong, 0);
        Wrong = 0;
        for (const double X : {-0.5, 0.0, 0.25, 0.5, 1.0, 1.5}) for (const double Y : {-0.5, 0.0, 0.25, 0.5, 1.0, 1.5})
            for (const double Z : {-1.0, -0.125, 1.125, 2.0}) Wrong += Solid.Contains(FVector(X, Y, Z));
        TestEqual(bFan ? TEXT("trimesh: exterior points taken into the fanned cube") : TEXT("trimesh: exterior points taken into the split cube"), Wrong, 0);
    }
    TestTrue(TEXT("trimesh: oblique (2,3,4) ray through the top diagonal"), Cube(false).Contains(FVector(0.5, 0.375, 0.5)));
    for (const FString& Text :
         {FString(TEXT("not json")),
          FString::Printf(TEXT(R"({%s,"box_min":[0,0,0],"box_max":[1,1,1]})"), Identity),                   // no kind
          FString::Printf(TEXT(R"({"kind":"sphere",%s})"), Identity),                                          // unknown kind
          FString(TEXT(R"({"kind":"box","transform":[[1,0,0],[0,1,0],[0,0,1]],"box_min":[0,0,0],"box_max":[1,1,1]})")), // not 4x4
          FString(TEXT(R"({"kind":"box","transform":[[0,0,0,0],[0,0,0,0],[0,0,0,0],[0,0,0,1]],"box_min":[0,0,0],"box_max":[1,1,1]})")), // singular
          FString::Printf(TEXT(R"({"kind":"box",%s,"box_min":[0,0],"box_max":[1,1,1]})"), Identity),         // short vector
          FString::Printf(TEXT(R"({"kind":"prism",%s,"height":2,"points":[[0,0,0],[1,0,0]]})"), Identity),    // not a polygon
          FString::Printf(TEXT(R"({"kind":"prism",%s,"points":[[0,0,0],[1,0,0],[0,1,0]]})"), Identity),       // no height
          FString::Printf(TEXT(R"({"kind":"trimesh",%s,"vertices":[[0,0,0],[1,0,0],[0,1,0],[0,0,1]]})"), Identity)}) // not triangles
    {
        Error.Reset();
        TestFalse(TEXT("malformed volume rejected: ") + Text, FSCRoomVolume::Parse(Text, Bad, Error));
        TestFalse(TEXT("rejection carries a reason"), Error.IsEmpty());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightAreaConfinement, "SCBridge.Lights.AreaConfinement",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightAreaConfinement::RunTest(const FString&)
{
    const FAuthoredShadowRule AuthoredShadows;
    // Synthetic scene (not game data). Channel 0 = outside every room volume; rooms a, b, c (container x -1..1, 1..3,
    // 3..5) form a light-reach chain a-b-c, so a and c share channel 1 and b takes channel 2.
    auto Channels = [](const FLightingChannels& C) { return int32(C.bChannel0) | int32(C.bChannel1) << 1 | int32(C.bChannel2) << 2; };
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FSCFolderPackageSource Source(TEXT("/tmp"));
    TArray<FString> Warnings;
    const auto Helper = [](const FString&) -> USceneComponent* { return nullptr; };
    // The environment: a light that belongs to no vehicle.
    AActor* Hangar = World->SpawnActor<AActor>();
    auto* HangarLight = NewObject<UPointLightComponent>(Hangar);
    Hangar->SetRootComponent(HangarLight);
    HangarLight->RegisterComponent();
    // Container point (0,3,0) m is anchor point (300,0,0) cm (CryEngine x/y swap of the light importer).
    FSCSceneInstance Instance;
    const TCHAR* Common = TEXT(R"("semantic_light_kind":"point","transform_basis":"cryengine_z_up","radius_m":2,"intensity_candela_proxy":25)");
    Instance.LightJson.Add(FString::Printf(TEXT(R"({"name":"InsideArea",%s,"position":[0,3,0],"affects_this_area_only":true})"), Common));
    Instance.LightJson.Add(FString::Printf(TEXT(R"({"name":"OutsideArea",%s,"position":[3,0,0],"affects_this_area_only":true})"), Common));
    Instance.LightJson.Add(FString::Printf(TEXT(R"({"name":"Everywhere",%s,"position":[0,3,0],"affects_this_area_only":false})"), Common));
    Instance.LightJson.Add(FString::Printf(TEXT(R"({"name":"Unflagged",%s,"position":[3,0,0]})"), Common));
    Instance.LightJson.Add(FString::Printf(TEXT(R"({"name":"InRoomB",%s,"position":[2,3,0],"affects_this_area_only":true})"), Common));
    Instance.LightJson.Add(FString::Printf(TEXT(R"({"name":"InRoomC",%s,"position":[4,3,0],"affects_this_area_only":true})"), Common));
    const FString Volume = TEXT(R"({"room":"a","source":"s","kind":"box","transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],)"
                                R"("box_min":[-1,2,-1],"box_max":[1,4,1]})");
    const FString VolumeB = TEXT(R"({"room":"b","source":"s","kind":"box","transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],)"
                                 R"("box_min":[1,2,-1],"box_max":[3,4,1]})");
    const FString VolumeC = TEXT(R"({"room":"c","source":"s","kind":"box","transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],)"
                                 R"("box_min":[3,2,-1],"box_max":[5,4,1]})");
    auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    auto* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    auto* Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
    TestTrue(TEXT("engine shapes load"), Cube && Sphere && Cone);
    auto AddMesh = [](ASCVehicle* V, UStaticMesh* Mesh, const FVector& Location) {
        auto* C = NewObject<UStaticMeshComponent>(V);
        C->SetMobility(EComponentMobility::Movable);
        C->SetupAttachment(V->GetRootComponent());
        C->SetStaticMesh(Mesh);
        C->SetRelativeLocation(Location);
        C->RegisterComponent();
        return C;
    };

    // A vehicle without room volumes keeps the engine defaults everywhere.
    auto* Plain = World->SpawnActor<ASCVehicle>();
    FSCLightImporter::Build(Source, Instance, Plain, Plain->GetRootComponent(), Helper, Warnings);
    auto* PlainMesh = AddMesh(Plain, Cube, FVector(300, 0, 0));
    FSCLightAreas::Apply(Plain);
    for (const ULightComponent* Light : Plain->Lights) TestEqual(TEXT("no volumes: light untouched"), Channels(Light->LightingChannels), 1);
    TestEqual(TEXT("no volumes: mesh untouched"), Channels(PlainMesh->LightingChannels), 1);
    TestEqual(TEXT("no volumes: environment untouched"), Channels(HangarLight->LightingChannels), 1);
    TestFalse(TEXT("no volumes: nothing is inside"), Plain->IsInsideLightArea(Plain->GetActorTransform().TransformPosition(FVector(300, 0, 0))));

    auto* Vehicle = World->SpawnActor<ASCVehicle>();
    Vehicle->SetActorLocation(FVector(5000, 0, 0)); // classification is relative to the placed vehicle
    FSCLightImporter::Build(Source, Instance, Vehicle, Vehicle->GetRootComponent(), Helper, Warnings);
    FSCLightAreas::Register(Vehicle, Vehicle->GetRootComponent(), {Volume, VolumeB, VolumeC, TEXT(R"({"kind":"sphere"})")}, false,
                            Warnings);
    TestEqual(TEXT("a malformed volume warns and is dropped"), Warnings.Num(), 1);
    TestEqual(TEXT("one area with three volumes"), Vehicle->LightAreas.Num() == 1 ? Vehicle->LightAreas[0].Volumes.Num() : -1, 3);
    // Samples are mesh-local cm; a component is inside when more than half of them are.
    Vehicle->MeshSamples.Add(Cube, {FVector3f(0, 0, 0), FVector3f(50, 0, 0), FVector3f(-2000, 0, 0)});   // 2 of 3 inside at (300,0,0)
    Vehicle->MeshSamples.Add(Sphere, {FVector3f(0, 0, 0), FVector3f(2000, 0, 0), FVector3f(-2000, 0, 0), FVector3f(0, 2000, 0)}); // 1 of 4
    auto* Mostly = AddMesh(Vehicle, Cube, FVector(300, 0, 0));
    auto* Hull = AddMesh(Vehicle, Sphere, FVector(300, 0, 0));
    auto* Split = AddMesh(Vehicle, Cube, FVector(380, 0, 0));                                           // 1 of 3 inside from here
    auto* UnsampledIn = AddMesh(Vehicle, Cone, FVector(300, 0, 0));                                      // no samples: bounds origin
    auto* UnsampledOut = AddMesh(Vehicle, Cone, FVector(0, 300, 0));
    auto* InB = AddMesh(Vehicle, Cube, FVector(300, 200, 0)); // container (2,3): room b (2 of 3 samples inside)
    auto* InC = AddMesh(Vehicle, Cube, FVector(300, 400, 0)); // container (4,3): room c
    FSCLightAreas::Apply(Vehicle);
    TestEqual(TEXT("six lights"), Vehicle->Lights.Num(), 6);
    TestEqual(TEXT("area-only light in room a: channel 1 only"), Channels(Vehicle->Lights[0]->LightingChannels), 2);
    TestEqual(TEXT("area-only light outside: channel 0 only"), Channels(Vehicle->Lights[1]->LightingChannels), 1);
    TestEqual(TEXT("affects everything: all channels"), Channels(Vehicle->Lights[2]->LightingChannels), 7);
    TestEqual(TEXT("flag absent: all channels"), Channels(Vehicle->Lights[3]->LightingChannels), 7);
    TestEqual(TEXT("area-only light in room b (reached from a and c): channel 2 only"), Channels(Vehicle->Lights[4]->LightingChannels), 4);
    TestEqual(TEXT("area-only light in room c (not reached from a): channel 1, shared with a"),
              Channels(Vehicle->Lights[5]->LightingChannels), 2);
    TestEqual(TEXT("light state records its room"), Vehicle->LightStates[4].AreaRoom, FString(TEXT("b")));
    TestEqual(TEXT("mesh in room b: channel 2"), Channels(InB->LightingChannels), 4);
    TestEqual(TEXT("mesh in room c: channel 1"), Channels(InC->LightingChannels), 2);
    TestEqual(TEXT("world query names the room"), Vehicle->LightRoomAt(FVector(5300, 200, 0)), FString(TEXT("b")));
    TestEqual(TEXT("world query gives the room's channel"), Vehicle->LightChannelAt(FVector(5300, 200, 0)), 2);
    TestEqual(TEXT("world query outside is channel 0"), Vehicle->LightChannelAt(FVector(300, 0, 0)), 0);
    TestTrue(TEXT("state records the flag"), Vehicle->LightStates[0].bAreaOnly && Vehicle->LightStates[1].bAreaOnly &&
                                                  !Vehicle->LightStates[2].bAreaOnly && !Vehicle->LightStates[3].bAreaOnly);
    TestTrue(TEXT("state records the side"), Vehicle->LightStates[0].bInsideArea && !Vehicle->LightStates[1].bInsideArea &&
                                                  Vehicle->LightStates[2].bInsideArea && !Vehicle->LightStates[3].bInsideArea &&
                                                  Vehicle->LightStates[4].bInsideArea && Vehicle->LightStates[5].bInsideArea);
    TestEqual(TEXT("majority of samples inside: channel 1"), Channels(Mostly->LightingChannels), 2);
    TestEqual(TEXT("minority of samples inside: channel 0"), Channels(Hull->LightingChannels), 1);
    TestEqual(TEXT("same mesh, different placement: channel 0"), Channels(Split->LightingChannels), 1);
    TestEqual(TEXT("unsampled mesh inside by its bounds origin"), Channels(UnsampledIn->LightingChannels), 2);
    TestEqual(TEXT("unsampled mesh outside by its bounds origin"), Channels(UnsampledOut->LightingChannels), 1);
    TestEqual(TEXT("environment light reaches every channel"), Channels(HangarLight->LightingChannels), 7);
    TestTrue(TEXT("world query inside"), Vehicle->IsInsideLightArea(FVector(5300, 0, 0)));
    TestFalse(TEXT("world query outside"), Vehicle->IsInsideLightArea(FVector(300, 0, 0)));
    Plain->Destroy();
    Vehicle->Destroy();
    Hangar->Destroy();
    World->DestroyWorld(false);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightRoomChannels, "SCBridge.Lights.RoomChannels",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightRoomChannels::RunTest(const FString&)
{
    const FAuthoredShadowRule AuthoredShadows;
    // Rooms take channel 1 or 2; rooms a light reaches across never share one when the reach graph allows it.
    int32 Conflicts = -1;
    auto Chain = FSCLightAreas::ColorRooms({TEXT("a"), TEXT("b"), TEXT("c")}, {{TEXT("a"), TEXT("b")}, {TEXT("c"), TEXT("b")}}, Conflicts);
    TestEqual(TEXT("chain: a"), Chain.FindRef(TEXT("a")), 1);
    TestEqual(TEXT("chain: b"), Chain.FindRef(TEXT("b")), 2);
    TestEqual(TEXT("chain: c shares a's channel"), Chain.FindRef(TEXT("c")), 1);
    TestEqual(TEXT("chain: no conflicts"), Conflicts, 0);
    auto Cycle = FSCLightAreas::ColorRooms({TEXT("a"), TEXT("b"), TEXT("c")},
                                           {{TEXT("a"), TEXT("b")}, {TEXT("b"), TEXT("c")}, {TEXT("c"), TEXT("a")}}, Conflicts);
    TestEqual(TEXT("odd cycle: one pair must share"), Conflicts, 1);
    TestTrue(TEXT("odd cycle: channels stay 1 or 2"), Cycle.Num() == 3 && Algo::AllOf(Cycle, [](const auto& P) { return P.Value == 1 || P.Value == 2; }));
    auto Apart = FSCLightAreas::ColorRooms({TEXT("a"), TEXT("b")}, {}, Conflicts);
    TestEqual(TEXT("unreached rooms still spread: a"), Apart.FindRef(TEXT("a")), 1);
    TestEqual(TEXT("unreached rooms still spread: b"), Apart.FindRef(TEXT("b")), 2);
    TestEqual(TEXT("unreached rooms: no conflicts"), Conflicts, 0);
    // Weighted odd cycle: the pair with the fewest reaching lights shares a channel (here a-b: 1 + 1 lights).
    TArray<TPair<FString, FString>> Weighted = {{TEXT("a"), TEXT("b")}, {TEXT("b"), TEXT("a")}};
    for (int32 I = 0; I < 5; ++I) Weighted.Add({TEXT("b"), TEXT("c")});
    for (int32 I = 0; I < 9; ++I) Weighted.Add({TEXT("c"), TEXT("a")});
    auto Lightest = FSCLightAreas::ColorRooms({TEXT("a"), TEXT("b"), TEXT("c")}, Weighted, Conflicts);
    TestEqual(TEXT("weighted cycle: the lightest pair shares"), Lightest.FindRef(TEXT("a")), Lightest.FindRef(TEXT("b")));
    TestNotEqual(TEXT("weighted cycle: the heaviest pair is apart"), Lightest.FindRef(TEXT("a")), Lightest.FindRef(TEXT("c")));
    TestEqual(TEXT("weighted cycle: one conflicting pair"), Conflicts, 1);
    auto Self = FSCLightAreas::ColorRooms({TEXT("a")}, {{TEXT("a"), TEXT("a")}}, Conflicts);
    TestEqual(TEXT("a room reaching itself is no conflict"), Conflicts, 0);
    TestEqual(TEXT("single room: channel 1"), Self.FindRef(TEXT("a")), 1);
    return true;
}

namespace
{
// The light grid's cell estimate (LightCulling.ush EstimateLocalLight, engine fork WP-E1 E1-2), statement for statement, with the
// range window and distance law of LocalLightFalloff.ush. RangeFadeScale 0: UE's window; > 0: 1 / F (squared linear tail).
double EngineRangeFade(double DistSq, double InvRadius, double RangeFadeScale)
{
    if (RangeFadeScale > 0) return FMath::Square(FMath::Clamp(RangeFadeScale * (1.0 - FMath::Sqrt(DistSq) * InvRadius), 0.0, 1.0));
    return FMath::Square(FMath::Clamp(1.0 - FMath::Square(DistSq * FMath::Square(InvRadius)), 0.0, 1.0));
}
double EngineDistanceLaw(double DistSq, double DistBiasSq, bool bBulb, double B, double C)
{
    return bBulb ? 1.0 / FMath::Square(FMath::Max(FMath::Sqrt(DistSq), B) + C) : 1.0 / (DistSq + DistBiasSq);
}
double EngineEstimateLocalLight(double DistSq, double Power, double InvRadius, double FalloffExponent, double RangeFadeScale, bool bBulb, double B, double C)
{
    if (FalloffExponent <= 0 && bBulb)
        return Power * EngineRangeFade(DistSq, InvRadius, RangeFadeScale) * EngineDistanceLaw(DistSq, 0, bBulb, B, C) / UE_DOUBLE_PI;
    const double TrueDistSq = DistSq;
    DistSq = FMath::Max(DistSq, 0.01);
    double Falloff = 1;
    if (FalloffExponent > 0)
        Falloff = FMath::Pow(1.0 - FMath::Clamp(DistSq * FMath::Square(InvRadius), 0.0, 1.0), FalloffExponent);
    else
        Falloff = EngineRangeFade(RangeFadeScale > 0 ? TrueDistSq : DistSq, InvRadius, RangeFadeScale) * EngineDistanceLaw(DistSq, 0, bBulb, B, C);
    return Power * Falloff / UE_DOUBLE_PI;
}
double ProbeLux(const UWorld* World, const FVector& Point)
{
    const TArray<FString> Lines = FSCLightImporter::Probe(World, Point, nullptr, 0);
    double Lux = -1;
    if (Lines.Num()) FParse::Value(*Lines[0], TEXT("lux_unshadowed="), Lux);
    return Lux;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightGridEstimate, "SCBridge.Lights.LightGridEstimate",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightGridEstimate::RunTest(const FString&)
{
    // The cell estimate is evaluated at the cell's nearest point (0 inside a cell that contains the light) and must not cull a cell
    // the light lights (Astra E1-1..E1-4 review 4): a tiny range-fade-only light (SquaredLinearTail, UE's inverse square, radius
    // 0.05 cm) has a positive window everywhere inside its radius.
    const double Radius = 0.05, InvRadius = 1.0 / Radius, Tail = 1.0 / 0.2;
    TestTrue(TEXT("tiny tail light: positive estimate at the light"), EngineEstimateLocalLight(0, 1, InvRadius, 0, Tail, false, 0, 0) > 0);
    TestTrue(TEXT("tiny tail light: positive estimate inside its radius"), EngineEstimateLocalLight(FMath::Square(0.04), 1, InvRadius, 0, Tail, false, 0, 0) > 0);
    TestEqual(TEXT("tiny tail light: none at its radius"), EngineEstimateLocalLight(FMath::Square(Radius), 1, InvRadius, 0, Tail, false, 0, 0), 0.0);
    // Conservative: never below what shading gives there (shading's inverse square is 1 / (d^2 + 1)).
    for (const double D : {0.0, 0.01, 0.02, 0.03, 0.04})
        TestTrue(FString::Printf(TEXT("tiny tail light: estimate >= shading at %.2f cm"), D),
                 EngineEstimateLocalLight(D * D, 1, InvRadius, 0, Tail, false, 0, 0) >= EngineRangeFade(D * D, InvRadius, Tail) * EngineDistanceLaw(D * D, 1, false, 0, 0) / UE_DOUBLE_PI);
    // UE's window keeps UE's estimate (clamped distance), unchanged.
    TestEqual(TEXT("UE window: UE's estimate at the light"), EngineEstimateLocalLight(0, 1, 1.0 / 100, 0, 0, false, 0, 0),
              EngineRangeFade(0.01, 1.0 / 100, 0) / 0.01 / UE_DOUBLE_PI, 1e-9);
    // The negative-offset bulb (E1-2 review) stays at the true distance: its peak 1 / (b + c)^2.
    TestEqual(TEXT("bulb offset: the peak at the light"), EngineEstimateLocalLight(0, 1, 1.0 / 100, 0, Tail, true, 0.05, -0.04), 1.0 / FMath::Square(0.01) / UE_DOUBLE_PI, 1e-3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightProbeFollowsRenderer, "SCBridge.Lights.ProbeFollowsRenderer",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightProbeFollowsRenderer::RunTest(const FString&)
{
    // The probe's lux is the renderer's law (Astra E1-1..E1-4 review 11): UE's inverse square regularised as 1 / (d^2 + 1 cm^2), the
    // projector near clip in either spot shape, lumens over the square frustum's solid angle. Lights 50 m apart, 10 m radius each.
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    const auto Make = [World](UClass* Class, const FVector& At) {
        auto* L = NewObject<ULocalLightComponent>(World, Class);
        L->SetMobility(EComponentMobility::Movable);
        L->IntensityUnits = ELightUnits::Candelas;
        L->Intensity = 100.f;
        L->AttenuationRadius = 1000.f;
        L->SetWorldLocation(At);
        L->RegisterComponentWithWorld(World);
        return L;
    };
    auto* Point = Make(UPointLightComponent::StaticClass(), FVector(0, 0, 0));
    auto* Cone = Cast<USpotLightComponent>(Make(USpotLightComponent::StaticClass(), FVector(5000, 0, 0)));
    Cone->InnerConeAngle = 20.f; Cone->OuterConeAngle = 30.f; Cone->ProjectorNearClip = 50.f;
    auto* Square = Cast<USpotLightComponent>(Make(USpotLightComponent::StaticClass(), FVector(10000, 0, 0)));
    Square->Shape = ESpotLightShape::SquareFrustum; Square->OuterConeAngle = 45.f;
    Square->IntensityUnits = ELightUnits::Lumens; Square->Intensity = 1000.f;
    ON_SCOPE_EXIT { Point->UnregisterComponent(); Cone->UnregisterComponent(); Square->UnregisterComponent(); World->DestroyWorld(false); };
    const auto Window = [](double D) { return FMath::Square(1.0 - FMath::Square(FMath::Square(D / 1000.0))); };
    TestEqual(TEXT("point at 1 cm: 100 cd x 1e4 / (1 + 1)"), ProbeLux(World, FVector(1, 0, 0)), 100.0 * 1e4 / 2.0 * Window(1), 0.1);
    TestEqual(TEXT("cone in front of its near clip: no light"), ProbeLux(World, FVector(5020, 0, 0)), 0.0, 1e-6);
    TestEqual(TEXT("cone behind its near clip: lit"), ProbeLux(World, FVector(5100, 0, 0)), 100.0 * 1e4 / (1e4 + 1) * Window(100), 0.1);
    const double Candela = 1000.0 / USpotLightComponent::GetSquareFrustumSolidAngle(UE_PI / 4);
    TestEqual(TEXT("square in lumens: over the square's solid angle"), ProbeLux(World, FVector(10100, 0, 0)), Candela * 1e4 / (1e4 + 1) * Window(100), 0.1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightComparisonRestorePerWorld, "SCBridge.Lights.ComparisonRestorePerWorld",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightComparisonRestorePerWorld::RunTest(const FString&)
{
    // sc.Lights.ProjectorShape / FalloffModels restore one world without erasing another world's saved lights (Astra E1-1..E1-4
    // review 12): two worlds switched to UE's cone / falloff, the first restored, then the second.
    UWorld* Worlds[2] = {UWorld::CreateWorld(EWorldType::Game, false), UWorld::CreateWorld(EWorldType::Game, false)};
    USpotLightComponent* Spots[2];
    UPointLightComponent* Points[2];
    for (int32 I = 0; I < 2; ++I)
    {
        Spots[I] = NewObject<USpotLightComponent>(Worlds[I]);
        Spots[I]->SetMobility(EComponentMobility::Movable);
        Spots[I]->Shape = ESpotLightShape::SquareFrustum; Spots[I]->LightFunctionAddress = ELightFunctionAddress::Border; Spots[I]->OuterConeAngle = 40.f;
        Spots[I]->RegisterComponentWithWorld(Worlds[I]);
        Points[I] = NewObject<UPointLightComponent>(Worlds[I]);
        Points[I]->SetMobility(EComponentMobility::Movable);
        Points[I]->RangeFade = ELightRangeFade::SquaredLinearTail; Points[I]->RangeFadeFraction = 0.2f;
        Points[I]->RegisterComponentWithWorld(Worlds[I]);
    }
    ON_SCOPE_EXIT { for (int32 I = 0; I < 2; ++I) { Spots[I]->UnregisterComponent(); Points[I]->UnregisterComponent(); Worlds[I]->DestroyWorld(false); } };
    const auto Run = [](const TCHAR* Command, UWorld* World) { IConsoleManager::Get().ProcessUserConsoleInput(Command, *GLog, World); };
    for (UWorld* W : Worlds) { Run(TEXT("sc.Lights.ProjectorShape 0"), W); Run(TEXT("sc.Lights.FalloffModels 0"), W); }
    TestTrue(TEXT("both worlds compare on UE's cone"), Spots[0]->Shape == ESpotLightShape::Cone && Spots[1]->Shape == ESpotLightShape::Cone);
    TestTrue(TEXT("both worlds compare on UE's range window"), Points[0]->RangeFade == ELightRangeFade::Default && Points[1]->RangeFade == ELightRangeFade::Default);
    Run(TEXT("sc.Lights.ProjectorShape 1"), Worlds[0]); Run(TEXT("sc.Lights.FalloffModels 1"), Worlds[0]);
    TestTrue(TEXT("first world restored: square"), Spots[0]->Shape == ESpotLightShape::SquareFrustum && Spots[0]->LightFunctionAddress == ELightFunctionAddress::Border);
    TestTrue(TEXT("second world still compares: cone"), Spots[1]->Shape == ESpotLightShape::Cone);
    Run(TEXT("sc.Lights.ProjectorShape 1"), Worlds[1]); Run(TEXT("sc.Lights.FalloffModels 1"), Worlds[1]);
    TestTrue(TEXT("second world restored: square"), Spots[1]->Shape == ESpotLightShape::SquareFrustum && Spots[1]->LightFunctionAddress == ELightFunctionAddress::Border);
    TestTrue(TEXT("first world restored: tail window"), Points[0]->RangeFade == ELightRangeFade::SquaredLinearTail);
    TestTrue(TEXT("second world restored: tail window"), Points[1]->RangeFade == ELightRangeFade::SquaredLinearTail);
    return true;
}
#endif
