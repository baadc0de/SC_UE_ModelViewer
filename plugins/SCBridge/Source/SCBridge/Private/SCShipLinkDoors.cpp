// WP-M1: door runtime state, access, and the data-derived room classification.
#include "SCVehicle.h"
#include "SCGangwayGround.h"
#include "Components/BoxComponent.h"
#include "Engine/OverlapResult.h"
#include "SCFirstPersonCharacter.h"
#include "SCRigComponent.h"
#include "SCViewerSubsystem.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/MeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodyInstance.h"

namespace
{
FBox DoorAperture(const ASCVehicle& Ship, const FSCDoorInteraction& Door,
    TArray<ASCVehicle::FShipDoor::FMovementCollider>& Colliders)
{
    FBox Bounds(ForceInit);
    TArray<USceneComponent*> Parts;
    Door.Rig->GetChildrenComponents(true, Parts);
    const FTransform& ShipFrame = Ship.GetActorTransform();
    const bool bSharedRig = Ship.Doors.ContainsByPredicate([&](const FSCDoorInteraction& Other) {
        return Other.Rig == Door.Rig && &Other != &Door;
    });
    for (USceneComponent* Part : Parts)
        if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Part);
            Primitive && Primitive->IsCollisionEnabled())
        {
            const USceneComponent* Attachment = Part;
            const USceneComponent* OwnerRig = Part->GetAttachParent();
            while (OwnerRig && !OwnerRig->IsA<USCRigComponent>())
            {
                Attachment = OwnerRig;
                OwnerRig = OwnerRig->GetAttachParent();
            }
            if (OwnerRig != Door.Rig || (bSharedRig && Attachment->GetAttachSocketName() != Door.Joint)) continue;
            const FTransform Pose = Primitive->GetComponentTransform().GetRelativeTransform(ShipFrame);
            Bounds += Primitive->CalcBounds(Pose).GetBox();
            if (!Door.bStartsOpen && Primitive->IsQueryCollisionEnabled() && Primitive->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block)
                Colliders.Add({Primitive, Attachment->GetAttachSocketName(), Pose, {}});
        }
    if (!Bounds.IsValid && !bSharedRig && Door.Rig->IsCollisionEnabled())
        Bounds = Door.Rig->CalcBounds(Door.Rig->GetComponentTransform().GetRelativeTransform(ShipFrame)).GetBox();
    return Bounds;
}

// If the owner's fragment drives this rig with one reversible clip, the authored pose samples also describe closing.
// An ambiguous fragment, shared timing, or a deformed mesh keeps the original conservative doorway bounds.
const FSCImportedClip* ReversibleDoorClip(const USCClipCatalog* Catalog, const FSCDoorInteraction& Door)
{
    if (!Catalog || !Door.bReverseClose || Door.OpenTag.IsNone() || Door.Rig->OwnerEntity.IsEmpty()) return nullptr;
    const FSCImportedClip* Selected = nullptr;
    for (const FSCImportedClip& Clip : Catalog->Clips)
    {
        if (!Clip.Owner.Equals(Door.Rig->OwnerEntity, ESearchCase::IgnoreCase)) continue;
        const FSCClipBinding* Binding = Clip.Bindings.FindByPredicate([&](const FSCClipBinding& Candidate) {
            return Candidate.Mesh == Door.Rig->GetSkeletalMeshAsset();
        });
        if (!Binding || !Clip.Fragments.ContainsByPredicate([&](const FSCClipFragment& F) {
            return F.OptionWeight > 0 && F.Fragment == Door.Fragment &&
                (F.Tags.Contains(Door.OpenTag) || F.FragmentTags.Contains(Door.OpenTag)) &&
                (F.AnimationNames.IsEmpty() || F.AnimationNames.Contains(Clip.Id));
        })) continue;
        if (Selected) return nullptr;
        Selected = &Clip;
    }
    return Selected;
}

bool SameColliderPose(const FTransform& A, const FTransform& B)
{
    return A.GetLocation().Equals(B.GetLocation(), 1.0) &&
        A.GetRotation().AngularDistance(B.GetRotation()) <= 0.001 &&
        A.GetScale3D().Equals(B.GetScale3D(), 0.001);
}

