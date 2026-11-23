// Authored cockpit seat, access and player-fragment records from a scene package.
#pragma once

#include "CoreMinimal.h"
#include "SCSeatRecords.generated.h"

UENUM()
enum class ESCSeatRole : uint8
{
    Unknown,
    Driver,
    Crew,
};

USTRUCT()
struct FSCSeatAction
{
    GENERATED_BODY()
    UPROPERTY() FName State;
    UPROPERTY() FName Animation;
    UPROPERTY() TArray<FName> QueryTags;
    UPROPERTY() float Speed = 1.f;
    UPROPERTY() float BlendIn = 0.f;
    UPROPERTY() float PositionAdjust = 0.f;
    UPROPERTY() bool bLoop = false;
};

/** Saved with a prepared level (the manifest's children keep it: a seat's routes are made from it). */
USTRUCT()
struct FSCSeatDefinition
{
    GENERATED_BODY()
    UPROPERTY() ESCSeatRole Role = ESCSeatRole::Unknown;
    UPROPERTY() FName Attachment;
    UPROPERTY() FString ClassTag;
    UPROPERTY() FString TransitionTemplate;
    UPROPERTY() FString ControlTemplate;
    UPROPERTY() FString DashboardPort;
    UPROPERTY() TOptional<double> MinYaw;
    UPROPERTY() TOptional<double> MaxYaw;
    UPROPERTY() TOptional<double> MinPitch;
    UPROPERTY() TOptional<double> MaxPitch;
    UPROPERTY() TOptional<bool> ValidateExit;
    UPROPERTY() TArray<FSCSeatAction> Actions;
};

USTRUCT()
struct FSCSeatTransition
{
    GENERATED_BODY()
    UPROPERTY() FName Interaction;
    UPROPERTY() FName SkipState;
    UPROPERTY() FName Attachment;
    UPROPERTY() FString FragmentTags;
};

USTRUCT()
struct FSCSeatGateway
{
    GENERATED_BODY()
    UPROPERTY() FName Fragment;
    UPROPERTY() FName OpenTag;
    UPROPERTY() FName CloseTag;
    UPROPERTY() TMap<FName, FName> Conditions;
    UPROPERTY() float OpenEnter = 0;
    UPROPERTY() float OpenExit = 0;
    UPROPERTY() float CloseEnter = 0;
    UPROPERTY() float CloseExit = 0;
    UPROPERTY() bool bKeepOpenOnEnter = false;
    UPROPERTY() bool bKeepOpenOnExit = false;
};

/** Port path is relative to the root entity, including nested loadout owners. */
USTRUCT()
struct FSCSeatEntrance
{
    GENERATED_BODY()
    UPROPERTY() FString Name;
    UPROPERTY() TArray<FString> SeatPortPath;
    UPROPERTY() FString DashboardPort;
    UPROPERTY() FSCSeatTransition Enter;
    UPROPERTY() FSCSeatTransition Exit;
    UPROPERTY() TMap<FName, FName> Conditions;
    UPROPERTY() TArray<FSCSeatGateway> Gateways;
    UPROPERTY() bool bUnsupportedSequence = false;
};
