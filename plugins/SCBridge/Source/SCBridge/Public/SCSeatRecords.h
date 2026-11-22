// Authored cockpit seat, access and player-fragment records from a scene package.
#pragma once

#include "CoreMinimal.h"

enum class ESCSeatRole : uint8
{
    Unknown,
    Driver,
    Crew,
};

struct FSCSeatAction
{
    FName State, Animation;
    TArray<FName> QueryTags;
    float Speed = 1.f, BlendIn = 0.f, PositionAdjust = 0.f;
    bool bLoop = false;
};

struct FSCSeatDefinition
{
    ESCSeatRole Role = ESCSeatRole::Unknown;
    FName Attachment;
    FString ClassTag, TransitionTemplate, ControlTemplate, DashboardPort;
    TOptional<double> MinYaw, MaxYaw, MinPitch, MaxPitch;
    TOptional<bool> ValidateExit;
    TArray<FSCSeatAction> Actions;
};

struct FSCSeatTransition
{
    FName Interaction, SkipState, Attachment;
    FString FragmentTags;
};

struct FSCSeatGateway
{
    FName Fragment, OpenTag, CloseTag;
    TMap<FName, FName> Conditions;
    float OpenEnter = 0, OpenExit = 0, CloseEnter = 0, CloseExit = 0;
    bool bKeepOpenOnEnter = false, bKeepOpenOnExit = false;
};

/** Port path is relative to the root entity, including nested loadout owners. */
struct FSCSeatEntrance
{
    FString Name;
    TArray<FString> SeatPortPath;
    FString DashboardPort;
    FSCSeatTransition Enter, Exit;
    TMap<FName, FName> Conditions;
    TArray<FSCSeatGateway> Gateways;
    bool bUnsupportedSequence = false;
};
