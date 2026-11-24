// WP-O1-4b-1b: a vehicle's interaction state is saved with a prepared level. Each structure the import builds (door
// records, door interactions with their tags, ship doors with their sweeps, door controls and pad reach, door sensors,
// port rigs, the doors' fragment states, the lift-driven doors, the physics grid) survives the actor's property serialization — the path a
// level save and load takes — so a prepared vehicle needs none of it rebuilt. WP-O1-4b-2: a prepared player body's kit and clips.
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
#include "SCLights.h"
#include "SCPlayerKitData.h"
#include "SCTraversalClips.h"
#include <limits>

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
    TestTrue(TEXT("a loaded light takes another authored state"), FSCLights::ApplyState(Loaded, 0, TEXT("auxiliaryState")));
    TestTrue(TEXT("its candela, colour and style from the saved values"),
             Loaded->LightStates[0].BaseCandela == 250.0 && Loaded->LightStates[0].BaseColor == FLinearColor::Red && Loaded->LightStates[0].LightStyleId == 3 &&
                 Loaded->LightStates[0].ActiveState == TEXT("auxiliaryState"));
    TestFalse(TEXT("a state it does not author leaves it"), FSCLights::ApplyState(Loaded, 0, TEXT("emergencyState")));
    return !HasAnyErrors();
}

