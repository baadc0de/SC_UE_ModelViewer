// FSCEnvironmentAccess: which authored end pose of each rig clears the way into an inventory container (WP-K1).
// Poses are evaluated without touching the actor: a clip's last frame over the rig's current local pose, the bones'
// component-space transforms, and the rig's collidable parts carried along by their bones.
#include "SCEnvironmentAccess.h"
#include "Cargo/SCCargoGridComponent.h"
#include "SCClipCatalog.h"
#include "SCEnvironment.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "SCViewerSubsystem.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCAccess, Log, All);

namespace
{
struct FRequest
{
    FTransform Frame;   // relative to the vehicle's actor (survives the environment being moved by placement)
    FBox Volume = FBox(ForceInit);
    double Reach = 0, Tolerance = 0, Band = 0;
    FName Requester;
    double MinGain = 1e-3; // m3: the least corridor volume a pose must clear to be played (one lattice cell for grids)
};
TMap<TWeakObjectPtr<ASCVehicle>, TMap<FName, FRequest>>& Requests()
{
    static TMap<TWeakObjectPtr<ASCVehicle>, TMap<FName, FRequest>> Map;
    return Map;
}

double BoxVolumeM3(const FBox& A, const FBox& B)
{
    const FVector Min = FVector::Max(A.Min, B.Min), Max = FVector::Min(A.Max, B.Max);
    const FVector D = Max - Min;
    return D.X > 0 && D.Y > 0 && D.Z > 0 ? D.X * D.Y * D.Z * 1e-6 : 0.0;
}

TArray<FTransform> ComponentSpace(const FReferenceSkeleton& Ref, const TArray<FTransform>& Local)
{
    TArray<FTransform> Space;
    Space.SetNum(Local.Num());
    for (int32 I = 0; I < Local.Num(); ++I)
    {
        const int32 Parent = Ref.GetParentIndex(I);
        Space[I] = Parent == INDEX_NONE ? Local[I] : Local[I] * Space[Parent];
    }
    return Space;
}

/** A collidable part carried by the rig: its current world transform and the bone it rides (INDEX_NONE: the rig's
 *  component frame). */
struct FPart
{
    const UPrimitiveComponent* Component = nullptr;
    FTransform World;
    int32 Bone = INDEX_NONE;
};

TArray<FPart> CollidableParts(const USCRigComponent* Rig)
{
    TArray<FPart> Parts;
    const FReferenceSkeleton& Ref = Rig->GetSkeletalMeshAsset()->GetRefSkeleton();
    TArray<USceneComponent*> Descendants;
    Rig->GetChildrenComponents(true, Descendants);
    for (USceneComponent* C : Descendants)
    {
        const auto* P = Cast<UPrimitiveComponent>(C);
        if (!P || !P->IsRegistered() || !P->IsCollisionEnabled()) continue;
        const USceneComponent* Direct = C;
        while (Direct->GetAttachParent() && Direct->GetAttachParent() != Rig) Direct = Direct->GetAttachParent();
        FPart Part;
        Part.Component = P;
        Part.World = P->GetComponentTransform();
        if (!Direct->GetAttachSocketName().IsNone()) Part.Bone = Ref.FindBoneIndex(Rig->GetSocketBoneName(Direct->GetAttachSocketName()));
        Parts.Add(Part);
    }
    return Parts;
}

struct FScore
{
    double Blocked = 0;   // m3 of collidable parts in the access corridors
    bool bCarries = false; // a part is the floor: its top within the floor band and covering most of the footprint
};

/** The parts posed by Local (bones moved from Current), scored against the request in its frame. */
FScore Score(const USCRigComponent* Rig, const TArray<FPart>& Parts, const TArray<FTransform>& CurrentSpace, const TArray<FTransform>& Local,
             const FTransform& Frame, const FBox& Volume, double Reach, double Tolerance, double Band)
{
    const FReferenceSkeleton& Ref = Rig->GetSkeletalMeshAsset()->GetRefSkeleton();
    const TArray<FTransform> Space = ComponentSpace(Ref, Local);
    const FTransform& RigWorld = Rig->GetComponentTransform();
    const FTransform ToFrame = Frame.Inverse();
    const FBox Inner(Volume.Min + FVector(Tolerance), Volume.Max - FVector(Tolerance));
    FBox Corridors[5] = {Inner, Inner, Inner, Inner, Inner};
    Corridors[0].Max.X += Reach;
    Corridors[1].Min.X -= Reach;
    Corridors[2].Max.Y += Reach;
    Corridors[3].Min.Y -= Reach;
    Corridors[4].Max.Z += Reach;
    const double Footprint = (Volume.Max.X - Volume.Min.X) * (Volume.Max.Y - Volume.Min.Y);
    FScore Out;
    for (const FPart& Part : Parts)
    {
        FTransform World = Part.World;
        if (Part.Bone != INDEX_NONE && CurrentSpace.IsValidIndex(Part.Bone) && Space.IsValidIndex(Part.Bone))
            World = Part.World * (CurrentSpace[Part.Bone] * RigWorld).Inverse() * (Space[Part.Bone] * RigWorld);
        const FBox Box = Part.Component->CalcBounds(World * ToFrame).GetBox();
        for (const FBox& Corridor : Corridors) Out.Blocked += BoxVolumeM3(Box, Corridor);
        if (Box.Max.Z >= Volume.Min.Z - Band && Box.Max.Z <= Volume.Min.Z + Tolerance)
        {
            const double W = FMath::Min(Box.Max.X, Volume.Max.X) - FMath::Max(Box.Min.X, Volume.Min.X);
            const double D = FMath::Min(Box.Max.Y, Volume.Max.Y) - FMath::Max(Box.Min.Y, Volume.Min.Y);
            Out.bCarries |= W > 0 && D > 0 && 2 * W * D > Footprint;
        }
    }
    return Out;
}

// The playback layers posing Rig (clip index:frame:order, t = targeted at this rig): the record of what posed it.
FString Layers(const ASCVehicle* Vehicle, const USCRigComponent* Rig)
{
    FString Out;
    for (const TArray<FSCClipPlayback>* List : {&Vehicle->HeldPlayback, &Vehicle->Playback})
        for (const FSCClipPlayback& P : *List)
            if (ASCVehicle::AppliesTo(P, Rig) &&
                Vehicle->ClipCatalog->Clips[P.Clip].Bindings.ContainsByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Rig->GetSkeletalMeshAsset(); }))
                Out += FString::Printf(TEXT("%s%d:%.0f:%llu%s"), Out.IsEmpty() ? TEXT("") : TEXT(","), P.Clip, P.Frame, P.Order, P.bTargeted ? TEXT("t") : TEXT(""));
    return Out.IsEmpty() ? TEXT("-") : Out;
}

