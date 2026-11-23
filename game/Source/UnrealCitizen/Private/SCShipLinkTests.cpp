#if WITH_DEV_AUTOMATION_TESTS
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoGridComponent.h"
#include "Cargo/SCCargoSubsystem.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "SCMeshImporter.h"
#include "SCRigComponent.h"
#include "SCShipLinkSubsystem.h"
#include "SCVehicle.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "StaticMeshAttributes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoEnvironmentAccessTest, "SCBridge.Cargo.EnvironmentDoorAccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCCargoEnvironmentAccessTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    USCCargoSubsystem* Cargo = World->GetSubsystem<USCCargoSubsystem>();
    if (!TestNotNull(TEXT("cargo subsystem"), Cargo)) return false;
    ASCVehicle* Actor = World->SpawnActor<ASCVehicle>();
    Actor->ShipDoors.AddDefaulted_GetRef().bLocked = true;
    auto* Grid = NewObject<USCCargoGridComponent>(Actor);
    Grid->OwnerVehicle = Actor;
    Grid->AccessDoors.Add(0);
    Grid->Kind = ESCCargoGridKind::Environment;
    FString Refusal;
    TestTrue(TEXT("hangar grid is accessible without a ship owner role"), Cargo->RequestAccess(Grid, true, 0, NAME_None, &Refusal));
    TestTrue(TEXT("no ship hold lock refusal on the hangar grid"), Refusal.IsEmpty());
    Grid->Kind = ESCCargoGridKind::Vehicle;
    TestFalse(TEXT("guest cannot access a locked ship hold"), Cargo->RequestAccess(Grid, true, 0, FName(TEXT("Guest")), &Refusal));
    TestEqual(TEXT("ship lock refuses with the hold message"), Refusal, FString(TEXT("Hold locked by owner")));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCShipLinkDoorObstructionTest, "SCBridge.ShipLink.DoorObstruction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCShipLinkDoorObstructionTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    ASCVehicle* Ship = World->SpawnActor<ASCVehicle>();
    USCRigComponent* Rig = NewObject<USCRigComponent>(Ship);
    Rig->SetupAttachment(Ship->GetRootComponent());
    Rig->RegisterComponent();
    auto Part = [&](const FVector& Location, const FVector& Extent) {
        UBoxComponent* Shape = NewObject<UBoxComponent>(Ship);
        Shape->SetMobility(EComponentMobility::Movable);
        Shape->SetupAttachment(Rig);
        Shape->SetBoxExtent(Extent);
        Shape->SetRelativeLocation(Location);
        Shape->SetCollisionProfileName(TEXT("BlockAll"));
        Shape->RegisterComponent();
        return Shape;
    };
    UBoxComponent* Collider = Part(FVector::ZeroVector, FVector(25));
    Collider->ComponentTags.Add(TEXT("SCGameCollision"));
    UBoxComponent* TraceOnly = Part(FVector(60, 0, 0), FVector(110, 30, 30));
    TraceOnly->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
    Ship->Doors.AddDefaulted_GetRef().Rig = Rig;
    Ship->BuildShipDoors();
    if (!TestEqual(TEXT("only the authored movement collider is used"), Ship->ShipDoors[0].MovementColliders.Num(), 1))
        return false;
    auto& Sweep = Ship->ShipDoors[0].MovementColliders[0].Sweep;
    for (int32 Step = 0; Step <= 10; ++Step)
        Sweep.Add({FTransform(FVector(-10 * Step, 0, 0)), 10});
    // The rigid movement collider moves with the clip. The closed test must still use its original pose.
    Collider->SetRelativeLocation(FVector(-100, 0, 0), false, nullptr, ETeleportType::TeleportPhysics);
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ASCCargoContainer* Box = World->SpawnActor<ASCCargoContainer>(FVector(95, 0, 0), FRotator::ZeroRotator, Spawn);
    Box->SetOccupancy(FVector(20), 0);
    TestFalse(TEXT("a box adjacent to the movement collision may rest in the trace-only bounds"), Ship->DoorwayOccupied(0));
    Box->SetActorLocation(FVector::ZeroVector, false, nullptr, ETeleportType::TeleportPhysics);
    TestTrue(TEXT("a box in the closed collider prevents closing"), Ship->DoorwayOccupied(0));
    Box->SetActorLocation(FVector(-100, 0, 0), false, nullptr, ETeleportType::TeleportPhysics);
    TestTrue(TEXT("a box touching the current movement collider prevents closing"), Ship->DoorwayOccupied(0));
    Box->SetActorLocation(FVector(-50, 0, 0), false, nullptr, ETeleportType::TeleportPhysics);
    TestTrue(TEXT("a box between the translation endpoints prevents closing"), Ship->DoorwayOccupied(0));

    Collider->SetRelativeLocation(FVector(-25, 0, 0), false, nullptr, ETeleportType::TeleportPhysics);
    auto& GroundDoor = Ship->ShipDoors[0];
    GroundDoor.MovementBounds = FBox(FVector(-125,-25,-25), FVector(25));
    GroundDoor.GroundLastSample = 2;
    GroundDoor.MovementColliders[0].GroundPose = Collider->GetRelativeTransform();
    Box->SetActorLocation(FVector(-95, 0, 0), false, nullptr, ETeleportType::TeleportPhysics);
    TestFalse(TEXT("grounded gangway never reached cargo beyond its shortened stroke"), Ship->DoorwayOccupied(0));
    Box->SetActorLocation(FVector(-13, 0, 0), false, nullptr, ETeleportType::TeleportPhysics);
    TestTrue(TEXT("grounded gangway still guards cargo in its closing stroke"), Ship->DoorwayOccupied(0));
    Collider->SetRelativeLocation(FVector(-55, 0, 0), false, nullptr, ETeleportType::TeleportPhysics);
    Box->SetActorLocation(FVector(95, 0, 0), false, nullptr, ETeleportType::TeleportPhysics);
    TestTrue(TEXT("unrelated later motion invalidates the stored ground pose"), Ship->DoorwayOccupied(0));
    GroundDoor.GroundLastSample = INDEX_NONE;

    Sweep.Reset();
    for (int32 Step = 0; Step <= 18; ++Step)
    {
        const double Radians = FMath::DegreesToRadians(5.0 * Step);
        const FQuat Rotation(FVector::UpVector, Radians);
        Sweep.Add({FTransform(Rotation, Rotation.RotateVector(FVector(100, 0, 0))), 13});
    }
    Collider->SetRelativeTransform(Sweep.Last().Pose, false, nullptr, ETeleportType::TeleportPhysics);
    Box->SetActorLocation(FVector(70, 70, 0), false, nullptr, ETeleportType::TeleportPhysics);
    TestTrue(TEXT("a box in the rotating path prevents closing"), Ship->DoorwayOccupied(0));
    Sweep.Reset();
    Box->SetActorLocation(FVector(95, 0, 0), false, nullptr, ETeleportType::TeleportPhysics);
    TestTrue(TEXT("an unknown movement path retains the conservative guard"), Ship->DoorwayOccupied(0));

    // A data-mesh proxy uses complex-as-simple Chaos collision, rather than the simple query boxes above.
    FSCDerivedMesh Data;
    Data.MaterialNames.Add(TEXT("Synthetic"));
    Data.bHasGeometry = true;
    Data.bEnableNanite = false;
    FStaticMeshAttributes Attributes(Data.Description);
    Attributes.Register();
    Attributes.GetVertexInstanceUVs().SetNumChannels(1);
    const auto Group = Data.Description.CreatePolygonGroup();
    TArray<FVertexInstanceID> Corners;
    for (const FVector3f Position : {FVector3f(0, -100, -100), FVector3f(0, 100, -100), FVector3f(0, 0, 100)})
    {
        const auto Vertex = Data.Description.CreateVertex();
        Attributes.GetVertexPositions()[Vertex] = Position;
        const auto Corner = Data.Description.CreateVertexInstance(Vertex);
        Attributes.GetVertexInstanceNormals()[Corner] = FVector3f(1, 0, 0);
        Attributes.GetVertexInstanceTangents()[Corner] = FVector3f(0, 1, 0);
        Attributes.GetVertexInstanceBinormalSigns()[Corner] = 1;
        Attributes.GetVertexInstanceColors()[Corner] = FVector4f(1, 1, 1, 1);
        Corners.Add(Corner);
    }
    Data.Description.CreateTriangle(Group, Corners);
    FSCMeshBuildStats Stats;
    FString Error;
    if (!TestTrue(*Error, FSCMeshImporter::DeriveCollision(Data, Error))) return false;
    UStaticMesh* Mesh = FSCMeshImporter::Instantiate(Data, Ship, NAME_None, true, nullptr, Stats, Error);
    if (!TestNotNull(*Error, Mesh) || !TestTrue(TEXT("created complex-as-simple collision"), Stats.bCollision)) return false;
    Collider->DestroyComponent();
    UStaticMeshComponent* Triangle = NewObject<UStaticMeshComponent>(Ship);
    Triangle->SetMobility(EComponentMobility::Movable);
    Triangle->SetStaticMesh(Mesh);
    Triangle->SetupAttachment(Rig);
    Triangle->SetCollisionProfileName(TEXT("BlockAll"));
    Triangle->RegisterComponent();
    Ship->BuildShipDoors();
    if (!TestEqual(TEXT("cooked triangle is the only movement collider"), Ship->ShipDoors[0].MovementColliders.Num(), 1))
        return false;
    auto& TriangleSweep = Ship->ShipDoors[0].MovementColliders[0].Sweep;
    for (int32 Step = 0; Step <= 10; ++Step)
        TriangleSweep.Add({FTransform(FVector(-10 * Step, 0, 0)), 10});
    Triangle->SetRelativeLocation(FVector(-100, 0, 0), false, nullptr, ETeleportType::TeleportPhysics);
    TestFalse(TEXT("the cooked triangle also ignores adjacent cargo"), Ship->DoorwayOccupied(0));
    Box->SetActorLocation(FVector(-50, 0, 0), false, nullptr, ETeleportType::TeleportPhysics);
    TestTrue(TEXT("the swept cooked triangle detects a crate between its endpoints"), Ship->DoorwayOccupied(0));
    return !HasAnyErrors();
}
#endif
