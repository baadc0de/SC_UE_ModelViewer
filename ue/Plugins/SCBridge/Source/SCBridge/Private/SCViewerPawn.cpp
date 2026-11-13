// ASCViewerPawn: orbit/fly camera math with eased focus transitions.
#include "SCViewerPawn.h"
#include "SCViewerSettings.h"
#include "Camera/CameraComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "UnrealClient.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCViewerCamera, Log, All);

namespace
{
constexpr float RotateDegreesPerPixel = 0.25f;
constexpr double ZoomPerNotch = 0.88;      // distance factor per wheel notch
constexpr float FlySpeedPerNotch = 1.25f;  // speed factor per wheel notch in fly mode
constexpr double FocusEase = 10.0;         // 1/s exponential approach of target and distance
constexpr double MinDistanceCm = 10.0;
constexpr double FitMargin = 1.08;        // breathing room around a framed box
constexpr double ObstacleClearanceCm = 50.0; // orbit camera distance kept from an obstacle in front of it
} // namespace

ASCViewerPawn::ASCViewerPawn()
{
    PrimaryActorTick.bCanEverTick = true;
    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
    RootComponent = Camera;
    Camera->bUsePawnControlRotation = false;
    SetActorEnableCollision(false);
}
void ASCViewerPawn::BeginPlay()
{
    Super::BeginPlay();
    FlySpeed = USCViewerSettings::Get()->FlySpeed;
    Target = TargetGoal = GetActorLocation() + GetActorForwardVector() * Distance;
    if (USCViewerSubsystem* S = USCViewerSubsystem::Get(this))
    {
        Mode = S->GetCameraMode();
        if (S->GetVehicle()) S->FocusComponent(S->GetFocused());
    }
    ApplyOrbit();
}
void ASCViewerPawn::SetCameraMode(ESCCameraMode NewMode)
{
    if (NewMode == Mode) return;
    if (Mode == ESCCameraMode::Fly && NewMode == ESCCameraMode::Orbit)
    {
        // Orbit the point the fly camera looks at: the first surface along the view within the last orbit distance, else
        // the point at that distance. The camera stays where it is.
        const FRotator Rot = GetActorRotation();
        Yaw = Rot.Yaw;
        Pitch = Rot.Pitch;
        FHitResult Hit;
        if (GetWorld() && GetWorld()->LineTraceSingleByChannel(Hit, GetActorLocation(), GetActorLocation() + Rot.Vector() * Distance, ECC_Camera,
                                                               FCollisionQueryParams(SCENE_QUERY_STAT(SCOrbitPivot), true, this)))
            Distance = FMath::Max(MinDistanceCm, Hit.Distance);
        Target = TargetGoal = GetActorLocation() + Rot.Vector() * Distance;
        DistanceGoal = Distance;
    }
    Mode = NewMode;
    Move = FVector::ZeroVector;
}
double ASCViewerPawn::ViewAspect() const
{
    const APlayerController* PC = Cast<APlayerController>(GetController());
    const ULocalPlayer* Player = PC ? PC->GetLocalPlayer() : nullptr;
    if (Player && Player->ViewportClient && Player->ViewportClient->Viewport)
    {
        const FIntPoint Pixels = Player->ViewportClient->Viewport->GetSizeXY();
        const double W = Pixels.X * Player->Size.X, H = Pixels.Y * Player->Size.Y;
        if (W > 0 && H > 0) return W / H;
    }
    return 16.0 / 9.0;
}
void ASCViewerPawn::ViewHalfAngles(double& HalfH, double& HalfV) const
{
    // Mirrors FMinimalViewInfo::CalculateProjectionMatrixGivenViewRectangle: FieldOfView is horizontal at the
    // camera's AspectRatio; under MaintainYFOV (project default) the vertical angle is held instead.
    const APlayerController* PC = Cast<APlayerController>(GetController());
    const ULocalPlayer* Player = PC ? PC->GetLocalPlayer() : nullptr;
    const EAspectRatioAxisConstraint Constraint = Player ? Player->AspectRatioAxisConstraint.GetValue() : AspectRatio_MaintainXFOV;
    const double Aspect = ViewAspect();
    const double Fov = FMath::DegreesToRadians(Camera->FieldOfView * 0.5);
    if (Constraint == AspectRatio_MaintainXFOV || (Constraint == AspectRatio_MajorAxisFOV && Aspect > 1.0))
    {
        HalfH = Fov;
        HalfV = FMath::Atan(FMath::Tan(HalfH) / Aspect);
    }
    else
    {
        HalfV = FMath::Atan(FMath::Tan(Fov) / FMath::Max(0.01, double(Camera->AspectRatio)));
        HalfH = FMath::Atan(FMath::Tan(HalfV) * Aspect);
    }
}
void ASCViewerPawn::FocusBounds(const FBox& Box)
{
    // Exact fit of the box for the current view direction: the smallest distance at which all eight corners lie
    // inside both effective half-angles, plus a small margin.
    double HalfH, HalfV;
    ViewHalfAngles(HalfH, HalfV);
    const FRotator Rot = Mode == ESCCameraMode::Orbit ? FRotator(Pitch, Yaw, 0) : GetActorRotation();
    const FRotationMatrix Basis(Rot);
    const FVector Forward = Basis.GetUnitAxis(EAxis::X), Right = Basis.GetUnitAxis(EAxis::Y), Up = Basis.GetUnitAxis(EAxis::Z);
    const FVector Centre = Box.GetCenter();
    FVector Corners[8];
    Box.GetVertices(Corners);
    double Fit = MinDistanceCm;
    for (const FVector& Corner : Corners)
    {
        const FVector P = Corner - Centre;
        // Camera at Centre - Forward * D; corner depth is D + P.Forward.
        Fit = FMath::Max(Fit, FMath::Abs(P | Right) / FMath::Tan(HalfH) - (P | Forward));
        Fit = FMath::Max(Fit, FMath::Abs(P | Up) / FMath::Tan(HalfV) - (P | Forward));
    }
    TargetGoal = Centre;
    DistanceGoal = Fit * FitMargin;
    UE_LOG(LogSCViewerCamera, Display, TEXT("VIEWER_FOCUS box_cm=%s aspect=%.3f half_h=%.1f half_v=%.1f distance_cm=%.0f"), *Box.GetSize().ToString(),
           ViewAspect(), FMath::RadiansToDegrees(HalfH), FMath::RadiansToDegrees(HalfV), DistanceGoal);
    if (Mode == ESCCameraMode::Fly)
    {
        SetActorLocation(TargetGoal - Forward * DistanceGoal);
        Target = TargetGoal;
        Distance = DistanceGoal;
    }
}
void ASCViewerPawn::SetView(const FVector& Location, const FRotator& Rotation, double PivotDistance)
{
    SetActorLocationAndRotation(Location, FRotator(Rotation.Pitch, Rotation.Yaw, 0));
    Yaw = Rotation.Yaw;
    Pitch = FMath::Clamp(Rotation.Pitch, -89.f, 89.f);
    if (PivotDistance > 0) Distance = FMath::Max(double(MinDistanceCm), PivotDistance);
    DistanceGoal = Distance;
    Target = TargetGoal = Location + GetActorForwardVector() * Distance;
}
void ASCViewerPawn::RotateDrag(const FVector2D& Delta)
{
    if (Mode == ESCCameraMode::Orbit)
    {
        Yaw += Delta.X * RotateDegreesPerPixel;
        Pitch = FMath::Clamp(Pitch - Delta.Y * RotateDegreesPerPixel, -89.f, 89.f);
        return;
    }
    FRotator Rot = GetActorRotation();
    Rot.Yaw += Delta.X * RotateDegreesPerPixel;
    Rot.Pitch = FMath::Clamp(Rot.Pitch - Delta.Y * RotateDegreesPerPixel, -89.f, 89.f);
    Rot.Roll = 0;
    SetActorRotation(Rot);
}
void ASCViewerPawn::PanDrag(const FVector2D& Delta)
{
    // Delta is in view-height fractions: a full-height drag moves the target by the view span at the target distance.
    double HalfH, HalfV;
    ViewHalfAngles(HalfH, HalfV);
    const double PixelCm = 2.0 * Distance * FMath::Tan(HalfV);
    const FRotator Rot = Mode == ESCCameraMode::Orbit ? FRotator(Pitch, Yaw, 0) : GetActorRotation();
    const FVector Offset = (-FRotationMatrix(Rot).GetUnitAxis(EAxis::Y) * Delta.X + FRotationMatrix(Rot).GetUnitAxis(EAxis::Z) * Delta.Y) * PixelCm;
    if (Mode == ESCCameraMode::Orbit)
    {
        Target += Offset;
        TargetGoal += Offset;
    }
    else
        AddActorWorldOffset(Offset);
}
void ASCViewerPawn::Wheel(float Notches)
{
    if (Mode == ESCCameraMode::Orbit)
    {
        DistanceGoal = FMath::Max(MinDistanceCm, DistanceGoal * FMath::Pow(ZoomPerNotch, Notches));
        return;
    }
    FlySpeed = FMath::Clamp(FlySpeed * FMath::Pow(FlySpeedPerNotch, Notches), 10.f, 100000.f);
    USCViewerSettings::Get()->FlySpeed = FlySpeed;
}
void ASCViewerPawn::SetMoveKey(int32 Axis, float Value)
{
    if (Axis >= 0 && Axis < 3) Move[Axis] = Value;
}
void ASCViewerPawn::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (Mode == ESCCameraMode::Orbit)
    {
        const double Alpha = 1.0 - FMath::Exp(-FocusEase * DeltaSeconds);
        Target = FMath::Lerp(Target, TargetGoal, Alpha);
        Distance = FMath::Lerp(Distance, DistanceGoal, Alpha);
        ApplyOrbit();
        return;
    }
    if (!Move.IsNearlyZero())
    {
        const FRotationMatrix R(GetActorRotation());
        const FVector Dir = R.GetUnitAxis(EAxis::X) * Move.X + R.GetUnitAxis(EAxis::Y) * Move.Y + FVector::UpVector * Move.Z;
        AddActorWorldOffset(Dir.GetClampedToMaxSize(1.0) * FlySpeed * SpeedModifier * DeltaSeconds);
    }
}
void ASCViewerPawn::ApplyOrbit()
{
    const FRotator Rot(Pitch, Yaw, 0);
    double Reach = Distance;
    // Like a spring arm: an environment wall or pillar between the model and the camera pulls the camera in front of it.
    // Only a surface facing the target counts: a target on or under a floor (orbit entered from fly mode) must not drag
    // the camera to the floor's underside.
    FHitResult Hit;
    if (AActor* Obstacle = OrbitObstacle.Get();
        Obstacle && Obstacle->ActorLineTraceSingle(Hit, Target, Target - Rot.Vector() * Distance, ECC_Camera, FCollisionQueryParams(SCENE_QUERY_STAT(SCOrbitObstacle), true)) &&
        !Hit.bStartPenetrating && FVector::DotProduct(Hit.ImpactNormal, Rot.Vector()) > 0)
        Reach = FMath::Max(MinDistanceCm, Hit.Distance - ObstacleClearanceCm);
    SetActorLocationAndRotation(Target - Rot.Vector() * Reach, Rot);
}