FSCAccessResult Apply(ASCVehicle* Vehicle, FName Key, const FRequest& R)
{
    FSCAccessResult Result;
    if (!Vehicle || !Vehicle->ClipCatalog) return Result;
    const FTransform Frame = R.Frame * Vehicle->GetActorTransform();
    constexpr double SameM3 = 1e-3; // comparison resolution of the blocked volume (a litre)
    const TArray<FSCImportedClip>& Clips = Vehicle->ClipCatalog->Clips;
    for (USCRigComponent* Rig : Vehicle->Rigs)
    {
        USkeletalMesh* Mesh = Rig ? Rig->GetSkeletalMeshAsset() : nullptr;
        if (!Mesh) continue;
        const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
        TArray<FTransform> Current = Rig->LocalPose.Num() == Ref.GetNum() ? Rig->LocalPose : Ref.GetRefBonePose();
        // A door already open (its pad, a sensor) clears the corridors only while it stays open: it is scored from its closed
        // pose (a single-procedural door's transition start, an open/close door's close fragment end), so the request depends
        // on it as on a door it opens, and the hold reads Closed once it is closed again.
        for (int32 D = 0; D < Vehicle->Doors.Num(); ++D)
        {
            const FSCDoorInteraction& Door = Vehicle->Doors[D];
            if (Door.Rig != Rig || !Vehicle->IsDoorOpen(D)) continue;
            const FName Tag = Door.bReverseClose ? Door.OpenTag : Door.CloseTag;
            const int32 Closing = Clips.IndexOfByPredicate([&](const FSCImportedClip& C) {
                return C.Bindings.ContainsByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Mesh; }) &&
                       C.Fragments.ContainsByPredicate([&](const FSCClipFragment& F) { return F.Fragment == Door.Fragment && (F.FragmentTags.Contains(Tag) || F.Tags.Contains(Tag)); });
            });
            if (Closing == INDEX_NONE) continue;
            const FSCImportedClip& C = Clips[Closing];
            const double ClosedFrame = Door.bReverseClose ? 0.0 : double(C.LastFrame);
            for (const FSCBoneTrack& T : C.Bindings.FindByPredicate([&](const FSCClipBinding& X) { return X.Mesh == Mesh; })->Tracks)
            {
                if (!Current.IsValidIndex(T.Bone)) continue;
                const FTransform Sample = FSCClipSampling::Sample(T, FSCClipSampling::TrackFrame(C, T, ClosedFrame, false));
                if (T.bRotation) Current[T.Bone].SetRotation(Sample.GetRotation());
                if (T.bPosition) Current[T.Bone].SetTranslation(Sample.GetTranslation());
            }
        }
        // A door moves only through its own interaction (C2 fixes): its candidate poses are its door interactions' open ends
        // (each fragment driving the rig: a root rig may carry several hatches), and it is opened as a press would
        // (ASCVehicle::PlayDoor), so its recorded state, its pads and its port links follow.
        TMap<int32, int32> ClipDoor; // candidate clip -> the door interaction whose open fragment it plays
        for (int32 D = 0; D < Vehicle->Doors.Num(); ++D)
        {
            const FSCDoorInteraction& Door = Vehicle->Doors[D];
            if (Door.Rig != Rig) continue;
            for (int32 Index = 0; Index < Clips.Num(); ++Index)
                if (!ClipDoor.Contains(Index) && Clips[Index].Fragments.ContainsByPredicate([&](const FSCClipFragment& F) {
                        return F.Fragment == Door.Fragment && (F.FragmentTags.Contains(Door.OpenTag) || F.Tags.Contains(Door.OpenTag));
                    }))
                    ClipDoor.Add(Index, D);
        }
        const bool bDoorRig = Vehicle->DoorForRig(Rig) != INDEX_NONE;
        TArray<int32> Candidates;
        for (int32 Index = 0; Index < Clips.Num(); ++Index)
            if (const FSCClipBinding* B = Clips[Index].Bindings.FindByPredicate([&](const FSCClipBinding& X) { return X.Mesh == Mesh; });
                B && !B->Tracks.IsEmpty() && FSCEnvironment::ClipRigs(Vehicle, Index).Contains(Rig) && // the rig's own clips
                (!bDoorRig || ClipDoor.Contains(Index)))
                Candidates.Add(Index);
        if (Candidates.IsEmpty()) continue;
        const TArray<FPart> Parts = CollidableParts(Rig);
        if (Parts.IsEmpty()) continue;
        ++Result.RigsTested;
        const TArray<FTransform> CurrentSpace = ComponentSpace(Ref, Current);
        const FScore Before = Score(Rig, Parts, CurrentSpace, Current, Frame, R.Volume, R.Reach, R.Tolerance, R.Band);
        FScore Best = Before;
        int32 BestClip = INDEX_NONE;
        FString Tried;
        for (const int32 Index : Candidates)
        {
            const FSCImportedClip& Clip = Clips[Index];
            TArray<FTransform> Pose = Current;
            for (const FSCBoneTrack& T : Clip.Bindings.FindByPredicate([&](const FSCClipBinding& X) { return X.Mesh == Mesh; })->Tracks)
            {
                if (!Pose.IsValidIndex(T.Bone)) continue;
                const FTransform Sample = FSCClipSampling::Sample(T, FSCClipSampling::TrackFrame(Clip, T, Clip.LastFrame, false));
                if (T.bRotation) Pose[T.Bone].SetRotation(Sample.GetRotation());
                if (T.bPosition) Pose[T.Bone].SetTranslation(Sample.GetTranslation());
            }
            const FScore S = Score(Rig, Parts, CurrentSpace, Pose, Frame, R.Volume, R.Reach, R.Tolerance, R.Band);
            Tried += FString::Printf(TEXT(" %d:%s:%.2f%s"), Index, *Clip.Id.ToString(), S.Blocked, S.bCarries ? TEXT(":floor") : TEXT(""));
            // Less in the way wins; at equal obstruction a pose that is the floor wins (a platform raised to the container);
            // otherwise the current pose stays: nothing moves without a gain.
            if (S.Blocked < Best.Blocked - FMath::Max(R.MinGain, SameM3) || (S.Blocked <= Best.Blocked + SameM3 && S.bCarries && !Best.bCarries))
            {
                Best = S;
                BestClip = Index;
            }
        }
        if (Before.Blocked > SameM3 || BestClip != INDEX_NONE)
            UE_LOG(LogSCAccess, Display, TEXT("ENV_ACCESS_RIG key=%s rig=%s blocked_m3=%.2f->%.2f floor=%d->%d clip=%s candidates=%s layers=%s"), *Key.ToString(),
                   *Rig->OwnerEntity, Before.Blocked, Best.Blocked, Before.bCarries, Best.bCarries,
                   BestClip == INDEX_NONE ? TEXT("-") : *Clips[BestClip].Id.ToString(), *Tried, *Layers(Vehicle, Rig));
        if (BestClip == INDEX_NONE) continue;
        // A door already opening (its target open) is not started again but is waited for like one this request starts.
        const int32 Door = ClipDoor.FindRef(BestClip, INDEX_NONE);
        if (bDoorRig)
        {
            if (Door == INDEX_NONE) continue;
            if (!R.Requester.IsNone() && (Vehicle->IsDoorLocked(Door) ||
                (Vehicle->ShipDoors.IsValidIndex(Door) && Vehicle->ShipDoors[Door].bCargoRelock)))
            {
                const auto* Link = USCShipLinkSubsystem::Get(Vehicle);
                if (!Link || Link->RoleOf(R.Requester, Vehicle) != ESCShipRole::Owner)
                {
                    Result.Refusal = TEXT("Hold locked by owner");
                    continue;
                }
            }
            if (!Vehicle->IsDoorOpen(Door) && !Vehicle->PlayDoor(Door, true, 0.f, 0, R.Requester, {}, !R.Requester.IsNone()))
                continue;
        }
        else if (!Vehicle->PlayClipAt(BestClip, Rig, 1.f)) continue;
        FSCAccessMove& Move = Result.Moves.AddDefaulted_GetRef();
        Move.Rig = Rig;
        Move.Clip = BestClip;
        Move.ClipId = Clips[BestClip].Id;
        Move.BlockedBefore = Before.Blocked;
        Move.BlockedAfter = Best.Blocked;
        Move.bDoor = bDoorRig;
        Move.Door = Door;
        Move.Seconds = Clips[BestClip].Fps > 0 ? Clips[BestClip].LastFrame / Clips[BestClip].Fps : 0.f;
        Result.Seconds = FMath::Max(Result.Seconds, Move.Seconds);
    }
    UE_LOG(LogSCAccess, Display, TEXT("ENV_ACCESS key=%s rigs_tested=%d moved=%d seconds=%.1f volume_m=%s reach_m=%.2f"), *Key.ToString(),
           Result.RigsTested, Result.Moves.Num(), Result.Seconds, *(R.Volume.GetSize() / 100.0).ToCompactString(), R.Reach / 100.0);
    return Result;
}
} // namespace

