// C2 fixes (synthetic geometry and clips; no game data):
// - SCBridge.Doors.RestStateAndToggle: a door spawns in its data state (closed: a single-procedural door holds its transition's
//   start; opened: an open/close door holds its open end), toggling plays from the current pose (no snap, also mid-stroke),
//   a single-procedural door closes by its transition backwards, item-port links move the port's door, ResetPose restores.
// - SCBridge.Doors.ReopenPlaysClip: a sensor door, a pad door and a single-procedural ramp, three cycles each: every stroke
//   plays its clip from the start of play with no jump, crosses its sound exactly once and ends with state and pose agreeing
//   (an open/close door used to resume the other fragment's stale held playhead: a snap, no sound); a door turned around
//   mid-stroke plays the new direction's start sound once; a raw clip records or forgets the door's state; ResetPose drops
//   the sensors' pending exits; a targeted layer whose rig is gone poses nothing.
// - SCBridge.Panels.ButtonPanel: the shared operable-panel mechanism on a game panel (light control): the crosshair on a
//   node of the render picks it (nothing between nodes, nothing from behind), E switches the panel's light groups to the
//   state the group's own interaction names, the panel's state machine takes the press and follows the group, and the face
//   shows the render of that state.
// - SCBridge.Lift.RiderPassesPath: a moving carriage carries the capsule on it (LiftRiddenBy), the leg's path obstacles are
//   what it passes through (not the carriage, not a door, not the surface the leg ends on), and a character standing over
//   a slab that reaches into the shaft rides the whole leg at a steady height on the carriage.
// - SCBridge.Reader.C2FixesFields: the cache-format-39 fields reach the records (and format 40's interaction_distance).
// - SCBridge.Doors.ExteriorPanelReach: a door panel high on the outside of a closed door (the C2's ramp and hatch pads,
//   3.5-4 m from a standing player's eye) resolves to its door from as far as its own UI is operated (PanelReach, the
//   game's render-to-texture interactionDistance), not beyond, not through an occluder; anything that is not a panel keeps
//   the 3 m reach; E on it plays the door like the interior pad.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "SCVehicle.h"
#include "SCLights.h"
#include "SCLift.h"
#include "SCRigComponent.h"
#include "SCClipCatalog.h"
#include "SCSkeletonBuilder.h"
#include "SCPackageReader.h"
#include "SCPackageSource.h"
#include "SCFirstPersonCharacter.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "HAL/IConsoleManager.h"

namespace
{
/** A rig of one bone ("Bone", index 1) and its clips: keys of the bone's X position, one per frame at 10 fps. */
USCRigComponent* AddRig(ASCVehicle* V, const TCHAR* Owner)
{
    FSCDerivedMesh Data;
    FSCRigNode Node;
    Node.SourceName = TEXT("Bone");
    Data.RigNodes.Add(Node);
    USkeleton* Skeleton;
    USkeletalMesh* Mesh = FSCSkeletonBuilder::Build(Data, V, Skeleton);
    FString Error;
    FSCSkeletalMeshImporter::Build(Data, Mesh, Error);
    auto* Rig = NewObject<USCRigComponent>(V);
    V->AddInstanceComponent(Rig);
    Rig->SetSkeletalMesh(Mesh);
    Rig->OwnerEntity = Owner;
    Rig->RegisterComponent();
    Rig->ResetPose();
    V->Rigs.Add(Rig);
    V->SkeletalMeshes.Add(Mesh);
    return Rig;
}
void AddClip(ASCVehicle* V, USCRigComponent* Rig, const TCHAR* Id, FName Tag, std::initializer_list<double> Xs)
{
    FSCImportedClip Clip;
    Clip.Id = Id;
    Clip.Fps = 10;
    Clip.LastFrame = int32(Xs.size()) - 1;
    FSCClipBinding Binding;
    Binding.Mesh = Rig->GetSkeletalMeshAsset();
    FSCBoneTrack Track;
    Track.Bone = 1;
    Track.bPosition = true;
    Track.Fps = 10;
    for (const double X : Xs) Track.Keys.Add(FTransform(FVector(X, 0, 0)));
    Binding.Tracks.Add(Track);
    Clip.Bindings.Add(Binding);
    FSCClipFragment F;
    F.Fragment = TEXT("SCItemDoor");
    F.FragmentTags.Add(Tag);
    F.Scopes = {TEXT("SCItemDoor")};
    F.Speed = 1;
    F.OptionWeight = 1;
    F.AnimationNames.Add(Clip.Id);
    F.OptionId = Clip.Id.ToString();
    Clip.Fragments.Add(F);
    V->ClipCatalog->Clips.Add(Clip);
}
double BoneX(const USCRigComponent* Rig) { return Rig->LocalPose.IsValidIndex(1) ? Rig->LocalPose[1].GetTranslation().X : -1; }
const FSCClipPlayback* Playing(const ASCVehicle* V, const USCRigComponent* Rig)
{
    return V->Playback.FindByPredicate([&](const FSCClipPlayback& P) { return P.TargetRig.Get() == Rig; });
}
TSharedPtr<FJsonObject> ParseJson(const FString& Text)
{
    TSharedPtr<FJsonObject> O;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O);
    return O;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCDoorRestStateTest, "SCBridge.Doors.RestStateAndToggle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCDoorRestStateTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* V = World->SpawnActor<ASCVehicle>();
    V->ClipCatalog = NewObject<USCClipCatalog>(V);
    // A: a single-procedural ramp (its one transition opens 0 -> 100; its separate close clip ends 5 off the bind pose,
    // which the game does not play for this door), closed in the data.
    USCRigComponent* A = AddRig(V, TEXT("Ramp"));
    AddClip(V, A, TEXT("ramp_open"), TEXT("Open"), {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100});
    AddClip(V, A, TEXT("ramp_close"), TEXT("Close"), {100, 80, 60, 40, 20, 5});
    FSCDoorRecord RA;
    RA.Door.bValid = true;
    RA.Door.TransitionTag = TEXT("Open");
    RA.Door.bIsOpened = false;
    V->DoorRecords.Add(A, RA);
    // B: an open/close door opened in the data, whose Open and Close go on to the door in port "port_c".
    USCRigComponent* B = AddRig(V, TEXT("Hatch"));
    AddClip(V, B, TEXT("hatch_open"), TEXT("Open"), {0, 25, 50});
    AddClip(V, B, TEXT("hatch_close"), TEXT("Close"), {50, 25, 0});
    FSCDoorRecord RB;
    RB.Door.bValid = true;
    RB.Door.OpenTag = TEXT("Open");
    RB.Door.CloseTag = TEXT("Close");
    RB.Door.bIsOpened = true;
    RB.PortLinks = {{TEXT("Open"), TEXT("port_c"), TEXT("Open")}, {TEXT("Close"), TEXT("port_c"), TEXT("Close")}};
    V->DoorRecords.Add(B, RB);
    // C: an open/close door without door data (Mannequin Open/Close), in port "port_c".
    USCRigComponent* C = AddRig(V, TEXT("Leaf"));
    AddClip(V, C, TEXT("leaf_open"), TEXT("Open"), {0, 30});
    AddClip(V, C, TEXT("leaf_close"), TEXT("Close"), {30, 0});
    V->PortRigs.Add(TEXT("port_c"), C);
    // D: a single-procedural door whose fragment has no Close clip at all (a door by its data alone).
    USCRigComponent* Dr = AddRig(V, TEXT("Hatch2"));
    AddClip(V, Dr, TEXT("hatch2_open"), TEXT("Open"), {0, 10, 20});
    FSCDoorRecord RD;
    RD.Door.bValid = true;
    RD.Door.TransitionTag = TEXT("Open");
    V->DoorRecords.Add(Dr, RD);
    // E: an open/close door closed in the data whose close fragment ends 7 off its bind pose: it spawns at that end.
    USCRigComponent* E = AddRig(V, TEXT("Locker"));
    AddClip(V, E, TEXT("locker_open"), TEXT("Open"), {7, 40});
    AddClip(V, E, TEXT("locker_close"), TEXT("Close"), {40, 7});
    FSCDoorRecord RE;
    RE.Door.bValid = true;
    RE.Door.OpenTag = TEXT("Open");
    RE.Door.CloseTag = TEXT("Close");
    RE.Door.bIsOpened = false;
    V->DoorRecords.Add(E, RE);
    V->BuildExploration();
    const int32 DA = V->DoorForRig(A), DB = V->DoorForRig(B), DC = V->DoorForRig(C);
    if (!TestTrue(TEXT("three doors"), DA != INDEX_NONE && DB != INDEX_NONE && DC != INDEX_NONE)) return false;
    TestTrue(TEXT("a door by its data alone (no Close clip)"), V->DoorForRig(Dr) != INDEX_NONE && V->Doors[V->DoorForRig(Dr)].bReverseClose);
    TestNearlyEqual(TEXT("a closed door holds its close fragment's end"), BoneX(E), 7.0, 1e-6);
    TestFalse(TEXT("and is recorded closed"), V->IsDoorOpen(V->DoorForRig(E)));

