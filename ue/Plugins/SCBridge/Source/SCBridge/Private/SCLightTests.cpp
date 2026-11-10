// Regression fixtures for instance-local shadow budgets, authored default light states and the game's light model.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCLightImporter.h"
#include "SCVehicle.h"
#include "Engine/World.h"
#include "Components/LightComponent.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "SCLightAreas.h"
#include "SCStarEngine.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightDefaults, "SCBridge.Lights.DefaultsAndShadowBudget",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightDefaults::RunTest(const FString&)
{
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
    TestTrue(TEXT("fallback shadows follow shadow_casting (on)"), bool(Vehicle->Lights[0]->CastShadows));
    TestFalse(TEXT("fallback shadows follow shadow_casting (off)"), bool(Vehicle->Lights[1]->CastShadows));
    TestEqual(TEXT("no warnings"), Warnings.Num(), 0);
    Vehicle->Destroy();
    World->DestroyWorld(false);
    MegaLights->Set(Previous, ECVF_SetByCode);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLightVerifier, "SCBridge.Lights.VerifierFollowsImporter",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLightVerifier::RunTest(const FString&)
{
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
    // Synthetic scene (not game data). Channel 0 = outside every room volume, channel 1 = inside one.
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
    const FString Volume = TEXT(R"({"room":"a","source":"s","kind":"box","transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],)"
                                R"("box_min":[-1,2,-1],"box_max":[1,4,1]})");
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
    FSCLightAreas::Register(Vehicle, Vehicle->GetRootComponent(), {Volume, TEXT(R"({"kind":"sphere"})")}, false, Warnings);
    TestEqual(TEXT("a malformed volume warns and is dropped"), Warnings.Num(), 1);
    TestEqual(TEXT("one area with one volume"), Vehicle->LightAreas.Num() == 1 ? Vehicle->LightAreas[0].Volumes.Num() : -1, 1);
    // Samples are mesh-local cm; a component is inside when more than half of them are.
    Vehicle->MeshSamples.Add(Cube, {FVector3f(0, 0, 0), FVector3f(50, 0, 0), FVector3f(-2000, 0, 0)});   // 2 of 3 inside at (300,0,0)
    Vehicle->MeshSamples.Add(Sphere, {FVector3f(0, 0, 0), FVector3f(2000, 0, 0), FVector3f(-2000, 0, 0), FVector3f(0, 2000, 0)}); // 1 of 4
    auto* Mostly = AddMesh(Vehicle, Cube, FVector(300, 0, 0));
    auto* Hull = AddMesh(Vehicle, Sphere, FVector(300, 0, 0));
    auto* Split = AddMesh(Vehicle, Cube, FVector(380, 0, 0));                                           // 1 of 3 inside from here
    auto* UnsampledIn = AddMesh(Vehicle, Cone, FVector(300, 0, 0));                                      // no samples: bounds origin
    auto* UnsampledOut = AddMesh(Vehicle, Cone, FVector(0, 300, 0));
    FSCLightAreas::Apply(Vehicle);
    TestEqual(TEXT("four lights"), Vehicle->Lights.Num(), 4);
    TestEqual(TEXT("area-only light inside a volume: channel 1 only"), Channels(Vehicle->Lights[0]->LightingChannels), 2);
    TestEqual(TEXT("area-only light outside: channel 0 only"), Channels(Vehicle->Lights[1]->LightingChannels), 1);
    TestEqual(TEXT("affects everything: both"), Channels(Vehicle->Lights[2]->LightingChannels), 3);
    TestEqual(TEXT("flag absent: both"), Channels(Vehicle->Lights[3]->LightingChannels), 3);
    TestTrue(TEXT("state records the flag"), Vehicle->LightStates[0].bAreaOnly && Vehicle->LightStates[1].bAreaOnly &&
                                                  !Vehicle->LightStates[2].bAreaOnly && !Vehicle->LightStates[3].bAreaOnly);
    TestTrue(TEXT("state records the side"), Vehicle->LightStates[0].bInsideArea && !Vehicle->LightStates[1].bInsideArea &&
                                                  Vehicle->LightStates[2].bInsideArea && !Vehicle->LightStates[3].bInsideArea);
    TestEqual(TEXT("majority of samples inside: channel 1"), Channels(Mostly->LightingChannels), 2);
    TestEqual(TEXT("minority of samples inside: channel 0"), Channels(Hull->LightingChannels), 1);
    TestEqual(TEXT("same mesh, different placement: channel 0"), Channels(Split->LightingChannels), 1);
    TestEqual(TEXT("unsampled mesh inside by its bounds origin"), Channels(UnsampledIn->LightingChannels), 2);
    TestEqual(TEXT("unsampled mesh outside by its bounds origin"), Channels(UnsampledOut->LightingChannels), 1);
    TestEqual(TEXT("environment light reaches both sides"), Channels(HangarLight->LightingChannels), 3);
    TestTrue(TEXT("world query inside"), Vehicle->IsInsideLightArea(FVector(5300, 0, 0)));
    TestFalse(TEXT("world query outside"), Vehicle->IsInsideLightArea(FVector(300, 0, 0)));
    Plain->Destroy();
    Vehicle->Destroy();
    Hangar->Destroy();
    World->DestroyWorld(false);
    return true;
}
#endif
