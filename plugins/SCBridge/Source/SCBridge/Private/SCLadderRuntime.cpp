// Authored get-on/off root motion and a height-sampled ladder climb loop.
#include "SCLadderRuntime.h"
#include "SCFirstPersonCharacter.h"
#include "SCPlayerBody.h"
#include "SCVehicle.h"
#include "SCViewerSubsystem.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCLadder, Log, All);

namespace
{
const FName MountBottom(TEXT("mount_bottom")), MountTop(TEXT("mount_top")), Climb(TEXT("climb"));
const FName DismountBottom(TEXT("dismount_bottom"));
const FName DismountTop[3][2] = {
    {TEXT("dismount_back_left"), TEXT("dismount_back_right")},
    {TEXT("dismount_left_left"), TEXT("dismount_left_right")},
    {TEXT("dismount_right_left"), TEXT("dismount_right_right")}
};

bool Faces(const FVector& A, const FVector& B, float Degrees)
{
    return Degrees <= 0.f || FVector::DotProduct(A, B) >= FMath::Cos(FMath::DegreesToRadians(Degrees));
}
}

bool FSCLadderRuntime::OwnsMovement() const { return Phase != EPhase::Walk; }

FSCLadderRuntime::FApproach FSCLadderRuntime::FindApproach(const ASCFirstPersonCharacter& Character, bool bAutomatic) const
{
    if (!Character.Vehicle || !Character.Body->HasBody() || Character.bNoclip || Character.bIsCrouched ||
        Character.bMenuOpen || Character.bTeleportMenuOpen || RemountDelay > 0.f ||
        !Character.GetCharacterMovement()->IsMovingOnGround()) return {};
    const FVector Feet = Character.GetActorLocation() -
        FVector::UpVector * Character.GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    const FRotator Heading(0, Character.GetViewRotation().Yaw, 0);
    const FVector Forward = Heading.Vector();
    const FVector Input = (Forward * Character.Move.X + FRotationMatrix(Heading).GetUnitAxis(EAxis::Y) * Character.Move.Y).GetSafeNormal();
    if (bAutomatic && Input.IsNearlyZero()) return {};
    FApproach Best;
    double BestDistance = TNumericLimits<double>::Max();
    for (int32 I = 0; I < Character.Vehicle->LadderAnchors.Num(); ++I)
    {
        const FSCLadderAnchor& Ladder = Character.Vehicle->LadderAnchors[I];
        const USceneComponent* Anchor = Ladder.Anchor.Get();
        if (!Anchor || (bAutomatic && !Ladder.Data.bAutoMount)) continue;
        const FTransform Frame = Anchor->GetComponentTransform();
        const FVector Local = Frame.InverseTransformPosition(Feet);
        const bool bTop = Local.Z > Ladder.Data.Height * 50.f;
        const FSCLadderEnd& EndParams = bTop ? Ladder.Data.Top : Ladder.Data.Bottom;
        const FVector Towards = Frame.TransformPosition(FVector(EndParams.RadiusOffset * 100.f, 0, bTop ? Ladder.Data.Height * 100.f : 0.f)) - Feet;
        const double Radius = 100.0 * (Character.IsRunning() ? EndParams.WalkRunRadius.Y : EndParams.WalkRunRadius.X);
        const double Distance = Towards.SizeSquared();
        if (Distance >= BestDistance || Distance > FMath::Square(Radius)) continue;
        const FVector Approach = Towards.GetSafeNormal2D();
        if (!Approach.IsNearlyZero() && (!Faces(Forward, Approach, EndParams.FacingAngle) ||
            (bAutomatic && !Faces(Input, Approach, EndParams.ApproachAngle)))) continue;
        Best = {I, bTop, bTop || Local.X <= 0.0 ? 1.f : -1.f};
        BestDistance = Distance;
    }
    return Best;
}

