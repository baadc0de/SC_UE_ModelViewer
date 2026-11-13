// FSCEnvironment: presented pose, floor placement and static rig baking for environments.
#include "SCEnvironment.h"
#include "SCClipCatalog.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Rendering/SkeletalMeshRenderData.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCEnvironment, Log, All);

namespace
{
// The bounds of the part a trace hit: the hit instance of an instanced component (its whole box spans every instance).
FBox HitPartBox(const FHitResult& Hit)
{
    const UPrimitiveComponent* C = Hit.GetComponent();
    if (!C) return FBox(ForceInit);
    FTransform Instance;
    if (const auto* ISM = Cast<UInstancedStaticMeshComponent>(C);
        ISM && ISM->GetStaticMesh() && Hit.Item != INDEX_NONE && ISM->GetInstanceTransform(Hit.Item, Instance, true))
        return ISM->GetStaticMesh()->GetBoundingBox().TransformBy(Instance);
    return C->Bounds.GetBox();
}
} // namespace

int32 FSCEnvironment::SetState(ASCVehicle* Environment, const FString& State)
{
    if (!Environment || !Environment->ClipCatalog) return 0;
    for (USCRigComponent* Rig : Environment->Rigs)
        if (Rig) Rig->ResetPose();
    const FString Lower = State.ToLower();
    if (Lower != TEXT("open") && Lower != TEXT("closed")) { UE_LOG(LogSCEnvironment, Display, TEXT("ENVIRONMENT_STATE bind")); return 0; }
    const bool bOpen = Lower == TEXT("open");
    TMap<FString, TPair<int32, FName>> Best;
    for (const FSCImportedClip& Clip : Environment->ClipCatalog->Clips)
    {
        FString Owner, Fragment;
        if (!Clip.Id.ToString().Split(TEXT("__"), &Owner, &Fragment)) continue;
        Fragment = Fragment.ToLower().Replace(TEXT("door_"), TEXT(""));
        const int32 Rank = Fragment == (bOpen ? TEXT("opened") : TEXT("closed")) ? 2 : Fragment == (bOpen ? TEXT("open") : TEXT("close")) ? 1 : 0;
        if (Rank && Rank > Best.FindRef(Owner).Key) Best.Add(Owner, {Rank, Clip.Id});
    }
    int32 Snapped = 0;
    for (const auto& [Owner, Pick] : Best) Snapped += Environment->SnapClip(Pick.Value, true);
    UE_LOG(LogSCEnvironment, Display, TEXT("ENVIRONMENT_STATE %s parts=%d"), *Lower, Snapped);
    return Snapped;
}

