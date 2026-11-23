// SCBridge.Lift.* (WP-P5): the C2 Hercules lift's motion profile under the constant-acceleration reading, its request queue
// and door handshake against stub doors, the stop order read from a package, and the link dispatcher on the C2's links.
// SCBridge.FloorPanel.* (WP-P5 floor panel): the options from the panel's links and the stops, the layout, the aim (a ray
// on the face -> UV -> row; nothing off the face), E dispatching the option's request, the view and the colour roles.
// Values are the C2's (LIVE 4.10.2: MaxSpeed 3.5, EasingDistance 5.1, OpenWaitTime 4, QueueWaitTime 5; stops z -14.65,
// -7.875, -0.5; gate 0.5 s, hold door 2.67 s, top door 1.67 s, belly hatch 6.67 s).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCLift.h"
#include "SCPackageReader.h"
#include "SCPackageSource.h"
#include "SCRigComponent.h"
#include "UObject/Package.h"
#include "Misc/ScopeExit.h"
#include "SCVehicle.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ObjectSaveContext.h"

namespace
{
constexpr double C2MaxSpeed = 3.5, C2Easing = 5.1;

class FLiftMemorySource : public FSCPackageSource
{
public:
    TMap<FString, TArray<uint8>> Files;
    void Add(const FString& Name, const FString& Text)
    { FTCHARToUTF8 Utf8(*Text); TArray<uint8> B; B.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); Files.Add(Name, MoveTemp(B)); }
    bool Exists(const FString& Path) const override { return Files.Contains(Path); }
    bool ReadBytes(const FString& Path, TArray<uint8>& Out, FString& Error) const override
    { if (auto* B = Files.Find(Path)) { Out = *B; return true; } Error = TEXT("Missing fixture"); return false; }
    TArray<FString> ListFiles(const FString& Prefix) const override
    { TArray<FString> R; for (const auto& P : Files) if (Prefix.IsEmpty() || P.Key.StartsWith(Prefix)) R.Add(P.Key); R.Sort(); return R; }
};

/** The C2 lift's package records (scene.json shape), stops in chain order ground, hold, top. */
const TCHAR* C2LiftScene = TEXT(R"({"version":2,"export_kind":"Decomposed","root_entity":{"entity_name":"Ship"},"children":[],
  "interiors":[{"name":"top_deck_c2","placements":[],
    "entities":[{"entity_id":"floor-000","entity_name":"Elevator_Floor-000","entity_class":"Elevator_Floor","kind":"path_node",
                 "transform":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[-7.5,-3,-0.5,1]],"links":["door-top"],"interaction_links":[],"interaction_points":[]}],
    "lifts":[{"entity_id":"platform","entity_name":"CRUS_Hercules_Lift_Platform-001","entity_class":"CRUS_Hercules_Lift_Platform",
      "params":{"max_speed":3.5,"easing_distance":5.1,"slow_down_at_bends":false,"open_wait_time":4.0,"queue_wait_time":5.0,"idling_period":120.0,
                "open_inner_door_interaction":"OpenInnerDoor","open_inner_door_finished_interaction":"OpenInnerDoorFinished",
                "close_all_doors_interaction":"CloseAllDoors","close_inner_door_finished_interaction":"CloseInnerDoorFinished"},
      "grid_geometry":"Objects/Spaceships/Ships/CRUS/Starlifter/Interior/Lift/LocalGrid_Lift_Platform.cgf","rest_node_id":"floor-001",
      "stops":[{"index":0,"node_entity_id":"floor-003","kind":"stop","position":[-7.5,-3,-14.65],"landing_door_ids":[],"gateway_timing_in_transit":0.5},
               {"index":1,"node_entity_id":"floor-001","kind":"stop","position":[-7.5,-3,-7.875],"landing_door_ids":["door-hold"]},
               {"index":2,"node_entity_id":"floor-000","kind":"stop","position":[-7.5,-3,-0.5],"landing_door_ids":["door-top"]}],
      "riders":["gate","panel"]}]}]})");

/** Stub doors: a door moves for its clip length after PlayDoor, then holds the played state. */
struct FStubDoors
{
    struct FDoor { bool bOpen = false; double Until = -1; double Length = 0; };
    TMap<const USCRigComponent*, FDoor> Doors;
    TArray<FString> Log; // "<t> <door> open|close"
    TMap<const USCRigComponent*, FString> Names;
    const double* Clock = nullptr;
    USCRigComponent* Make(const TCHAR* Name, double Length)
    {
        USCRigComponent* Rig = NewObject<USCRigComponent>(GetTransientPackage());
        Rig->AddToRoot();
        Doors.Add(Rig, {false, -1, Length});
        Names.Add(Rig, Name);
        return Rig;
    }
    void Release() { for (auto& [Rig, D] : Doors) const_cast<USCRigComponent*>(Rig)->RemoveFromRoot(); }
    void Bind(FSCLift& Lift)
    {
        Clock = &Lift.Clock;
        Lift.Host.PlayDoor = [this](USCRigComponent* Rig, bool bOpen, float Speed) {
            FDoor& D = Doors.FindChecked(Rig);
            D.bOpen = bOpen;
            D.Until = *Clock + D.Length / FMath::Max(0.01f, Speed);
            Log.Add(FString::Printf(TEXT("%.2f %s %s"), *Clock, *Names[Rig], bOpen ? TEXT("open") : TEXT("close")));
            return true;
        };
        Lift.Host.IsDoorOpen = [this](const USCRigComponent* Rig) { return Doors.FindChecked(Rig).bOpen; };
        Lift.Host.IsDoorMoving = [this](const USCRigComponent* Rig) { return *Clock < Doors.FindChecked(Rig).Until; };
    }
};

bool ReadC2(FAutomationTestBase& Test, FSCSceneManifest& M)
{
    FLiftMemorySource Source;
    Source.Add(TEXT("scene.json"), C2LiftScene);
    FString Error;
    return Test.TestTrue(TEXT("reads: ") + Error, FSCPackageReader::Read(Source, M, Error)) && Test.TestEqual(TEXT("lifts"), M.Interiors[0].Lifts.Num(), 1);
}
/** Identity mapping metres -> cm (z up): the tests measure the carriage in the container frame. */
FVector StopCm(const FVector& Metres) { return Metres * 100.0; }

