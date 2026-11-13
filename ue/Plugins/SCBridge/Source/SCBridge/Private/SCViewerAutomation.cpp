// FSCViewerAutomation: drives LoadEntity from the command line / console; never names an entity itself.
#include "SCViewerAutomation.h"
#include "SCViewerSubsystem.h"
#include "SCViewOptions.h"
#include "SCViewerPawn.h"
#include "SCVehicle.h"
#include "SCFirstPersonCharacter.h"
#include "SCPlayerBody.h"
#include "SCRigComponent.h"
#include "SCLightImporter.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/UObjectIterator.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "InputCoreTypes.h"
#include "RenderTimer.h"
#include "DynamicRHI.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include <atomic>

// Reads the path tracer's accumulated sample count from the view state of path-traced views (game thread, per frame).
class FSCPathTracerProbe final : public FSceneViewExtensionBase
{
public:
    explicit FSCPathTracerProbe(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}
    virtual void SetupViewFamily(FSceneViewFamily&) override {}
    virtual void SetupView(FSceneViewFamily& Family, FSceneView& View) override
    {
        if (Family.EngineShowFlags.PathTracing && View.State) Samples = View.State->GetPathTracingSampleIndex();
    }
    virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}
    std::atomic<int64> Samples{-1};
};

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
    FParse::Value(Cmd, TEXT("SCPlayerWeapon="), PlayerWeapon);
    FParse::Value(Cmd, TEXT("SCPlayerView="), PlayerView);
    FString ScriptFile, DemoEntity;
    if (FParse::Param(Cmd, TEXT("SCPlayerDemo")) || FParse::Value(Cmd, TEXT("SCPlayerDemo="), DemoEntity))
    {
        // Built-in demo route; the route itself names no ship (spawn kinds and relative moves only).
        if (!FParse::Value(Cmd, TEXT("SCScript="), ScriptFile))
            ScriptFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../scripts/routes/player-demo.txt"));
        if (!DemoEntity.IsEmpty() && Queue.IsEmpty())
        {
            Queue.Add(DemoEntity);
            bQueued = true;
        }
        if (Camera.IsEmpty()) Camera = TEXT("first");
    }
    if (!ScriptFile.IsEmpty() || FParse::Value(Cmd, TEXT("SCScript="), ScriptFile))
    {
        TArray<FString> Lines;
        if (!FFileHelper::LoadFileToStringArray(Lines, *ScriptFile))
            UE_LOG(LogSCViewerAutomation, Error, TEXT("SCRIPT cannot read %s"), *ScriptFile);
        for (FString Line : Lines)
        {
            Line.TrimStartAndEndInline();
            if (Line.IsEmpty() || Line.StartsWith(TEXT("#"))) continue;
            FString Delay, Command;
            if (!Line.Split(TEXT(" "), &Delay, &Command) || !Delay.IsNumeric())
            {
                UE_LOG(LogSCViewerAutomation, Error, TEXT("SCRIPT bad line \"%s\": expected <delay seconds> <command>"), *Line);
                continue;
            }
            Script.Add({FCString::Atod(*Delay), Command.TrimStartAndEnd()});
        }
        UE_LOG(LogSCViewerAutomation, Display, TEXT("SCRIPT loaded file=%s steps=%d"), *ScriptFile, Script.Num());
        InOwner.SetPanelsVisible(false); // scripted runs (captures) show the full-width scene; sc.UI.Panels 1 restores
    }
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
    Commands.Add(MakeUnique<FAutoConsoleCommand>(TEXT("sc.Fragment"), TEXT("sc.Fragment <fragment> [tag]: play a Mannequin fragment of the loaded vehicle"),
        FConsoleCommandWithArgsDelegate::CreateLambda([Weak](const TArray<FString>& Args) {
            ASCVehicle* V = Weak.IsValid() ? Weak->GetVehicle() : nullptr;
            if (!V || Args.IsEmpty()) return;
            const FName Tag = Args.IsValidIndex(1) ? FName(*Args[1]) : NAME_None;
            UE_LOG(LogSCViewerAutomation, Display, TEXT("FRAGMENT_CMD fragment=%s tag=%s ok=%d"), *Args[0], *Tag.ToString(), V->PlayFragment(FName(*Args[0]), Tag));
        })));
    RegisterFirstPersonCommands();
    RegisterPlayerCommands();
}

