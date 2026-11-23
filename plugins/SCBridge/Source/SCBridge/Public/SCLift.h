// WP-P5 lifts: a carriage that moves its anchor (and everything riding it) between the stops of a package `lifts[]`
// record, with the doors it opens and closes, and the interaction network that routes button presses to it.
//
// - FSCLiftProfile: the speed profile of one leg (a reading of MaxSpeed / EasingDistance, see Make).
// - FSCLift: model (stops, doors, anchor), logic (request queue, door handshake, gateway wait) and events.
// - FSCInteractionNetwork: the exported interaction links of one vehicle, by interior; Route gives the deliveries of one
//   (entity, interaction), ASCVehicle::SendInteraction executes them.
#pragma once
#include "CoreMinimal.h"
#include "SCInteractionRecords.h"
#include "SCLift.generated.h"

class USceneComponent;
class USCRigComponent;

/** Speed phases of a leg (the carriage class's effect triggers RampUp / FullSpeed / RampDown). */
UENUM()
enum class ESCLiftSpeedPhase : uint8 { None, RampUp, FullSpeed, RampDown };
/** Lift logic states (WP-P5 §5). */
UENUM()
enum class ESCLiftState : uint8 { PoweredDown, Idle, Closing, WaitingForGateway, Moving, Opening, Open };
/** A door's state as the lift sees it. */
UENUM()
enum class ESCLiftDoorState : uint8 { Closed, Opening, Open, Closing };
SCBRIDGE_API const TCHAR* LexToString(ESCLiftState State);
SCBRIDGE_API const TCHAR* LexToString(ESCLiftSpeedPhase Phase);
SCBRIDGE_API const TCHAR* LexToString(ESCLiftDoorState State);

/** One leg's motion. Reading (WP-P5 Q1, not verified against the game): constant acceleration over EasingDistance up to
 *  MaxSpeed, cruise, the mirror deceleration; a = MaxSpeed^2 / (2 EasingDistance). A leg shorter than 2 EasingDistance is a
 *  triangle that peaks below MaxSpeed. Units: metres, seconds. */
struct SCBRIDGE_API FSCLiftProfile
{
    double Distance = 0, MaxSpeed = 0, Accel = 0;
    double RampTime = 0, CruiseTime = 0, PeakSpeed = 0;
    static FSCLiftProfile Make(double Distance, double MaxSpeed, double EasingDistance);
    double Duration() const { return 2 * RampTime + CruiseTime; }
    double PositionAt(double T) const;
    double VelocityAt(double T) const;
    ESCLiftSpeedPhase PhaseAt(double T) const;
};

/** Where one leg ends and how it moves. Rule (WP-P5 ground stop): a leg toward a stop ends early where the carriage first
 *  meets a blocking surface on its way (ContactTravel: metres along the leg from StartS), and the profile is made for that
 *  shortened distance, so the carriage still ramps down to rest on the surface. Without a contact inside the leg (unset,
 *  negative, or at or beyond the stop) the authored stop ends it. The game's data names no ground stop (SCItemElevatorParams
 *  `Collision` is null on every lift class of LIVE 4.10.2): this is the viewer's reading, see WP-P5 "Ground stop". */
struct SCBRIDGE_API FSCLiftLegPlan
{
    double StartS = 0, EndS = 0, AuthoredDistance = 0, Distance = 0;
    bool bContact = false;
    FSCLiftProfile Profile;
    static FSCLiftLegPlan Make(double StartS, double StopS, TOptional<double> ContactTravel, double MaxSpeed, double EasingDistance);
};

/** A door the lift drives: its rig and the clip rate (door timing `animation_speed`). */
USTRUCT()
struct FSCLiftDoor
{
    GENERATED_BODY()
    UPROPERTY() FString EntityId;
    UPROPERTY() TWeakObjectPtr<USCRigComponent> Rig;
    UPROPERTY() float Speed = 1.f;
    UPROPERTY() double ZCm = 0; // container frame (gateway doors: where the leg must pass it)
};
USTRUCT()
struct FSCLiftStop
{
    GENERATED_BODY()
    UPROPERTY() int32 Index = 0;
    UPROPERTY() FString NodeId;
    UPROPERTY() FString NodeName;
    UPROPERTY() FString Name;          // the floor-panel interaction routed to the stop ("Top Deck"), else the index
    UPROPERTY() FVector Local = FVector::ZeroVector; // UE container-local cm
    UPROPERTY() double S = 0;          // metres along the path from stop 0
    UPROPERTY() double ZCm = 0;        // container frame (CryEngine z, cm)
    UPROPERTY() TArray<FSCLiftDoor> LandingDoors;
};
/** A lift as a prepared level saves it (WP-O1-4b-1b-2): its model (FSCLift's data, its anchors among the vehicle's saved
 *  components) and where it rests. ASCVehicle saves one per lift and makes the lifts again from them when it loads: the
 *  model copied, the hooks into the vehicle bound again; nothing is derived. */
