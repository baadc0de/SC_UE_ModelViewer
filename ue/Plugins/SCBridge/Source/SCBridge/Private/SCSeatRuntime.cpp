// The cockpit route uses the exact access port, interaction helper, socket and Mannequin states in the ship package.
#include "SCSeatRuntime.h"
#include "Cargo/SCCargoRecords.h"
#include "SCFirstPersonCharacter.h"
#include "SCPackageSource.h"
#include "SCPlayerBody.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "SCViewerSubsystem.h"
#include "Engine/SkeletalMesh.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCSeat, Log, All);

namespace
{
const FName SeatState(TEXT("SeatState"));
const FName Enter(TEXT("Enter")), Retract(TEXT("Retract")), Select(TEXT("Select")), Idle(TEXT("Idle"));
const FName Deselect(TEXT("Deselect")), Deploy(TEXT("Deploy")), Exit(TEXT("Exit"));

bool DescendsFrom(const USceneComponent* Component, const USceneComponent* Ancestor)
{
    for (; Component; Component = Component->GetAttachParent()) if (Component == Ancestor) return true;
    return false;
}
}

bool FSCSeatRuntime::FRoute::IsValid() const
{
    return Access.IsValid() && SeatRig.IsValid() && (Definition.DashboardPort.IsEmpty() || DashRig.IsValid());
}

FTransform FSCSeatRuntime::FRoute::Socket() const
{
    FTransform World = SeatRig->GetSocketTransform(Definition.Attachment);
    World.SetScale3D(FVector::OneVector);
    return World;
}

