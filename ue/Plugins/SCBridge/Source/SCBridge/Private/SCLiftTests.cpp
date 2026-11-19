// SCBridge.Lift.* (WP-P5): the C2 Hercules lift's motion profile under the constant-acceleration reading, its request queue
// and door handshake against stub doors, the stop order read from a package, and the link dispatcher on the C2's links.
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
#endif