USTRUCT()
struct FSCSavedLift
{
    GENERATED_BODY()
    UPROPERTY() FString EntityId;
    UPROPERTY() FString EntityName;
    UPROPERTY() FString ClassName;
    UPROPERTY() int32 Interior = INDEX_NONE;
    UPROPERTY() int32 Id = INDEX_NONE;
    UPROPERTY() float MaxSpeed = 0;
    UPROPERTY() float EasingDistance = 0;
    UPROPERTY() float OpenWaitTime = 0;
    UPROPERTY() float QueueWaitTime = 0;
    UPROPERTY() float IdlingPeriod = 0;
    UPROPERTY() FName OpenInnerDoor;
    UPROPERTY() FName OpenInnerDoorFinished;
    UPROPERTY() FName CloseAllDoors;
    UPROPERTY() FName CloseInnerDoorFinished;
    UPROPERTY() TArray<FSCLiftStop> Stops;
    UPROPERTY() TArray<FSCLiftDoor> Gate;
    UPROPERTY() TArray<FSCLiftDoor> GatewayDoors;
    UPROPERTY() TArray<FString> Riders;
    UPROPERTY() int32 RestStop = 0;
    UPROPERTY() FSCLiftPanelStyle PanelStyle;
    UPROPERTY() TWeakObjectPtr<USceneComponent> Anchor;
    UPROPERTY() FVector AnchorBase = FVector::ZeroVector;
    UPROPERTY() TWeakObjectPtr<USceneComponent> CarriageAnchor;
    /** Where it rests: its stop, path position and state, whether it is powered up, and the doors it found no clip for
     *  (not waited for). The prep step saves lifts at rest (FSCLift::IsIdle; USCCookLibrary refuses one that is not):
     *  Restore's handling of another state (idle at the nearest stop) is a fallback, not a saved motion. */
    UPROPERTY() ESCLiftState State = ESCLiftState::Idle;
    UPROPERTY() int32 CurrentStop = 0;
    UPROPERTY() double S = 0;
    UPROPERTY() bool bAtContact = false;
    UPROPERTY() bool bPoweredUp = true;
    UPROPERTY() TArray<TWeakObjectPtr<USCRigComponent>> Inert;
    UPROPERTY() int32 Direction = 0; // the last leg's (ChooseTarget prefers stops ahead)
    UPROPERTY() double IdleSeconds = 0; // how long it had idled (its power-down comes after IdlingPeriod)
};
struct FSCLiftRequest
{
    int32 Stop = INDEX_NONE;
    FString Source;
    FName Interaction;
    double Time = 0;
};

/** Hooks into the vehicle the lift lives in (stubs in tests). */
struct FSCLiftHost
{
    /** Play a door's Open (true) / Close clip at Speed; false when the door has no clip. */
    TFunction<bool(USCRigComponent*, bool bOpen, float Speed)> PlayDoor;
    /** The door's last played state is Open. */
    TFunction<bool(const USCRigComponent*)> IsDoorOpen;
    /** The door's clip is still playing. */
    TFunction<bool(const USCRigComponent*)> IsDoorMoving;
    /** Fire an interaction of an entity: its exported interaction links are followed. */
    TFunction<void(const FString& EntityId, FName Interaction)> Emit;
    /** What stands in the leg From -> To of the carriage (a sweep of its collision; WP-P5 section 5 step 3). Unset (tests):
     *  the gateway doors in the leg's span decide. */
    enum class ELeg : uint8 { Clear, Wait, Blocked };
    TFunction<ELeg(int32 From, int32 To, FString& OutBy)> SweepLeg;
    /** Deliver again the door deliveries (open verbs) of a request's source interaction; returns the doors started. */
    TFunction<int32(const FString& Source, FName Interaction)> RedeliverDoors;
    /** The first blocking surface under the carriage on its leg from where it stands toward stop To (world collision; the
     *  vehicle itself and the carriage's riders excluded): the travel to it in metres along the leg, and what it is. False:
     *  nothing in the way, or not a leg downward. Unset (tests): the authored stops end every leg. */
    TFunction<bool(int32 To, double& OutTravel, FString& OutBy)> GroundContact;
};

