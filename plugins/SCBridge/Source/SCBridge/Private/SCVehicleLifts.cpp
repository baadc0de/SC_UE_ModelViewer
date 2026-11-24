// WP-P5: lifts and the interaction-link dispatcher of ASCVehicle, and their assembly at import.
#include "SCVehicleLifts.h"
#include "SCVehicle.h"
#include "SCLift.h"
#include "SCMeshImporter.h"
#include "SCRigComponent.h"
#include "SCLights.h"
#include "SCAudioSystem.h"
#include "Components/LightComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCLiftLinks, Log, All);

namespace
{
constexpr int32 MaxLinkDepth = 8; // a link chain longer than this is a cycle in the data (logged, not followed)
const FName LiftTag(TEXT("SCLift"));

/** The box of the carriage's own colliders (its placement and the riders attached below it) in the vehicle frame Frame:
 *  the components that block the player. */
FBox CarriageBox(const FTransform& Frame, const USceneComponent* Carriage)
{
    FBox Local(ForceInit);
    TArray<USceneComponent*> Parts;
    Carriage->GetChildrenComponents(true, Parts);
    for (const USceneComponent* Part : Parts)
        if (const auto* P = Cast<UPrimitiveComponent>(Part);
            P && P->IsRegistered() && P->IsCollisionEnabled() && P->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block)
            Local += P->CalcBounds(P->GetComponentTransform().GetRelativeTransform(Frame)).GetBox();
    return Local;
}
}

