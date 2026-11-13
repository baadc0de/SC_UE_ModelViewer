// Vehicle parameters and on-demand evaluation of sparse, ordered animation layers.
#include "SCVehicle.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "SCMaterialBuilder.h"
#include "SCAnimImporter.h"
#include "SCLightImporter.h"
#include "SCRigComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Components/StaticMeshComponent.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "PhysicsEngine/BodySetup.h"
#include "SCMeshImporter.h"
#include "Engine/SkeletalMesh.h"
ASCVehicle::ASCVehicle()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("VehicleRoot"));
    RootComponent->SetMobility(EComponentMobility::Movable);
}
bool ASCVehicle::PortRelative(const FString& Id, const FString& Helper, const FMatrix& Offset, FTransform& Out) const
{
    const FSCInstanceHelpers* Helpers = InstanceHelpers.Find(Id);
    const FTransform* Frame = Helpers ? Helpers->Frames.Find(Helper) : nullptr;
    if (!Frame) return false;
    // The helper (anchor space) lands on the joint frame plus the port offset; offset axes are the joint's own.
    Out = Frame->Inverse() * FTransform(FSCMeshImporter::Transform(Offset));
    return true;
}
void ASCVehicle::SetEngineGlow(float Strength)
{
    const float Value = FSCMaterialBuilder::EngineGlowToEmissive(Manifest.EngineGlow, Strength);
    for (UMaterialInstanceDynamic* MID : EngineGlowMaterials)
        if (MID)
        {
            MID->SetScalarParameterValue(TEXT("EmissiveStrength"), Value);
            float ReadBack = -1.f;
            MID->GetScalarParameterValue(FName(TEXT("EmissiveStrength")), ReadBack);
            UE_LOG(LogTemp, Display, TEXT("ENGINE_GLOW control=%g emissive=%g instance=%s parent=%s readback=%g"), Strength, Value,
                   *MID->GetName(), MID->Parent ? *MID->Parent->GetName() : TEXT("none"), ReadBack);
        }
}

