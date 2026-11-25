// Spawn categories come from DataCore, interaction semantics from Mannequin and receiving joints.
#include "SCExploration.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Components/PrimitiveComponent.h"
#include "Components/MeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"

const FName FSCSpawnKind::Entrance(TEXT("Entrance"));
const FName FSCSpawnKind::SeatAccess(TEXT("SeatAccess"));
const FName FSCSpawnKind::Seat(TEXT("Seat"));
const FName FSCSpawnKind::Interior(TEXT("Interior"));
const FName FSCSpawnKind::Door(TEXT("Door"));
const FName FSCSpawnKind::Hull(TEXT("Hull"));
const FName FSCSpawnKind::Selected(TEXT("Selected"));
const FName FSCSpawnKind::Hangar(TEXT("Hangar"));
const TArray<FName>& FSCSpawnKind::StartOrder()
{
    // Entrance only when the data identifies a hull entrance (see WP-P1 follow-ups: no exported
    // property yet). A generic DataCore Door (lockers, weapon racks, bulkheads) never outranks the
    // seat access / seat hints. Selected ("spawn where I look") is a user choice, never ranked.
    static const TArray<FName> Order = {Entrance, SeatAccess, Seat, Interior, Door, Hull};
    return Order;
}
void ASCVehicle::AddSpawnPoint(FName Kind, const FString& Label, const FTransform& WorldTransform)
{
    FSCSpawnPoint Point;
    Point.Kind = Kind;
    Point.DisplayName = Label;
    Point.Transform = WorldTransform.GetRelativeTransform(GetActorTransform());
    SpawnPoints.Add(Point);
    ++SpawnRevision;
}
int32 ASCVehicle::SetSelectedSpawn(const FTransform& WorldTransform)
{
    // One user-selected location: replace, do not accumulate.
    SpawnPoints.RemoveAll([](const FSCSpawnPoint& P) { return P.Kind == FSCSpawnKind::Selected; });
    AddSpawnPoint(FSCSpawnKind::Selected, TEXT("Selected surface"), WorldTransform);
    return SpawnPoints.Num() - 1;
}
void ASCVehicle::AddItemSpawn(const FSCSceneInstance& Instance, USceneComponent* Anchor)
{
    // DataCore AttachDef.Type is the category; each type keeps its own spawn kind. The anchor's
    // transform is read in BuildExploration: helpers on rig sockets settle only after assembly.
    for (FName Kind : {FSCSpawnKind::Seat, FSCSpawnKind::SeatAccess, FSCSpawnKind::Door})
        if (Instance.ItemType.Equals(Kind.ToString(), ESearchCase::IgnoreCase))
            PendingItemSpawns.Add({Kind, Instance.EntityName, Anchor});
}
FBox ASCVehicle::ExplorationBounds() const
{
    FBox Box(ForceInit);
    TInlineComponentArray<UPrimitiveComponent*> Components(this);
    for (const auto* C : Components)
        if (C->IsCollisionEnabled()) Box += C->Bounds.GetBox();
    return Box.IsValid ? Box : GetComponentsBoundingBox(true);
}
void ASCVehicle::BuildExploration()
{
    Doors.Reset();
    // Item spawns first (stable order: kind:n addressing), from the assembled anchor transforms.
    TArray<FSCSpawnPoint> Items;
    for (const auto& Pending : PendingItemSpawns)
        if (Pending.Anchor.IsValid())
        {
            FSCSpawnPoint Point;
            Point.Kind = Pending.Kind;
            Point.DisplayName = Pending.Label;
            // Hint: the floor of the item's own geometry (bottom centre of what it carries), else its
            // anchor. Some ports sit at the vehicle origin and the item's mesh carries the offset.
            FTransform World = Pending.Anchor->GetComponentTransform();
            TArray<USceneComponent*> Parts;
            Pending.Anchor->GetChildrenComponents(true, Parts);
            // The item's box is taken in the vehicle's frame: a world box carried back into the vehicle grows by
            // the vehicle's rotation, and what is beside the seat would then count as on it.
            FBox Box(ForceInit), Local(ForceInit);
            for (const USceneComponent* Part : Parts)
                if (const auto* Primitive = Cast<UPrimitiveComponent>(Part); Primitive && Primitive->IsRegistered() && Primitive->IsCollisionEnabled())
                {
                    Box += Primitive->Bounds.GetBox();
                    Local += Primitive->CalcBounds(Primitive->GetComponentTransform().GetRelativeTransform(GetActorTransform())).GetBox();
                }
            if (Box.IsValid) World.SetLocation(FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z));
            Point.Transform = World.GetRelativeTransform(GetActorTransform());
            Point.Bounds = Local;
            // A port at the vehicle origin whose item carries no collidable geometry has no location
            // information (e.g. seat-access items whose position lives in interaction helpers): skip.
            if (!Box.IsValid && Point.Transform.GetLocation().IsNearlyZero(1))
            {
                UE_LOG(LogTemp, Display, TEXT("EXPLORATION_SPAWN_UNLOCATED kind=%s label=%s"), *Pending.Kind.ToString(), *Pending.Label);
                continue;
            }
            Items.Add(Point);
        }
    PendingItemSpawns.Reset();
    SpawnPoints.Insert(Items, 0);
    ++SpawnRevision;
    CollectDoors(true);
    const FBox Bounds = ExplorationBounds();
    if (Bounds.IsValid)
        AddSpawnPoint(FSCSpawnKind::Hull, TEXT("Above hull centre"), FTransform(FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.Max.Z)));
    const int32 Posed = ApplyDoorRestPoses();
    BuildShipDoors();
    UE_LOG(LogTemp, Display, TEXT("EXPLORATION spawns=%d doors=%d door_rest_poses=%d collision_ready=%d"), SpawnPoints.Num(), Doors.Num(), Posed,
           bCollisionReady);
}
void ASCVehicle::BuildDoorInteractions()
{
    // An environment's doors (a hangar's gates and doors): the same door interactions, rest poses and door state as a vehicle
    // explored in first person, without its spawn points.
    Doors.Reset();
    CollectDoors(false);
    const int32 Posed = ApplyDoorRestPoses();
    BuildShipDoors();
    UE_LOG(LogTemp, Display, TEXT("ENVIRONMENT_DOORS doors=%d door_rest_poses=%d"), Doors.Num(), Posed);
}
void ASCVehicle::CollectDoors(bool bSpawnPoints)
{
    if (ClipCatalog)
        for (const auto& Clip : ClipCatalog->Clips)
            for (const auto& Fragment : Clip.Fragments)
            {
                auto HasTag = [](const FSCClipFragment& F, FName Tag) { return !Tag.IsNone() && (F.FragmentTags.Contains(Tag) || F.Tags.Contains(Tag)); };
                auto FragmentHasTag = [&](FName Tag) {
                    return ClipCatalog->Clips.ContainsByPredicate([&](const FSCImportedClip& Other) {
                        return Other.Fragments.ContainsByPredicate([&](const FSCClipFragment& F) { return F.Fragment == Fragment.Fragment && HasTag(F, Tag); });
                    });
                };
                // A door by its own data (C2 fixes): a rig the clip binds whose door record's open tag (a single-procedural door's
                // transition) is this fragment's, with its close tag on the fragment for an open/close door.
                bool bDataDoor = false;
                for (const USCRigComponent* Rig : Rigs)
                    if (const FSCDoorRecord* R = Rig ? DoorRecords.Find(Rig) : nullptr;
                        R && R->Door.bValid && Clip.Bindings.ContainsByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Rig->GetSkeletalMeshAsset(); }))
                    {
                        const bool bSingle = !R->Door.TransitionTag.IsNone();
                        bDataDoor |= HasTag(Fragment, bSingle ? R->Door.TransitionTag : R->Door.OpenTag) && (bSingle || FragmentHasTag(R->Door.CloseTag));
                    }
                // Else the category is the Mannequin Open/Close tag pair on a rig the clip binds; no fragment or entity vocabulary.
                if (!bDataDoor && (!HasTag(Fragment, TEXT("Open")) || !FragmentHasTag(TEXT("Close")))) continue;
                TArray<FName> Joints;
                TSharedPtr<FJsonObject> Json;
                if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Fragment.Json), Json) && Json)
                {
                    const TArray<TSharedPtr<FJsonValue>>* Procedures;
                    if (Json->TryGetArrayField(TEXT("procedurals"), Procedures))
                        for (const auto& P : *Procedures)
                        {
                            const TSharedPtr<FJsonObject>* Params;
                            FString Joint;
                            if (P->AsObject() && P->AsObject()->TryGetObjectField(TEXT("params"), Params) &&
                                (*Params)->TryGetStringField(TEXT("AttachmentJoint"), Joint) && !Joint.IsEmpty())
                                Joints.AddUnique(FName(*Joint));
                        }
                }
                for (USCRigComponent* Rig : Rigs)
                {
                    const auto* Instance = Manifest.Children.FindByPredicate([&](const FSCSceneInstance& I) { return I.EntityName == Rig->OwnerEntity; });
                    const auto* Binding = Clip.Bindings.FindByPredicate([&](const FSCClipBinding& B) { return B.Mesh == Rig->GetSkeletalMeshAsset(); });
                    if (!Binding) continue;
                    TArray<FName> Driven = Joints;
                    if (Driven.IsEmpty())
                        for (const auto& Track : Binding->Tracks)
                            Driven.AddUnique(Rig->GetSkeletalMeshAsset()->GetRefSkeleton().GetBoneName(Track.Bone));
                    for (FName Joint : Driven)
                    {
                        if (!Rig->DoesSocketExist(Joint)) continue;
                        if (Doors.ContainsByPredicate([&](const FSCDoorInteraction& D) { return D.Rig == Rig && D.Fragment == Fragment.Fragment; })) break;
                        FSCDoorInteraction Door;
                        Door.Rig = Rig;
                        Door.Fragment = Fragment.Fragment;
                        Door.Joint = Joint;
                        // The door's own data (SCItemDoorParams via `door`): its fragment tags, spawn state, rate and port links.
                        if (const FSCDoorRecord* Record = DoorRecords.Find(Rig); Record && Record->Door.bValid)
                        {
                            const FSCDoorTiming& D = Record->Door;
                            Door.bReverseClose = !D.TransitionTag.IsNone();
                            if (Door.bReverseClose) Door.OpenTag = D.TransitionTag;
                            else if (!D.OpenTag.IsNone()) Door.OpenTag = D.OpenTag;
                            if (!D.CloseTag.IsNone()) Door.CloseTag = D.CloseTag;
                            Door.bStartsOpen = D.bIsOpened.Get(false);
                            Door.Speed = D.bSpeedKnown && D.AnimationSpeed > 0 ? D.AnimationSpeed : 1.f;
                            Door.PortLinks = Record->PortLinks;
                        }
                        const FString Label = Instance ? Instance->EntityName :
                            Clip.Owner.Contains(TEXT("/")) ? FPaths::GetBaseFilename(Clip.Owner) : Joint.ToString();
                        Door.DisplayName = Label.Replace(TEXT("_"), TEXT(" "));
                        Doors.Add(Door);
                        if (bSpawnPoints) AddSpawnPoint(FSCSpawnKind::Door, Door.DisplayName, Rig->GetSocketTransform(Joint));
                        UE_LOG(LogTemp, Display, TEXT("EXPLORATION_INTERACTION fragment=%s owner=%s item_type=%s joint=%s label=\"%s\""),
                               *Fragment.Fragment.ToString(), *Rig->OwnerEntity, Instance ? *Instance->ItemType : TEXT("-"),
                               *Joint.ToString(), *Door.DisplayName);
                        break;
                    }
                }
            }
}
int32 ASCVehicle::ApplyDoorRestPoses()
{
    // The authored spawn state (SCItemDoorParams.IsOpened; DoorOpenState's default is DoorClosed): a single-procedural door
    // is its transition's time (closed at the start, open at the end), so it holds that frame; an open/close door opened in
    // the data holds its open fragment's end; a closed one keeps the bind pose, which is its open fragment's start.
    int32 Posed = 0;
    TSet<TPair<const USCRigComponent*, FName>> Done;
    for (const FSCDoorInteraction& D : Doors)
    {
        if (!D.Rig || Done.Contains({D.Rig.Get(), D.Fragment})) continue;
        Done.Add({D.Rig.Get(), D.Fragment});
        // The authored end pose of the state: open = the end of the open fragment; closed = the start of a single-procedural
        // door's transition, or the end of an open/close door's close fragment. A door without door data keeps its bind pose
        // closed (the Mannequin pair only).
        const FSCDoorRecord* Record = DoorRecords.Find(D.Rig.Get());
        const bool bData = Record && Record->Door.bValid;
        bool bPosed = false;
        if (D.bStartsOpen) bPosed = PlayFragmentAt(D.Fragment, D.OpenTag, D.Rig, 1.f, ESCFragmentStart::HoldLast);
        else if (D.bReverseClose) bPosed = PlayFragmentAt(D.Fragment, D.OpenTag, D.Rig, 1.f, ESCFragmentStart::HoldFirst);
        else if (bData) bPosed = PlayFragmentAt(D.Fragment, D.CloseTag, D.Rig, 1.f, ESCFragmentStart::HoldLast);
        if (bPosed || (!bData && !D.bStartsOpen))
        {
            FragmentTargets.Add({D.Rig.Get(), D.Fragment}, D.bStartsOpen ? D.OpenTag : D.CloseTag);
            UnsettledTargets.Remove({D.Rig.Get(), D.Fragment});
        }
        else
            UE_LOG(LogTemp, Warning, TEXT("DOOR_REST owner=%s fragment=%s open=%d: no fragment poses the authored state; left at the bind pose, state not recorded"),
                   *D.Rig->OwnerEntity, *D.Fragment.ToString(), D.bStartsOpen);
        Posed += bPosed;
    }
    if (!ScreenFaces.IsEmpty()) RefreshPanelScreens();
    return Posed;
}
bool ASCVehicle::PlayDoor(int32 Index, bool bOpen, float SpeedScale, int32 Depth, FName AsActor, const TOptional<FVector>& LocalUse, bool bCargoAccess)
{
    if (!Doors.IsValidIndex(Index) || !Doors[Index].Rig) return false;
    const USCShipLinkSubsystem* Link = USCShipLinkSubsystem::Get(this);
    const bool bOwnerCargoOpen = bOpen && bCargoAccess && Link &&
        Link->RoleOf(AsActor, this) == ESCShipRole::Owner && !IsLiftDriven(Doors[Index].Rig.Get()) &&
        !(ShipDoors.IsValidIndex(Index) && ShipDoors[Index].bSecurePending);
    if (!AsActor.IsNone() && !bOwnerCargoOpen &&
        !DoorOrderRefusal(Index, bOpen ? ESCShipDoorAction::Open : ESCShipDoorAction::Close, AsActor, LocalUse).IsEmpty()) return false;
    if (Depth > 8) // a cycle of item-port links in the data
    {
        UE_LOG(LogTemp, Warning, TEXT("DOOR_PORT_LINK depth=%d: link chain too deep, not followed"), Depth);
        return false;
    }
    const FSCDoorInteraction D = Doors[Index]; // a copy: the deliveries below may touch the array's owner state
    USCRigComponent* Rig = D.Rig;
    const bool bBackward = !bOpen && D.bReverseClose;
    const FName Tag = bOpen || bBackward ? D.OpenTag : D.CloseTag;
    const float Speed = (SpeedScale > 0 ? SpeedScale : D.Speed) * (bBackward ? -1.f : 1.f);
    const uint64 PriorOrder = NextOrder;
    const bool bWasAtEnd = IsDoorStateKnown(Index) && IsDoorOpen(Index) == bOpen; // before the fragment records its tag
    const int32 OuterOrder = OrderingDoor;
    OrderingDoor = Index; // this door's move is reported below, once; other doors its fragment drives stop
    const bool bStarted = PlayFragmentAt(D.Fragment, Tag, Rig, Speed, ESCFragmentStart::CurrentPose);
    OrderingDoor = OuterOrder;
    if (!bStarted) return false;
    // WP-A1: the door's move (its effects listen); it arrives when the playback it started ends.
    StartDoorMotion(Index, bOpen, PriorOrder, bWasAtEnd);
    if (DoorOrderBatch.Contains(Index)) DoorOrderDelivered.Add(Index);
    if (ShipDoors.IsValidIndex(Index))
    {
        FShipDoor& State = ShipDoors[Index];
        State.GroundLastSample = INDEX_NONE;
        if (bOpen && !State.GroundProbes.IsEmpty())
            for (FSCClipPlayback& P : Playback)
                if (P.TargetRig.Get() == Rig && P.Order > PriorOrder) P.OpeningGangway = Index;
        const bool bOwnerEgress = Link && Link->RoleOf(AsActor, this) == ESCShipRole::Owner &&
            State.Group == ESCShipDoorGroup::Exterior && LocalUse.IsSet() && IsInsideLightArea(LocalUse.GetValue());
        State.bActorOpenedWithoutAccess = bOpen && !AsActor.IsNone() && IsDoorLocked(Index) && !bOwnerCargoOpen && !bOwnerEgress;
    }
    if (bOwnerCargoOpen && IsDoorLocked(Index))
    {
        SetDoorLocked(Index, false);
        ShipDoors[Index].bCargoRelock = true;
    }
    FragmentTargets.Add({Rig, D.Fragment}, bOpen ? D.OpenTag : D.CloseTag);
    UnsettledTargets.Remove({Rig, D.Fragment});    if (!ScreenFaces.IsEmpty()) RefreshPanelScreens();
    UpdateTickEnabled();
    // The door's own interaction goes on to the items of the vehicle ports its links name (the C2's front hatch: its Open and
    // Close reach the front doors and the ramp).
    for (const FSCItemPortLink& L : D.PortLinks)
    {
        bool bSourceOpen = false, bSourceToggle = false, bTargetOpen = false, bTargetToggle = false;
        if (!FSCInteractionNetwork::DoorVerb(L.SourceSlot, bSourceOpen, bSourceToggle) || bSourceToggle || bSourceOpen != bOpen) continue;
        const TWeakObjectPtr<USCRigComponent>* Target = PortRigs.Find(L.TargetPort);
        const int32 TargetDoor = Target ? DoorForRig(Target->Get()) : INDEX_NONE;
        bool bHandled = false;
        if (TargetDoor != INDEX_NONE && FSCInteractionNetwork::DoorVerb(L.TargetSlot, bTargetOpen, bTargetToggle))
        {
            if (bTargetToggle) bTargetOpen = !IsDoorOpen(TargetDoor);
            // A batch order (sc.Doors.Play) gives its doors their own targets: a link does not reverse one of them.
            if (const bool* Wanted = DoorOrderBatch.Find(TargetDoor); Wanted && *Wanted != bTargetOpen) continue;
            bHandled = IsDoorOpen(TargetDoor) == bTargetOpen || PlayDoor(TargetDoor, bTargetOpen, 0.f, Depth + 1, AsActor, LocalUse, bCargoAccess);
        }
        UE_LOG(LogTemp, Display, TEXT("DOOR_PORT_LINK source=%s slot=%s port=%s target=%s target_slot=%s handled=%d depth=%d"), *Rig->OwnerEntity,
               *L.SourceSlot.ToString(), *L.TargetPort, Target && Target->IsValid() ? *(*Target)->OwnerEntity : TEXT("-"), *L.TargetSlot.ToString(),
               bHandled, Depth);
    }
    return true;
}
int32 ASCVehicle::NearestDoor(FVector Position, float Reach) const
{
    int32 Best = INDEX_NONE;
    double Distance = FMath::Square(Reach);
    for (int32 I = 0; I < Doors.Num(); ++I)
    {
        const auto& D = Doors[I];
        if (!D.Rig || IsLiftDriven(D.Rig.Get())) continue;
        const double Candidate = FVector::DistSquared(Position, D.Rig->GetSocketLocation(D.Joint));
        if (Candidate <= Distance) { Distance = Candidate; Best = I; }
    }
    return Best;
}
USCRigComponent* ASCVehicle::DoorForPanel(const USceneComponent* Panel) const
{
    const TObjectPtr<USCRigComponent>* Rig = Panel ? DoorControls.Find(const_cast<USceneComponent*>(Panel)) : nullptr;
    return Rig ? Rig->Get() : nullptr;
}
float ASCVehicle::PanelReachOf(const USceneComponent* Panel) const
{
    return Panel ? PanelReach.FindRef(const_cast<USceneComponent*>(Panel)) : 0.f;
}
void ASCVehicle::SetPanelReach(const USceneComponent* Panel, float Metres)
{
    if (!Panel || !(Metres > 0)) return;
    float& Reach = PanelReach.FindOrAdd(const_cast<USceneComponent*>(Panel));
    Reach = FMath::Max(Reach, Metres * 100.f);
    MaxPanelReach = FMath::Max(MaxPanelReach, Reach);
}
int32 ASCVehicle::FindInteraction(FVector Eye, FVector Direction, float Reach, const AActor* Ignore, FSCInteractionPick* Pick) const
{
    // What the crosshair is on (the ray itself bounds the reach): the nearest rig owning the hit part.
    // Doors are large and their driven joints (hinges, hydraulics) can sit metres from the surface the
    // player faces, so joint distance alone misses them. The ray runs as far as the farthest-operated door panel: a panel
    // is used from as far as its own UI allows (PanelReach; the C2's exterior pads sit 3.5-4 m above a standing player's
    // eye), anything else only within Reach.
    FSCInteractionPick Local;
    FSCInteractionPick& Out = Pick ? *Pick : Local;
    Out = FSCInteractionPick();
    Out.Reach = Reach;
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCInteract), true, Ignore);
    const float Longest = FMath::Max(Reach, MaxPanelReach);
    const bool bHit = GetWorld() && GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Direction.GetSafeNormal() * Longest, ECC_Visibility, Params);
    if (bHit)
    {
        Out.Hit = GetNameSafe(Hit.GetComponent());
        Out.Distance = Hit.Distance;
    }
    if (bHit && Hit.GetActor() == this)
    {
        // A door panel (pad) operates its door, wherever the panel stands, within the panel's own reach: the panel's own
        // parts only (a rig mounted under the panel is another item: it keeps Reach).
        bool bCrossedRig = false;
        for (const USceneComponent* C = Hit.GetComponent(); C; C = C->GetAttachParent())
        {
            if (USCRigComponent* Door = DoorForPanel(C))
            {
                const float PadReach = bCrossedRig ? Reach : FMath::Max(Reach, PanelReachOf(C));
                const int32 Index = DoorForRig(Door);
                if (Index != INDEX_NONE && Hit.Distance <= PadReach)
                {
                    Out.Via = TEXT("panel");
                    Out.Reach = PadReach;
                    Out.bLockControl = AimDoorPanelLock(C, Eye, Direction, PadReach, IsDoorLocked(Index));
                    return Index;
                }
            }
            bCrossedRig |= C->IsA<USCRigComponent>();
        }
    }
    if (bHit && Hit.GetActor() == this && Hit.Distance <= Reach)
    {
        FName Socket = NAME_None;
        const USCRigComponent* Rig = nullptr;
        for (const USceneComponent* C = Hit.GetComponent(); C && !Rig; C = C->GetAttachParent())
        {
            Rig = Cast<USCRigComponent>(C);
            if (!Rig) Socket = C->GetAttachSocketName();
        }
        TArray<int32> OnRig;
        for (int32 I = 0; Rig && !IsLiftDriven(Rig) && I < Doors.Num(); ++I)
            if (Doors[I].Rig == Rig) OnRig.AddUnique(I);
        if (OnRig.Num() == 1) { Out.Via = TEXT("rig"); return OnRig[0]; } // the entity is the interaction (a door, a seat)
        if (OnRig.Num() > 1)
        {
            // Several fragments drive one rig (root-entity panels): the one whose driven joint carries
            // the hit part, else the driven joint nearest to the hit within reach.
            const auto& Ref = Rig->GetSkeletalMeshAsset()->GetRefSkeleton();
            for (int32 Bone = Ref.FindBoneIndex(Socket); Bone != INDEX_NONE; Bone = Ref.GetParentIndex(Bone))
                for (int32 I : OnRig)
                    if (Ref.GetBoneName(Bone) == Doors[I].Joint) { Out.Via = TEXT("rig"); return I; }
            int32 Best = INDEX_NONE;
            double Distance = FMath::Square(Reach);
            for (int32 I : OnRig)
            {
                const double Candidate = FVector::DistSquared(Hit.ImpactPoint, Rig->GetSocketLocation(Doors[I].Joint));
                if (Candidate <= Distance) { Distance = Candidate; Best = I; }
            }
            if (Best != INDEX_NONE) { Out.Via = TEXT("rig"); return Best; }
        }
    }
    const int32 Nearest = NearestDoor(Eye, Reach);
    if (Nearest != INDEX_NONE) Out.Via = TEXT("nearest");
    return Nearest;
}
bool ASCVehicle::IsDoorOpen(int32 Index) const
{
    return Doors.IsValidIndex(Index) && FragmentTargets.FindRef({Doors[Index].Rig.Get(), Doors[Index].Fragment}) == Doors[Index].OpenTag;
}
bool ASCVehicle::IsDoorPlaying(int32 Index) const
{
    if (!Doors.IsValidIndex(Index) || !Doors[Index].Rig || !ClipCatalog) return false;
    const FSCDoorInteraction& D = Doors[Index];
    return Playback.ContainsByPredicate([&](const FSCClipPlayback& P) {
        return AppliesTo(P, D.Rig) && ClipCatalog->Clips.IsValidIndex(P.Clip) &&
               ClipCatalog->Clips[P.Clip].Bindings.ContainsByPredicate([&](const FSCClipBinding& B) { return B.Mesh == D.Rig->GetSkeletalMeshAsset(); }) &&
               ClipCatalog->Clips[P.Clip].Fragments.ContainsByPredicate([&](const FSCClipFragment& F) { return F.Fragment == D.Fragment; });
    });
}
bool ASCVehicle::ToggleDoor(int32 Index)
{
    if (!Doors.IsValidIndex(Index)) return false;
    const auto& D = Doors[Index];
    // The press is the door's own (its DoorAnimationSpeed), from its current pose, and goes on along its port links.
    if (!OrderShipDoor(Index, IsDoorOpen(Index) ? ESCShipDoorAction::Close : ESCShipDoorAction::Open, ShipLinkPlayer()).IsEmpty()) return false;
    UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_DOOR fragment=%s owner=%s joint=%s open=%d label=\"%s\""), *D.Fragment.ToString(),
           D.Rig ? *D.Rig->OwnerEntity : TEXT("-"), *D.Joint.ToString(), IsDoorOpen(Index), *D.DisplayName);
    return true;
}