class SCBRIDGE_API FSCLift
{
public:
    FString EntityId, EntityName, ClassName;
    int32 Interior = INDEX_NONE, Id = INDEX_NONE;
    float MaxSpeed = 0, EasingDistance = 0, OpenWaitTime = 0, QueueWaitTime = 0, IdlingPeriod = 0;
    FName OpenInnerDoor, OpenInnerDoorFinished, CloseAllDoors, CloseInnerDoorFinished;
    TArray<FSCLiftStop> Stops;
    TArray<FSCLiftDoor> Gate;        // riders that are doors (the carriage gate)
    TArray<FSCLiftDoor> GatewayDoors; // doors a link opens for the lift (the belly hatch)
    TArray<FString> Riders;
    int32 RestStop = 0;
    /** The door pads' look for a floor panel the viewer draws (`lifts[].panel_style`; empty before cache format 35). */
    FSCLiftPanelStyle PanelStyle;
    /** Moves with the carriage: everything riding it is attached below. Container-local base at RestStop. */
    TWeakObjectPtr<USceneComponent> Anchor;
    FVector AnchorBase = FVector::ZeroVector;
    /** The carriage placement's own anchor (its colliders bound the leg sweep). */
    TWeakObjectPtr<USceneComponent> CarriageAnchor;
    FSCLiftHost Host;

    // State (readable at any time).
    ESCLiftState State = ESCLiftState::Idle;
    ESCLiftSpeedPhase SpeedPhase = ESCLiftSpeedPhase::None;
    int32 CurrentStop = 0, TargetStop = INDEX_NONE, FromStop = INDEX_NONE;
    double S = 0, V = 0;
    /** The carriage stands short of CurrentStop, on the surface its last leg met (S is then not the stop's S). */
    bool bAtContact = false;
    TArray<FSCLiftRequest> Queue;
    double Clock = 0;
    /** The clock when the current (or last) leg started, and where it ends (metres along the path): the leg of a rider. */
    double LegStartedAt = -1;
    double LegEndPosition() const { return LegEndS; }

    // Events (WP-I1 subscribes).
    DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnRequested, int32 /*Stop*/, const FString& /*Source*/, FName /*Interaction*/);
    DECLARE_MULTICAST_DELEGATE_OneParam(FOnStop, int32 /*Stop*/);
    DECLARE_MULTICAST_DELEGATE_TwoParams(FOnDoors, int32 /*Stop*/, ESCLiftDoorState);
    DECLARE_MULTICAST_DELEGATE_OneParam(FOnPhase, FName /*Start|RampUp|FullSpeed|RampDown|Stop|PowerUp|PowerDown*/);
    FOnRequested OnRequested;
    FOnStop OnRequestCleared, OnDeparted, OnPassed, OnArrived;
    FOnDoors OnDoors;
    FOnPhase OnPhase;

    /** Build from a package record. StopLocal: a stop's container-local metres -> UE container-local cm. */
    void Init(const FSCLiftRecord& Record, TFunctionRef<FVector(const FVector&)> StopLocal);
    /** The lift as a prepared level saves it, and made again from that (its hooks and events are the caller's). */
    FSCSavedLift Save() const;
    void Restore(const FSCSavedLift& Saved);
    int32 StopOfNode(const FString& NodeId) const;
    /** Inputs (WP-P5 §6). Sources are the sending entity and interaction, for the events and logs. */
    bool RequestStop(int32 Stop, const FString& Source, FName Interaction);
    bool Call(int32 Stop, int32 Direction, const FString& Source, FName Interaction);
    bool OpenDoors();
    bool CloseDoors();
    bool CancelStop(int32 Stop);
    /** After the vehicle's ResetPose put every door at its rest state: a lift whose doors were opening or open at its stop
     *  drives the gate and the stop's landing doors open again (its Opening state waits for them to settle open). */
    void ReopenDoorsAfterReset();
    /** A target slot delivered to the carriage entity, or to path node Stop. False: not an input of the lift. */
    bool ReceiveCarriage(FName Slot, const FString& Source, FName SourceSlot);
    bool ReceiveNode(int32 Stop, FName Slot, const FString& Source, FName SourceSlot);
    void Tick(double DeltaSeconds);
    /** Nothing to do: no request queued, not moving, no door in motion (doors may stand open). */
    bool IsIdle() const;
    /** Whether the vehicle must keep ticking for this lift. */
    bool NeedsTick() const;
    ESCLiftDoorState DoorState(int32 Stop) const;
    /** Container-local UE position of the carriage at path position Metres. */
    FVector LocalAt(double Metres) const;
    /** Container-frame height (CryEngine z, cm) of the carriage at path position Metres. */
    double ZAt(double Metres) const;
    FString Describe() const;
    /** The phase names the lift emits (effect triggers). */
    static FName PhaseName(ESCLiftSpeedPhase Phase);