void ASCVehicle::MarkDirty(int32 ClipIndex)
{
    for (const auto& Binding : ClipCatalog->Clips[ClipIndex].Bindings)
        DirtyMeshes.Add(Binding.Mesh);
}
bool ASCVehicle::StartClip(int32 Index, float Speed, bool bSnap, bool bLast, const TArray<FName>& Scopes, USCRigComponent* Target)
{
    if (!ClipCatalog || !ClipCatalog->Clips.IsValidIndex(Index) || !FMath::IsFinite(Speed)) return false;
    const auto& Clip = ClipCatalog->Clips[Index];
    if (Clip.Bindings.IsEmpty()) return false;
    Playback.RemoveAll([&](const FSCClipPlayback& P) { return P.Clip == Index && P.TargetRig.Get() == Target; });
    HeldPlayback.RemoveAll([&](const FSCClipPlayback& P) { return P.Clip == Index && P.TargetRig.Get() == Target; });
    FSCClipPlayback P;
    P.Clip = Index;
    P.TargetRig = Target;
    P.Speed = Speed;
    P.Frame = (bSnap ? bLast : Speed < 0) ? Clip.LastFrame : 0;
    P.bPlaying = !bSnap && Speed != 0 && Clip.LastFrame > 0;
    P.Order = ++NextOrder;
    P.Scopes = Scopes;
    (P.bPlaying ? Playback : HeldPlayback).Add(MoveTemp(P));
    MarkDirty(Index);
    EvaluateClips();
    SetActorTickEnabled(!Playback.IsEmpty());
    return true;
}
bool ASCVehicle::PlayClip(FName Id)
{
    if (!ClipCatalog) return false;
    return StartClip(ClipCatalog->Clips.IndexOfByPredicate([&](const FSCImportedClip& C) { return C.Id == Id; }), 1, false, false);
}
bool ASCVehicle::SnapClip(FName Id, bool bLast)
{
    if (!ClipCatalog) return false;
    return StartClip(ClipCatalog->Clips.IndexOfByPredicate([&](const FSCImportedClip& C) { return C.Id == Id; }), 1, true, bLast);
}
bool ASCVehicle::PlayFragment(FName Fragment, FName Tag)
{
    return PlayFragmentAt(Fragment, Tag, nullptr);
}
bool ASCVehicle::PlayFragmentAt(FName Fragment, FName Tag, USCRigComponent* Target)
{
    if (!ClipCatalog) return false;
    struct FOption
    {
        FString Key;
        float Weight = 1;
        TArray<FName> Scopes;
        TMap<int32, float> Clips;
    };
    TArray<FOption> Options;
    const uint64 OrderBefore = NextOrder;
    for (int32 Index = 0; Index < ClipCatalog->Clips.Num(); ++Index)
    {
        const auto& C = ClipCatalog->Clips[Index];
        if (C.Bindings.IsEmpty() || (Target && !C.Bindings.ContainsByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Target->GetSkeletalMeshAsset(); }))) continue;
        for (const auto& F : C.Fragments)
        {
            if (F.Fragment != Fragment || F.OptionWeight <= 0 || (!Tag.IsNone() && !F.Tags.Contains(Tag) && !F.FragmentTags.Contains(Tag)))
                continue;
            // Aliases keep metadata, but play the canonical animation only once.
            if (!F.AnimationNames.Contains(C.Id) && F.AnimationNames.ContainsByPredicate([&](FName Id) {
                    return ClipCatalog->Clips.ContainsByPredicate([&](const FSCImportedClip& Other) { return Other.Id == Id; });
                }))
                continue;
            const FString Key = F.OptionId.IsEmpty() ? F.Json : F.OptionId;
            FOption* Option = Options.FindByPredicate([&](const FOption& O) { return O.Key == Key; });
            if (!Option)
            {
                FOption NewOption;
                NewOption.Key = Key;
                NewOption.Weight = F.OptionWeight;
                NewOption.Scopes = F.Scopes;
                Option = &Options.Add_GetRef(MoveTemp(NewOption));
            }
            Option->Clips.Add(Index, F.Speed);
        }
    }
    // Weighted alternatives compete for scopes; disjoint scope groups can run together.
    // A reproducible actor-local random stream makes option selection testable.
    auto Conflicts = [](const TArray<FName>& A, const TArray<FName>& B) {
        return A.IsEmpty() || B.IsEmpty() || A.ContainsByPredicate([&](FName Scope) { return B.Contains(Scope); });
    };
    bool Found = false;
    while (!Options.IsEmpty())
    {
        Options.Sort([](const FOption& A, const FOption& B) { return A.Key < B.Key; });
        double Total = 0;
        for (const auto& O : Options)
            Total += O.Weight;
        double Draw = FragmentRandom.FRand() * Total;
        int32 Choice = Options.Num() - 1;
        for (int32 I = 0; I < Options.Num(); ++I)
        {
            Draw -= Options[I].Weight;
            if (Draw < 0)
            {
                Choice = I;
                break;
            }
        }
        const FOption Selected = Options[Choice];
        for (int32 I = Playback.Num() - 1; I >= 0; --I)
            if (Playback[I].TargetRig.Get() == Target && Conflicts(Selected.Scopes, Playback[I].Scopes))
            {
                Playback[I].bPlaying = false;
                HeldPlayback.Add(Playback[I]);
                Playback.RemoveAt(I);
            }
        TArray<int32> Indices;
        Selected.Clips.GetKeys(Indices);
        Indices.Sort();
        for (int32 Index : Indices)
            Found |= StartClip(Index, Selected.Clips[Index], false, false, Selected.Scopes, Target);
        Options.RemoveAll([&](const FOption& O) { return Conflicts(Selected.Scopes, O.Scopes); });
    }
    if (Found)
    {
        // Record the played state on every rig instance this call actually drove (entries started
        // above), so per-door state and the orbit toggle follow what is posed.
        if (!Target) FragmentTargets.Add({nullptr, Fragment}, Tag);
        auto Record = [&](const FSCClipPlayback& P) {
            if (P.Order <= OrderBefore) return;
            for (USCRigComponent* Rig : Rigs)
                if ((!P.TargetRig.IsValid() || P.TargetRig.Get() == Rig) &&
                    ClipCatalog->Clips[P.Clip].Bindings.ContainsByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Rig->GetSkeletalMeshAsset(); }))
                    FragmentTargets.Add({Rig, Fragment}, Tag);
        };
        for (const auto& P : Playback) Record(P);
        for (const auto& P : HeldPlayback) Record(P);
    }
    return Found;
}
bool ASCVehicle::IsFragmentOpen(FName Fragment, const USCRigComponent* Rig) const
{
    return FragmentTargets.FindRef({Rig, Fragment}) == FName(TEXT("Open"));
}
bool ASCVehicle::ToggleClip(FName Fragment, FName OpenTag, FName CloseTag)
{
    return PlayFragment(Fragment, FragmentTargets.FindRef({nullptr, Fragment}) == OpenTag ? CloseTag : OpenTag);
}
void ASCVehicle::ResetPose()
{
    Playback.Reset();
    HeldPlayback.Reset();
    DirtyMeshes.Reset();
    FragmentTargets.Reset();
    SetActorTickEnabled(false);
    for (USCRigComponent* Rig : Rigs)
        Rig->ResetPose();
}
void ASCVehicle::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!ClipCatalog || Playback.IsEmpty() || DeltaSeconds <= 0) return;
    for (int32 I = Playback.Num() - 1; I >= 0; --I)
    {
        auto& P = Playback[I];
        const auto& C = ClipCatalog->Clips[P.Clip];
        const double Previous = P.Frame;
        P.Frame = FMath::Clamp(P.Frame + DeltaSeconds * C.Fps * P.Speed, 0., double(C.LastFrame));
        if (P.Frame != Previous) MarkDirty(P.Clip);
        // A forward clip at frame zero has not finished; reverse playback ends at zero.
        if ((P.Speed > 0 && P.Frame >= C.LastFrame) || (P.Speed < 0 && P.Frame <= 0))
        {
            P.bPlaying = false;
            HeldPlayback.Add(P);
            Playback.RemoveAt(I);
        }
    }
    EvaluateClips();
    SetActorTickEnabled(!Playback.IsEmpty());
}
void ASCVehicle::EvaluateClips()
{
    if (DirtyMeshes.IsEmpty()) return;
    TArray<FSCClipPlayback> ClipLayers = HeldPlayback;
    ClipLayers.Append(Playback);
    ClipLayers.Sort([](const FSCClipPlayback& A, const FSCClipPlayback& B) { return A.Order < B.Order; });
    // Only affected rigs are rebuilt. Held layers preserve the last authored pose and overlap order.
    for (USCRigComponent* Rig : Rigs)
    {
        if (!DirtyMeshes.Contains(Rig->GetSkeletalMeshAsset())) continue;
        TArray<FTransform> Pose = Rig->GetSkeletalMeshAsset()->GetRefSkeleton().GetRefBonePose();
        for (const auto& P : ClipLayers)
            for (const auto& B : ClipCatalog->Clips[P.Clip].Bindings)
                if ((!P.TargetRig.IsValid() || P.TargetRig.Get() == Rig) && B.Mesh == Rig->GetSkeletalMeshAsset())
                    for (const auto& T : B.Tracks)
                    {
                        const FTransform Sample =
                            FSCAnimImporter::Sample(T, FSCAnimImporter::TrackFrame(ClipCatalog->Clips[P.Clip], T, P.Frame, P.Speed < 0));
                        if (T.bRotation) Pose[T.Bone].SetRotation(Sample.GetRotation());
                        if (T.bPosition) Pose[T.Bone].SetTranslation(Sample.GetTranslation());
                    }
        bool Changed = Pose.Num() != Rig->LocalPose.Num();
        for (int32 I = 0; !Changed && I < Pose.Num(); ++I)
            Changed |= !Pose[I].Equals(Rig->LocalPose[I], 1.e-8);
        if (!Changed) continue;
        Rig->LocalPose = MoveTemp(Pose);
        Rig->RefreshBoneTransforms();
        ++PoseEvaluations;
    }
    DirtyMeshes.Reset();
}

