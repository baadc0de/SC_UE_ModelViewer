// FSCEnvironment: presented pose, floor placement and static rig baking for environments.
#include "SCEnvironment.h"
#include "SCClipCatalog.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "Components/InstancedStaticMeshComponent.h"
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

double FSCEnvironment::PlaceOnFloor(ASCVehicle* Environment, double FloorZ, const AActor* Ignore)
{
    UWorld* World = Environment ? Environment->GetWorld() : nullptr;
    if (!World) return 0;
    // The ship floor is the first large (>= 20 m across, the ship elevator), upward-facing surface below a point 2 m under
    // the origin: a top-loading modern hangar's origin is on its roof hatch, with the room and the elevator below it.
    // Front-loading and utility hangars put the origin on a tiled floor: when nothing lies below, the first upward floor
    // tile (>= 2 m across) under a point 2 m above the origin is the floor.
    constexpr double ElevatorSpan = 2000.0, TileSpan = 200.0;
    const FBox Bounds = Environment->GetComponentsBoundingBox(true);
    const FVector Origin = Environment->GetActorLocation();
    auto Probe = [&](double From, double MinFloorSpan) {
        FCollisionQueryParams Query(SCENE_QUERY_STAT(SCEnvFloor), true);
        if (Ignore) Query.AddIgnoredActor(Ignore);
        for (int32 Step = 0; Step < 64 && From > Bounds.Min.Z; ++Step)
        {
            FHitResult Hit;
            if (!World->LineTraceSingleByChannel(Hit, FVector(Origin.X, Origin.Y, From), FVector(Origin.X, Origin.Y, Bounds.Min.Z - 100), ECC_Visibility, Query))
                break;
            const FVector Size = HitPartBox(Hit).GetSize();
            if (Hit.ImpactNormal.Z > 0.9 && FMath::Min(Size.X, Size.Y) >= MinFloorSpan) return Hit.ImpactPoint.Z;
            if (Hit.GetComponent()) Query.AddIgnoredComponent(Hit.GetComponent());
            From = Hit.ImpactPoint.Z;
        }
        return TNumericLimits<double>::Max();
    };
    double Found = Probe(Origin.Z - 200.0, ElevatorSpan);
    if (Found == TNumericLimits<double>::Max()) Found = Probe(Origin.Z + 200.0, TileSpan);
    const double Offset = Found < TNumericLimits<double>::Max() ? FloorZ - Found : 0.0;
    if (Offset != 0) Environment->AddActorWorldOffset(FVector(0, 0, Offset));
    UE_LOG(LogSCEnvironment, Display, TEXT("ENVIRONMENT_PLACE floor_z=%.1f offset_z=%.1f"), Found < TNumericLimits<double>::Max() ? Found : 0., Offset);
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