void FSCSeatRuntime::Setup(ASCVehicle& Vehicle)
{
    Host = &Vehicle;
    Routes.Reset();
    for (const FSCSceneInstance& Access : Vehicle.Manifest.Children)
    {
        if (Access.SeatEntrances.IsEmpty()) continue;
        USceneComponent* Anchor = Vehicle.PortAnchors.FindRef(SCCargoRecords::PortKey(Access.PortPath));
        if (!Anchor)
        {
            UE_LOG(LogSCSeat, Warning, TEXT("SEAT_ROUTE_UNAVAILABLE owner=%s reason=missing_access_anchor"), *Access.EntityName);
            continue;
        }
        for (const FSCSeatEntrance& Entrance : Access.SeatEntrances)
        {
            // This slice supports cockpit entrances without gateways or a dependency on simulated ship state.
            if (Entrance.bHasGateways || Entrance.bRequiresWorldState || Entrance.Enter.Interaction.IsNone() ||
                Entrance.Exit.Interaction.IsNone() || Entrance.Enter.SkipState != FName(TEXT("SkipDeploy")) ||
                Entrance.Exit.SkipState != FName(TEXT("SkipRetract")))
                continue;
            const FString Key = SCCargoRecords::PortKey(Entrance.SeatPortPath);
            const FSCSceneInstance* Child = Vehicle.Manifest.Children.FindByPredicate([&](const FSCSceneInstance& I) {
                return I.Seat.IsSet() && I.PortPath == Entrance.SeatPortPath;
            });
            USCRigComponent* Rig = Vehicle.PortRigs.FindRef(Key).Get();
            if (!Child || !Rig || Child->Seat->Attachment.IsNone() || !Rig->DoesSocketExist(Child->Seat->Attachment) ||
                (!Entrance.Enter.Attachment.IsNone() && Child->Seat->Attachment != Entrance.Enter.Attachment) ||
                (!Entrance.Exit.Attachment.IsNone() && Child->Seat->Attachment != Entrance.Exit.Attachment))
                continue;

            FRoute Route;
            Route.Access = Anchor;
            Route.SeatRig = Rig;
            Route.Definition = Child->Seat.GetValue();
            Route.Entrance = Entrance;
            if (!Entrance.DashboardPort.IsEmpty()) Route.Definition.DashboardPort = Entrance.DashboardPort;
            if (!Route.Definition.DashboardPort.IsEmpty())
            {
                TArray<FString> DashPath = Entrance.SeatPortPath;
                DashPath.Last() = Route.Definition.DashboardPort;
                Route.DashRig = Vehicle.PortRigs.FindRef(SCCargoRecords::PortKey(DashPath));
            }
            if (!Route.IsValid() || Route.Definition.Role == ESCSeatRole::Unknown)
            {
                UE_LOG(LogSCSeat, Warning, TEXT("SEAT_ROUTE_UNAVAILABLE route=%s reason=missing_rig_or_role"), *Entrance.Name);
                continue;
            }
            for (const FSCSeatAction& Action : Route.Definition.Actions)
            {
                const FString* Path = Action.State == Enter ? &Entrance.Enter.FragmentTags : Action.State == Exit ? &Entrance.Exit.FragmentTags : nullptr;
                if (Path && !Path->IsEmpty() && !Action.QueryTags.Contains(FName(**Path))) continue;
                if (!Route.Actions.Contains(Action.State) && FMath::IsFinite(Action.Speed) && Action.Speed > 0.f)
                    Route.Actions.Add(Action.State, Action);
            }
            bool bComplete = true;
            for (const FName State : {Enter, Retract, Select, Idle, Deselect, Deploy, Exit}) bComplete &= Route.Actions.Contains(State);
            if (!bComplete) continue;
            for (const FSCInteractionPoint& Point : Access.InteractionPoints)
            {
                const auto* Press = Point.Interactions.FindByPredicate([&](const FSCPointInteraction& I) { return I.Name == Entrance.Enter.Interaction; });
                if (!Press || !Point.bHasHelperTransform || Point.Radius <= 0) continue;
                Route.Point = Point;
                Route.Press = *Press;
                const FSCInstanceHelpers* Helpers = Vehicle.AttachmentHelpers.Find(Anchor);
                const FTransform* Resolved = Helpers ? Helpers->Frames.Find(Point.HelperName) : nullptr;
                if (!Resolved)
                {
                    UE_LOG(LogSCSeat, Warning, TEXT("SEAT_ROUTE_UNAVAILABLE route=%s reason=missing_helper helper=%s"), *Entrance.Name, *Point.HelperName);
                    continue;
                }
                Route.PointFrame = *Resolved;
                if (PlayRigs(Route, Vehicle, Deploy, true))
                {
                    const FVector Local = Vehicle.GetActorTransform().InverseTransformPosition(
                        (Route.PointFrame * Anchor->GetComponentTransform()).GetLocation());
                    UE_LOG(LogSCSeat, Display, TEXT("SEAT_POINT route=%s port=%s relative=%s label=\"%s\""),
                        *Entrance.Name, *Key, *Local.ToCompactString(), *Route.Press.DisplayName);
                    Routes.Add(MoveTemp(Route));
                }
                break;
            }
        }
    }
    UE_LOG(LogSCSeat, Display, TEXT("SEAT_ROUTES ready=%d"), Routes.Num());
}

int32 FSCSeatRuntime::Focus(const ASCFirstPersonCharacter& Character) const
{
    if (!Character.Body->HasBody() || Character.bNoclip || Character.bIsCrouched ||
        !Character.GetCharacterMovement()->IsMovingOnGround() || !Character.GetWorld())
        return INDEX_NONE;
    const FVector Eye = Character.InteractionOrigin();
    const FVector View = Character.GetViewRotation().Vector();
    int32 Best = INDEX_NONE;
    double BestAngle = -1, BestDistance = 0;
    for (int32 Index = 0; Index < Routes.Num(); ++Index)
    {
        const FRoute& Route = Routes[Index];
        if (!Route.IsValid()) continue;
        const FTransform Point = Route.PointFrame * Route.Access->GetComponentTransform();
        const FVector Offset = Point.GetLocation() - Eye;
        const double Distance = Offset.Size();
        if (Distance > 100. * Route.Point.Radius) continue;
        const FVector Direction = Offset.GetSafeNormal();
        const double Aim = Distance > UE_KINDA_SMALL_NUMBER ? FVector::DotProduct(View, Direction) : 1.;
        const auto Passes = [](double Cosine, const TOptional<float>& Limit) {
            return !Limit.IsSet() || Limit.GetValue() >= 180.f || Cosine >= FMath::Cos(FMath::DegreesToRadians(Limit.GetValue() / 2.));
        };
        // The local Cry +Y of this helper maps to its UE +Z before the asset's wrapper is applied.
        if (!Passes(Aim, Route.Point.ViewAngleLimit) ||
            !Passes(FVector::DotProduct(Point.GetUnitAxis(EAxis::Z), -Direction), Route.Point.AngleRange)) continue;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(SCSeatPoint), true, &Character);
        Params.AddIgnoredActor(Character.Body->GetKit());
        FHitResult Hit;
        if (Character.GetWorld()->LineTraceSingleByChannel(Hit, Eye, Point.GetLocation(), ECC_Visibility, Params) &&
            !DescendsFrom(Hit.GetComponent(), Route.Access.Get()) && !DescendsFrom(Hit.GetComponent(), Route.SeatRig.Get()))
            continue;
        if (Best == INDEX_NONE || Aim > BestAngle || (FMath::IsNearlyEqual(Aim, BestAngle) && Distance < BestDistance))
        {
            Best = Index;
            BestAngle = Aim;
            BestDistance = Distance;
        }
    }
    return Best;
}

