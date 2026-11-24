// The mechanism of a cargo grid (WP-K1): which clips extend and retract it, and in which direction. Candidates are the
// clips animating a bone of the chain the grid hangs from, on each rig above it. Each is evaluated off-actor: the grid's
// frame with the rig posed at the clip's first and at its last frame (the bones the clip animates sampled, the rest as
// the rig stands). A clip that does not carry the grid is not its mechanism (an idle, a light). Per rig one clip is kept,
// the one that carries the grid furthest (a deploy and its mirrored retract are one mechanism). The open pose is the
// combination of the kept clips' ends (each first or last frame, rigs composed inner to outer: a plate unfolds on the hull
// strut that swings it out) in which the lattice's centre stands furthest from the owner's centre: an extended cargo
// plate is out of the hull. A clip judged alone can look closing (a plate unfolding turns its floor towards the hull
// while the strut carries it out), so the ends are chosen together. No clip names, no fragment families: the authored data
// has no direction signal for these clips (Hull A's are Chrparams clips without Mannequin fragments).
#include "Cargo/SCCargoGridComponent.h"
#include "SCCargoState.h"
#include "SCClipCatalog.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "Engine/SkeletalMesh.h"

namespace
{
constexpr double MinTravelCm = 1;       // a clip moving the grid less than this does not carry it
constexpr double MinTurnDegrees = 1;    // nor one turning it less than this

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

// The rig's local pose with the clip's tracks sampled at Frame over Current.
TArray<FTransform> PoseAt(const FSCImportedClip& Clip, const FSCClipBinding& Binding, const TArray<FTransform>& Current, double Frame)
{
    TArray<FTransform> Pose = Current;
    for (const FSCBoneTrack& T : Binding.Tracks)
    {
        if (!Pose.IsValidIndex(T.Bone)) continue;
        const FTransform Sample = FSCClipSampling::Sample(T, FSCClipSampling::TrackFrame(Clip, T, Frame, false));
        if (T.bRotation) Pose[T.Bone].SetRotation(Sample.GetRotation());
        if (T.bPosition) Pose[T.Bone].SetTranslation(Sample.GetTranslation());
    }
    return Pose;
}
} // namespace

bool FSCCargoMover::Classify(const FTransform& Start, const FTransform& End, const FVector& Centre, bool& bOutForwardOpens, double& OutTravelCm)
{
    OutTravelCm = FVector::Dist(Start.GetLocation(), End.GetLocation());
    const double Turn = FMath::RadiansToDegrees(Start.GetRotation().AngularDistance(End.GetRotation()));
    if (OutTravelCm < MinTravelCm && Turn < MinTurnDegrees) return false;
    bOutForwardOpens = FVector::DistSquared(End.GetLocation(), Centre) >= FVector::DistSquared(Start.GetLocation(), Centre);
    return true;
}

uint32 FSCCargoMover::OpenEnds(const FVector& Point, const TArray<FTransform>& FirstDeltas, const TArray<FTransform>& LastDeltas, const FVector& Centre)
{
    const int32 N = FMath::Min(FMath::Min(FirstDeltas.Num(), LastDeltas.Num()), 8);
    uint32 Best = 0;
    double BestDist = -1;
    for (uint32 Mask = 0; Mask < (1u << N); ++Mask)
    {
        FVector P = Point;
        for (int32 I = 0; I < N; ++I) P = ((Mask >> I) & 1 ? LastDeltas[I] : FirstDeltas[I]).TransformPosition(P);
        const double D = FVector::Dist(P, Centre);
        if (D > BestDist + MinTravelCm || (D > BestDist - MinTravelCm && FMath::CountBits(Mask) > FMath::CountBits(Best)))
        {
            Best = Mask;
            BestDist = FMath::Max(BestDist, D);
        }
    }
    return Best;
}

