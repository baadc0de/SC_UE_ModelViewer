// FSCLift: carriage motion, request queue and door handshake (WP-P5); FSCInteractionNetwork: link routing.
#include "SCLift.h"
#include "SCRigComponent.h"
#include "Components/SceneComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCLift, Log, All);

const TCHAR* LexToString(ESCLiftState State)
{
    switch (State)
    {
    case ESCLiftState::PoweredDown: return TEXT("PoweredDown");
    case ESCLiftState::Idle: return TEXT("Idle");
    case ESCLiftState::Closing: return TEXT("Closing");
    case ESCLiftState::WaitingForGateway: return TEXT("WaitingForGateway");
    case ESCLiftState::Moving: return TEXT("Moving");
    case ESCLiftState::Opening: return TEXT("Opening");
    case ESCLiftState::Open: return TEXT("Open");
    }
    return TEXT("?");
}
const TCHAR* LexToString(ESCLiftSpeedPhase Phase)
{
    switch (Phase)
    {
    case ESCLiftSpeedPhase::None: return TEXT("None");
    case ESCLiftSpeedPhase::RampUp: return TEXT("RampUp");
    case ESCLiftSpeedPhase::FullSpeed: return TEXT("FullSpeed");
    case ESCLiftSpeedPhase::RampDown: return TEXT("RampDown");
    }
    return TEXT("?");
}
const TCHAR* LexToString(ESCLiftDoorState State)
{
    switch (State)
    {
    case ESCLiftDoorState::Closed: return TEXT("Closed");
    case ESCLiftDoorState::Opening: return TEXT("Opening");
    case ESCLiftDoorState::Open: return TEXT("Open");
    case ESCLiftDoorState::Closing: return TEXT("Closing");
    }
    return TEXT("?");
}

// ------------------------------------------------------------------------------------------------------------- profile
FSCLiftProfile FSCLiftProfile::Make(double Distance, double MaxSpeed, double EasingDistance)
{
    // The reading of the data (WP-P5 Q1): the carriage ramps up over EasingDistance to MaxSpeed with constant acceleration,
    // cruises, and ramps down the same way. A zero EasingDistance is an instant step to MaxSpeed.
    FSCLiftProfile P;
    P.Distance = FMath::Max(0.0, Distance);
    P.MaxSpeed = MaxSpeed;
    if (P.Distance <= 0 || MaxSpeed <= 0) return P;
    if (EasingDistance <= 0)
    {
        P.Accel = 0;
        P.CruiseTime = P.Distance / MaxSpeed;
        P.PeakSpeed = MaxSpeed;
        return P;
    }
    P.Accel = MaxSpeed * MaxSpeed / (2 * EasingDistance);
    if (P.Distance >= 2 * EasingDistance)
    {
        P.RampTime = MaxSpeed / P.Accel;
        P.CruiseTime = (P.Distance - 2 * EasingDistance) / MaxSpeed;
        P.PeakSpeed = MaxSpeed;
    }
    else // triangle: half the leg accelerating, half decelerating
    {
        P.RampTime = FMath::Sqrt(P.Distance / P.Accel);
        P.PeakSpeed = P.Accel * P.RampTime;
    }
    return P;
}
double FSCLiftProfile::PositionAt(double T) const
{
    T = FMath::Clamp(T, 0.0, Duration());
    if (Accel <= 0) return FMath::Min(Distance, T * MaxSpeed);
    const double Ramp = 0.5 * Accel * RampTime * RampTime;
    if (T <= RampTime) return 0.5 * Accel * T * T;
    if (T <= RampTime + CruiseTime) return Ramp + PeakSpeed * (T - RampTime);
    const double Down = Duration() - T;
    return Distance - 0.5 * Accel * Down * Down;
}
double FSCLiftProfile::VelocityAt(double T) const
{
    if (T <= 0 || T >= Duration()) return 0;
    if (Accel <= 0) return MaxSpeed;
    if (T <= RampTime) return Accel * T;
    if (T <= RampTime + CruiseTime) return PeakSpeed;
    return Accel * (Duration() - T);
}
ESCLiftSpeedPhase FSCLiftProfile::PhaseAt(double T) const
{
    if (T < 0 || T >= Duration()) return ESCLiftSpeedPhase::None;
    if (T < RampTime) return ESCLiftSpeedPhase::RampUp;
    if (T < RampTime + CruiseTime) return ESCLiftSpeedPhase::FullSpeed;
    return ESCLiftSpeedPhase::RampDown;
}