void BuildMovementSweep(const ASCVehicle& Ship, const FSCDoorInteraction& Door, ASCVehicle::FShipDoor& State)
{
    const USkeletalMesh* Mesh = Door.Rig->GetSkeletalMeshAsset();
    const FSCImportedClip* Clip = State.MovementColliders.IsEmpty() || !Mesh ? nullptr : ReversibleDoorClip(Ship.ClipCatalog, Door);
    if (!Clip) return;
    const FSCClipBinding* Binding = nullptr;
    for (const FSCClipBinding& Candidate : Clip->Bindings)
        if (Candidate.Mesh == Mesh)
        {
            if (Binding) return;
            Binding = &Candidate;
        }
    const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
    const TArray<FTransform>& Reference = Skeleton.GetRefBonePose();
    if (!Binding || Binding->Tracks.IsEmpty() || Clip->LastFrame < 1 || Binding->Tracks.ContainsByPredicate([&](const FSCBoneTrack& Track) {
        // Reverse alignment can differ for mixed-length sources. Keep the bounds guard for those clips.
        return !Reference.IsValidIndex(Track.Bone) || Track.Keys.Num() != Clip->LastFrame + 1 ||
            !FMath::IsNearlyEqual(Track.Fps, Clip->Fps);
    })) return;
    const FTransform RigInShip = Door.Rig->GetComponentTransform().GetRelativeTransform(Ship.GetActorTransform());
    const FVector RigScale = RigInShip.GetScale3D().GetAbs();
    struct FPart
    {
        FTransform SocketOffset;
        FBox ShapeBounds;
        int32 Bone;
        double Radius;
    };
    TArray<FPart> Parts;
    for (const auto& Collider : State.MovementColliders)
    {
        UPrimitiveComponent* Component = Collider.Component.Get();
        const int32 Bone = Collider.Socket.IsNone() ? INDEX_NONE : Skeleton.FindBoneIndex(Collider.Socket);
        if (!Component || Component->IsA<USkeletalMeshComponent>() || (!Collider.Socket.IsNone() && !Reference.IsValidIndex(Bone))) return;
        const FBodyInstance* Body = Component->GetBodyInstance(NAME_None, false);
        if (!Body || !Body->IsValidBodyInstance()) return;
        const FTransform Socket = Bone == INDEX_NONE ? Door.Rig->GetComponentTransform() : Door.Rig->GetSocketTransform(Collider.Socket);
        const FTransform Offset = Component->GetComponentTransform().GetRelativeTransform(Socket);
        const FBox WorldBounds = Body->GetBodyBounds();
        if (!WorldBounds.IsValid) return;
        const FBox ShapeBounds = WorldBounds.InverseTransformBy(Component->GetComponentTransform());
        const FBox Bounds = ShapeBounds.TransformBy(Offset);
        if (!Bounds.IsValid) return;
        Parts.Add({Offset, ShapeBounds, Bone, Bounds.Min.GetAbs().ComponentMax(Bounds.Max.GetAbs()).Size()});
    }
    TArray<FTransform> Previous, Local, ComponentSpace;
    ComponentSpace.SetNum(Reference.Num());
    for (int32 Frame = 0; Frame <= Clip->LastFrame; ++Frame)
    {
        Local = Reference;
        for (const FSCBoneTrack& Track : Binding->Tracks)
        {
            if (Track.bPosition) Local[Track.Bone].SetTranslation(Track.Keys[Frame].GetTranslation());
            if (Track.bRotation) Local[Track.Bone].SetRotation(Track.Keys[Frame].GetRotation());
        }
        for (int32 Bone = 0; Bone < Reference.Num(); ++Bone)
        {
            const int32 Parent = Skeleton.GetParentIndex(Bone);
            ComponentSpace[Bone] = Parent == INDEX_NONE ? Local[Bone] : Local[Bone] * ComponentSpace[Parent];
        }
        for (int32 I = 0; I < Parts.Num(); ++I)
        {
            const FPart& Part = Parts[I];
            auto& Collider = State.MovementColliders[I];
            const FTransform& BonePose = Part.Bone == INDEX_NONE ? FTransform::Identity : ComponentSpace[Part.Bone];
            const FTransform Pose = Part.SocketOffset * BonePose * RigInShip;
            if (Frame == 0)
            {
                if (!SameColliderPose(Pose, Collider.ClosedPose))
                {
                    for (auto& Reset : State.MovementColliders) Reset.Sweep.Reset();
                    return;
                }
                Collider.Sweep.Reserve(Clip->LastFrame + 1);
            }
            else
            {
                // Between adjacent keys, each joint's translation is bounded by its endpoint delta and every point's
                // rotational travel is bounded by radius times angle. Apply both bounds up the parent chain.
                double Radius = Part.Radius, Travel = 0;
                for (int32 Bone = Part.Bone; Bone != INDEX_NONE; Bone = Skeleton.GetParentIndex(Bone))
                {
                    const FTransform& A = Previous[Bone];
                    const FTransform& B = Local[Bone];
                    const double Scale = A.GetScale3D().GetAbs().GetMax();
                    Radius *= Scale;
                    Travel = Travel * Scale + FVector::Dist(A.GetLocation(), B.GetLocation()) +
                        Radius * A.GetRotation().AngularDistance(B.GetRotation());
                    Radius += FMath::Max(A.GetLocation().Size(), B.GetLocation().Size());
                }
                auto& Prior = Collider.Sweep.Last();
                Prior.TravelCm = Travel * RigScale.GetMax();
                State.MovementBounds += Part.ShapeBounds.TransformBy(Prior.Pose).ExpandBy(Prior.TravelCm);
            }
            Collider.Sweep.Add({Pose, 0});
            State.MovementBounds += Part.ShapeBounds.TransformBy(Pose);
        }
        Swap(Previous, Local);
    }
}

