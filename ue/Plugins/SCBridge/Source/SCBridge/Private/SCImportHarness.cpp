#include "SCImportHarness.h"
#include "SCImporter.h"
#include "SCVehicle.h"
#include "SCViewerSubsystem.h"
#include "SCFirstPersonCharacter.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Camera/CameraActor.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"

void FSCImportHarness::Start()
{
    FString Folder;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SCPackage="), Folder)) return;
    Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Folder](float)
    {
        UWorld* World = nullptr;
        for (const auto& Context : GEngine->GetWorldContexts()) if (Context.World() && Context.World()->IsGameWorld() && Context.World()->HasBegunPlay()) World = Context.World();
        if (!World) return true;
        FString Tar;
        if (FParse::Value(FCommandLine::Get(), TEXT("SCVerifyTar="), Tar))
        {
            const bool bOK = FSCImporter::Verify(Folder, Tar, World);
            FPlatformMisc::RequestExitWithStatus(false, bOK ? 0 : 1);
        }
        else
        {
            ASCVehicle* Vehicle = USCImportLibrary::ImportFolder(World, Folder);
            if (Vehicle)
            {
                FString Clip, Fragment, Tag, Reference, PoseClip;
                bool RigOK = true;
                if (FParse::Param(FCommandLine::Get(), TEXT("SCVerifyRig")))
                {
                    FParse::Value(FCommandLine::Get(), TEXT("SCPoseReference="), Reference);
                    FParse::Value(FCommandLine::Get(), TEXT("SCPoseClip="), PoseClip);
                    RigOK = USCImportLibrary::VerifyRig(Vehicle, Reference, FName(*PoseClip));
                    if (!RigOK) { FPlatformMisc::RequestExitWithStatus(false, 1); return false; }
                }
                if (FParse::Value(FCommandLine::Get(), TEXT("SCSnapClip="), Clip)) Vehicle->SnapClip(FName(*Clip), true);
                if (FParse::Value(FCommandLine::Get(), TEXT("SCPlayClip="), Clip)) Vehicle->PlayClip(FName(*Clip));
                if (FParse::Value(FCommandLine::Get(), TEXT("SCFragment="), Fragment))
                { FParse::Value(FCommandLine::Get(), TEXT("SCTag="), Tag); Vehicle->PlayFragment(FName(*Fragment), FName(*Tag)); }
                const FBox Bounds = Vehicle->GetComponentsBoundingBox(true);
                // Review camera: -SCView=yaw,pitch,distance (degrees, x bounds radius) orbits the bounds
                // centre, -SCTarget=x,y,z offsets the look-at point in cm. Defaults frame the whole model.
                FVector Center = Bounds.GetCenter(); const double Radius = Bounds.GetExtent().Size();
                FString View, Target; TArray<FString> V, T;
                double Yaw = 45, Pitch = 23, Distance = 1.536;
                if (FParse::Value(FCommandLine::Get(), TEXT("SCView="), View, false) && View.ParseIntoArray(V, TEXT(",")) == 3)
                { Yaw = FCString::Atod(*V[0]); Pitch = FCString::Atod(*V[1]); Distance = FCString::Atod(*V[2]); }
                if (FParse::Value(FCommandLine::Get(), TEXT("SCTarget="), Target, false) && Target.ParseIntoArray(T, TEXT(",")) == 3)
                    Center += FVector(FCString::Atod(*T[0]), FCString::Atod(*T[1]), FCString::Atod(*T[2]));
                const FVector Offset = FRotator(Pitch, Yaw, 0).Vector() * Radius * Distance;
                auto* Camera = World->SpawnActor<ACameraActor>(Center + Offset, FRotator::ZeroRotator);
                Camera->SetActorRotation((Center - Camera->GetActorLocation()).Rotation());
                if (auto* PC = World->GetFirstPlayerController()) PC->SetViewTarget(Camera);
                if (FParse::Param(FCommandLine::Get(), TEXT("SCExplore")))
                {
                    auto* Viewer = USCViewerSubsystem::Get(World);
                    if (!Viewer) { FPlatformMisc::RequestExitWithStatus(false,1); return false; }
                    Viewer->AdoptPackageVehicle(Vehicle);
                    float WalkSeconds = 0;
                    FParse::Value(FCommandLine::Get(),TEXT("SCWalkSeconds="),WalkSeconds);
                    if (WalkSeconds>0)
                        for (int32 Door=0; Door<Vehicle->Doors.Num(); ++Door) { Vehicle->ToggleDoor(Door); Vehicle->Tick(60); }
                    if (!Viewer->SetCameraMode(ESCCameraMode::FirstPerson)) { FPlatformMisc::RequestExitWithStatus(false,1); return false; }
                    // Possession resets the old review camera view target.
                    if (auto* PC = World->GetFirstPlayerController()) PC->SetViewTarget(Viewer->GetFirstPerson());
                    if (WalkSeconds>0)
                    {
                        TWeakObjectPtr<ASCFirstPersonCharacter> Character = Viewer->GetFirstPerson();
                        const FVector Start = Character->GetActorLocation();
                        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
                            [Character,Start,Elapsed=0.f,WalkSeconds,FloorFrames=0,Started=false](float Dt) mutable {
                                if (!Character.IsValid()) return false;
                                // The registration frame includes the synchronous import stall.
                                if (!Started) { Started=true; return true; }
                                // Do not count import/initial physics stalls as simulated walking time.
                                Elapsed += FMath::Min(Dt,0.1f);
                                Character->SetMoveInput(Elapsed<1 ? FVector::ZeroVector : FVector(1,0,0));
                                FloorFrames += Character->GetCharacterMovement()->IsMovingOnGround();
                                if (Elapsed < WalkSeconds+1) return true;
                                Character->ClearInput();
                                bool Passed = Character->RecoveryCount==0 && FloorFrames>20 &&
                                    FVector::Dist2D(Start,Character->GetActorLocation())>20;
                                UE_LOG(LogTemp,Display,TEXT("P1_GAME_WALK passed=%d position=%s distance_cm=%.2f floor_frames=%d capsule_hits=%d recovery=%d"),
                                    Passed,*Character->GetActorLocation().ToString(),FVector::Dist(Start,Character->GetActorLocation()),FloorFrames,Character->CapsuleHits,Character->RecoveryCount);
                                auto* S = USCViewerSubsystem::Get(Character->GetWorld());
                                const bool Switched = S && S->SetCameraMode(ESCCameraMode::Orbit) &&
                                    S->SetCameraMode(ESCCameraMode::Fly) && S->SetCameraMode(ESCCameraMode::FirstPerson);
                                if (S) S->Unload();
                                const bool Unloaded = S && !S->GetFirstPerson() && !S->GetVehicle();
                                Passed &= Switched && Unloaded;
                                UE_LOG(LogTemp,Display,TEXT("P1_GAME_LIFECYCLE passed=%d switched=%d unloaded=%d"),Passed,Switched,Unloaded);
                                FPlatformMisc::RequestExitWithStatus(false,Passed?0:1);
                                return false;
                            }));
                    }
                }
                float Glow;
                if (FParse::Value(FCommandLine::Get(), TEXT("SCEngineGlow="), Glow)) Vehicle->SetEngineGlow(Glow); // review aid
            }
            if (!FParse::Param(FCommandLine::Get(), TEXT("SCStay")) && !FParse::Param(FCommandLine::Get(), TEXT("SCExplore"))) FPlatformMisc::RequestExitWithStatus(false, Vehicle ? 0 : 1);
        }
        return false;
    }), 0.5f);
}
void FSCImportHarness::Stop()
{
    FTSTicker::GetCoreTicker().RemoveTicker(Tick);
}