FSCAccessResult FSCEnvironmentAccess::Open(ASCVehicle* Vehicle, FName Key, const FTransform& Frame, const FBox& Volume, double ReachCm,
                                           double ToleranceCm, double FloorBandCm, double MinGainM3, FName Requester)
{
    if (!Vehicle || !Volume.IsValid) return {};
    FRequest R;
    R.Frame = Frame.GetRelativeTransform(Vehicle->GetActorTransform());
    R.Volume = Volume;
    R.Reach = ReachCm;
    R.Tolerance = ToleranceCm;
    R.Band = FloorBandCm;
    R.MinGain = MinGainM3;
    R.Requester = Requester;
    Requests().FindOrAdd(Vehicle).Add(Key, R);
    return Apply(Vehicle, Key, R);
}

FName FSCEnvironmentAccess::GridKey(const USCCargoGridComponent* Grid)
{
    return Grid ? FName(*FString::Printf(TEXT("grid:%d"), Grid->GridId)) : NAME_None;
}

FSCAccessResult FSCEnvironmentAccess::OpenGrid(const USCCargoGridComponent* Grid, FName Requester)
{
    ASCVehicle* Vehicle = Grid ? Grid->OwnerVehicle.Get() : nullptr;
    if (!Vehicle) return {};
    const FSCCargoLattice& L = Grid->Lattice;
    const FVector MaxItem = L.MaxItemM.GetMax() > 0 ? L.MaxItemM : L.InteriorM; // a zero limit is no limit: the interior
    return Open(Vehicle, GridKey(Grid), Grid->LatticeToWorld(), FBox(FVector::ZeroVector, L.InteriorM * 100.0), MaxItem.GetMax() * 100.0,
                Grid->Record.Container.GridCellSize * 100.0, L.CellM.Z * 100.0, L.CellM.X * L.CellM.Y * L.CellM.Z, Requester);
}