// WP-O1-4b-2: a prepared player body's parsed kit and body clips are saved with its actor, every field as parsed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCSavedPlayerKitTest, "SCBridge.Prepare.SavedPlayerKit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCSavedPlayerKitTest::RunTest(const FString&)
{
    ASCVehicle* V = NewObject<ASCVehicle>(GetTransientPackage());
    V->AddToRoot();
    ASCVehicle* Loaded = nullptr;
    ON_SCOPE_EXIT
    {
        V->RemoveFromRoot();
        if (Loaded) Loaded->RemoveFromRoot();
    };
    // What SCLoadPlayerKit parses, one value in every field kind.
    const TSharedRef<FSCPlayerKit> Kit = MakeShared<FSCPlayerKit>();
    Kit->Gender = TEXT("female");
    Kit->Loadout = TEXT("Data/Scripts/L.xml");
    Kit->CameraBone = TEXT("HeadCam");
    Kit->RootMotionBone = TEXT("Locomotion");
    FSCItemPort& Hand = Kit->HandPorts.AddDefaulted_GetRef();
    Hand.Name = TEXT("weapon_attach_hand_right");
    Hand.AttachBone = TEXT("RightHand");
    Hand.Flags = {TEXT("invisible_fp")};
    Hand.Types.Emplace(TEXT("WeaponPersonal"), TArray<FString>{TEXT("Small"), TEXT("Medium")});
    Hand.Offset = FMatrix(FPlane(1, 0, 0, 0), FPlane(0, 0, 1, 0), FPlane(0, -1, 0, 0), FPlane(0.1, 0.2, 0.3, 1));
    FSCPlayerWeapon& Weapon = Kit->Weapons.AddDefaulted_GetRef();
    Weapon.Entity = TEXT("behr_rifle_ballistic_01");
    Weapon.Size = 3;
    Weapon.StowPort.Name = TEXT("wep_stocked_2");
    Weapon.OffHandGripHelper = TEXT("grip_off");
    FSCFireAction& Action = Weapon.FireActions.AddDefaulted_GetRef();
    Action.Kind = TEXT("rapid");
    Action.FireRate = 600.f;
    Action.StopFragment = TEXT("stop");
    Action.EffectHelpers = {TEXT("eject")};
    FSCWeaponFragment& Fire = Weapon.FireFragments.AddDefaulted_GetRef();
    Fire.Fragment = TEXT("fire");
    FSCWeaponFragment::FLayer& FireLayer = Fire.Layers.AddDefaulted_GetRef();
    FireLayer.Clip = TEXT("rifle_fire");
    FireLayer.bLoop = true;
    FireLayer.Speed = 1.5f;
    Weapon.ClipSidecars.Add(TEXT("rifle_fire"), TEXT("Packages/rifle/fire.json"));
    Weapon.FireClipJson.Add(TEXT("rifle_fire"), TEXT("{\"frames\": 3}"));
    FSCMotionState& State = Kit->States.AddDefaulted_GetRef();
    State.Stance = TEXT("stand");
    State.bFirstPerson = true;
    FSCMotionFragment& Idle = State.Fragments.AddDefaulted_GetRef();
    Idle.Fragment = TEXT("MotionIdle");
    Idle.QueryFragTags = {TEXT("ADS")};
    FSCMotionLayer& Layer = Idle.Layers.AddDefaulted_GetRef();
    Layer.Clip = TEXT("idle");
    Layer.BlendSpace = TEXT("move2d");
    Layer.bAdditive = true;
    Layer.BlendOut = 0.25f;
    Layer.Layer = 2;
    Layer.bUnsupported = true;
    FSCBlendSpace& Space = Kit->BlendSpaces.Add(TEXT("move2d"));
    Space.Id = TEXT("move2d");
    FSCBlendDimension& Dimension = Space.Dimensions.AddDefaulted_GetRef();
    Dimension.Name = TEXT("MoveSpeed");
    Dimension.Max = 7.f;
    Dimension.AuthoredMin = 0.5f;
    FSCBlendExample& Example = Space.Examples.AddDefaulted_GetRef();
    Example.Clip = TEXT("walk");
    Example.Params = {1.5f, std::numeric_limits<float>::quiet_NaN()};
    Example.Component = 1;
    Space.Faces = {{0, 1, 2}, {2, 3}};
    Space.Components = {TEXT("a"), TEXT("b")};
    FSCBlendSelector& Selector = Space.Selectors.AddDefaulted_GetRef();
    Selector.Name = TEXT("Turn");
    Selector.Dimension = 1;
    Selector.bChooseBlendSpace = true;
    Space.Unsupported = TEXT("two choosing selectors");
    FSCFlashlight& Torch = Kit->Flashlights.AddDefaulted_GetRef();
    Torch.Color = FColor(255, 230, 200);
    Torch.FovDeg = 60.f;
    Torch.ProjectorTexture = TEXT("Packages/helmet/cookie.png");
    Kit->ClipSidecars.Add(TEXT("idle"), TEXT("Packages/body/idle.json"));
    Kit->RootPort.Name = TEXT("body");
    FSCPlayerKit::FPart& Part = Kit->Parts.AddDefaulted_GetRef();
    Part.EntityName = TEXT("helmet");
    Part.Id = TEXT("child_3");
    Part.Port = Hand;
    Kit->Diagnostics.Add(TEXT("a sequenced layer"));
    const TSharedRef<FSCPlayerClipMap> Clips = MakeShared<FSCPlayerClipMap>();
    const TSharedRef<FSCPlayerClip> Clip = MakeShared<FSCPlayerClip>();
    Clip->Name = TEXT("idle");
    Clip->Frames = 30;
    Clip->Duration = 1.f;
    Clip->bCyclic = true;
    FSCPlayerClip::FTrack& Track = Clip->Tracks.AddDefaulted_GetRef();
    Track.Bone = 4;
    Track.RotTimes = {0.f, 15.f};
    Track.Rot = {FQuat4f::Identity, FQuat4f(0.f, 0.f, 0.7071068f, 0.7071068f)};
    Track.PosTimes = {0.f};
    Track.Pos = {FVector3f(1.f, 2.f, 3.f)};
    Clips->Add(Clip->Name, Clip);
    Clips->Add(TEXT("walk"), MakeShared<FSCPlayerClip>());
    // Every other field a value of its own (the attach resolves weapons by child id, ports by owner and helper, ...).
    Kit->Skeleton = TEXT("body.skel");
    Hand.OwnerEntity = TEXT("body");
    Hand.Attachment = TEXT("hand_attach");
    Hand.ItemOffsetHelper = TEXT("offset");
    Hand.SelectTag = TEXT("right");
    Weapon.ChildId = TEXT("child_7");
    Weapon.Subtype = TEXT("Rifle");
    Weapon.ItemType = TEXT("WeaponPersonal");
    Weapon.MannequinTags = {TEXT("rifle")};
    Weapon.HandGripHelper = TEXT("grip");
    Action.FireFragment = TEXT("fire");
    Action.OneShotFragment = TEXT("shot");
    Action.FireHelper = TEXT("barrel");
    Action.MuzzleHelper = TEXT("muzzle");
    Fire.Tags = {TEXT("rifle")};
    Fire.FragTags = {TEXT("ADS")};
    State.WeaponChildId = TEXT("child_7");
    State.MoveSpeedTag = TEXT("walk");
    State.Tags = {TEXT("stand")};
    Idle.Tags = {TEXT("t")};
    Idle.FragTags = {TEXT("f")};
    Layer.bMissing = true;
    Layer.bLoop = true;
    Layer.Speed = 1.25f;
    Layer.BlendIn = 0.125f;
    Layer.Weight = 0.5f;
    Layer.StartTime = 0.0625f;
    Layer.ExitTime = 0.75f;
    Layer.HasJointMask = true;
    Dimension.Min = -1.f;
    Dimension.AuthoredMax = 6.5f;
    Example.PlaybackScale = 1.2f;
    Example.bMissing = true;
    Selector.bLocked = true;
    Torch.Entity = TEXT("helmet");
    Torch.Port = TEXT("Armor_Helmet");
    Torch.DiffuseMult = 20.f;
    Torch.BulbRadiusM = 0.005f;
    Torch.DistanceM = 40.f;
    Torch.Cookie = TEXT("textures/lights/cookie.tif");
    Part.MeshAsset = TEXT("helmet.glb");
    Clip->Fps = 60.f;
    Clip->bAdditive = true;
    Track.PosTimes = {7.f};
    USCPlayerKitData* Data = NewObject<USCPlayerKitData>(V, TEXT("PlayerKit"));
    Data->Kit = Kit;
    Data->Clips = Clips;
    V->PlayerKit = Data;

    // Saved and loaded with the actor (a DuplicateObject copy: the data object goes with it, its payload written and read).
    Loaded = DuplicateObject(V, GetTransientPackage());
    Loaded->AddToRoot();
    const USCPlayerKitData* Back = Loaded->PlayerKit;
    if (!TestTrue(TEXT("the loaded actor's own kit data"), Back && Back != Data && Back->GetOuter() == Loaded)) return false;
    if (!TestTrue(TEXT("... with a kit and clips"), Back->HasData() && Back->Kit != Data->Kit)) return false;
    const FSCPlayerKit& K = *Back->Kit;
    TestTrue(TEXT("the kit's names"), K.Gender == Kit->Gender && K.Loadout == Kit->Loadout && K.CameraBone == Kit->CameraBone && K.RootMotionBone == Kit->RootMotionBone);
    TestTrue(TEXT("a hand port: flags, types, offset"),
             K.HandPorts.Num() == 1 && K.HandPorts[0].AttachBone == TEXT("RightHand") && K.HandPorts[0].Flags.Num() == 1 && K.HandPorts[0].Types.Num() == 1 &&
                 K.HandPorts[0].Types[0].Value.Num() == 2 && K.HandPorts[0].Offset.Equals(Hand.Offset, 0.f));
    if (TestEqual(TEXT("a weapon"), K.Weapons.Num(), 1))
    {
        const FSCPlayerWeapon& W = K.Weapons[0];
        TestTrue(TEXT("... its entity, size, stow port and grip"), W.Entity == Weapon.Entity && W.Size == 3 && W.StowPort.Name == Weapon.StowPort.Name &&
                                                                     W.OffHandGripHelper == TEXT("grip_off"));
        TestTrue(TEXT("... its fire action"), W.FireActions.Num() == 1 && W.FireActions[0].FireRate == 600.f && W.FireActions[0].StopFragment == TEXT("stop") &&
                                                 W.FireActions[0].EffectHelpers.Num() == 1);
        TestTrue(TEXT("... its fire fragment"), W.FireFragments.Num() == 1 && W.FireFragments[0].Layers.Num() == 1 && W.FireFragments[0].Layers[0].bLoop &&
                                                   W.FireFragments[0].Layers[0].Speed == 1.5f);
        TestTrue(TEXT("... its clips"), W.ClipSidecars.FindRef(TEXT("rifle_fire")) == TEXT("Packages/rifle/fire.json") &&
                                           W.FireClipJson.FindRef(TEXT("rifle_fire")) == TEXT("{\"frames\": 3}"));
    }
    if (TestTrue(TEXT("a motion state with a fragment and a layer"),
                 K.States.Num() == 1 && K.States[0].Fragments.Num() == 1 && K.States[0].Fragments[0].Layers.Num() == 1))
    {
        const FSCMotionLayer& L = K.States[0].Fragments[0].Layers[0];
        TestTrue(TEXT("... the state"), K.States[0].bFirstPerson && K.States[0].Stance == TEXT("stand") && K.States[0].Fragments[0].QueryFragTags.Num() == 1);
        TestTrue(TEXT("... the layer"), L.Clip == TEXT("idle") && L.BlendSpace == TEXT("move2d") && L.bAdditive && L.BlendOut == 0.25f && L.Layer == 2 && L.bUnsupported);
    }
    if (const FSCBlendSpace* B = K.BlendSpaces.Find(TEXT("move2d")); TestNotNull(TEXT("the blend space"), B))
    {
        TestTrue(TEXT("... its dimension"), B->Dimensions.Num() == 1 && B->Dimensions[0].Max == 7.f && B->Dimensions[0].AuthoredMin == 0.5f);
        TestTrue(TEXT("... its example, the unresolved parameter still NaN"), B->Examples.Num() == 1 && B->Examples[0].Params.Num() == 2 &&
                                                                                FMath::IsNaN(B->Examples[0].Params[1]) && B->Examples[0].Component == 1);
        TestTrue(TEXT("... its faces"), B->Faces.Num() == 2 && B->Faces[0].Num() == 3 && B->Faces[1].Num() == 2 && B->Faces[1][1] == 3);
        TestTrue(TEXT("... its components and selector"), B->Components.Num() == 2 && B->Selectors.Num() == 1 && B->Selectors[0].bChooseBlendSpace &&
                                                             B->Selectors[0].Dimension == 1 && B->Unsupported == Space.Unsupported);
    }
    TestTrue(TEXT("the torch"), K.Flashlights.Num() == 1 && K.Flashlights[0].Color == Torch.Color && K.Flashlights[0].FovDeg == 60.f &&
                                   K.Flashlights[0].ProjectorTexture == Torch.ProjectorTexture);
    TestTrue(TEXT("the body clips, root port, parts and diagnostics"),
             K.ClipSidecars.Num() == 1 && K.RootPort.Name == TEXT("body") && K.Parts.Num() == 1 && K.Parts[0].Id == TEXT("child_3") &&
                 K.Parts[0].Port.Types.Num() == 1 && K.Diagnostics.Num() == 1);
    // Every other field as set.
    {
        const FSCItemPort& H = K.HandPorts[0];
        const FSCPlayerWeapon& W = K.Weapons[0];
        const FSCFireAction& A = W.FireActions[0];
        const FSCWeaponFragment& F = W.FireFragments[0];
        const FSCMotionState& M = K.States[0];
        const FSCMotionFragment& I = M.Fragments[0];
        const FSCMotionLayer& L = I.Layers[0];
        const FSCBlendSpace& BS = K.BlendSpaces[TEXT("move2d")];
        const FSCFlashlight& Fl = K.Flashlights[0];
        TestTrue(TEXT("the port's owner, attachment, helper and tag"), K.Skeleton == TEXT("body.skel") && H.OwnerEntity == TEXT("body") &&
                                                                          H.Attachment == TEXT("hand_attach") && H.ItemOffsetHelper == TEXT("offset") &&
                                                                          H.SelectTag == TEXT("right") && K.Parts[0].Port.AttachBone == TEXT("RightHand"));
        TestTrue(TEXT("the weapon's child id, types, tags and grip"), W.ChildId == TEXT("child_7") && W.Subtype == TEXT("Rifle") &&
                                                                         W.ItemType == TEXT("WeaponPersonal") && W.MannequinTags.Num() == 1 &&
                                                                         W.HandGripHelper == TEXT("grip"));
        TestTrue(TEXT("the fire action's fragments and helpers"), A.Kind == TEXT("rapid") && A.FireFragment == TEXT("fire") && A.OneShotFragment == TEXT("shot") &&
                                                                      A.FireHelper == TEXT("barrel") && A.MuzzleHelper == TEXT("muzzle"));
        TestTrue(TEXT("the fire fragment's tags"), F.Fragment == TEXT("fire") && F.Tags.Num() == 1 && F.FragTags.Num() == 1 && F.Layers[0].Clip == TEXT("rifle_fire"));
        TestTrue(TEXT("the state's weapon, speed tag and tags"), M.WeaponChildId == TEXT("child_7") && M.MoveSpeedTag == TEXT("walk") && M.Tags.Num() == 1 &&
                                                                     I.Fragment == TEXT("MotionIdle") && I.Tags.Num() == 1 && I.FragTags.Num() == 1);
        TestTrue(TEXT("the layer's timing and flags"), L.bMissing && L.bLoop && L.Speed == 1.25f && L.BlendIn == 0.125f && L.Weight == 0.5f &&
                                                          L.StartTime == 0.0625f && L.ExitTime == 0.75f && L.HasJointMask);
        TestTrue(TEXT("the blend space's ranges, example and selector"), BS.Id == TEXT("move2d") && BS.Dimensions[0].Name == TEXT("MoveSpeed") &&
                                                                            BS.Dimensions[0].Min == -1.f && BS.Dimensions[0].AuthoredMax == 6.5f &&
                                                                            BS.Examples[0].Clip == TEXT("walk") && BS.Examples[0].Params[0] == 1.5f &&
                                                                            BS.Examples[0].PlaybackScale == 1.2f && BS.Examples[0].bMissing &&
                                                                            BS.Selectors[0].Name == TEXT("Turn") && BS.Selectors[0].bLocked);
        TestTrue(TEXT("the torch's item, port and light"), Fl.Entity == TEXT("helmet") && Fl.Port == TEXT("Armor_Helmet") && Fl.DiffuseMult == 20.f &&
                                                              Fl.BulbRadiusM == 0.005f && Fl.DistanceM == 40.f && Fl.Cookie == TEXT("textures/lights/cookie.tif"));
        TestTrue(TEXT("the part and the kit's clip"), K.Parts[0].EntityName == TEXT("helmet") && K.Parts[0].MeshAsset == TEXT("helmet.glb") &&
                                                         K.ClipSidecars.FindRef(TEXT("idle")) == TEXT("Packages/body/idle.json"));
    }
    if (!TestEqual(TEXT("the clips"), Back->Clips->Num(), 2)) return false;
    const TSharedPtr<const FSCPlayerClip> C = Back->Clips->FindRef(TEXT("idle"));
    if (!TestTrue(TEXT("the idle clip"), C.IsValid() && C->Name == TEXT("idle") && C->bCyclic && C->bAdditive && C->Fps == 60.f && C->Frames == 30.f &&
                                             C->Duration == 1.f && C->Tracks.Num() == 1))
        return false;
    const FSCPlayerClip::FTrack& T = C->Tracks[0];
    TestTrue(TEXT("... its track"), T.Bone == 4 && T.RotTimes.Num() == 2 && T.RotTimes[1] == 15.f && T.PosTimes.Num() == 1 && T.PosTimes[0] == 7.f && T.Rot.Num() == 2 && T.Rot[1].Equals(Track.Rot[1], 0.f) && T.Pos.Num() == 1 &&
                                       T.Pos[0] == FVector3f(1.f, 2.f, 3.f));
    // The loaded clip samples as the parsed one.
    TArray<FTransform> PoseA, PoseB;
    PoseA.Init(FTransform::Identity, 5);
    PoseB = PoseA;
    Clip->Sample(0.25f, PoseA);
    C->Sample(0.25f, PoseB);
    TestTrue(TEXT("... it samples the same"), PoseA[4].Equals(PoseB[4], 0.0));

    // A payload of another layout, or not exactly one payload, reads as none (a load skips it whole).
    TArray<uint8> Payload;
    USCPlayerKitData::WritePayload(*Kit, *Clips, Payload);
    TSharedPtr<const FSCPlayerKit> ReadKit;
    TSharedPtr<const FSCPlayerClipMap> ReadClips;
    TestTrue(TEXT("this layout reads"), USCPlayerKitData::ReadPayload(USCPlayerKitData::Version, Payload, ReadKit, ReadClips) && ReadKit && ReadClips);
    TestFalse(TEXT("another layout does not"), USCPlayerKitData::ReadPayload(USCPlayerKitData::Version + 1, Payload, ReadKit, ReadClips) || ReadKit || ReadClips);
    TArray<uint8> Longer = Payload;
    Longer.Add(0);
    TestFalse(TEXT("nor a payload with more after it"), USCPlayerKitData::ReadPayload(USCPlayerKitData::Version, Longer, ReadKit, ReadClips));

    // A body without data stays without (no kit and no clips).
    Data->Kit.Reset();
    Data->Clips.Reset();
    const USCPlayerKitData* Empty = DuplicateObject(Data, GetTransientPackage());
    TestTrue(TEXT("no data loads as none"), Empty && !Empty->HasData() && !Empty->Kit && !Empty->Clips);
    return !HasAnyErrors();
}