bool FSCLadderRuntime::HaveClips(ASCFirstPersonCharacter& Character, const FSCLadderDefinition& Data)
{
    if (Kit.Get() != Character.Body->GetKit())
    {
        Request.Cancel();
        Clips.Reset();
        bLoading = bClipLoadFailed = false;
        Kit = Character.Body->GetKit();
    }
    if (bClipLoadFailed) return false;
    if (bLoading)
    {
        auto Result = Request.Take();
        if (!Result.IsSet()) return false;
        bLoading = false;
        bClipLoadFailed = !Result->Error.IsEmpty();
        if (bClipLoadFailed)
        {
            UE_LOG(LogSCLadder, Warning, TEXT("LADDER_CLIPS %s"), *Result->Error);
            return false;
        }
        for (auto& [Name, Clip] : Result->Clips) Clips.Add(Name, MoveTemp(Clip));
    }
    TArray<FString> Missing;
    for (const auto& [State, Motion] : Data.Actions)
        if (!Clips.Contains(Motion.Clip)) Missing.AddUnique(Motion.Clip.ToString());
    if (Missing.IsEmpty()) return true;
    bLoading = Request.Start(*Character.Body, USCViewerSubsystem::Get(&Character), MoveTemp(Missing), TEXT("SCLadderClipsPackage="));
    bClipLoadFailed = !bLoading;
    return false;
}

FTransform FSCLadderRuntime::OnLadder(const FSCLadderAnchor& Ladder, float Z) const
{
    const FTransform Frame = Ladder.Anchor->GetComponentTransform();
    const FVector Forward = Frame.GetUnitAxis(EAxis::X) * FacingSign;
    const FVector Up = Frame.GetUnitAxis(EAxis::Z);
    const FQuat Rotation = FRotationMatrix::MakeFromXZ(Forward, Up).ToQuat();
    const FVector Root = Frame.TransformPosition(FVector(-FacingSign * Ladder.Data.HorizontalOffset * 100.f, 0, Z));
    return FTransform(Rotation, Root - Rotation.RotateVector(BodyInActor.GetLocation()));
}

FTransform FSCLadderRuntime::RootMotion(const ASCFirstPersonCharacter& Character, const FSCPlayerClip& Clip, float Seconds) const
{
    return BodyInActor.Inverse() * Character.Body->GetAnimator().ActionRootDelta(Clip, Seconds) * BodyInActor;
}

void FSCLadderRuntime::StartAction(const FSCLadderAnchor& Ladder, FName State, EPhase NewPhase)
{
    Phase = NewPhase;
    Action = State;
    ActionClock = 0;
    UE_LOG(LogSCLadder, Display, TEXT("LADDER_STATE id=%s state=%s clip=%s height_cm=%.2f"),
        *Ladder.EntityId, *State.ToString(), *Ladder.Data.Actions.FindChecked(State).Clip.ToString(), Height);
}

bool FSCLadderRuntime::TryMount(ASCFirstPersonCharacter& Character, const FApproach& Approach)
{
    if (!Character.Vehicle || !Character.Vehicle->LadderAnchors.IsValidIndex(Approach.Index)) return false;
    const FSCLadderAnchor& Ladder = Character.Vehicle->LadderAnchors[Approach.Index];
    if (!Ladder.Anchor.IsValid()) return false;
    if (!HaveClips(Character, Ladder.Data)) return bLoading;
    const ESCWeaponState Weapon = Character.Body->GetWeaponState();
    if (Weapon == ESCWeaponState::Armed) Character.Body->RequestSlot(INDEX_NONE);
    if (Weapon != ESCWeaponState::Unarmed) return true;

    const auto& Data = Ladder.Data;
    const FName Mount = Approach.bTop ? MountTop : MountBottom;
    const FSCLadderAction& Motion = Data.Actions.FindChecked(Mount);
    const FSCPlayerClip& Clip = *Clips.FindChecked(Motion.Clip);
    BodyInActor = Character.Body->BodyFrame().GetRelativeTransform(Character.GetActorTransform());
    BodyInActor.SetScale3D(FVector::OneVector);
    const FSCPlayerClip& Loop = *Clips.FindChecked(Data.Actions.FindChecked(Climb).Clip);
    Cycle = RootMotion(Character, Loop, Loop.Duration).GetLocation().Z;
    if (!FMath::IsFinite(Cycle) || Cycle <= UE_KINDA_SMALL_NUMBER || Motion.Speed <= 0.f) return false;
    Height = Approach.bTop
        ? Data.Height * 100.f + RootMotion(Character, Clip, Clip.Duration).GetLocation().Z
        : Data.MountBottom * 100.f;
    if (Height < 0.f || Height > Data.Height * 100.f) return false;

    FacingSign = Approach.FacingSign;
    Host = Character.Vehicle;
    Active = Approach.Index;
    Start = Character.GetActorTransform().GetRelativeTransform(Ladder.Anchor->GetComponentTransform());
    ReturnToApproach = Start;
    Velocity = 0.f;
    RelativeView = FRotator::ZeroRotator;
    bNeedFreshInput = Approach.bTop;
    Character.ClearViewHold();
    Character.Body->SetAiming(false);
    Character.Body->SetTrigger(false);
    Character.SetBase(static_cast<FMovementBaseInterfaceData*>(nullptr));
    Character.GetCharacterMovement()->StopMovementImmediately();
    Character.GetCharacterMovement()->DisableMovement();
    SavedCollision = Character.GetCapsuleComponent()->GetCollisionEnabled();
    if (Data.bIgnoreStaticCollision) Character.GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    bSavedControllerYaw = Character.bUseControllerRotationYaw;
    Character.bUseControllerRotationYaw = false;
    bManualPending = false;
    StartAction(Ladder, Mount, EPhase::Mount);
    return true;
}