int32 ASCVehicle::DoorForRig(const USCRigComponent* Rig) const
{
    for (int32 I = 0; Rig && I < Doors.Num(); ++I)
        if (Doors[I].Rig == Rig) return I;
    return INDEX_NONE;
}
void ASCVehicle::UpdateDoorSensors(const FVector& PlayerWorld, double Now)
{
    for (const FSCDoorSensor& Sensor : DoorSensors)
    {
        const USceneComponent* Anchor = Sensor.Anchor.Get();
        if (!Anchor || !Sensor.Box.IsInsideOrOn(Anchor->GetComponentTransform().InverseTransformPosition(PlayerWorld))) continue;
        for (const auto& Rig : Sensor.Doors)
        {
            const int32 Door = DoorForRig(Rig.Get());
            // A lift's doors open and close with its carriage (WP-P5): a sensor reopening one would undo the handshake.
            if (Door == INDEX_NONE || IsLiftDriven(Rig.Get())) continue;
            FSensorDoorState& State = SensorDoorStates.FindOrAdd(Door);
            State.LastInside = Now;
            State.ExitDelay = FMath::Max(State.ExitDelay, Sensor.ExitDelay);
            if (!IsDoorOpen(Door) && OrderShipDoor(Door, ESCShipDoorAction::Open, ShipLinkPlayer(), PlayerWorld).IsEmpty()) State.bOpenedBySensor = true;
        }
    }
    for (auto& [Door, State] : SensorDoorStates)
        if (State.bOpenedBySensor && State.LastInside < Now && Now - State.LastInside > State.ExitDelay)
        {
            if (IsDoorOpen(Door)) ToggleDoor(Door);
            State.bOpenedBySensor = false;
        }
}

