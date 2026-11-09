// Spawn categories come from DataCore, interaction semantics from Mannequin and receiving joints.
#include "SCExploration.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Components/PrimitiveComponent.h"
#include "Misc/Paths.h"

void ASCVehicle::AddSpawnPoint(FName Kind, const FString& Label, const FTransform& WorldTransform)
{
    FSCSpawnPoint Point;
    Point.Kind = Kind;
    Point.DisplayName = Label;
    Point.Transform = WorldTransform.GetRelativeTransform(GetActorTransform());
    SpawnPoints.Add(Point);
}
void ASCVehicle::AddItemSpawn(const FSCSceneInstance& Instance, USceneComponent* Anchor)
{
    const FString& Type = Instance.ItemType;
    if (Type.Equals(TEXT("Seat"), ESearchCase::IgnoreCase))
        AddSpawnPoint(TEXT("Seat"), Instance.EntityName, Anchor->GetComponentTransform());
    else if (Type.Equals(TEXT("SeatAccess"), ESearchCase::IgnoreCase))
        AddSpawnPoint(TEXT("SeatAccess"), Instance.EntityName, Anchor->GetComponentTransform());
    else if (Type.Equals(TEXT("Door"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("Entrance"), ESearchCase::IgnoreCase))
        AddSpawnPoint(TEXT("Entrance"), Instance.EntityName, Anchor->GetComponentTransform());
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
                if (!HasClose) continue;
                // Fragment identifiers are semantic controller categories, never geometry/clip filenames.
                const FString Semantic = Fragment.Fragment.ToString().ToLower();
                const bool DoorFragment = Semantic.Contains(TEXT("door")) || Semantic.Contains(TEXT("canopy")) ||
                    Semantic.Contains(TEXT("hatch")) || Semantic.Contains(TEXT("entrance")) || Semantic.Contains(TEXT("ramp"));
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
                    if (!DoorFragment && (!Instance || !Instance->ItemType.Equals(TEXT("Door"), ESearchCase::IgnoreCase))) continue;
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
                        AddSpawnPoint(TEXT("Entrance"), Door.DisplayName, Rig->GetSocketTransform(Joint));
                        break;
                    }
                }
            }
    const FBox Bounds = ExplorationBounds();
    if (Bounds.IsValid)
        AddSpawnPoint(TEXT("Hull"), TEXT("Above hull centre"), FTransform(FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.Max.Z)));
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
bool ASCVehicle::ToggleDoor(int32 Index)
{
    if (!Doors.IsValidIndex(Index)) return false;
    auto& D = Doors[Index];
    const FName Tag = D.bOpen ? TEXT("Close") : TEXT("Open");
    if (!PlayFragmentAt(D.Fragment, Tag, D.Rig)) return false;
    D.bOpen = !D.bOpen;
    UE_LOG(LogTemp, Display, TEXT("FIRST_PERSON_DOOR fragment=%s joint=%s open=%d"), *D.Fragment.ToString(), *D.Joint.ToString(), D.bOpen);
    return true;
}
