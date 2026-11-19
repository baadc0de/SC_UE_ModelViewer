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

class USceneComponent;
class USCRigComponent;

/** Speed phases of a leg (the carriage class's effect triggers RampUp / FullSpeed / RampDown). */
enum class ESCLiftSpeedPhase : uint8 { None, RampUp, FullSpeed, RampDown };
/** Lift logic states (WP-P5 §5). */
enum class ESCLiftState : uint8 { PoweredDown, Idle, Closing, WaitingForGateway, Moving, Opening, Open };
/** A door's state as the lift sees it. */
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
struct FSCLiftDoor
{
    FString EntityId;
    TWeakObjectPtr<USCRigComponent> Rig;
    float Speed = 1.f;
    double ZCm = 0; // container frame (gateway doors: where the leg must pass it)
};
struct FSCLiftStop
{
    int32 Index = 0;
    FString NodeId, NodeName;
    FString Name;          // the floor-panel interaction routed to the stop ("Top Deck"), else the index
    FVector Local = FVector::ZeroVector; // UE container-local cm
    double S = 0;          // metres along the path from stop 0
    double ZCm = 0;        // container frame (CryEngine z, cm)
    TArray<FSCLiftDoor> LandingDoors;
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
    int32 StopOfNode(const FString& NodeId) const;
    /** Inputs (WP-P5 §6). Sources are the sending entity and interaction, for the events and logs. */
    bool RequestStop(int32 Stop, const FString& Source, FName Interaction);
    bool Call(int32 Stop, int32 Direction, const FString& Source, FName Interaction);
    bool OpenDoors();
    bool CloseDoors();
    bool CancelStop(int32 Stop);
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

/** The exported interaction links of a vehicle and what each entity is (a door rig, a lift carriage, a lift's stop). */
struct SCBRIDGE_API FSCInteractionNetwork
{
    struct FEndpoint
    {
        TArray<FSCInteractionLink> Links;
        TWeakObjectPtr<USCRigComponent> DoorRig;
        float DoorSpeed = 1.f;
        int32 Lift = INDEX_NONE; // carriage of lift
        int32 LiftOfNode = INDEX_NONE, Stop = INDEX_NONE;
        FString Name;
    };
    struct FDelivery
    {
        int32 Interior = INDEX_NONE;
        FString Source, Target;
        FName SourceSlot, TargetSlot;
    };
    TArray<TMap<FString, FEndpoint>> Interiors;
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