bool PawnInDoor(const ASCVehicle& Ship, const FBox& Aperture)
{
    if (!Aperture.IsValid) return false;
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(&Ship);
    const ASCFirstPersonCharacter* Player = Viewer ? Viewer->GetFirstPerson() : nullptr;
    const UCapsuleComponent* Capsule = Player ? Player->GetCapsuleComponent() : nullptr;
    if (!Capsule) return false;
    const FVector Position = Ship.GetActorTransform().InverseTransformPosition(Capsule->GetComponentLocation());
    const double Radius = Capsule->GetScaledCapsuleRadius();
    const double HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
    return Aperture.ExpandBy(FVector(Radius, Radius, HalfHeight)).IsInsideOrOn(Position);
}

}

void ASCVehicle::RememberGangwayStop(int32 Index)
{
    FShipDoor& Door = ShipDoors[Index];
    Door.GroundLastSample = INDEX_NONE;
    if (Door.MovementColliders.IsEmpty() || Door.MovementColliders.ContainsByPredicate([](const auto& C) {
        return C.Sweep.IsEmpty() || !C.Component.IsValid();
    })) return;
    const FSCImportedClip* Clip = ReversibleDoorClip(ClipCatalog, Doors[Index]);
    const FSCClipPlayback* Held = nullptr;
    for (const auto& P : HeldPlayback)
        if (P.OpeningGangway == Index)
        {
            if (Held || &ClipCatalog->Clips[P.Clip] != Clip || P.Speed <= 0) return;
            Held = &P;
        }
    if (!Held) return;
    Door.GroundLastSample = FMath::FloorToInt(Held->Frame);
    for (auto& C : Door.MovementColliders)
        C.GroundPose = C.Component->GetComponentTransform().GetRelativeTransform(GetActorTransform());
}