namespace
{
ASCFirstPersonCharacter* FirstPerson(const TWeakObjectPtr<USCViewerSubsystem>& Weak)
{
    ASCFirstPersonCharacter* C = Weak.IsValid() ? Weak->GetFirstPerson() : nullptr;
    if (!C) UE_LOG(LogSCViewerAutomation, Warning, TEXT("FP_CMD ignored: not in first person (sc.FP.Spawn enters it)"));
    return C;
}
bool Flag(const TArray<FString>& Args, int32 Index, bool bDefault)
{
    return Args.IsValidIndex(Index) ? FCString::Atoi(*Args[Index]) != 0 : bDefault;
}
FString Where(USCViewerSubsystem& S)
{
    ASCVehicle* V = S.GetVehicle();
    if (ASCFirstPersonCharacter* C = S.GetFirstPerson())
    {
        const FRotator View = C->GetVehicleRelativeView();
        return FString::Printf(TEXT("mode=first pos=%s yaw=%.1f pitch=%.1f speed=%.0f floor=%d crouched=%d noclip=%d stuck=%d recover=%d"),
            *C->GetVehicleRelativeLocation().ToCompactString(), View.Yaw, View.Pitch, C->GetVelocity().Size(),
            C->GetCharacterMovement()->IsMovingOnGround(), C->bIsCrouched, C->bNoclip, C->bStuck, C->RecoveryCount);
    }
    ASCViewerPawn* Pawn = S.GetViewerPawn();
    const FVector P = Pawn ? Pawn->Camera->GetComponentLocation() : FVector::ZeroVector;
    return FString::Printf(TEXT("mode=%d pos=%s"), int32(S.GetCameraMode()),
        *(V ? V->GetActorTransform().InverseTransformPosition(P) : P).ToCompactString());
}
} // namespace

