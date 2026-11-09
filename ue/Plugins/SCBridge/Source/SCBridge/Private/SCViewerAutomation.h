// Command-line and console automation of the viewer subsystem (verification runs, screenshots, manual testing).
// -SCLoad=A,B,...  load entities in order once the archive is open; -SCHold=<s> waits between loads;
// -SCCamera=orbit|fly applies after the first load; -SCFlyView=dx,dy,dz,pitch,yaw then places the camera
// (cm from the loaded bounds centre, degrees); -SCDefaults resets options to the core defaults first;
// -SCLod=<n> / -SCMip=<n> override them; -SCQuitAfterLoad exits (status 0 only if every load succeeded).
// Console: sc.Load <entity>, sc.Unload, sc.Cancel, sc.Camera <orbit|fly|first>.
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
    TWeakObjectPtr<USCViewerSubsystem> Owner;
    TArray<FString> Queue;
    FString Camera, FlyView;
    bool bQuit = false, bQueued = false, bAllSucceeded = true, bCameraApplied = false, bWaiting = false;
    double Hold = 0, NextAt = 0;
    TArray<TUniquePtr<FAutoConsoleCommand>> Commands;
};