bool FSCLadderRuntime::TryDismount(ASCFirstPersonCharacter& Character, const FSCLadderAnchor& Ladder, bool bTop)
{
    int32 Choices = bTop ? 3 : 1;
    const auto& Data = Ladder.Data;
    for (int32 Direction = 0; Direction < Choices; ++Direction)
    {
        if (bTop && (!Data.TopExits[Direction] || Height < (Data.Height - Data.StopTop[Direction]) * 100.f)) continue;
        const bool bRightFoot = FMath::Fmod(Height, Cycle) >= Cycle * .5f;
        const FName State = bTop ? DismountTop[Direction][bRightFoot] : DismountBottom;
        const FSCLadderAction* Motion = Data.Actions.Find(State);
        if (!Motion || Motion->Speed <= 0.f) continue;
        const FSCPlayerClip& Clip = *Clips.FindChecked(Motion->Clip);
        const FTransform Current = OnLadder(Ladder, Height);
        FTransform Target = RootMotion(Character, Clip, Clip.Duration) * Current;
        const FVector ExpectedFeet = Target.GetLocation() -
            FVector::UpVector * Character.GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        FVector Centre;
        bool bCrouched = false;
        if (!Character.ResolveSpawn(ExpectedFeet, Centre, bCrouched) || bCrouched) continue;
        Target.SetLocation(Centre);
        const FTransform Anchor = Ladder.Anchor->GetComponentTransform();
        Start = Current.GetRelativeTransform(Anchor);
        End = Target.GetRelativeTransform(Anchor);
        RemountDelay = bTop ? Data.Top.RemountDelay : Data.Bottom.RemountDelay;
        StartAction(Ladder, State, EPhase::Dismount);
        return true;
    }
    return false;
}

bool FSCLadderRuntime::ApplyLook(ASCFirstPersonCharacter& Character, FVector2D Pixels)
{
    if (!OwnsMovement()) return false;
    if (!Character.Controller || !Host.IsValid() || !Host->LadderAnchors.IsValidIndex(Active)) return true;
    const FSCLadderDefinition& Data = Host->LadderAnchors[Active].Data;
    RelativeView.Yaw = FMath::Clamp(RelativeView.Yaw + Pixels.X * .2, -double(Data.HorizontalView), double(Data.HorizontalView));
    RelativeView.Pitch = FMath::Clamp(RelativeView.Pitch - Pixels.Y * .2, -double(Data.UpView), double(Data.UpView));
    Character.Controller->SetControlRotation((Character.GetActorQuat() * RelativeView.Quaternion()).Rotator());
    return true;
}