bool ASCVehicle::DoorwayOccupied(int32 Index) const
{
    if (!ShipDoors.IsValidIndex(Index)) return false;
    const FShipDoor& Door = ShipDoors[Index];
    const UWorld* World = GetWorld();
    const FTransform& ShipFrame = GetActorTransform();
    FBox Aperture = Door.Aperture;
    Aperture += Door.MovementBounds;
    for (const auto& Collider : Door.MovementColliders)
        if (const UPrimitiveComponent* Part = Collider.Component.Get())
        {
            const FBodyInstance* Body = Part->GetBodyInstance(NAME_None, false);
            Aperture += Body && Body->IsValidBodyInstance() ? Body->GetBodyBounds().InverseTransformBy(ShipFrame) :
                Part->CalcBounds(Part->GetComponentTransform().GetRelativeTransform(ShipFrame)).GetBox();
        }
    if (!World || !Aperture.IsValid) return false;
    if (PawnInDoor(*this, Aperture)) return true;
    const FVector Half = Aperture.GetExtent() * ShipFrame.GetScale3D().GetAbs();
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    Objects.AddObjectTypesToQuery(ECC_PhysicsBody);
    const FCollisionQueryParams Params(SCENE_QUERY_STAT(SCSecureDoorway), false, this);
    TArray<FOverlapResult> Hits;
    World->OverlapMultiByObjectType(Hits, ShipFrame.TransformPosition(Aperture.GetCenter()), ShipFrame.GetRotation(),
        Objects, FCollisionShape::MakeBox(Half), Params);
    return Hits.ContainsByPredicate([&](const FOverlapResult& Hit) {
        // A doorway blocker (DoorwayBlockerTag; UnrealCitizen: a cargo box) by its box root.
        const AActor* Blocker = Hit.GetActor();
        const UBoxComponent* Box = Blocker && Blocker->ActorHasTag(DoorwayBlockerTag) ? Cast<UBoxComponent>(Blocker->GetRootComponent()) : nullptr;
        if (!Box) return false;
        const FVector Scale = ShipFrame.GetScale3D();
        if (Door.MovementColliders.IsEmpty() || Scale.X <= 0 ||
            !FMath::IsNearlyEqual(Scale.X, Scale.Y) || !FMath::IsNearlyEqual(Scale.X, Scale.Z)) return true;
        const FVector Position = Box->GetComponentLocation();
        const FQuat Rotation = Box->GetComponentQuat();
        const FCollisionShape Shape = FCollisionShape::MakeBox(Box->GetScaledBoxExtent());
        return Door.MovementColliders.ContainsByPredicate([&](const FShipDoor::FMovementCollider& Collider) {
            const UPrimitiveComponent* Part = Collider.Component.Get();
            const FBodyInstance* Body = Part ? Part->GetBodyInstance(NAME_None, false) : nullptr;
            // Narrow only from the fully open authored pose. Interrupted motion and other pose layers keep the bounds guard.
            if (!Body || !Body->IsValidBodyInstance() || Collider.Sweep.IsEmpty()) return true;
            const bool bGrounded = Collider.Sweep.IsValidIndex(Door.GroundLastSample);
            const FTransform& EndPose = bGrounded ? Collider.GroundPose : Collider.Sweep.Last().Pose;
            if (!SameColliderPose(Part->GetComponentTransform().GetRelativeTransform(ShipFrame), EndPose))
                return true;
            if (Body->OverlapTest(Position, Rotation, Shape)) return true;
            const FTransform Current = Body->GetUnrealWorldTransform();
            const FTransform BodyOffset = Current.GetRelativeTransform(Part->GetComponentTransform());
            // The last interval retains its full conservative travel allowance when contact was between two keys.
            const int32 Count = bGrounded ? Door.GroundLastSample + 1 : Collider.Sweep.Num();
            return MakeArrayView(Collider.Sweep).Left(Count).ContainsByPredicate([&](const FShipDoor::FMovementCollider::FSample& Sample) {
                const FTransform Pose = BodyOffset * Sample.Pose * ShipFrame;
                // Map the grown box to the collider's current physics frame without changing the live door pose.
                const FVector InCurrent = Current.TransformPositionNoScale(Pose.InverseTransformPositionNoScale(Position));
                const FQuat InCurrentRotation = Current.GetRotation() * Pose.GetRotation().Inverse() * Rotation;
                const FVector Half = Box->GetScaledBoxExtent() + FVector(Sample.TravelCm * Scale.X);
                return Body->OverlapTest(InCurrent, InCurrentRotation, FCollisionShape::MakeBox(Half));
            });
        });
    });
}

