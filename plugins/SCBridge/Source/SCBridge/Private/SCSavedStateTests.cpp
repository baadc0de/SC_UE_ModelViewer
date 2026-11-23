// WP-O1-4b-1b: a vehicle's interaction state is saved with a prepared level. Each structure the import builds (door
// records, door interactions with their tags, ship doors with their sweeps, door controls and pad reach, door sensors,
// port rigs, the doors' fragment states, the lift-driven doors, the physics grid) survives the actor's property serialization — the path a
// level save and load takes — so a prepared vehicle needs none of it rebuilt.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/Material.h"
#include "Components/PointLightComponent.h"
#include "SCLightImporter.h"

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
    // A lift gate: the lift drives it, never a door order.
    USCRigComponent* Gate = NewObject<USCRigComponent>(Source);
    Source->LiftDrivenRigs.Add(Gate);

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
    TestTrue(TEXT("the lift gate stays the lift's"), Loaded->IsLiftDriven(Gate) && !Loaded->IsLiftDriven(Rig));
    return !HasAnyErrors();
}

// WP-O1-4b-1c: a vehicle's screens and panels are saved with a prepared level: the screen faces with their state, hover and
// custom renders, the panel aims, and the drawn panels (floor select and button panels) with their faces, layouts,
// colours, state machines, renders and render targets. A loaded panel is drawn again (what it showed is not saved).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCSavedPanelsTest, "SCBridge.Prepare.SavedPanels", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCSavedPanelsTest::RunTest(const FString&)
{
    ASCVehicle* V = NewObject<ASCVehicle>(GetTransientPackage());
    V->AddToRoot();
    ASCVehicle* Loaded = nullptr;
    ON_SCOPE_EXIT
    {
        V->RemoveFromRoot();
        if (Loaded) Loaded->RemoveFromRoot();
    };
    USceneComponent* Pad = NewObject<USceneComponent>(V, TEXT("Pad"));
    UStaticMeshComponent* Screen = NewObject<UStaticMeshComponent>(V, TEXT("Screen"));
    for (UActorComponent* Part : TArray<UActorComponent*>{Pad, Screen}) V->AddInstanceComponent(Part);
    UMaterialInterface* Render = UMaterial::GetDefaultMaterial(MD_Surface);
    UTexture2D* Image = UTexture2D::CreateTransient(4, 4);
    Image->AddToRoot();
    UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(V, TEXT("PanelTarget"));
    ON_SCOPE_EXIT { Image->RemoveFromRoot(); };

    FSCScreenFace Face;
    Face.Component = Screen;
    Face.Slot = 1;
    Face.Panel = Pad;
    Face.Default = Render;
    Face.States.Add(TEXT("DoorOpenState.DoorOpened"), Render);
    Face.Hovers.Add(TEXT("Open|"), Render);
    Face.Image = TEXT("UI/pad.png");
    Face.Hovered = TEXT("Open");
    V->ScreenFaces.Add(Face);
    FSCScreenFaceGeometry Geometry;
    Geometry.Positions = {FVector3f(0), FVector3f(10, 0, 0), FVector3f(0, 10, 0)};
    Geometry.UVs = {FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)};
    Geometry.Fronts = {FVector3f(0, 0, 1)};
    Geometry.Nodes = {INDEX_NONE};
    Geometry.Soft = {false};
    TestTrue(TEXT("the face builds"), Geometry.Build(FVector(0, 1, 0)));
    FSCPanelAim Aim;
    Aim.Component = Screen;
    Aim.Face = 0;
    Aim.Geometry = Geometry;
    Aim.Buttons.Add({TEXT("Open"), TEXT("Open"), FBox2D(FVector2D(0, 0), FVector2D(0.5, 0.5)), TEXT("Open")});
    Aim.Fallback = TEXT("Open");
    Aim.Reach = 1500;
    V->PanelAims.Add(Aim);
    FSCFloorPanel Panel;
    Panel.Kind = ESCPanelKind::Buttons;
    Panel.Interior = 0;
    Panel.EntityId = TEXT("light-panel");
    Panel.Anchor = Pad;
    Panel.Component = Screen;
    Panel.Geometry = Geometry;
    Panel.Layout.Options.Add({INDEX_NONE, TEXT("Auxiliary"), TEXT("Auxiliary"), FBox2D(FVector2D(0, 0), FVector2D(1, 0.5)), TEXT("aux")});
    Panel.Colours.Row = FLinearColor::Red;
    Panel.Target = Target;
    Panel.Masks.Add(Image);
    FSCUiStateType Lights;
    Lights.Name = TEXT("LightState");
    Lights.DefaultState = TEXT("LightsOn");
    Lights.Transitions.Add({FString(), TEXT("Auxiliary"), TEXT("LightsAux")});
    Lights.Linked.Add({TEXT("/State/LightGroup/Auxiliary"), TEXT("LightsAux")});
    Panel.StateTypes.Add(Lights);
    FSCUiStateImage Aux;
    Aux.StateType = TEXT("LightState");
    Aux.State = TEXT("LightsAux");
    Aux.States = {TEXT("LightState.LightsAux")};
    Aux.ImagePath = TEXT("UI/aux.png");
    Panel.StateImages.Add(Aux);
    Panel.UiState.Add(TEXT("LightState"), TEXT("LightsAux"));
    Panel.Images.Add(TEXT("UI/aux.png"), Image);
    Panel.Sendables.Add({TEXT("Auxiliary"), TEXT("/State/LightGroup/Auxiliary")});
    Panel.bDrawn = true;
    V->FloorPanels.Add(Panel);
    V->DoorPanelLocks.Add(Panel);
    V->FloorPanelTargets.Add(Target);

    // Saved and loaded (a DuplicateObject copy: components remapped, PostLoad run).
    Loaded = DuplicateObject(V, GetTransientPackage());
    Loaded->AddToRoot();
    const USceneComponent* LoadedPad = FindObject<USceneComponent>(Loaded, TEXT("Pad"));
    const UStaticMeshComponent* LoadedScreen = FindObject<UStaticMeshComponent>(Loaded, TEXT("Screen"));
    if (!TestTrue(TEXT("the copy's components"), LoadedPad && LoadedScreen)) return false;
    TestTrue(TEXT("the screen face with its renders, on the loaded components"),
             Loaded->ScreenFaces.Num() == 1 && Loaded->ScreenFaces[0].Component.Get() == LoadedScreen && Loaded->ScreenFaces[0].Panel.Get() == LoadedPad &&
                 Loaded->ScreenFaces[0].Slot == 1 && Loaded->ScreenFaces[0].Default == Render && Loaded->ScreenFaces[0].States.Num() == 1 &&
                 Loaded->ScreenFaces[0].Hovers.Num() == 1 && Loaded->ScreenFaces[0].Image == TEXT("UI/pad.png") && Loaded->ScreenFaces[0].Hovered.IsEmpty());
    TestTrue(TEXT("the panel aim"), Loaded->PanelAims.Num() == 1 && Loaded->PanelAims[0].Component.Get() == LoadedScreen &&
                                        Loaded->PanelAims[0].Buttons.Num() == 1 && Loaded->PanelAims[0].Buttons[0].Interaction == TEXT("Open") &&
                                        Loaded->PanelAims[0].Reach == 1500 && Loaded->PanelAims[0].Geometry.Width == Geometry.Width &&
                                        Loaded->PanelAims[0].Geometry.Positions.Num() == 3);
    if (!TestTrue(TEXT("the drawn panels"), Loaded->FloorPanels.Num() == 1 && Loaded->DoorPanelLocks.Num() == 1)) return false;
    const FSCFloorPanel& P = Loaded->FloorPanels[0];
    TestTrue(TEXT("its face, layout, colours and target"),
             P.Kind == ESCPanelKind::Buttons && P.EntityId == TEXT("light-panel") && P.Anchor.Get() == LoadedPad && P.Component.Get() == LoadedScreen &&
                 P.Geometry.Height == Geometry.Height && P.Layout.Options.Num() == 1 && P.Layout.Options[0].Node == TEXT("aux") &&
                 P.Colours.Row == FLinearColor::Red && P.Target && P.Target->GetOuter() == Loaded && P.Masks.Num() == 1 && P.Masks[0] == Image);
    TestTrue(TEXT("its state machine and renders"),
             P.StateTypes.Num() == 1 && P.StateTypes[0].Transitions.Num() == 1 && P.StateTypes[0].Transitions[0].State == TEXT("LightsAux") &&
                 P.StateTypes[0].Linked.Num() == 1 && P.StateTypes[0].Linked[0].Value == TEXT("LightsAux") && P.StateImages.Num() == 1 &&
                 P.StateImages[0].Key() == TEXT("LightState.LightsAux") && P.UiState.FindRef(TEXT("LightState")) == TEXT("LightsAux") &&
                 P.Images.FindRef(TEXT("UI/aux.png")) == Image && P.Sendables.Num() == 1 && P.ImageForState() == TEXT("UI/aux.png"));
    TestFalse(TEXT("drawn again at load (what it showed is not saved)"), P.bDrawn);
    return !HasAnyErrors();
}

