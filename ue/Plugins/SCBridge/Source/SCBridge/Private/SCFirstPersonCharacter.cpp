// ACharacter owns gravity, step-up, crouch and capsule sweeps; no ship-specific navigation rules.
#include "SCFirstPersonCharacter.h"
#include "SCAudioSystem.h"
#include "SCVehicle.h"
#include "SCPlayerBody.h"
#include "SCLightAreas.h"
#include "SCLightImporter.h"
#include "SCPlayerKit.h"
#include "SCViewerSubsystem.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/InputComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"

// C2 fixes: riders of a moving lift carriage are carried through what its leg passes (1); 0 turns the rule off (A/B, tests).
static TAutoConsoleVariable<int32> CVarSCLiftCarryRiders(TEXT("sc.Lift.CarryRiders"), 1,
    TEXT("1: a capsule on a moving lift carriage is carried through what the leg passes (C2 fixes); 0: swept like any based move"));

namespace
{
constexpr float NoclipRefusalSeconds = 3;   // how long the HUD shows a refused noclip exit
constexpr float BlockedOfferSeconds = 2;    // blocked input this long offers noclip
constexpr float InteractReach = 300;        // E reaches 3 m (brief)
// The viewer's own beam (no game flashlight in the kit): a plain cone, UE's distance law and range fade, no light function.
// Candelas like every imported light: the viewer meters the scene with the game's exposure, under which a unitless 2000
// was invisible in a lit hangar.
void ApplyFallbackTorch(USpotLightComponent* Spot)
{
    using C = ASCFirstPersonCharacter;
    Spot->SetIntensityUnits(ELightUnits::Candelas);
    Spot->SetIntensity(C::TorchCandelas);
    Spot->SetLightColor(FLinearColor::White);
    Spot->SetAttenuationRadius(C::TorchReach);
    Spot->SetSourceRadius(0);
    Spot->SetDistanceAttenuation(ELightDistanceAttenuation::Default);
    Spot->SetRangeFade(ELightRangeFade::Default);
    Spot->SetShape(ESpotLightShape::Cone);
    Spot->SetLightFunctionAddress(ELightFunctionAddress::Wrap);
    Spot->SetLightFunctionMaterial(nullptr);
    Spot->SetInnerConeAngle(C::TorchInnerCone);
    Spot->SetOuterConeAngle(C::TorchOuterCone);
}
}