void ASCVehicle::RememberDoorPanelLabel(const USCRigComponent* Rig, const FSCUiBinding& Binding)
{
    if (Rig && !Binding.DoorLabel.IsEmpty() && !DoorPanelLabels.Contains(Rig)) DoorPanelLabels.Add(Rig, Binding.DoorLabel);
}

void ASCVehicle::BuildShipDoors()
{
    ShipDoors.Reset(Doors.Num());
    TArray<FString> Labels;
    for (const FSCDoorInteraction& Door : Doors)
    {
        FShipDoor& Data = ShipDoors.AddDefaulted_GetRef();
        FString Label = Door.DisplayName;
        if (Door.Rig)
        {
            const FSCDoorRecord* Record = DoorRecords.Find(Door.Rig.Get());
            Data.bLocked = Record && Record->Door.bIsLocked.Get(false);
            Data.Aperture = DoorAperture(*this, Door, Data.MovementColliders);
            BuildMovementSweep(*this, Door, Data);
            const FSCSceneInstance* Source = Manifest.Children.FindByPredicate([&](const FSCSceneInstance& Item) {
                return Item.EntityName == Door.Rig->OwnerEntity;
            });
            if (Source)
            {
                Data.bRamp = SCGangwayGround::IsRampRole(Source->ItemType, Source->ItemSubType,
                    Source->PortPath.IsEmpty() ? FString() : Source->PortPath.Last());
                if (Data.bRamp) BuildGangwayProbes(ShipDoors.Num() - 1);
                if (!Source->DisplayName.IsEmpty()) Label = Source->DisplayName;
                else if (!Source->PortPath.IsEmpty())
                {
                    Label = Source->PortPath.Last();
                    Label.RemoveFromStart(TEXT("hardpoint_"), ESearchCase::IgnoreCase);
                    Label.ReplaceInline(TEXT("_"), TEXT(" "));
                    if (!Label.IsEmpty()) Label[0] = FChar::ToUpper(Label[0]);
                }
            }
            if (Data.Aperture.IsValid && !LightAreas.IsEmpty())
            {
                const FVector Extent = Data.Aperture.GetExtent();
                int32 Axis = Extent.Y < Extent.X ? 1 : 0;
                if (Extent.Z < Extent[Axis]) Axis = 2;
                const FVector Centre = Data.Aperture.GetCenter();
                // Probe each side of the opening. Start close to its faces and allow a gap between authored room volumes.
                for (double Gap : {10.0, 25.0, 50.0})
                {
                    FVector Offset(ForceInit);
                    Offset[Axis] = Extent[Axis] + Gap;
                    const FString A = LightRoomAt(GetActorTransform().TransformPosition(Centre + Offset));
                    const FString B = LightRoomAt(GetActorTransform().TransformPosition(Centre - Offset));
                    const ESCShipDoorGroup Estimate = FSCShipDoorPolicy::Classify(true, A, B);
                    if (Estimate != ESCShipDoorGroup::Other)
                    {
                        Data.Group = Estimate;
                        Data.GroupSource = TEXT("room_volumes");
                        if (Estimate == ESCShipDoorGroup::Interior) break; // a room on either side is stronger evidence
                    }
                }
            }
            if (const FString* PanelLabel = DoorPanelLabels.Find(Door.Rig.Get())) Label = *PanelLabel;
        }
        Labels.Add(MoveTemp(Label));
    }
    SCHangarWaypoints::NumberDuplicates(Labels);
    for (int32 Id = 0; Id < ShipDoors.Num(); ++Id) ShipDoors[Id].Label = MoveTemp(Labels[Id]);
    RefreshPanelScreens();
}

FString ASCVehicle::ShipDoorLabel(int32 Index) const
{
    if (ShipDoors.IsValidIndex(Index) && !ShipDoors[Index].Label.IsEmpty()) return ShipDoors[Index].Label;
    return Doors.IsValidIndex(Index) ? Doors[Index].DisplayName : FString();
}

