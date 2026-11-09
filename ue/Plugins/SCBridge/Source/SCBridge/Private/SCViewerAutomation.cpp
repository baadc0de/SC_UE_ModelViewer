// FSCViewerAutomation: drives LoadEntity from the command line / console; never names an entity itself.
#include "SCViewerAutomation.h"
#include "SCViewerSubsystem.h"
#include "SCViewOptions.h"
#include "SCViewerPawn.h"
#include "SCVehicle.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCViewerAutomation, Log, All);

FSCViewerAutomation::FSCViewerAutomation(USCViewerSubsystem& InOwner) : Owner(&InOwner)
{
    const TCHAR* Cmd = FCommandLine::Get();
    FString List;
    if (FParse::Value(Cmd, TEXT("SCLoad="), List, false)) List.ParseIntoArray(Queue, TEXT(","), true);
    bQueued = !Queue.IsEmpty();
    FParse::Value(Cmd, TEXT("SCCamera="), Camera);
    FParse::Value(Cmd, TEXT("SCFlyView="), FlyView, false);
    FParse::Value(Cmd, TEXT("SCHold="), Hold);
    bQuit = FParse::Param(Cmd, TEXT("SCQuitAfterLoad"));
    USCViewOptions* Options = InOwner.GetOptions();
    if (FParse::Param(Cmd, TEXT("SCDefaults"))) Options->ResetToDefaults();
    FParse::Value(Cmd, TEXT("SCLod="), Options->Lod);
    FParse::Value(Cmd, TEXT("SCMip="), Options->Mip);
    if (IsRunningCommandlet()) return; // headless hosts call the subsystem directly; one console registration
    TWeakObjectPtr<USCViewerSubsystem> Weak = &InOwner;
    Commands.Add(MakeUnique<FAutoConsoleCommand>(TEXT("sc.Load"), TEXT("sc.Load <entity>: load with the current view options"),
        FConsoleCommandWithArgsDelegate::CreateLambda([Weak](const TArray<FString>& Args) {
            if (Weak.IsValid() && !Args.IsEmpty()) Weak->LoadEntity(Args[0], nullptr);
        })));
    Commands.Add(MakeUnique<FAutoConsoleCommand>(TEXT("sc.Unload"), TEXT("sc.Unload: destroy the loaded entity and collect garbage"),
        FConsoleCommandDelegate::CreateLambda([Weak] { if (Weak.IsValid()) Weak->Unload(); })));
    Commands.Add(MakeUnique<FAutoConsoleCommand>(TEXT("sc.Cancel"), TEXT("sc.Cancel: cancel the running load"),
        FConsoleCommandDelegate::CreateLambda([Weak] { if (Weak.IsValid()) Weak->CancelLoad(); })));
    Commands.Add(MakeUnique<FAutoConsoleCommand>(TEXT("sc.Camera"), TEXT("sc.Camera orbit|fly|first"),
        FConsoleCommandWithArgsDelegate::CreateLambda([Weak](const TArray<FString>& Args) {
            if (!Weak.IsValid() || Args.IsEmpty()) return;
            const FString& M = Args[0];
            Weak->SetCameraMode(M.Equals(TEXT("fly"), ESearchCase::IgnoreCase)     ? ESCCameraMode::Fly
                                : M.Equals(TEXT("first"), ESearchCase::IgnoreCase) ? ESCCameraMode::FirstPerson
                                                                                  : ESCCameraMode::Orbit);
        })));
}

void FSCViewerAutomation::Tick()
{
    USCViewerSubsystem* S = Owner.Get();
    if (!S || S->IsBusy()) return;
    if (bWaiting)
    {
        bWaiting = false;
        bAllSucceeded &= S->GetLoadPhase() == ESCLoadPhase::Succeeded;
        NextAt = FPlatformTime::Seconds() + Hold;
        if (!bCameraApplied && !Camera.IsEmpty())
        {
            bCameraApplied = true;
            S->SetCameraMode(Camera.Equals(TEXT("fly"), ESearchCase::IgnoreCase) ? ESCCameraMode::Fly : Camera.Equals(TEXT("first"), ESearchCase::IgnoreCase) ? ESCCameraMode::FirstPerson : ESCCameraMode::Orbit);
            TArray<FString> V;
            ASCViewerPawn* Pawn = S->GetViewerPawn();
            if (Pawn && S->GetVehicle() && FlyView.ParseIntoArray(V, TEXT(",")) == 5)
            {
                const FVector Centre = S->GetVehicle()->GetComponentsBoundingBox(true).GetCenter();
                Pawn->SetView(Centre + FVector(FCString::Atod(*V[0]), FCString::Atod(*V[1]), FCString::Atod(*V[2])),
                              FRotator(FCString::Atof(*V[3]), FCString::Atof(*V[4]), 0));
            }
        }
    }
    if (S->GetArchiveState() == ESCArchiveState::Failed && !Queue.IsEmpty())
    {
        UE_LOG(LogSCViewerAutomation, Error, TEXT("AUTOMATION archive failed: %s"), *S->GetArchiveError());
        Queue.Empty();
        bAllSucceeded = false;
        if (bQuit) FPlatformMisc::RequestExitWithStatus(false, 1);
        bQuit = bQueued = false;
        return;
    }
    if (S->GetArchiveState() != ESCArchiveState::Ready || FPlatformTime::Seconds() < NextAt) return;
    if (Queue.IsEmpty())
    {
        if (bQueued)
        {
            bQueued = false;
            UE_LOG(LogSCViewerAutomation, Display, TEXT("AUTOMATION_DONE all_succeeded=%d"), bAllSucceeded);
            if (bQuit) FPlatformMisc::RequestExitWithStatus(false, bAllSucceeded ? 0 : 1);
        }
        return;
    }
    const FString Entity = Queue[0].TrimStartAndEnd();
    Queue.RemoveAt(0);
    bWaiting = S->LoadEntity(Entity, nullptr);
    bAllSucceeded &= bWaiting;
}