// ----------------------------------------------------------------------------------------------------- dispatcher
int32 ASCVehicle::SendInteraction(const FString& SourceEntityId, FName Interaction)
{
    const int32 Interior = Interactions.InteriorOf(SourceEntityId);
    int32 Holders = 0;
    for (const FSCInteractionInterior& Ids : Interactions.Interiors) Holders += Ids.Entities.Contains(SourceEntityId);
    if (Holders > 1) // ids are unique within one container: a container placed twice repeats them
        UE_LOG(LogSCLiftLinks, Warning, TEXT("LINK_SEND source=%s is in %d interiors; sending in the first (%d). Panels send in their own."),
               *SourceEntityId, Holders, Interior);
    if (Interior == INDEX_NONE)
    {
        UE_LOG(LogSCLiftLinks, Display, TEXT("LINK_SEND source=%s interaction=\"%s\" deliveries=0 reason=unknown_entity"), *SourceEntityId,
               *Interaction.ToString());
        return 0;
    }
    return SendInteractionIn(Interior, SourceEntityId, Interaction);
}
int32 ASCVehicle::SendInteractionIn(int32 Interior, const FString& SourceEntityId, FName Interaction)
{
    // WP-A1: a top-level send is a press of the source entity: its class's Activate trigger effects play (the game's
    // EntityEffectSystem fires that tag on an interaction; nested deliveries are the lift's own, not presses).
    if (SendDepth == 0)
        if (USCAudioSubsystem* Audio = USCAudioSubsystem::Get(this)) Audio->FireEntityTag(this, Interior, SourceEntityId, TEXT("Activate"));
    const int32 Depth = SendDepth++;
    ON_SCOPE_EXIT { --SendDepth; };
    if (Depth > MaxLinkDepth)
    {
        UE_LOG(LogSCLiftLinks, Warning, TEXT("LINK_SEND source=%s interaction=\"%s\" depth=%d: link chain too deep (a cycle), not followed"),
               *SourceEntityId, *Interaction.ToString(), Depth);
        return 0;
    }
    int32 Handled = 0;
    const TArray<FSCInteractionNetwork::FDelivery> Deliveries = Interactions.Route(Interior, SourceEntityId, Interaction);
    for (const FSCInteractionNetwork::FDelivery& D : Deliveries)
    {
        const FSCInteractionNetwork::FEndpoint* Target = Interactions.Find(Interior, D.Target);
        bool bHandled = false;
        const TCHAR* Kind = TEXT("unknown");
        if (Target && Lifts.IsValidIndex(Target->Lift))
        {
            Kind = TEXT("carriage");
            bHandled = Lifts[Target->Lift]->ReceiveCarriage(D.TargetSlot, D.Source, D.SourceSlot);
        }
        else if (Target && Lifts.IsValidIndex(Target->LiftOfNode))
        {
            Kind = TEXT("stop");
            bHandled = Lifts[Target->LiftOfNode]->ReceiveNode(Target->Stop, D.TargetSlot, D.Source, D.SourceSlot);
        }
        else if (Target && Target->DoorRig.IsValid())
        {
            Kind = TEXT("door");
            bool bOpen = false, bToggle = false;
            if (FSCInteractionNetwork::DoorVerb(D.TargetSlot, bOpen, bToggle))
            {
                USCRigComponent* Rig = Target->DoorRig.Get();
                if (bToggle) bOpen = !IsRigOpen(Rig);
                bHandled = IsRigOpen(Rig) == bOpen || PlayDoorRig(Rig, bOpen, Target->DoorSpeed);
            }
        }
        UE_LOG(LogSCLiftLinks, Display, TEXT("LINK_DELIVER source=%s slot=\"%s\" target=%s (%s) target_slot=\"%s\" kind=%s handled=%d depth=%d"),
               *D.Source, *D.SourceSlot.ToString(), *D.Target, Target ? *Target->Name : TEXT("-"), *D.TargetSlot.ToString(), Kind, bHandled, Depth);
        Handled += bHandled;
    }
    if (Depth == 0)
        UE_LOG(LogSCLiftLinks, Display, TEXT("LINK_SEND source=%s interaction=\"%s\" deliveries=%d handled=%d"), *SourceEntityId,
               *Interaction.ToString(), Deliveries.Num(), Handled);
    UpdateTickEnabled();
    return Handled;
}
bool ASCVehicle::PlayDoorRig(USCRigComponent* Rig, bool bOpen, float Speed)
{
    const int32 Door = DoorForRig(Rig);
    if (Door == INDEX_NONE) return false;
    // The door's own fragments from its current pose (ASCVehicle::PlayDoor); Speed <= 0 is the door's DoorAnimationSpeed.
    const bool bPlayed = PlayDoor(Door, bOpen, Speed);
    UpdateTickEnabled();
    return bPlayed;
}
bool ASCVehicle::IsRigOpen(const USCRigComponent* Rig) const
{
    const int32 Door = DoorForRig(Rig);
    return Door != INDEX_NONE && IsDoorOpen(Door);
}
bool ASCVehicle::IsRigPlaying(const USCRigComponent* Rig) const
{
    return Rig && Playback.ContainsByPredicate([&](const FSCClipPlayback& P) { return P.TargetRig.Get() == Rig; });
}
const ASCVehicle::FLiftPanel* ASCVehicle::FindLiftPanel(FVector Eye, FVector Direction, float Reach, const AActor* Ignore) const
{
    if (LiftPanels.IsEmpty() || !GetWorld()) return nullptr;
    const FVector End = Eye + Direction.GetSafeNormal() * Reach;
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCLiftPanel), true, Ignore);
    double Nearest = Reach;
    if (GetWorld()->LineTraceSingleByChannel(Hit, Eye, End, ECC_Visibility, Params))
    {
        if (Hit.GetActor() == this)
            for (const USceneComponent* C = Hit.GetComponent(); C; C = C->GetAttachParent())
                if (const FLiftPanel* Panel = LiftPanels.Find(const_cast<USceneComponent*>(C))) return Panel;
        Nearest = Hit.Distance;
    }
    // A panel part without collision (the C2's call and floor buttons have none) is found by the ray against the part's own
    // bounds, when nothing that collides is in front of it.
    const FLiftPanel* Best = nullptr;
    for (const auto& [Anchor, Panel] : LiftPanels)
    {
        const USceneComponent* A = Anchor.Get();
        if (!A) continue;
        TArray<USceneComponent*> Parts;
        A->GetChildrenComponents(true, Parts);
        for (const USceneComponent* Part : Parts)
        {
            const auto* Primitive = Cast<UPrimitiveComponent>(Part);
            if (!Primitive || !Primitive->IsRegistered() || !Primitive->IsVisible() || Primitive->bHiddenInGame) continue;
            const FTransform& World = Primitive->GetComponentTransform();
            const FBox Local = Primitive->CalcBounds(FTransform::Identity).GetBox();
            FVector HitPoint, HitNormal;
            float Time = 0;
            if (Local.IsValid && FMath::LineExtentBoxIntersection(Local, World.InverseTransformPosition(Eye), World.InverseTransformPosition(End),
                                                                  FVector::ZeroVector, HitPoint, HitNormal, Time) &&
                Time * Reach < Nearest)
            {
                Nearest = Time * Reach;
                Best = &Panel;
            }
        }
    }
    return Best;
}
int32 ASCVehicle::RedeliverDoorLinks(int32 Interior, const FString& SourceEntityId, FName Interaction)
{
    // Only the door deliveries of the source's interaction, and only the opening verbs: the stop request itself stays queued.
    int32 Started = 0;
    for (const FSCInteractionNetwork::FDelivery& D : Interactions.Route(Interior, SourceEntityId, Interaction))
    {
        const FSCInteractionNetwork::FEndpoint* Target = Interactions.Find(Interior, D.Target);
        bool bOpen = false, bToggle = false;
        if (!Target || !Target->DoorRig.IsValid() || !FSCInteractionNetwork::DoorVerb(D.TargetSlot, bOpen, bToggle) || !bOpen) continue;
        USCRigComponent* Rig = Target->DoorRig.Get();
        if (IsRigOpen(Rig) || IsRigPlaying(Rig)) continue;
        if (PlayDoorRig(Rig, true, Target->DoorSpeed))
        {
            ++Started;
            UE_LOG(LogSCLiftLinks, Display, TEXT("LINK_REDELIVER source=%s slot=\"%s\" target=%s (%s) target_slot=\"%s\""), *SourceEntityId,
                   *Interaction.ToString(), *D.Target, *Target->Name, *D.TargetSlot.ToString());
        }
    }
    return Started;
}
FSCLiftHost::ELeg ASCVehicle::SweepLiftLeg(int32 LiftIndex, int32 From, int32 To, FString& OutBy)
{
    // The doors in the leg's path: the box of the carriage's own colliders (its placement: platform, rails, pedestal) at the
    // start and at the end of the leg spans the path; a door of the vehicle whose entity origin lies in that path, and not
    // already inside the carriage's box where it stands, is in the way. Closed: the leg is blocked; its clip playing: waited
    // for; open: clear. The lift's own gate and landing doors stop with it and are not obstacles. A reading of the brief's
    // sweep: the box is larger than the carriage and its colliders are trimeshes (no shape sweep), and a door rig's
    // colliders include its fixed parts (the C2 hatch's guide rails run up the shaft), so door origins decide.
    const FSCLift* Lift = Lifts.IsValidIndex(LiftIndex) ? Lifts[LiftIndex].Get() : nullptr;
    const USceneComponent* Anchor = Lift ? Lift->Anchor.Get() : nullptr;
    const USceneComponent* Carriage = Lift ? Lift->CarriageAnchor.Get() : nullptr;
    const USceneComponent* Container = Anchor ? Anchor->GetAttachParent() : nullptr;
    if (!Carriage || !Container || !GetWorld() || !Lift->Stops.IsValidIndex(From) || !Lift->Stops.IsValidIndex(To)) return FSCLiftHost::ELeg::Clear;
    const FTransform Frame = GetActorTransform();
    const FBox Local = CarriageBox(Frame, Carriage);
    if (!Local.IsValid) return FSCLiftHost::ELeg::Clear;
    const FVector Start = Frame.TransformPosition(Local.GetCenter());
    // From where the carriage stands (its stop, or short of it on a surface its last leg met).
    const FVector End = Start + Container->GetComponentTransform().TransformVector(Lift->Stops[To].Local - Lift->LocalAt(Lift->S));
    const FBox Here = Local;
    const FBox There = Local.ShiftBy(Frame.InverseTransformVector(End - Start));
    const FBox Path = Here + There;
    FSCLiftHost::ELeg Result = FSCLiftHost::ELeg::Clear;
    TSet<const USCRigComponent*> Seen;
    for (const FSCDoorInteraction& D : Doors)
    {
        const USCRigComponent* Rig = D.Rig;
        if (!Rig || Seen.Contains(Rig) || IsLiftDriven(Rig) || LiftCarrying(Rig) == LiftIndex) continue;
        Seen.Add(Rig);
        const FVector Origin = Frame.InverseTransformPosition(Rig->GetComponentLocation());
        if (!Path.IsInside(Origin) || Here.IsInside(Origin)) continue;
        if (IsRigPlaying(Rig)) { Result = FSCLiftHost::ELeg::Wait; OutBy = Rig->OwnerEntity; continue; }
        if (IsRigOpen(Rig)) continue;
        OutBy = FString::Printf(TEXT("%s (closed) at %s"), *Rig->OwnerEntity, *Origin.ToCompactString());
        return FSCLiftHost::ELeg::Blocked;
    }
    return Result;
}
int32 ASCVehicle::LiftCarrying(const USceneComponent* C) const
{
    for (; C; C = C->GetAttachParent())
        if (C->ComponentHasTag(LiftTag))
            for (int32 I = 0; I < Lifts.Num(); ++I)
                if (Lifts[I]->Anchor.Get() == C) return I;
    return INDEX_NONE;
}
int32 ASCVehicle::LiftRiddenBy(const FVector& Bottom, const UPrimitiveComponent* Base, double FootReach) const
{
    // The carriage's members are carried with it (its own physics grid, `grid_geometry`, which the viewer does not load). A
    // rider stands on the carriage: its movement base is part of the carriage, or one of the carriage's own colliders lies
    // under its feet within FootReach (a capsule on a ledge 1 cm above the carriage's floor, the C2 hold's slab, is on it;
    // one on a landing beside the shaft, or anywhere the carriage only passes under, is not).
    const FTransform Frame = GetActorTransform();
    const FVector A = Frame.InverseTransformPosition(Bottom);
    for (int32 I = 0; I < Lifts.Num(); ++I)
    {
        const FSCLift& Lift = *Lifts[I];
        const USceneComponent* Carriage = Lift.CarriageAnchor.Get();
        if (Lift.State != ESCLiftState::Moving || !Carriage) continue;
        if (Base && LiftCarrying(Base) == I) return I;
        const FBox Box = CarriageBox(Frame, Carriage);
        if (!Box.IsValid || A.X < Box.Min.X || A.X > Box.Max.X || A.Y < Box.Min.Y || A.Y > Box.Max.Y) continue;
        TArray<USceneComponent*> Parts;
        Carriage->GetChildrenComponents(true, Parts);
        FCollisionQueryParams Params(SCENE_QUERY_STAT(SCLiftRider), true);
        for (const USceneComponent* Part : Parts)
        {
            FHitResult Hit;
            auto* P = const_cast<UPrimitiveComponent*>(Cast<UPrimitiveComponent>(Part));
            if (P && P->IsCollisionEnabled() && P->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block &&
                P->LineTraceComponent(Hit, Bottom + FVector(0, 0, 5), Bottom - FVector(0, 0, FootReach), Params))
                return I;
        }
    }
    return INDEX_NONE;
}
const TArray<TWeakObjectPtr<UPrimitiveComponent>>& ASCVehicle::LiftPathObstacles(int32 LiftIndex)
{
    static const TArray<TWeakObjectPtr<UPrimitiveComponent>> None;
    const FSCLift* Lift = Lifts.IsValidIndex(LiftIndex) ? Lifts[LiftIndex].Get() : nullptr;
    const USceneComponent* Carriage = Lift ? Lift->CarriageAnchor.Get() : nullptr;
    const USceneComponent* Anchor = Lift ? Lift->Anchor.Get() : nullptr;
    const USceneComponent* Container = Anchor ? Anchor->GetAttachParent() : nullptr;
    if (!Carriage || !Container || !GetWorld() || Lift->State != ESCLiftState::Moving) return None;
    FLiftPath& Path = LiftPaths.FindOrAdd(LiftIndex);
    if (Path.Clock == Lift->LegStartedAt && Path.To == Lift->TargetStop) return Path.Obstacles;
    Path = FLiftPath();
    Path.From = Lift->FromStop;
    Path.To = Lift->TargetStop;
    Path.Clock = Lift->LegStartedAt;
    // The leg's swept box: the carriage's colliders where it stands and where the leg ends (vehicle frame), less a skin of
    // a centimetre so the surface a leg ends on (the ground it rests on) is not "passed through".
    const FTransform Frame = GetActorTransform();
    const FBox Here = CarriageBox(Frame, Carriage);
    if (!Here.IsValid) return Path.Obstacles;
    const FVector Leg = Frame.InverseTransformVector(Container->GetComponentTransform().TransformVector(Lift->LocalAt(Lift->LegEndPosition()) - Lift->LocalAt(Lift->S)));
    const FBox Swept = (Here + Here.ShiftBy(Leg)).ExpandBy(-1.0);
    TArray<FOverlapResult> Overlaps;
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    GetWorld()->OverlapMultiByObjectType(Overlaps, Frame.TransformPosition(Swept.GetCenter()), Frame.GetRotation(), Objects,
                                         FCollisionShape::MakeBox(FVector3f(Swept.GetExtent() * Frame.GetScale3D().GetAbs())),
                                         FCollisionQueryParams(SCENE_QUERY_STAT(SCLiftPath), false));
    TArray<FString> Names;
    for (const FOverlapResult& O : Overlaps)
    {
        UPrimitiveComponent* C = O.GetComponent();
        const AActor* Holder = C ? C->GetOwner() : nullptr;
        if (!C || (Holder && Holder->IsA<APawn>()) || C->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block || LiftCarrying(C) == LiftIndex) continue;
        bool bDoor = false; // doors stay what they are: the leg check waits for them or is blocked by them
        for (const USceneComponent* P = C; P && !bDoor; P = P->GetAttachParent())
            if (const auto* Rig = Cast<USCRigComponent>(P)) bDoor = DoorForRig(Rig) != INDEX_NONE || IsLiftDriven(Rig);
        if (bDoor || Path.Obstacles.Contains(C)) continue;
        Path.Obstacles.Add(C);
        Names.Add(C->ComponentTags.IsEmpty() ? C->GetName() : FPaths::GetBaseFilename(C->ComponentTags[0].ToString()));
    }
    UE_LOG(LogSCLiftLinks, Display, TEXT("LIFT_PATH lift=%d from=%d to=%d obstacles=%d passed=%s"), LiftIndex, Path.From, Path.To, Path.Obstacles.Num(),
           Names.IsEmpty() ? TEXT("-") : *FString::Join(Names, TEXT(",")));
    return Path.Obstacles;
}
bool SCVehicleLifts::GroundContact(const ASCVehicle* V, int32 LiftIndex, int32 To, double& OutTravel, FString& OutBy)
{
    // WP-P5 ground stop: the box of the carriage's colliders swept from where it stands toward stop To. The first blocking
    // surface it meets on the way down (what the player stands on: the hangar pad, terrain, cargo) ends the leg; the vehicle
    // itself (hull, gear, the carriage) and pawns (the riders) are not ground, nor is anything the box already overlaps where
    // it stands. Hits are taken in order along the sweep, and only those that face the motion (a wall the box grazes does not).
    const FSCLift* Lift = V && V->Lifts.IsValidIndex(LiftIndex) ? V->Lifts[LiftIndex].Get() : nullptr;
    const USceneComponent* Anchor = Lift ? Lift->Anchor.Get() : nullptr;
    const USceneComponent* Carriage = Lift ? Lift->CarriageAnchor.Get() : nullptr;
    const USceneComponent* Container = Anchor ? Anchor->GetAttachParent() : nullptr;
    UWorld* World = V ? V->GetWorld() : nullptr;
    if (!Carriage || !Container || !World || !Lift->Stops.IsValidIndex(To)) return false;
    const FTransform Frame = V->GetActorTransform();
    const FBox Local = CarriageBox(Frame, Carriage);
    if (!Local.IsValid) return false;
    const FVector Leg = Container->GetComponentTransform().TransformVector(Lift->Stops[To].Local - Lift->LocalAt(Lift->S));
    const double Length = Leg.Size();
    if (Length < 1.0 || Leg.Z >= 0) return false; // the ground is under the carriage: only a leg downward can meet it
    const FVector Dir = Leg / Length;
    const FVector Start = Frame.TransformPosition(Local.GetCenter());
    const FCollisionShape Box = FCollisionShape::MakeBox(FVector3f(Local.GetExtent() * Frame.GetScale3D().GetAbs()));
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCLiftGround), false, V);
    TArray<FHitResult> Hits;
    World->SweepMultiByObjectType(Hits, Start, Start + Leg, Frame.GetRotation(), Objects, Box, Params);
    Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Time < B.Time; });
    for (const FHitResult& H : Hits)
    {
        const UPrimitiveComponent* C = H.GetComponent();
        const AActor* A = H.GetActor();
        if (!C || H.bStartPenetrating || A == V || (A && (A->IsA<APawn>() || A->IsAttachedTo(V))) ||
            C->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block || FVector::DotProduct(H.Normal, Dir) > -0.1)
            continue;
        OutTravel = H.Time * Length / 100.0;
        const double Bottom = Frame.InverseTransformPosition(Start + Leg * H.Time).Z - Local.GetExtent().Z;
        OutBy = FString::Printf(TEXT("%s/%s (%s) carriage_bottom_z_cm=%.1f"), A ? *A->GetName() : TEXT("-"), *C->GetName(),
                                C->IsA<UStaticMeshComponent>() && Cast<UStaticMeshComponent>(C)->GetStaticMesh()
                                    ? *Cast<UStaticMeshComponent>(C)->GetStaticMesh()->GetName()
                                    : *C->GetClass()->GetName(),
                                Bottom);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------- assembly
void SCVehicleLifts::BindLift(ASCVehicle* V, FSCLift& Lift)
{
    TWeakObjectPtr<ASCVehicle> Weak = V;
    Lift.Host.PlayDoor = [Weak](USCRigComponent* Rig, bool bOpen, float Speed) { return Weak.IsValid() && Weak->PlayDoorRig(Rig, bOpen, Speed); };
    Lift.Host.IsDoorOpen = [Weak](const USCRigComponent* Rig) { return Weak.IsValid() && Weak->IsRigOpen(Rig); };
    Lift.Host.IsDoorMoving = [Weak](const USCRigComponent* Rig) { return Weak.IsValid() && Weak->IsRigPlaying(Rig); };
    const int32 LinkInterior = Lift.Interior;
    Lift.Host.Emit = [Weak, LinkInterior](const FString& Entity, FName Interaction) {
        if (Weak.IsValid()) Weak->SendInteractionIn(LinkInterior, Entity, Interaction);
    };
    const int32 LiftId = Lift.Id;
    Lift.Host.RedeliverDoors = [Weak, LinkInterior](const FString& Source, FName Interaction) {
        return Weak.IsValid() ? Weak->RedeliverDoorLinks(LinkInterior, Source, Interaction) : 0;
    };
    Lift.Host.SweepLeg = [Weak, LiftId](int32 From, int32 To, FString& By) {
        return Weak.IsValid() ? Weak->SweepLiftLeg(LiftId, From, To, By) : FSCLiftHost::ELeg::Clear;
    };
    Lift.Host.GroundContact = [Weak, LiftId](int32 To, double& Travel, FString& By) {
        return Weak.IsValid() && SCVehicleLifts::GroundContact(Weak.Get(), LiftId, To, Travel, By);
    };
    // WP-A1: the carriage class's sound effects follow the lift's events: each phase fires the trigger effects of the
    // same tag name (Start, RampUp, FullSpeed, RampDown, Stop, PowerUp, PowerDown), and the state tag InTransit is
    // held from Start to Stop (the brief's mapping of FSCLift's events onto the game's elevator tags).
    Lift.OnPhase.AddLambda([Weak, LinkInterior, Carriage = Lift.EntityId](FName Phase) {
        USCAudioSubsystem* Audio = Weak.IsValid() ? USCAudioSubsystem::Get(Weak.Get()) : nullptr;
        if (!Audio) return;
        Audio->FireEntityTag(Weak.Get(), LinkInterior, Carriage, Phase.ToString());
        if (Phase == TEXT("Start")) Audio->SetEntityTag(Weak.Get(), LinkInterior, Carriage, TEXT("InTransit"), true);
        else if (Phase == TEXT("Stop")) Audio->SetEntityTag(Weak.Get(), LinkInterior, Carriage, TEXT("InTransit"), false);
    });
}
bool ASCVehicle::LiftsAtRest() const
{
    if (!Playback.IsEmpty()) return false; // a clip moving any door (a lift's doors move by clips of any origin)
    for (const TSharedPtr<FSCLift>& Lift : Lifts)
        if (Lift && !Lift->IsIdle()) return false;
    return true;
}
void ASCVehicle::SaveLifts()
{
    // Only lifts at rest are saved consistently (their doors' clips are not saved): otherwise none is, and the load drops
    // their controls (the prep step refuses such a vehicle; another level save keeps the vehicle's lifts out of it).
    SavedLifts.Reset();
    bLiftsOmitted = false;
    if (Lifts.IsEmpty()) return;
    if (!LiftsAtRest())
    {
        bLiftsOmitted = true;
        UE_LOG(LogSCLiftLinks, Warning, TEXT("LIFT_SAVE skipped: the vehicle is not at rest (a lift moving, or a clip playing); its lifts are not saved"));
        return;
    }
    for (const TSharedPtr<FSCLift>& Lift : Lifts)
        if (Lift) SavedLifts.Add(Lift->Save());
}
int32 ASCVehicle::RestoreLifts()
{
    if (!Lifts.IsEmpty()) return 0;
    if (bLiftsOmitted)
    {
        // Saved without its lifts: no controller answers their panels and stops, so those are not offered.
        LiftPanels.Reset();
        for (FSCInteractionInterior& Interior : Interactions.Interiors)
            for (auto& [Id, Endpoint] : Interior.Entities)
                Endpoint.Lift = Endpoint.LiftOfNode = Endpoint.Stop = INDEX_NONE;
        UE_LOG(LogSCLiftLinks, Warning, TEXT("LIFT_RESTORE none: the level was saved with its lifts left out"));
        return 0;
    }
    for (const FSCSavedLift& Saved : SavedLifts)
    {
        TSharedPtr<FSCLift> Lift = MakeShared<FSCLift>();
        Lift->Restore(Saved);
        Lift->Id = Lifts.Num(); // the network's carriage and stop entries index Lifts
        SCVehicleLifts::BindLift(this, *Lift);
        Lifts.Add(MoveTemp(Lift));
    }
    if (!Lifts.IsEmpty())
        UE_LOG(LogSCLiftLinks, Display, TEXT("LIFT_RESTORE lifts=%d panels=%d driven_doors=%d"), Lifts.Num(), LiftPanels.Num(), LiftDrivenRigs.Num());
    return Lifts.Num();
}
TSet<FString> SCVehicleLifts::CarriageIds(const FSCInterior& Interior)
{
    TSet<FString> Out;
    for (const FSCLiftRecord& Lift : Interior.Lifts) Out.Add(Lift.EntityId);
    return Out;
}

void SCVehicleLifts::BuildInteriorLifts(ASCVehicle* V, int32 InteriorIndex, const FSCInterior& Interior, USceneComponent* Container, bool bHelper,
                                        const TMap<FString, FSCLiftBuildPart>& Parts)
{
    if (!V || !Container) return;
    // The network: every entity's links, and which entities are doors.
    for (const FSCSceneInstance& P : Interior.Placements)
    {
        if (P.EntityId.IsEmpty()) continue;
        FSCInteractionNetwork::FEndpoint& E = V->Interactions.At(InteriorIndex, P.EntityId);
        E.Links = P.InteractionLinks;
        E.Name = P.SocEntityName;
        if (const FSCLiftBuildPart* Part = Parts.Find(P.EntityId); Part && Part->Rig && P.Door.bValid)
        {
            E.DoorRig = Part->Rig;
            E.DoorSpeed = P.Door.AnimationSpeed;
        }
    }
    for (const FSCInteriorEntity& Entity : Interior.Entities)
    {
        FSCInteractionNetwork::FEndpoint& E = V->Interactions.At(InteriorIndex, Entity.EntityId);
        E.Links = Entity.InteractionLinks;
        E.Name = Entity.EntityName;
    }
    auto PlacementOf = [&](const FString& Id) { return Interior.Placements.FindByPredicate([&](const FSCSceneInstance& P) { return P.EntityId == Id; }); };
    auto Door = [&](const FString& Id) -> TOptional<FSCLiftDoor> {
        const FSCSceneInstance* P = PlacementOf(Id);
        const FSCLiftBuildPart* Part = Parts.Find(Id);
        if (!P || !Part || !Part->Rig || !P->Door.bValid) return {};
        if (!P->Door.bSpeedKnown || P->Door.AnimationSpeed <= 0)
            UE_LOG(LogSCLiftLinks, Warning, TEXT("LIFT_BUILD door=%s (%s): no usable DoorAnimationSpeed in the data; its clips play at their own rate"),
                   *Id, *P->SocEntityName);
        FSCLiftDoor D;
        D.EntityId = Id;
        D.Rig = Part->Rig;
        D.Speed = P->Door.AnimationSpeed;
        D.ZCm = P->LocalMatrix.M[3][2] * 100.0; // container frame (CryEngine z)
        return D;
    };
    TWeakObjectPtr<ASCVehicle> Weak = V;
    for (const FSCLiftRecord& Record : Interior.Lifts)
    {
        const FSCLiftBuildPart* Carriage = Parts.Find(Record.EntityId);
        if (!Carriage || !Carriage->Anchor)
        {
            UE_LOG(LogSCLiftLinks, Warning, TEXT("LIFT_BUILD entity=%s: the carriage was not placed; lift not operable"), *Record.EntityId);
            continue;
        }
        TSharedPtr<FSCLift> Lift = MakeShared<FSCLift>();
        Lift->Id = V->Lifts.Num();
        Lift->Interior = InteriorIndex;
        // A stop's container-local metres map into the container like a placement's translation does (CryEngine local ->
        // glTF -> UE), so the carriage moves by the stops' differences in the frame its parts are placed in.
        Lift->Init(Record, [&](const FVector& Metres) {
            FMatrix M = FMatrix::Identity;
            M.M[3][0] = Metres.X; M.M[3][1] = Metres.Y; M.M[3][2] = Metres.Z;
            return FSCMeshImporter::InstanceTransform(M, FMatrix::Identity, bHelper, FSCMeshImporter::InCryFrame(TEXT("cryengine_z_up"), bHelper)).GetOrigin();
        });
        // The moving anchor: identity under the container; the carriage and its riders keep their world transforms below it.
        USceneComponent* Anchor = NewObject<USceneComponent>(
            V, MakeUniqueObjectName(V, USceneComponent::StaticClass(), FName(*FString::Printf(TEXT("Lift_%s"), *Record.EntityId))), RF_Transient);
        V->AddInstanceComponent(Anchor);
        Anchor->SetMobility(EComponentMobility::Movable);
        Anchor->SetupAttachment(Container);
        Anchor->ComponentTags.Add(LiftTag);
        Anchor->RegisterComponent();
        Lift->Anchor = Anchor;
        Lift->AnchorBase = FVector::ZeroVector;
        Carriage->Anchor->AttachToComponent(Anchor, FAttachmentTransformRules::KeepWorldTransform);
        // Each rider hangs on what it is attached to (parent_attachment): the carriage or another rider, at the node it
        // names when that parent is a rig with such a socket, keeping its world transform (the authored placement).
        auto AttachRider = [&](USceneComponent* C, const FString& ParentId, const FString& Node) {
            const FSCLiftBuildPart* Parent = Parts.Find(ParentId);
            if (!C || !Parent || !Parent->Anchor) return false;
            if (Parent->Rig && !Node.IsEmpty() && Parent->Rig->DoesSocketExist(FName(*Node)))
                C->AttachToComponent(Parent->Rig, FAttachmentTransformRules::KeepWorldTransform, FName(*Node));
            else
                C->AttachToComponent(Parent->Anchor, FAttachmentTransformRules::KeepWorldTransform);
            return true;
        };
        int32 Moved = 1;
        for (const FString& Rider : Record.Riders)
        {
            const FSCSceneInstance* P = PlacementOf(Rider);
            const FSCLiftBuildPart* Part = Parts.Find(Rider);
            if (P && Part && AttachRider(Part->Anchor, P->Parent.ParentEntityId, P->Parent.AttachmentTarget)) ++Moved;
            if (TOptional<FSCLiftDoor> D = Door(Rider)) Lift->Gate.Add(*D);
        }
        // Lights attached to the carriage or a rider ride with it.
        TSet<FString> Carried{Record.EntityId};
        Carried.Append(Record.Riders);
        int32 LightsMoved = 0;
        TArray<USceneComponent*> ContainerChildren(Container->GetAttachChildren());
        for (USceneComponent* C : ContainerChildren)
            if (ULightComponent* Light = Cast<ULightComponent>(C))
                for (const FName& Tag : Light->ComponentTags)
                    if (FString Rides = Tag.ToString(); Rides.RemoveFromStart(FSCLights::RidesTagPrefix) && Carried.Contains(Rides))
                        LightsMoved += AttachRider(Light, Rides, FString());
        for (int32 S = 0; S < Record.Stops.Num() && S < Lift->Stops.Num(); ++S)
            for (const FString& Id : Record.Stops[S].LandingDoorIds)
                if (TOptional<FSCLiftDoor> D = Door(Id)) Lift->Stops[S].LandingDoors.Add(*D);
                else UE_LOG(LogSCLiftLinks, Warning, TEXT("LIFT_BUILD lift=%d stop=%d landing door %s has no placed door rig: the stop opens without it"),
                            Lift->Id, S, *Id);
        // Run-time links the game builds from a stop's traversal tree reach a vehicle item port the export does not resolve
        // (Q4): reported, not delivered. The explicit links (the C2's panel and buttons to its hatch) carry the ride.
        for (const FSCLiftStopRecord& S : Record.Stops)
            if (const FSCInteriorEntity* Node = Interior.Entities.FindByPredicate([&](const FSCInteriorEntity& E) { return E.EntityId == S.NodeEntityId; }))
                for (const FSCTraversalLink& T : Node->TraversalLinks)
                    UE_LOG(LogSCLiftLinks, Display, TEXT("LIFT_TRAVERSAL lift=%d stop=%d interaction=%s item_port=%s linked=%s resolved=0 reason=item_port_not_exported"),
                           Lift->Id, S.Index, *T.LocalInteraction.ToString(), *T.ItemPortName, *T.LinkedInteraction.ToString());
        // Who operates the lift: the carriage, its riders and every entity with a link to the carriage or a stop.
        TSet<FString> LiftIds{Record.EntityId};
        for (const FSCLiftStopRecord& S : Record.Stops) LiftIds.Add(S.NodeEntityId);
        TSet<FString> Operators{Record.EntityId};
        Operators.Append(Record.Riders);
        auto LinksOf = [&](const FString& Id) -> const TArray<FSCInteractionLink>* {
            const FSCInteractionNetwork::FEndpoint* E = V->Interactions.Find(InteriorIndex, Id);
            return E ? &E->Links : nullptr;
        };
        if (V->Interactions.Interiors.IsValidIndex(InteriorIndex))
            for (const auto& [Id, Endpoint] : V->Interactions.Interiors[InteriorIndex].Entities)
                if (Endpoint.Links.ContainsByPredicate([&](const FSCInteractionLink& L) { return LiftIds.Contains(L.TargetEntityId); })) Operators.Add(Id);
        // Gateway doors: doors a door verb of an operator's link drives (the C2 belly hatch), not a landing door or the gate.
        TSet<FString> Driven;
        for (const FSCLiftDoor& D : Lift->Gate) Driven.Add(D.EntityId);
        for (const FSCLiftStop& S : Lift->Stops)
            for (const FSCLiftDoor& D : S.LandingDoors) Driven.Add(D.EntityId);
        for (const FString& Op : Operators)
            if (const TArray<FSCInteractionLink>* Links = LinksOf(Op))
                for (const FSCInteractionLink& L : *Links)
                {
                    bool bOpen, bToggle;
                    if (!FSCInteractionNetwork::DoorVerb(L.TargetSlot, bOpen, bToggle) || Driven.Contains(L.TargetEntityId)) continue;
                    if (TOptional<FSCLiftDoor> D = Door(L.TargetEntityId))
                    {
                        Lift->GatewayDoors.Add(*D);
                        Driven.Add(L.TargetEntityId);
                    }
                }
        // Stop names: the display name of the interaction whose link asks the carriage for that stop.
        for (const FSCSceneInstance& P : Interior.Placements)
            for (const FSCInteractionLink& L : P.InteractionLinks)
            {
                int32 Stop, Dir;
                if (L.TargetEntityId != Record.EntityId || !FSCInteractionNetwork::StopSlot(L.TargetSlot, Stop, Dir) || Dir != 0 ||
                    !Lift->Stops.IsValidIndex(Stop))
                    continue;
                for (const FSCInteractionPoint& Point : P.InteractionPoints)
                    for (const FSCPointInteraction& I : Point.Interactions)
                        if (I.Name == L.SourceSlot && !I.DisplayName.IsEmpty()) Lift->Stops[Stop].Name = I.DisplayName;
            }
        // The press path: a placed panel whose links reach this lift, with the choices its interaction point offers.
        int32 Panels = 0;
        for (const FSCSceneInstance& P : Interior.Placements)
        {
            const FSCLiftBuildPart* Part = Parts.Find(P.EntityId);
            if (!Part || !Part->Anchor || P.EntityId == Record.EntityId ||
                !P.InteractionLinks.ContainsByPredicate([&](const FSCInteractionLink& L) { return LiftIds.Contains(L.TargetEntityId); }))
                continue;
            ASCVehicle::FLiftPanel Panel;
            Panel.Interior = InteriorIndex;
            Panel.EntityId = P.EntityId;
            Panel.Label = P.SocEntityName;
            for (const FSCInteractionPoint& Point : P.InteractionPoints)
                for (const FSCPointInteraction& I : Point.Interactions)
                    if (P.InteractionLinks.ContainsByPredicate([&](const FSCInteractionLink& L) { return L.SourceSlot == I.Name; })) Panel.Choices.Add(I);
            if (Panel.Choices.IsEmpty()) continue;
            V->LiftPanels.Add(Part->Anchor, MoveTemp(Panel));
            ++Panels;
        }
        for (const FSCLiftDoor& D : Lift->Gate) V->LiftDrivenRigs.Add(const_cast<USCRigComponent*>(D.Rig.Get()));
        for (const FSCLiftStop& S : Lift->Stops)
            for (const FSCLiftDoor& D : S.LandingDoors) V->LiftDrivenRigs.Add(const_cast<USCRigComponent*>(D.Rig.Get()));
        V->Interactions.At(InteriorIndex, Record.EntityId).Lift = Lift->Id;
        for (const FSCLiftStop& S : Lift->Stops)
        {
            FSCInteractionNetwork::FEndpoint& E = V->Interactions.At(InteriorIndex, S.NodeId);
            E.LiftOfNode = Lift->Id;
            E.Stop = S.Index;
        }
        Lift->CarriageAnchor = Carriage->Anchor;
        SCVehicleLifts::BindLift(V, *Lift);
        UE_LOG(LogSCLiftLinks, Display,
               TEXT("LIFT_BUILD lift=%d entity=%s class=%s stops=%d rest=%d riders=%d moved=%d lights=%d gate=%d gateways=%d panels=%d"), Lift->Id,
               *Record.EntityId, *Record.EntityClass, Lift->Stops.Num(), Lift->RestStop, Record.Riders.Num(), Moved, LightsMoved, Lift->Gate.Num(),
               Lift->GatewayDoors.Num(), Panels);
        V->Lifts.Add(MoveTemp(Lift));
    }
    if (!Interior.Lifts.IsEmpty()) V->UpdateTickEnabled();
}