void FSCLadderRuntime::Restore(ASCFirstPersonCharacter& Character)
{
    Phase = EPhase::Walk;
    Active = INDEX_NONE;
    Character.Body->SetSeatAction(nullptr, 0.f, 1.f, 0.f);
    Character.bUseControllerRotationYaw = bSavedControllerYaw;
    Character.GetCapsuleComponent()->SetCollisionEnabled(SavedCollision);
    Character.SetActorRotation(FRotator(0, Character.GetActorRotation().Yaw, 0));
    Character.GetCharacterMovement()->SetMovementMode(MOVE_Walking);
    Character.ClearInput();
    UE_LOG(LogSCLadder, Display, TEXT("LADDER_STATE state=walking"));
}

void FSCLadderRuntime::Recover(ASCFirstPersonCharacter& Character)
{
    if (!OwnsMovement()) return;
    if (Character.IsActorBeingDestroyed() || !Character.GetWorld())
    {
        Restore(Character);
        return;
    }
    FTransform Safe = Character.LastSpawn;
    if (Host.IsValid() && !Host->IsActorBeingDestroyed() && Host->LadderAnchors.IsValidIndex(Active))
        if (const USceneComponent* Anchor = Host->LadderAnchors[Active].Anchor.Get())
            Safe = ReturnToApproach * Anchor->GetComponentTransform();

    auto Standing = [&](FTransform& At) {
        FVector Centre;
        bool bCrouched = false;
        const FVector Feet = At.GetLocation() - FVector::UpVector * Character.GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        if (!Character.ResolveSpawn(Feet, Centre, bCrouched) || bCrouched) return false;
        At.SetLocation(Centre);
        return true;
    };
    bool bClear = Standing(Safe);
    if (!bClear)
    {
        Safe = Character.LastSpawn;
        bClear = Standing(Safe);
    }
    RemountDelay = Host.IsValid() && Host->LadderAnchors.IsValidIndex(Active)
        ? FMath::Max(Host->LadderAnchors[Active].Data.Bottom.RemountDelay, Host->LadderAnchors[Active].Data.Top.RemountDelay)
        : 0.f;
    Character.SetActorTransform(Safe, false, nullptr, ETeleportType::TeleportPhysics);
    if (!bClear) Character.SetActorEnableCollision(false);
    Restore(Character);
    if (!bClear) Character.ToggleNoclip();
}

void FSCLadderRuntime::Stop(ASCFirstPersonCharacter& Character)
{
    Recover(Character);
    Request.Cancel();
    bLoading = bClipLoadFailed = bManualPending = false;
    Clips.Reset();
    Kit.Reset();
}

bool FSCLadderRuntime::Interact(ASCFirstPersonCharacter& Character)
{
    if (OwnsMovement())
    {
        Recover(Character);
        return true;
    }
    const bool bHandled = TryMount(Character, FindApproach(Character, false));
    bManualPending = bHandled && !OwnsMovement();
    return bHandled;
}

FString FSCLadderRuntime::State() const
{
    return FString::Printf(TEXT("LADDER_STATUS state=%s index=%d height_cm=%.2f loading=%d"),
        !OwnsMovement() ? TEXT("walking") : *Action.ToString(), Active, Height, bLoading);
}