FString FSCSeatRuntime::Prompt(const ASCFirstPersonCharacter& Character) const
{
    if (Phase == EPhase::Seated) return TEXT("E: Exit seat");
    if (Phase == EPhase::LoadClips) return TEXT("Loading seat animations…");
    if (Phase != EPhase::Walk) return FString();
    const int32 Index = Focus(Character);
    if (Index == INDEX_NONE) return FString();
    const FSCPointInteraction& Press = Routes[Index].Press;
    return FString::Printf(TEXT("E: %s"), Press.DisplayName.IsEmpty() ? *Press.Name.ToString() : *Press.DisplayName);
}

ESCSeatRole FSCSeatRuntime::OccupiedRole() const
{
    return Phase == EPhase::Seated && Host.IsValid() && Routes.IsValidIndex(Active) && Routes[Active].IsValid()
        ? Routes[Active].Definition.Role : ESCSeatRole::Unknown;
}

bool FSCSeatRuntime::OwnsMovement() const
{
    return Phase != EPhase::Walk && Phase != EPhase::LoadClips && Phase != EPhase::Holster;
}

bool FSCSeatRuntime::Interact(ASCFirstPersonCharacter& Character)
{
    if (Phase == EPhase::Seated)
    {
        StartAction(EPhase::Deselect, Deselect);
        return true;
    }
    if (Phase != EPhase::Walk) return true;
    Active = Focus(Character);
    if (Active == INDEX_NONE) return false;
    if (RequestClips(Character)) return true;
    UE_LOG(LogSCSeat, Warning, TEXT("SEAT_CLIPS archive unavailable"));
    Active = INDEX_NONE;
    return false;
}

bool FSCSeatRuntime::RequestClips(ASCFirstPersonCharacter& Character)
{
    const FString Gender = Character.Body->Gender();
    TArray<FString> Names;
    for (const auto& Pair : Routes[Active].Actions) Names.AddUnique(Pair.Value.Animation.ToString());
    Names.Sort();
    TMap<FString, int32> Bones;
    const auto& Ref = Character.Body->GetLeader()->GetSkeletalMeshAsset()->GetRefSkeleton();
    for (int32 Index = 0; Index < Ref.GetNum(); ++Index) Bones.Add(Ref.GetBoneName(Index).ToString(), Index);

    FString File;
    if (FParse::Value(FCommandLine::Get(), TEXT("SCSeatClipsPackage="), File))
    {
        Parse = UE::Tasks::Launch(UE_SOURCE_LOCATION, [File, Gender, Names = MoveTemp(Names), Bones = MoveTemp(Bones)] {
            TArray64<uint8> Tar;
            if (!FFileHelper::LoadFileToArray(Tar, *File)) return FClipResult{{}, TEXT("Cannot read seat clips: ") + File};
            return ReadClips(MoveTemp(Tar), Gender, Names, Bones);
        }, UE::Tasks::ETaskPriority::BackgroundNormal);
    }
    else
    {
        UGameInstance* Game = Character.GetGameInstance();
        const USCViewerSubsystem* Viewer = Game ? Game->GetSubsystem<USCViewerSubsystem>() : nullptr;
        const auto Client = Viewer ? Viewer->GetClient() : nullptr;
        if (!Client) return false;
        Progress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
        auto Export = Client->ExportPlayerClips(Gender, Names, Progress.ToSharedRef());
        Parse = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Export, Gender, Names = MoveTemp(Names), Bones = MoveTemp(Bones)]() mutable {
            FSCExportResult& Result = Export.GetResult();
            if (!Result.bOK) return FClipResult{{}, Result.Error.ToString()};
            return ReadClips(MoveTemp(Result.Tar), Gender, Names, Bones);
        }, UE::Tasks::Prerequisites(Export), UE::Tasks::ETaskPriority::BackgroundNormal);
    }
    PlayerKit = Character.Body->GetKit();
    Phase = EPhase::LoadClips;
    return true;
}

