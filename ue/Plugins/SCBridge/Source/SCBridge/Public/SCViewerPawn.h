// Viewer camera pawn: orbit (target/yaw/pitch/distance, fit to bounds) and free-fly (WASD/QE + mouse look).
// Input arrives from the UI's viewport input widget, so it works with any input mode and over UMG/Slate panels.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "SCViewerSubsystem.h"
#include "SCViewerPawn.generated.h"

class UCameraComponent;

UCLASS()
class SCBRIDGE_API ASCViewerPawn : public APawn
{
    GENERATED_BODY()
public:
    ASCViewerPawn();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;

    UFUNCTION(BlueprintCallable, Category = "SC|Camera") void SetCameraMode(ESCCameraMode Mode);
    UFUNCTION(BlueprintPure, Category = "SC|Camera") ESCCameraMode GetCameraMode() const { return Mode; }
    /** Orbit: target the box centre at a distance that fits its sphere in view. Fly: move to the same viewpoint. */
    UFUNCTION(BlueprintCallable, Category = "SC|Camera") void FocusBounds(const FBox& Box);
    /** Place the camera directly (fly mode keeps it; orbit re-targets in front of it). */
    UFUNCTION(BlueprintCallable, Category = "SC|Camera") void SetView(const FVector& Location, const FRotator& Rotation);

    // Input from the viewport input widget.
    void RotateDrag(const FVector2D& Pixels); // orbit around the target, or look around in fly mode
    void PanDrag(const FVector2D& ViewHeightFraction);
    void Wheel(float Notches);               // orbit: zoom; fly: speed
    void SetMoveKey(int32 Axis, float Value); // 0 forward, 1 right, 2 up; -1..1
    void SetSpeedModifier(float Multiplier) { SpeedModifier = Multiplier; }
    float GetFlySpeed() const { return FlySpeed; }

    UPROPERTY(VisibleAnywhere, Category = "SC|Camera") TObjectPtr<UCameraComponent> Camera;

private:
    void ApplyOrbit();
    double ViewAspect() const; // width / height of this player's view rect
    void ViewHalfAngles(double& HalfH, double& HalfV) const; // effective projection half-angles, radians
    ESCCameraMode Mode = ESCCameraMode::Orbit;
    FVector Target = FVector::ZeroVector, TargetGoal = FVector::ZeroVector;
    double Distance = 2000, DistanceGoal = 2000;
    float Yaw = -135.f, Pitch = -20.f; // three-quarter front view of a +X-forward model
    FVector Move = FVector::ZeroVector;
    float FlySpeed = 1500.f, SpeedModifier = 1.f;
};
