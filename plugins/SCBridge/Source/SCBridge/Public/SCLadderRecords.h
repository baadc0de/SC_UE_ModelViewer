// Authored placement, contact settings and player fragments for an interior ladder.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class USceneComponent;

struct FSCLadderEnd
{
    FVector2D WalkRunRadius = FVector2D::ZeroVector;
    float RadiusOffset = 0, ApproachAngle = 0, FacingAngle = 0, RemountDelay = 0;
};

struct FSCLadderAction
{
    FName Clip;
    float Speed = 0, Blend = 0, PositionAdjust = 0;
};

struct FSCLadderDefinition
{
    float Height = 0, HorizontalOffset = 0, TravelInRungs = 0;
    float MountBottom = 0, StopBottom = 0;
    FVector StopTop = FVector::ZeroVector; // back, left, right
    FVector2D SpeedUpDown = FVector2D::ZeroVector;
    float Acceleration = 0, InertiaDecay = 0, HorizontalView = 0, UpView = 0;
    FSCLadderEnd Bottom, Top;
    bool bAutoMount = false, bIgnoreStaticCollision = false;
    FIntVector TopExits = FIntVector::ZeroValue; // back, left, right
    TMap<FName, FSCLadderAction> Actions;

    static TOptional<FSCLadderDefinition> Read(const FJsonObject& Placement);
};

struct FSCLadderAnchor
{
    TWeakObjectPtr<USceneComponent> Anchor;
    FString EntityId;
    FSCLadderDefinition Data;
};
