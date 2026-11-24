// WP-G1: stop opening gangways where their moving collision surface meets supporting ground.
#include "SCGangwayGround.h"
#include "SCClipCatalog.h"
#include "SCVehicle.h"
#include "SCAnimImporter.h"
#include "SCRigComponent.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "PhysicsEngine/BodySetup.h"

void ASCVehicle::BuildGangwayProbes(int32 Index)
{
    const FSCDoorInteraction& Door = Doors[Index];
    FShipDoor& State = ShipDoors[Index];
    USCRigComponent* Rig = Door.Rig;
    if (!Rig || !Rig->GetSkeletalMeshAsset()) return;
    TArray<USceneComponent*> RigParts;
    Rig->GetChildrenComponents(true, RigParts);
    for (USceneComponent* Child : RigParts)
    {
        auto* Mesh = Cast<UStaticMeshComponent>(Child);
        if (!Mesh || !Mesh->IsQueryCollisionEnabled() || Mesh->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block) continue;
        const UBodySetup* Body = Mesh->GetBodySetup();
        if (!Body) continue;
        USceneComponent* Attachment = Mesh;
        while (Attachment->GetAttachParent() && !Attachment->GetAttachParent()->IsA<USCRigComponent>())
            Attachment = Attachment->GetAttachParent();
        if (Attachment->GetAttachParent() != Rig) continue;
        const FName Socket = Attachment->GetAttachSocketName();
        const int32 Bone = Rig->GetBoneIndex(Socket);
        if (Bone == INDEX_NONE) continue; // a rigid part fixed to the rig origin does not follow a door clip
        const FTransform ToBone = Mesh->GetComponentTransform().GetRelativeTransform(Rig->GetSocketTransform(Socket));
        FShipDoor::FGroundProbeSet Set;
        Set.Bone = Bone;
        TSet<FIntVector> Cells;
        auto Add = [&](FVector P) {
            P = ToBone.TransformPosition(P);
            // Keep measured surface points. A spatial hash coalesces duplicate triangle edges and tessellation.
            const FIntVector Key(FMath::RoundToInt(P.X / 2.), FMath::RoundToInt(P.Y / 2.), FMath::RoundToInt(P.Z / 2.));
            if (!Cells.Contains(Key)) { Cells.Add(Key); Set.Points.Add(P); }
        };
        for (const auto& Geometry : Body->TriMeshGeometries)
        {
            if (!Geometry) continue;
            for (int32 T = 0; T < Geometry->Elements().GetNumTriangles(); ++T)
            {
                Chaos::FTriangle Triangle;
                int32 V0, V1, V2;
                Geometry->GetTransformedTriangle(T, Chaos::FRigidTransform3::Identity, Triangle, V0, V1, V2);
                const FVector A(Triangle[0]), B(Triangle[1]), C(Triangle[2]);
                const double Span = FMath::Max3(FVector::Dist(A,B), FVector::Dist(B,C), FVector::Dist(C,A));
                const int32 N = FMath::Max(1, FMath::CeilToInt(Span / 35.));
                for (int32 I = 0; I <= N; ++I)
                    for (int32 J = 0; I + J <= N; ++J)
                        Add(A + (B-A) * (double(I) / N) + (C-A) * (double(J) / N));
            }
        }
        if (!Set.Points.IsEmpty()) State.GroundProbes.Add(MoveTemp(Set));
    }
}