FSCSeatRuntime::FClipResult FSCSeatRuntime::ReadClips(TArray64<uint8>&& Tar, const FString& Gender, const TArray<FString>& Names,
    const TMap<FString, int32>& Bones)
{
    FSCTarPackageSource Source(MoveTemp(Tar));
    FClipResult Result;
    if (!Source.IsValid())
    {
        Result.Error = Source.GetError();
        return Result;
    }
    auto ReadText = [&](const FString& Path, FString& Json) {
        TArray<uint8> Bytes;
        if (!Source.ReadBytes(Path, Bytes, Result.Error)) return false;
        Json = FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()));
        return true;
    };
    FString Json;
    TSharedPtr<FJsonObject> Scene;
    const TSharedPtr<FJsonObject>* Root = nullptr;
    const TSharedPtr<FJsonObject>* ClipSet = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
    FString PackageGender;
    if (!ReadText(TEXT("Packages/player_clips/scene.json"), Json) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Scene) || !Scene ||
        !Scene->TryGetObjectField(TEXT("root_entity"), Root) || !(*Root)->TryGetArrayField(TEXT("animations"), Records) ||
        !Scene->TryGetObjectField(TEXT("player_clips"), ClipSet) || !(*ClipSet)->TryGetStringField(TEXT("gender"), PackageGender) ||
        !PackageGender.Equals(Gender, ESearchCase::IgnoreCase))
    {
        if (Result.Error.IsEmpty()) Result.Error = TEXT("Invalid seat clip manifest");
        return Result;
    }
    TMap<FName, FString> Paths;
    for (const auto& Value : *Records)
        if (Value && Value->Type == EJson::Object)
        {
            FString Name, Sidecar;
            const auto Object = Value->AsObject();
            if (Object && Object->TryGetStringField(TEXT("name"), Name) && Object->TryGetStringField(TEXT("sidecar"), Sidecar))
                Paths.Add(FName(*Name), Sidecar);
        }
    for (const FString& Name : Names)
    {
        const FString* Path = Paths.Find(FName(*Name));
        const auto Clip = MakeShared<FSCPlayerClip>();
        if (!Path)
        {
            Result.Error = TEXT("Missing seat animation ") + Name;
            return Result;
        }
        if (!ReadText(FString(TEXT("Packages/player_clips/")) + *Path, Json) ||
            !FSCPlayerClip::Parse(Json,
                [&](const FString& Bone) { const int32* Index = Bones.Find(Bone); return Index ? *Index : INDEX_NONE; },
                *Clip, Result.Error) || Clip->bAdditive ||
            Clip->Tracks.IsEmpty() || !FMath::IsFinite(Clip->Duration) || Clip->Duration <= 0.f)
        {
            if (Result.Error.IsEmpty()) Result.Error = TEXT("Unreadable seat animation ") + Name;
            return Result;
        }
        Result.Clips.Add(FName(*Name), Clip);
    }
    return Result;
}