void Run(FSCLift& Lift, double Seconds, double Step = 1.0 / 60)
{
    for (double T = 0; T < Seconds; T += Step) Lift.Tick(Step);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLiftProfileTest, "SCBridge.Lift.Profile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLiftProfileTest::RunTest(const FString&)
{
    // The brief's figures under the reading a = MaxSpeed^2 / (2 EasingDistance) = 1.201 m/s^2.
    struct FLeg { double Distance, Seconds, Peak; };
    for (const FLeg& L : {FLeg{7.375, 4.956, 2.976}, FLeg{6.775, 4.750, 2.853}, FLeg{14.15, 6.957, 3.5}})
    {
        const FSCLiftProfile P = FSCLiftProfile::Make(L.Distance, C2MaxSpeed, C2Easing);
        TestNearlyEqual(*FString::Printf(TEXT("%.3f m leg time"), L.Distance), P.Duration(), L.Seconds, 0.005);
        TestNearlyEqual(*FString::Printf(TEXT("%.3f m peak"), L.Distance), P.PeakSpeed, L.Peak, 0.005);
        double Last = -1;
        bool bMonotonic = true, bBounded = true;
        for (double T = 0; T <= P.Duration() + 1e-9; T += 0.01)
        {
            const double S = P.PositionAt(T);
            bMonotonic &= S >= Last - 1e-9;
            bBounded &= P.VelocityAt(T) <= C2MaxSpeed + 1e-9;
            Last = S;
        }
        TestTrue(TEXT("monotonic s"), bMonotonic);
        TestTrue(TEXT("v <= MaxSpeed"), bBounded);
        TestNearlyEqual(TEXT("ends at the stop"), P.PositionAt(P.Duration()), L.Distance, 1e-6);
        TestEqual(TEXT("ramps up first"), P.PhaseAt(0.1), ESCLiftSpeedPhase::RampUp);
        TestEqual(TEXT("ramps down last"), P.PhaseAt(P.Duration() - 0.1), ESCLiftSpeedPhase::RampDown);
    }
    TestEqual(TEXT("a triangle never cruises"), FSCLiftProfile::Make(7.375, C2MaxSpeed, C2Easing).PhaseAt(2.6), ESCLiftSpeedPhase::RampDown);
    TestEqual(TEXT("the long leg cruises"), FSCLiftProfile::Make(14.15, C2MaxSpeed, C2Easing).PhaseAt(3.4), ESCLiftSpeedPhase::FullSpeed);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLiftStopOrderTest, "SCBridge.Lift.StopOrder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLiftStopOrderTest::RunTest(const FString&)
{
    FSCSceneManifest M;
    if (!ReadC2(*this, M)) return false;
    const FSCLiftRecord& R = M.Interiors[0].Lifts[0];
    TestEqual(TEXT("params"), R.MaxSpeed, 3.5f);
    TestEqual(TEXT("close interaction"), R.CloseAllDoorsInteraction, FName(TEXT("CloseAllDoors")));
    TestEqual(TEXT("riders"), R.Riders.Num(), 2);
    TestEqual(TEXT("entities"), M.Interiors[0].Entities.Num(), 1);
    FSCLift Lift;
    Lift.Init(R, [](const FVector& V) { return StopCm(V); });
    if (!TestEqual(TEXT("stops"), Lift.Stops.Num(), 3)) return false;
    TestEqual(TEXT("ground first"), Lift.Stops[0].NodeId, FString(TEXT("floor-003")));
    TestEqual(TEXT("rests at the hold"), Lift.RestStop, 1);
    TestEqual(TEXT("at the hold"), Lift.CurrentStop, 1);
    TestNearlyEqual(TEXT("z ground"), Lift.Stops[0].ZCm, -1465.0, 0.01);
    TestNearlyEqual(TEXT("z hold"), Lift.Stops[1].ZCm, -787.5, 0.01);
    TestNearlyEqual(TEXT("z top"), Lift.Stops[2].ZCm, -50.0, 0.01);
    TestNearlyEqual(TEXT("path hold -> top"), Lift.Stops[2].S - Lift.Stops[1].S, 7.375, 1e-4);
    TestNearlyEqual(TEXT("path ground -> hold"), Lift.Stops[1].S - Lift.Stops[0].S, 6.775, 1e-4);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLiftQueueTest, "SCBridge.Lift.QueueAndDoors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLiftQueueTest::RunTest(const FString&)
{
    FSCSceneManifest M;
    if (!ReadC2(*this, M)) return false;
    FSCLift Lift;
    Lift.Init(M.Interiors[0].Lifts[0], [](const FVector& V) { return StopCm(V); });
    FStubDoors Doors;
    Doors.Bind(Lift);
    ON_SCOPE_EXIT { Doors.Release(); };
    Lift.Gate.Add({TEXT("gate"), Doors.Make(TEXT("gate"), 0.5)});
    Lift.Stops[1].LandingDoors.Add({TEXT("door-hold"), Doors.Make(TEXT("hold"), 80.0 / 30)});
    Lift.Stops[2].LandingDoors.Add({TEXT("door-top"), Doors.Make(TEXT("top"), 50.0 / 30)});
    USCRigComponent* Hatch = Doors.Make(TEXT("hatch"), 200.0 / 30);
    FSCLiftDoor HatchDoor{TEXT("hatch"), Hatch};
    HatchDoor.ZCm = -975;
    Lift.GatewayDoors.Add(HatchDoor);
    TArray<FString> Emitted;
    Lift.Host.Emit = [&](const FString& Entity, FName Interaction) { Emitted.Add(Entity + TEXT(":") + Interaction.ToString()); };

    // A call where the carriage stands only opens its doors: gate and hold door together.
    Lift.ReceiveNode(1, TEXT("DescentRequest"), TEXT("call-hold"), TEXT("Descend"));
    Run(Lift, 3.0);
    TestEqual(TEXT("open at the hold"), Lift.State, ESCLiftState::Open);
    TestEqual(TEXT("first doors"), Doors.Log, TArray<FString>{TEXT("0.02 gate open"), TEXT("0.02 hold open")});
    TestTrue(TEXT("the stop's DoorOpen is fired"), Emitted.Contains(TEXT("floor-001:DoorOpen")));
    // Top Deck from the floor panel: OpenWaitTime and QueueWaitTime pass, the doors close, then it moves and opens at the top.
    Doors.Log.Reset();
    const double Pressed = Lift.Clock;
    Lift.ReceiveCarriage(TEXT("From Elevator call 2"), TEXT("panel"), TEXT("RequestFloor1"));
    Run(Lift, 4.9);
    TestTrue(TEXT("still open during the batching wait"), Lift.State == ESCLiftState::Open);
    double Closed = -1, Departed = -1, Arrived = -1;
    for (int32 I = 0; I < 2000 && !(Lift.CurrentStop == 2 && Lift.State == ESCLiftState::Open); ++I)
    {
        Lift.Tick(1.0 / 60);
        if (Closed < 0 && Lift.State == ESCLiftState::WaitingForGateway) Closed = Lift.Clock;
        if (Departed < 0 && Lift.State == ESCLiftState::Moving) Departed = Lift.Clock;
        if (Arrived < 0 && Lift.CurrentStop == 2) Arrived = Lift.Clock;
    }
    TestEqual(TEXT("at the top"), Lift.CurrentStop, 2);
    TestEqual(TEXT("door order"), Doors.Log.Num(), 4);
    if (Doors.Log.Num() == 4)
    {
        TestTrue(TEXT("gate closes first"), Doors.Log[0].EndsWith(TEXT("gate close")) && Doors.Log[1].EndsWith(TEXT("hold close")));
        TestTrue(TEXT("then opens at the top"), Doors.Log[2].EndsWith(TEXT("gate open")) && Doors.Log[3].EndsWith(TEXT("top open")));
    }
    TestTrue(TEXT("doors close after the queue wait"), Closed - Pressed >= 5.0 + 80.0 / 30 - 0.05);
    TestNearlyEqual(TEXT("leg time (7.375 m)"), Arrived - Departed, 4.956, 0.05);
    TestTrue(TEXT("Arrived at 2 is emitted"), Emitted.Contains(TEXT("platform:Arrived at 2")));
    TestNearlyEqual(TEXT("carriage at the top"), Lift.ZAt(Lift.S), -50.0, 0.01);

    // To the ground with the hatch closed: blocked, no move. With the hatch opening: waits for its clip, then moves.
    Run(Lift, 4.5);
    Lift.ReceiveCarriage(TEXT("From Elevator call 0"), TEXT("panel"), TEXT("RequestSurface"));
    Run(Lift, 12.0);
    TestEqual(TEXT("blocked by the closed hatch"), Lift.State, ESCLiftState::Idle);
    TestEqual(TEXT("still at the top"), Lift.CurrentStop, 2);
    Lift.ReceiveCarriage(TEXT("From Elevator call 0"), TEXT("panel"), TEXT("RequestSurface"));
    Lift.Host.PlayDoor(Hatch, true, 1.f);
    const double HatchStarted = Lift.Clock;
    double Waited = -1, Moved = -1;
    for (int32 I = 0; I < 3000 && Lift.CurrentStop != 0; ++I)
    {
        Lift.Tick(1.0 / 60);
        if (Waited < 0 && Lift.State == ESCLiftState::WaitingForGateway) Waited = Lift.Clock;
        if (Moved < 0 && Lift.State == ESCLiftState::Moving) Moved = Lift.Clock;
    }
    TestTrue(TEXT("waited for the gateway"), Waited >= 0);
    TestTrue(TEXT("moved only after the hatch opened"), Moved - HatchStarted >= 200.0 / 30 - 0.02);
    TestEqual(TEXT("at the ground"), Lift.CurrentStop, 0);
    // Queue order: two requests from the ground: the nearer stop first.
    Run(Lift, 2.0);
    Lift.ReceiveCarriage(TEXT("From Elevator call 2"), TEXT("console"), NAME_None);
    Lift.ReceiveCarriage(TEXT("From Elevator call 1"), TEXT("console"), NAME_None);
    for (int32 I = 0; I < 3000 && Lift.State != ESCLiftState::Moving; ++I) Lift.Tick(1.0 / 60);
    TestEqual(TEXT("nearest requested stop first"), Lift.TargetStop, 1);
    return true;
}

// Review fixes: a door without a clip neither hangs the handshake nor lets the carriage leave with it open; a stop
// cancelled while the carriage waits for a gateway is not travelled to; a leg sweep that finds an obstacle blocks it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLiftFaultsTest, "SCBridge.Lift.Faults", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLiftFaultsTest::RunTest(const FString&)
{
    FSCSceneManifest M;
    if (!ReadC2(*this, M)) return false;
    FSCLift Lift;
    Lift.Init(M.Interiors[0].Lifts[0], [](const FVector& V) { return StopCm(V); });
    FStubDoors Doors;
    Doors.Bind(Lift);
    ON_SCOPE_EXIT { Doors.Release(); };
    USCRigComponent* Gate = Doors.Make(TEXT("gate"), 0.5);
    USCRigComponent* Hold = Doors.Make(TEXT("hold"), 80.0 / 30);
    Lift.Gate.Add({TEXT("gate"), Gate});
    Lift.Stops[1].LandingDoors.Add({TEXT("door-hold"), Hold});
    // The gate has no clip at all: the doors still open (the hold door) and the lift is not stuck in Opening.
    auto Play = Lift.Host.PlayDoor;
    Lift.Host.PlayDoor = [&](USCRigComponent* Rig, bool bOpen, float Speed) { return Rig != Gate && Play(Rig, bOpen, Speed); };
    Lift.ReceiveNode(1, TEXT("DescentRequest"), TEXT("call"), NAME_None);
    Run(Lift, 3.0);
    TestEqual(TEXT("open without the gate's clip"), Lift.State, ESCLiftState::Open);
    // The hold door cannot close (no Close clip): the leg is blocked and the lift stands open at the hold.
    Lift.Host.PlayDoor = [&](USCRigComponent* Rig, bool bOpen, float Speed) { return Rig != Gate && bOpen && Play(Rig, bOpen, Speed); };
    Lift.ReceiveCarriage(TEXT("From Elevator call 2"), TEXT("panel"), NAME_None);
    Run(Lift, 12.0);
    TestEqual(TEXT("never leaves with a door open"), Lift.CurrentStop, 1);
    TestEqual(TEXT("stands open"), Lift.State, ESCLiftState::Open);
    TestTrue(TEXT("the request is dropped"), Lift.Queue.IsEmpty());
    // Doors work again; a sweep reports a moving door: the lift waits; the request is cancelled meanwhile: it stays.
    Lift.Host.PlayDoor = Play;
    FSCLiftHost::ELeg Sweep = FSCLiftHost::ELeg::Wait;
    Lift.Host.SweepLeg = [&](int32, int32, FString& By) { By = TEXT("hatch"); return Sweep; };
    Lift.ReceiveCarriage(TEXT("From Elevator call 0"), TEXT("panel"), NAME_None);
    for (int32 I = 0; I < 2000 && Lift.State != ESCLiftState::WaitingForGateway; ++I) Lift.Tick(1.0 / 60);
    TestEqual(TEXT("waiting for the gateway"), Lift.State, ESCLiftState::WaitingForGateway);
    Run(Lift, 1.0);
    TestEqual(TEXT("still waiting"), Lift.State, ESCLiftState::WaitingForGateway);
    Lift.ReceiveNode(0, TEXT("CancelStop"), TEXT("console"), NAME_None);
    Run(Lift, 1.0);
    TestEqual(TEXT("a cancelled stop is not travelled to"), Lift.State, ESCLiftState::Idle);
    TestEqual(TEXT("at the hold"), Lift.CurrentStop, 1);
    // Something in the way: blocked, no move.
    Sweep = FSCLiftHost::ELeg::Blocked;
    Lift.ReceiveCarriage(TEXT("From Elevator call 2"), TEXT("panel"), NAME_None);
    Run(Lift, 1.0);
    TestEqual(TEXT("blocked leg: no move"), Lift.CurrentStop, 1);
    TestTrue(TEXT("blocked request dropped"), Lift.Queue.IsEmpty());
    Sweep = FSCLiftHost::ELeg::Clear;
    Lift.ReceiveCarriage(TEXT("From Elevator call 2"), TEXT("panel"), NAME_None);
    for (int32 I = 0; I < 2000 && Lift.CurrentStop != 2; ++I) Lift.Tick(1.0 / 60);
    TestEqual(TEXT("a clear leg runs"), Lift.CurrentStop, 2);
    return true;
}

// WP-P5 ground stop: a leg toward a stop below the ground ends on the first surface under the carriage, with the profile made
// for the shortened distance; without a contact in the leg the authored stop ends it. The C2's case: the ground stop is
// 0.6 m below the hangar pad of the viewer's landed pose (s 0.6 m along the path from stop 0).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLiftGroundStopTest, "SCBridge.Lift.GroundStop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLiftGroundStopTest::RunTest(const FString&)
{
    constexpr double HoldS = 6.775, Surface = 0.6;
    const double Authored = FSCLiftProfile::Make(HoldS, C2MaxSpeed, C2Easing).Duration();
    // The rule alone.
    {
        const FSCLiftLegPlan P = FSCLiftLegPlan::Make(HoldS, 0, HoldS - Surface, C2MaxSpeed, C2Easing);
        TestTrue(TEXT("contact above the stop shortens the leg"), P.bContact);
        TestNearlyEqual(TEXT("ends on the surface"), P.EndS, Surface, 1e-9);
        TestNearlyEqual(TEXT("shortened distance"), P.Distance, HoldS - Surface, 1e-9);
        TestNearlyEqual(TEXT("authored distance kept"), P.AuthoredDistance, HoldS, 1e-9);
        const FSCLiftProfile Short = FSCLiftProfile::Make(HoldS - Surface, C2MaxSpeed, C2Easing);
        TestNearlyEqual(TEXT("time recomputed for the shortened distance"), P.Profile.Duration(), Short.Duration(), 1e-9);
        TestTrue(TEXT("shorter than the authored leg"), P.Profile.Duration() < Authored - 0.1);
        TestNearlyEqual(TEXT("rests at the contact"), P.Profile.PositionAt(P.Profile.Duration()), HoldS - Surface, 1e-6);
        TestTrue(TEXT("decelerates into the contact"), P.Profile.PhaseAt(P.Profile.Duration() - 0.05) == ESCLiftSpeedPhase::RampDown &&
                                                         P.Profile.VelocityAt(P.Profile.Duration() - 0.05) < 0.1);
        for (const TOptional<double>& None : {TOptional<double>(), TOptional<double>(HoldS), TOptional<double>(HoldS + 1), TOptional<double>(-0.5)})
        {
            const FSCLiftLegPlan N = FSCLiftLegPlan::Make(HoldS, 0, None, C2MaxSpeed, C2Easing);
            TestFalse(TEXT("no contact in the leg"), N.bContact);
            TestNearlyEqual(TEXT("the authored stop"), N.EndS, 0.0, 1e-9);
            TestNearlyEqual(TEXT("the authored time"), N.Profile.Duration(), Authored, 1e-9);
        }
    }
    // In the lift: hold -> ground stops on the surface and arrives there (doors, events); ground -> hold starts from it;
    // hold -> top (no surface under a leg upward) is unchanged.
    FSCSceneManifest M;
    if (!ReadC2(*this, M)) return false;
    FSCLift Lift;
    Lift.Init(M.Interiors[0].Lifts[0], [](const FVector& V) { return StopCm(V); });
    FStubDoors Doors;
    Doors.Bind(Lift);
    ON_SCOPE_EXIT { Doors.Release(); };
    Lift.Gate.Add({TEXT("gate"), Doors.Make(TEXT("gate"), 0.5)});
    TArray<FString> Emitted;
    Lift.Host.Emit = [&](const FString& Entity, FName Interaction) { Emitted.Add(Entity + TEXT(":") + Interaction.ToString()); };
    Lift.Host.GroundContact = [&](int32 To, double& Travel, FString& By) {
        if (To != 0 || Lift.S <= Surface) return false;
        Travel = Lift.S - Surface;
        By = TEXT("pad");
        return true;
    };
    auto Ride = [&](int32 To) {
        Lift.ReceiveCarriage(*FString::Printf(TEXT("From Elevator call %d"), To), TEXT("panel"), NAME_None);
        double Departed = -1, Arrived = -1, Last = Lift.S;
        bool bMonotonic = true;
        for (int32 I = 0; I < 3000 && !(Lift.CurrentStop == To && Lift.State == ESCLiftState::Open); ++I)
        {
            Lift.Tick(1.0 / 60);
            if (Departed < 0 && Lift.State == ESCLiftState::Moving) Departed = Lift.Clock;
            if (Lift.State == ESCLiftState::Moving) { bMonotonic &= (Lift.S - Last) * (To == 0 ? -1 : 1) >= -1e-9; Last = Lift.S; }
            if (Arrived < 0 && Lift.CurrentStop == To) Arrived = Lift.Clock;
        }
        TestTrue(*FString::Printf(TEXT("monotonic to %d"), To), bMonotonic);
        return Arrived - Departed;
    };
    const double Down = Ride(0);
    TestEqual(TEXT("arrived at the ground stop"), Lift.CurrentStop, 0);
    TestEqual(TEXT("doors open there"), Lift.State, ESCLiftState::Open);
    TestTrue(TEXT("on the surface"), Lift.bAtContact);
    TestNearlyEqual(TEXT("contact height"), Lift.ZAt(Lift.S), -1465.0 + Surface * 100, 0.01);
    TestNearlyEqual(TEXT("shortened leg time"), Down, FSCLiftProfile::Make(HoldS - Surface, C2MaxSpeed, C2Easing).Duration(), 0.04);
    TestTrue(TEXT("Arrived at 0 is emitted"), Emitted.Contains(TEXT("platform:Arrived at 0")));
    TestTrue(TEXT("the gate opened"), !Doors.Log.IsEmpty() && Doors.Log.Last().EndsWith(TEXT("gate open")));
    const double Up = Ride(1);
    TestEqual(TEXT("back at the hold"), Lift.CurrentStop, 1);
    TestFalse(TEXT("not on a surface"), Lift.bAtContact);
    TestNearlyEqual(TEXT("hold height"), Lift.ZAt(Lift.S), -787.5, 0.01);
    TestNearlyEqual(TEXT("up from the surface: the shortened distance"), Up, FSCLiftProfile::Make(HoldS - Surface, C2MaxSpeed, C2Easing).Duration(), 0.04);
    const double Top = Ride(2);
    TestNearlyEqual(TEXT("hold -> top unchanged"), Top, 4.956, 0.04);
    TestNearlyEqual(TEXT("top height"), Lift.ZAt(Lift.S), -50.0, 0.01);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCLiftDispatchTest, "SCBridge.Lift.Dispatch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCLiftDispatchTest::RunTest(const FString&)
{
    // The C2's InteractionLinkController links (top_deck_c2.soc), ids shortened.
    FSCInteractionNetwork N;
    auto Link = [](const TCHAR* From, const TCHAR* To, const TCHAR* Slot) { return FSCInteractionLink{FName(From), To, FName(Slot)}; };
    N.At(0, TEXT("panel")).Links = {Link(TEXT("RequestSurface"), TEXT("platform"), TEXT("From Elevator call 0")),
                                    Link(TEXT("RequestFloor0"), TEXT("platform"), TEXT("From Elevator call 1")),
                                    Link(TEXT("RequestFloor1"), TEXT("platform"), TEXT("From Elevator call 2")),
                                    Link(TEXT("RequestSurface"), TEXT("hatch"), TEXT("Open"))};
    N.At(0, TEXT("call-hold")).Links = {Link(TEXT("Descend"), TEXT("floor-001"), TEXT("DescentRequest"))};
    N.At(0, TEXT("ground-a")).Links = {Link(TEXT("Descend"), TEXT("floor-003"), TEXT("DescentRequest")), Link(TEXT("Descend"), TEXT("hatch"), TEXT("Open"))};
    N.At(0, TEXT("platform")).Links = {Link(TEXT("Arrived at 1"), TEXT("hatch"), TEXT("Close")), Link(TEXT("Arrived at 2"), TEXT("hatch"), TEXT("Close"))};
    N.At(0, TEXT("floor-003")).Links = {Link(TEXT("DoorOpen"), TEXT("floor-001"), TEXT("DoorOpen"))};
    auto Targets = [&](const TCHAR* Source, const TCHAR* Slot) {
        TArray<FString> Out;
        for (const FSCInteractionNetwork::FDelivery& D : N.Route(0, Source, FName(Slot))) Out.Add(D.Target + TEXT(":") + D.TargetSlot.ToString());
        return Out;
    };
    TestEqual(TEXT("Exit Ship"), Targets(TEXT("panel"), TEXT("RequestSurface")), TArray<FString>{TEXT("platform:From Elevator call 0"), TEXT("hatch:Open")});
    TestEqual(TEXT("Top Deck"), Targets(TEXT("panel"), TEXT("RequestFloor1")), TArray<FString>{TEXT("platform:From Elevator call 2")});
    TestEqual(TEXT("ground call"), Targets(TEXT("ground-a"), TEXT("Descend")), TArray<FString>{TEXT("floor-003:DescentRequest"), TEXT("hatch:Open")});
    TestEqual(TEXT("arrival at the hold closes the hatch"), Targets(TEXT("platform"), TEXT("Arrived at 1")), TArray<FString>{TEXT("hatch:Close")});
    TestTrue(TEXT("arrival at the ground sends nothing"), Targets(TEXT("platform"), TEXT("Arrived at 0")).IsEmpty());
    TestTrue(TEXT("an unknown interaction routes nowhere"), Targets(TEXT("panel"), TEXT("RequestFloor2")).IsEmpty());
    TestEqual(TEXT("interior of an id"), N.InteriorOf(TEXT("floor-003")), 0);
    int32 Stop = -1, Dir = 9;
    TestTrue(TEXT("call slot"), FSCInteractionNetwork::StopSlot(TEXT("From Elevator call 2"), Stop, Dir) && Stop == 2 && Dir == 0);
    TestTrue(TEXT("600i descent slot"), FSCInteractionNetwork::StopSlot(TEXT("Request descent from 1"), Stop, Dir) && Stop == 1 && Dir == -1);
    TestFalse(TEXT("not a stop slot"), FSCInteractionNetwork::StopSlot(TEXT("From Elevator call"), Stop, Dir));
    bool bOpen = false, bToggle = false;
    TestTrue(TEXT("OpenSelf opens"), FSCInteractionNetwork::DoorVerb(TEXT("OpenSelf"), bOpen, bToggle) && bOpen && !bToggle);
    TestTrue(TEXT("Close closes"), FSCInteractionNetwork::DoorVerb(TEXT("Close"), bOpen, bToggle) && !bOpen);
    TestFalse(TEXT("DescentRequest is not a door verb"), FSCInteractionNetwork::DoorVerb(TEXT("DescentRequest"), bOpen, bToggle));
    // A node's DoorOpen with the carriage elsewhere is ignored (the relayed Floor-003 -> Floor-001 DoorOpen).
    FSCSceneManifest M;
    if (!ReadC2(*this, M)) return false;
    FSCLift Lift;
    Lift.Init(M.Interiors[0].Lifts[0], [](const FVector& V) { return StopCm(V); });
    TestTrue(TEXT("handled"), Lift.ReceiveNode(0, TEXT("DoorOpen"), TEXT("floor-003"), TEXT("DoorOpen")));
    TestEqual(TEXT("no doors at another stop"), Lift.State, ESCLiftState::Idle);
    TestFalse(TEXT("GatewayOpen is not a lift input"), Lift.ReceiveNode(0, TEXT("GatewayOpen"), TEXT("x"), NAME_None));
    return true;
}
namespace
{
/** A vehicle (no world) holding the C2 lift, its links (top_deck_c2.soc, ids shortened) and its two panels. The stop names
 *  are the floor panel's display names (BuildInteriorLifts routes them in an import). */
ASCVehicle* MakeC2Vehicle(const FSCSceneManifest& M)
{
    ASCVehicle* V = NewObject<ASCVehicle>(GetTransientPackage());
    V->AddToRoot();
    TSharedPtr<FSCLift> Lift = MakeShared<FSCLift>();
    Lift->Init(M.Interiors[0].Lifts[0], [](const FVector& X) { return StopCm(X); });
    Lift->Id = 0;
    Lift->Interior = 0;
    const TCHAR* Names[] = {TEXT("Exit Ship"), TEXT("Lower Deck"), TEXT("Top Deck")};
    for (int32 S = 0; S < 3; ++S) Lift->Stops[S].Name = Names[S];
    V->Lifts.Add(Lift);
    auto Link = [](const TCHAR* From, const TCHAR* To, const TCHAR* Slot) { return FSCInteractionLink{FName(From), To, FName(Slot)}; };
    V->Interactions.At(0, TEXT("panel")).Links = {Link(TEXT("RequestSurface"), TEXT("platform"), TEXT("From Elevator call 0")),
                                                  Link(TEXT("RequestFloor0"), TEXT("platform"), TEXT("From Elevator call 1")),
                                                  Link(TEXT("RequestFloor1"), TEXT("platform"), TEXT("From Elevator call 2")),
                                                  Link(TEXT("RequestSurface"), TEXT("hatch"), TEXT("Open"))};
    V->Interactions.At(0, TEXT("call-hold")).Links = {Link(TEXT("Descend"), TEXT("floor-001"), TEXT("DescentRequest"))};
    V->Interactions.At(0, TEXT("platform")).Lift = 0;
    for (int32 S = 0; S < 3; ++S)
    {
        FSCInteractionNetwork::FEndpoint& E = V->Interactions.At(0, Lift->Stops[S].NodeId);
        E.LiftOfNode = 0;
        E.Stop = S;
    }
    return V;
}
FSCPointInteraction Choice(const TCHAR* Name, const TCHAR* Display, bool bPrimary)
{
    FSCPointInteraction C;
    C.Name = Name;
    C.DisplayName = Display;
    C.bPrimary = bPrimary;
    return C;
}
ASCVehicle::FLiftPanel FloorPanelRecord()
{
    // The point's interactions in authored order: Top Deck (primary), Lower Deck, Exit Ship.
    ASCVehicle::FLiftPanel P;
    P.Interior = 0;
    P.EntityId = TEXT("panel");
    P.Choices = {Choice(TEXT("RequestFloor1"), TEXT("Top Deck"), true), Choice(TEXT("RequestFloor0"), TEXT("Lower Deck"), false),
                 Choice(TEXT("RequestSurface"), TEXT("Exit Ship"), false)};
    return P;
}
/** A flat face in the component's YZ plane facing +X (a wall panel seen from +X), 27 x 23 cm, its UVs a window of a
 *  larger texture like the C2's floor panel (u 2.387..2.504, v 0.640..0.738) with v growing downwards. */
FSCScreenFaceGeometry WallFace()
{
    FSCScreenFaceGeometry G;
    const FVector3f TL(0, 13.5f, 11.5f), TR(0, -13.5f, 11.5f), BL(0, 13.5f, -11.5f), BR(0, -13.5f, -11.5f); // seen from +X, right is -Y
    const FVector2f UTL(2.387f, 0.640f), UTR(2.504f, 0.640f), UBL(2.387f, 0.738f), UBR(2.504f, 0.738f);
    G.Positions = {TL, BL, TR, TR, BL, BR};
    G.UVs = {UTL, UBL, UTR, UTR, UBL, UBR};
    G.Fronts = {FVector3f(1, 0, 0), FVector3f(1, 0, 0)};
    G.Nodes = {INDEX_NONE, INDEX_NONE};
    return G;
}
FSCFloorPanelOption Option(int32 Stop, const TCHAR* Interaction, const TCHAR* Label)
{
    FSCFloorPanelOption O;
    O.Stop = Stop;
    O.Interaction = Interaction;
    O.Label = Label;
    return O;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFloorPanelOptionsTest, "SCBridge.FloorPanel.OptionsAndLayout", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFloorPanelOptionsTest::RunTest(const FString&)
{
    FSCSceneManifest M;
    if (!ReadC2(*this, M)) return false;
    ASCVehicle* V = MakeC2Vehicle(M);
    ON_SCOPE_EXIT { V->RemoveFromRoot(); };
    int32 Lift = INDEX_NONE;
    const TArray<FSCFloorPanelOption> Options = V->FloorPanelOptions(FloorPanelRecord(), Lift);
    TestEqual(TEXT("the lift"), Lift, 0);
    TArray<FString> Rows;
    for (const FSCFloorPanelOption& O : Options) Rows.Add(FString::Printf(TEXT("%d %s %s"), O.Stop, *O.Label, *O.Interaction.ToString()));
    TestEqual(TEXT("rows from the highest stop down, labelled with the stops' names, each the panel's own interaction"), Rows,
              TArray<FString>{TEXT("2 Top Deck RequestFloor1"), TEXT("1 Lower Deck RequestFloor0"), TEXT("0 Exit Ship RequestSurface")});
    ASCVehicle::FLiftPanel Call;
    Call.Interior = 0;
    Call.EntityId = TEXT("call-hold");
    Call.Choices = {Choice(TEXT("Descend"), TEXT("Call Elevator"), true)};
    TestTrue(TEXT("a call button picks no floor"), V->FloorPanelOptions(Call, Lift).IsEmpty());
    // E without a drawn row: a call button sends its primary interaction; a floor-select panel nothing, drawn or not (it
    // is not in FloorPanels here: no face was built for it).
    TestEqual(TEXT("call button: its primary"), V->LiftPanelPressInteraction(Call), FName(TEXT("Descend")));
    TestTrue(TEXT("floor-select panel without a drawn face: nothing"), V->LiftPanelPressInteraction(FloorPanelRecord()).IsNone());
    // A row is labelled with its own interaction's display name (two panels may name one stop differently).
    ASCVehicle::FLiftPanel Renamed = FloorPanelRecord();
    Renamed.Choices[1].DisplayName = TEXT("Cargo Hold");
    const TArray<FSCFloorPanelOption> RenamedRows = V->FloorPanelOptions(Renamed, Lift);
    TestTrue(TEXT("the choice's own name"), RenamedRows.Num() == 3 && RenamedRows[1].Label == TEXT("Cargo Hold") && RenamedRows[1].Stop == 1);

    // The layout over the C2 face's aspect (27 x 23 cm).
    const FSCFloorPanelLayout L = FSCFloorPanelLayout::Make(Options, 27.0 / 23.0);
    TestEqual(TEXT("three rows"), L.Options.Num(), 3);
    for (int32 I = 0; I < L.Options.Num(); ++I)
    {
        const FBox2D& R = L.Options[I].Rect;
        TestTrue(TEXT("inside the face"), R.Min.X > 0 && R.Min.Y > 0 && R.Max.X < 1 && R.Max.Y < 1);
        TestEqual(TEXT("the row's centre selects it"), L.OptionAt(R.GetCenter()), I);
        if (I > 0) TestTrue(TEXT("top to bottom, apart"), R.Min.Y > L.Options[I - 1].Rect.Max.Y);
        TestTrue(TEXT("equal rows"), FMath::IsNearlyEqual(R.GetSize().Y, L.Options[0].Rect.GetSize().Y, 1e-9));
    }
    if (L.Options.Num() != 3) return false;
    TestTrue(TEXT("the status bar under the rows"), L.Status.Min.Y > L.Options[2].Rect.Max.Y && L.Status.Max.Y < 1);
    TestTrue(TEXT("square margins"), FMath::IsNearlyEqual(L.Options[0].Rect.Min.X * 27.0, L.Options[0].Rect.Min.Y * 23.0, 1e-6));
    TestEqual(TEXT("between rows: none"), L.OptionAt(FVector2D(0.5, (L.Options[0].Rect.Max.Y + L.Options[1].Rect.Min.Y) / 2)), INDEX_NONE);
    TestEqual(TEXT("the status bar: none"), L.OptionAt(L.Status.GetCenter()), INDEX_NONE);
    TestEqual(TEXT("the margin: none"), L.OptionAt(FVector2D(0.01, L.Options[0].Rect.GetCenter().Y)), INDEX_NONE);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFloorPanelAimTest, "SCBridge.FloorPanel.AimToOption", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFloorPanelAimTest::RunTest(const FString&)
{
    FSCScreenFaceGeometry G = WallFace();
    if (!TestTrue(TEXT("builds"), G.Build(FVector::UpVector))) return false;
    TestTrue(TEXT("front +X"), G.Normal.Equals(FVector(1, 0, 0), 1e-6));
    TestTrue(TEXT("right is -Y seen from the front"), G.Right.Equals(FVector(0, -1, 0), 1e-6));
    TestTrue(TEXT("size"), FMath::IsNearlyEqual(G.Width, 27.0, 1e-4) && FMath::IsNearlyEqual(G.Height, 23.0, 1e-4));
    TestTrue(TEXT("an affine unwrap fits exactly"), G.FitError < 1e-4);
    // The UV window maps onto the whole image, upright and unmirrored (M_SC_Screen samples through this map).
    TestTrue(TEXT("top-left"), G.ContentFromUV(FVector2f(2.387f, 0.640f)).Equals(FVector2D(0, 0), 1e-4));
    TestTrue(TEXT("top-right"), G.ContentFromUV(FVector2f(2.504f, 0.640f)).Equals(FVector2D(1, 0), 1e-4));
    TestTrue(TEXT("bottom-left"), G.ContentFromUV(FVector2f(2.387f, 0.738f)).Equals(FVector2D(0, 1), 1e-4));
    // A reflected, stretched placement (scale -1 across, 2 along): the image stays upright and unmirrored as seen, so the
    // UV corner that is now on the viewer's right maps to the image's right, and the aspect is the placed face's.
    FSCScreenFaceGeometry Mirrored = WallFace().Scaled(FVector(1, -1, 2));
    if (TestTrue(TEXT("reflected builds"), Mirrored.Build(FVector::UpVector)))
    {
        TestTrue(TEXT("still faces +X"), Mirrored.Normal.Equals(FVector(1, 0, 0), 1e-6));
        TestTrue(TEXT("the authored top-left is now top-right"), Mirrored.ContentFromUV(FVector2f(2.387f, 0.640f)).Equals(FVector2D(1, 0), 1e-4));
        TestTrue(TEXT("the placed aspect"), FMath::IsNearlyEqual(Mirrored.Aspect(), 27.0 / 46.0, 1e-4));
    }

    // Rays from 40 cm in front: each option's centre, through the UV, back to its row.
    const FSCFloorPanelLayout L = FSCFloorPanelLayout::Make(
        {Option(2, TEXT("RequestFloor1"), TEXT("Top Deck")), Option(1, TEXT("RequestFloor0"), TEXT("Lower Deck")), Option(0, TEXT("RequestSurface"), TEXT("Exit Ship"))},
        G.Aspect());
    const FVector Eye(40, 0, 0);
    for (int32 I = 0; I < 3; ++I)
    {
        const FVector Point = G.PointAt(L.Options[I].Rect.GetCenter());
        double Distance;
        FVector2f UV;
        TestTrue(TEXT("hits the face"), G.Raycast(Eye, (Point - Eye).GetSafeNormal(), 100, Distance, UV));
        TestEqual(TEXT("UV -> the option's row"), L.OptionAt(G.ContentFromUV(UV)), I);
    }
    double Distance;
    FVector2f UV;
    TestFalse(TEXT("beside the face: no hit"), G.Raycast(Eye, (FVector(0, 20, 0) - Eye).GetSafeNormal(), 100, Distance, UV));
    TestFalse(TEXT("from behind: no hit"), G.Raycast(FVector(-40, 0, 0), FVector(1, 0, 0), 100, Distance, UV));
    TestFalse(TEXT("beyond reach: no hit"), G.Raycast(Eye, FVector(-1, 0, 0), 30, Distance, UV));

    // The vehicle's aim: the panel and option under the crosshair; off the face nothing is highlighted.
    FSCSceneManifest M;
    if (!ReadC2(*this, M)) return false;
    ASCVehicle* V = MakeC2Vehicle(M);
    ON_SCOPE_EXIT { V->RemoveFromRoot(); };
    UStaticMeshComponent* Face = NewObject<UStaticMeshComponent>(V);
    FSCFloorPanel& P = V->FloorPanels.AddDefaulted_GetRef();
    P.Lift = 0;
    P.Interior = 0;
    P.EntityId = TEXT("panel");
    P.Component = Face;
    P.Geometry = G;
    P.Layout = L;
    int32 Panel, Opt;
    TestTrue(TEXT("aim at Lower Deck"), V->AimFloorPanel(Eye, G.PointAt(L.Options[1].Rect.GetCenter()) - Eye, 200, nullptr, Panel, Opt));
    TestEqual(TEXT("panel"), Panel, 0);
    TestEqual(TEXT("option"), Opt, 1);
    V->SetFloorPanelHighlight(Panel, Opt);
    TestEqual(TEXT("highlighted"), V->FloorPanels[0].Highlight, 1);
    TestTrue(TEXT("on the face, in the status bar"), V->AimFloorPanel(Eye, G.PointAt(L.Status.GetCenter()) - Eye, 200, nullptr, Panel, Opt));
    TestEqual(TEXT("no row"), Opt, INDEX_NONE);
    TestFalse(TEXT("off the face"), V->AimFloorPanel(Eye, FVector(0, 25, 0) - Eye, 200, nullptr, Panel, Opt));
    TestEqual(TEXT("no panel"), Panel, INDEX_NONE);
    V->SetFloorPanelHighlight(Panel, Opt);
    TestEqual(TEXT("nothing highlighted off the face"), V->FloorPanels[0].Highlight, INDEX_NONE);
    TestFalse(TEXT("out of reach"), V->AimFloorPanel(Eye, G.PointAt(L.Options[0].Rect.GetCenter()) - Eye, 30, nullptr, Panel, Opt));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFloorPanelPressTest, "SCBridge.FloorPanel.PressDispatches", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFloorPanelPressTest::RunTest(const FString&)
{
    FSCSceneManifest M;
    if (!ReadC2(*this, M)) return false;
    ASCVehicle* V = MakeC2Vehicle(M);
    ON_SCOPE_EXIT { V->RemoveFromRoot(); };
    FSCLift& Lift = *V->Lifts[0];
    FStubDoors Stub;
    ON_SCOPE_EXIT { Stub.Release(); };
    Stub.Bind(Lift);
    int32 LiftIndex;
    FSCFloorPanel& P = V->FloorPanels.AddDefaulted_GetRef();
    P.Lift = 0;
    P.Interior = 0;
    P.EntityId = TEXT("panel");
    P.Layout = FSCFloorPanelLayout::Make(V->FloorPanelOptions(FloorPanelRecord(), LiftIndex), 1.2);
    // The rest view: at the hold, nothing called.
    FSCFloorPanelView View = FSCFloorPanelView::Of(Lift, INDEX_NONE, 0);
    TestEqual(TEXT("current: the hold"), View.Current, 1);
    TestTrue(TEXT("nothing called"), View.Target == INDEX_NONE && View.Requested.IsEmpty() && View.Direction == 0);

    // E on "Exit Ship" (row 2): the panel's RequestSurface through the dispatcher; the carriage is asked for stop 0 (the
    // hatch's Open goes to no door rig here).
    V->PressFloorPanel(0, 2);
    TestEqual(TEXT("one request"), Lift.Queue.Num(), 1);
    if (Lift.Queue.Num() == 1)
    {
        TestEqual(TEXT("stop 0"), Lift.Queue[0].Stop, 0);
        TestEqual(TEXT("from the panel"), Lift.Queue[0].Source, FString(TEXT("panel")));
        TestEqual(TEXT("its interaction"), Lift.Queue[0].Interaction, FName(TEXT("RequestSurface")));
    }
    View = FSCFloorPanelView::Of(Lift, 2, 0);
    TestEqual(TEXT("called: Exit Ship"), View.Requested, TArray<int32>{0});
    // E on "Top Deck" (row 0) asks for stop 2; an invalid row sends nothing.
    Lift.Queue.Reset();
    V->PressFloorPanel(0, 0);
    TestTrue(TEXT("stop 2 from RequestFloor1"), Lift.Queue.Num() == 1 && Lift.Queue[0].Stop == 2 && Lift.Queue[0].Interaction == TEXT("RequestFloor1"));
    TestEqual(TEXT("no row: nothing"), V->PressFloorPanel(0, 5), 0);
    TestEqual(TEXT("still one request"), Lift.Queue.Num(), 1);

    // The view follows the lift: travelling up to the top (no current floor, target 2, up), then at the top.
    bool bSawMoving = false;
    for (double T = 0; T < 40 && !(Lift.State == ESCLiftState::Open && Lift.CurrentStop == 2); T += 1.0 / 30)
    {
        Lift.Tick(1.0 / 30);
        if (Lift.State == ESCLiftState::Moving && !bSawMoving)
        {
            bSawMoving = true;
            View = FSCFloorPanelView::Of(Lift, INDEX_NONE, Lift.Clock);
            TestTrue(TEXT("moving: no current floor, target the top, up"), View.Current == INDEX_NONE && View.Target == 2 && View.Direction == 1);
        }
    }
    TestTrue(TEXT("moved"), bSawMoving);
    View = FSCFloorPanelView::Of(Lift, INDEX_NONE, Lift.Clock);
    TestTrue(TEXT("arrived: current the top, nothing called"), View.Current == 2 && View.Target == INDEX_NONE && View.Requested.IsEmpty() && View.Direction == 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFloorPanelColoursTest, "SCBridge.FloorPanel.Colours", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFloorPanelColoursTest::RunTest(const FString&)
{
    // Visibly synthetic palette values: the roles decide, not the colours.
    FSCLiftPanelStyle Style;
    const TCHAR* Roles[] = {TEXT("Base"), TEXT("Moderate"), TEXT("Accent1"), TEXT("Accent2"), TEXT("Bright"), TEXT("Selected"), TEXT("Background")};
    for (int32 I = 0; I < UE_ARRAY_COUNT(Roles); ++I) Style.Palette.Add(Roles[I], FColor(uint8(10 * (I + 1)), 0, 0, 255));
    Style.LabelColourRole = TEXT("Selected");
    const FSCFloorPanelColours C = FSCFloorPanelColours::From(Style);
    auto Is = [&](const FLinearColor& L, const TCHAR* Role) { return L.Equals(FLinearColor::FromSRGBColor(Style.Palette[Role])); };
    TestTrue(TEXT("from data"), C.bFromData);
    TestTrue(TEXT("rows: the pads' button role"), Is(C.Row, TEXT("Base")));
    TestTrue(TEXT("field"), Is(C.Field, TEXT("Accent2")));
    TestTrue(TEXT("label: the pad label's own role"), Is(C.Label, TEXT("Selected")));
    TestTrue(TEXT("current floor"), Is(C.Current, TEXT("Accent1")));
    TestTrue(TEXT("called floor"), Is(C.Requested, TEXT("Moderate")));
    TestTrue(TEXT("status bar"), Is(C.Status, TEXT("Background")));
    const FSCFloorPanelColours None = FSCFloorPanelColours::From(FSCLiftPanelStyle());
    TestFalse(TEXT("no palette"), None.bFromData);
    TestTrue(TEXT("white on black"), None.Label == FLinearColor::White && None.Field == FLinearColor::Black && None.Row == FLinearColor::Black);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFloorPanelPaintTest, "SCBridge.FloorPanel.FaceKeepsItsImageOverPaint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFloorPanelPaintTest::RunTest(const FString&)
{
    // A blank screen face records a paint slot at import (its authored instance); the viewer then puts its own image on
    // the face. A paint applied afterwards (the remembered paint at load) must not put the authored instance back.
    ASCVehicle* V = NewObject<ASCVehicle>(GetTransientPackage());
    V->AddToRoot();
    ON_SCOPE_EXIT { V->RemoveFromRoot(); };
    UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(V);
    UMaterialInterface* Authored = UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), V);
    UMaterialInterface* Drawn = UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), V);
    V->Materials.Add(Cast<UMaterialInstanceDynamic>(Authored));
    V->Materials.Add(Cast<UMaterialInstanceDynamic>(Drawn));
    C->SetMaterial(0, Authored);
    FSCPaintSlot& Paint = V->PaintSlots.AddDefaulted_GetRef();
    Paint.Component = C;
    Paint.Slot = 0;
    Paint.Default = Authored;
    FSCScreenFace& Face = V->ScreenFaces.AddDefaulted_GetRef();
    Face.Component = C;
    Face.Slot = 0;
    Face.Default = Authored;
    TestTrue(TEXT("set"), V->SetScreenFaceMaterial(0, Drawn));
    V->SetPaint(INDEX_NONE);
    TestEqual(TEXT("the face keeps the drawn image after a paint"), C->GetMaterial(0), Drawn);
    return true;
}

// WP-O1-4b-1b-2: a prepared vehicle's lifts, interaction network and lift panels are saved with its level and made again
// when it loads: the lift's model and where it rests copied from what was saved, its hooks into the vehicle bound again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCSavedLiftsTest, "SCBridge.Prepare.SavedLifts", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCSavedLiftsTest::RunTest(const FString&)
{
    FSCSceneManifest M;
    if (!ReadC2(*this, M)) return false;
    ASCVehicle* V = MakeC2Vehicle(M);
    ASCVehicle* Loaded = nullptr;
    ON_SCOPE_EXIT
    {
        V->RemoveFromRoot();
        if (Loaded) Loaded->RemoveFromRoot();
    };
    USceneComponent* PanelAnchor = NewObject<USceneComponent>(V, TEXT("PanelAnchor"));
    FSCLiftPanel Panel;
    Panel.Interior = 0;
    Panel.EntityId = TEXT("panel");
    Panel.Label = TEXT("Elevator panel");
    Panel.Choices = {Choice(TEXT("RequestSurface"), TEXT("Exit Ship"), true)};
    V->LiftPanels.Add(PanelAnchor, Panel);
    FSCLift& Lift = *V->Lifts[0];
    // Its anchors and gate rig are the vehicle's components (saved with it, remapped to the loaded actor's own).
    Lift.Anchor = NewObject<USceneComponent>(V, TEXT("LiftAnchor"));
    Lift.CarriageAnchor = NewObject<USceneComponent>(V, TEXT("Carriage"));
    Lift.CarriageAnchor->SetupAttachment(Lift.Anchor.Get()); // the carriage rides the moving anchor, a rider the carriage
    USceneComponent* Rider = NewObject<USceneComponent>(V, TEXT("Rider"));
    Rider->SetupAttachment(Lift.CarriageAnchor.Get());
    // The actor's components, as the import adds them (a level saves an actor's instance components).
    for (UActorComponent* Part : TArray<UActorComponent*>{Lift.Anchor.Get(), Lift.CarriageAnchor.Get(), Rider, PanelAnchor}) V->AddInstanceComponent(Part);
    USCRigComponent* GateRig = NewObject<USCRigComponent>(V, TEXT("Gate"));
    V->AddInstanceComponent(GateRig);
    Lift.Gate.Add({TEXT("gate"), GateRig, 0.5f, 0.0});
    Lift.CurrentStop = 2; // resting at the top deck
    Lift.S = Lift.Stops[2].S;

    // A level save (PreSave: SaveLifts) and a load of the copy (DuplicateObject: its components remapped, PostLoad run:
    // RestoreLifts); the native lift list is not copied, so the loaded actor's lifts come from what was saved.
    FObjectSaveContextData SaveData;
    V->PreSave(FObjectPreSaveContext(SaveData));
    Loaded = DuplicateObject(V, GetTransientPackage());
    Loaded->AddToRoot();
    if (!TestEqual(TEXT("one lift made again at load"), Loaded->Lifts.Num(), 1)) return false;
    const FSCLift& Made = *Loaded->Lifts[0];
    TestTrue(TEXT("its anchors and gate are the loaded actor's components"),
             Made.Anchor.IsValid() && Made.Anchor->GetOuter() == Loaded && Made.Anchor.Get() != Lift.Anchor.Get() &&
                 Made.CarriageAnchor.IsValid() && Made.CarriageAnchor->GetOuter() == Loaded && Made.Gate.Num() == 1 &&
                 Made.Gate[0].Rig.IsValid() && Made.Gate[0].Rig->GetOuter() == Loaded);
    TestTrue(TEXT("its model"), Made.EntityId == Lift.EntityId && Made.Stops.Num() == 3 && Made.Stops[2].Name == TEXT("Top Deck") &&
                                    Made.Stops[1].S == Lift.Stops[1].S && Made.MaxSpeed == Lift.MaxSpeed && Made.RestStop == Lift.RestStop &&
                                    Made.Gate.Num() == 1 && Made.Gate[0].Speed == 0.5f && Made.Id == 0 && Made.Interior == 0);
    TestTrue(TEXT("where it rests, idle"), Made.CurrentStop == 2 && Made.S == Lift.Stops[2].S && Made.State == ESCLiftState::Idle && Made.Queue.IsEmpty());
    TestTrue(TEXT("its hooks bound"), bool(Made.Host.PlayDoor) && bool(Made.Host.Emit) && bool(Made.Host.SweepLeg) && bool(Made.Host.GroundContact));
    const FSCInteractionNetwork::FEndpoint* PanelLinks = Loaded->Interactions.Find(0, TEXT("panel"));
    const FSCInteractionNetwork::FEndpoint* Carriage = Loaded->Interactions.Find(0, TEXT("platform"));
    TestTrue(TEXT("the interaction network"), PanelLinks && PanelLinks->Links.Num() == 4 && PanelLinks->Links[3].TargetEntityId == TEXT("hatch") &&
                                                  Carriage && Carriage->Lift == 0 && Loaded->Interactions.Route(0, TEXT("panel"), TEXT("RequestSurface")).Num() == 2);
    USceneComponent* LoadedPanelAnchor = FindObject<USceneComponent>(Loaded, TEXT("PanelAnchor"));
    const FSCLiftPanel* SavedPanel = LoadedPanelAnchor ? Loaded->LiftPanels.Find(LoadedPanelAnchor) : nullptr;
    TestTrue(TEXT("the lift panel"), SavedPanel && SavedPanel->EntityId == TEXT("panel") && SavedPanel->Choices.Num() == 1 &&
                                         SavedPanel->Choices[0].DisplayName == TEXT("Exit Ship"));
    TestEqual(TEXT("a second restore makes nothing"), Loaded->RestoreLifts(), 0);

    // The loaded carriage still rides the lift's anchor and its rider the carriage; driven to the exit stop (doors stubbed,
    // no gateway), the anchor moves by the legs' displacement, the carriage and rider with it.
    const USceneComponent* LoadedRider = FindObject<USceneComponent>(Loaded, TEXT("Rider"));
    TestTrue(FString::Printf(TEXT("the carriage rides the anchor (%s under %s)"), *GetPathNameSafe(Made.CarriageAnchor.Get()),
                             *GetPathNameSafe(Made.CarriageAnchor->GetAttachParent())), Made.CarriageAnchor->GetAttachParent() == Made.Anchor.Get());
    TestTrue(FString::Printf(TEXT("the rider rides the carriage (%s under %s)"), *GetPathNameSafe(LoadedRider),
                             *GetPathNameSafe(LoadedRider ? LoadedRider->GetAttachParent() : nullptr)),
             LoadedRider && LoadedRider->GetAttachParent() == Made.CarriageAnchor.Get());
    FSCLift& Drive = *Loaded->Lifts[0];
    FStubDoors Doors;
    Doors.Doors.Add(Drive.Gate[0].Rig.Get(), {false, -1, 0.5});
    Doors.Names.Add(Drive.Gate[0].Rig.Get(), TEXT("gate"));
    Doors.Bind(Drive);
    Drive.Host.Emit = [](const FString&, FName) {};
    Drive.Host.RedeliverDoors = [](const FString&, FName) { return 0; };
    Drive.Host.SweepLeg = {};
    Drive.Host.GroundContact = {};
    TestTrue(TEXT("the loaded lift takes a request"), Drive.RequestStop(0, TEXT("test"), TEXT("RequestSurface")));
    Run(Drive, 40);
    const FVector Expected = Drive.AnchorBase + Drive.Stops[0].Local - Drive.Stops[Drive.RestStop].Local;
    TestTrue(FString::Printf(TEXT("arrived at the exit stop with its anchor moved (%s, expected %s)"), *Drive.Anchor->GetRelativeLocation().ToCompactString(),
                             *Expected.ToCompactString()),
             Drive.CurrentStop == 0 && Drive.Anchor->GetRelativeLocation().Equals(Expected, 0.5) &&
                 !Drive.Stops[0].Local.Equals(Drive.Stops[Drive.RestStop].Local, 1.0));

    // A lift saved between stops (in motion: no current stop), powered down, with a door it found no clip for: it rests at
    // the stop nearest where it was, keeps its power state and does not wait for that door.
    FSCSavedLift Moving = Lift.Save();
    Moving.CurrentStop = INDEX_NONE;
    Moving.S = Lift.Stops[1].S + 0.5; // just past the lower deck
    Moving.State = ESCLiftState::Moving;
    Moving.bPoweredUp = false;
    USCRigComponent* Stuck = NewObject<USCRigComponent>(V);
    Moving.Inert.Add(Stuck);
    FSCLift Again;
    Again.Restore(Moving);
    TestTrue(TEXT("saved in motion: rests at its nearest stop, idle"), Again.CurrentStop == 1 && Again.S == Lift.Stops[1].S && Again.State == ESCLiftState::Idle);
    const FSCSavedLift Resaved = Again.Save();
    TestTrue(TEXT("its power state and the door without a clip kept"), !Resaved.bPoweredUp && Resaved.Inert.Num() == 1 && Resaved.Inert[0].Get() == Stuck);
    return true;
}
#endif