    // Spawn: the data state.
    TestTrue(TEXT("A single-procedural, closes backwards"), V->Doors[DA].bReverseClose && V->Doors[DA].OpenTag == FName(TEXT("Open")));
    TestFalse(TEXT("A closed at spawn"), V->IsDoorOpen(DA));
    TestNearlyEqual(TEXT("A holds its transition's start"), BoneX(A), 0.0, 1e-6);
    TestTrue(TEXT("B open at spawn (is_opened)"), V->IsDoorOpen(DB));
    TestNearlyEqual(TEXT("B holds its open end"), BoneX(B), 50.0, 1e-6);
    TestFalse(TEXT("C closed (no data)"), V->IsDoorOpen(DC));
    TestNearlyEqual(TEXT("C at bind"), BoneX(C), 0.0, 1e-6);

    // A opens from where it stands (frame 0), half way, then is turned around: it plays its transition backwards from the
    // frame it shows, and ends at its start (not at the separate close clip's end).
    TestTrue(TEXT("A opens"), V->ToggleDoor(DA));
    const FSCClipPlayback* P = Playing(V, A);
    TestTrue(TEXT("forward from frame 0"), P && P->Speed > 0 && FMath::IsNearlyZero(P->Frame));
    V->Tick(0.55f);
    TestNearlyEqual(TEXT("past half open, between keys"), BoneX(A), 55.0, 1e-3);
    TestTrue(TEXT("A turned around mid-stroke"), V->ToggleDoor(DA));
    P = Playing(V, A);
    TestTrue(TEXT("its transition, backwards, from the frame it shows"), P && P->Speed < 0 && FMath::IsNearlyEqual(P->Frame, 5.5, 1e-6) &&
                                                                        V->ClipCatalog->Clips[P->Clip].Id == FName(TEXT("ramp_open")));
    TestNearlyEqual(TEXT("no snap at the turn (the exact playhead)"), BoneX(A), 55.0, 1e-3);
    V->Tick(1.f);
    TestNearlyEqual(TEXT("closed at its transition's start"), BoneX(A), 0.0, 1e-3);
    TestFalse(TEXT("A recorded closed"), V->IsDoorOpen(DA));

    // B closes from its open pose (the close clip's first frame), and its link closes C (already closed: nothing plays);
    // opening B opens C along the port link.
    TestTrue(TEXT("B closes"), V->ToggleDoor(DB));
    P = Playing(V, B);
    TestTrue(TEXT("close clip from its frame matching the open pose"), P && P->Speed > 0 && FMath::IsNearlyZero(P->Frame) &&
                                                                         V->ClipCatalog->Clips[P->Clip].Id == FName(TEXT("hatch_close")));
    TestNull(TEXT("C does not move (closed already)"), Playing(V, C));
    V->Tick(1.f);
    TestTrue(TEXT("B opens"), V->ToggleDoor(DB));
    TestTrue(TEXT("the port link opens C"), V->IsDoorOpen(DC) && Playing(V, C) != nullptr);
    V->Tick(1.f);
    TestNearlyEqual(TEXT("C open"), BoneX(C), 30.0, 1e-3);

    // ResetPose: every door back at its data state.
    V->ResetPose();
    TestNearlyEqual(TEXT("A closed again"), BoneX(A), 0.0, 1e-6);
    TestTrue(TEXT("B open again"), V->IsDoorOpen(DB) && FMath::IsNearlyEqual(BoneX(B), 50.0, 1e-6));
    TestFalse(TEXT("C closed again"), V->IsDoorOpen(DC));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCDoorReopenTest, "SCBridge.Doors.ReopenPlaysClip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCDoorReopenTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* V = World->SpawnActor<ASCVehicle>();
    V->ClipCatalog = NewObject<USCClipCatalog>(V);
    // Each clip's source carries the game's door sounds: an open/close door's at the start of its clip (t = 0.0001), a
    // single-procedural door's deploy forwards at its start and retract backwards at its end (t = 0.9999).
    auto Sound = [&](const TCHAR* Trigger, double Frame, int8 Direction) {
        FSCClipAudioEvent E;
        E.Frame = Frame;
        E.Direction = Direction;
        E.Trigger = Trigger;
        V->ClipCatalog->Clips.Last().Bindings[0].AudioEvents.Add(E);
    };
    auto OpenClose = [&](const TCHAR* Owner) {
        USCRigComponent* Rig = AddRig(V, Owner);
        AddClip(V, Rig, *FString::Printf(TEXT("%s_open"), Owner), TEXT("Open"), {0, 10, 20, 30, 40, 50});
        Sound(TEXT("Play_Door_Open"), 0.0005, 1);
        AddClip(V, Rig, *FString::Printf(TEXT("%s_close"), Owner), TEXT("Close"), {50, 40, 30, 20, 10, 0});
        Sound(TEXT("Play_Door_Close"), 0.0005, 1);
        FSCDoorRecord R;
        R.Door.bValid = true;
        R.Door.OpenTag = TEXT("Open");
        R.Door.CloseTag = TEXT("Close");
        R.Door.bIsOpened = false; // closed in the data: it spawns holding its close fragment's end
        V->DoorRecords.Add(Rig, R);
        return Rig;
    };
    USCRigComponent* S = OpenClose(TEXT("SensorDoor")); // the C2's circle doors: a proximity sensor opens and closes it
    USCRigComponent* P = OpenClose(TEXT("PadDoor"));    // the C2's side doors: a press (E, a pad) toggles it
    USCRigComponent* R = AddRig(V, TEXT("Ramp"));       // the C2's rear ramp: one transition, closed by playing it backwards
    AddClip(V, R, TEXT("ramp_transition"), TEXT("Open"), {0, 10, 20, 30, 40, 50});
    Sound(TEXT("Play_Ramp_Deploy"), 0.0005, 1);
    Sound(TEXT("Play_Ramp_Retract"), 4.9995, -1);
    FSCDoorRecord RR;
    RR.Door.bValid = true;
    RR.Door.TransitionTag = TEXT("Open");
    RR.Door.bIsOpened = false;
    V->DoorRecords.Add(R, RR);
    V->BuildExploration();
    const int32 DS = V->DoorForRig(S), DP = V->DoorForRig(P), DR = V->DoorForRig(R);
    if (!TestTrue(TEXT("three doors"), DS != INDEX_NONE && DP != INDEX_NONE && DR != INDEX_NONE)) return false;
    FSCDoorSensor Sensor;
    Sensor.Anchor = V->GetRootComponent();
    Sensor.Box = FBox(FVector(-100), FVector(100));
    Sensor.ExitDelay = 0.5f;
    Sensor.Doors.Add(S);
    V->DoorSensors.Add(Sensor);