void ASCVehicle::RefreshLightShadowPolicy()
{
    FSCLightImporter::RefreshShadows(this);
}

bool ASCVehicle::IsInsideLightArea(const FVector& World) const
{
    for (const FSCLightArea& Area : LightAreas)
        if (Area.Contains(World)) return true;
    return false;
}
FString ASCVehicle::LightRoomAt(const FVector& World) const
{
    for (const FSCLightArea& Area : LightAreas)
        if (FString Room = Area.RoomAt(World); !Room.IsEmpty()) return Room;
    return FString();
}
int32 ASCVehicle::LightChannelAt(const FVector& World) const
{
    const FString Room = LightRoomAt(World);
    if (Room.IsEmpty()) return 0;
    const int32* Channel = LightRoomChannels.Find(Room);
    return Channel ? *Channel : 1; // before FSCLightAreas::Apply: the WP-U3b inside channel
}
bool ASCVehicle::IsInPhysicsGrid(const FVector& World) const
{
    if (PhysicsGridTriangles.Num() < 3) return false;
    const FVector P = GetActorTransform().InverseTransformPosition(World);
    if (!PhysicsGridBounds.IsInside(P)) return false;
    // Parity of crossings along a ray off every axis and diagonal (no grazing of axis-aligned edges); the ray ends
    // past the bounds, so every crossing is counted. Each shape is its own closed volume (overlapping shapes: inside
    // any), and hits at the same distance are one crossing (a ray through an edge shared by two triangles).
    const FVector End = P + FVector(1, 0.0013, 0.0007).GetSafeNormal() * (PhysicsGridBounds.GetSize().Size() * 2 + 100);
    int32 Begin = 0;
    TArray<double> Hits;
    for (const int32 ShapeEnd : PhysicsGridShapeEnds.IsEmpty() ? TArray<int32>{PhysicsGridTriangles.Num()} : PhysicsGridShapeEnds)
    {
        Hits.Reset();
        for (int32 T = Begin; T + 2 < FMath::Min(ShapeEnd, PhysicsGridTriangles.Num()); T += 3)
        {
            FVector Hit, Normal;
            if (FMath::SegmentTriangleIntersection(P, End, PhysicsGridTriangles[T], PhysicsGridTriangles[T + 1], PhysicsGridTriangles[T + 2], Hit, Normal))
                Hits.Add(FVector::Dist(P, Hit));
        }
        Begin = ShapeEnd;
        Hits.Sort();
        int32 Crossings = 0;
        for (int32 H = 0; H < Hits.Num(); ++H)
            if (H == 0 || Hits[H] - Hits[H - 1] > 1e-3) ++Crossings; // 10 um apart: the same surface point
        if (Crossings % 2 == 1) return true;
    }
    return false;
}
bool ASCVehicle::IsInPhysicsGrid(const FVector& Bottom, const FVector& Top) const
{
    if (PhysicsGridTriangles.Num() < 3) return false;
    if (IsInPhysicsGrid(Bottom)) return true;
    const FTransform& Actor = GetActorTransform();
    const FVector A = Actor.InverseTransformPosition(Bottom), B = Actor.InverseTransformPosition(Top);
    if (!PhysicsGridBounds.Intersect(FBox(FVector::Min(A, B), FVector::Max(A, B)))) return false;
    for (int32 T = 0; T + 2 < PhysicsGridTriangles.Num(); T += 3)
    {
        FVector Hit, Normal;
        if (FMath::SegmentTriangleIntersection(A, B, PhysicsGridTriangles[T], PhysicsGridTriangles[T + 1], PhysicsGridTriangles[T + 2], Hit, Normal))
            return true;
    }
    return false;
}

