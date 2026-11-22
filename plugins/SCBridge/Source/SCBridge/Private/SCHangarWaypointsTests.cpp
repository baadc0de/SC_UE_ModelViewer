// Hangar waypoints (synthetic records and geometry; no game data):
// - SCBridge.Hangar.WaypointRule: which environment records are waypoints (transit gateways, operated panels except the
//   transit panel a gateway in their container stands for, interior inventory containers), in that order; labels cleaned and
//   numbered; BuildWaypoints anchors them in the actor's frame and a cooked environment (no sources) keeps its own.
// - SCBridge.Hangar.TeleportMenu: the menu's sections (ship spawns, then hangar places, each under its heading) and the
//   selection across them (keys skip headings and wrap); a hangar place is listed only where a standing capsule fits,
//   Enter teleports there, W / S move the selection instead of walking while the menu is open, and a movement key pressed
//   in the menu is not held after it.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "SCHangarWaypoints.h"
#include "SCPackageReader.h"
#include "SCVehicle.h"
#include "SCFirstPersonCharacter.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"

namespace
{
FSCUiBinding Binding(const TCHAR* ItemType, bool bOperated)
{
    FSCUiBinding B;
    B.Kind = TEXT("physical");
    B.bClassified = B.bOwnerResolved = true;
    B.SourceItemType = ItemType;
    B.bOperated = bOperated;
    return B;
}
FSCSceneInstance Placement(const TCHAR* Name, TArray<FSCUiBinding> Bindings = {})
{
    FSCSceneInstance P;
    P.EntityName = TEXT("Objects/Synthetic/shared_mesh.cgf"); // the reader keeps a placement's mesh path here
    P.SocEntityName = Name;
    P.UiBindings = MoveTemp(Bindings);
    return P;
}
FSCInteriorEntity Entity(const TCHAR* Name, const TCHAR* Kind)
{
    FSCInteriorEntity E;
    E.EntityName = Name;
    E.Kind = Kind;
    return E;
}
UBoxComponent* AddBlock(AActor* Owner, USceneComponent* Parent, const FVector& Centre, const FVector& Extent)
{
    auto* Box = NewObject<UBoxComponent>(Owner);
    Box->SetBoxExtent(Extent);
    Box->SetCollisionProfileName(TEXT("BlockAll"));
    Box->SetupAttachment(Parent);
    Box->SetRelativeLocation(Centre);
    Box->RegisterComponent();
    return Box;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCHangarWaypointRule, "SCBridge.Hangar.WaypointRule", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCHangarWaypointRule::RunTest(const FString&)
{
    FSCSceneManifest M;
    // 0: the hangar room: a kiosk the player operates (a display with a kiosk interaction), a screen only shown, a prop
    //    without a UI, a door-control panel, and a light group.
    FSCInterior& Room = M.Interiors.AddDefaulted_GetRef();
    Room.Placements = {Placement(TEXT("Synthetic_Kiosk-012"), {Binding(TEXT("Display"), true)}), Placement(TEXT("Synthetic_Sign"), {Binding(TEXT("Display"), false)}),
                       Placement(TEXT("Synthetic_Cabinet")), Placement(TEXT("Synthetic_Kiosk-013"), {Binding(TEXT("Display"), true)}),
                       Placement(TEXT("Synthetic_Pad"), {Binding(TEXT("ControlPanel"), true)})};
    Room.Entities = {Entity(TEXT("Synthetic_LightGroup"), TEXT("light_group"))};
    // 1, 2: two personnel-elevator containers, each a transit gateway with its limbo node and its call panel.
    for (int32 I = 0; I < 2; ++I)
    {
        FSCInterior& Lift = M.Interiors.AddDefaulted_GetRef();
        Lift.Entities = {Entity(TEXT("Synthetic_Limbo"), TEXT("transit_limbo")), Entity(TEXT("Synthetic_Gateway"), TEXT("transit_gateway"))};
        Lift.Placements = {Placement(TEXT("Synthetic_CallPanel"), {Binding(TEXT(""), true)})};
    }
    // 3: a drop-off lift without a gateway: its transit panel (no item type) is the place.
    M.Interiors.AddDefaulted_GetRef().Placements = {Placement(TEXT("Synthetic_DropOffPanel-001"), {Binding(TEXT(""), true)})};
    // 4: a gateway container that also holds a kiosk: the gateway stands for its call panel only.
    FSCInterior& Mixed = M.Interiors.AddDefaulted_GetRef();
    Mixed.Entities = {Entity(TEXT("Synthetic_Gateway"), TEXT("transit_gateway"))};
    Mixed.Placements = {Placement(TEXT("Synthetic_CallPanel"), {Binding(TEXT(""), true)}), Placement(TEXT("Synthetic_FreightKiosk"), {Binding(TEXT("Synthetic"), true)})};
    // 5: two gateways and two transit panels: which serves which is not in the data, every panel stays.
    FSCInterior& Pair = M.Interiors.AddDefaulted_GetRef();
    Pair.Entities = {Entity(TEXT("Synthetic_GatewayA"), TEXT("transit_gateway")), Entity(TEXT("Synthetic_GatewayB"), TEXT("transit_gateway"))};
    Pair.Placements = {Placement(TEXT("Synthetic_PanelA"), {Binding(TEXT(""), true)}), Placement(TEXT("Synthetic_PanelB"), {Binding(TEXT(""), true)})};
    // An interior inventory container and a vehicle one (no interior: not a hangar place).
    FSCInventoryContainerRecord Bank;
    Bank.EntityClass = TEXT("Synthetic_ItemBank");
    Bank.InteriorIndex = 0;
    FSCInventoryContainerRecord Hold;
    Hold.EntityName = TEXT("Synthetic_Hold");
    M.InventoryContainers = {Hold, Bank};

    const TArray<FSCWaypointCandidate> W = SCHangarWaypoints::Select(M);
    if (!TestEqual(TEXT("five gateways, seven panels, one inventory container"), W.Num(), 13)) return false;
    TestTrue(TEXT("gateways first, in export order"), W[0].Source == ESCWaypointSource::TransitGateway && W[0].Interior == 1 && W[0].Index == 1 &&
                                                           W[1].Interior == 2 && W[2].Interior == 4 && W[3].Interior == 5 && W[4].Index == 1);
    TestTrue(TEXT("operated panels: the kiosks and the door pad of the room, the drop-off panel; not the sign, not the cabinet, not "
                  "the call panel a lone gateway stands for"),
             W[5].Source == ESCWaypointSource::Panel && W[5].Interior == 0 && W[5].Index == 0 && W[6].Index == 3 && W[7].Interior == 0 &&
             W[7].Index == 4 && W[8].Interior == 3 && W[8].Index == 0);
    TestTrue(TEXT("a kiosk beside a gateway stays a place"), W[9].Source == ESCWaypointSource::Panel && W[9].Interior == 4 && W[9].Index == 1);
    TestTrue(TEXT("two gateways, two transit panels: both panels stay"), W[10].Interior == 5 && W[10].Index == 0 && W[11].Interior == 5 && W[11].Index == 1);
    TestTrue(TEXT("the interior inventory container"), W[12].Source == ESCWaypointSource::Inventory && W[12].Index == 1 && W[12].Label == TEXT("Synthetic ItemBank"));
    TestEqual(TEXT("labels: the authored entity name (not the shared mesh), cleaned like vehicle spawns, without the instance counter"),
              W[5].Label, FString(TEXT("Synthetic Kiosk")));
    TestEqual(TEXT("a mesh LOD suffix is not part of the name"), SCHangarWaypoints::CleanLabel(TEXT("Synthetic_Pad_Mesh_LOD0")), FString(TEXT("Synthetic Pad Mesh")));
    TestEqual(TEXT("an inner number stays"), SCHangarWaypoints::CleanLabel(TEXT("Synthetic_1_a")), FString(TEXT("Synthetic 1 a")));
    TArray<FString> Labels = {TEXT("A"), TEXT("B"), TEXT("A")};
    SCHangarWaypoints::NumberDuplicates(Labels);
    TestTrue(TEXT("repeated labels numbered in order, unique ones kept"), Labels[0] == TEXT("A #1") && Labels[1] == TEXT("B") && Labels[2] == TEXT("A #2"));

    // BuildWaypoints: each source at its anchor (plus its local hint), relative to the actor; numbered labels.
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* Env = World->SpawnActor<ASCVehicle>(ASCVehicle::StaticClass(), FTransform(FVector(1000, 0, 0)));
    for (const FVector& At : {FVector(100, 0, 0), FVector(0, 200, 0)})
    {
        auto* Anchor = NewObject<USceneComponent>(Env);
        Anchor->SetupAttachment(Env->GetRootComponent());
        Anchor->SetRelativeLocation(At);
        Anchor->RegisterComponent();
        Env->WaypointSources.Add({ESCWaypointSource::Panel, TEXT("Kiosk"), Anchor, FVector(0, 0, 10)});
    }
    TestEqual(TEXT("built"), Env->BuildWaypoints(), 2);
    TestTrue(TEXT("actor-relative, numbered, kind Hangar, with their evidence"),
             Env->Waypoints.Num() == 2 && Env->Waypoints[1].Transform.GetLocation().Equals(FVector(0, 200, 10), 0.01) &&
             Env->Waypoints[0].DisplayName == TEXT("Kiosk #1") && Env->Waypoints[1].Kind == FSCSpawnKind::Hangar &&
             Env->Waypoints[0].Source == FName(TEXT("panel")));
    // A cooked environment: no sources, no rigs (no ship pad to find): the saved waypoints stay.
    Env->WaypointSources.Reset();
    const int32 Revision = Env->WaypointRevision;
    TestTrue(TEXT("a cooked environment keeps its waypoints"), Env->BuildWaypoints() == 2 && Env->WaypointRevision == Revision);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCHangarTeleportMenu, "SCBridge.Hangar.TeleportMenu", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCHangarTeleportMenu::RunTest(const FString&)
{
    using EType = FSCTeleportRow::EType;
    // Sections.
    const TArray<FSCTeleportRow> Both = SCTeleportMenu::Rows(2, 2);
    TestTrue(TEXT("ship heading, its spawns, hangar heading, its places"),
             Both.Num() == 6 && !Both[0].IsDestination() && Both[1].Type == EType::Ship && Both[2].Index == 1 && !Both[3].IsDestination() &&
             Both[3].Index == int32(EType::Hangar) && Both[4].Type == EType::Hangar && Both[5].Index == 1);
    TestTrue(TEXT("no heading for an empty section"), SCTeleportMenu::Rows(2, 0).Num() == 3 && SCTeleportMenu::Rows(0, 1).Num() == 2 &&
                                                         SCTeleportMenu::Rows(0, 1)[0].Index == int32(EType::Hangar));
    TestEqual(TEXT("down from the last ship spawn: the first hangar place"), SCTeleportMenu::Step(Both, 2, 1), 4);
    TestEqual(TEXT("up from the first hangar place: the last ship spawn"), SCTeleportMenu::Step(Both, 4, -1), 2);
    TestEqual(TEXT("wraps at the end"), SCTeleportMenu::Step(Both, 5, 1), 1);
    TestEqual(TEXT("wraps at the start"), SCTeleportMenu::Step(Both, 1, -1), 5);
    TestEqual(TEXT("from a heading or nothing: the first destination"), SCTeleportMenu::Step(Both, 3, 1), 1);
    TestEqual(TEXT("nothing to select"), SCTeleportMenu::Step(SCTeleportMenu::Rows(0, 0), INDEX_NONE, 1), int32(INDEX_NONE));

    // The character: a floor, a ship with two spawns on it, a hangar with three waypoints (one inside a solid block).
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    World->InitializeActorsForPlay(FURL());
    World->BeginPlay();
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Character = World->SpawnActor<ASCFirstPersonCharacter>(ASCFirstPersonCharacter::StaticClass(), FTransform(FVector(0, 0, 5000)), Params);
    AActor* Set = World->SpawnActor<AActor>();
    auto* Root = NewObject<USceneComponent>(Set);
    Set->SetRootComponent(Root);
    Root->RegisterComponent();
    AddBlock(Set, Root, FVector(0, 0, -10), FVector(20000, 20000, 10)); // floor, top at z = 0
    AddBlock(Set, Root, FVector(3000, 3000, 300), FVector(400, 400, 300)); // a solid block, a waypoint inside it
    auto* Ship = World->SpawnActor<ASCVehicle>();
    Ship->AddSpawnPoint(FSCSpawnKind::Interior, TEXT("Deck"), FTransform(FVector(0, 0, 0)));
    Ship->AddSpawnPoint(FSCSpawnKind::Hull, TEXT("Above hull centre"), FTransform(FVector(500, 0, 0)));
    Ship->bCollisionReady = true;
    auto* Hangar = World->SpawnActor<ASCVehicle>(ASCVehicle::StaticClass(), FTransform(FVector(1000, 1000, 0)));
    for (const FVector& At : {FVector(1000, 0, 0), FVector(2000, 2000, 0), FVector(0, 1000, 0)})
    {
        FSCSpawnPoint& P = Hangar->Waypoints.AddDefaulted_GetRef();
        P.Kind = FSCSpawnKind::Hangar;
        P.DisplayName = At.ToCompactString();
        P.Transform = FTransform(At);
    }
    Character->HangarOverride = Hangar;
    if (!TestTrue(TEXT("explores the ship"), Character->Explore(Ship))) return false;
    const TArray<FSCSpawnPoint>& Spots = Character->GetHangarSpots();
    TestTrue(TEXT("the place inside the block is not offered"), Spots.Num() == 2 && Spots[1].DisplayName == FVector(0, 1000, 0).ToCompactString());
    const TArray<FSCTeleportRow> Rows = Character->TeleportRows();
    TestTrue(TEXT("both sections"), Rows.Num() == 6 && Rows[4].Type == EType::Hangar);

    TestFalse(TEXT("Down does nothing with the menu closed"), Character->HandleKey(EKeys::Down, true));
    Character->HandleAction(ESCFirstPersonAction::Teleport, true);
    TestTrue(TEXT("T opens the menu on the current spawn"), Character->bTeleportMenuOpen && Character->TeleportSelectionRow() == 1);
    Character->HandleKey(EKeys::Down, true);
    Character->HandleKey(EKeys::Down, true);
    TestEqual(TEXT("Down crosses into the hangar section, past its heading"), Character->TeleportSelectionRow(), 4);
    Character->HandleKey(EKeys::W, true);
    TestEqual(TEXT("W moves up while the menu is open"), Character->TeleportSelectionRow(), 2);
    Character->HandleKey(EKeys::W, false);
    Character->HandleKey(EKeys::Up, true);
    Character->HandleKey(EKeys::Up, true);
    TestEqual(TEXT("Up wraps to the last hangar place"), Character->TeleportSelectionRow(), 5);
    Character->HandleKey(EKeys::LeftShift, true); // held while the menu is open: ignored, so nothing runs after the teleport
    TestTrue(TEXT("Enter teleports there"), Character->HandleKey(EKeys::Enter, true));
    TestFalse(TEXT("a key pressed in the menu is not held after it"), Character->IsRunning());
    Character->HandleKey(EKeys::LeftShift, false);
    const FVector Local = Hangar->GetActorTransform().InverseTransformPosition(Character->GetActorLocation());
    TestTrue(TEXT("standing at the hangar place, menu closed"), FVector2D(Local).Equals(FVector2D(0, 1000), 1) && Local.Z > 0 && !Character->bTeleportMenuOpen);
    TestTrue(TEXT("the mouse path: a row teleports"), Character->TeleportToRow(2) && Character->CurrentSpawn == 1);
    // A clicked row is its destination: after the list renumbers (a spawn inserted in front), it still names the same one.
    const FSCTeleportRow Clicked = Rows[4];
    const FString ClickedLabel = Character->DestinationLabel(Clicked);
    const FSCSpawnPoint Front = Ship->SpawnPoints[0];
    Ship->SpawnPoints.Insert(Front, 0);
    ++Ship->SpawnRevision;
    TestTrue(TEXT("the same hangar place after the ship list grew"), Character->FindDestination(Clicked, ClickedLabel) == 5 &&
                                                                       Character->TeleportToDestination(Clicked, ClickedLabel));
    TestFalse(TEXT("a destination no longer listed is refused"), Character->TeleportToDestination({EType::Hangar, 7}, TEXT("gone")));
    TestFalse(TEXT("a heading is not a destination"), Character->TeleportToRow(4)); // the Hangar heading, after the third ship spawn
    TestTrue(TEXT("hangar places by index"), Character->TeleportToHangar(0) && !Character->TeleportToHangar(2));
    return !HasAnyErrors();
}
#endif