    // One stroke: Act starts it; the door's clip must start at the start of play from the pose the door shows (frame 0 forward,
    // the last frame backwards), advance, cross its sound, and end at the state's pose, with the recorded state agreeing.
    auto Stroke = [&](const FString& What, USCRigComponent* Rig, int32 Door, bool bOpen, const TCHAR* Trigger, TFunctionRef<void()> Act) {
        const double Before = BoneX(Rig);
        TestNearlyEqual(*(What + TEXT(": at rest at the other state's pose")), Before, bOpen ? 0.0 : 50.0, 1e-3);
        const uint64 Events = V->ClipAudioEvents;
        Act();
        TestEqual(*(What + TEXT(": recorded state")), V->IsDoorOpen(Door), bOpen);
        const FSCClipPlayback* Clip = Playing(V, Rig);
        if (!TestNotNull(*(What + TEXT(": a clip plays")), Clip)) return;
        const double Start = Clip->Speed < 0 ? V->ClipCatalog->Clips[Clip->Clip].LastFrame : 0.0;
        TestTrue(*FString::Printf(TEXT("%s: from the start pose (frame %.2f, start of play %.0f)"), *What, Clip->Frame, Start), FMath::IsNearlyEqual(Clip->Frame, Start, 1e-3));
        TestNearlyEqual(*(What + TEXT(": no snap at the start")), BoneX(Rig), Before, 1e-3);
        V->Tick(0.25f);
        const double Mid = BoneX(Rig);
        TestTrue(*FString::Printf(TEXT("%s: the playhead advances (x %.2f mid-stroke)"), *What, Mid), Mid > 1.0 && Mid < 49.0);
        TestTrue(*FString::Printf(TEXT("%s: its sound fires (%llu events, last %s)"), *What, V->ClipAudioEvents - Events, *V->LastClipAudioTrigger),
                 V->ClipAudioEvents > Events && V->LastClipAudioTrigger == Trigger);
        V->Tick(1.f);
        TestEqual(*(What + TEXT(": one sound per stroke")), int64(V->ClipAudioEvents - Events), int64(1));
        TestNull(*(What + TEXT(": finished")), Playing(V, Rig));
        TestNearlyEqual(*(What + TEXT(": at the state's pose")), BoneX(Rig), bOpen ? 50.0 : 0.0, 1e-3);
        TestEqual(*(What + TEXT(": the state agrees")), V->IsDoorOpen(Door), bOpen);
    };
    const FVector Inside(0, 0, 0), Outside(1000, 0, 0);
    double Now = 10;
    for (int32 Cycle = 1; Cycle <= 3; ++Cycle)
    {
        // The sensor: walking in opens it; walking out closes it after the sensor's exit delay (not before).
        Stroke(FString::Printf(TEXT("sensor open %d"), Cycle), S, DS, true, TEXT("Play_Door_Open"), [&] { V->UpdateDoorSensors(Inside, Now); });
        V->UpdateDoorSensors(Inside, Now += 1.5);
        V->UpdateDoorSensors(Outside, Now += 0.3);
        TestTrue(*FString::Printf(TEXT("sensor %d: still open within the exit delay"), Cycle), V->IsDoorOpen(DS) && !Playing(V, S));
        Stroke(FString::Printf(TEXT("sensor close %d"), Cycle), S, DS, false, TEXT("Play_Door_Close"), [&] { V->UpdateDoorSensors(Outside, Now += 0.3); });
        Now += 2;
        // The pad door and the ramp: presses.
        Stroke(FString::Printf(TEXT("pad open %d"), Cycle), P, DP, true, TEXT("Play_Door_Open"), [&] { V->ToggleDoor(DP); });
        Stroke(FString::Printf(TEXT("pad close %d"), Cycle), P, DP, false, TEXT("Play_Door_Close"), [&] { V->ToggleDoor(DP); });
        Stroke(FString::Printf(TEXT("ramp open %d"), Cycle), R, DR, true, TEXT("Play_Ramp_Deploy"), [&] { V->ToggleDoor(DR); });
        Stroke(FString::Printf(TEXT("ramp close %d"), Cycle), R, DR, false, TEXT("Play_Ramp_Retract"), [&] { V->ToggleDoor(DR); });
    }
    // Turned around mid-stroke (the open/close door): the other clip from the frame nearest the pose, no snap, and the new
    // direction's start sound (authored at the start of play) plays once.
    TestTrue(TEXT("pad opens"), V->ToggleDoor(DP));
    V->Tick(0.25f);
    const double Mid = BoneX(P);
    uint64 Events = V->ClipAudioEvents;
    TestTrue(TEXT("pad turned around"), V->ToggleDoor(DP));
    const FSCClipPlayback* Turn = Playing(V, P);
    TestTrue(TEXT("its close clip from the frame showing the pose"), Turn && V->ClipCatalog->Clips[Turn->Clip].Id == FName(TEXT("PadDoor_close")) &&
                                                                     FMath::IsNearlyEqual(Turn->Frame, (50.0 - Mid) / 10.0, 0.06));
    TestNearlyEqual(TEXT("no snap at the turn"), BoneX(P), Mid, 0.6);
    V->Tick(1.f);
    TestTrue(TEXT("the close sound plays once at the turn"), V->ClipAudioEvents - Events == 1 && V->LastClipAudioTrigger == TEXT("Play_Door_Close"));
    TestNearlyEqual(TEXT("closed"), BoneX(P), 0.0, 1e-3);
    // The ramp turned around: its transition backwards from its exact playhead, its retract sound once.
    TestTrue(TEXT("ramp opens"), V->ToggleDoor(DR));
    V->Tick(0.25f);
    const double RampMid = BoneX(R);
    Events = V->ClipAudioEvents;
    TestTrue(TEXT("ramp turned around"), V->ToggleDoor(DR));
    TestNearlyEqual(TEXT("ramp: no snap at the turn"), BoneX(R), RampMid, 1e-3);
    V->Tick(1.f);
    TestTrue(TEXT("the retract sound plays once at the turn"), V->ClipAudioEvents - Events == 1 && V->LastClipAudioTrigger == TEXT("Play_Ramp_Retract"));

    // A clip outside the door interaction: the door's own open clip snapped to its end records the door open (a press then
    // closes it, from its close clip's start); another clip on the door's channels leaves the state unknown (read closed).
    const int32 PadOpenClip = V->ClipCatalog->Clips.IndexOfByPredicate([](const FSCImportedClip& C) { return C.Id == FName(TEXT("PadDoor_open")); });
    TestTrue(TEXT("raw snap of the open clip"), V->SnapClipAt(PadOpenClip, P, true));
    TestTrue(TEXT("recorded open with the open pose"), V->IsDoorOpen(DP) && FMath::IsNearlyEqual(BoneX(P), 50.0, 1e-3));
    Stroke(TEXT("pad close after a raw open"), P, DP, false, TEXT("Play_Door_Close"), [&] { V->ToggleDoor(DP); });
    AddClip(V, P, TEXT("pad_other"), TEXT("Wiggle"), {0, 30});
    V->ClipCatalog->Clips.Last().Fragments[0].Fragment = TEXT("Other");
    TestTrue(TEXT("raw clip on the door's channels"), V->SnapClipAt(V->ClipCatalog->Clips.Num() - 1, P, true));
    TestFalse(TEXT("its state unknown: read closed"), V->IsDoorOpen(DP));
    TestTrue(TEXT("a press opens it from the pose it shows"), V->ToggleDoor(DP) && V->IsDoorOpen(DP) && Playing(V, P) != nullptr);
    V->Tick(2.f);
    TestNearlyEqual(TEXT("open"), BoneX(P), 50.0, 1e-3);

    // ResetPose clears the sensors' pending exits: a door the sensor opened, reset, then opened by a press stays open.
    V->UpdateDoorSensors(Inside, Now += 1);
    V->Tick(2.f);
    V->ResetPose();
    TestTrue(TEXT("sensor door closed by the reset"), !V->IsDoorOpen(DS) && FMath::IsNearlyEqual(BoneX(S), 0.0, 1e-3));
    TestTrue(TEXT("pressed open"), V->ToggleDoor(DS));
    V->Tick(2.f);
    V->UpdateDoorSensors(Outside, Now += 5);
    TestTrue(TEXT("no stale sensor exit closes it"), V->IsDoorOpen(DS) && !Playing(V, S));