void FSCEnvironmentAccess::Release(ASCVehicle* Vehicle, FName Key)
{
    if (TMap<FName, FRequest>* Map = Requests().Find(Vehicle)) Map->Remove(Key);
}

int32 FSCEnvironmentAccess::Reapply(ASCVehicle* Vehicle)
{
    for (auto It = Requests().CreateIterator(); It; ++It)
        if (!It->Key.IsValid()) It.RemoveCurrent(); // vehicles gone
    const TMap<FName, FRequest>* Map = Requests().Find(Vehicle);
    int32 Moved = 0;
    if (Map)
        for (const auto& [Key, R] : *Map) Moved += Apply(Vehicle, Key, R).Moves.Num();
    return Moved;
}

int32 FSCEnvironmentAccess::NumRequests(const ASCVehicle* Vehicle)
{
    const TMap<FName, FRequest>* Map = Requests().Find(const_cast<ASCVehicle*>(Vehicle));
    return Map ? Map->Num() : 0;
}

namespace
{
FAutoConsoleCommandWithWorldAndArgs GSCEnvOpenAccess(TEXT("sc.Env.OpenAccess"),
    TEXT("sc.Env.OpenAccess [release]: open the way into every inventory-container grid of the environment (doors, gates and "
         "platforms take the end pose that clears the grid's access corridors); 'release' forgets the requests"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World);
        ASCVehicle* Environment = Viewer ? Viewer->GetEnvironment() : nullptr;
        if (!Environment)
        {
            UE_LOG(LogSCAccess, Warning, TEXT("ENV_ACCESS no environment"));
            return;
        }
        const bool bRelease = Args.IsValidIndex(0) && Args[0].Equals(TEXT("release"), ESearchCase::IgnoreCase);
        int32 Grids = 0, Moved = 0;
        Environment->ForEachComponent<USCCargoGridComponent>(false, [&](USCCargoGridComponent* Grid) {
            ++Grids;
            if (bRelease) FSCEnvironmentAccess::Release(Environment, FSCEnvironmentAccess::GridKey(Grid));
            else Moved += FSCEnvironmentAccess::OpenGrid(Grid).Moves.Num();
        });
        UE_LOG(LogSCAccess, Display, TEXT("ENV_ACCESS_CMD grids=%d %s=%d rigs=%d"), Grids, bRelease ? TEXT("released") : TEXT("moved"),
               bRelease ? Grids : Moved, Environment->Rigs.Num());
    }));

