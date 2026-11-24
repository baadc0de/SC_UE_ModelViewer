// Authored placement, contact settings and player fragments for an interior ladder.
#pragma once

#include "CoreMinimal.h"
#include "SCLadderRecords.generated.h"

class FJsonObject;
class USceneComponent;

USTRUCT()
struct FSCLadderEnd
{
    GENERATED_BODY()
    UPROPERTY() FVector2D WalkRunRadius = FVector2D::ZeroVector;
    UPROPERTY() float RadiusOffset = 0;
    UPROPERTY() float ApproachAngle = 0;
    UPROPERTY() float FacingAngle = 0;
    UPROPERTY() float RemountDelay = 0;
};

USTRUCT()
struct FSCLadderAction
{
    GENERATED_BODY()
    UPROPERTY() FName Clip;
    UPROPERTY() float Speed = 0;
    UPROPERTY() float Blend = 0;
    UPROPERTY() float PositionAdjust = 0;
};

/** Saved with a prepared level (ASCVehicle::LadderAnchors, WP-O1-4b-1b-3). */
USTRUCT()
struct SCBRIDGE_API FSCLadderDefinition
{
    GENERATED_BODY()
    UPROPERTY() float Height = 0;
    UPROPERTY() float HorizontalOffset = 0;
    UPROPERTY() float TravelInRungs = 0;
    UPROPERTY() float MountBottom = 0;
    UPROPERTY() float StopBottom = 0;
    UPROPERTY() FVector StopTop = FVector::ZeroVector; // back, left, right
    UPROPERTY() FVector2D SpeedUpDown = FVector2D::ZeroVector;
    UPROPERTY() float Acceleration = 0;
    UPROPERTY() float InertiaDecay = 0;
    UPROPERTY() float HorizontalView = 0;
    UPROPERTY() float UpView = 0;
    UPROPERTY() FSCLadderEnd Bottom;
    UPROPERTY() FSCLadderEnd Top;
    UPROPERTY() bool bAutoMount = false;
    UPROPERTY() bool bIgnoreStaticCollision = false;
    UPROPERTY() FIntVector TopExits = FIntVector::ZeroValue; // back, left, right
    UPROPERTY() TMap<FName, FSCLadderAction> Actions;

    static TOptional<FSCLadderDefinition> Read(const FJsonObject& Placement);
};

USTRUCT()
struct FSCLadderAnchor
{
    GENERATED_BODY()
    UPROPERTY() TWeakObjectPtr<USceneComponent> Anchor;
    UPROPERTY() FString EntityId;
    UPROPERTY() FSCLadderDefinition Data;
};