void FSCSeatRuntime::Begin(ASCFirstPersonCharacter& Character)
{
    const FSCPlayerAnimator& Animator = Character.Body->GetAnimator();
    const FRoute& Route = Routes[Active];
    const FSCPlayerClip& EnterClip = *Clips.FindChecked(Route.Actions.FindChecked(Enter).Animation);
    const FSCPlayerClip& ExitClip = *Clips.FindChecked(Route.Actions.FindChecked(Exit).Animation);
    if (Animator.ActionRootDelta(EnterClip, EnterClip.Duration).Equals(FTransform::Identity) ||
        Animator.ActionRootDelta(ExitClip, ExitClip.Duration).Equals(FTransform::Identity))
    {
        UE_LOG(LogSCSeat, Warning, TEXT("SEAT_CLIPS missing entry or exit root motion"));
        Stop(Character);
        return;
    }
    Character.ClearViewHold();
    Character.ClearInput();
    Character.SetBase(static_cast<FMovementBaseInterfaceData*>(nullptr));
    Character.GetCharacterMovement()->StopMovementImmediately();
    Character.GetCharacterMovement()->DisableMovement();
    SavedCollision = Character.GetCapsuleComponent()->GetCollisionEnabled();
    Character.GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    StartInShip = Character.GetActorTransform().GetRelativeTransform(Host->GetActorTransform());
    BodyInActor = Character.Body->BodyFrame().GetRelativeTransform(Character.GetActorTransform());
    BodyInActor.SetScale3D(FVector::OneVector);
    Character.Body->SetAiming(false);
    Character.Body->SetTrigger(false);
    bSavedControllerYaw = Character.bUseControllerRotationYaw;
    Character.bUseControllerRotationYaw = false;
    RelativeView = (Character.GetActorQuat().Inverse() * Character.GetViewRotation().Quaternion()).Rotator();
    StartAction(EPhase::Enter, Enter);
}

void FSCSeatRuntime::StartAction(EPhase NewPhase, FName State)
{
    Phase = NewPhase;
    ActionState = State;
    ActionClock = 0.f;
    UE_LOG(LogSCSeat, Display, TEXT("SEAT_STATE name=\"%s\" state=%s action=%s"),
        *Routes[Active].Entrance.Name, *State.ToString(), *Routes[Active].Actions.FindChecked(State).Animation.ToString());
}

bool FSCSeatRuntime::PlayRigs(const FRoute& Route, ASCVehicle& Vehicle, FName Tag, bool bHold)
{
    const auto Start = bHold ? ESCFragmentStart::HoldLast : ESCFragmentStart::Begin;
    const bool bSeat = Vehicle.PlayFragmentAt(SeatState, Tag, Route.SeatRig.Get(), 1.f, Start);
    const bool bDash = !Route.DashRig.IsValid() || Vehicle.PlayFragmentAt(SeatState, Tag, Route.DashRig.Get(), 1.f, Start);
    UE_LOG(LogSCSeat, Display, TEXT("SEAT_RIG route=%s state=%s seat=%d dashboard=%d hold=%d"),
        *Route.Entrance.Name, *Tag.ToString(), bSeat, bDash, bHold);
    return bSeat && bDash;
}

FTransform FSCSeatRuntime::Motion(const ASCFirstPersonCharacter& Character, float Seconds) const
{
    const FName Name = Routes[Active].Actions.FindChecked(ActionState).Animation;
    const FTransform Root = Character.Body->GetAnimator().ActionRootDelta(*Clips.FindChecked(Name), Seconds);
    return BodyInActor.Inverse() * Root * BodyInActor;
}

FTransform FSCSeatRuntime::SeatedTransform(const FRoute& Route) const
{
    const FTransform Socket = Route.Socket();
    // The raw seat bone uses Cry +Y forward and +Z up. In the imported node's local basis these are +Z and -X.
    const FQuat Heading = FRotationMatrix::MakeFromXZ(Socket.GetUnitAxis(EAxis::Z), -Socket.GetUnitAxis(EAxis::X)).ToQuat();
    return FTransform(Heading, Socket.GetLocation() - Heading.RotateVector(BodyInActor.GetLocation()));
}

bool FSCSeatRuntime::ApplyLook(ASCFirstPersonCharacter& Character, FVector2D Pixels)
{
    if (!OwnsMovement()) return false;
    if (!Character.Controller || !Routes.IsValidIndex(Active)) return true;
    const FSCSeatDefinition& Seat = Routes[Active].Definition;
    RelativeView.Yaw = FMath::Clamp(FRotator::NormalizeAxis(RelativeView.Yaw + Pixels.X * .2),
        Seat.MinYaw.Get(-180.), Seat.MaxYaw.Get(180.));
    RelativeView.Pitch = FMath::Clamp(RelativeView.Pitch - Pixels.Y * .2, Seat.MinPitch.Get(-89.), Seat.MaxPitch.Get(89.));
    RelativeView.Roll = 0;
    Character.Controller->SetControlRotation((Character.GetActorQuat() * RelativeView.Quaternion()).Rotator());
    return true;
}