void FSCViewerAutomation::RegisterFirstPersonCommands()
{
    TWeakObjectPtr<USCViewerSubsystem> Weak = Owner;
    auto Add = [&](const TCHAR* Name, const TCHAR* Help, TFunction<void(const TArray<FString>&)> Body) {
        Commands.Add(MakeUnique<FAutoConsoleCommand>(Name, Help,
            FConsoleCommandWithArgsDelegate::CreateLambda([Body](const TArray<FString>& Args) { Body(Args); })));
    };
    Add(TEXT("sc.FP.Spawns"), TEXT("sc.FP.Spawns: log the spawn list (index, kind, label, vehicle-relative location)"), [Weak](const TArray<FString>&) {
        ASCVehicle* V = Weak.IsValid() ? Weak->GetVehicle() : nullptr;
        for (int32 I = 0; V && I < V->SpawnPoints.Num(); ++I)
            UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_SPAWN_LIST index=%d kind=%s pos=%s label=\"%s\""), I,
                   *V->SpawnPoints[I].Kind.ToString(), *V->SpawnPoints[I].Transform.GetLocation().ToCompactString(), *V->SpawnPoints[I].DisplayName);
        for (int32 I = 0; V && I < V->Doors.Num(); ++I)
            UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_DOOR_LIST index=%d fragment=%s open=%d label=\"%s\""), I,
                   *V->Doors[I].Fragment.ToString(), V->IsDoorOpen(I), *V->Doors[I].DisplayName);
    });
    Add(TEXT("sc.FP.Spawn"), TEXT("sc.FP.Spawn <index|kind[:n]>: enter first person if needed and teleport to a spawn"), [Weak](const TArray<FString>& Args) {
        USCViewerSubsystem* S = Weak.Get();
        ASCVehicle* V = S ? S->GetVehicle() : nullptr;
        if (!V || Args.IsEmpty()) return;
        if (!S->GetFirstPerson() && !S->SetCameraMode(ESCCameraMode::FirstPerson))
        {
            UE_LOG(LogSCViewerAutomation, Warning, TEXT("FP_CMD spawn ok=0 reason=first_person_refused"));
            return;
        }
        int32 Index = INDEX_NONE;
        if (Args[0].IsNumeric())
            Index = FCString::Atoi(*Args[0]);
        else
        {
            FString Kind = Args[0], Nth = TEXT("0");
            Args[0].Split(TEXT(":"), &Kind, &Nth);
            TArray<int32> Matches;
            for (int32 I = 0; I < V->SpawnPoints.Num(); ++I)
                if (V->SpawnPoints[I].Kind.ToString().Equals(Kind, ESearchCase::IgnoreCase)) Matches.Add(I);
            if (Matches.IsValidIndex(FCString::Atoi(*Nth))) Index = Matches[FCString::Atoi(*Nth)];
        }
        const bool OK = S->GetFirstPerson()->TeleportToSpawn(Index);
        UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_CMD spawn arg=%s index=%d ok=%d"), *Args[0], Index, OK);
    });
    Add(TEXT("sc.FP.Look"), TEXT("sc.FP.Look <yaw> <pitch>: absolute view relative to the vehicle (degrees)"), [Weak](const TArray<FString>& Args) {
        if (auto* C = FirstPerson(Weak); C && Args.Num() >= 2) C->SetVehicleRelativeView(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]));
    });
    Add(TEXT("sc.FP.Move"), TEXT("sc.FP.Move <forward> <right> <seconds> [run 0|1] [up]: hold movement input (-1..1) for a duration"),
        [Weak](const TArray<FString>& Args) {
            if (auto* C = FirstPerson(Weak); C && Args.Num() >= 3)
                C->HoldMoveInput(FVector(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), Args.IsValidIndex(4) ? FCString::Atof(*Args[4]) : 0.f),
                                 FCString::Atof(*Args[2]), Flag(Args, 3, false));
        });
    Add(TEXT("sc.FP.Interact"), TEXT("sc.FP.Interact: E on the nearest interaction"), [Weak](const TArray<FString>&) {
        if (auto* C = FirstPerson(Weak))
        {
            const int32 Door = C->FocusedInteraction();
            const bool OK = C->Interact();
            UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_CMD interact door=%d ok=%d open=%d label=\"%s\""), Door, OK,
                   C->Vehicle && C->Vehicle->IsDoorOpen(Door), Door == INDEX_NONE ? TEXT("") : *C->Vehicle->Doors[Door].DisplayName);
        }
    });
    Add(TEXT("sc.FP.Door"), TEXT("sc.FP.Door <index>: toggle an interaction by index (sc.FP.Spawns lists them)"), [Weak](const TArray<FString>& Args) {
        ASCVehicle* V = Weak.IsValid() ? Weak->GetVehicle() : nullptr;
        if (V && !Args.IsEmpty()) V->ToggleDoor(FCString::Atoi(*Args[0]));
    });
    Add(TEXT("sc.FP.Key"), TEXT("sc.FP.Key <key> 0|1: press/release a key of the first-person key table (e.g. W, LeftShift, D)"),
        [Weak](const TArray<FString>& Args) {
            if (auto* C = FirstPerson(Weak); C && !Args.IsEmpty())
                UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_CMD key=%s down=%d handled=%d"), *Args[0], Flag(Args, 1, true),
                       C->HandleKey(FKey(FName(*Args[0])), Flag(Args, 1, true)));
        });
    Add(TEXT("sc.FP.Crouch"), TEXT("sc.FP.Crouch 0|1"), [Weak](const TArray<FString>& Args) {
        if (auto* C = FirstPerson(Weak)) C->SetCrouching(Flag(Args, 0, true));
    });
    Add(TEXT("sc.FP.Jump"), TEXT("sc.FP.Jump: one jump"), [Weak](const TArray<FString>&) {
        if (auto* C = FirstPerson(Weak)) C->Jump();
    });
    Add(TEXT("sc.FP.Torch"), TEXT("sc.FP.Torch 0|1"), [Weak](const TArray<FString>& Args) {
        if (auto* C = FirstPerson(Weak)) C->SetTorch(Flag(Args, 0, true));
    });
    Add(TEXT("sc.FP.Noclip"), TEXT("sc.FP.Noclip 0|1 (an exit inside geometry is refused)"), [Weak](const TArray<FString>& Args) {
        if (auto* C = FirstPerson(Weak))
        {
            const bool OK = C->SetNoclip(Flag(Args, 0, true));
            UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_CMD noclip want=%d ok=%d"), Flag(Args, 0, true), OK);
        }
    });
    Add(TEXT("sc.FP.Menu"), TEXT("sc.FP.Menu 0|1: show the teleport menu"), [Weak](const TArray<FString>& Args) {
        if (auto* C = FirstPerson(Weak)) C->bTeleportMenuOpen = Flag(Args, 0, true);
    });
    Add(TEXT("sc.FP.Teleport"), TEXT("sc.FP.Teleport <x> <y> <z>: vehicle-relative floor hint (cm)"), [Weak](const TArray<FString>& Args) {
        if (auto* C = FirstPerson(Weak); C && C->Vehicle && Args.Num() >= 3)
        {
            const FVector Local(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]), FCString::Atod(*Args[2]));
            const bool OK = C->TeleportToFloor(C->Vehicle->GetActorTransform().TransformPosition(Local));
            UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_CMD teleport ok=%d"), OK);
        }
    });
    Add(TEXT("sc.FP.Place"), TEXT("sc.FP.Place <x> <y> <z>: put the capsule centre at a vehicle-relative point without checks (noclip tests)"),
        [Weak](const TArray<FString>& Args) {
            if (auto* C = FirstPerson(Weak); C && C->Vehicle && Args.Num() >= 3)
                C->SetActorLocation(C->Vehicle->GetActorTransform().TransformPosition(
                    FVector(FCString::Atod(*Args[0]), FCString::Atod(*Args[1]), FCString::Atod(*Args[2]))), false, nullptr, ETeleportType::TeleportPhysics);
        });
    Add(TEXT("sc.FP.Embed"), TEXT("sc.FP.Embed [depth cm]: noclip, then put the capsule centre this far beyond the surface under the crosshair (noclip-exit test)"),
        [Weak](const TArray<FString>& Args) {
            auto* C = FirstPerson(Weak);
            if (!C) return;
            const FVector Eye = C->Camera->GetComponentLocation(), Dir = C->GetViewRotation().Vector();
            FHitResult Hit;
            FCollisionQueryParams Q(SCENE_QUERY_STAT(SCEmbed), true, C);
            if (!C->GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Dir * 1000, ECC_Pawn, Q))
            {
                UE_LOG(LogSCViewerAutomation, Warning, TEXT("FP_CMD embed ok=0 reason=no_surface_within_10m"));
                return;
            }
            C->SetNoclip(true);
            const double Depth = Args.IsEmpty() ? C->GetProfile().CapsuleRadius : FCString::Atod(*Args[0]);
            C->SetActorLocation(Hit.ImpactPoint + Dir * Depth, false, nullptr, ETeleportType::TeleportPhysics);
            UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_CMD embed ok=1 pos=%s"), *C->GetVehicleRelativeLocation().ToCompactString());
        });
    Add(TEXT("sc.FP.ShowCollision"), TEXT("sc.FP.ShowCollision 0|1: draw the game's own colliders the pawn walks on (WP-P3)"),
        [Weak](const TArray<FString>& Args) {
            auto* C = FirstPerson(Weak);
            if (!C) return;
            const bool bShow = Flag(Args, 0, true);
            int32 Count = 0;
            for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
                if (It->GetWorld() == C->GetWorld() && It->ComponentHasTag(TEXT("SCGameCollision")))
                {
                    It->SetVisibility(bShow);
                    It->SetHiddenInGame(!bShow);
                    ++Count;
                }
            UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_CMD show_collision=%d components=%d"), bShow, Count);
        });
    Add(TEXT("sc.FP.Blocker"), TEXT("sc.FP.Blocker [cm]: sweep the pawn capsule ahead along the view yaw and log what blocks it"),
        [Weak](const TArray<FString>& Args) {
            auto* C = FirstPerson(Weak);
            if (!C) return;
            const double Reach = Args.IsEmpty() ? 100 : FCString::Atod(*Args[0]);
            const FVector From = C->GetActorLocation(), Ahead = FRotator(0, C->GetViewRotation().Yaw, 0).Vector();
            FHitResult Hit;
            FCollisionQueryParams Q(SCENE_QUERY_STAT(SCBlocker), true, C);
            Q.AddIgnoredComponents(C->GetCapsuleComponent()->GetMoveIgnoreComponents()); // as movement sees it (WP-P3 grid)
            const bool bHit = C->GetWorld()->SweepSingleByChannel(Hit, From, From + Ahead * Reach, FQuat::Identity, ECC_Pawn,
                C->GetCapsuleComponent()->GetCollisionShape(), Q);
            const UPrimitiveComponent* P = Hit.GetComponent();
            FString Tags;
            if (P) for (const FName& T : P->ComponentTags) Tags += T.ToString() + TEXT(" ");
            UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_BLOCKER hit=%d dist=%.1f component=%s parent=%s socket=%s tags=%s point=%s normal=%s"), bHit,
                bHit ? Hit.Distance : 0.0, P ? *P->GetName() : TEXT("-"), P && P->GetAttachParent() ? *P->GetAttachParent()->GetName() : TEXT("-"),
                P ? *P->GetAttachSocketName().ToString() : TEXT("-"), *Tags, *Hit.ImpactPoint.ToCompactString(), *Hit.ImpactNormal.ToCompactString());
        });
    Add(TEXT("sc.FP.Where"), TEXT("sc.FP.Where: log mode, vehicle-relative position and movement state"), [Weak](const TArray<FString>&) {
        if (Weak.IsValid()) UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_WHERE %s"), *Where(*Weak.Get()));
    });
    Add(TEXT("sc.FP.Pads"), TEXT("sc.FP.Pads: aim at every door panel from 80 cm in front (four sides) and log whether E resolves to its door"),
        [Weak](const TArray<FString>&) {
            ASCVehicle* V = Weak.IsValid() ? Weak->GetVehicle() : nullptr;
            if (!V) return;
            const AActor* Ignore = Weak->GetFirstPerson();
            int32 Total = 0, Resolved = 0, NoDoor = 0;
            for (const auto& [Panel, Rig] : V->DoorControls)
            {
                const USceneComponent* Anchor = Panel.Get();
                const int32 Expected = V->DoorForRig(Rig.Get());
                if (!Anchor) continue;
                ++Total;
                if (Expected == INDEX_NONE) { ++NoDoor; continue; }
                FBox Box(ForceInit);
                TArray<USceneComponent*> Parts;
                Anchor->GetChildrenComponents(true, Parts);
                for (const USceneComponent* Part : Parts)
                    if (const auto* P = Cast<UPrimitiveComponent>(Part); P && P->IsVisible()) Box += P->Bounds.GetBox();
                if (!Box.IsValid) continue;
                bool bOK = false;
                for (const FVector& D : {FVector::ForwardVector, FVector::BackwardVector, FVector::RightVector, FVector::LeftVector})
                    bOK |= V->FindInteraction(Box.GetCenter() + D * (Box.GetExtent().GetMax() + 80), -D, 300, Ignore) == Expected;
                Resolved += bOK;
                if (!bOK) UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_PAD_UNRESOLVED panel=%s door=\"%s\" at=%s"), *Anchor->GetName(),
                                 *V->Doors[Expected].DisplayName, *Box.GetCenter().ToCompactString());
            }
            UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_PADS total=%d resolved=%d no_door=%d"), Total, Resolved, NoDoor);
        });
    Add(TEXT("sc.FP.Probe"), TEXT("sc.FP.Probe [reach cm]: log what the view ray hits (component chain, rig, socket) and the focused interaction"),
        [Weak](const TArray<FString>& Args) {
            auto* C = FirstPerson(Weak);
            if (!C) return;
            const double Reach = Args.IsEmpty() ? 1000 : FCString::Atod(*Args[0]);
            const FVector Eye = C->Camera->GetComponentLocation();
            FHitResult Hit;
            FCollisionQueryParams Q(SCENE_QUERY_STAT(SCProbe), true, C);
            const bool bHit = C->GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + C->GetViewRotation().Vector() * Reach, ECC_Visibility, Q);
            FString Chain;
            for (const USceneComponent* P = bHit ? Hit.GetComponent() : nullptr; P; P = P->GetAttachParent())
            {
                const auto* Rig = Cast<USCRigComponent>(P);
                Chain += FString::Printf(TEXT(" > %s%s%s"), *P->GetName(), Rig ? *(TEXT("[rig ") + Rig->OwnerEntity + TEXT("]")) : TEXT(""),
                                         P->GetAttachSocketName().IsNone() ? TEXT("") : *(TEXT("@") + P->GetAttachSocketName().ToString()));
                if (Rig) break;
            }
            UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_PROBE hit=%d dist=%.0f point=%s focus=%d chain=%s"), bHit, Hit.Distance,
                   C->Vehicle ? *C->Vehicle->GetActorTransform().InverseTransformPosition(Hit.ImpactPoint).ToCompactString() : TEXT("-"),
                   C->FocusedInteraction(), *Chain);
        });
    Add(TEXT("sc.SpawnWhereLooking"), TEXT("sc.SpawnWhereLooking: orbit/fly camera ray -> first person on that surface"), [Weak](const TArray<FString>&) {
        if (Weak.IsValid()) UE_LOG(LogSCViewerAutomation, Display, TEXT("FP_CMD spawn_where_looking ok=%d"), Weak->SpawnWhereLooking());
    });
}

