// Command-line and console automation of the viewer subsystem (verification runs, screenshots, manual testing).
// -SCLoad=A,B,...  load entities in order once the archive is open; -SCHold=<s> waits between loads;
// -SCCamera=orbit|fly|first applies after the first load; -SCFlyView=dx,dy,dz,pitch,yaw then places the camera
// (cm from the loaded bounds centre, degrees); -SCDefaults resets options to the core defaults first;
// -SCLod=<n> / -SCMip=<n> override them; -SCQuitAfterLoad exits (status 0 only if every load succeeded).
// -SCScript=<file> runs "<delay seconds> <console command>" lines once a vehicle is loaded (see ue/README.md).
// Console: sc.Load <entity>, sc.Unload, sc.Cancel, sc.Camera <orbit|fly|first>, sc.FP.* (first person).
#pragma once
#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

class USCViewerSubsystem;

class FSCViewerAutomation
{
public:
    explicit FSCViewerAutomation(USCViewerSubsystem& InOwner);
    void Tick();

private:
    struct FStep
    {
        double Delay = 0;
        FString Command;
    };
    void RegisterFirstPersonCommands();
    void TickScript(USCViewerSubsystem& S);
    void RunStep(USCViewerSubsystem& S, const FStep& Step);
    void LogFrameSamples();
    TWeakObjectPtr<USCViewerSubsystem> Owner;
    TArray<FString> Queue;
    FString Camera, FlyView;
    bool bQuit = false, bQueued = false, bAllSucceeded = true, bCameraApplied = false, bWaiting = false;
    double Hold = 0, NextAt = 0;
    TArray<TUniquePtr<FAutoConsoleCommand>> Commands;
    // -SCScript runner
    TArray<FStep> Script;
    int32 StepIndex = INDEX_NONE; // INDEX_NONE: not started; Script.Num(): done
    double StepAt = 0, SampleUntil = 0;
    FString SampleTag;
    TArray<float> FrameMs, GameMs, RenderMs, GpuMs;
};