bool FSCSeatRuntime::StandingClear(const ASCFirstPersonCharacter& Character, FVector Centre)
{
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCSeatExit), false, &Character);
    Character.AddGridIgnores(Params, Centre, Character.GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
    return Character.GetWorld() && !Character.GetWorld()->OverlapBlockingTestByChannel(
        Centre, FQuat::Identity, ECC_Pawn, Character.GetCapsuleComponent()->GetCollisionShape(), Params);
}

bool FSCSeatRuntime::ReturnToSeat(ASCFirstPersonCharacter& Character, const FRoute& Route)
{
    UE_LOG(LogSCSeat, Display, TEXT("SEAT_EXIT_BLOCKED route=%s"), *Route.Entrance.Name);
    if (!PlayRigs(Route, *Host, Retract))
    {
        Stop(Character);
        return false;
    }
    Character.SetActorTransform(SeatedTransform(Route), false, nullptr, ETeleportType::TeleportPhysics);
    StartAction(EPhase::Retract, Retract);
    return true;
}

void FSCSeatRuntime::Restore(ASCFirstPersonCharacter& Character)
{
    Character.Body->SetSeatAction(nullptr, 0.f, 1.f, 0.f);
    Character.bUseControllerRotationYaw = bSavedControllerYaw;
    Character.GetCapsuleComponent()->SetCollisionEnabled(SavedCollision);
    Character.GetCharacterMovement()->SetMovementMode(MOVE_Walking);
    Character.ClearInput();
}

void FSCSeatRuntime::Stop(ASCFirstPersonCharacter& Character)
{
    if (Progress) Progress->RequestCancel();
    Progress.Reset();
    Parse = {};
    if (OwnsMovement())
    {
        FTransform Spawn = Character.LastSpawn;
        if (Host.IsValid() && !Host->IsActorBeingDestroyed())
        {
            Spawn = StartInShip * Host->GetActorTransform();
            if (Routes.IsValidIndex(Active) && Routes[Active].IsValid()) PlayRigs(Routes[Active], *Host, Deploy, true);
        }
        bool bClear = StandingClear(Character, Spawn.GetLocation());
        if (!bClear && StandingClear(Character, Character.LastSpawn.GetLocation()))
        {
            Spawn = Character.LastSpawn;
            bClear = true;
        }
        Spawn.SetRotation(FRotator(0, Spawn.Rotator().Yaw, 0).Quaternion());
        Character.SetActorTransform(Spawn, false, nullptr, ETeleportType::TeleportPhysics);
        Phase = EPhase::Walk;
        if (!bClear) Character.SetActorEnableCollision(false);
        Restore(Character);
        if (!bClear && !Character.IsActorBeingDestroyed()) Character.ToggleNoclip();
    }
    Phase = EPhase::Walk;
    Active = INDEX_NONE;
    Clips.Reset();
}