FVector FSCEnvironment::PlaceOnFloor(ASCVehicle* Environment, double FloorZ, const AActor* Ignore)
{
    UWorld* World = Environment ? Environment->GetWorld() : nullptr;
    if (!World) return FVector::ZeroVector;
    // The ship stands on the environment's ship elevator: its largest flat moving part (attached under a rig, >= 20 m
    // across, <= 15 m thick) in the presented pose: of the moving parts at least half the largest one's area (the elevator
    // and the shaft cover doors around it, not roof-hatch leaves or cargo pads) the highest, the platform the ship stands on. Static ceilings and roof frames are larger
    // but never move. Hangars place their origin differently (modern top-loaders on the roof hatch, front-loaders and
    // utility hangars on the floor or at a corner), so the elevator, not the origin, decides where the ship goes.
    constexpr double MinSpan = 2000.0, MaxThickness = 1500.0, SameSize = 0.5;
    struct FCandidate { FBox Box; double Area; FString Name; };
    TArray<FCandidate> Candidates;
    Environment->ForEachComponent<UStaticMeshComponent>(false, [&](UStaticMeshComponent* C) {
        if (C->IsA<UInstancedStaticMeshComponent>() || !C->IsVisible() || C->bHiddenInGame || C->ComponentHasTag(TEXT("SCGameCollision")) ||
            C->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
            return;
        bool bMoving = false;
        for (const USceneComponent* Parent = C->GetAttachParent(); Parent && !bMoving; Parent = Parent->GetAttachParent())
            bMoving = Parent->IsA<USCRigComponent>();
        if (!bMoving) return;
        const FBox Box = C->Bounds.GetBox();
        const FVector Size = Box.GetSize();
        if (FMath::Min(Size.X, Size.Y) >= MinSpan && Size.Z <= MaxThickness)
            Candidates.Add({Box, Size.X * Size.Y, C->ComponentTags.Num() ? FPaths::GetBaseFilename(C->ComponentTags[0].ToString()) : C->GetName()});
    });
    double Largest = 0;
    for (const FCandidate& Candidate : Candidates)
    {
        UE_LOG(LogSCEnvironment, Display, TEXT("ENVIRONMENT_FLOOR_CANDIDATE %s area_m2=%.0f top_z=%.1f centre=%s"), *Candidate.Name,
               Candidate.Area / 1.0e4, Candidate.Box.Max.Z, *FVector2D(Candidate.Box.GetCenter()).ToString());
        Largest = FMath::Max(Largest, Candidate.Area);
    }
    const FCandidate* Best = nullptr;
    for (const FCandidate& Candidate : Candidates)
        if (Candidate.Area >= Largest * SameSize && (!Best || Candidate.Box.Max.Z > Best->Box.Max.Z)) Best = &Candidate;
    FVector Offset = FVector::ZeroVector;
    if (Best)
    {
        Offset = FVector(-Best->Box.GetCenter().X, -Best->Box.GetCenter().Y, FloorZ - Best->Box.Max.Z);
        UE_LOG(LogSCEnvironment, Display, TEXT("ENVIRONMENT_PLACE elevator=%s floor_z=%.1f offset=%s"), *Best->Name, Best->Box.Max.Z, *Offset.ToString());
    }
    else
    {
        // No elevator-sized part: the first upward floor surface (>= 2 m across) under a point 2 m above the origin.
        FCollisionQueryParams Query(SCENE_QUERY_STAT(SCEnvFloor), true);
        if (Ignore) Query.AddIgnoredActor(Ignore);
        const FBox Bounds = Environment->GetComponentsBoundingBox(true);
        const FVector Origin = Environment->GetActorLocation();
        double From = Origin.Z + 200.0;
        for (int32 Step = 0; Step < 64 && From > Bounds.Min.Z; ++Step)
        {
            FHitResult Hit;
            if (!World->LineTraceSingleByChannel(Hit, FVector(Origin.X, Origin.Y, From), FVector(Origin.X, Origin.Y, Bounds.Min.Z - 100), ECC_Visibility, Query))
                break;
            if (Hit.ImpactNormal.Z > 0.9 && FMath::Min(HitPartBox(Hit).GetSize().X, HitPartBox(Hit).GetSize().Y) >= 200.0)
            {
                Offset.Z = FloorZ - Hit.ImpactPoint.Z;
                break;
            }
            if (Hit.GetComponent()) Query.AddIgnoredComponent(Hit.GetComponent());
            From = Hit.ImpactPoint.Z;
        }
        UE_LOG(LogSCEnvironment, Display, TEXT("ENVIRONMENT_PLACE elevator=none offset=%s"), *Offset.ToString());
    }
    if (!Offset.IsZero()) Environment->AddActorWorldOffset(Offset);
    return Offset;
}

FSCRigBakeStats FSCEnvironment::BakeRigsStatic(ASCVehicle* Environment)
{
    FSCRigBakeStats Stats;
    if (!Environment || !Environment->GetRootComponent()) return Stats;
    USceneComponent* Root = Environment->GetRootComponent();
    for (USCRigComponent* Rig : Environment->Rigs)
    {
        if (!Rig) continue;
        // Everything attached to the rig (bone-socket parts, helper frames with their lights and parts) keeps its posed
        // world transform under the actor root.
        TArray<USceneComponent*> Children = Rig->GetAttachChildren();
        for (USceneComponent* Child : Children)
            if (Child)
            {
                Child->AttachToComponent(Root, FAttachmentTransformRules::KeepWorldTransform);
                ++Stats.Parts;
            }
        const USkeletalMesh* Mesh = Rig->GetSkeletalMeshAsset();
        const FSkeletalMeshRenderData* Render = Mesh ? Mesh->GetResourceForRendering() : nullptr;
        if (Render && !Render->LODRenderData.IsEmpty() && Render->LODRenderData[0].GetNumVertices() > 0) ++Stats.SkinnedDropped;
        Rig->DestroyComponent();
        ++Stats.Rigs;
    }
    Environment->Rigs.Empty();
    UE_LOG(LogSCEnvironment, Display, TEXT("ENVIRONMENT_BAKE rigs=%d parts=%d skinned_dropped=%d"), Stats.Rigs, Stats.Parts, Stats.SkinnedDropped);
    return Stats;
}