void FSCViewerAutomation::RegisterPlayerCommands()
{
    TWeakObjectPtr<USCViewerSubsystem> Weak = Owner;
    auto Add = [&](const TCHAR* Name, const TCHAR* Help, TFunction<void(const TArray<FString>&)> Body) {
        Commands.Add(MakeUnique<FAutoConsoleCommand>(Name, Help,
            FConsoleCommandWithArgsDelegate::CreateLambda([Body](const TArray<FString>& Args) { Body(Args); })));
    };
    auto BodyOf = [](const TWeakObjectPtr<USCViewerSubsystem>& W) -> USCPlayerBodyComponent* {
        ASCFirstPersonCharacter* C = FirstPerson(W);
        if (C && !C->Body->HasBody()) UE_LOG(LogSCViewerAutomation, Warning, TEXT("PLAYER_CMD ignored: no player body (%s)"), *W->GetPlayerKitStatus());
        return C && C->Body->HasBody() ? C->Body.Get() : nullptr;
    };
    Add(TEXT("sc.Player.Load"), TEXT("sc.Player.Load: start (or retry) the player-body export/import"), [Weak](const TArray<FString>&) {
        if (Weak.IsValid()) UE_LOG(LogSCViewerAutomation, Display, TEXT("PLAYER_CMD load ok=%d"), Weak->RequestPlayerKit());
    });
    Add(TEXT("sc.Player.Weapon"), TEXT("sc.Player.Weapon <slot 1..n|none>: draw the weapon on that key (its own slot or none holsters)"),
        [Weak, BodyOf](const TArray<FString>& Args) {
            USCPlayerBodyComponent* Body = BodyOf(Weak);
            if (!Body || Args.IsEmpty()) return;
            const int32 Slot = Args[0].IsNumeric() ? FCString::Atoi(*Args[0]) - 1 : INDEX_NONE;
            const bool OK = Body->RequestSlot(Slot);
            UE_LOG(LogSCViewerAutomation, Display, TEXT("PLAYER_CMD weapon arg=%s ok=%d %s"), *Args[0], OK, *Body->Describe());
        });
    Add(TEXT("sc.Player.View"), TEXT("sc.Player.View fp|tp [orbit yaw deg] [distance cm]: first / third person body view"),
        [Weak, BodyOf](const TArray<FString>& Args) {
            USCPlayerBodyComponent* Body = BodyOf(Weak);
            if (!Body || Args.IsEmpty()) return;
            Body->SetThirdPerson(Args[0].Equals(TEXT("tp"), ESearchCase::IgnoreCase), Args.IsValidIndex(1) ? FCString::Atof(*Args[1]) : 0.f,
                                 Args.IsValidIndex(2) ? FCString::Atof(*Args[2]) : -1.f);
        });
    Add(TEXT("sc.Player.Body"), TEXT("sc.Player.Body 0|1: detach (capsule only) / re-attach the player body (frame-time A/B)"),
        [Weak](const TArray<FString>& Args) {
            ASCFirstPersonCharacter* C = FirstPerson(Weak);
            if (!C) return;
            if (Flag(Args, 0, true)) Weak->AttachPlayerKit();
            else { C->Body->Detach(); C->BodyStatus = TEXT("Player body off"); }
            UE_LOG(LogSCViewerAutomation, Display, TEXT("PLAYER_CMD body attached=%d"), C->Body->HasBody());
        });
    Add(TEXT("sc.Player.State"), TEXT("sc.Player.State: log the body, weapon and animator state (PLAYER_STATE)"), [Weak](const TArray<FString>&) {
        ASCFirstPersonCharacter* C = FirstPerson(Weak);
        if (C) UE_LOG(LogSCViewerAutomation, Display, TEXT("PLAYER_STATE kit=\"%s\" walk=%.0f run=%.0f %s"), *Weak->GetPlayerKitStatus(),
                      C->GetProfile().WalkSpeed, C->GetProfile().RunSpeed, *C->Body->Describe());
    });
}

