// Measure both collision alternatives on the actual deployed door geometry; never retained by the viewer.
#include "SCFirstPersonCollisionComparison.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"

bool SCCompareDoorCollision(ASCVehicle* Vehicle, USCRigComponent* Rig, FName Joint)
{
    const double Start = FPlatformTime::Seconds();
    auto* Asset = NewObject<UPhysicsAsset>(Vehicle, NAME_None, RF_Transient);
    TMap<FName, TArray<FVector>> BoneVertices;
    TArray<UStaticMeshComponent*> Surfaces;
    FBox Bounds(ForceInit);
    TInlineComponentArray<UStaticMeshComponent*> All(Vehicle);
    bool ExactSurfaceHit = false;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(SCP1Compare),false);
    for (auto* C : All)
    {
        if (C->GetAttachParent() != Rig || !C->IsCollisionEnabled() || !C->GetStaticMesh()->GetRenderData()) continue;
        const auto& Skeleton = Rig->GetSkeletalMeshAsset()->GetRefSkeleton();
        const int32 BoneIndex = Skeleton.FindBoneIndex(C->GetAttachSocketName());
        const int32 JointIndex = Skeleton.FindBoneIndex(Joint);
        // A child controller owns the complete door rig, including the panel and linkages.
        // Root fragments only own their driven subtree (e.g. a canopy on the hull).
        if (Rig->OwnerEntity == Vehicle->Manifest.Root.EntityName &&
            BoneIndex != JointIndex && !Skeleton.BoneIsChildOf(BoneIndex,JointIndex)) continue;
        Surfaces.Add(C);
        Bounds += C->Bounds.GetBox();
        const auto& LOD = C->GetStaticMesh()->GetRenderData()->LODResources[0];
        const FName Bone = C->GetAttachSocketName();
        const FTransform ToBone = C->GetComponentTransform().GetRelativeTransform(Rig->GetSocketTransform(Bone));
        for (uint32 I=0; I<(uint32)LOD.GetNumVertices(); ++I)
            BoneVertices.FindOrAdd(Bone).Add(ToBone.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(I))));
        const auto Indices = LOD.IndexBuffer.GetArrayView();
        for (int32 I=0; I+2<Indices.Num() && !ExactSurfaceHit; I+=3)
        {
            FVector V[3];
            for (int32 K=0; K<3; ++K) V[K] = C->GetComponentTransform().TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Indices[I+K])));
            const FVector N = FVector::CrossProduct(V[1]-V[0],V[2]-V[0]).GetSafeNormal();
            if (N.IsNearlyZero()) continue;
            const FVector P = (V[0]+V[1]+V[2])/3;
            FHitResult Hit;
            ExactSurfaceHit = C->LineTraceComponent(Hit,P+N*10,P-N*10,Query) && FVector::Dist(Hit.ImpactPoint,P)<1;
        }
    }
    if (Surfaces.IsEmpty()) return true; // a soft door is covered by the skinned-surface test
    for (const auto& Pair : BoneVertices)
    {
        auto* Body = NewObject<USkeletalBodySetup>(Asset);
        Body->BoneName = Pair.Key;
        Body->PhysicsType = PhysType_Kinematic;
        auto& Convex = Body->AggGeom.ConvexElems.AddDefaulted_GetRef();
        Convex.VertexData = Pair.Value;
        Convex.UpdateElemBox();
        Body->CreatePhysicsMeshes();
        Asset->SkeletalBodySetups.Add(Body);
    }
    Asset->UpdateBodySetupIndexMap();
    Asset->UpdateBoundsBodiesArray();
    auto* ConvexRig = NewObject<USCRigComponent>(Vehicle, NAME_None, RF_Transient);
    Vehicle->AddInstanceComponent(ConvexRig);
    ConvexRig->SetSkeletalMesh(Rig->GetSkeletalMeshAsset());
    ConvexRig->SetPhysicsAsset(Asset);
    ConvexRig->SetCollisionProfileName(TEXT("BlockAll"));
    ConvexRig->SetWorldTransform(Rig->GetComponentTransform());
    ConvexRig->RegisterComponent();
    ConvexRig->LocalPose = Rig->LocalPose;
    ConvexRig->RefreshBoneTransforms();
    ConvexRig->UpdateKinematicBonesToAnim(ConvexRig->GetComponentSpaceTransforms(),ETeleportType::TeleportPhysics,false);
    const double CookSeconds = FPlatformTime::Seconds()-Start;
    int32 ExactHits = 0, ConvexHits = 0, Differences = 0;
    const double QueryStart = FPlatformTime::Seconds();
    // A uniform grid samples openings and surface height, not just a body-exists flag.
    for (int32 X=0; X<21; ++X)
        for (int32 Y=0; Y<21; ++Y)
        {
            const FVector A(FMath::Lerp(Bounds.Min.X,Bounds.Max.X,(X+.5)/21),FMath::Lerp(Bounds.Min.Y,Bounds.Max.Y,(Y+.5)/21),Bounds.Max.Z+10);
            const FVector B(A.X,A.Y,Bounds.Min.Z-10);
            FHitResult ConvexHit, ExactHit;
            bool Exact = false;
            for (auto* C : Surfaces)
            {
                FHitResult Hit;
                if (C->LineTraceComponent(Hit,A,B,Query) && (!Exact || Hit.Distance<ExactHit.Distance)) { Exact=true; ExactHit=Hit; }
            }
            const bool Convex = ConvexRig->LineTraceComponent(ConvexHit,A,B,Query);
            ExactHits += Exact;
            ConvexHits += Convex;
            Differences += Exact != Convex || (Exact && Convex && FVector::Dist(ExactHit.ImpactPoint,ConvexHit.ImpactPoint)>1);
        }
    UE_LOG(LogTemp,Display,TEXT("P1_COLLISION_COMPARE owner=%s joint=%s exact_surface=%d bodies=%d convex_cook_s=%.6f queries_s=%.6f exact_hits=%d convex_hits=%d differences=%d"),
        *Rig->OwnerEntity,*Joint.ToString(),ExactSurfaceHit,BoneVertices.Num(),CookSeconds,FPlatformTime::Seconds()-QueryStart,ExactHits,ConvexHits,Differences);
    ConvexRig->DestroyComponent();
    return ExactSurfaceHit;
}
#endif