FSCLiftLegPlan FSCLiftLegPlan::Make(double StartS, double StopS, TOptional<double> ContactTravel, double MaxSpeed, double EasingDistance)
{
    FSCLiftLegPlan P;
    P.StartS = StartS;
    P.EndS = StopS;
    P.AuthoredDistance = P.Distance = FMath::Abs(StopS - StartS);
    // A contact counts when it lies inside the leg: from where the carriage stands up to (not at) the authored stop.
    constexpr double Tolerance = 1e-4; // metres
    if (ContactTravel.IsSet() && *ContactTravel >= 0 && *ContactTravel < P.AuthoredDistance - Tolerance)
    {
        P.bContact = true;
        P.Distance = *ContactTravel;
        P.EndS = StartS + (StopS >= StartS ? 1.0 : -1.0) * P.Distance;
    }
    P.Profile = FSCLiftProfile::Make(P.Distance, MaxSpeed, EasingDistance);
    return P;
}

// ---------------------------------------------------------------------------------------------------------------- lift
FName FSCLift::PhaseName(ESCLiftSpeedPhase Phase) { return FName(LexToString(Phase)); }

void FSCLift::Init(const FSCLiftRecord& Record, TFunctionRef<FVector(const FVector&)> StopLocal)
{
    EntityId = Record.EntityId;
    EntityName = Record.EntityName;
    ClassName = Record.EntityClass;
    PanelStyle = Record.PanelStyle;
    MaxSpeed = Record.MaxSpeed;
    EasingDistance = FMath::Max(0.f, Record.EasingDistance);
    OpenWaitTime = FMath::Max(0.f, Record.OpenWaitTime);
    QueueWaitTime = FMath::Max(0.f, Record.QueueWaitTime);
    IdlingPeriod = Record.IdlingPeriod;
    OpenInnerDoor = Record.OpenInnerDoorInteraction;
    OpenInnerDoorFinished = Record.OpenInnerDoorFinishedInteraction;
    CloseAllDoors = Record.CloseAllDoorsInteraction;
    CloseInnerDoorFinished = Record.CloseInnerDoorFinishedInteraction;
    Riders = Record.Riders;
    Stops.Reset();
    double Along = 0;
    for (int32 I = 0; I < Record.Stops.Num(); ++I)
    {
        const FSCLiftStopRecord& R = Record.Stops[I];
        if (I > 0) Along += FVector::Dist(Record.Stops[I - 1].Position, R.Position);
        FSCLiftStop& Stop = Stops.AddDefaulted_GetRef();
        Stop.Index = I;
        Stop.NodeId = R.NodeEntityId;
        Stop.NodeName = R.NodeEntityName;
        Stop.Name = FString::FromInt(I);
        Stop.Local = StopLocal(R.Position);
        Stop.S = Along;
        Stop.ZCm = R.Position.Z * 100.0;
    }
    RestStop = FMath::Max(0, StopOfNode(Record.RestNodeId));
    CurrentStop = RestStop;
    S = Stops.IsValidIndex(RestStop) ? Stops[RestStop].S : 0;
    State = ESCLiftState::Idle;
    UE_LOG(LogSCLift, Display,
           TEXT("LIFT_PROFILE lift=%d reading=constant_acceleration a=MaxSpeed^2/(2*EasingDistance) max_speed=%.3f easing_m=%.3f accel=%.4f ")
           TEXT("(WP-P5 Q1: the ramp's shape is engine code; this reading is not verified against the game)"),
           Id, MaxSpeed, EasingDistance, EasingDistance > 0 ? MaxSpeed * MaxSpeed / (2 * EasingDistance) : 0.0);
}
int32 FSCLift::StopOfNode(const FString& NodeId) const
{
    return Stops.IndexOfByPredicate([&](const FSCLiftStop& S) { return S.NodeId == NodeId; });
}
FVector FSCLift::LocalAt(double Metres) const
{
    if (Stops.IsEmpty()) return FVector::ZeroVector;
    if (Metres <= Stops[0].S) return Stops[0].Local;
    for (int32 I = 1; I < Stops.Num(); ++I)
        if (Metres <= Stops[I].S)
        {
            const double Span = Stops[I].S - Stops[I - 1].S;
            return FMath::Lerp(Stops[I - 1].Local, Stops[I].Local, Span > 0 ? (Metres - Stops[I - 1].S) / Span : 1.0);
        }
    return Stops.Last().Local;
}
double FSCLift::ZAt(double Metres) const
{
    if (Stops.IsEmpty()) return 0;
    if (Metres <= Stops[0].S) return Stops[0].ZCm;
    for (int32 I = 1; I < Stops.Num(); ++I)
        if (Metres <= Stops[I].S)
        {
            const double Span = Stops[I].S - Stops[I - 1].S;
            return FMath::Lerp(Stops[I - 1].ZCm, Stops[I].ZCm, Span > 0 ? (Metres - Stops[I - 1].S) / Span : 1.0);
        }
    return Stops.Last().ZCm;
}
void FSCLift::MoveTo(double Metres)
{
    S = Metres;
    if (USceneComponent* A = Anchor.Get(); A && Stops.IsValidIndex(RestStop))
        A->SetRelativeLocation(AnchorBase + LocalAt(Metres) - Stops[RestStop].Local, false, nullptr, ETeleportType::None);
}
void FSCLift::SetState(ESCLiftState To)
{
    if (State == To) return;
    UE_LOG(LogSCLift, Display, TEXT("LIFT_STATE lift=%d from=%s to=%s stop=%d target=%d t=%.3f"), Id, LexToString(State), LexToString(To),
           CurrentStop, TargetStop, Clock);
    State = To;
    StateTime = 0;
}
void FSCLift::Phase(FName Name)
{
    UE_LOG(LogSCLift, Display, TEXT("LIFT_PHASE lift=%d phase=%s t=%.3f s_m=%.3f v=%.3f"), Id, *Name.ToString(), Clock, S, V);
    OnPhase.Broadcast(Name);
}
void FSCLift::Enqueue(int32 Stop, const FString& Source, FName Interaction)
{
    UE_LOG(LogSCLift, Display, TEXT("LIFT_REQUESTED lift=%d stop=%d name=\"%s\" source=%s interaction=\"%s\" state=%s at=%d t=%.3f"), Id, Stop,
           *Stops[Stop].Name, *Source, *Interaction.ToString(), LexToString(State), CurrentStop, Clock);
    OnRequested.Broadcast(Stop, Source, Interaction);
    if (!Queue.ContainsByPredicate([&](const FSCLiftRequest& R) { return R.Stop == Stop; }))
        Queue.Add({Stop, Source, Interaction, Clock});
    IdleSince = Clock;
}
bool FSCLift::RequestStop(int32 Stop, const FString& Source, FName Interaction)
{
    if (!Stops.IsValidIndex(Stop)) return false;
    Enqueue(Stop, Source, Interaction);
    return true;
}
bool FSCLift::Call(int32 Stop, int32 InDirection, const FString& Source, FName Interaction)
{
    // The direction of a call is recorded by the source interaction; the queue serves stops (WP-P5 §5 step 1).
    return RequestStop(Stop, Source, Interaction);
}
bool FSCLift::CancelStop(int32 Stop)
{
    const int32 Removed = Queue.RemoveAll([&](const FSCLiftRequest& R) { return R.Stop == Stop; });
    if (Removed) OnRequestCleared.Broadcast(Stop);
    // A leg chosen for the cancelled stop and not yet started is not travelled (the doors are closed: Idle serves the rest).
    if (Stop == TargetStop && State == ESCLiftState::WaitingForGateway)
    {
        TargetStop = INDEX_NONE;
        SetState(ESCLiftState::Idle);
    }
    return Removed > 0;
}
bool FSCLift::DoorsAt(int32 Stop, bool bOpen)
{
    if (!Stops.IsValidIndex(Stop)) return false;
    bool bAny = false;
    auto Drive = [&](const FSCLiftDoor& Door, const TCHAR* Role) {
        USCRigComponent* Rig = Door.Rig.Get();
        if (!Rig) return;
        if (Host.IsDoorOpen && Host.IsDoorOpen(Rig) == bOpen && !(Host.IsDoorMoving && Host.IsDoorMoving(Rig))) return;
        const bool bPlayed = Host.PlayDoor && Host.PlayDoor(Rig, bOpen, Door.Speed);
        bAny |= bPlayed;
        if (bPlayed) Inert.Remove(Rig); else Inert.Add(Rig);
        UE_LOG(LogSCLift, Display, TEXT("LIFT_DOOR lift=%d stop=%d door=%s role=%s %s done=0 played=%d speed=%.2f t=%.3f"), Id, Stop, *Door.EntityId, Role,
               bOpen ? TEXT("open") : TEXT("close"), bPlayed, Door.Speed, Clock);
    };
    for (const FSCLiftDoor& Door : Gate) Drive(Door, TEXT("gate"));
    for (const FSCLiftDoor& Door : Stops[Stop].LandingDoors) Drive(Door, TEXT("landing"));
    return bAny;
}
bool FSCLift::DoorsSettled(int32 Stop, bool bOpen) const
{
    if (!Stops.IsValidIndex(Stop)) return true;
    auto Settled = [&](const FSCLiftDoor& Door) {
        const USCRigComponent* Rig = Door.Rig.Get();
        if (!Rig || Inert.Contains(Rig)) return true; // a door without the clip never moves (logged played=0)
        if (Host.IsDoorMoving && Host.IsDoorMoving(Rig)) return false;
        return !Host.IsDoorOpen || Host.IsDoorOpen(Rig) == bOpen || !Host.PlayDoor;
    };
    for (const FSCLiftDoor& Door : Gate) if (!Settled(Door)) return false;
    for (const FSCLiftDoor& Door : Stops[Stop].LandingDoors) if (!Settled(Door)) return false;
    return true;
}
ESCLiftDoorState FSCLift::DoorState(int32 Stop) const
{
    if (Stop != CurrentStop) return ESCLiftDoorState::Closed;
    switch (State)
    {
    case ESCLiftState::Opening: return ESCLiftDoorState::Opening;
    case ESCLiftState::Open: return ESCLiftDoorState::Open;
    case ESCLiftState::Closing: return ESCLiftDoorState::Closing;
    default: return ESCLiftDoorState::Closed;
    }
}
bool FSCLift::OpenDoors()
{
    // Nothing opens between stops, nor while the doors close: a door clip started anew plays from its far end (the
    // closing door would jump open). A request at this stop waits in the queue and is served once they have closed.
    if (!Stops.IsValidIndex(CurrentStop) || State == ESCLiftState::Opening || State == ESCLiftState::Open || State == ESCLiftState::Moving ||
        State == ESCLiftState::WaitingForGateway || State == ESCLiftState::Closing)
        return false;
    DoorsAt(CurrentStop, true);
    SetState(ESCLiftState::Opening);
    OnDoors.Broadcast(CurrentStop, ESCLiftDoorState::Opening);
    if (Host.Emit)
    {
        if (!OpenInnerDoor.IsNone()) Host.Emit(EntityId, OpenInnerDoor);
        Host.Emit(Stops[CurrentStop].NodeId, TEXT("DoorOpen"));
    }
    return true;
}
bool FSCLift::CloseDoors()
{
    if (!Stops.IsValidIndex(CurrentStop) || (State != ESCLiftState::Open && State != ESCLiftState::Opening)) return false;
    DoorsAt(CurrentStop, false);
    SetState(ESCLiftState::Closing);
    OnDoors.Broadcast(CurrentStop, ESCLiftDoorState::Closing);
    if (Host.Emit)
    {
        if (!CloseAllDoors.IsNone()) Host.Emit(EntityId, CloseAllDoors);
        Host.Emit(Stops[CurrentStop].NodeId, TEXT("DoorClose"));
    }
    return true;
}
int32 FSCLift::ChooseTarget() const
{
    // The nearest requested stop in the direction of travel, else the nearest one (WP-P5 §5 step 1, a reading).
    const double From = S;
    int32 Best = INDEX_NONE, BestAhead = INDEX_NONE;
    double BestD = TNumericLimits<double>::Max(), BestAheadD = BestD;
    for (const FSCLiftRequest& R : Queue)
    {
        if (R.Stop == CurrentStop) continue;
        const double D = Stops[R.Stop].S - From;
        if (FMath::Abs(D) < BestD) { BestD = FMath::Abs(D); Best = R.Stop; }
        if (Direction != 0 && D * Direction > 0 && FMath::Abs(D) < BestAheadD) { BestAheadD = FMath::Abs(D); BestAhead = R.Stop; }
    }
    return BestAhead != INDEX_NONE ? BestAhead : Best;
}
void FSCLift::BeginLeg(int32 To)
{
    GatewayRetry = INDEX_NONE;
    FromStop = CurrentStop;
    TargetStop = To;
    // The leg starts where the carriage stands (short of its stop when it rests on a surface it met) and ends at the stop,
    // or on the first blocking surface under the carriage on the way (WP-P5 ground stop).
    TOptional<double> Contact;
    FString ContactBy;
    if (double Travel = 0; Host.GroundContact && Host.GroundContact(To, Travel, ContactBy)) Contact = Travel;
    const FSCLiftLegPlan Plan = FSCLiftLegPlan::Make(S, Stops[To].S, Contact, MaxSpeed, EasingDistance);
    Direction = Stops[To].S > S ? 1 : -1;
    Leg = Plan.Profile;
    LegStartS = Plan.StartS;
    LegEndS = Plan.EndS;
    bLegContact = Plan.bContact;
    bAtContact = false;
    LegTime = 0;
    NextPass = FromStop + Direction;
    LegStartedAt = Clock;
    const double Distance = Plan.Distance;
    UE_LOG(LogSCLift, Display, TEXT("LIFT_DEPARTED lift=%d from=%d to=%d dist_m=%.3f predicted_s=%.3f peak_v=%.3f t=%.3f"), Id, FromStop, To, Distance,
           Leg.Duration(), Leg.PeakSpeed, Clock);
    if (Plan.bContact)
        UE_LOG(LogSCLift, Display,
               TEXT("LIFT_GROUND_CONTACT lift=%d stop=%d authored_z=%.1f contact_z=%.1f authored_dist_m=%.3f dist_m=%.3f authored_s=%.3f predicted_s=%.3f by=%s t=%.3f"),
               Id, To, Stops[To].ZCm, ZAt(Plan.EndS), Plan.AuthoredDistance, Plan.Distance,
               FSCLiftProfile::Make(Plan.AuthoredDistance, MaxSpeed, EasingDistance).Duration(), Leg.Duration(), *ContactBy, Clock);
    OnDeparted.Broadcast(FromStop);
    CurrentStop = INDEX_NONE;
    SetState(ESCLiftState::Moving);
    Phase(TEXT("Start"));
}
void FSCLift::Arrive()
{
    const double Distance = Leg.Distance;
    CurrentStop = TargetStop;
    TargetStop = INDEX_NONE;
    MoveTo(LegEndS); // the stop, or the surface the leg met (arrival all the same: doors, events, logs)
    bAtContact = bLegContact;
    V = 0;
    SpeedPhase = ESCLiftSpeedPhase::None;
    UE_LOG(LogSCLift, Display, TEXT("LIFT_ARRIVED lift=%d stop=%d name=\"%s\" t_leg=%.3f predicted_s=%.3f dist_m=%.3f z_cm=%.1f contact=%d t=%.3f"), Id,
           CurrentStop, *Stops[CurrentStop].Name, LegTime, Leg.Duration(), Distance, ZAt(S), bAtContact, Clock);
    Phase(TEXT("Stop"));
    SetState(ESCLiftState::Idle);
    OnArrived.Broadcast(CurrentStop);
    if (Host.Emit) Host.Emit(EntityId, FName(*FString::Printf(TEXT("Arrived at %d"), CurrentStop)));
    CancelStop(CurrentStop);
    IdleSince = Clock;
    OpenDoors();
}
void FSCLift::Tick(double DeltaSeconds)
{
    if (DeltaSeconds <= 0 || Stops.Num() < 2) return;
    Clock += DeltaSeconds;
    StateTime += DeltaSeconds;
    switch (State)
    {
    case ESCLiftState::PoweredDown:
    case ESCLiftState::Idle:
    case ESCLiftState::Open:
    {
        if (!Queue.IsEmpty() && !bPoweredUp) { bPoweredUp = true; Phase(TEXT("PowerUp")); }
        if (Queue.ContainsByPredicate([&](const FSCLiftRequest& R) { return R.Stop == CurrentStop; }))
        {
            // Called where it stands: only the doors open (cleared at once when they already are).
            CancelStop(CurrentStop);
            if (State != ESCLiftState::Open) OpenDoors();
            break;
        }
        if (Queue.IsEmpty())
        {
            if (bPoweredUp && IdlingPeriod > 0 && Clock - IdleSince >= IdlingPeriod)
            {
                bPoweredUp = false; // an event only: the doors stay as they are
                Phase(TEXT("PowerDown"));
                if (State == ESCLiftState::Idle) SetState(ESCLiftState::PoweredDown);
            }
            break;
        }
        if (State == ESCLiftState::Open)
        {
            // Doors open: the dwell (OpenWaitTime since they opened) and the batching wait (QueueWaitTime since the first
            // request) both pass before the doors close (WP-P5 Q2, a reading).
            if (Clock - OpenedAt < OpenWaitTime || Clock - Queue[0].Time < QueueWaitTime) break;
            CloseDoors();
            break;
        }
        TargetStop = ChooseTarget();
        if (TargetStop != INDEX_NONE) SetState(ESCLiftState::WaitingForGateway);
        break;
    }
    case ESCLiftState::Closing:
    {
        if (!DoorsSettled(CurrentStop, false)) break;
        // Never leave with a door open: one that stayed open (no Close clip) blocks the leg, and the lift stands open.
        FString StuckOpen;
        auto Check = [&](const FSCLiftDoor& Door) {
            if (const USCRigComponent* Rig = Door.Rig.Get(); Rig && Host.IsDoorOpen && Host.IsDoorOpen(Rig)) StuckOpen = Door.EntityId;
        };
        for (const FSCLiftDoor& Door : Gate) Check(Door);
        for (const FSCLiftDoor& Door : Stops[CurrentStop].LandingDoors) Check(Door);
        if (!StuckOpen.IsEmpty())
        {
            UE_LOG(LogSCLift, Display, TEXT("LIFT_BLOCKED lift=%d by=%s reason=door_did_not_close stop=%d t=%.3f"), Id, *StuckOpen, CurrentStop, Clock);
            while (!Queue.IsEmpty()) CancelStop(Queue.Last().Stop);
            OpenedAt = Clock;
            SetState(ESCLiftState::Open);
            break;
        }
        UE_LOG(LogSCLift, Display, TEXT("LIFT_DOOR lift=%d stop=%d close done=1 t=%.3f"), Id, CurrentStop, Clock);
        OnDoors.Broadcast(CurrentStop, ESCLiftDoorState::Closed);
        if (Host.Emit && !CloseInnerDoorFinished.IsNone()) Host.Emit(EntityId, CloseInnerDoorFinished);
        TargetStop = ChooseTarget();
        SetState(TargetStop == INDEX_NONE ? ESCLiftState::Idle : ESCLiftState::WaitingForGateway);
        break;
    }
    case ESCLiftState::WaitingForGateway:
    {
        if (!Stops.IsValidIndex(TargetStop)) { SetState(ESCLiftState::Idle); break; }
        auto Block = [&](const FString& By, const TCHAR* Reason) {
            UE_LOG(LogSCLift, Display, TEXT("LIFT_BLOCKED lift=%d by=%s reason=%s from=%d to=%d t=%.3f"), Id, *By, Reason, CurrentStop, TargetStop, Clock);
            const int32 Blocked = TargetStop;
            TargetStop = INDEX_NONE;
            CancelStop(Blocked);
            SetState(ESCLiftState::Idle);
        };
        // Never leave with the gate or the stop's landing door open (a door opened apart from the lift's own handshake).
        FString StuckOpen;
        if (DoorsOpenAt(CurrentStop, StuckOpen))
        {
            if (!StuckOpen.IsEmpty()) { Block(StuckOpen, TEXT("door_did_not_close")); break; }
            if (StateTime <= DeltaSeconds + UE_SMALL_NUMBER) DoorsAt(CurrentStop, false);
            break;
        }
        // What stands in the leg: the carriage's collision swept along it (the host), else the gateway doors in its span
        // (WP-P5 section 5 step 3). A door on the way that is moving is waited for; a closed one is reopened once by the
        // links of the requests for the target (the ground call opened the hatch, `Arrived at 2` closed it since); anything
        // else blocks the leg.
        FString By;
        FSCLiftHost::ELeg Way = FSCLiftHost::ELeg::Clear;
        if (Host.SweepLeg)
            Way = Host.SweepLeg(CurrentStop, TargetStop, By);
        else
        {
            const double Lo = FMath::Min(Stops[CurrentStop].ZCm, Stops[TargetStop].ZCm), Hi = FMath::Max(Stops[CurrentStop].ZCm, Stops[TargetStop].ZCm);
            for (const FSCLiftDoor& Door : GatewayDoors)
            {
                const USCRigComponent* Rig = Door.Rig.Get();
                if (!Rig || Door.ZCm <= Lo || Door.ZCm >= Hi) continue;
                if (Host.IsDoorMoving && Host.IsDoorMoving(Rig)) { Way = FSCLiftHost::ELeg::Wait; continue; }
                if (Host.IsDoorOpen && !Host.IsDoorOpen(Rig)) { Way = FSCLiftHost::ELeg::Blocked; By = Door.EntityId; break; }
            }
        }
        if (Way == FSCLiftHost::ELeg::Wait) break;
        if (Way == FSCLiftHost::ELeg::Blocked)
        {
            int32 Reopened = 0;
            if (GatewayRetry != TargetStop && Host.RedeliverDoors)
                for (const FSCLiftRequest& R : Queue)
                    if (R.Stop == TargetStop) Reopened += Host.RedeliverDoors(R.Source, R.Interaction);
            GatewayRetry = TargetStop;
            if (Reopened)
            {
                UE_LOG(LogSCLift, Display, TEXT("LIFT_GATEWAY lift=%d reopened=%d by_request_links=1 blocked_by=%s to=%d t=%.3f"), Id, Reopened, *By,
                       TargetStop, Clock);
                break;
            }
            Block(By, TEXT("in_the_way"));
            break;
        }
        BeginLeg(TargetStop);
        break;
    }
    case ESCLiftState::Moving:
    {
        LegTime += DeltaSeconds;
        const double T = FMath::Min(LegTime, Leg.Duration());
        V = Direction * Leg.VelocityAt(T);
        const ESCLiftSpeedPhase P = Leg.PhaseAt(T);
        if (P != SpeedPhase && P != ESCLiftSpeedPhase::None)
        {
            SpeedPhase = P;
            Phase(PhaseName(P));
        }
        MoveTo(LegStartS + Direction * Leg.PositionAt(T));
        while (Stops.IsValidIndex(NextPass) && NextPass != TargetStop && (S - Stops[NextPass].S) * Direction >= 0)
        {
            UE_LOG(LogSCLift, Display, TEXT("LIFT_PASSED lift=%d stop=%d t=%.3f"), Id, NextPass, Clock);
            OnPassed.Broadcast(NextPass);
            NextPass += Direction;
        }
        if (LegTime >= Leg.Duration()) Arrive();
        break;
    }
    case ESCLiftState::Opening:
        if (!DoorsSettled(CurrentStop, true)) break;
        UE_LOG(LogSCLift, Display, TEXT("LIFT_DOOR lift=%d stop=%d open done=1 t=%.3f"), Id, CurrentStop, Clock);
        OnDoors.Broadcast(CurrentStop, ESCLiftDoorState::Open);
        if (Host.Emit && !OpenInnerDoorFinished.IsNone()) Host.Emit(EntityId, OpenInnerDoorFinished);
        OpenedAt = Clock;
        IdleSince = Clock;
        SetState(ESCLiftState::Open);
        break;
    }
}
bool FSCLift::IsIdle() const
{
    const bool bRest = State == ESCLiftState::Idle || State == ESCLiftState::Open || State == ESCLiftState::PoweredDown;
    // Its gateway doors too: the C2's hatch closes for 6.67 s after the carriage arrived at the hold.
    for (const FSCLiftDoor& Door : GatewayDoors)
        if (const USCRigComponent* Rig = Door.Rig.Get(); Rig && Host.IsDoorMoving && Host.IsDoorMoving(Rig)) return false;
    return bRest && Queue.IsEmpty() && (!Stops.IsValidIndex(CurrentStop) || DoorsSettled(CurrentStop, State == ESCLiftState::Open));
}
bool FSCLift::DoorsOpenAt(int32 Stop, FString& OutStuck) const
{
    // True while the gate or the stop's landing door is open or moving; OutStuck names an open one that cannot close.
    OutStuck.Reset();
    bool bAny = false;
    auto Check = [&](const FSCLiftDoor& Door) {
        const USCRigComponent* Rig = Door.Rig.Get();
        if (!Rig) return;
        const bool bMoving = Host.IsDoorMoving && Host.IsDoorMoving(Rig);
        const bool bOpen = Host.IsDoorOpen && Host.IsDoorOpen(Rig);
        if (!bMoving && !bOpen) return;
        bAny = true;
        if (bOpen && !bMoving && Inert.Contains(Rig)) OutStuck = Door.EntityId;
    };
    if (!Stops.IsValidIndex(Stop)) return false;
    for (const FSCLiftDoor& Door : Gate) Check(Door);
    for (const FSCLiftDoor& Door : Stops[Stop].LandingDoors) Check(Door);
    return bAny;
}
bool FSCLift::NeedsTick() const
{
    return !IsIdle() || (bPoweredUp && IdlingPeriod > 0);
}
bool FSCLift::ReceiveCarriage(FName Slot, const FString& Source, FName SourceSlot)
{
    int32 Stop, Dir;
    if (FSCInteractionNetwork::StopSlot(Slot, Stop, Dir)) return Dir == 0 ? RequestStop(Stop, Source, SourceSlot) : Call(Stop, Dir, Source, SourceSlot);
    if (Slot == OpenInnerDoor) return OpenDoors() || true;
    if (Slot == CloseAllDoors) return CloseDoors() || true;
    return false;
}
bool FSCLift::ReceiveNode(int32 Stop, FName Slot, const FString& Source, FName SourceSlot)
{
    if (!Stops.IsValidIndex(Stop)) return false;
    static const FName Ascent(TEXT("AscentRequest")), Descent(TEXT("DescentRequest")), Open(TEXT("DoorOpen")), Close(TEXT("DoorClose")), Cancel(TEXT("CancelStop"));
    if (Slot == Ascent || Slot == Descent) return Call(Stop, Slot == Ascent ? 1 : -1, Source, SourceSlot);
    if (Slot == Open || Slot == Close)
    {
        // The landing door at a stop opens and closes with the carriage there (WP-P5 §6); elsewhere the input is ignored.
        if (Stop != CurrentStop)
        {
            UE_LOG(LogSCLift, Display, TEXT("LIFT_IGNORED lift=%d stop=%d slot=%s source=%s reason=carriage_at_%d"), Id, Stop, *Slot.ToString(), *Source,
                   CurrentStop);
            return true;
        }
        if (Slot == Open) OpenDoors(); else CloseDoors();
        return true;
    }
    if (Slot == Cancel) { CancelStop(Stop); return true; }
    return false;
}
FString FSCLift::Describe() const
{
    TArray<FString> Q;
    for (const FSCLiftRequest& R : Queue) Q.Add(FString::FromInt(R.Stop));
    return FString::Printf(TEXT("lift=%d s_m=%.3f z_cm=%.1f v=%.3f state=%s phase=%s at=%d target=%d queue=[%s] contact=%d"), Id, S, ZAt(S), V,
                           LexToString(State), LexToString(SpeedPhase), CurrentStop, TargetStop, *FString::Join(Q, TEXT(",")), bAtContact);
}