private:
    FSCLiftProfile Leg;
    double LegStartS = 0, LegEndS = 0; // the leg runs from LegStartS to LegEndS (the stop, or the surface it meets first)
    bool bLegContact = false;
    /** Doors whose last Open/Close found no clip: they cannot move, so they are not waited for. */
    TSet<TWeakObjectPtr<const USCRigComponent>> Inert;
    double LegTime = 0, StateTime = 0, OpenedAt = -1, IdleSince = 0;
    bool bPoweredUp = true;
    int32 NextPass = INDEX_NONE;
    void SetState(ESCLiftState To);
    void Enqueue(int32 Stop, const FString& Source, FName Interaction);
    int32 ChooseTarget() const;
    void BeginLeg(int32 To);
    void Arrive();
    void MoveTo(double Metres);
    bool DoorsAt(int32 Stop, bool bOpen); // start the gate and the stop's landing doors
    bool DoorsSettled(int32 Stop, bool bOpen) const;
    bool DoorsOpenAt(int32 Stop, FString& OutStuck) const;
    int32 GatewayRetry = INDEX_NONE; // the target whose requests' gateway links were delivered again
    void Phase(FName Name);
    int32 Direction = 0;
};

/** An entity of the interaction network: its links and what it is (a door rig, a lift carriage, a lift's stop). */
USTRUCT()
struct SCBRIDGE_API FSCInteractionEndpoint
{
    GENERATED_BODY()
    UPROPERTY() TArray<FSCInteractionLink> Links;
    UPROPERTY() TWeakObjectPtr<USCRigComponent> DoorRig;
    UPROPERTY() float DoorSpeed = 1.f;
    UPROPERTY() int32 Lift = INDEX_NONE; // carriage of lift
    UPROPERTY() int32 LiftOfNode = INDEX_NONE;
    UPROPERTY() int32 Stop = INDEX_NONE;
    UPROPERTY() FString Name;
};
/** One interior's entities of the interaction network, by entity id. */
USTRUCT()
struct SCBRIDGE_API FSCInteractionInterior
{
    GENERATED_BODY()
    UPROPERTY() TMap<FString, FSCInteractionEndpoint> Entities;
};
/** One delivery of a routed interaction. */
struct FSCInteractionDelivery
{
    int32 Interior = INDEX_NONE;
    FString Source, Target;
    FName SourceSlot, TargetSlot;
};
/** The exported interaction links of a vehicle and what each entity is (a door rig, a lift carriage, a lift's stop).
 *  Saved with a prepared level (WP-O1-4b-1b-2). */
USTRUCT()
struct SCBRIDGE_API FSCInteractionNetwork
{
    GENERATED_BODY()
    using FEndpoint = FSCInteractionEndpoint;
    using FDelivery = FSCInteractionDelivery;
    UPROPERTY() TArray<FSCInteractionInterior> Interiors;
    FEndpoint& At(int32 Interior, const FString& EntityId);
    const FEndpoint* Find(int32 Interior, const FString& EntityId) const;
    /** The interior an entity id belongs to (the first that has it), INDEX_NONE without. */
    int32 InteriorOf(const FString& EntityId) const;
    /** The deliveries of SourceEntityId's interaction: its links whose source slot is Interaction, in authored order. */
    TArray<FDelivery> Route(int32 Interior, const FString& SourceEntityId, FName Interaction) const;
    /** Door verbs a door target accepts (WP-P5 §6). Open: true opens, false closes; Toggle sets bToggle. */
    static bool DoorVerb(FName Slot, bool& bOpen, bool& bToggle);
    /** `From Elevator call <N>` / `Request descent from <N>` / `Request ascent from <N>` -> N and direction (0 none). */
    static bool StopSlot(FName Slot, int32& Stop, int32& Direction);
};