void ASCVehicle::GangwayStops(float Seconds, TMap<int32, double>& OutFractions) const
{
    TSet<int32> Opening;
    for (const FSCClipPlayback& Layer : Playback)
        if (Layer.OpeningGangway != INDEX_NONE) Opening.Add(Layer.OpeningGangway);
    for (const int32 Index : Opening)
    {
        if (!ShipDoors.IsValidIndex(Index) || !Doors.IsValidIndex(Index)) continue;
        const FShipDoor& State = ShipDoors[Index];
        const USCRigComponent* Rig = Doors[Index].Rig;
        if (!Rig || !Rig->GetSkeletalMeshAsset() || State.GroundProbes.IsEmpty()) continue;
        const auto& Ref = Rig->GetSkeletalMeshAsset()->GetRefSkeleton();
        if (Rig->LocalPose.Num() != Ref.GetNum()) continue;
        struct FLayer { const FSCClipPlayback* Play; const FSCImportedClip* Clip; const FSCClipBinding* Binding; };
        TArray<FLayer> OpeningLayers;
        double Span = 0.;
        for (const FSCClipPlayback& Play : Playback)
            if (Play.OpeningGangway == Index)
            {
                const auto& Clip = ClipCatalog->Clips[Play.Clip];
                for (const auto& Binding : Clip.Bindings)
                    if (Binding.Mesh == Rig->GetSkeletalMeshAsset())
                    {
                        OpeningLayers.Add({&Play, &Clip, &Binding});
                        Span = FMath::Max(Span, double(Seconds) * Clip.Fps * FMath::Abs(Play.Speed));
                    }
            }
        if (Span <= 0) continue;
        OpeningLayers.StableSort([](const FLayer& A, const FLayer& B) { return A.Play->Order < B.Play->Order; });
        auto Pose = [&](double T, TArray<FVector>& Points) {
            TArray<FTransform> Bones = Rig->LocalPose;
            for (const FLayer& Layer : OpeningLayers)
            {
                const double Frame = FMath::Clamp(Layer.Play->Frame + T / Span * Seconds * Layer.Clip->Fps * Layer.Play->Speed,
                                                  0., double(Layer.Clip->LastFrame));
                for (const FSCBoneTrack& Track : Layer.Binding->Tracks)
                {
                    if (!Bones.IsValidIndex(Track.Bone)) continue;
                    const auto Key = FSCClipSampling::Sample(Track, FSCClipSampling::TrackFrame(*Layer.Clip, Track, Frame, Layer.Play->Speed < 0));
                    if (Track.bRotation) Bones[Track.Bone].SetRotation(Key.GetRotation());
                    if (Track.bPosition) Bones[Track.Bone].SetTranslation(Key.GetTranslation());
                }
            }
            for (int32 I = 0; I < Bones.Num(); ++I)
            {
                const int32 Parent = Ref.GetParentIndex(I);
                if (Parent != INDEX_NONE) Bones[I] *= Bones[Parent];
            }
            for (const auto& Set : State.GroundProbes)
            {
                const FTransform World = Bones[Set.Bone] * Rig->GetComponentTransform();
                for (FVector P : Set.Points) Points.Add(World.TransformPosition(P));
            }
        };
        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
        Objects.AddObjectTypesToQuery(ECC_PhysicsBody);
        const FCollisionQueryParams Params(SCENE_QUERY_STAT(SCGangwayGround), false, this);
        TArray<FHitResult> Hits;
        auto Trace = [&](FVector A, FVector B) -> TOptional<double> {
            Hits.Reset();
            GetWorld()->LineTraceMultiByObjectType(Hits, A, B, Objects, Params);
            TOptional<double> First;
            for (const FHitResult& Hit : Hits)
            {
                const AActor* Owner = Hit.GetActor();
                if (!Hit.GetComponent() || Hit.GetComponent()->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block ||
                    (Owner && (Owner->IsA<APawn>() || Owner->IsAttachedTo(this))) || Hit.bStartPenetrating ||
                    Hit.ImpactNormal.Z < 0.1 || FVector::DotProduct(B-A,Hit.ImpactNormal) >= 0.) continue;
                const double Time = FMath::Max(0., double(Hit.Time) - 0.1 / FVector::Dist(A, B));
                if (!First.IsSet() || Time < First.GetValue()) First = Time;
            }
            return First;
        };
        auto MayHit = [&](const FBox& Box) {
            return GetWorld()->OverlapAnyTestByObjectType(Box.GetCenter(), FQuat::Identity, Objects,
                FCollisionShape::MakeBox(Box.GetExtent()), Params);
        };
        if (auto Hit = SCGangwayGround::LimitFrame(0., Span, Pose, Trace, MayHit); Hit.IsSet())
        {
            OutFractions.Add(Index, Hit.GetValue() / Span);
            UE_LOG(LogTemp, Display, TEXT("GANGWAY_GROUND_CONTACT door=%d rig=%s fraction=%.5f"), Index, *Rig->GetName(), Hit.GetValue() / Span);
        }
    }
}
