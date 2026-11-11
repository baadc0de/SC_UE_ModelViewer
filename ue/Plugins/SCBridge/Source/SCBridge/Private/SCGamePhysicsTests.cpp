// WP-P3 headless tests: collider classes, inline grid shapes, the physics-grid volume test. Synthetic fixtures only.
#include "Misc/AutomationTest.h"
#include "SCMeshImporter.h"
#include "SCPackageReader.h"
#include "SCPackageSource.h"
#include "SCVehicle.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
TSharedPtr<FJsonObject> Parse(const FString& Text)
{
    TSharedPtr<FJsonObject> O;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O);
    return O;
}
class FSceneSource : public FSCPackageSource
{
public:
    TArray<uint8> Scene;
    explicit FSceneSource(const FString& Text) { FTCHARToUTF8 U(*Text); Scene.Append(reinterpret_cast<const uint8*>(U.Get()), U.Length()); }
    bool Exists(const FString& Path) const override { return Path == TEXT("scene.json"); }
    bool ReadBytes(const FString& Path, TArray<uint8>& Out, FString& Error) const override
    { if (Path != TEXT("scene.json")) { Error = TEXT("Missing fixture"); return false; } Out = Scene; return true; }
    TArray<FString> ListFiles(const FString&) const override { return {TEXT("scene.json")}; }
};
// A synthetic 2 m cube from (0,0,0) to (2,2,2) in the SC frame, outward-wound.
const TCHAR* CubeShape = TEXT(R"({"kind":"trimesh","part":1,"vertices":[[0,0,0],[2,0,0],[2,2,0],[0,2,0],[0,0,2],[2,0,2],[2,2,2],[0,2,2]],)"
    R"("triangles":[[0,2,1],[0,3,2],[4,5,6],[4,6,7],[0,1,5],[0,5,4],[1,2,6],[1,6,5],[2,3,7],[2,7,6],[3,0,4],[3,4,7]]})");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCGamePhysicsClasses, "SCBridge.GamePhysics.Classes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCGamePhysicsClasses::RunTest(const FString&)
{
    TestTrue(TEXT("Part 0 without a class blocks"), FSCMeshImporter::PhysicsBlocksActor(Parse(TEXT(R"({"kind":"box","part":0})"))));
    TestTrue(TEXT("Player-only blocks"), FSCMeshImporter::PhysicsBlocksActor(Parse(TEXT(R"({"kind":"box","part":0,"class":"player_only"})"))));
    TestTrue(TEXT("No-terrain blocks"), FSCMeshImporter::PhysicsBlocksActor(Parse(TEXT(R"({"kind":"box","part":0,"class":"no_terrain"})"))));
    for (const TCHAR* Class : {TEXT("interaction"), TEXT("tires_only"), TEXT("grid_volume")})
        TestFalse(FString::Printf(TEXT("%s does not block"), Class),
                  FSCMeshImporter::PhysicsBlocksActor(Parse(FString::Printf(TEXT(R"({"kind":"box","part":1,"class":"%s"})"), Class))));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCGamePhysicsGrid, "SCBridge.GamePhysics.Grid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCGamePhysicsGrid::RunTest(const FString&)
{
    TArray<FVector> Tris;
    FString Error;
    TestTrue(TEXT("Inline trimesh tessellates"), FSCMeshImporter::PhysicsShapeTriangles(Parse(CubeShape), Tris, [](int32, TArray<FVector>&) { return false; }, Error));
    TestEqual(TEXT("Twelve triangles"), Tris.Num(), 36);
    TestFalse(TEXT("Out-of-range index rejected"), FSCMeshImporter::PhysicsShapeTriangles(
        Parse(TEXT(R"({"kind":"trimesh","vertices":[[0,0,0]],"triangles":[[0,0,1]]})")), Tris, [](int32, TArray<FVector>&) { return false; }, Error));

    const FSceneSource Source(FString(TEXT(R"({"version":2,"export_kind":"Decomposed","root_entity":{"entity_name":"Synthetic","physics_grid":{"source_path":"g.cgf","shapes":[)"))
                              + CubeShape + TEXT("]}}}"));
    FSCSceneManifest Scene;
    TestTrue(*Error, FSCPackageReader::Read(Source, Scene, Error));
    TestEqual(TEXT("Grid triangles"), Scene.PhysicsGrid.Num(), 36);
    // SC (x, y, z) m -> UE (y, x, z) cm.
    FBox Bounds(ForceInit);
    for (const FVector& P : Scene.PhysicsGrid) Bounds += P;
    TestTrue(TEXT("Grid in UE cm"), Bounds.Min.Equals(FVector::ZeroVector) && Bounds.Max.Equals(FVector(200)));

    ASCVehicle* Vehicle = NewObject<ASCVehicle>(GetTransientPackage());
    Vehicle->PhysicsGridTriangles = Scene.PhysicsGrid;
    for (const FVector& P : Scene.PhysicsGrid) Vehicle->PhysicsGridBounds += P;
    TestTrue(TEXT("Centre inside"), Vehicle->IsInPhysicsGrid(FVector(100, 100, 100)));
    TestFalse(TEXT("Beside outside"), Vehicle->IsInPhysicsGrid(FVector(300, 100, 100)));
    TestTrue(TEXT("Axis whose head is inside touches"), Vehicle->IsInPhysicsGrid(FVector(100, 100, -50), FVector(100, 100, 50)));
    TestFalse(TEXT("Axis above the volume does not"), Vehicle->IsInPhysicsGrid(FVector(100, 100, 250), FVector(100, 100, 400)));
    Vehicle->PhysicsGridTriangles.Reset();
    TestFalse(TEXT("No volume, never inside"), Vehicle->IsInPhysicsGrid(FVector(100, 100, 100)));
    return true;
}
#endif