double ASCVehicle::LowestVisibleZ(FString* OutPart) const
{
    // Candidates by bounds (a rig's bounds are its reference pose, a rotated part's box reaches below its surface), then
    // the true lowest surface of every candidate near the bottom: a rig's CPU-skinned collision (current pose), else a
    // fine grid of vertical traces against the part's own collision; bounds only for parts without collision.
    TArray<const UPrimitiveComponent*> Parts;
    double BoundsLowest = TNumericLimits<double>::Max();
    ForEachComponent<UPrimitiveComponent>(false, [&](const UPrimitiveComponent* C) {
        if (!C->IsRegistered() || !C->IsVisible() || C->bHiddenInGame || C->ComponentHasTag(TEXT("SCGameCollision"))) return;
        // Only solid surfaces bear weight: parts the importer drew without shadows (glass, glow, decals, light beams) and
        // parts drawn entirely with translucent materials are skipped.
        if (!C->CastShadow) return;
        if (const auto* SM = Cast<UStaticMeshComponent>(C); SM && SM->GetStaticMesh() && SM->GetStaticMesh()->GetRenderData() &&
                                                            !SM->GetStaticMesh()->GetRenderData()->LODResources.IsEmpty())
        {
            bool bSolid = false;
            for (const FStaticMeshSection& Section : SM->GetStaticMesh()->GetRenderData()->LODResources[0].Sections)
                if (const UMaterialInterface* M = SM->GetMaterial(Section.MaterialIndex); M && !IsTranslucentBlendMode(*M))
                    bSolid = true;
            if (!bSolid) return;
        }
        Parts.Add(C);
        BoundsLowest = FMath::Min(BoundsLowest, C->Bounds.GetBox().Min.Z);
    });
    constexpr double CandidateBandCm = 150.0; // a part whose box starts this close to the lowest box may hold the lowest point
    double Lowest = TNumericLimits<double>::Max();
    auto TakeAt = [&](const FVector& P, const UPrimitiveComponent* C) {
        if (P.Z >= Lowest) return;
        Lowest = P.Z;
        if (OutPart)
            *OutPart = FString::Printf(TEXT("%s at %s socket=%s"), C->ComponentTags.Num() ? *C->ComponentTags[0].ToString() : *C->GetName(),
                                       *P.ToCompactString(), *C->GetAttachSocketName().ToString());
    };
    auto Take = [&](double Z, const UPrimitiveComponent* C) { TakeAt(FVector(C->Bounds.Origin.X, C->Bounds.Origin.Y, Z), C); };
    TArray<FVector3f> Skinned;
    for (const UPrimitiveComponent* C : Parts)
    {
        const FBox Box = C->Bounds.GetBox();
        if (Box.Min.Z > BoundsLowest + CandidateBandCm) continue;
        if (const auto* Rig = Cast<USCRigComponent>(C); Rig && Rig->ReadCollisionVertices(Skinned) && !Skinned.IsEmpty())
        {
            const FTransform& World = Rig->GetComponentTransform();
            for (const FVector3f& V : Skinned) TakeAt(World.TransformPosition(FVector(V)), C);
            continue;
        }
        // The part's collision triangles (local space) placed by its current transform: exact, and independent of when the
        // physics scene catches up with a pose snapped this frame.
        bool bVertices = false;
        if (UBodySetup* Body = const_cast<UPrimitiveComponent*>(C)->GetBodySetup())
        {
            const FTransform& World = C->GetComponentTransform();
            for (const Chaos::FTriangleMeshImplicitObjectPtr& Mesh : Body->TriMeshGeometries)
            {
                if (!Mesh) continue;
                const auto& Particles = Mesh->Particles();
                for (uint32 I = 0; I < Particles.Size(); ++I) TakeAt(World.TransformPosition(FVector(Particles.GetX(I))), C);
                bVertices |= Particles.Size() > 0;
            }
        }
        if (!bVertices) Take(Box.Min.Z, C);
    }
    return Lowest;
}
int32 ASCVehicle::RestOnLandingGear()
{
    if (!ClipCatalog) return 0;
    int32 Posed = 0;
    for (int32 Index = 0; Index < ClipCatalog->Clips.Num(); ++Index)
    {
        const FSCImportedClip& C = ClipCatalog->Clips[Index];
        if (!C.Bindings.IsEmpty() && C.Fragments.ContainsByPredicate([](const auto& F) {
                return F.Fragment == FName(TEXT("Landing_Gear")) && F.FragmentTags.Contains(FName(TEXT("Compress")));
            }))
            Posed += StartClip(Index, 1, true, true) ? 1 : 0;
    }
    return Posed;
}