bool ASCVehicle::IsDoorLocked(int32 Index) const
{
    return ShipDoors.IsValidIndex(Index) && ShipDoors[Index].bLocked;
}

bool ASCVehicle::SetDoorLocked(int32 Index, bool bLocked)
{
    if (!Doors.IsValidIndex(Index) || !Doors[Index].Rig || LiftDrivenRigs.Contains(Doors[Index].Rig.Get())) return false;
    if (!ShipDoors.IsValidIndex(Index)) ShipDoors.SetNum(Doors.Num());
    FShipDoor& Door = ShipDoors[Index];
    Door.bSecurePending = false;
    if (Door.bLocked == bLocked) return true;
    Door.bLocked = bLocked;
    RefreshPanelScreens();
    return true;
}

FName ASCVehicle::ShipLinkPlayer() const
{
    if (const USCShipLinkSubsystem* ShipLink = USCShipLinkSubsystem::Get(this)) return ShipLink->GetPlayer();
    return TEXT("you");
}

FString ASCVehicle::DoorOrderRefusal(int32 Index, ESCShipDoorAction Action, FName Actor, const TOptional<FVector>& LocalUse) const
{
    if (!Doors.IsValidIndex(Index) || !Doors[Index].Rig) return TEXT("No such door");
    const FShipDoor* Info = ShipDoors.IsValidIndex(Index) ? &ShipDoors[Index] : nullptr;
    if (LiftDrivenRigs.Contains(Doors[Index].Rig.Get())) return TEXT("Lift-controlled door");
    const USCShipLinkSubsystem* ShipLink = USCShipLinkSubsystem::Get(this);
    const ESCShipRole ActorRole = ShipLink ? ShipLink->RoleOf(Actor, this) : ESCShipRole::None;
    const bool bLocalInside = LocalUse.IsSet() && IsInsideLightArea(LocalUse.GetValue());
    if (Info && Info->bCargoRelock)
    {
        if (Action == ESCShipDoorAction::Open && ActorRole != ESCShipRole::Owner) return TEXT("Locked");
        if (Action == ESCShipDoorAction::Lock || Action == ESCShipDoorAction::Unlock) return TEXT("Door in use by cargo");
    }
    if (Action == ESCShipDoorAction::Close)
        if (const FString Why = DoorInUse(Index); !Why.IsEmpty()) return Why;
    if (Info && Info->bSecurePending && Action == ESCShipDoorAction::Open) return TEXT("Securing");
    return FSCShipDoorPolicy::Refusal(ActorRole, Action, IsDoorLocked(Index),
        Info ? Info->Group : ESCShipDoorGroup::Other, bLocalInside);
}

FString ASCVehicle::OrderShipDoor(int32 Index, ESCShipDoorAction Action, FName Actor, const TOptional<FVector>& LocalUse)
{
    const FString Refusal = DoorOrderRefusal(Index, Action, Actor, LocalUse);
    if (!Refusal.IsEmpty()) return Refusal;
    if (Action == ESCShipDoorAction::Lock || Action == ESCShipDoorAction::Unlock)
        return SetDoorLocked(Index, Action == ESCShipDoorAction::Lock) ? FString() : FString(TEXT("Lock unavailable"));
    const bool bOpen = Action == ESCShipDoorAction::Open;
    if (IsDoorOpen(Index) == bOpen) return {};
    return PlayDoor(Index, bOpen, 0.f, 0, Actor, LocalUse) ? FString() : FString(TEXT("Door cannot move"));
}

TArray<FSCShipDoorResult> ASCVehicle::SecureShip(FName Actor)
{
    TArray<FSCShipDoorResult> Results;
    for (int32 Id = 0; Id < ShipDoors.Num(); ++Id)
    {
        FShipDoor& Door = ShipDoors[Id];
        if (Door.Group != ESCShipDoorGroup::Exterior || LiftDrivenRigs.Contains(Doors[Id].Rig.Get())) continue;
        FSCShipDoorResult& Result = Results.AddDefaulted_GetRef();
        Result.Door = Id;
        Result.Refusal = DoorOrderRefusal(Id, ESCShipDoorAction::Lock, Actor);
        if (!Result.Refusal.IsEmpty()) continue;
        if (IsDoorOpen(Id) && DoorwayOccupied(Id))
        {
            Result.Refusal = TEXT("Doorway occupied");
            continue;
        }
        Result.Refusal = OrderShipDoor(Id, ESCShipDoorAction::Close, Actor);
        if (!Result.Refusal.IsEmpty()) continue;
        if (IsDoorPlaying(Id)) Door.bSecurePending = Result.bPending = true;
        else SetDoorLocked(Id, true);
    }
    return Results;
}