void FSCSeatRuntime::Tick(ASCFirstPersonCharacter& Character, float DeltaSeconds)
{
    if (Phase == EPhase::Walk) return;
    if (!Host.IsValid() || Host->IsActorBeingDestroyed() || Character.Vehicle != Host.Get() || !PlayerKit.IsValid() || Character.Body->GetKit() != PlayerKit.Get() ||
        !Routes.IsValidIndex(Active) || !Routes[Active].IsValid())
    {
        Stop(Character);
        return;
    }
    if (Phase == EPhase::LoadClips)
    {
        if (Focus(Character) != Active) { Stop(Character); return; }
        if (!Parse.IsCompleted()) return;
        FClipResult Result = MoveTemp(Parse.GetResult());
        Parse = {};
        Progress.Reset();
        if (!Result.Error.IsEmpty())
        {
            UE_LOG(LogSCSeat, Warning, TEXT("SEAT_CLIPS %s"), *Result.Error);
            Stop(Character);
            return;
        }
        Clips = MoveTemp(Result.Clips);
        Phase = EPhase::Holster;
    }
    if (Phase == EPhase::Holster)
    {
        if (Focus(Character) != Active) { Stop(Character); return; }
        const auto Weapon = Character.Body->GetWeaponState();
        if (Weapon == ESCWeaponState::Armed) Character.Body->RequestSlot(INDEX_NONE);
        if (Weapon == ESCWeaponState::Unarmed) Begin(Character);
        return;
    }

    const FRoute& Route = Routes[Active];
    const FSCSeatAction& Action = Route.Actions.FindChecked(ActionState);
    const FSCPlayerClip* Clip = Clips.FindChecked(Action.Animation).Get();
    ActionClock += DeltaSeconds;
    const float Seconds = ActionClock * Action.Speed;
    const float Sample = Action.bLoop ? FMath::Fmod(Seconds, Clip->Duration) : FMath::Min(Seconds, Clip->Duration);
    const float Blend = Action.BlendIn > 0.f ? FMath::Clamp(ActionClock / Action.BlendIn, 0.f, 1.f) : 1.f;
    FTransform Frame = SeatedTransform(Route);
    if (Phase == EPhase::Enter)
    {
        const FTransform Start = StartInShip * Host->GetActorTransform();
        const FTransform Aligned = Motion(Character, Clip->Duration).Inverse() * Frame;
        FTransform Warped;
        Warped.Blend(Start, Aligned, Action.PositionAdjust > 0.f ? FMath::Clamp(ActionClock / Action.PositionAdjust, 0.f, 1.f) : 1.f);
        Frame = Motion(Character, Sample) * Warped;
    }
    if (Phase == EPhase::Exit) Frame = Motion(Character, Sample) * ExitInShip * Host->GetActorTransform();
    Character.SetActorTransform(Frame, false, nullptr, ETeleportType::TeleportPhysics);
    ApplyLook(Character, FVector2D::ZeroVector);
    Character.Body->SetSeatAction(Clip, Sample, Blend, float(RelativeView.Pitch));

    const bool bBodyDone = Seconds >= Clip->Duration;
    const bool bRigsDone = !Host->IsRigPlaying(Route.SeatRig.Get()) && !Host->IsRigPlaying(Route.DashRig.Get());
    switch (Phase)
    {
    case EPhase::Enter:
        if (bBodyDone)
        {
            const double Error = FVector::Dist(Frame.GetLocation(), SeatedTransform(Route).GetLocation());
            UE_LOG(LogSCSeat, Display, TEXT("SEAT_ATTACH route=%s err_cm=%.3f"), *Route.Entrance.Name, Error);
            if (PlayRigs(Route, *Host, Retract)) StartAction(EPhase::Retract, Retract);
            else Stop(Character);
        }
        break;
    case EPhase::Retract:
        if (bRigsDone) StartAction(EPhase::Select, Select);
        break;
    case EPhase::Select:
        if (bBodyDone) StartAction(EPhase::Seated, Idle);
        break;
    case EPhase::Deselect:
        if (bBodyDone)
        {
            if (PlayRigs(Route, *Host, Deploy)) StartAction(EPhase::Deploy, Deploy);
            else Stop(Character);
        }
        break;
    case EPhase::Deploy:
        if (bRigsDone)
        {
            StartAction(EPhase::Exit, Exit);
            ExitInShip = Frame.GetRelativeTransform(Host->GetActorTransform());
            const FSCPlayerClip& ExitClip = *Clips.FindChecked(Route.Actions.FindChecked(Exit).Animation);
            const FVector End = (Motion(Character, ExitClip.Duration) * Frame).GetLocation();
            if (!StandingClear(Character, End)) ReturnToSeat(Character, Route);
        }
        break;
    case EPhase::Exit:
        if (bBodyDone)
        {
            // A cab or loose cargo may have arrived since the preflight. Check the actual standing point again.
            if (!StandingClear(Character, Frame.GetLocation()))
            {
                ReturnToSeat(Character, Route);
                break;
            }
            Character.SetActorRotation(FRotator(0, Frame.Rotator().Yaw, 0));
            Restore(Character);
            Phase = EPhase::Walk;
            Active = INDEX_NONE;
            Clips.Reset();
            UE_LOG(LogSCSeat, Display, TEXT("SEAT_STATE state=Walking"));
        }
        break;
    default: break;
    }
}