// ---------------------------------------------------------------------------------------------------------- network
FSCInteractionNetwork::FEndpoint& FSCInteractionNetwork::At(int32 Interior, const FString& EntityId)
{
    if (!Interiors.IsValidIndex(Interior)) Interiors.SetNum(Interior + 1);
    return Interiors[Interior].FindOrAdd(EntityId);
}
const FSCInteractionNetwork::FEndpoint* FSCInteractionNetwork::Find(int32 Interior, const FString& EntityId) const
{
    return Interiors.IsValidIndex(Interior) ? Interiors[Interior].Find(EntityId) : nullptr;
}
int32 FSCInteractionNetwork::InteriorOf(const FString& EntityId) const
{
    for (int32 I = 0; I < Interiors.Num(); ++I)
        if (Interiors[I].Contains(EntityId)) return I;
    return INDEX_NONE;
}
TArray<FSCInteractionNetwork::FDelivery> FSCInteractionNetwork::Route(int32 Interior, const FString& SourceEntityId, FName Interaction) const
{
    TArray<FDelivery> Out;
    if (const FEndpoint* Source = Find(Interior, SourceEntityId))
        for (const FSCInteractionLink& Link : Source->Links)
            if (Link.SourceSlot == Interaction) Out.Add({Interior, SourceEntityId, Link.TargetEntityId, Interaction, Link.TargetSlot});
    return Out;
}
bool FSCInteractionNetwork::DoorVerb(FName Slot, bool& bOpen, bool& bToggle)
{
    const FString S = Slot.ToString();
    bToggle = S == TEXT("Toggle");
    bOpen = S == TEXT("Open") || S == TEXT("OpenSelf");
    return bToggle || bOpen || S == TEXT("Close") || S == TEXT("CloseSelf");
}
bool FSCInteractionNetwork::StopSlot(FName Slot, int32& Stop, int32& Direction)
{
    const FString S = Slot.ToString();
    for (const auto& [Prefix, Dir] : {TPair<const TCHAR*, int32>(TEXT("From Elevator call "), 0), {TEXT("Request descent from "), -1},
                                      {TEXT("Request ascent from "), 1}})
        if (S.StartsWith(Prefix) && S.Mid(FCString::Strlen(Prefix)).IsNumeric())
        {
            Stop = FCString::Atoi(*S.Mid(FCString::Strlen(Prefix)));
            Direction = Dir;
            return true;
        }
    return false;
}