void FSCViewerAutomation::ApplyPlayerOptions(USCViewerSubsystem& S)
{
    if (bPlayerOptionsApplied || (PlayerWeapon.IsEmpty() && PlayerView.IsEmpty())) return;
    ASCFirstPersonCharacter* C = S.GetFirstPerson();
    if (!C || !C->Body->HasBody()) return;
    bPlayerOptionsApplied = true;
    if (!PlayerView.IsEmpty())
    {
        TArray<FString> V;
        PlayerView.ParseIntoArray(V, TEXT(","));
        C->Body->SetThirdPerson(V[0].Equals(TEXT("tp"), ESearchCase::IgnoreCase), V.IsValidIndex(1) ? FCString::Atof(*V[1]) : 0.f,
                                V.IsValidIndex(2) ? FCString::Atof(*V[2]) : -1.f);
    }
    if (!PlayerWeapon.IsEmpty())
        UE_LOG(LogSCViewerAutomation, Display, TEXT("PLAYER_CMD weapon arg=%s ok=%d (command line)"), *PlayerWeapon,
               C->Body->RequestSlot(PlayerWeapon.IsNumeric() ? FCString::Atoi(*PlayerWeapon) - 1 : INDEX_NONE));
}

void FSCViewerAutomation::LogFrameSamples()
{
    auto Stats = [](TArray<float> V) {
        if (V.IsEmpty()) return FString(TEXT("n/a"));
        V.Sort();
        double Sum = 0;
        for (float X : V) Sum += X;
        return FString::Printf(TEXT("mean=%.2f p50=%.2f p95=%.2f"), Sum / V.Num(), V[V.Num() / 2], V[FMath::Min(V.Num() - 1, V.Num() * 95 / 100)]);
    };
    UE_LOG(LogSCViewerAutomation, Display, TEXT("SCRIPT_FRAMETIME tag=%s frames=%d frame_ms[%s] game_ms[%s] render_ms[%s] rhi_ms[%s] gpu_ms[%s]"),
           *SampleTag, FrameMs.Num(), *Stats(FrameMs), *Stats(GameMs), *Stats(RenderMs), *Stats(RhiMs), *Stats(GpuMs));
}