// Verification: what stops an item leaving each environment grid sideways, now. From the lattice centre at a quarter and
// three quarters of its height, along +-X / +-Y to one reach past the face: the first blocking hit for the player
// (Pawn channel: the game's colliders) and for physics bodies and traces (Visibility: the render collision).
FAutoConsoleCommandWithWorld GSCEnvProbeAccess(TEXT("sc.Env.ProbeAccess"),
    TEXT("sc.Env.ProbeAccess: log what blocks each environment grid's sides (ENV_ACCESS_PROBE)"),
    FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) {
        USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World);
        ASCVehicle* Environment = Viewer ? Viewer->GetEnvironment() : nullptr;
        if (!Environment || !World) return;
        Environment->ForEachComponent<USCCargoGridComponent>(false, [&](USCCargoGridComponent* Grid) {
            const FTransform L = Grid->LatticeToWorld();
            const FVector Size = Grid->Lattice.InteriorM * 100.0;
            const double Reach = (Grid->Lattice.MaxItemM.GetMax() > 0 ? Grid->Lattice.MaxItemM : Grid->Lattice.InteriorM).GetMax() * 100.0;
            static const TCHAR* Names[] = {TEXT("+X"), TEXT("-X"), TEXT("+Y"), TEXT("-Y")};
            const FVector Dirs[] = {FVector::XAxisVector, -FVector::XAxisVector, FVector::YAxisVector, -FVector::YAxisVector};
            for (const double Height : {0.25, 0.75})
                for (int32 D = 0; D < 4; ++D)
                {
                    const FVector Centre(Size.X / 2, Size.Y / 2, Size.Z * Height);
                    const double Face = FMath::Abs(FVector::DotProduct(Dirs[D], Size / 2));
                    const FVector From = L.TransformPosition(Centre), To = L.TransformPosition(Centre + Dirs[D] * (Face + Reach));
                    FString Line;
                    for (const ECollisionChannel Channel : {ECC_Pawn, ECC_Visibility})
                    {
                        FHitResult Hit;
                        FCollisionQueryParams Q(SCENE_QUERY_STAT(SCEnvProbeAccess), true);
                        for (TActorIterator<AActor> It(World); It; ++It)
                            if (*It != Environment) Q.AddIgnoredActor(*It); // the environment alone: not boxes, drones, the ship
                        const bool bHit = World->LineTraceSingleByChannel(Hit, From, To, Channel, Q);
                        bool bRig = false;
                        for (const USceneComponent* C = Hit.GetComponent(); C && !bRig; C = C->GetAttachParent()) bRig = C->IsA<USCRigComponent>();
                        const UPrimitiveComponent* C = Hit.GetComponent();
                        Line += FString::Printf(TEXT(" %s=%s"), Channel == ECC_Pawn ? TEXT("pawn") : TEXT("vis"),
                                                !bHit ? TEXT("clear")
                                                      : *FString::Printf(TEXT("%.2fm_past_face(%s%s)"), (Hit.Distance - Face) / 100.0,
                                                                         C && C->ComponentTags.Num() ? *FPaths::GetBaseFilename(C->ComponentTags[0].ToString()) : *GetNameSafe(C),
                                                                         bRig ? TEXT(",moving") : TEXT("")));
                    }
                    UE_LOG(LogSCAccess, Display, TEXT("ENV_ACCESS_PROBE grid=%d side=%s height=%.2f%s"), Grid->GridId, Names[D], Height, *Line);
                }
        });
    }));
} // namespace
