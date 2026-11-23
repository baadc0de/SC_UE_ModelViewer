// WP-O1-4b-1b: a vehicle's interaction state is saved with a prepared level. Each structure the import builds (door
// records, door interactions with their tags, ship doors with their sweeps, door controls and pad reach, door sensors,
// port rigs, the doors' fragment states, the physics grid) survives the actor's property serialization — the path a
// level save and load takes — so a prepared vehicle needs none of it rebuilt.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCSavedDoorStateTest, "SCBridge.Prepare.SavedDoorState", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCSavedDoorStateTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    ASCVehicle* Source = World->SpawnActor<ASCVehicle>();
    ASCVehicle* Loaded = World->SpawnActor<ASCVehicle>();
    if (!TestTrue(TEXT("vehicles"), Source && Loaded)) return false;
    USCRigComponent* Rig = NewObject<USCRigComponent>(Source);
    USceneComponent* Pad = NewObject<USceneComponent>(Source);
    USceneComponent* SensorFrame = NewObject<USceneComponent>(Source);

    // What the import builds for one door, its pad and its sensor.
    FSCDoorRecord Record;
    Record.Door.bValid = true;
    Record.Door.bIsOpened = true;
    Record.Door.bIsLocked = false;
    Record.Door.TransitionTag = TEXT("Open");
    Record.PortLinks.Add({TEXT("Open"), TEXT("hardpoint_ramp"), TEXT("Open")});
    Source->DoorRecords.Add(Rig, Record);
    FSCDoorInteraction Door;
    Door.Rig = Rig;
    Door.Fragment = TEXT("Door");
    Door.OpenTag = TEXT("Deploy");
    Door.CloseTag = TEXT("Retract");
    Door.bReverseClose = true;
    Door.bStartsOpen = true;
    Door.Speed = 0.5f;
    Door.PortLinks = Record.PortLinks;
    Source->Doors.Add(Door);
    FSCShipDoor Ship;
    Ship.Group = ESCShipDoorGroup::Exterior;
    Ship.Label = TEXT("Ramp");
    Ship.Aperture = FBox(FVector(-1), FVector(1));
    Ship.bRamp = true;
    Ship.bLocked = true;
    FSCShipDoorCollider& Collider = Ship.MovementColliders.AddDefaulted_GetRef();
    Collider.Socket = TEXT("Ramp_Joint");
    Collider.Sweep.Add({FTransform(FVector(0, 0, 10)), 12.5});
    Ship.GroundProbes.Add({3, {FVector(1, 2, 3)}});
    Source->ShipDoors.Add(Ship);
    Source->DoorControls.Add(Pad, Rig);
    Source->SetPanelReach(Pad, 15.f);
    FSCDoorSensor Sensor;
    Sensor.Anchor = SensorFrame;
    Sensor.Box = FBox(FVector(-50), FVector(50));
    Sensor.ExitDelay = 2.f;
    Sensor.Doors.Add(Rig);
    Source->DoorSensors.Add(Sensor);
    Source->PortRigs.Add(TEXT("hardpoint_ramp"), Rig);
    Source->DoorPanelLabels.Add(Rig, TEXT("Ramp"));
    Source->PhysicsGridTriangles = {FVector(0), FVector(1, 0, 0), FVector(0, 1, 0)};
    Source->PhysicsGridShapeEnds = {3};
    Source->PhysicsGridBounds = FBox(FVector(0), FVector(1));
    Source->ShipDoors[0].bSecurePending = true;
    // The doors' recorded states (ApplyDoorRestPoses records each door's authored state): a second door, without data,
    // whose open and close tags are one, so its recorded state reads open.
    USCRigComponent* Hatch = NewObject<USCRigComponent>(Source);
    FSCDoorInteraction Second;
    Second.Rig = Hatch;
    Second.Fragment = TEXT("Hatch");
    Second.OpenTag = Second.CloseTag = TEXT("Toggle");
    Source->Doors.Add(Second);
    AddExpectedMessage(TEXT("DOOR_REST"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1); // door 0 has no clips
    Source->ApplyDoorRestPoses();
    TestTrue(TEXT("the second door's state reads open"), Source->IsDoorOpen(1));

    // Through the actor's property serialization (a level save and load).
    TArray<uint8> Bytes;
    FObjectWriter(Source, Bytes);
    FObjectReader(Loaded, Bytes);

    const FSCDoorRecord* R = Loaded->DoorRecords.Find(Rig);
    TestTrue(TEXT("the door record"), R && R->Door.bValid && R->Door.bIsOpened.Get(false) && R->Door.bIsLocked.IsSet() &&
                                          R->Door.TransitionTag == TEXT("Open") && R->PortLinks.Num() == 1 && R->PortLinks[0].TargetPort == TEXT("hardpoint_ramp"));
    TestTrue(TEXT("the door interaction's tags, speed and links"),
             Loaded->Doors.Num() == 2 && Loaded->Doors[0].OpenTag == TEXT("Deploy") && Loaded->Doors[0].CloseTag == TEXT("Retract") &&
                 Loaded->Doors[0].bReverseClose && Loaded->Doors[0].bStartsOpen && Loaded->Doors[0].Speed == 0.5f && Loaded->Doors[0].PortLinks.Num() == 1);
    TestTrue(TEXT("the ship door with its sweep and probes"),
             Loaded->ShipDoors.Num() == 1 && Loaded->ShipDoors[0].Group == ESCShipDoorGroup::Exterior && Loaded->ShipDoors[0].Label == TEXT("Ramp") &&
                 Loaded->ShipDoors[0].bRamp && Loaded->ShipDoors[0].bLocked && Loaded->ShipDoors[0].bSecurePending &&
                 Loaded->ShipDoors[0].Aperture.Equals(FBox(FVector(-1), FVector(1))) && Loaded->ShipDoors[0].MovementColliders.Num() == 1 &&
                 Loaded->ShipDoors[0].MovementColliders[0].Socket == TEXT("Ramp_Joint") && Loaded->ShipDoors[0].MovementColliders[0].Sweep.Num() == 1 &&
                 Loaded->ShipDoors[0].MovementColliders[0].Sweep[0].TravelCm == 12.5 && Loaded->ShipDoors[0].GroundProbes.Num() == 1 &&
                 Loaded->ShipDoors[0].GroundProbes[0].Bone == 3);
    TestTrue(TEXT("the pad operates its door from its reach"), Loaded->DoorForPanel(Pad) == Rig && Loaded->PanelReachOf(Pad) == 1500.f && Loaded->MaxPanelReach == 1500.f);
    TestTrue(TEXT("the sensor"), Loaded->DoorSensors.Num() == 1 && Loaded->DoorSensors[0].Anchor.Get() == SensorFrame &&
                                     Loaded->DoorSensors[0].ExitDelay == 2.f && Loaded->DoorSensors[0].Doors.Num() == 1 && Loaded->DoorSensors[0].Doors[0].Get() == Rig);
    TestTrue(TEXT("the port rig and the pad's label"), Loaded->PortRigs.FindRef(TEXT("hardpoint_ramp")).Get() == Rig && Loaded->DoorPanelLabels.FindRef(Rig) == TEXT("Ramp"));
    TestTrue(TEXT("the physics grid"), Loaded->PhysicsGridTriangles.Num() == 3 && Loaded->PhysicsGridShapeEnds == TArray<int32>({3}) &&
                                           Loaded->PhysicsGridBounds.Equals(FBox(FVector(0), FVector(1))));
    TestTrue(TEXT("the doors' fragment states"), Loaded->IsDoorOpen(1) && !Loaded->IsDoorOpen(0));
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