void USCEyeCameraComponent::GetCameraView(float DeltaTime, FMinimalViewInfo& DesiredView)
{
    Super::GetCameraView(DeltaTime, DesiredView);
    if (NearClip > 0) DesiredView.PerspectiveNearClipPlane = NearClip;
}
ASCFirstPersonCharacter::ASCFirstPersonCharacter()
{
    GetCapsuleComponent()->OnComponentHit.AddDynamic(this, &ASCFirstPersonCharacter::CapsuleHit);
    auto* Movement = GetCharacterMovement();
    Movement->GetNavAgentPropertiesRef().bCanCrouch = true;
    Movement->bRunPhysicsWithNoController = true; // scripted commandlet walks use the same movement component
    // The movement ticks after the character (Explore adds that edge): UE's default (tick before the owner) would make the
    // pair a cycle and the engine would drop one direction (C2 fixes review).
    Movement->bTickBeforeOwner = false;
    Camera = CreateDefaultSubobject<USCEyeCameraComponent>(TEXT("FirstPersonCamera"));
    Body = CreateDefaultSubobject<USCPlayerBodyComponent>(TEXT("PlayerBody"));
    Camera->SetupAttachment(GetCapsuleComponent());
    Camera->bUsePawnControlRotation = true;
    Flashlight = CreateDefaultSubobject<USpotLightComponent>(TEXT("ExplorationFlashlight"));
    Flashlight->SetupAttachment(Camera);
    // Movable like every imported light: a local light defaults to Stationary, which does not follow its moving
    // parent, so the torch stayed where the character spawned.
    Flashlight->SetMobility(EComponentMobility::Movable);
    Flashlight->SetRelativeLocation(FVector(TorchAhead, 0, 0));
    ApplyFallbackTorch(Flashlight); // until a player kit supplies the game's flashlight (SetTorchLight)
    TorchSource = TEXT("fallback");
    Flashlight->SetVisibility(false);
    Flashlight->LightingChannels.bChannel1 = true; // not an area light: it reaches every room channel (WP-U3b §12, WP-L1)
    Flashlight->LightingChannels.bChannel2 = true;
    SetProfile(FSCCharacterProfile());
}
void ASCFirstPersonCharacter::SetProfile(const FSCCharacterProfile& InProfile)
{
    Profile = InProfile;
    auto* Movement = GetCharacterMovement();
    GetCapsuleComponent()->SetCapsuleSize(Profile.CapsuleRadius, bIsCrouched ? Profile.CrouchedHalfHeight : Profile.StandingHalfHeight,
                                          GetCapsuleComponent()->IsRegistered());
    Movement->SetCrouchedHalfHeight(Profile.CrouchedHalfHeight);
    Movement->MaxStepHeight = Profile.StepHeight;
    Movement->MaxFlySpeed = Profile.NoclipSpeed;
    Movement->BrakingDecelerationFlying = Profile.NoclipBraking;
    CrouchedEyeHeight = Profile.CrouchedEyeHeight;
    SetRunning(bRunning);
    RecalculateBaseEyeHeight();
    Camera->SetRelativeLocation(EyeOffset());
}
void ASCFirstPersonCharacter::RecalculateBaseEyeHeight()
{
    BaseEyeHeight = bIsCrouched ? Profile.CrouchedEyeHeight : Profile.EyeHeight;
}
FVector ASCFirstPersonCharacter::EyeOffset() const
{
    return EyeSource ? EyeSource() : FVector(0, 0, BaseEyeHeight);
}
void ASCFirstPersonCharacter::RefreshEye()
{
    if (Camera->GetAttachParent() == GetCapsuleComponent()) Camera->SetRelativeLocation(EyeOffset());
}
void ASCFirstPersonCharacter::SetRunning(bool bRun)
{
    bRunning = bRun;
    GetCharacterMovement()->MaxWalkSpeed = bRun ? Profile.RunSpeed : Profile.WalkSpeed;
}
void ASCFirstPersonCharacter::SetMoveInput(FVector Input) { Move = Input.GetClampedToMaxSize(1); }
void ASCFirstPersonCharacter::ClearInput()
{
    Held.Empty();
    HoldSeconds = 0;
    Move = FVector::ZeroVector;
    SetRunning(false);
    StopJumping();
    UnCrouch();
    Body->SetAiming(false);
}
void ASCFirstPersonCharacter::CapsuleHit(UPrimitiveComponent*, AActor*, UPrimitiveComponent*, FVector, const FHitResult&) { ++CapsuleHits; }
void ASCFirstPersonCharacter::HoldViewOn(const USceneComponent* Frame, const FVector& Local)
{
    ViewHoldFrame = Frame;
    ViewHoldLocal = Local;
    ApplyViewHold();
}
void ASCFirstPersonCharacter::ApplyViewHold()
{
    const USceneComponent* Frame = ViewHoldFrame.Get();
    if (!Frame || !Vehicle) return;
    const FVector Target = Frame->GetComponentTransform().TransformPosition(ViewHoldLocal);
    const FRotator Want = Vehicle->GetActorTransform().InverseTransformVectorNoScale(Target - InteractionOrigin()).Rotation();
    // A quarter of the way each tick: the eye swings with the view (the head bends and the camera, ahead of the capsule's
    // axis, turns with its yaw), so looking steeply down at a close panel a full turn overshoots; the damped turn settles.
    const FRotator Now = GetVehicleRelativeView();
    SetVehicleRelativeView(Now.Yaw + 0.25f * FRotator::NormalizeAxis(Want.Yaw - Now.Yaw), Now.Pitch + 0.25f * FRotator::NormalizeAxis(Want.Pitch - Now.Pitch));
}
void ASCFirstPersonCharacter::Look(FVector2D Pixels)
{
    if (bMobiGlasOpen) return; // the cursor drives the mobiGlas; the view holds still
    if (!Pixels.IsNearlyZero()) ClearViewHold();
    if (bOrbiting && Body->IsThirdPerson())
    {
        Body->AddOrbit(Pixels.X * .2f, -Pixels.Y * .2f);
        return;
    }
    if (Controller)
    {
        FRotator View = Controller->GetControlRotation();
        View.Yaw += Pixels.X * .2f;
        View.Pitch = FMath::Clamp(FRotator::NormalizeAxis(View.Pitch - Pixels.Y * .2f), -89.f, 89.f);
        Controller->SetControlRotation(View);
    }
}
void ASCFirstPersonCharacter::AddViewKick(float PitchDegrees, float YawDegrees)
{
    if (!Controller) return;
    FRotator View = Controller->GetControlRotation();
    View.Yaw += YawDegrees;
    View.Pitch = FMath::Clamp(FRotator::NormalizeAxis(View.Pitch + PitchDegrees), -89.f, 89.f);
    Controller->SetControlRotation(View);
}
void ASCFirstPersonCharacter::SetVehicleRelativeView(float Yaw, float Pitch)
{
    const FQuat Base = Vehicle ? Vehicle->GetActorQuat() : FQuat::Identity;
    const FRotator View = (Base * FRotator(FMath::Clamp(Pitch, -89.f, 89.f), Yaw, 0).Quaternion()).Rotator();
    if (Controller) Controller->SetControlRotation(View);
    SetActorRotation(FRotator(0, View.Yaw, 0));
}
FRotator ASCFirstPersonCharacter::GetVehicleRelativeView() const
{
    const FQuat Base = Vehicle ? Vehicle->GetActorQuat() : FQuat::Identity;
    return (Base.Inverse() * GetViewRotation().Quaternion()).Rotator();
}
FVector ASCFirstPersonCharacter::GetVehicleRelativeLocation() const
{
    return Vehicle ? Vehicle->GetActorTransform().InverseTransformPosition(GetActorLocation()) : GetActorLocation();
}
void ASCFirstPersonCharacter::HoldMoveInput(FVector Input, float Seconds, bool bRun)
{
    HoldInput = Input.GetClampedToMaxSize(1);
    HoldSeconds = FMath::Max(0.f, Seconds);
    SetMoveInput(HoldSeconds > 0 ? HoldInput : FVector::ZeroVector);
    SetRunning(HoldSeconds > 0 && bRun);
}
void ASCFirstPersonCharacter::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!Vehicle || Vehicle->IsActorBeingDestroyed()) { ClearInput(); return; }
    Vehicle->UpdateDoorSensors(GetActorLocation(), GetWorld()->GetTimeSeconds()); // the game's door proximity sensors
    ApplyViewHold();
    UpdateLiftFocusAndRide();
    // The body and its kit are lit like the surfaces around them: by the lights of the room the player stands in.
    ASCVehicle* Kit = Body->GetKit();
    if (const int32 Channel = Vehicle->LightChannelAt(GetActorLocation()); LightAreaState != int8(Channel) || LightAreaKit.Get() != Kit)
    {
        LightAreaState = int8(Channel);
        LightAreaKit = Kit;
        for (const AActor* Actor : {static_cast<const AActor*>(this), static_cast<const AActor*>(Kit)})
        {
            if (!Actor) continue;
            TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
            for (UPrimitiveComponent* Primitive : Primitives) FSCLightAreas::SetChannel(Primitive, Channel);
        }
    }
    // WP-P3: inside the physics grid the vehicle's own (hull) colliders do not block; the grid's contents do.
    // C2 fixes: on a moving lift carriage, what the leg passes through does not block either (the carriage carries its riders).
    const FVector Axis(0, 0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
    const bool bInGrid = Vehicle->IsInPhysicsGrid(GetActorLocation() - Axis, GetActorLocation() + Axis);
    const int32 Ridden = bNoclip || CVarSCLiftCarryRiders.GetValueOnGameThread() == 0 ? INDEX_NONE : Vehicle->LiftRiddenBy(GetActorLocation() - Axis, Cast<UPrimitiveComponent>(GetMovementBaseObject()), GetCharacterMovement()->MaxStepHeight + 5);
    const double Leg = Vehicle->Lifts.IsValidIndex(Ridden) ? Vehicle->Lifts[Ridden]->LegStartedAt : -1;
    if (GridState != int8(bInGrid) || GridVehicle.Get() != Vehicle || Ridden != RiddenLift || Leg != RiddenLeg)
    {
        const bool bGridChanged = GridState != int8(bInGrid) || GridVehicle.Get() != Vehicle;
        GridState = int8(bInGrid);
        GridVehicle = Vehicle;
        RefreshMoveIgnores(bInGrid, Ridden);
        if (bGridChanged)
            UE_LOG(LogTemp, Display, TEXT("FP_GRID inside=%d hull_colliders=%d relative=%s"), int32(bInGrid), Vehicle->HullColliders.Num(),
                   *GetVehicleRelativeLocation().ToCompactString());
    }
    UpdateRideTrace(Ridden);
    // Single eye anchor; a camera re-attached to another driver (e.g. a head joint) is left alone.
    if (Camera->GetAttachParent() == GetCapsuleComponent()) Camera->SetRelativeLocation(EyeOffset());
    if (HoldSeconds > 0)
    {
        HoldSeconds -= DeltaSeconds;
        if (HoldSeconds <= 0) { HoldSeconds = 0; ApplyHeldInput(); }
    }
    NoclipRefusedSeconds = FMath::Max(0.f, NoclipRefusedSeconds - DeltaSeconds);
    const FRotator Rotation = GetViewRotation(); // control rotation; actor rotation without a controller
    const FRotator Heading(0, Rotation.Yaw, 0);
    if (!bTeleportMenuOpen)
    {
        AddMovementInput((bNoclip ? Rotation : Heading).Vector(), Move.X);
        AddMovementInput(FRotationMatrix(Heading).GetUnitAxis(EAxis::Y), Move.Y);
        if (bNoclip) AddMovementInput(FVector::UpVector, Move.Z);
    }
    const bool WantsMove = !Move.IsNearlyZero() && !bNoclip && !bTeleportMenuOpen;
    BlockedSeconds = WantsMove && FVector::DistSquared(LastPosition, GetActorLocation()) < 1 ? BlockedSeconds + DeltaSeconds : 0;
    bStuck = BlockedSeconds >= BlockedOfferSeconds;
    LastPosition = GetActorLocation();
    if (!bNoclip && GetActorLocation().Z < RecoveryZ)
    {
        SetActorTransform(LastSpawn, false, nullptr, ETeleportType::TeleportPhysics);
        GetCharacterMovement()->StopMovementImmediately();
        ++RecoveryCount;
        UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_RECOVER count=%d"), RecoveryCount);
    }
}
namespace
{
/** Distance from the segment A-B to a box, all in the box's frame. The distance to a convex set is convex along
 *  the segment, so a ternary search finds its minimum. */
double SegmentToBox(const FBox& Box, const FVector& A, const FVector& B)
{
    auto At = [&](double T) { return Box.ComputeSquaredDistanceToPoint(FMath::Lerp(A, B, T)); };
    double Lo = 0, Hi = 1;
    for (int32 I = 0; I < 48; ++I)
    {
        const double M1 = Lo + (Hi - Lo) / 3, M2 = Hi - (Hi - Lo) / 3;
        if (At(M1) < At(M2)) Hi = M2; else Lo = M1;
    }
    return FMath::Sqrt(At((Lo + Hi) / 2));
}
}
void ASCFirstPersonCharacter::AddGridIgnores(FCollisionQueryParams& Params, const FVector& Centre, double HalfHeight) const
{
    if (Vehicle && Vehicle->IsInPhysicsGrid(Centre - FVector(0, 0, HalfHeight), Centre + FVector(0, 0, HalfHeight)))
        for (const UPrimitiveComponent* Hull : Vehicle->HullColliders) Params.AddIgnoredComponent(Hull);
}
bool ASCFirstPersonCharacter::FindCapsuleSpot(FVector Hint, double Half, FVector& Centre, const FSCSeatBox* Seat) const
{
    const bool bSeated = Seat != nullptr;
    const double Radius = Profile.CapsuleRadius, Step = Profile.StepHeight;
    const double WalkableZ = GetCharacterMovement()->GetWalkableFloorZ();
    const double Skin = 2.5; // capsule clearance above the surface it stands on
    const FCollisionShape Shape = FCollisionShape::MakeCapsule(Radius, Half);
    const FCollisionQueryParams Base(SCENE_QUERY_STAT(SCSpawn), false, this);
    // Search a capsule-sized neighbourhood of the helper; never use an asset-name offset.
    // A seat is searched within two capsule diameters (seat bucket, footwell), never beyond.
    for (int32 Ring = 0; Ring <= (bSeated ? 2 : 4); ++Ring)
        for (int32 Direction = 0; Direction < (Ring ? 8 : 1); ++Direction)
        {
            const double Angle = Direction * UE_DOUBLE_PI / 4;
            const FVector Offset(FMath::Cos(Angle) * Ring * Radius * 2, FMath::Sin(Angle) * Ring * Radius * 2, 0);
            // Grid membership (hull colliders ignored inside the vehicle's physics grid) is decided where each query is,
            // as movement decides it for the capsule: the floor probe at its column, the capsule tests at the candidate.
            FCollisionQueryParams Column = Base;
            AddGridIgnores(Column, Hint + Offset + FVector(0, 0, Half), Half);
            FHitResult Hit;
            if (!GetWorld()->LineTraceSingleByChannel(Hit, Hint + Offset + FVector(0, 0, Half + Step),
                    Hint + Offset - FVector(0, 0, bSeated ? Step : Half * 4), ECC_Pawn, Column) ||
                Hit.ImpactNormal.Z < WalkableZ) continue;
            const FVector Candidate = Hit.ImpactPoint + FVector(0, 0, Half + Skin);
            FCollisionQueryParams Params = Base;
            AddGridIgnores(Params, Candidate, Half);
            if (GetWorld()->OverlapBlockingTestByChannel(Candidate, FQuat::Identity, ECC_Pawn, Shape, Params)) continue;
            if (Seat)
            {
                // Sitting needs no room to walk, but the spot must be the seat: the capsule has to be on the
                // seat's own geometry or against it. A clear capsule further off is the space beside the seat (a
                // bay behind its back, the far side of a bulkhead), not the seat. The capsule (its axis segment,
                // swept by Radius) is within its standing clearance of the seat's box, measured in the seat's frame
                // so the box does not grow with the vehicle's heading. Tolerance: the trace's float precision.
                const FVector Axis(0, 0, Half - Radius);
                const double Gap = SegmentToBox(Seat->Box, Seat->Frame.InverseTransformPosition(Candidate - Axis),
                                                Seat->Frame.InverseTransformPosition(Candidate + Axis));
                if (Gap > Radius + Skin + 0.1) continue;
                Centre = Candidate;
                return true;
            }
            // A clear capsule in a tiny pocket is not a usable spot. Require a two-capsule-width step
            // with floor support, using the same collision channel.
            for (int32 Heading = 0; Heading < 8; ++Heading)
            {
                const FVector Ahead = Candidate + FRotator(0, Heading * 45, 0).Vector() * Radius * 4;
                FHitResult Floor, Obstacle;
                if (!GetWorld()->LineTraceSingleByChannel(Floor, Ahead + FVector(0, 0, Step), Ahead - FVector(0, 0, Half + Step), ECC_Pawn, Params) ||
                    Floor.ImpactNormal.Z < WalkableZ) continue;
                const FVector End = Floor.ImpactPoint + FVector(0, 0, Half + Skin);
                if (FMath::Abs(End.Z - Candidate.Z) > Step) continue;
                if (!GetWorld()->SweepSingleByChannel(Obstacle, Candidate, End, FQuat::Identity, ECC_Pawn, Shape, Params))
                {
                    Centre = Candidate;
                    return true;
                }
            }
        }
    return false;
}
bool ASCFirstPersonCharacter::ResolveSpawn(FVector Hint, FVector& Centre, bool& bCrouched, const FSCSeatBox* Seat) const
{
    if (Seat)
    {
        // A seat without a box (no collidable geometry) is the hint itself.
        const FSCSeatBox AtHint{FBox(Hint, Hint), FTransform::Identity};
        bCrouched = FindCapsuleSpot(Hint, Profile.CrouchedHalfHeight, Centre, Seat->Box.IsValid ? Seat : &AtHint);
        return bCrouched;
    }
    bCrouched = false;
    if (FindCapsuleSpot(Hint, Profile.StandingHalfHeight, Centre)) return true;
    bCrouched = FindCapsuleSpot(Hint, Profile.CrouchedHalfHeight, Centre);
    return bCrouched;
}
bool ASCFirstPersonCharacter::TeleportToFloor(FVector Floor, bool bSeated)
{
    const FSCSeatBox Hint; // a seat known only by its hint
    return Place(Floor, bSeated ? &Hint : nullptr);
}
bool ASCFirstPersonCharacter::TeleportToSeat(FVector Floor, const FBox& Seat, const FTransform& Frame)
{
    const FSCSeatBox Box{Seat, Frame};
    return Place(Floor, &Box);
}
bool ASCFirstPersonCharacter::Place(FVector Floor, const FSCSeatBox* Seat)
{
    FVector Centre;
    bool bCrouchedSpot = false;
    if (!ResolveSpawn(Floor, Centre, bCrouchedSpot, Seat)) return false;
    if (bCrouchedSpot)
    {
        // Shrink the capsule now so it is placed at the crouched centre without overlapping.
        Crouch();
        GetCharacterMovement()->Crouch();
    }
    else
        UnCrouch();
    SetActorLocation(Centre, false, nullptr, ETeleportType::TeleportPhysics);
    GetCharacterMovement()->StopMovementImmediately();
    LastSpawn = GetActorTransform();
    LastPosition = Centre;
    BlockedSeconds = 0;
    bStuck = false;
    bTeleportMenuOpen = false;
    UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_SPAWN location=%s relative=%s crouched=%d"), *Centre.ToString(),
           *GetVehicleRelativeLocation().ToCompactString(), bCrouchedSpot);
    return true;
}
bool ASCFirstPersonCharacter::TeleportToSpawn(int32 Index)
{
    if (!Vehicle || !Vehicle->SpawnPoints.IsValidIndex(Index)) return false;
    const auto& Point = Vehicle->SpawnPoints[Index];
    const FVector Floor = (Point.Transform * Vehicle->GetActorTransform()).GetLocation();
    const bool bSeat = Point.Kind == FSCSpawnKind::Seat;
    bool bPlaced;
    if (bSeat && Point.Bounds.IsValid)
    {
        // The vehicle-local box in the vehicle's rigid frame: the vehicle's scale goes into the box's lengths.
        const FTransform Actor = Vehicle->GetActorTransform();
        FBox Box(ForceInit);
        Box += Point.Bounds.Min * Actor.GetScale3D();
        Box += Point.Bounds.Max * Actor.GetScale3D();
        bPlaced = TeleportToSeat(Floor, Box, FTransform(Actor.GetRotation(), Actor.GetLocation()));
    }
    else
        bPlaced = TeleportToFloor(Floor, bSeat);
    if (!bPlaced)
    {
        UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_SPAWN_REJECT index=%d kind=%s pos=%s label=\"%s\" reason=no_clear_capsule"), Index,
               *Point.Kind.ToString(), *Point.Transform.GetLocation().ToCompactString(), *Point.DisplayName);
        return false;
    }
    CurrentSpawn = Index;
    UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_AT index=%d kind=%s label=\"%s\""), Index, *Point.Kind.ToString(), *Point.DisplayName);
    return true;
}
ASCVehicle* ASCFirstPersonCharacter::GetHangar() const
{
    if (HangarOverride.IsValid()) return HangarOverride.Get();
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(this);
    return Viewer ? Viewer->GetEnvironment() : nullptr;
}
const TArray<FSCSpawnPoint>& ASCFirstPersonCharacter::GetHangarSpots()
{
    ASCVehicle* Hangar = GetHangar();
    const int32 Revision = Hangar ? Hangar->WaypointRevision : INDEX_NONE;
    if (Hangar == HangarListed.Get() && Revision == HangarListedRevision) return HangarSpots;
    HangarListed = Hangar;
    HangarListedRevision = Revision;
    ++HangarSpotsRevision;
    HangarSpots.Reset();
    if (!Hangar || !GetWorld()) return HangarSpots;
    // A waypoint is a safe standing place when a standing capsule fits on a walkable floor around it (the spawn search
    // every teleport runs; crouched-only spots are not offered: a place to walk from).
    int32 Rejected = 0;
    for (const FSCSpawnPoint& W : Hangar->Waypoints)
    {
        FVector Centre;
        bool bCrouched = false;
        const FVector Hint = Hangar->GetActorTransform().TransformPosition(W.Transform.GetLocation());
        if (ResolveSpawn(Hint, Centre, bCrouched) && !bCrouched)
        {
            HangarSpots.Add(W);
            continue;
        }
        ++Rejected;
        UE_LOG(LogTemp, Display, TEXT("HANGAR_WAYPOINT_REJECT source=%s pos=%s label=\"%s\" reason=%s"), *W.Source.ToString(),
               *W.Transform.GetLocation().ToCompactString(), *W.DisplayName, bCrouched ? TEXT("crouched_only") : TEXT("no_clear_capsule"));
    }
    UE_LOG(LogTemp, Display, TEXT("HANGAR_SPOTS hangar=%s waypoints=%d standing=%d rejected=%d"), *Hangar->GetName(), Hangar->Waypoints.Num(),
           HangarSpots.Num(), Rejected);
    return HangarSpots;
}
bool ASCFirstPersonCharacter::TeleportToHangar(int32 Index)
{
    ASCVehicle* Hangar = GetHangar();
    const TArray<FSCSpawnPoint>& Spots = GetHangarSpots();
    if (!Hangar || !Spots.IsValidIndex(Index)) return false;
    const FSCSpawnPoint Point = Spots[Index];
    const FVector Hint = Hangar->GetActorTransform().TransformPosition(Point.Transform.GetLocation());
    // A hangar place is a place to stand: one where only a crouched capsule fits now (cargo set down there since the
    // list was checked) is refused, like one where nothing fits.
    FVector Centre;
    bool bCrouched = false;
    if (!ResolveSpawn(Hint, Centre, bCrouched) || bCrouched || !TeleportToFloor(Hint))
    {
        UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_SPAWN_REJECT hangar=%d source=%s label=\"%s\" reason=%s"), Index,
               *Point.Source.ToString(), *Point.DisplayName, bCrouched ? TEXT("crouched_only") : TEXT("no_clear_capsule"));
        return false;
    }
    // Falling recovers below everything that collides: the hangar's floors too (a lower deck must not count as a fall).
    if (HangarRecoveryFor.Get() != Hangar)
    {
        HangarRecoveryFor = Hangar;
        const FBox Bounds = Hangar->ExplorationBounds();
        if (Bounds.IsValid) RecoveryZ = FMath::Min(RecoveryZ, Bounds.Min.Z - Profile.StandingHalfHeight * 4);
    }
    UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_AT hangar=%d kind=%s source=%s label=\"%s\""), Index, *Point.Kind.ToString(), *Point.Source.ToString(),
           *Point.DisplayName);
    return true;
}
TArray<FSCTeleportRow> ASCFirstPersonCharacter::TeleportRows()
{
    return SCTeleportMenu::Rows(Vehicle ? Vehicle->SpawnPoints.Num() : 0, GetHangarSpots().Num());
}
bool ASCFirstPersonCharacter::TeleportToRow(int32 Row)
{
    const TArray<FSCTeleportRow> Rows = TeleportRows();
    if (!Rows.IsValidIndex(Row) || !Rows[Row].IsDestination()) return false;
    TeleportSelection = Rows[Row];
    TeleportSelectionLabel = DestinationLabel(Rows[Row]);
    return Rows[Row].Type == FSCTeleportRow::EType::Ship ? TeleportToSpawn(Rows[Row].Index) : TeleportToHangar(Rows[Row].Index);
}
FString ASCFirstPersonCharacter::DestinationLabel(const FSCTeleportRow& Row)
{
    if (Row.Type == FSCTeleportRow::EType::Ship) return Vehicle && Vehicle->SpawnPoints.IsValidIndex(Row.Index) ? Vehicle->SpawnPoints[Row.Index].DisplayName : FString();
    const TArray<FSCSpawnPoint>& Spots = GetHangarSpots();
    return Row.Type == FSCTeleportRow::EType::Hangar && Spots.IsValidIndex(Row.Index) ? Spots[Row.Index].DisplayName : FString();
}
int32 ASCFirstPersonCharacter::FindDestination(const FSCTeleportRow& Row, const FString& Label)
{
    if (!Row.IsDestination()) return INDEX_NONE;
    const TArray<FSCTeleportRow> Rows = TeleportRows();
    const int32 Same = Rows.IndexOfByPredicate([&](const FSCTeleportRow& R) { return R.Type == Row.Type && R.Index == Row.Index; });
    if (Same != INDEX_NONE && DestinationLabel(Rows[Same]) == Label) return Same;
    return Rows.IndexOfByPredicate([&](const FSCTeleportRow& R) { return R.Type == Row.Type && DestinationLabel(R) == Label; });
}
bool ASCFirstPersonCharacter::TeleportToDestination(const FSCTeleportRow& Row, const FString& Label)
{
    const int32 At = FindDestination(Row, Label);
    if (At == INDEX_NONE)
    {
        UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_SPAWN_REJECT label=\"%s\" reason=no_longer_listed"), *Label);
        return false;
    }
    return TeleportToRow(At);
}
int32 ASCFirstPersonCharacter::TeleportSelectionRow()
{
    return FindDestination(TeleportSelection, TeleportSelectionLabel);
}
void ASCFirstPersonCharacter::SetTeleportMenuOpen(bool bOpen)
{
    bTeleportMenuOpen = bOpen;
    if (!bOpen) return;
    // Ships, cargo and doors move: the hangar's standing places are checked again whenever the menu opens.
    InvalidateHangarSpots();
    // Opening highlights the last destination (else the current spawn, else the first): the keys move from there.
    if (TeleportSelectionRow() != INDEX_NONE) return;
    const TArray<FSCTeleportRow> Rows = TeleportRows();
    const int32 Current = Rows.IndexOfByPredicate([&](const FSCTeleportRow& R) { return R.Type == FSCTeleportRow::EType::Ship && R.Index == CurrentSpawn; });
    const int32 Row = SCTeleportMenu::Step(Rows, Current, 0);
    TeleportSelection = Rows.IsValidIndex(Row) ? Rows[Row] : FSCTeleportRow();
    TeleportSelectionLabel = Rows.IsValidIndex(Row) ? DestinationLabel(Rows[Row]) : FString();
}
void ASCFirstPersonCharacter::MoveTeleportSelection(int32 Delta)
{
    const TArray<FSCTeleportRow> Rows = TeleportRows();
    const int32 Row = SCTeleportMenu::Step(Rows, TeleportSelectionRow(), Delta);
    TeleportSelection = Rows.IsValidIndex(Row) ? Rows[Row] : FSCTeleportRow();
    TeleportSelectionLabel = Rows.IsValidIndex(Row) ? DestinationLabel(Rows[Row]) : FString();
}
bool ASCFirstPersonCharacter::Explore(ASCVehicle* InVehicle)
{
    Vehicle = InVehicle;
    if (!Vehicle || !Vehicle->bCollisionReady) return false;
    // A moving carriage (WP-P5) moves before the character follows its movement base in the same frame, and the movement
    // runs after the character's own tick (C2 fixes: this frame's input and move-ignore list, e.g. what a ridden leg
    // passes, apply to this frame's move; without the prerequisite the engine may run the movement first).
    AddTickPrerequisiteActor(Vehicle);
    GetCharacterMovement()->PrimaryComponentTick.AddPrerequisite(this, PrimaryActorTick);
    // Below everything that collides (and the world origin floor) by four capsule half-heights:
    // computed once, the per-tick check must not walk every component.
    const FBox Bounds = Vehicle->ExplorationBounds();
    RecoveryZ = (Bounds.IsValid ? FMath::Min(0.0, Bounds.Min.Z) : 0.0) - Profile.StandingHalfHeight * 4;
    HangarRecoveryFor.Reset(); // the hangar's floors are added on the first teleport to one of its places
    TArray<int32> Candidates;
    for (int32 I = 0; I < Vehicle->SpawnPoints.Num(); ++I) Candidates.Add(I);
    Candidates.StableSort([&](int32 A, int32 B) { return Vehicle->SpawnPoints[A].Transform.GetLocation().Z < Vehicle->SpawnPoints[B].Transform.GetLocation().Z; });
    for (FName Kind : FSCSpawnKind::StartOrder())
        for (int32 I : Candidates)
            if (Vehicle->SpawnPoints[I].Kind == Kind && TeleportToSpawn(I))
            {
                UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_BEST kind=%s label=%s"), *Kind.ToString(), *Vehicle->SpawnPoints[I].DisplayName);
                return true;
            }
    return false;
}
int32 ASCFirstPersonCharacter::FocusedInteraction() const
{
    return Vehicle ? Vehicle->FindInteraction(InteractionOrigin(), GetViewRotation().Vector(), InteractReach, this) : INDEX_NONE;
}
FVector ASCFirstPersonCharacter::InteractionOrigin() const
{
    // Third person: reach from the character's eye (camera joint) along its view, not from the orbit camera.
    return Body && Body->HasBody() && Body->IsThirdPerson() ? Body->HeadLocation() : Camera->GetComponentLocation();
}
bool ASCFirstPersonCharacter::Interact()
{
    if (!Vehicle) return false;
    // WP-P5: a floor panel under the crosshair sends the option there (nothing off its rows); another lift panel (a call
    // button) its primary interaction, along its exported links; anything else is the door interaction. The aim is taken
    // now, not from the last tick: the view may have turned since.
    UpdateFloorPanelAim();
    if (Vehicle->FloorPanels.IsValidIndex(AimedFloorPanel))
    {
        if (AimedFloorOption == INDEX_NONE)
        {
            UE_LOG(LogTemp, Display, TEXT("FP_PANEL_PRESS panel=%s option=none handled=0 (the crosshair is on the panel, off its rows)"),
                   *Vehicle->FloorPanels[AimedFloorPanel].EntityId);
            return false;
        }
        return Vehicle->PressFloorPanel(AimedFloorPanel, AimedFloorOption) > 0;
    }
    if (const ASCVehicle::FLiftPanel* Panel = Vehicle->FindLiftPanel(InteractionOrigin(), GetViewRotation().Vector(), InteractReach, this))
    {
        const FName Interaction = Vehicle->LiftPanelPressInteraction(*Panel);
        if (Interaction.IsNone())
        {
            UE_LOG(LogTemp, Display, TEXT("FP_PANEL_PRESS panel=%s option=none handled=0 (a floor-select panel: the crosshair is off its face's rows)"),
                   *Panel->EntityId);
            return false;
        }
        const int32 Handled = Vehicle->SendInteractionIn(Panel->Interior, Panel->EntityId, Interaction);
        UE_LOG(LogTemp, Display, TEXT("FP_PANEL_PRESS panel=%s label=\"%s\" interaction=\"%s\" handled=%d"), *Panel->EntityId, *Panel->Label,
               *Interaction.ToString(), Handled);
        return Handled > 0;
    }
    // The door interaction (a pad, the rig under the crosshair, or the nearest driven joint), logged with how it was found.
    FSCInteractionPick Pick;
    const int32 Door = Vehicle->FindInteraction(InteractionOrigin(), GetViewRotation().Vector(), InteractReach, this, &Pick);
    const bool bToggled = Vehicle->ToggleDoor(Door);
    UE_LOG(LogTemp, Display, TEXT("FP_INTERACT door=%d via=%s hit=%s dist=%.0f reach=%.0f toggled=%d label=\"%s\""), Door, *Pick.Via, *Pick.Hit,
           Pick.Distance, Pick.Reach, bToggled, Vehicle->Doors.IsValidIndex(Door) ? *Vehicle->Doors[Door].DisplayName : TEXT(""));
    return bToggled;
}
bool ASCFirstPersonCharacter::FocusedPanel(FString& EntityId, FName& Choice) const
{
    if (!Vehicle) return false;
    if (Vehicle->FloorPanels.IsValidIndex(AimedFloorPanel))
    {
        const FSCFloorPanel& P = Vehicle->FloorPanels[AimedFloorPanel];
        EntityId = P.EntityId;
        Choice = P.Layout.Options.IsValidIndex(AimedFloorOption) ? P.Layout.Options[AimedFloorOption].Interaction : NAME_None;
        return true;
    }
    const ASCVehicle::FLiftPanel* Panel = Vehicle->FindLiftPanel(InteractionOrigin(), GetViewRotation().Vector(), InteractReach, this);
    if (!Panel || Panel->Choices.IsEmpty()) return false;
    EntityId = Panel->EntityId;
    Choice = Vehicle->LiftPanelPressInteraction(*Panel);
    return true;
}
void ASCFirstPersonCharacter::EndPlay(const EEndPlayReason::Type Reason)
{
    // The highlight lives on the vehicle: leaving first person must not leave a row lit with no crosshair on it.
    if (Vehicle && Vehicle->FloorPanels.IsValidIndex(AimedFloorPanel))
    {
        Vehicle->SetFloorPanelHighlight(INDEX_NONE, INDEX_NONE);
        Vehicle->RefreshFloorPanels();
    }
    AimedFloorPanel = AimedFloorOption = INDEX_NONE;
    Super::EndPlay(Reason);
}
void ASCFirstPersonCharacter::UpdateLiftFocusAndRide()
{
    if (!Vehicle || Vehicle->Lifts.IsEmpty()) return;
    UpdateFloorPanelAim();
    // Riding: the movement base is a part the carriage carries. Logged on change and once a second while riding.
    UpdateRideLog();
}
void ASCFirstPersonCharacter::UpdateFloorPanelAim()
{
    // WP-P5 floor panel: the option under the crosshair is highlighted (nothing off the panel's face or its rows).
    if (Vehicle && !Vehicle->FloorPanels.IsEmpty())
    {
        int32 Panel = INDEX_NONE, Option = INDEX_NONE;
        FVector2D Content(-1, -1);
        Vehicle->AimFloorPanel(InteractionOrigin(), GetViewRotation().Vector(), InteractReach, this, Panel, Option, &Content);
        if (Panel != AimedFloorPanel || Option != AimedFloorOption)
        {
            AimedFloorPanel = Panel;
            AimedFloorOption = Option;
            const FSCFloorPanelOption* O = Vehicle->FloorPanels.IsValidIndex(Panel) && Vehicle->FloorPanels[Panel].Layout.Options.IsValidIndex(Option)
                                               ? &Vehicle->FloorPanels[Panel].Layout.Options[Option] : nullptr;
            UE_LOG(LogTemp, Display, TEXT("FP_PANEL_AIM panel=%d option=%d stop=%d label=\"%s\" content=%.3f,%.3f"), Panel, Option, O ? O->Stop : INDEX_NONE,
                   O ? *O->Label : TEXT(""), Content.X, Content.Y);
        }
        Vehicle->SetFloorPanelHighlight(AimedFloorPanel, AimedFloorOption);
        Vehicle->RefreshFloorPanels();
    }
}
void ASCFirstPersonCharacter::UpdateRideLog()
{
    const UPrimitiveComponent* Base = Cast<UPrimitiveComponent>(GetMovementBaseObject());
    const int32 Lift = Base ? Vehicle->LiftCarrying(Base) : INDEX_NONE;
    const double Now = GetWorld()->GetTimeSeconds();
    if (Lift != RideLift || (Lift != INDEX_NONE && Now - LastRideLog >= 1.0))
    {
        const USceneComponent* Anchor = Vehicle->Lifts.IsValidIndex(Lift) ? Vehicle->Lifts[Lift]->Anchor.Get() : nullptr;
        const double Feet = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        const auto& Floor = GetCharacterMovement()->CurrentFloor;
        UE_LOG(LogTemp, Display, TEXT("LIFT_RIDE lift=%d base=%s asset=%s rel_z_cm=%.2f floor_dist_cm=%.2f lift_state=%s pos=%s"), Lift,
               Base ? *Base->GetName() : TEXT("-"),
               Base && !Base->ComponentTags.IsEmpty() ? *FPaths::GetBaseFilename(Base->ComponentTags[0].ToString()) : Base && Base->GetOwner() ? *Base->GetOwner()->GetName() : TEXT("-"), Anchor ? Feet - Anchor->GetComponentLocation().Z : 0.0, Floor.bBlockingHit ? Floor.FloorDist : -1.0,
               Vehicle->Lifts.IsValidIndex(Lift) ? LexToString(Vehicle->Lifts[Lift]->State) : TEXT("-"), *GetVehicleRelativeLocation().ToCompactString());
        RideLift = Lift;
        LastRideLog = Now;
    }
}
void ASCFirstPersonCharacter::RefreshMoveIgnores(bool bInGrid, int32 Lift)
{
    UCapsuleComponent* Capsule = GetCapsuleComponent();
    Capsule->ClearMoveIgnoreComponents();
    if (bInGrid)
        for (UPrimitiveComponent* Hull : Vehicle->HullColliders) Capsule->IgnoreComponentWhenMoving(Hull, true);
    // The rider is carried through what the carriage's leg passes (the C2's hold shoulder slab reaches into the shaft under
    // the hold floor; swept with the carriage, the capsule's edge caught on it and stayed at the lower deck).
    int32 Passed = 0;
    if (Vehicle->Lifts.IsValidIndex(Lift))
        for (const TWeakObjectPtr<UPrimitiveComponent>& C : Vehicle->LiftPathObstacles(Lift))
            if (UPrimitiveComponent* P = C.Get()) { Capsule->IgnoreComponentWhenMoving(P, true); ++Passed; }
    if (Lift != RiddenLift || (Vehicle->Lifts.IsValidIndex(Lift) && Vehicle->Lifts[Lift]->LegStartedAt != RiddenLeg))
        UE_LOG(LogTemp, Display, TEXT("LIFT_RIDER lift=%d leg_t=%.3f passed=%d relative=%s"), Lift,
               Vehicle->Lifts.IsValidIndex(Lift) ? Vehicle->Lifts[Lift]->LegStartedAt : -1.0, Passed, *GetVehicleRelativeLocation().ToCompactString());
    RiddenLift = Lift;
    RiddenLeg = Vehicle->Lifts.IsValidIndex(Lift) ? Vehicle->Lifts[Lift]->LegStartedAt : -1;
}
void ASCFirstPersonCharacter::UpdateRideTrace(int32 Lift)
{
    // Per tick while a carriage carries the capsule: the feet against the carriage anchor the last movement update moved the
    // capsule with (the vehicle moves the anchor before this tick, the capsule follows after it), the floor distance and the
    // base. At the leg's end, LIFT_RIDE_LEG: the spread of that distance (steady for a rider kept on the carriage).
    const USceneComponent* Anchor = Vehicle->Lifts.IsValidIndex(Lift) ? Vehicle->Lifts[Lift]->Anchor.Get() : nullptr;
    const double Feet = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    if (Anchor && bTraceAnchor)
    {
        const double Rel = Feet - TraceAnchorZ;
        const UPrimitiveComponent* Base = Cast<UPrimitiveComponent>(GetMovementBaseObject());
        const bool bOnCarriage = Base && Vehicle->LiftCarrying(Base) == Lift;
        TraceMin = TraceSamples ? FMath::Min(TraceMin, Rel) : Rel;
        TraceMax = TraceSamples ? FMath::Max(TraceMax, Rel) : Rel;
        ++TraceSamples;
        TraceOtherBase += !bOnCarriage;
        const auto& Floor = GetCharacterMovement()->CurrentFloor;
        UE_LOG(LogTemp, Display, TEXT("LIFT_RIDE_TICK lift=%d t=%.3f s_m=%.3f anchor_z=%.2f rel_z_cm=%.2f floor_dist_cm=%.2f base=%s"), Lift,
               GetWorld()->GetTimeSeconds(), Vehicle->Lifts[Lift]->S, TraceAnchorZ, Rel, Floor.bBlockingHit ? Floor.FloorDist : -1.0,
               Base ? *Base->GetName() : TEXT("-"));
    }
    else if (!Anchor && TraceSamples > 0)
    {
        UE_LOG(LogTemp, Display, TEXT("LIFT_RIDE_LEG samples=%d rel_z_min_cm=%.2f rel_z_max_cm=%.2f spread_cm=%.2f off_carriage=%d relative=%s"), TraceSamples,
               TraceMin, TraceMax, TraceMax - TraceMin, TraceOtherBase, *GetVehicleRelativeLocation().ToCompactString());
        TraceSamples = TraceOtherBase = 0;
    }
    bTraceAnchor = Anchor != nullptr;
    if (Anchor) TraceAnchorZ = Anchor->GetComponentLocation().Z;
}
void ASCFirstPersonCharacter::ToggleNoclip()
{
    if (bNoclip)
    {
        FCollisionQueryParams Params(SCENE_QUERY_STAT(SCNoclip), false, this);
        AddGridIgnores(Params, GetActorLocation(), GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
        if (GetWorld()->OverlapBlockingTestByChannel(GetActorLocation(), FQuat::Identity, ECC_Pawn,
                GetCapsuleComponent()->GetCollisionShape(), Params))
        {
            NoclipRefusedSeconds = NoclipRefusalSeconds;
            UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_NOCLIP refused=1 relative=%s"), *GetVehicleRelativeLocation().ToCompactString());
            return;
        }
    }
    bNoclip = !bNoclip;
    NoclipRefusedSeconds = 0;
    SetActorEnableCollision(!bNoclip);
    GetCharacterMovement()->SetMovementMode(bNoclip ? MOVE_Flying : MOVE_Falling);
    GetCharacterMovement()->StopMovementImmediately();
    UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_NOCLIP on=%d relative=%s"), bNoclip, *GetVehicleRelativeLocation().ToCompactString());
}
bool ASCFirstPersonCharacter::SetNoclip(bool bOn)
{
    if (bOn != bNoclip) ToggleNoclip();
    return bNoclip == bOn;
}
void ASCFirstPersonCharacter::SetCrouching(bool bCrouch)
{
    if (bCrouch) Crouch(); else UnCrouch();
}
void ASCFirstPersonCharacter::SetMobiGlasOpen(bool bOpen)
{
    if (bOpen == bMobiGlasOpen) return;
    bMobiGlasOpen = bOpen;
    if (bOpen)
    {
        // The mouse now drives the UI and the live-view orbit; the player stands still where it is (its view comes
        // back unchanged on close): release movement, trigger and sights.
        ClearInput();
        Body->SetTrigger(false);
        bOrbiting = false;
    }
    UE_LOG(LogTemp, Display, TEXT("FP_MOBIGLAS open=%d"), bOpen);
}
void ASCFirstPersonCharacter::SetTorchAudio(ASCVehicle* Kit, const FString& ItemEntity)
{
    TorchKit = Kit;
    TorchItem = ItemEntity;
}
void ASCFirstPersonCharacter::SetTorch(bool bOn)
{
    if (bOn != Flashlight->IsVisible())
        if (USCAudioSubsystem* Audio = USCAudioSubsystem::Get(this))
            Audio->PlayItemAction(TorchKit.Get(), TorchItem, bOn ? TEXT("Light_On_SFX") : TEXT("Light_Off_SFX"), Flashlight); // WP-A1
    Flashlight->SetVisibility(bOn);
    UE_LOG(LogTemp, Display, TEXT("FP_TORCH on=%d source=%s at=%s dir=%s cd=%.0f"), bOn, *TorchSource,
           *Flashlight->GetComponentLocation().ToCompactString(), *Flashlight->GetForwardVector().ToCompactString(), Flashlight->Intensity);
}
void ASCFirstPersonCharacter::SetTorchLight(const FSCFlashlight* Game, UMaterialInterface* LightFunction)
{
    if (Game)
    {
        FSCLightImporter::ApplyFlashlight(Flashlight, *Game, LightFunction);
        TorchSource = TEXT("game:") + Game->Entity;
        const FSCLightImporter::FFlashlightSettings S = FSCLightImporter::FlashlightSettings(*Game);
        UE_LOG(LogTemp, Display,
               TEXT("FP_TORCH_LIGHT source=%s port=%s cd=%.0f radius_cm=%.0f half_fov=%.1f bulb_cm=%.2f color=%s cookie=%s gobo=%d"),
               *TorchSource, *Game->Port, S.Candela, S.RadiusCm, S.HalfFovDeg, S.BulbCm, *Game->Color.ToHex(), *Game->Cookie,
               LightFunction != nullptr);
        if (!LightFunction)
            UE_LOG(LogTemp, Warning, TEXT("FP_TORCH_LIGHT %s: cookie %s has no light function (package path '%s'): the projector is unshaped"),
                   *Game->Entity, *Game->Cookie, *Game->ProjectorTexture);
        return;
    }
    ApplyFallbackTorch(Flashlight);
    TorchSource = TEXT("fallback");
    UE_LOG(LogTemp, Display, TEXT("FP_TORCH_LIGHT source=fallback (no game flashlight: no body, a loadout without one, or sc.FP.TorchLight 0) cd=%.0f"),
           TorchCandelas);
}
const TArray<TPair<FKey, ESCFirstPersonAction>>& ASCFirstPersonCharacter::KeyTable()
{
    using A = ESCFirstPersonAction;
    static const TArray<TPair<FKey, A>> Table = {
        {EKeys::W, A::Forward}, {EKeys::S, A::Back}, {EKeys::A, A::Left}, {EKeys::D, A::Right},
        {EKeys::Q, A::Down}, {EKeys::SpaceBar, A::Up}, {EKeys::LeftShift, A::Run}, {EKeys::LeftControl, A::Crouch},
        {EKeys::E, A::Interact}, {EKeys::F, A::Torch}, {EKeys::V, A::Noclip}, {EKeys::T, A::Teleport}, {EKeys::Escape, A::Close},
        {EKeys::H, A::Holster}, {EKeys::C, A::View}, {EKeys::One, A::Weapon1}, {EKeys::Two, A::Weapon2}, {EKeys::Three, A::Weapon3},
        {EKeys::Four, A::Weapon4}, {EKeys::Five, A::Weapon5}, {EKeys::Six, A::Weapon6}, {EKeys::Seven, A::Weapon7},
        {EKeys::Eight, A::Weapon8}, {EKeys::Nine, A::Weapon9}, {EKeys::RightMouseButton, A::Aim},
        {EKeys::LeftMouseButton, A::Fire}, {EKeys::Z, A::Orbit}, {EKeys::F1, A::MobiGlas},
        {EKeys::Up, A::MenuUp}, {EKeys::Down, A::MenuDown}, {EKeys::Enter, A::MenuSelect}};
    return Table;
}
bool ASCFirstPersonCharacter::HandleKey(FKey Key, bool bDown)
{
    for (const auto& Entry : KeyTable())
        if (Entry.Key == Key) return HandleAction(Entry.Value, bDown);
    return false;
}
void ASCFirstPersonCharacter::ApplyHeldInput()
{
    using A = ESCFirstPersonAction;
    auto Axis = [&](A Positive, A Negative) { return float(Held.Contains(Positive)) - float(Held.Contains(Negative)); };
    SetMoveInput(FVector(Axis(A::Forward, A::Back), Axis(A::Right, A::Left), Axis(A::Up, A::Down)));
    SetRunning(Held.Contains(A::Run));
}
bool ASCFirstPersonCharacter::HandleAction(ESCFirstPersonAction Action, bool bDown)
{
    using A = ESCFirstPersonAction;
    // The open teleport menu takes W / S as Up / Down (a press moves the selection; nothing is held, so closing the menu
    // does not start a walk).
    if (bTeleportMenuOpen && bDown && (Action == A::Forward || Action == A::Back)) Action = Action == A::Forward ? A::MenuUp : A::MenuDown;
    if (Action == A::MenuUp || Action == A::MenuDown || Action == A::MenuSelect)
    {
        if (!bTeleportMenuOpen) return false;
        if (bDown && Action == A::MenuSelect) TeleportToRow(TeleportSelectionRow());
        else if (bDown) MoveTeleportSelection(Action == A::MenuUp ? -1 : 1);
        return true;
    }
    if (Action <= A::Crouch) // held actions
    {
        // Standing still while the mobiGlas or the teleport menu is up (releases still pass): nothing starts that the
        // menu's closing (a teleport, a click) would leave running.
        if (bDown && (bMobiGlasOpen || bTeleportMenuOpen)) return true;
        if (bDown) Held.Add(Action); else Held.Remove(Action);
        if (HoldSeconds <= 0) ApplyHeldInput();
        if (Action == A::Up && !bNoclip) { if (bDown) Jump(); else StopJumping(); }
        if (Action == A::Crouch) SetCrouching(bDown);
        return true;
    }
    if (Action == A::Aim) // held: aiming lasts while the button is down
    {
        if (bDown && bMobiGlasOpen) return true; // the mouse belongs to the mobiGlas
        Body->SetAiming(bDown);
        return Body->HasBody();
    }
    if (Action == A::Fire) // held: the drawn weapon fires while the button is down
    {
        if (bDown && bMobiGlasOpen) return true;
        Body->SetTrigger(bDown);
        return Body->HasBody();
    }
    if (Action == A::Orbit) // held: orbit the third-person camera; a quick second press puts it behind again
    {
        bOrbiting = bDown;
        if (bDown)
        {
            const double Now = FPlatformTime::Seconds();
            if (LastOrbitPress >= 0 && Now - LastOrbitPress < DoubleTapSeconds)
            {
                Body->ResetOrbit();
                LastOrbitPress = -1;
            }
            else
                LastOrbitPress = Now;
        }
        return true;
    }
    if (!bDown) return false;
    switch (Action)
    {
    case A::Interact: Interact(); return true;
    case A::Torch: SetTorch(!Flashlight->IsVisible()); return true;
    case A::Noclip: ToggleNoclip(); return true;
    case A::Teleport: SetTeleportMenuOpen(!bTeleportMenuOpen); ClearInput(); return true;
    case A::Close: SetTeleportMenuOpen(false); SetMobiGlasOpen(false); ClearInput(); return true;
    case A::MobiGlas: SetMobiGlasOpen(!bMobiGlasOpen); return true;
    case A::Holster: return Body->RequestSlot(INDEX_NONE);
    case A::View: Body->SetThirdPerson(!Body->IsThirdPerson()); return Body->HasBody();
    default:
        if (Action >= A::Weapon1 && Action <= A::Weapon9) return Body->RequestSlot(int32(Action) - int32(A::Weapon1));
        return false;
    }
}
void ASCFirstPersonCharacter::SetupPlayerInputComponent(UInputComponent* Input)
{
    Super::SetupPlayerInputComponent(Input);
    for (const auto& Entry : KeyTable())
        for (EInputEvent Event : {IE_Pressed, IE_Released})
        {
            FInputKeyBinding Binding(FInputChord(Entry.Key), Event);
            const ESCFirstPersonAction Action = Entry.Value;
            Binding.KeyDelegate.GetDelegateForManualSet().BindLambda([this, Action, Event] { HandleAction(Action, Event == IE_Pressed); });
            Input->KeyBindings.Add(MoveTemp(Binding));
        }
    Input->BindAxisKey(EKeys::MouseX).AxisDelegate.GetDelegateForManualSet().BindLambda([this](float Value) { Look(FVector2D(Value, 0)); });
    Input->BindAxisKey(EKeys::MouseY).AxisDelegate.GetDelegateForManualSet().BindLambda([this](float Value) { Look(FVector2D(0, -Value)); });
}
FString ASCFirstPersonCharacter::HUDText() const
{
    if (!Vehicle) return FString();
    const int32 Door = FocusedInteraction();
    FString Prompt = Door == INDEX_NONE ? FString()
        : FString::Printf(TEXT("E: %s %s"), Vehicle->IsDoorOpen(Door) ? TEXT("Close") : TEXT("Open"), *Vehicle->Doors[Door].DisplayName);
    // WP-P5: a floor panel's highlighted option (its face shows the rest); another lift panel's primary interaction.
    if (Vehicle->FloorPanels.IsValidIndex(AimedFloorPanel))
    {
        const FSCFloorPanel& P = Vehicle->FloorPanels[AimedFloorPanel];
        Prompt = P.Layout.Options.IsValidIndex(AimedFloorOption) ? FString::Printf(TEXT("E: %s"), *P.Layout.Options[AimedFloorOption].Label) : FString();
    }
    else if (const ASCVehicle::FLiftPanel* Panel = Vehicle->FindLiftPanel(InteractionOrigin(), GetViewRotation().Vector(), InteractReach, this))
    {
        const FName Interaction = Vehicle->LiftPanelPressInteraction(*Panel);
        const FSCPointInteraction* C = Panel->Choices.FindByPredicate([&](const FSCPointInteraction& X) { return X.Name == Interaction; });
        Prompt = !C ? FString() : FString::Printf(TEXT("E: %s"), C->DisplayName.IsEmpty() ? *C->Name.ToString() : *C->DisplayName);
    }
    const TCHAR* Status = NoclipRefusedSeconds > 0 ? TEXT("Cannot leave noclip here: the capsule overlaps geometry")
        : bStuck ? TEXT("Movement blocked — press V for noclip")
        : Vehicle->bCollisionReady ? TEXT("Collision ready") : TEXT("Collision cooking…");
    const FString Player = Body->HasBody() ? Body->HUDLine() : BodyStatus;
    return FString::Printf(TEXT("%s · %.1f m/s · torch %s · %s cm from ship origin\n%s\n%s%s%s\nWASD move · Shift run · Space jump · Ctrl crouch · E interact · F torch · V noclip · T teleport · C view · Z orbit (double-tap: behind) · LMB fire · RMB aim · F1 mobiGlas"),
        bNoclip ? TEXT("Noclip") : bIsCrouched ? TEXT("Crouched") : bRunning ? TEXT("Running") : TEXT("Walking"),
        GetVelocity().Size() / 100, Flashlight->IsVisible() ? TEXT("on") : TEXT("off"),
        *GetVehicleRelativeLocation().ToCompactString(), *Prompt, Status, Player.IsEmpty() ? TEXT("") : TEXT("\n"), *Player);
}