bool ASCVehicle::SetPaint(int32 Index)
{
    if (Index != INDEX_NONE && !Manifest.Paints.IsValidIndex(Index)) return false;
    // A prepared (cooked) vehicle keeps no paint slots: its paint was baked in by the prep step (a scenario's skin).
    if (PaintSlots.IsEmpty() && !Manifest.Paints.IsEmpty())
    {
        UE_LOG(LogTemp, Warning, TEXT("PAINT index=%d refused: the vehicle has no paint slots (a prepared vehicle carries the paint it was prepared with)"),
               Index);
        return false;
    }
    int32 Swapped = 0;
    PaintColorOverrides.Reset(); // custom colours belong to the paint they were set on (SetPaintColors)
    CustomPaintMaterials.Reset();
    for (const FSCPaintSlot& Entry : PaintSlots)
    {
        UMeshComponent* C = Entry.Component.Get();
        if (!C) continue;
        UMaterialInterface* Material = Index == INDEX_NONE ? Entry.Default.Get() : Entry.Paints.IsValidIndex(Index) ? Entry.Paints[Index].Get() : nullptr;
        if (!Material) Material = Entry.Default.Get(); // the paint leaves this slot as authored
        if (Material && C->GetMaterial(Entry.Slot) != Material) { C->SetMaterial(Entry.Slot, Material); ++Swapped; }
    }
    ActivePaint = Index;
    UE_LOG(LogTemp, Display, TEXT("PAINT index=%d label=\"%s\" palette=%s slots=%d swapped=%d"), Index,
           Index == INDEX_NONE ? TEXT("default") : *Manifest.Paints[Index].Label(), Index == INDEX_NONE ? *Manifest.Root.PaletteId : *Manifest.Paints[Index].PaletteId,
           PaintSlots.Num(), Swapped);
    return true;
}
