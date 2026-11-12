// ACharacter owns gravity, step-up, crouch and capsule sweeps; no ship-specific navigation rules.
#include "SCFirstPersonCharacter.h"
#include "SCVehicle.h"
#include "SCPlayerBody.h"
#include "SCLightAreas.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/InputComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

namespace
{
constexpr float NoclipRefusalSeconds = 3;   // how long the HUD shows a refused noclip exit
constexpr float BlockedOfferSeconds = 2;    // blocked input this long offers noclip
constexpr float InteractReach = 300;        // E reaches 3 m (brief)
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
    Camera = CreateDefaultSubobject<USCEyeCameraComponent>(TEXT("FirstPersonCamera"));
    Body = CreateDefaultSubobject<USCPlayerBodyComponent>(TEXT("PlayerBody"));
    Camera->SetupAttachment(GetCapsuleComponent());
    Camera->bUsePawnControlRotation = true;
    Flashlight = CreateDefaultSubobject<USpotLightComponent>(TEXT("ExplorationFlashlight"));
    Flashlight->SetupAttachment(Camera);
    // Viewer-owned torch, not an imported game light.
    Flashlight->SetIntensity(2000);
    Flashlight->SetAttenuationRadius(2000);
    Flashlight->SetOuterConeAngle(35);
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
}
void ASCFirstPersonCharacter::CapsuleHit(UPrimitiveComponent*, AActor*, UPrimitiveComponent*, FVector, const FHitResult&) { ++CapsuleHits; }
void ASCFirstPersonCharacter::Look(FVector2D Pixels)
{
    if (Controller)
    {
        FRotator View = Controller->GetControlRotation();
        View.Yaw += Pixels.X * .2f;
        View.Pitch = FMath::Clamp(FRotator::NormalizeAxis(View.Pitch - Pixels.Y * .2f), -89.f, 89.f);
        Controller->SetControlRotation(View);
    }
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
    const FVector Axis(0, 0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
    if (const bool bInGrid = Vehicle->IsInPhysicsGrid(GetActorLocation() - Axis, GetActorLocation() + Axis);
        GridState != int8(bInGrid) || GridVehicle.Get() != Vehicle)
    {
        GridState = int8(bInGrid);
        GridVehicle = Vehicle;
        UCapsuleComponent* Capsule = GetCapsuleComponent();
        Capsule->ClearMoveIgnoreComponents();
        if (bInGrid)
            for (UPrimitiveComponent* Hull : Vehicle->HullColliders) Capsule->IgnoreComponentWhenMoving(Hull, true);
        UE_LOG(LogTemp, Display, TEXT("FP_GRID inside=%d hull_colliders=%d relative=%s"), int32(bInGrid), Vehicle->HullColliders.Num(),
               *GetVehicleRelativeLocation().ToCompactString());
    }
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
bool ASCFirstPersonCharacter::Explore(ASCVehicle* InVehicle)
{
    Vehicle = InVehicle;
    if (!Vehicle || !Vehicle->bCollisionReady) return false;
    // Below everything that collides (and the world origin floor) by four capsule half-heights:
    // computed once, the per-tick check must not walk every component.
    const FBox Bounds = Vehicle->ExplorationBounds();
    RecoveryZ = (Bounds.IsValid ? FMath::Min(0.0, Bounds.Min.Z) : 0.0) - Profile.StandingHalfHeight * 4;
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
    return Vehicle && Vehicle->ToggleDoor(FocusedInteraction());
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
void ASCFirstPersonCharacter::SetTorch(bool bOn)
{
    Flashlight->SetVisibility(bOn);
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
        {EKeys::Eight, A::Weapon8}, {EKeys::Nine, A::Weapon9}};
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
    if (Action <= A::Crouch) // held actions
    {
        if (bDown) Held.Add(Action); else Held.Remove(Action);
        if (HoldSeconds <= 0) ApplyHeldInput();
        if (Action == A::Up && !bNoclip) { if (bDown) Jump(); else StopJumping(); }
        if (Action == A::Crouch) SetCrouching(bDown);
        return true;
    }
    if (!bDown) return false;
    switch (Action)
    {
    case A::Interact: Interact(); return true;
    case A::Torch: SetTorch(!Flashlight->IsVisible()); return true;
    case A::Noclip: ToggleNoclip(); return true;
    case A::Teleport: bTeleportMenuOpen = !bTeleportMenuOpen; ClearInput(); return true;
    case A::Close: bTeleportMenuOpen = false; ClearInput(); return true;
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
    const FString Prompt = Door == INDEX_NONE ? FString()
        : FString::Printf(TEXT("E: %s %s"), Vehicle->IsDoorOpen(Door) ? TEXT("Close") : TEXT("Open"), *Vehicle->Doors[Door].DisplayName);
    const TCHAR* Status = NoclipRefusedSeconds > 0 ? TEXT("Cannot leave noclip here: the capsule overlaps geometry")
        : bStuck ? TEXT("Movement blocked — press V for noclip")
        : Vehicle->bCollisionReady ? TEXT("Collision ready") : TEXT("Collision cooking…");
    const FString Player = Body->HasBody() ? Body->HUDLine() : BodyStatus;
    return FString::Printf(TEXT("%s · %.1f m/s · torch %s · %s cm from ship origin\n%s\n%s%s%s\nWASD move · Shift run · Space jump · Ctrl crouch · E interact · F torch · V noclip · T teleport"),
        bNoclip ? TEXT("Noclip") : bIsCrouched ? TEXT("Crouched") : bRunning ? TEXT("Running") : TEXT("Walking"),
        GetVelocity().Size() / 100, Flashlight->IsVisible() ? TEXT("on") : TEXT("off"),
        *GetVehicleRelativeLocation().ToCompactString(), *Prompt, Status, Player.IsEmpty() ? TEXT("") : TEXT("\n"), *Player);
}
