// Spawn categories come from DataCore, interaction semantics from Mannequin and receiving joints.
#include "SCExploration.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Components/PrimitiveComponent.h"
#include "Misc/Paths.h"

const FName FSCSpawnKind::Entrance(TEXT("Entrance"));
const FName FSCSpawnKind::SeatAccess(TEXT("SeatAccess"));
const FName FSCSpawnKind::Seat(TEXT("Seat"));
const FName FSCSpawnKind::Interior(TEXT("Interior"));
const FName FSCSpawnKind::Door(TEXT("Door"));
const FName FSCSpawnKind::Hull(TEXT("Hull"));
const FName FSCSpawnKind::Selected(TEXT("Selected"));
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
            FBox Box(ForceInit);
            for (const USceneComponent* Part : Parts)
                if (const auto* Primitive = Cast<UPrimitiveComponent>(Part); Primitive && Primitive->IsRegistered() && Primitive->IsCollisionEnabled())
                    Box += Primitive->Bounds.GetBox();
            if (Box.IsValid) World.SetLocation(FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z));
            Point.Transform = World.GetRelativeTransform(GetActorTransform());
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
    if (ClipCatalog)
        for (const auto& Clip : ClipCatalog->Clips)
            for (const auto& Fragment : Clip.Fragments)
            {
                if (!Fragment.FragmentTags.Contains(TEXT("Open")) && !Fragment.Tags.Contains(TEXT("Open"))) continue;
                const bool HasClose = ClipCatalog->Clips.ContainsByPredicate([&](const FSCImportedClip& Other) {
                    return Other.Fragments.ContainsByPredicate([&](const FSCClipFragment& F) {
                        return F.Fragment == Fragment.Fragment && (F.FragmentTags.Contains(TEXT("Close")) || F.Tags.Contains(TEXT("Close")));
                    });
                });
                // The category is the Mannequin Open/Close tag pair on a rig the clip binds; no
                // fragment or entity vocabulary.
                if (!HasClose) continue;
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
                        const FString Label = Instance ? Instance->EntityName :
                            Clip.Owner.Contains(TEXT("/")) ? FPaths::GetBaseFilename(Clip.Owner) : Joint.ToString();
                        Door.DisplayName = Label.Replace(TEXT("_"), TEXT(" "));
                        Doors.Add(Door);
                        AddSpawnPoint(FSCSpawnKind::Door, Door.DisplayName, Rig->GetSocketTransform(Joint));
                        UE_LOG(LogTemp, Display, TEXT("EXPLORATION_INTERACTION fragment=%s owner=%s item_type=%s joint=%s label=\"%s\""),
                               *Fragment.Fragment.ToString(), *Rig->OwnerEntity, Instance ? *Instance->ItemType : TEXT("-"),
                               *Joint.ToString(), *Door.DisplayName);
                        break;
                    }
                }
            }
    const FBox Bounds = ExplorationBounds();
    if (Bounds.IsValid)
        AddSpawnPoint(FSCSpawnKind::Hull, TEXT("Above hull centre"), FTransform(FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.Max.Z)));
    UE_LOG(LogTemp, Display, TEXT("EXPLORATION spawns=%d doors=%d collision_ready=%d"), SpawnPoints.Num(), Doors.Num(), bCollisionReady);
}
int32 ASCVehicle::NearestDoor(FVector Position, float Reach) const
{
    int32 Best = INDEX_NONE;
    double Distance = FMath::Square(Reach);
    for (int32 I = 0; I < Doors.Num(); ++I)
    {
        const auto& D = Doors[I];
        if (!D.Rig) continue;
        const double Candidate = FVector::DistSquared(Position, D.Rig->GetSocketLocation(D.Joint));
        if (Candidate <= Distance) { Distance = Candidate; Best = I; }
    }
    return Best;
}
int32 ASCVehicle::FindInteraction(FVector Eye, FVector Direction, float Reach, const AActor* Ignore) const
{
    // What the crosshair is on (the ray itself bounds the reach): the nearest rig owning the hit part.
    // Doors are large and their driven joints (hinges, hydraulics) can sit metres from the surface the
    // player faces, so joint distance alone misses them.
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCInteract), true, Ignore);
    if (GetWorld() && GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Direction.GetSafeNormal() * Reach, ECC_Visibility, Params) &&
        Hit.GetActor() == this)
    {
        FName Socket = NAME_None;
        const USCRigComponent* Rig = nullptr;
        for (const USceneComponent* C = Hit.GetComponent(); C && !Rig; C = C->GetAttachParent())
        {
            Rig = Cast<USCRigComponent>(C);
            if (!Rig) Socket = C->GetAttachSocketName();
        }
        TArray<int32> OnRig;
        for (int32 I = 0; Rig && I < Doors.Num(); ++I)
            if (Doors[I].Rig == Rig) OnRig.AddUnique(I);
        if (OnRig.Num() == 1) return OnRig[0]; // the entity is the interaction (a door, a seat)
        if (OnRig.Num() > 1)
        {
            // Several fragments drive one rig (root-entity panels): the one whose driven joint carries
            // the hit part, else the driven joint nearest to the hit within reach.
            const auto& Ref = Rig->GetSkeletalMeshAsset()->GetRefSkeleton();
            for (int32 Bone = Ref.FindBoneIndex(Socket); Bone != INDEX_NONE; Bone = Ref.GetParentIndex(Bone))
                for (int32 I : OnRig)
                    if (Ref.GetBoneName(Bone) == Doors[I].Joint) return I;
            int32 Best = INDEX_NONE;
            double Distance = FMath::Square(Reach);
            for (int32 I : OnRig)
            {
                const double Candidate = FVector::DistSquared(Hit.ImpactPoint, Rig->GetSocketLocation(Doors[I].Joint));
                if (Candidate <= Distance) { Distance = Candidate; Best = I; }
            }
            if (Best != INDEX_NONE) return Best;
        }
    }
    return NearestDoor(Eye, Reach);
}
bool ASCVehicle::IsDoorOpen(int32 Index) const
{
    return Doors.IsValidIndex(Index) && IsFragmentOpen(Doors[Index].Fragment, Doors[Index].Rig);
}
bool ASCVehicle::ToggleDoor(int32 Index)
{
    if (!Doors.IsValidIndex(Index)) return false;
    const auto& D = Doors[Index];
    const FName Tag = IsDoorOpen(Index) ? TEXT("Close") : TEXT("Open");
    if (!PlayFragmentAt(D.Fragment, Tag, D.Rig)) return false;
    UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_DOOR fragment=%s owner=%s joint=%s open=%d label=\"%s\""), *D.Fragment.ToString(),
           D.Rig ? *D.Rig->OwnerEntity : TEXT("-"), *D.Joint.ToString(), IsDoorOpen(Index), *D.DisplayName);
    return true;
}
