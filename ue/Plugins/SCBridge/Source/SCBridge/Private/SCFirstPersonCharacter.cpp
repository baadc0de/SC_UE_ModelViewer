// ACharacter owns gravity, step-up, crouch and capsule sweeps; no ship-specific navigation rules.
#include "SCFirstPersonCharacter.h"
#include "SCVehicle.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/InputComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

ASCFirstPersonCharacter::ASCFirstPersonCharacter()
{
    GetCapsuleComponent()->InitCapsuleSize(34, 88);
    GetCapsuleComponent()->OnComponentHit.AddDynamic(this, &ASCFirstPersonCharacter::CapsuleHit);
    auto* Movement = GetCharacterMovement();
    Movement->MaxWalkSpeed = 400;
    Movement->MaxFlySpeed = 650;
    Movement->SetCrouchedHalfHeight(44);
    Movement->MaxStepHeight = 45;
    Movement->GetNavAgentPropertiesRef().bCanCrouch = true;
    Movement->bRunPhysicsWithNoController = true; // scripted commandlet walks use the same movement component
    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
    Camera->SetupAttachment(GetCapsuleComponent());
    Camera->bUsePawnControlRotation = true;
    Camera->SetRelativeLocation(FVector(0, 0, BaseEyeHeight));
    Flashlight = CreateDefaultSubobject<USpotLightComponent>(TEXT("ExplorationFlashlight"));
    Flashlight->SetupAttachment(Camera);
    // Viewer-owned torch, not an imported game light.
    Flashlight->SetIntensity(2000);
    Flashlight->SetAttenuationRadius(2000);
    Flashlight->SetOuterConeAngle(35);
    Flashlight->SetVisibility(false);
}
void ASCFirstPersonCharacter::SetRunning(bool bRun) { GetCharacterMovement()->MaxWalkSpeed = bRun ? 650 : 400; }
void ASCFirstPersonCharacter::SetMoveInput(FVector Input) { Move = Input.GetClampedToMaxSize(1); }
void ASCFirstPersonCharacter::ClearInput() { Held.Empty(); Move = FVector::ZeroVector; SetRunning(false); StopJumping(); UnCrouch(); }
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
void ASCFirstPersonCharacter::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!Vehicle || Vehicle->IsActorBeingDestroyed()) { ClearInput(); return; }
    Camera->SetRelativeLocation(FVector(0, 0, BaseEyeHeight));
    const FRotator Rotation = GetControlRotation();
    const FRotator Heading(0, Rotation.Yaw, 0);
    if (!bTeleportMenuOpen)
    {
        AddMovementInput((bNoclip ? Rotation : Heading).Vector(), Move.X);
        AddMovementInput(FRotationMatrix(Heading).GetUnitAxis(EAxis::Y), Move.Y);
        if (bNoclip) AddMovementInput(FVector::UpVector, Move.Z);
    }
    const bool WantsMove = !Move.IsNearlyZero() && !bNoclip && !bTeleportMenuOpen;
    BlockedSeconds = WantsMove && FVector::DistSquared(LastPosition, GetActorLocation()) < 1 ? BlockedSeconds + DeltaSeconds : 0;
    bStuck = BlockedSeconds >= 2;
    LastPosition = GetActorLocation();
    const FBox Bounds = Vehicle->ExplorationBounds();
    if (!bNoclip && Bounds.IsValid && GetActorLocation().Z < FMath::Min(0.0, Bounds.Min.Z) - GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 4)
    {
        SetActorTransform(LastSpawn, false, nullptr, ETeleportType::TeleportPhysics);
        GetCharacterMovement()->StopMovementImmediately();
        ++RecoveryCount;
        UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_RECOVER count=%d"), RecoveryCount);
    }
}
bool ASCFirstPersonCharacter::ResolveSpawn(FVector Hint, FVector& Centre) const
{
    const auto* Capsule = GetCapsuleComponent();
    const double Radius = Capsule->GetScaledCapsuleRadius(), Half = 88;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCSpawn), false, this);
    const double Step = GetCharacterMovement()->MaxStepHeight;
    // Search a capsule-sized neighbourhood of the helper; never use an asset-name offset.
    for (int32 Ring = 0; Ring <= 4; ++Ring)
        for (int32 Direction = 0; Direction < (Ring ? 8 : 1); ++Direction)
        {
            const double Angle = Direction * UE_DOUBLE_PI / 4;
            const FVector Offset(FMath::Cos(Angle) * Ring * Radius * 2, FMath::Sin(Angle) * Ring * Radius * 2, 0);
            FHitResult Hit;
            if (!GetWorld()->LineTraceSingleByChannel(Hit, Hint + Offset + FVector(0, 0, Half + Step),
                    Hint + Offset - FVector(0, 0, Half * 4), ECC_Pawn, Params) ||
                Hit.ImpactNormal.Z < GetCharacterMovement()->GetWalkableFloorZ()) continue;
            const FVector Candidate = Hit.ImpactPoint + FVector(0, 0, Half + 2.5);
            if (!GetWorld()->OverlapBlockingTestByChannel(Candidate, FQuat::Identity, ECC_Pawn,
                    FCollisionShape::MakeCapsule(Radius, Half), Params))
            {
                // A clear capsule in a tiny pocket is not a usable entrance. Require a
                // two-capsule-width step with floor support, using the same collision channel.
                for (int32 Heading=0; Heading<8; ++Heading)
                {
                    const FVector Ahead = Candidate + FRotator(0,Heading*45,0).Vector()*Radius*4;
                    FHitResult Floor, Obstacle;
                    if (!GetWorld()->LineTraceSingleByChannel(Floor,Ahead+FVector(0,0,Step),Ahead-FVector(0,0,Half+Step),ECC_Pawn,Params) ||
                        Floor.ImpactNormal.Z<GetCharacterMovement()->GetWalkableFloorZ()) continue;
                    const FVector End = Floor.ImpactPoint+FVector(0,0,Half+2.5);
                    if (FMath::Abs(End.Z-Candidate.Z)>Step) continue;
                    if (!GetWorld()->SweepSingleByChannel(Obstacle,Candidate,End,FQuat::Identity,ECC_Pawn,
                            FCollisionShape::MakeCapsule(Radius,Half),Params))
                    { Centre = Candidate; return true; }
                }
            }
        }
    return false;
}
bool ASCFirstPersonCharacter::TeleportToFloor(FVector Floor)
{
    FVector Centre;
    if (!ResolveSpawn(Floor, Centre)) return false;
    UnCrouch();
    SetActorLocation(Centre, false, nullptr, ETeleportType::TeleportPhysics);
    GetCharacterMovement()->StopMovementImmediately();
    LastSpawn = GetActorTransform();
    LastPosition = Centre;
    BlockedSeconds = 0;
    bStuck = false;
    bTeleportMenuOpen = false;
    UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_SPAWN location=%s"), *Centre.ToString());
    return true;
}
bool ASCFirstPersonCharacter::TeleportToSpawn(int32 Index)
{
    if (!Vehicle || !Vehicle->SpawnPoints.IsValidIndex(Index)) return false;
    const auto& Point = Vehicle->SpawnPoints[Index];
    return TeleportToFloor((Point.Transform * Vehicle->GetActorTransform()).GetLocation());
}
bool ASCFirstPersonCharacter::Explore(ASCVehicle* InVehicle)
{
    Vehicle = InVehicle;
    if (!Vehicle || !Vehicle->bCollisionReady) return false;
    TArray<int32> Candidates;
    for (int32 I=0; I<Vehicle->SpawnPoints.Num(); ++I) Candidates.Add(I);
    Candidates.StableSort([&](int32 A, int32 B) { return Vehicle->SpawnPoints[A].Transform.GetLocation().Z < Vehicle->SpawnPoints[B].Transform.GetLocation().Z; });
    for (FName Kind : {FName(TEXT("Entrance")), FName(TEXT("SeatAccess")), FName(TEXT("Seat")), FName(TEXT("Interior")), FName(TEXT("Hull"))})
        for (int32 I : Candidates)
            if (Vehicle->SpawnPoints[I].Kind == Kind && TeleportToSpawn(I))
            {
                UE_LOG(LogTemp,Display,TEXT("FIRST_PERSON_BEST kind=%s label=%s"),*Kind.ToString(),*Vehicle->SpawnPoints[I].DisplayName);
                return true;
            }
    return false;
}
bool ASCFirstPersonCharacter::Interact()
{
    return Vehicle && Vehicle->ToggleDoor(Vehicle->NearestDoor(Camera->GetComponentLocation()));
}
void ASCFirstPersonCharacter::ToggleNoclip()
{
    if (bNoclip)
    {
        FCollisionQueryParams Params(SCENE_QUERY_STAT(SCNoclip), false, this);
        if (GetWorld()->OverlapBlockingTestByChannel(GetActorLocation(), FQuat::Identity, ECC_Pawn,
                GetCapsuleComponent()->GetCollisionShape(), Params)) { bStuck = true; return; }
    }
    bNoclip = !bNoclip;
    SetActorEnableCollision(!bNoclip);
    GetCharacterMovement()->SetMovementMode(bNoclip ? MOVE_Flying : MOVE_Falling);
    GetCharacterMovement()->StopMovementImmediately();
}
bool ASCFirstPersonCharacter::HandleKey(FKey Key, bool bDown)
{
    const TArray<FKey> MoveKeys = {EKeys::W,EKeys::S,EKeys::A,EKeys::D,EKeys::Q,EKeys::SpaceBar,EKeys::LeftShift,EKeys::LeftControl};
    if (MoveKeys.Contains(Key))
    {
        if (bDown) Held.Add(Key); else Held.Remove(Key);
        auto Axis = [&](FKey Positive, FKey Negative) { return float(Held.Contains(Positive)) - float(Held.Contains(Negative)); };
        SetMoveInput(FVector(Axis(EKeys::W,EKeys::S), Axis(EKeys::D,EKeys::A), Axis(EKeys::SpaceBar,EKeys::Q)));
        SetRunning(Held.Contains(EKeys::LeftShift));
        if (Key == EKeys::SpaceBar && !bNoclip) { if (bDown) Jump(); else StopJumping(); }
        if (Key == EKeys::LeftControl) { if (bDown) Crouch(); else UnCrouch(); }
        return true;
    }
    if (!bDown) return false;
    if (Key == EKeys::E) { Interact(); return true; }
    if (Key == EKeys::F) { Flashlight->ToggleVisibility(); return true; }
    if (Key == EKeys::V) { ToggleNoclip(); return true; }
    if (Key == EKeys::T) { bTeleportMenuOpen = !bTeleportMenuOpen; ClearInput(); return true; }
    if (Key == EKeys::Escape) { bTeleportMenuOpen = false; ClearInput(); return true; }
    return false;
}
void ASCFirstPersonCharacter::SetupPlayerInputComponent(UInputComponent* Input)
{
    Super::SetupPlayerInputComponent(Input);
    for (FKey Key : {EKeys::W,EKeys::S,EKeys::A,EKeys::D,EKeys::Q,EKeys::SpaceBar,EKeys::LeftShift,EKeys::LeftControl,EKeys::E,EKeys::F,EKeys::V,EKeys::T,EKeys::Escape})
    {
        for (EInputEvent Event : {IE_Pressed, IE_Released})
        {
            FInputKeyBinding Binding(FInputChord(Key), Event);
            Binding.KeyDelegate.GetDelegateForManualSet().BindLambda([this,Key,Event] { HandleKey(Key, Event == IE_Pressed); });
            Input->KeyBindings.Add(MoveTemp(Binding));
        }
    }
    Input->BindAxisKey(EKeys::MouseX).AxisDelegate.GetDelegateForManualSet().BindLambda([this](float Value) { Look(FVector2D(Value,0)); });
    Input->BindAxisKey(EKeys::MouseY).AxisDelegate.GetDelegateForManualSet().BindLambda([this](float Value) { Look(FVector2D(0,-Value)); });
}
FString ASCFirstPersonCharacter::HUDText() const
{
    if (!Vehicle) return FString();
    const int32 Door = Vehicle->NearestDoor(Camera->GetComponentLocation());
    return FString::Printf(TEXT("%s · %.1f m/s · %s cm from ship origin\n%s\n%s\nWASD move · Shift run · Space jump · Ctrl crouch · E interact · F torch · V noclip · T teleport"),
        bNoclip ? TEXT("Noclip") : TEXT("Walking"), GetVelocity().Size()/100,
        *Vehicle->GetActorTransform().InverseTransformPosition(GetActorLocation()).ToCompactString(),
        Door == INDEX_NONE ? TEXT("") : *FString::Printf(TEXT("E: %s %s"), Vehicle->Doors[Door].bOpen ? TEXT("Close") : TEXT("Open"), *Vehicle->Doors[Door].DisplayName),
        bStuck ? TEXT("Movement blocked — press V for noclip") : Vehicle->bCollisionReady ? TEXT("Collision ready") : TEXT("Collision cooking…"));
}