// WP-O1-4b-1b-3: a vehicle's ladders, usables, seat records, light groups, its lights' authored states and its room
// channels are saved with a prepared level; a loaded light takes another of its states from what was saved (no JSON).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCSavedInteractionsTest, "SCBridge.Prepare.SavedInteractions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCSavedInteractionsTest::RunTest(const FString&)
{
    ASCVehicle* V = NewObject<ASCVehicle>(GetTransientPackage());
    V->AddToRoot();
    ASCVehicle* Loaded = nullptr;
    ON_SCOPE_EXIT
    {
        V->RemoveFromRoot();
        if (Loaded) Loaded->RemoveFromRoot();
    };
    USceneComponent* LadderFrame = NewObject<USceneComponent>(V, TEXT("Ladder"));
    USceneComponent* Bed = NewObject<USceneComponent>(V, TEXT("Bed"));
    UPointLightComponent* Lamp = NewObject<UPointLightComponent>(V, TEXT("Lamp"));
    for (UActorComponent* Part : TArray<UActorComponent*>{LadderFrame, Bed, Lamp}) V->AddInstanceComponent(Part);

    FSCLadderAnchor Ladder;
    Ladder.Anchor = LadderFrame;
    Ladder.EntityId = TEXT("ladder-1");
    Ladder.Data.Height = 3.5f;
    Ladder.Data.Top.RemountDelay = 0.75f;
    Ladder.Data.TopExits = FIntVector(1, 0, 1);
    Ladder.Data.Actions.Add(TEXT("ClimbUp"), {TEXT("ladder_climb_up"), 1.2f, 0.2f, 0.f});
    V->LadderAnchors.Add(Ladder);
    FSCUsableTarget Usable;
    Usable.Interior = 0;
    Usable.EntityId = TEXT("bed-1");
    Usable.Label = TEXT("Bed");
    FSCUsablePoint& Point = Usable.Points.AddDefaulted_GetRef();
    Point.Helper = TEXT("use_lie");
    Point.bLocated = true;
    Point.RadiusCm = 120;
    Point.Options.Add({TEXT("LieDown"), TEXT("Lie down"), true});
    V->UsableTargets.Add(Bed, Usable);
    V->LightGroups.Add({0, TEXT("group-1"), TEXT("Cabin lights"), {TEXT("panel-1")}, {{TEXT("Auxiliary"), TEXT("/State/LightGroup/Auxiliary"), TEXT("auxiliaryState")}}, FString()});
    V->LightRoomChannels.Add(TEXT("room-a"), 2);
    FSCImportedLightState Light;
    Light.Component = Lamp;
    Light.Name = TEXT("lamp");
    Light.AuthoredStates.Add(TEXT("defaultState"), {1000.0, FLinearColor::White, 0});
    Light.AuthoredStates.Add(TEXT("auxiliaryState"), {250.0, FLinearColor::Red, 3});
    Light.WallLightScale = 0.5f;
    V->LightStates.Add(Light);
    FSCSceneInstance Seat;
    Seat.EntityName = TEXT("pilot_seat");
    Seat.PortPath = {TEXT("hardpoint_seat_pilot")};
    FSCSeatDefinition Definition;
    Definition.Role = ESCSeatRole::Driver;
    Definition.Attachment = TEXT("Seat_Attach");
    Definition.MinYaw = -60.0;
    Seat.Seat = Definition;
    FSCSeatEntrance Entrance;
    Entrance.Name = TEXT("pilot");
    Entrance.SeatPortPath = {TEXT("hardpoint_seat_pilot")};
    Entrance.Enter.Interaction = TEXT("Enter");
    Entrance.Gateways.AddDefaulted_GetRef().Conditions.Add(TEXT("Hatch"), TEXT("Open"));
    Seat.SeatEntrances.Add(Entrance);
    FSCInteractionPoint& Press = Seat.InteractionPoints.AddDefaulted_GetRef();
    Press.HelperName = TEXT("use_enter");
    Press.Radius = 1.5f;
    Press.bHasHelperTransform = true;
    Press.ViewAngleLimit = 90.f;
    Press.Interactions.Add({TEXT("Enter"), TEXT("Sit"), true});
    V->Manifest.Children.Add(Seat);

    // Saved and loaded (a DuplicateObject copy: components remapped, PostLoad run).
    Loaded = DuplicateObject(V, GetTransientPackage());
    Loaded->AddToRoot();
    const USceneComponent* LoadedLadder = FindObject<USceneComponent>(Loaded, TEXT("Ladder"));
    USceneComponent* LoadedBed = FindObject<USceneComponent>(Loaded, TEXT("Bed"));
    TestTrue(TEXT("the ladder on the loaded anchor, with its data"),
             Loaded->LadderAnchors.Num() == 1 && LoadedLadder && Loaded->LadderAnchors[0].Anchor.Get() == LoadedLadder &&
                 Loaded->LadderAnchors[0].Data.Height == 3.5f && Loaded->LadderAnchors[0].Data.Top.RemountDelay == 0.75f &&
                 Loaded->LadderAnchors[0].Data.TopExits == FIntVector(1, 0, 1) && Loaded->LadderAnchors[0].Data.Actions.Contains(TEXT("ClimbUp")));
    const FSCUsableTarget* LoadedUsable = LoadedBed ? Loaded->UsableTargets.Find(LoadedBed) : nullptr;
    TestTrue(TEXT("the usable on the loaded anchor"), LoadedUsable && LoadedUsable->Points.Num() == 1 && LoadedUsable->Points[0].RadiusCm == 120 &&
                                                         LoadedUsable->Points[0].Options.Num() == 1 && LoadedUsable->Points[0].Options[0].bPrimary);
    TestTrue(TEXT("the light group"), Loaded->LightGroups.Num() == 1 && Loaded->LightGroups[0].Interactions.Num() == 1 &&
                                          Loaded->LightGroups[0].Interactions[0].LightState == TEXT("auxiliaryState") &&
                                          Loaded->LightGroupsLinkedBy(0, {TEXT("group-1")}).Num() == 1);
    TestEqual(TEXT("the room channels"), Loaded->LightRoomChannels.FindRef(TEXT("room-a")), 2);
    const FSCSceneInstance* LoadedSeat = Loaded->Manifest.Children.FindByPredicate([](const FSCSceneInstance& I) { return I.EntityName == TEXT("pilot_seat"); });
    TestTrue(TEXT("the seat records"), LoadedSeat && LoadedSeat->Seat.IsSet() && LoadedSeat->Seat->Role == ESCSeatRole::Driver &&
                                           LoadedSeat->Seat->MinYaw.Get(0) == -60.0 && LoadedSeat->SeatEntrances.Num() == 1 &&
                                           LoadedSeat->SeatEntrances[0].Enter.Interaction == TEXT("Enter") &&
                                           LoadedSeat->SeatEntrances[0].Gateways.Num() == 1 && LoadedSeat->InteractionPoints.Num() == 1 &&
                                           LoadedSeat->InteractionPoints[0].bHasHelperTransform && LoadedSeat->InteractionPoints[0].ViewAngleLimit.Get(0) == 90.f &&
                                           LoadedSeat->InteractionPoints[0].Interactions.Num() == 1);
    // Another authored state on the loaded light, from what was saved.
    if (!TestEqual(TEXT("the light"), Loaded->LightStates.Num(), 1)) return false;
    TestTrue(TEXT("its state values and wall scale"), Loaded->LightStates[0].AuthoredStates.Num() == 2 && Loaded->LightStates[0].WallLightScale == 0.5f);
    TestTrue(TEXT("a loaded light takes another authored state"), FSCLightImporter::ApplyState(Loaded, 0, TEXT("auxiliaryState")));
    TestTrue(TEXT("its candela, colour and style from the saved values"),
             Loaded->LightStates[0].BaseCandela == 250.0 && Loaded->LightStates[0].BaseColor == FLinearColor::Red && Loaded->LightStates[0].LightStyleId == 3 &&
                 Loaded->LightStates[0].ActiveState == TEXT("auxiliaryState"));
    TestFalse(TEXT("a state it does not author leaves it"), FSCLightImporter::ApplyState(Loaded, 0, TEXT("emergencyState")));
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