    // A targeted layer whose rig is gone poses nothing: not the sensor door that shares its mesh.
    auto* Twin = NewObject<USCRigComponent>(V);
    V->AddInstanceComponent(Twin);
    Twin->SetSkeletalMesh(S->GetSkeletalMeshAsset());
    Twin->RegisterComponent();
    Twin->ResetPose();
    V->Rigs.Add(Twin);
    const int32 SensorClose = V->ClipCatalog->Clips.IndexOfByPredicate([](const FSCImportedClip& C) { return C.Id == FName(TEXT("SensorDoor_close")); });
    TestTrue(TEXT("the twin plays the close clip"), V->PlayClipAt(SensorClose, Twin, 1.f));
    V->Rigs.Remove(Twin);
    Twin->DestroyComponent();
    V->Tick(0.25f);
    TestNearlyEqual(TEXT("the sensor door keeps its open pose"), BoneX(S), 50.0, 1e-3);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCButtonPanelTest, "SCBridge.Panels.ButtonPanel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCButtonPanelTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* V = World->SpawnActor<ASCVehicle>();
    // The panel's face: a 40 x 70 cm portrait quad facing +X, UV 0..1 over it (the render's image space).
    auto* Face = NewObject<UStaticMeshComponent>(V);
    Face->SetupAttachment(V->GetRootComponent());
    Face->SetRelativeLocation(FVector(0, 0, 100));
    Face->RegisterComponent();
    FSCButtonFace F;
    const FVector3f TL(0, 20, 35), TR(0, -20, 35), BL(0, 20, -35), BR(0, -20, -35);
    F.Geometry.Positions = {TL, BL, TR, TR, BL, BR};
    F.Geometry.UVs = {FVector2f(0, 0), FVector2f(0, 1), FVector2f(1, 0), FVector2f(1, 0), FVector2f(0, 1), FVector2f(1, 1)};
    F.Geometry.Fronts = {FVector3f(3, 0, 0), FVector3f(3, 0, 0)};
    F.Geometry.Nodes = {INDEX_NONE, INDEX_NONE};
    F.Geometry.Soft = {false, false};
    F.Component = Face;
    F.Slot = 0;
    F.EntityId = TEXT("panel");
    F.Links = {TEXT("chip")}; // the chipset the group links too
    F.Sendables = {{TEXT("LightsOn"), TEXT("/Interaction/LightGroup/SetDefault")},
                   {TEXT("LightsLow"), TEXT("/Interaction/LightGroup/SetAuxiliary")},
                   {TEXT("LightsAlarm"), TEXT("/Interaction/LightGroup/SetEmergency")}};
    F.ImageSizePx = FVector2D(720, 1280);
    auto Button = [](const TCHAR* Node, const TCHAR* I, double Y) { return FSCUiButton{Node, FName(I), FBox2D(FVector2D(89, Y), FVector2D(630, Y + 313))}; };
    F.Buttons = {Button(TEXT("OnLightsButton"), TEXT("LightsOn"), 117), Button(TEXT("AuxLightsButton"), TEXT("LightsLow"), 484),
                 Button(TEXT("EmergencyLightsButton"), TEXT("LightsAlarm"), 850)};
    FSCUiStateType Light;
    Light.Name = TEXT("LightState");
    Light.DefaultState = TEXT("LightsOn");
    Light.Linked = {{TEXT("/State/LightGroup/Default"), TEXT("LightsOn")}, {TEXT("/State/LightGroup/Auxiliary"), TEXT("LightsLow")},
                    {TEXT("/State/LightGroup/Emergency"), TEXT("LightsAlarm")}};
    FSCUiStateType Switch;
    Switch.Name = TEXT("LightSwitchState");
    Switch.DefaultState = TEXT("LightSwitchOn");
    Switch.Transitions = {{FString(), TEXT("LightsOn"), TEXT("LightSwitchOn")}, {FString(), TEXT("LightsLow"), TEXT("LightSwitchLow")},
                          {FString(), TEXT("LightsAlarm"), TEXT("LightSwitchAlarm")}};
    F.StateTypes = {Light, Switch};
    auto Image = [](const TCHAR* Path, std::initializer_list<const TCHAR*> Pins) {
        FSCUiStateImage I;
        I.ImagePath = Path;
        for (const TCHAR* S : Pins) I.States.Add(S);
        I.States.Sort();
        return I;
    };
    F.StateImages = {Image(TEXT("alarm.png"), {TEXT("LightState.LightsAlarm")}), Image(TEXT("low.png"), {TEXT("LightSwitchState.LightSwitchLow")}),
                     Image(TEXT("sw_alarm.png"), {TEXT("LightSwitchState.LightSwitchAlarm")}),
                     Image(TEXT("emergency.png"), {TEXT("LightState.LightsAlarm"), TEXT("LightSwitchState.LightSwitchAlarm")})};
    F.DefaultImage = TEXT("default.png");
    F.MakeMaterial = [V](UTexture*) -> UMaterialInterface* { return UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), V); };
    V->Interactions.At(0, TEXT("panel"));
    // The group: what it receives (its linking tags and the light state each ends in), and its lights.
    V->LightGroups.Add({0, TEXT("lg"), TEXT("LG_Test"), {TEXT("chip")},
                        {{TEXT("Override Disabled"), FString(), TEXT("DisableOverride")},
                         {TEXT("Default Override"), TEXT("/Interaction/LightGroup/SetDefault"), TEXT("Default")},
                         {TEXT("Auxiliary Override"), TEXT("/Interaction/LightGroup/SetAuxiliary"), TEXT("Auxiliary")},
                         {TEXT("Emergency Override"), TEXT("/Interaction/LightGroup/SetEmergency"), TEXT("Emergency")}},
                        FString()});
    auto AddLight = [&](const TCHAR* Group, const TCHAR* Json) {
        auto* L = NewObject<UPointLightComponent>(V);
        L->SetupAttachment(V->GetRootComponent());
        L->RegisterComponent();
        FSCImportedLightState S;
        S.Component = L;
        S.GroupEntityId = Group;
        S.Interior = 0;
        S.Json = ParseJson(Json);
        FSCLights::AuthorStates(S); // as the import does
        V->LightStates.Add(S);
        return L;
    };
    UPointLightComponent* L1 = AddLight(TEXT("lg"), TEXT(R"({"color":[1,1,1],"states":{"defaultState":{"color":[1,1,1],"intensity_candela_proxy":100},
        "auxiliaryState":{"color":[1,1,1],"intensity_candela_proxy":40},"emergencyState":{"color":[1,0.1,0.1],"intensity_candela_proxy":25}}})"));
    UPointLightComponent* L2 = AddLight(TEXT("lg"), TEXT(R"({"color":[1,1,1],"states":{"defaultState":{"color":[1,1,1],"intensity_candela_proxy":80},
        "emergencyState":{"color":[1,0,0],"intensity_candela_proxy":10}}})"));
    UPointLightComponent* Other = AddLight(TEXT("other"), TEXT(R"({"states":{"defaultState":{"intensity_candela_proxy":70},"emergencyState":{"intensity_candela_proxy":1}}})"));
    for (UPointLightComponent* L : {L1, L2, Other}) L->SetIntensity(50);
    TArray<FSCButtonFace> Faces = {F};
    V->BuildButtonPanels(Faces, [](const FString&) -> UTexture2D* { return nullptr; });
    if (!TestEqual(TEXT("one operable game panel"), V->FloorPanels.Num(), 1)) return false;
    const FSCFloorPanel& P = V->FloorPanels[0];
    TestTrue(TEXT("a Buttons panel with the render's nodes"), P.Kind == ESCPanelKind::Buttons && P.Layout.Options.Num() == 3);
    TestEqual(TEXT("its face"), P.Face, 0);
    TestEqual(TEXT("the group it drives (the chipset both link)"), V->LightGroupsLinkedBy(0, P.Links), TArray<int32>{0});
    TestEqual(TEXT("default render at rest"), P.ImageForState(), FString(TEXT("default.png")));

    // Aim: a ray at each node's centre picks that node; between nodes nothing; from behind nothing.
    for (int32 O = 0; O < 3; ++O)
    {
        FVector At;
        TestTrue(TEXT("world point"), V->FloorPanelWorldPoint(0, O, At));
        int32 Panel = INDEX_NONE, Option = INDEX_NONE;
        TestTrue(*FString::Printf(TEXT("aim at node %d"), O), V->AimFloorPanel(At + FVector(80, 10, 5), (At - (At + FVector(80, 10, 5))).GetSafeNormal(), 300, nullptr, Panel, Option));
        TestEqual(*FString::Printf(TEXT("node %d under the crosshair"), O), Option, O);
    }
    {
        const FVector Gap = Face->GetComponentTransform().TransformPosition(FVector(0, 0, 35 - 70 * (430.0 / 1280.0) - 2)); // between On and Auxiliary
        int32 Panel = INDEX_NONE, Option = INDEX_NONE;
        TestTrue(TEXT("on the face between nodes"), V->AimFloorPanel(Gap + FVector(80, 0, 0), FVector(-1, 0, 0), 300, nullptr, Panel, Option));
        TestEqual(TEXT("no node between nodes"), Option, INDEX_NONE);
        TestFalse(TEXT("nothing from behind"), V->AimFloorPanel(Gap - FVector(80, 0, 0), FVector(1, 0, 0), 300, nullptr, Panel, Option));
    }

    // E on Emergency: the group's Emergency on its lights (only those), the panel's switch and light states, its render.
    TestTrue(TEXT("Emergency pressed"), V->PressFloorPanel(0, 2) > 0);
    TestNearlyEqual(TEXT("light 1 emergency"), double(L1->Intensity), 25.0, 1e-3);
    TestTrue(TEXT("light 1 red"), L1->GetLightColor().Equals(FLinearColor(1, 0.1f, 0.1f), 1e-3f));
    TestNearlyEqual(TEXT("light 2 emergency"), double(L2->Intensity), 10.0, 1e-3);
    TestNearlyEqual(TEXT("a light of another group unchanged"), double(Other->Intensity), 50.0, 1e-3);
    TestEqual(TEXT("the group's state"), V->LightGroups[0].LightState, FString(TEXT("Emergency")));
    TestEqual(TEXT("switch state from the press"), V->FloorPanels[0].UiState.FindRef(TEXT("LightSwitchState")), FString(TEXT("LightSwitchAlarm")));
    TestEqual(TEXT("light state from the group"), V->FloorPanels[0].UiState.FindRef(TEXT("LightState")), FString(TEXT("LightsAlarm")));
    TestEqual(TEXT("the combined render"), V->FloorPanels[0].ImageForState(), FString(TEXT("emergency.png")));
    // Auxiliary: a light without that state keeps what it shows; the render pinning the switch alone.
    TestTrue(TEXT("Auxiliary pressed"), V->PressFloorPanel(0, 1) > 0);
    TestNearlyEqual(TEXT("light 1 auxiliary"), double(L1->Intensity), 40.0, 1e-3);
    TestNearlyEqual(TEXT("light 2 has no auxiliary state: unchanged"), double(L2->Intensity), 10.0, 1e-3);
    TestEqual(TEXT("render for LightSwitchLow + LightsLow"), V->FloorPanels[0].ImageForState(), FString(TEXT("low.png")));
    // On: back to the default render.
    TestTrue(TEXT("On pressed"), V->PressFloorPanel(0, 0) > 0);
    TestNearlyEqual(TEXT("light 2 default"), double(L2->Intensity), 80.0, 1e-3);
    TestEqual(TEXT("default render"), V->FloorPanels[0].ImageForState(), FString(TEXT("default.png")));
    // The group switched by another path (sc.Lights.Groups, another panel): the panel follows it.
    V->DeliverLightGroupTag(0, TEXT("/Interaction/LightGroup/SetEmergency"));
    TestEqual(TEXT("a panel follows its group however it changed"), V->FloorPanels[0].UiState.FindRef(TEXT("LightState")), FString(TEXT("LightsAlarm")));
    TestEqual(TEXT("its own switch state stays"), V->FloorPanels[0].UiState.FindRef(TEXT("LightSwitchState")), FString(TEXT("LightSwitchOn")));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLiftRiderTest, "SCBridge.Lift.RiderPassesPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLiftRiderTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    World->InitializeActorsForPlay(FURL());
    World->BeginPlay();
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* V = World->SpawnActor<ASCVehicle>(ASCVehicle::StaticClass(), FTransform::Identity, Params);
    auto Block = [&](AActor* Owner, USceneComponent* Parent, const FVector& Centre, const FVector& Extent) {
        auto* Box = NewObject<UBoxComponent>(Owner);
        Box->SetBoxExtent(Extent);
        Box->SetCollisionProfileName(TEXT("BlockAll"));
        Box->SetupAttachment(Parent);
        Box->SetRelativeLocation(Centre);
        Box->RegisterComponent();
        return Box;
    };
    // A lift of two stops on a vertical shaft at (0, 0): top z 0, bottom z -1000 (container frame, cm); speed 3.5 m/s.
    auto* Container = NewObject<USceneComponent>(V);
    Container->SetupAttachment(V->GetRootComponent());
    Container->RegisterComponent();
    TSharedPtr<FSCLift> Lift = MakeShared<FSCLift>();
    Lift->Id = 0;
    Lift->MaxSpeed = 3.5f;
    Lift->EasingDistance = 2.f;
    Lift->OpenWaitTime = 0.f;
    Lift->QueueWaitTime = 0.f;
    Lift->IdlingPeriod = 120.f;
    FSCLiftStop Bottom, Top;
    Bottom.Index = 0; Bottom.Local = FVector(0, 0, -1000); Bottom.S = 0; Bottom.ZCm = -1000;
    Top.Index = 1; Top.Local = FVector::ZeroVector; Top.S = 10; Top.ZCm = 0;
    Lift->Stops = {Bottom, Top};
    Lift->RestStop = Lift->CurrentStop = 1;
    Lift->S = 10;
    auto* Anchor = NewObject<USceneComponent>(V);
    Anchor->SetupAttachment(Container);
    Anchor->ComponentTags.Add(TEXT("SCLift"));
    Anchor->RegisterComponent();
    auto* Carriage = NewObject<USceneComponent>(V);
    Carriage->SetupAttachment(Anchor);
    Carriage->RegisterComponent();
    Block(V, Carriage, FVector(0, 0, -10), FVector(150, 150, 10)); // the platform: top at the stop's height
    Lift->Anchor = Anchor;
    Lift->AnchorBase = FVector::ZeroVector;
    Lift->CarriageAnchor = Carriage;
    Lift->Host.PlayDoor = [](USCRigComponent*, bool, float) { return false; };
    Lift->Host.IsDoorOpen = [](const USCRigComponent*) { return true; };
    Lift->Host.IsDoorMoving = [](const USCRigComponent*) { return false; };
    V->Lifts.Add(Lift);
    // A slab reaching half way into the shaft 3 m under the top stop (the C2 hold's shoulder ceiling), the ground the leg ends
    // on just under the bottom stop, a wall beside the shaft outside the path.
    AActor* Set = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
    auto* SetRoot = NewObject<USceneComponent>(Set);
    Set->SetRootComponent(SetRoot);
    SetRoot->RegisterComponent();
    UBoxComponent* Slab = Block(Set, SetRoot, FVector(150, 0, -300), FVector(150, 400, 30));
    UBoxComponent* Ground = Block(Set, SetRoot, FVector(0, 0, -1020 - 50), FVector(500, 500, 50));
    UBoxComponent* Wall = Block(Set, SetRoot, FVector(600, 0, -500), FVector(50, 400, 600));
    // The vehicle's own lowest part under the shaft (its exploration bounds reach down there: the fall recovery is below it).
    Block(V, V->GetRootComponent(), FVector(0, 0, -1400), FVector(300, 300, 50));

    // The rider: standing on the platform over the slab's edge (its axis 90 cm toward the slab, its edge over it).
    auto* Character = World->SpawnActor<ASCFirstPersonCharacter>(ASCFirstPersonCharacter::StaticClass(), FTransform(FVector(90, 0, 200)), Params);
    const double Half = Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    V->AddSpawnPoint(FSCSpawnKind::Interior, TEXT("carriage"), FTransform(FVector(90, 0, 0)));
    V->bCollisionReady = true;
    TestTrue(TEXT("explores onto the carriage"), Character->Explore(V));
    for (int32 I = 0; I < 30; ++I) { Character->Tick(1.f / 60); Character->GetCharacterMovement()->TickComponent(1.f / 60, LEVELTICK_All, nullptr); }
    const double Start = Character->GetActorLocation().Z - Half;
    TestNearlyEqual(TEXT("standing on the platform"), Start, 0.0, 3.0);

    // At rest nothing rides; on the leg the capsule over the carriage rides it, and the leg's obstacles are the slab only.
    TestEqual(TEXT("not moving: no rider"), V->LiftRiddenBy(Character->GetActorLocation() - FVector(0, 0, Half)), INDEX_NONE);
    Lift->RequestStop(0, TEXT("test"), NAME_None);
    for (int32 I = 0; I < 600 && Lift->State != ESCLiftState::Moving; ++I) Lift->Tick(1.0 / 60);
    if (!TestEqual(TEXT("the leg started"), Lift->State, ESCLiftState::Moving)) return false;
    TestEqual(TEXT("the capsule on the moving carriage rides it"),
              V->LiftRiddenBy(Character->GetActorLocation() - FVector(0, 0, Half)), 0);
    TestEqual(TEXT("not beside the carriage"), V->LiftRiddenBy(FVector(400, 0, -10)), INDEX_NONE);
    const TArray<TWeakObjectPtr<UPrimitiveComponent>>& Passed = V->LiftPathObstacles(0);
    TestTrue(TEXT("the slab is passed through"), Passed.Contains(Slab));
    TestFalse(TEXT("the ground the leg ends on is not"), Passed.Contains(Ground));
    TestFalse(TEXT("a wall outside the path is not"), Passed.Contains(Wall));
    TestFalse(TEXT("the carriage's own platform is not"), Passed.ContainsByPredicate([&](const TWeakObjectPtr<UPrimitiveComponent>& C) { return C.Get() && V->LiftCarrying(C.Get()) == 0; }));

    // The ride, frame by frame as in the game loop (the vehicle, then the character, then its movement), from the stop it stands
    // at to To; returns the spread of the feet's height over the carriage (min, max).
    IConsoleVariable* Carry = IConsoleManager::Get().FindConsoleVariable(TEXT("sc.Lift.CarryRiders"));
    if (!TestNotNull(TEXT("sc.Lift.CarryRiders"), Carry)) return false;
    ON_SCOPE_EXIT { Carry->Set(1); };
    auto Ride = [&](int32 To, double& Min, double& Max) {
        Min = TNumericLimits<double>::Max();
        Max = -Min;
        if (Lift->State != ESCLiftState::Moving) Lift->RequestStop(To, TEXT("test"), NAME_None);
        for (int32 I = 0; I < 2400 && !(Lift->CurrentStop == To && Lift->State != ESCLiftState::Moving); ++I)
        {
            V->Tick(1.f / 60);
            Character->Tick(1.f / 60);
            Character->GetCharacterMovement()->TickComponent(1.f / 60, LEVELTICK_All, nullptr);
            if (Lift->State != ESCLiftState::Moving) continue;
            const double Rel = Character->GetActorLocation().Z - Half - Anchor->GetComponentLocation().Z;
            Min = FMath::Min(Min, Rel);
            Max = FMath::Max(Max, Rel);
        }
    };
    double Min, Max;
    // Without the rule (failing first): the capsule's edge lands on the slab and stays there while the carriage goes on.
    Carry->Set(0);
    Ride(0, Min, Max);
    TestEqual(TEXT("off: arrived at the bottom stop"), Lift->CurrentStop, 0);
    TestTrue(*FString::Printf(TEXT("off: the rider is caught on the slab (feet over the carriage %.2f .. %.2f cm)"), Min, Max), Max - Min > 100.0);
    TestTrue(TEXT("off: left on the slab"), Character->GetActorLocation().Z - Half > -400.0);
    // With the rule: back at the top, on the carriage again, down past the slab and up again.
    Carry->Set(1);
    Ride(1, Min, Max);
    TestTrue(TEXT("on the carriage at the top again"), Character->Explore(V));
    for (int32 I = 0; I < 30; ++I) { Character->Tick(1.f / 60); Character->GetCharacterMovement()->TickComponent(1.f / 60, LEVELTICK_All, nullptr); }
    Ride(0, Min, Max);
    TestEqual(TEXT("arrived at the bottom stop"), Lift->CurrentStop, 0);
    TestTrue(*FString::Printf(TEXT("down: rider kept on the carriage (feet over the carriage %.2f .. %.2f cm)"), Min, Max), Max - Min < 4.0 && Min > -3.0);
    TestNearlyEqual(TEXT("at the bottom with the carriage"), Character->GetActorLocation().Z - Half, -1000.0, 4.0);
    Ride(1, Min, Max);
    TestTrue(*FString::Printf(TEXT("up: rider kept on the carriage (feet over the carriage %.2f .. %.2f cm)"), Min, Max), Max - Min < 4.0 && Min > -3.0);
    TestNearlyEqual(TEXT("at the top with the carriage"), Character->GetActorLocation().Z - Half, 0.0, 4.0);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCReaderC2FixesTest, "SCBridge.Reader.C2FixesFields", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCReaderC2FixesTest::RunTest(const FString&)
{
    class FSource : public FSCPackageSource
    {
    public:
        FString Text;
        bool Exists(const FString& Path) const override { return Path == TEXT("scene.json"); }
        bool ReadBytes(const FString& Path, TArray<uint8>& Out, FString& Error) const override
        {
            FTCHARToUTF8 Utf8(*Text);
            Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
            return true;
        }
        TArray<FString> ListFiles(const FString&) const override { return {TEXT("scene.json")}; }
    } Source;
    Source.Text = TEXT(R"({"version":2,"export_kind":"Decomposed","root_entity":{"entity_name":"Ship"},
      "children":[{"entity_name":"Hatch","source_transform_basis":"gltf_y_up","local_transform_sc":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
        "door":{"animation_params":"SCItemDoorSingleProceduralParams","animation_speed":1.5,"is_opened":false,"is_locked":true,"transition_tag":"Open","open_tag":null,"close_tag":null},
        "item_port_links":[{"source_slot":"Open","target_port":"hardpoint_front_ramp","target_slot":"Open"}]}],
      "interiors":[{"name":"hold","container_transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
        "placements":[{"cgf_path":"screen.cgf","entity_id":"p","transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],"links":["lg"],
          "sendable_interactions":[{"name":"LightsAlarm","linking_tag":"/Interaction/LightGroup/SetEmergency"}],
          "ui_bindings":[{"binding_kind":"physical","generated_image_path":"Data/UI/a.png","image_size_px":[720,1280],"interaction_distance":15.0,
            "buttons":[{"node":"EmergencyLightsButton","interaction":"LightsAlarm","rect_px":[89,850,541,313]}],
            "state_types":[{"name":"LightSwitchState","default_state":"LightSwitchOn","states":["LightSwitchOn","LightSwitchAlarm"],
              "transitions":[{"interaction":"LightsAlarm","state":"LightSwitchAlarm"},{"from":"LightSwitchAlarm","interaction":"Toggle","state":"LightSwitchOn"},{"from":"LightSwitchOn","interaction":"Toggle","state":"LightSwitchAlarm"}],"linked":[{"tag":"/State/LightSwitch/Emergency","state":"LightSwitchAlarm"}]}],
            "state_images":[{"state_type":"LightState","state":"LightsAlarm","generated_image_path":"Data/UI/b.png"},
              {"states":[{"state_type":"LightSwitchState","state":"LightSwitchAlarm"},{"state_type":"LightState","state":"LightsAlarm"}],"generated_image_path":"Data/UI/c.png"}]}]}],
        "entities":[{"entity_id":"lg","entity_name":"LG_Hold","entity_class":"LightGroup","kind":"light_group","transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
          "links":["chip"],"light_group":{"interactions":[{"name":"Emergency Override","linking_tag":"/Interaction/LightGroup/SetEmergency","light_state":"Emergency"}]}}]}]})");
    FSCSceneManifest M;
    FString Error;
    if (!TestTrue(TEXT("reads: ") + Error, FSCPackageReader::Read(Source, M, Error))) return false;
    if (!TestEqual(TEXT("child"), M.Children.Num(), 1) || !TestEqual(TEXT("interior"), M.Interiors.Num(), 1)) return false;
    const FSCDoorTiming& D = M.Children[0].Door;
    TestTrue(TEXT("child door"), D.bValid && D.bIsOpened.IsSet() && !D.bIsOpened.GetValue() && D.TransitionTag == FName(TEXT("Open")) &&
                                     D.OpenTag.IsNone() && FMath::IsNearlyEqual(D.AnimationSpeed, 1.5f) && D.AnimationParams == TEXT("SCItemDoorSingleProceduralParams"));
    TestTrue(TEXT("authored lock state"), D.bIsLocked.Get(false));
    TestTrue(TEXT("item port link"), M.Children[0].ItemPortLinks.Num() == 1 && M.Children[0].ItemPortLinks[0].TargetPort == TEXT("hardpoint_front_ramp"));
    const FSCSceneInstance& P = M.Interiors[0].Placements[0];
    TestTrue(TEXT("sendable"), P.SendableInteractions.Num() == 1 && P.SendableInteractions[0].LinkingTag == TEXT("/Interaction/LightGroup/SetEmergency"));
    if (TestEqual(TEXT("binding"), P.UiBindings.Num(), 1))
    {
        const FSCUiBinding& B = P.UiBindings[0];
        TestTrue(TEXT("button"), B.Buttons.Num() == 1 && B.Buttons[0].Interaction == FName(TEXT("LightsAlarm")) &&
                                     B.Buttons[0].RectPx.Max.Equals(FVector2D(630, 1163)) && B.ImageSizePx.Equals(FVector2D(720, 1280)));
        TestTrue(TEXT("state type"), B.StateTypes.Num() == 1 && B.StateTypes[0].Next(TEXT("LightsAlarm"), TEXT("LightSwitchOn")) && *B.StateTypes[0].Next(TEXT("LightsAlarm"), TEXT("LightSwitchOn")) == TEXT("LightSwitchAlarm") &&
                                         B.StateTypes[0].Linked.Num() == 1);
        TestTrue(TEXT("a transition from the current state"), B.StateTypes[0].Next(TEXT("Toggle"), TEXT("LightSwitchOn")) &&
                                                               *B.StateTypes[0].Next(TEXT("Toggle"), TEXT("LightSwitchOn")) == TEXT("LightSwitchAlarm") &&
                                                               *B.StateTypes[0].Next(TEXT("Toggle"), TEXT("LightSwitchAlarm")) == TEXT("LightSwitchOn"));
        TestEqual(TEXT("single and combined images"), B.StateImages.Num(), 2);
        TestEqual(TEXT("cache format 40: the screen's operating distance (m)"), B.InteractionDistance, 15.f);
        if (B.StateImages.Num() == 2)
        {
            TestEqual(TEXT("single key"), B.StateImages[0].Key(), FString(TEXT("LightState.LightsAlarm")));
            TestEqual(TEXT("combined key, sorted"), B.StateImages[1].Key(), FString(TEXT("LightState.LightsAlarm+LightSwitchState.LightSwitchAlarm")));
        }
    }
    const FSCInteriorEntity* G = M.Interiors[0].Entities.FindByPredicate([](const FSCInteriorEntity& E) { return E.Kind == TEXT("light_group"); });
    TestTrue(TEXT("light group"), G && G->LightGroupInteractions.Num() == 1 && G->LightGroupInteractions[0].LightState == TEXT("Emergency") && G->Links.Num() == 1);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCExteriorPanelReachTest, "SCBridge.Doors.ExteriorPanelReach", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCExteriorPanelReachTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* V = World->SpawnActor<ASCVehicle>();
    V->ClipCatalog = NewObject<USCClipCatalog>(V);
    // A closed open/close door (its joint at the vehicle origin) and its exterior pad 4 m up on it: the pad's anchor rides
    // the door's rig (a loadout child on the door's IP_Exterior), its collidable face a small box.
    USCRigComponent* Rig = AddRig(V, TEXT("Hatch"));
    AddClip(V, Rig, TEXT("hatch_open"), TEXT("Open"), {0, 25, 50});
    AddClip(V, Rig, TEXT("hatch_close"), TEXT("Close"), {50, 25, 0});
    FSCDoorRecord R;
    R.Door.bValid = true;
    R.Door.OpenTag = TEXT("Open");
    R.Door.CloseTag = TEXT("Close");
    R.Door.bIsOpened = false;
    V->DoorRecords.Add(Rig, R);
    auto* Pad = NewObject<USceneComponent>(V);
    V->AddInstanceComponent(Pad);
    Pad->SetupAttachment(Rig);
    Pad->SetRelativeLocation(FVector(0, 0, 400));
    Pad->RegisterComponent();
    auto AddBox = [&](USceneComponent* Parent, const FVector& At, const FVector& Extent) {
        auto* Box = NewObject<UBoxComponent>(V);
        V->AddInstanceComponent(Box);
        Box->SetBoxExtent(Extent);
        Box->SetCollisionProfileName(TEXT("BlockAll"));
        Box->SetupAttachment(Parent);
        Box->SetRelativeLocation(At);
        Box->RegisterComponent();
        return Box;
    };
    AddBox(Pad, FVector::ZeroVector, FVector(2, 10, 10));
    // A part of the door that is not a panel, at the same height beside it.
    AddBox(Rig, FVector(0, 300, 400), FVector(2, 10, 10));
    V->DoorControls.Add(Pad, Rig);
    V->BuildExploration();
    const int32 Door = V->DoorForRig(Rig);
    if (!TestTrue(TEXT("the door"), Door != INDEX_NONE)) return false;
    const FVector Eye(-360, 0, 400), Along = FVector::ForwardVector;
    FSCInteractionPick Pick;
    TestEqual(TEXT("without its operating distance the pad is out of the 3 m reach"), V->FindInteraction(Eye, Along, 300, nullptr, &Pick), int32(INDEX_NONE));
    V->SetPanelReach(Pad, 15.f); // its UI's interactionDistance (m)
    TestEqual(TEXT("a pad is operated from as far as its UI allows"), V->FindInteraction(Eye, Along, 300, nullptr, &Pick), Door);
    TestTrue(TEXT("found as the panel, its own reach"), Pick.Via == TEXT("panel") && FMath::IsNearlyEqual(Pick.Reach, 1500.0) && Pick.Distance > 300);
    TestEqual(TEXT("from inside the 3 m reach as before"), V->FindInteraction(FVector(-100, 0, 400), Along, 300), Door);
    TestEqual(TEXT("not beyond its operating distance"), V->FindInteraction(FVector(-1600, 0, 400), Along, 300), int32(INDEX_NONE));
    TestEqual(TEXT("a part that is not a panel keeps the 3 m reach"), V->FindInteraction(FVector(-360, 300, 400), Along, 300), int32(INDEX_NONE));
    AActor* Wall = World->SpawnActor<AActor>();
    auto* WallBox = NewObject<UBoxComponent>(Wall);
    Wall->SetRootComponent(WallBox);
    WallBox->SetBoxExtent(FVector(5, 50, 50));
    WallBox->SetCollisionProfileName(TEXT("BlockAll"));
    WallBox->SetWorldLocation(FVector(-200, 0, 400));
    WallBox->RegisterComponent();
    TestEqual(TEXT("not through an occluder"), V->FindInteraction(Eye, Along, 300), int32(INDEX_NONE));
    Wall->Destroy();
    // E on the exterior pad plays the door's own press (its clip from the current pose, its state), like the interior pad.
    TestTrue(TEXT("the press opens the door"), V->ToggleDoor(V->FindInteraction(Eye, Along, 300)) && V->IsDoorOpen(Door) && Playing(V, Rig));
    V->Tick(1.f);
    TestNearlyEqual(TEXT("open at its open fragment's end"), BoneX(Rig), 50.0, 1e-3);
    TestTrue(TEXT("and closes again"), V->ToggleDoor(V->FindInteraction(Eye, Along, 300)) && !V->IsDoorOpen(Door));
    V->Tick(1.f);
    TestTrue(TEXT("lock applies without moving the door"), V->SetDoorLocked(Door, true) && !V->IsDoorPlaying(Door));
    TestFalse(TEXT("E on the locked pad refuses an unpaired actor"), V->ToggleDoor(V->FindInteraction(Eye, Along, 300)));
    TestEqual(TEXT("refused order exposes the reason"), V->OrderShipDoor(Door, ESCShipDoorAction::Open, TEXT("visitor"), Eye), FString(TEXT("Locked")));
    FSCDoorSensor& Sensor = V->DoorSensors.AddDefaulted_GetRef();
    Sensor.Anchor = Pad;
    Sensor.Box = FBox(FVector(-1000), FVector(1000));
    Sensor.Doors.Add(Rig);
    V->UpdateDoorSensors(Eye, 1.0);
    TestTrue(TEXT("a sensor cannot open the locked door"), !V->IsDoorOpen(Door) && !V->IsDoorPlaying(Door));
    TestTrue(TEXT("unlock applies without moving the door"), V->SetDoorLocked(Door, false) && !V->IsDoorPlaying(Door));
    V->UpdateDoorSensors(Eye, 2.0);
    TestTrue(TEXT("sensor can open the unlocked door"), V->IsDoorOpen(Door));
    return !HasAnyErrors();
}