void ASCVehicle::FinishSecureOrders()
{
    for (int32 Index = 0; Index < ShipDoors.Num(); ++Index)
        if (ShipDoors[Index].bSecurePending && !IsDoorPlaying(Index))
        {
            ShipDoors[Index].bSecurePending = false;
            if (!IsDoorOpen(Index)) SetDoorLocked(Index, true);
        }
}

const FName ASCVehicle::DoorwayBlockerTag(TEXT("SCDoorwayBlocker"));

namespace
{
TArray<TPair<FName, ASCVehicle::FDoorUseQuery>>& DoorUseQueries()
{
    static TArray<TPair<FName, ASCVehicle::FDoorUseQuery>> Queries;
    return Queries;
}
} // namespace
void ASCVehicle::AddDoorUseQuery(FName Name, FDoorUseQuery Query)
{
    RemoveDoorUseQuery(Name);
    DoorUseQueries().Emplace(Name, MoveTemp(Query));
}
void ASCVehicle::RemoveDoorUseQuery(FName Name) { DoorUseQueries().RemoveAll([&](const auto& Q) { return Q.Key == Name; }); }
FString ASCVehicle::DoorInUse(int32 Index) const
{
    for (const auto& [Name, Query] : DoorUseQueries())
        if (FString Why = Query(*this, Index); !Why.IsEmpty()) return Why;
    return FString();
}

void ASCVehicle::RestoreCargoDoorLocks()
{
    for (int32 Id = 0; Id < ShipDoors.Num(); ++Id)
    {
        FShipDoor& Door = ShipDoors[Id];
        if (!Door.bCargoRelock || !DoorInUse(Id).IsEmpty()) continue;
        if (IsDoorOpen(Id))
        {
            if (!DoorwayOccupied(Id)) PlayDoor(Id, false);
            continue;
        }
        if (IsDoorPlaying(Id)) continue;
        Door.bCargoRelock = false;
        SetDoorLocked(Id, true);
    }
}

bool ASCVehicle::AimDoorPanelLock(const USceneComponent* Panel, const FVector& Eye, const FVector& Direction, double Reach, bool bLocked) const
{
    for (const FSCFloorPanel& Face : DoorPanelLocks)
    {
        if (Face.Anchor.Get() != Panel || !Face.Component.IsValid()) continue;
        const FTransform Frame = Face.Frame();
        const FVector LocalEye = Frame.InverseTransformPosition(Eye);
        const FVector Dir = Frame.InverseTransformVectorNoScale(Direction.GetSafeNormal());
        double Distance = 0;
        FVector2f UV;
        if (!Face.Geometry.Raycast(LocalEye, Dir, Reach, Distance, UV)) continue;
        const int32 Option = Face.Layout.OptionAt(FVector2D(UV.X, UV.Y) - Face.UVBase);
        if (Face.Layout.Options.IsValidIndex(Option))
        {
            const FName Correct = bLocked ? TEXT("UnlockDoor") : TEXT("LockDoor");
            const FName Hit = Face.Layout.Options[Option].Interaction;
            if (Hit == Correct) return true;
            // The package has clickable rectangles for the default render. In the locked image the other
            // state-machine action occupies the same lock control when the panel authors UnlockDoor.
            if (bLocked && Hit == FName(TEXT("LockDoor")) && Face.StateTypes.ContainsByPredicate([](const FSCUiStateType& Type) {
                return Type.Name == TEXT("DoorLockState") && Type.Next(TEXT("UnlockDoor"), TEXT("DoorLocked")) != nullptr;
            })) return true;
        }
    }
    return false;
}