TArray<FSCCargoMover> SCCargoFindMovers(const ASCVehicle* Vehicle, const USceneComponent* Anchor, const FVector& GridPoint)
{
    TArray<FSCCargoMover> Out;
    if (!Vehicle || !Vehicle->ClipCatalog || !Anchor) return Out;
    const TArray<FSCImportedClip>& Clips = Vehicle->ClipCatalog->Clips;
    const FVector Centre = Vehicle->GetComponentsBoundingBox(false, true).GetCenter();
    const FTransform AnchorWorld = Anchor->GetComponentTransform();
    TArray<FTransform> First, Last;
    TArray<FName> Sockets;
    for (const USceneComponent* Child = Anchor; Child && Child->GetAttachParent(); Child = Child->GetAttachParent())
    {
        USCRigComponent* Rig = Cast<USCRigComponent>(Child->GetAttachParent());
        const USkeletalMesh* Mesh = Rig ? Rig->GetSkeletalMeshAsset() : nullptr;
        if (!Mesh || Child->GetAttachSocketName().IsNone()) continue;
        const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
        const int32 AttachBone = Ref.FindBoneIndex(Rig->GetSocketBoneName(Child->GetAttachSocketName()));
        if (AttachBone == INDEX_NONE) continue;
        TSet<int32> Chain;
        for (int32 Bone = AttachBone; Bone != INDEX_NONE; Bone = Ref.GetParentIndex(Bone)) Chain.Add(Bone);
        const TArray<FTransform> Current = Rig->LocalPose.Num() == Ref.GetNum() ? Rig->LocalPose : Ref.GetRefBonePose();
        const FTransform RigWorld = Rig->GetComponentTransform();
        const FTransform AttachNow = ComponentSpace(Ref, Current)[AttachBone] * RigWorld;
        // The attach bone's move in the world with the rig posed by Local (everything hung from it moves with it).
        auto DeltaAt = [&](const TArray<FTransform>& Local) { return AttachNow.Inverse() * (ComponentSpace(Ref, Local)[AttachBone] * RigWorld); };
        auto GridAt = [&](const TArray<FTransform>& Local) { return AnchorWorld * DeltaAt(Local); };
        FTransform BestFirst, BestLast;
        TOptional<FSCCargoMover> Best;
        for (int32 Index = 0; Index < Clips.Num(); ++Index)
        {
            const FSCImportedClip& Clip = Clips[Index];
            const FSCClipBinding* Binding = Clip.Bindings.FindByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Mesh; });
            if (Clip.LastFrame <= 0 || !Binding || !Binding->Tracks.ContainsByPredicate([&](const FSCBoneTrack& T) { return Chain.Contains(T.Bone); }))
                continue;
            FSCCargoMover M;
            if (!FSCCargoMover::Classify(GridAt(PoseAt(Clip, *Binding, Current, 0)), GridAt(PoseAt(Clip, *Binding, Current, Clip.LastFrame)), Centre,
                                         M.bForwardOpens, M.TravelCm))
            {
                UE_LOG(LogSCCargo, Display, TEXT("CARGO_MOVER_SKIP rig=%s clip=%s reason=\"does not carry the grid\""), *Rig->OwnerEntity, *Clip.Id.ToString());
                continue;
            }
            M.Rig = Rig;
            M.Clip = Index;
            M.ClipId = Clip.Id;
            M.Seconds = Clip.Fps > 0 ? Clip.LastFrame / Clip.Fps : 0.f;
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_MOVER_CANDIDATE rig=%s clip=%s travel_cm=%.0f alone_open_end=%s seconds=%.1f"), *Rig->OwnerEntity,
                   *Clip.Id.ToString(), M.TravelCm, M.bForwardOpens ? TEXT("last") : TEXT("first"), M.Seconds);
            // One mechanism per rig: the clip carrying the grid furthest (the first of equals: aliases of one clip).
            if (!Best || M.TravelCm > Best->TravelCm + MinTravelCm)
            {
                Best = M;
                BestFirst = DeltaAt(PoseAt(Clip, *Binding, Current, 0));
                BestLast = DeltaAt(PoseAt(Clip, *Binding, Current, Clip.LastFrame));
            }
        }
        if (!Best) continue;
        Out.Add(*Best);
        First.Add(BestFirst);
        Last.Add(BestLast);
        Sockets.Add(Child->GetAttachSocketName());
    }
    // The open pose: the ends together (inner to outer, the order found).
    const uint32 Ends = FSCCargoMover::OpenEnds(GridPoint, First, Last, Centre);
    for (int32 I = 0; I < Out.Num(); ++I)
    {
        FSCCargoMover& M = Out[I];
        M.bForwardOpens = ((Ends >> I) & 1) != 0;
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_MOVER rig=%s socket=%s clip=%s seconds=%.1f travel_cm=%.0f open_end=%s"), M.Rig.IsValid() ? *M.Rig->OwnerEntity : TEXT("?"),
               *Sockets[I].ToString(), *M.ClipId.ToString(), M.Seconds, M.TravelCm, M.bForwardOpens ? TEXT("last") : TEXT("first"));
    }
    return Out;
}