void FSCViewerAutomation::RunStep(USCViewerSubsystem& S, const FStep& Step)
{
    TArray<FString> Words;
    Step.Command.ParseIntoArrayWS(Words);
    if (Words.IsEmpty()) return;
    const double Now = FPlatformTime::Seconds();
    if (Words[0] == TEXT("shot") || Words[0] == TEXT("snap"))
    {
        // shot: hold still for an external screenshot (capture_route.sh), clearing any timed input first.
        // snap: the same marker without touching input (moving shots with sc.FP.Key held).
        if (ASCFirstPersonCharacter* C = S.GetFirstPerson(); C && Words[0] == TEXT("shot")) C->HoldMoveInput(FVector::ZeroVector, 0, false);
        UE_LOG(LogSCViewerAutomation, Display, TEXT("SCRIPT_SHOT tag=%s %s"), Words.IsValidIndex(1) ? *Words[1] : TEXT("shot"), *Where(S));
        if (ASCFirstPersonCharacter* C = S.GetFirstPerson(); C && FParse::Param(FCommandLine::Get(), TEXT("SCLightProbe")))
        {
            // -SCLightProbe: which lights reach the player and the surface under the crosshair (verification only).
            FHitResult Hit;
            const FVector Eye = C->Camera->GetComponentLocation();
            FCollisionQueryParams Q(SCENE_QUERY_STAT(SCLightProbeView), true, C);
            // Receiver channels: the player's light side (WP-U3b §12), and the channels of the component that was hit.
            auto Bits = [](const FLightingChannels& L) { return uint8(L.bChannel0 | (L.bChannel1 << 1) | (L.bChannel2 << 2)); };
            struct FPoint { const TCHAR* Name; FVector At; uint8 Channels; };
            TArray<FPoint> Points{{TEXT("player"), C->GetActorLocation(), uint8(C->Vehicle && C->Vehicle->IsInsideLightArea(C->GetActorLocation()) ? 2 : 1)}};
            if (C->GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + C->GetViewRotation().Vector() * 3000, ECC_Visibility, Q) && Hit.GetComponent())
                Points.Add({TEXT("view"), Hit.ImpactPoint + Hit.ImpactNormal * 3, Bits(Hit.GetComponent()->LightingChannels)});
            for (const FPoint& Point : Points)
                for (const FString& Line : FSCLightImporter::Probe(C->GetWorld(), Point.At, C, 10, Point.Channels))
                    UE_LOG(LogSCViewerAutomation, Display, TEXT("LIGHT_PROBE tag=%s at=%s %s"), Words.IsValidIndex(1) ? *Words[1] : TEXT("shot"), Point.Name, *Line);
        }
        StepAt = Now + (Words.IsValidIndex(2) ? FCString::Atod(*Words[2]) : 3.0);
        return;
    }
    if (Words[0] == TEXT("waitkit"))
    {
        // Hold the route until the player body is attached (or the kit failed), at most <seconds>.
        S.RequestPlayerKit();
        WaitKitUntil = Now + (Words.IsValidIndex(1) ? FCString::Atod(*Words[1]) : 600.0);
        StepAt = Now;
        return;
    }
    if (Words[0] == TEXT("waitpt"))
    {
        // Hold the route until the path tracer has accumulated its full sample count, at most <seconds>.
        if (!PtProbe) PtProbe = FSceneViewExtensions::NewExtension<FSCPathTracerProbe>();
        PtProbe->Samples = -1;
        bPtRestarted = false;
        WaitPtStart = Now;
        WaitPtUntil = Now + (Words.IsValidIndex(1) ? FCString::Atod(*Words[1]) : 600.0);
        StepAt = Now;
        return;
    }
    if (Words[0] == TEXT("frametime"))
    {
        SampleTag = Words.IsValidIndex(2) ? Words[2] : TEXT("sample");
        FrameMs.Reset(); GameMs.Reset(); RenderMs.Reset(); RhiMs.Reset(); GpuMs.Reset();
        SampleUntil = StepAt = Now + (Words.IsValidIndex(1) ? FCString::Atod(*Words[1]) : 5.0);
        return;
    }
    UWorld* World = S.GetGameInstance() ? S.GetGameInstance()->GetWorld() : nullptr;
    if (APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr)
        PC->ConsoleCommand(Step.Command);
    else if (GEngine)
        GEngine->Exec(World, *Step.Command);
}