// SCBridge.Doors.MotionEvents (WP-A1 door effects): a door's moves and their ends as ASCVehicle::OnDoorMotion reports them:
// an order moves it and it arrives when its playback ends; turned around mid-stroke the new move replaces the old (no arrival
// for the first); ResetPose, a snap, a raw clip, a fragment command and its rig destroyed stop a move (door effects come
// from door orders); two doors move and arrive on their own; a move stopped short otherwise leaves its state unknown; a lock
// change is reported.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCDoorMotionEventsTest, "SCBridge.Doors.MotionEvents", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCDoorMotionEventsTest::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* V = World->SpawnActor<ASCVehicle>();
    V->ClipCatalog = NewObject<USCClipCatalog>(V);
    USCRigComponent* A = AddRig(V, TEXT("Hatch"));
    AddClip(V, A, TEXT("hatch_open"), TEXT("Open"), {0, 25, 50});
    AddClip(V, A, TEXT("hatch_close"), TEXT("Close"), {50, 25, 0});
    FSCDoorRecord R;
    R.Door.bValid = true;
    R.Door.OpenTag = TEXT("Open");
    R.Door.CloseTag = TEXT("Close");
    R.Door.bIsOpened = false;
    V->DoorRecords.Add(A, R);
    USCRigComponent* B = AddRig(V, TEXT("Leaf"));
    AddClip(V, B, TEXT("leaf_open"), TEXT("Open"), {0, 30});
    AddClip(V, B, TEXT("leaf_close"), TEXT("Close"), {30, 0});
    V->DoorRecords.Add(B, R);
    // A bed: one clip, played forwards to close and backwards to open (SCItem_Bed_Anims_Reverse_Close): its first frame is open.
    USCRigComponent* Bed = AddRig(V, TEXT("Bed"));
    AddClip(V, Bed, TEXT("bed_close"), TEXT("Close"), {50, 0});
    {
        FSCImportedClip& BedClip = V->ClipCatalog->Clips.Last();
        FSCClipFragment Open = BedClip.Fragments[0];
        Open.FragmentTags = {TEXT("Open")};
        Open.Speed = -1;
        BedClip.Fragments.Add(Open);
        // Before it, an alias entry (metadata naming another clip, played forwards): not what plays this clip.
        FSCClipFragment Alias = Open;
        Alias.Speed = 1;
        Alias.AnimationNames = {TEXT("hatch_open")};
        Alias.OptionId = TEXT("hatch_open");
        BedClip.Fragments.Insert(Alias, 0);
    }
    V->DoorRecords.Add(Bed, R);
    V->BuildExploration();
    const int32 DA = V->DoorForRig(A), DB = V->DoorForRig(B), DBed = V->DoorForRig(Bed);
    if (!TestTrue(TEXT("two doors"), DA != INDEX_NONE && DB != INDEX_NONE)) return false;
    TArray<FString> Events;
    const FDelegateHandle Handle = ASCVehicle::OnDoorMotion.AddLambda([&](ASCVehicle* Vehicle, int32 Door, bool bOpen, ASCVehicle::EDoorMotion E) {
        if (Vehicle != V) return;
        static const TCHAR* Names[] = {TEXT("moved"), TEXT("arrived"), TEXT("cancelled"), TEXT("locked"), TEXT("unlocked")};
        Events.Add(FString::Printf(TEXT("%s:%d:%s"), Door == DA ? TEXT("A") : TEXT("B"), bOpen, Names[uint8(E)]));
    });
    ON_SCOPE_EXIT { ASCVehicle::OnDoorMotion.Remove(Handle); };
    auto Expect = [&](const TCHAR* What, std::initializer_list<const TCHAR*> Want) {
        TArray<FString> W;
        for (const TCHAR* S : Want) W.Add(S);
        TestEqual(What, FString::Join(Events, TEXT(" ")), FString::Join(W, TEXT(" ")));
        Events.Reset();
    };
    Events.Reset(); // the rest poses do not move a door

    // An order: it moves, and arrives when its playback ends.
    TestTrue(TEXT("A opens"), V->PlayDoor(DA, true));
    Expect(TEXT("moved"), {TEXT("A:1:moved")});
    V->Tick(0.1f);
    Expect(TEXT("still moving"), {});
    V->Tick(1.f);
    Expect(TEXT("arrived open"), {TEXT("A:1:arrived")});

    // Turned around mid-stroke: the new move replaces the old; only the second arrives.
    TestTrue(TEXT("A closes"), V->PlayDoor(DA, false));
    V->Tick(0.1f);
    TestTrue(TEXT("A turned around"), V->PlayDoor(DA, true));
    V->Tick(1.f);
    Expect(TEXT("two moves, one arrival"), {TEXT("A:0:moved"), TEXT("A:1:moved"), TEXT("A:1:arrived")});

    // The same order again mid-stroke (a lift re-ordering a closing door): one move, one arrival.
    TestTrue(TEXT("A closes"), V->PlayDoor(DA, false));
    V->Tick(0.05f);
    V->PlayDoor(DA, false);
    V->Tick(1.f);
    Expect(TEXT("a repeated order is one move"), {TEXT("A:0:moved"), TEXT("A:0:arrived")});
    TestTrue(TEXT("A opens"), V->PlayDoor(DA, true));
    V->Tick(1.f);
    Events.Reset();

    // Two doors (here two rigs) move and arrive on their own.
    TestTrue(TEXT("A closes again"), V->PlayDoor(DA, false));
    TestTrue(TEXT("B opens"), V->PlayDoor(DB, true));
    V->Tick(0.15f); // B's 1-frame clip ends; A's 2-frame clip does not
    V->Tick(1.f);
    Expect(TEXT("each arrives"), {TEXT("A:0:moved"), TEXT("B:1:moved"), TEXT("B:1:arrived"), TEXT("A:0:arrived")});

    // Ordered open and turned back before it moved: the close starts at its end (the pose it shows), so no closing stroke
    // (no close / closed effects): the open move is cancelled.
    TestTrue(TEXT("A opens"), V->PlayDoor(DA, true));
    TestTrue(TEXT("and at once closes"), V->PlayDoor(DA, false));
    V->Tick(1.f);
    Expect(TEXT("a reversal with no travel"), {TEXT("A:1:moved"), TEXT("A:0:cancelled")});
    TestTrue(TEXT("A closed"), V->IsDoorStateKnown(DA) && !V->IsDoorOpen(DA) && FMath::IsNearlyEqual(BoneX(A), 0.0, 1e-3));
    // Ordered closed again at rest closed: nothing moves and its rest (held closed loops) goes on: no event.
    TestTrue(TEXT("A closes again"), V->PlayDoor(DA, false));
    V->Tick(1.f);
    Expect(TEXT("an order at rest at its end"), {});

    // ResetPose cancels a move (and every door's rest: each door reports a cancel), with no arrival.
    TestTrue(TEXT("A opens"), V->PlayDoor(DA, true));
    V->ResetPose();
    V->Tick(1.f);
    TestTrue(TEXT("reset cancels A's move"), Events.Num() >= 2 && Events[0] == TEXT("A:1:moved") && Events[1] == TEXT("A:1:cancelled"));
    TestFalse(TEXT("no arrival after a reset"), Events.ContainsByPredicate([](const FString& E) { return E.EndsWith(TEXT("arrived")); }));
    TestTrue(TEXT("B's rest ends too"), Events.Contains(TEXT("B:0:cancelled")));
    Events.Reset();

    // Door effects come from door orders: a fragment command taking a moving door over stops its move (no arrival).
    TestTrue(TEXT("A opens (from the rest the reset left)"), V->PlayDoor(DA, true));
    TestTrue(TEXT("fragment"), V->PlayFragmentAt(TEXT("SCItemDoor"), TEXT("Open"), A));
    V->Tick(1.f);
    Expect(TEXT("a fragment stops the move"), {TEXT("A:1:moved"), TEXT("A:1:cancelled")});

    // So does a raw clip on its channels, and a snap.
    const int32 Open = V->ClipCatalog->Clips.IndexOfByPredicate([](const FSCImportedClip& C) { return C.Id == FName(TEXT("hatch_open")); });
    const int32 Close = V->ClipCatalog->Clips.IndexOfByPredicate([](const FSCImportedClip& C) { return C.Id == FName(TEXT("hatch_close")); });
    TestTrue(TEXT("A closes"), V->PlayDoor(DA, false));
    TestTrue(TEXT("raw open clip"), V->PlayClipAt(Open, A, 1.f));
    V->Tick(1.f);
    Expect(TEXT("a raw clip stops the move"), {TEXT("A:0:moved"), TEXT("A:0:cancelled")});
    TestTrue(TEXT("A closes"), V->PlayDoor(DA, false));
    V->Tick(0.05f);
    TestTrue(TEXT("snapped"), V->SnapClipAt(Close, A, true));
    V->Tick(1.f);
    Expect(TEXT("a snap stops the move"), {TEXT("A:0:moved"), TEXT("A:0:cancelled")});

    // A door on another rig is not touched by them.
    TestTrue(TEXT("B moves"), V->PlayDoor(DB, !V->IsDoorOpen(DB)));
    TestTrue(TEXT("A's raw clip"), V->PlayClipAt(Open, A, 1.f));
    V->Tick(1.f);
    TestTrue(TEXT("B arrives undisturbed"), Events.Num() >= 2 && Events[0].EndsWith(TEXT("moved")) && Events.Last().StartsWith(TEXT("B:")) &&
                                             Events.Last().EndsWith(TEXT("arrived")));
    Events.Reset();

    // Its rig destroyed while moving: cancelled, no arrival.
    TestTrue(TEXT("B moves"), V->PlayDoor(DB, !V->IsDoorOpen(DB)));
    const bool bBOpen = V->IsDoorOpen(DB);
    B->DestroyComponent();
    V->Tick(1.f);
    Expect(TEXT("rig gone cancels"), {bBOpen ? TEXT("B:1:moved") : TEXT("B:0:moved"), bBOpen ? TEXT("B:1:cancelled") : TEXT("B:0:cancelled")});

    // Its playback stopped short by something that does not drive its channels (a Mannequin scope preempting it): cancelled,
    // and the door shows neither end: unsettled (sc.Doors.Play does not skip it), its target kept (the next order, a Ship
    // Link close, goes the other way and settles it).
    TestTrue(TEXT("A moves"), V->PlayDoor(DA, !V->IsDoorOpen(DA)));
    const bool bAOpen = V->IsDoorOpen(DA);
    V->Tick(0.05f);
    V->Playback.RemoveAll([&](const FSCClipPlayback& P) { return P.TargetRig.Get() == A; });
    V->Tick(0.05f);
    Expect(TEXT("preempted cancels"), {bAOpen ? TEXT("A:1:moved") : TEXT("A:0:moved"), bAOpen ? TEXT("A:1:cancelled") : TEXT("A:0:cancelled")});
    TestFalse(TEXT("its state unsettled"), V->IsDoorStateKnown(DA));
    TestEqual(TEXT("its target kept"), V->IsDoorOpen(DA), bAOpen);
    TestTrue(TEXT("the next order goes the other way"), V->PlayDoor(DA, !bAOpen));
    V->Tick(1.f);
    TestTrue(TEXT("and settles it at that end"), V->IsDoorStateKnown(DA) && V->IsDoorOpen(DA) == !bAOpen &&
                                                    FMath::IsNearlyEqual(BoneX(A), bAOpen ? 0.0 : 50.0, 1e-3));
    Events.Reset();

    // A raw clip on the door's channels records its end; a fragment on other bones of the rig preempting it through a shared
    // scope freezes it short of that end: unsettled too.
    AddClip(V, A, TEXT("hatch_wave"), TEXT("Wave"), {0, 5});
    {
        FSCImportedClip& Wave = V->ClipCatalog->Clips.Last();
        Wave.Bindings[0].Tracks[0].Bone = 0;
        Wave.Fragments[0].Fragment = TEXT("Wave");
    }
    TestTrue(TEXT("raw clip"), V->PlayClipAt(V->IsDoorOpen(DA) ? Close : Open, A, 1.f));
    TestTrue(TEXT("its end recorded"), V->IsDoorStateKnown(DA));
    V->Tick(0.05f);
    TestTrue(TEXT("a fragment on other bones"), V->PlayFragmentAt(TEXT("Wave"), TEXT("Wave"), A));
    TestFalse(TEXT("the frozen door is unsettled"), V->IsDoorStateKnown(DA));
    V->Tick(1.f);
    // A clip with no travel left (the close of an open-then-close before a tick: it starts at its end) preempted: the door
    // stays settled closed.
    TestTrue(TEXT("A closes"), V->PlayDoor(DA, false));
    V->Tick(1.f);
    TestTrue(TEXT("A opens"), V->PlayDoor(DA, true));
    TestTrue(TEXT("and at once closes"), V->PlayDoor(DA, false));
    TestTrue(TEXT("the other fragment"), V->PlayFragmentAt(TEXT("Wave"), TEXT("Wave"), A));
    TestTrue(TEXT("still settled closed"), V->IsDoorStateKnown(DA) && !V->IsDoorOpen(DA));
    V->Tick(1.f);
    Events.Reset();

    // A raw clip's recorded state follows the end its fragment plays it to: the bed's clip held at speed zero shows its
    // first frame, open; played forwards it ends closed.
    const int32 BedIndex = V->ClipCatalog->Clips.IndexOfByPredicate([](const FSCImportedClip& C) { return C.Id == FName(TEXT("bed_close")); });
    TestTrue(TEXT("bed held at its first frame"), V->PlayClipAt(BedIndex, Bed, 0.f));
    TestTrue(TEXT("recorded open"), V->IsDoorStateKnown(DBed) && V->IsDoorOpen(DBed));
    TestTrue(TEXT("bed played forwards"), V->PlayClipAt(BedIndex, Bed, 1.f));
    V->Tick(1.f);
    TestTrue(TEXT("recorded closed"), V->IsDoorStateKnown(DBed) && !V->IsDoorOpen(DBed));
    Events.Reset();

    // A lock change is reported.
    TestTrue(TEXT("locks"), V->SetDoorLocked(DA, true));
    Expect(TEXT("locked"), {TEXT("A:0:locked")});
    return !HasAnyErrors();
}

#endif