void FSCLadderRuntime::Tick(ASCFirstPersonCharacter& Character, float Delta)
{
    if (!OwnsMovement())
    {
        RemountDelay = FMath::Max(RemountDelay - Delta, 0.f);
        const FApproach Approach = FindApproach(Character, !bManualPending);
        if (!TryMount(Character, Approach)) bManualPending = false;
        return;
    }
    if (!Host.IsValid() || Host->IsActorBeingDestroyed() || Host.Get() != Character.Vehicle ||
        Kit.Get() != Character.Body->GetKit() || !Host->LadderAnchors.IsValidIndex(Active) ||
        !Host->LadderAnchors[Active].Anchor.IsValid())
    {
        Stop(Character);
        return;
    }
    const FSCLadderAnchor& Ladder = Host->LadderAnchors[Active];
    const FSCLadderDefinition& Data = Ladder.Data;
    const FSCLadderAction& Motion = Data.Actions.FindChecked(Action);
    const FSCPlayerClip* Clip = Clips.FindChecked(Motion.Clip).Get();
    ActionClock += Delta;

    FTransform Frame;
    float Sample = FMath::Min(ActionClock * Motion.Speed, Clip->Duration);
    const float Blend = Motion.Blend > 0.f ? FMath::Min(ActionClock / Motion.Blend, 1.f) : 1.f;
    const FTransform Anchor = Ladder.Anchor->GetComponentTransform();
    if (Phase == EPhase::Climb)
    {
        if (FMath::IsNearlyZero(Character.Move.X)) bNeedFreshInput = false;
        const float Input = bNeedFreshInput ? 0.f : Character.Move.X;
        const float PerRung = Cycle / Data.TravelInRungs;
        const float Want = Input * (Input < 0.f ? Data.SpeedUpDown.Y : Data.SpeedUpDown.X) * PerRung;
        Velocity = FMath::FInterpConstantTo(Velocity, Want, Delta,
            (FMath::IsNearlyZero(Want) ? Data.InertiaDecay : Data.Acceleration) * PerRung);
        const float PreviousHeight = Height;
        Height = FMath::Clamp(Height + Velocity * Delta, Data.StopBottom * 100.f, Data.Height * 100.f);
        if ((Velocity < 0.f && Height <= Data.StopBottom * 100.f) || Velocity > 0.f)
        {
            if (TryDismount(Character, Ladder, Velocity > 0.f)) return;
            if (Velocity > 0.f && (Data.TopExits.X || Data.TopExits.Y || Data.TopExits.Z))
            {
                const double Stop = FMath::Max3(Data.TopExits.X ? Data.StopTop.X : 0.0,
                    Data.TopExits.Y ? Data.StopTop.Y : 0.0, Data.TopExits.Z ? Data.StopTop.Z : 0.0);
                const float Ceiling = float((Data.Height - Stop) * 100.);
                Height = FMath::Min(Height, FMath::Max(PreviousHeight, Ceiling));
            }
        }
        Frame = OnLadder(Ladder, Height);
        Sample = (FMath::Fmod(Height, Cycle) / Cycle) * Clip->Duration;
    }
    else
    {
        const FTransform Target = Phase == EPhase::Mount ? OnLadder(Ladder, Height) : End * Anchor;
        const FTransform Source = Start * Anchor;
        const FTransform DeltaRoot = RootMotion(Character, *Clip, Sample);
        const FTransform Align = RootMotion(Character, *Clip, Clip->Duration).Inverse() * Target;
        FTransform Warped;
        const float Adjust = Motion.PositionAdjust > 0.f ? FMath::Min(ActionClock / Motion.PositionAdjust, 1.f) : 1.f;
        Warped.Blend(Source, Align, Adjust);
        Frame = DeltaRoot * Warped;
    }

    FHitResult Hit;
    Character.SetActorTransform(Frame, !Data.bIgnoreStaticCollision, &Hit, ETeleportType::TeleportPhysics);
    if (Hit.bBlockingHit)
    {
        if (Phase != EPhase::Climb) Recover(Character);
        else Height = float(Anchor.InverseTransformPosition(
            (BodyInActor * Character.GetActorTransform()).GetLocation()).Z);
        return;
    }
    ApplyLook(Character, FVector2D::ZeroVector);
    Character.Body->SetSeatAction(Clip, Sample, Blend, float(RelativeView.Pitch));
    if (Sample < Clip->Duration || Phase == EPhase::Climb) return;
    if (Phase == EPhase::Mount) StartAction(Ladder, Climb, EPhase::Climb);
    else
    {
        FVector Centre;
        bool bCrouched = false;
        const FVector Feet = Frame.GetLocation() - FVector::UpVector * Character.GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        if (!Character.ResolveSpawn(Feet, Centre, bCrouched) || bCrouched)
        {
            const FTransform Climbing = OnLadder(Ladder, Height);
            Character.SetActorTransform(Climbing, false, nullptr, ETeleportType::TeleportPhysics);
            Velocity = 0.f;
            bNeedFreshInput = true;
            StartAction(Ladder, Climb, EPhase::Climb);
            return;
        }
        Character.SetActorLocation(Centre, false, nullptr, ETeleportType::TeleportPhysics);
        Restore(Character);
    }
}