// WP-O1-4b-2b: the traversal clips a prepared scenario keeps: every seat and ladder action its records name, and the clips
// read for them saved and loaded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCSavedTraversalClipsTest, "SCBridge.Prepare.SavedTraversalClips", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCSavedTraversalClipsTest::RunTest(const FString&)
{
    ASCVehicle* V = NewObject<ASCVehicle>(GetTransientPackage());
    V->AddToRoot();
    ON_SCOPE_EXIT { V->RemoveFromRoot(); };
    // Two seats (one action named by both, one without an animation) and a ladder.
    for (const TArray<FName>& Animations : {TArray<FName>{TEXT("seat_enter"), TEXT("seat_idle")}, TArray<FName>{TEXT("seat_idle"), NAME_None}})
    {
        FSCSceneInstance& Child = V->Manifest.Children.AddDefaulted_GetRef();
        FSCSeatDefinition Seat;
        for (const FName Animation : Animations)
        {
            FSCSeatAction& Action = Seat.Actions.AddDefaulted_GetRef();
            Action.State = TEXT("Enter");
            Action.Animation = Animation;
        }
        Child.Seat = Seat;
    }
    V->Manifest.Children.AddDefaulted(); // no seat
    FSCLadderAnchor& Ladder = V->LadderAnchors.AddDefaulted_GetRef();
    Ladder.Data.Actions.Add(TEXT("ClimbUp")).Clip = TEXT("ladder_up");
    Ladder.Data.Actions.Add(TEXT("Exit")).Clip = TEXT("ladder_exit_top");
    TestEqual(TEXT("every seat and ladder action, once, sorted"), FString::Join(SCTraversal::ClipNames(*V), TEXT(",")),
              FString(TEXT("ladder_exit_top,ladder_up,seat_enter,seat_idle")));

    USCTraversalClipsData* Data = NewObject<USCTraversalClipsData>(GetTransientPackage());
    Data->BodyLevel = TEXT("/Game/SC/Players/Maps/Player_male_x");
    const TSharedRef<FSCPlayerClipMap> Clips = MakeShared<FSCPlayerClipMap>();
    const TSharedRef<FSCPlayerClip> Clip = MakeShared<FSCPlayerClip>();
    Clip->Name = TEXT("ladder_up");
    Clip->Duration = 2.f;
    Clip->Tracks.AddDefaulted_GetRef().Bone = 3;
    Clips->Add(Clip->Name, Clip);
    Data->Clips = Clips;
    const USCTraversalClipsData* Back = DuplicateObject(Data, GetTransientPackage());
    TestTrue(TEXT("saved and loaded: the body and the clips"),
             Back && Back->BodyLevel == Data->BodyLevel && Back->Clips && Back->Clips != Data->Clips && Back->Clips->Num() == 1 &&
                 Back->Clips->FindRef(TEXT("ladder_up")) && Back->Clips->FindRef(TEXT("ladder_up"))->Duration == 2.f &&
                 Back->Clips->FindRef(TEXT("ladder_up"))->Tracks.Num() == 1 && Back->Clips->FindRef(TEXT("ladder_up"))->Tracks[0].Bone == 3);
    // Which source serves a request in a prepared scenario.
    FSCTraversalClips::FResult Served;
    TestTrue(TEXT("the set covers the request: served"), FSCTraversalClips::ServeFromPrepared(&Clips.Get(), {TEXT("ladder_up")}, true, Served) &&
                                                             Served.Error.IsEmpty() && Served.Clips.Num() == 1);
    TestFalse(TEXT("a loaded set lacking a name, the archive open: the archive serves it"),
              FSCTraversalClips::ServeFromPrepared(&Clips.Get(), {TEXT("ladder_up"), TEXT("seat_enter")}, true, Served));
    TestTrue(TEXT("... the archive closed: the missing clip"),
             FSCTraversalClips::ServeFromPrepared(&Clips.Get(), {TEXT("seat_enter")}, false, Served) && Served.Error.Contains(TEXT("seat_enter")) &&
                 Served.Clips.IsEmpty());
    TestTrue(TEXT("a set that does not load, even with the archive open: its error, no fallback"),
             FSCTraversalClips::ServeFromPrepared(nullptr, {TEXT("ladder_up")}, true, Served) && !Served.Error.IsEmpty());

    Data->Clips = MakeShared<FSCPlayerClipMap>();
    const USCTraversalClipsData* None = DuplicateObject(Data, GetTransientPackage());
    TestTrue(TEXT("a scenario with no seat or ladder: an empty set, not none"), None && None->Clips && None->Clips->IsEmpty());
    return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