void FSCViewerAutomation::TickScript(USCViewerSubsystem& S)
{
    if (Script.IsEmpty() || StepIndex >= Script.Num()) return;
    const double Now = FPlatformTime::Seconds();
    if (SampleUntil > 0)
    {
        FrameMs.Add(FApp::GetDeltaTime() * 1000);
        GameMs.Add(FPlatformTime::ToMilliseconds(GGameThreadTime));
        RenderMs.Add(FPlatformTime::ToMilliseconds(GRenderThreadTime));
        RhiMs.Add(FPlatformTime::ToMilliseconds(GRHIThreadTime));
        GpuMs.Add(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0)));
        if (Now < SampleUntil) return;
        SampleUntil = 0;
        LogFrameSamples();
    }
    if (StepIndex == INDEX_NONE)
    {
        // Start once a load finished (folder harness or archive), after the -SCCamera switch.
        if (!S.GetVehicle() || S.IsBusy() || bWaiting || !Queue.IsEmpty() || (!Camera.IsEmpty() && !bCameraApplied)) return;
        StepIndex = 0;
        StepAt = Now + Script[0].Delay;
        UE_LOG(LogSCViewerAutomation, Display, TEXT("SCRIPT_START steps=%d %s"), Script.Num(), *Where(S));
    }
    if (WaitKitUntil > 0)
    {
        ASCFirstPersonCharacter* C = S.GetFirstPerson();
        const bool bDone = (C && C->Body->HasBody()) || S.GetPlayerKitState() == USCViewerSubsystem::EPlayerKitState::Failed || Now > WaitKitUntil;
        if (!bDone) return;
        WaitKitUntil = 0;
        UE_LOG(LogSCViewerAutomation, Display, TEXT("SCRIPT_WAITKIT attached=%d status=\"%s\""), C && C->Body->HasBody(), *S.GetPlayerKitStatus());
        StepAt = FMath::Max(StepAt, Now + Script[StepIndex].Delay);
    }
    if (WaitPtUntil > 0)
    {
        static const IConsoleVariable* Spp = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing.SamplesPerPixel"));
        const int64 Target = Spp ? Spp->GetInt() : 1, Samples = PtProbe ? PtProbe->Samples.load() : -1;
        if (Samples >= 0 && Samples < Target) bPtRestarted = true;
        if (!(bPtRestarted && Samples >= Target) && Now <= WaitPtUntil) return;
        UE_LOG(LogSCViewerAutomation, Display, TEXT("SCRIPT_WAITPT samples=%lld target=%lld seconds=%.1f%s"), Samples, Target, Now - WaitPtStart,
               Now > WaitPtUntil ? TEXT(" timeout") : TEXT(""));
        WaitPtUntil = 0;
        StepAt = FMath::Max(StepAt, Now + Script[StepIndex].Delay);
    }
    // A timed move finishes before the next step's delay starts counting.
    if (ASCFirstPersonCharacter* C = S.GetFirstPerson(); C && C->IsHoldingMove())
    {
        StepAt = FMath::Max(StepAt, Now + Script[StepIndex].Delay);
        return;
    }
    if (Now < StepAt) return;
    const FStep Step = Script[StepIndex];
    RunStep(S, Step);
    UE_LOG(LogSCViewerAutomation, Display, TEXT("SCRIPT step=%d cmd=\"%s\" %s"), StepIndex + 1, *Step.Command, *Where(S));
    ++StepIndex;
    if (StepIndex < Script.Num())
        StepAt = FMath::Max(StepAt, Now) + Script[StepIndex].Delay;
    else
        UE_LOG(LogSCViewerAutomation, Display, TEXT("SCRIPT_DONE steps=%d %s"), Script.Num(), *Where(S));
}

void FSCViewerAutomation::Tick()
{
    USCViewerSubsystem* S = Owner.Get();
    if (!S) return;
    TickScript(*S);
    ApplyPlayerOptions(*S);
    if (S->IsBusy()) return;
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
