// FSCInstancing: repeated rigid parts -> instanced static mesh components (one primitive per group).
#include "SCInstancing.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

namespace
{
// Glowing parts stay individual: the emissive pass (SCEmissiveLights) builds per-component Lumen proxies and lights.
bool Emissive(const UStaticMeshComponent* C)
{
    for (int32 I = 0; I < C->GetNumMaterials(); ++I)
    {
        float Strength = 0;
        FLinearColor Colour = FLinearColor::Black;
        const UMaterialInterface* M = C->GetMaterial(I);
        if (M && M->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("EmissiveStrength"))), Strength) &&
            M->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(TEXT("EmissiveColor"))), Colour) && Strength * Colour.GetLuminance() > 0)
            return true;
    }
    return false;
}
bool UnderRig(const USceneComponent* C)
{
    for (const USceneComponent* P = C->GetAttachParent(); P; P = P->GetAttachParent())
        if (P->IsA<USCRigComponent>()) return true;
    return false;
}
// Everything that makes two parts render, collide and light identically, except their transforms.
bool SameSetup(const UStaticMeshComponent* A, const UStaticMeshComponent* B)
{
    if (A->GetStaticMesh() != B->GetStaticMesh() || A->GetNumMaterials() != B->GetNumMaterials()) return false;
    for (int32 I = 0; I < A->GetNumMaterials(); ++I)
        if (A->GetMaterial(I) != B->GetMaterial(I)) return false;
    return A->IsVisible() == B->IsVisible() && A->bHiddenInGame == B->bHiddenInGame && A->CastShadow == B->CastShadow &&
           A->bAffectDistanceFieldLighting == B->bAffectDistanceFieldLighting &&
           A->bAffectDynamicIndirectLighting == B->bAffectDynamicIndirectLighting && A->bVisibleInRayTracing == B->bVisibleInRayTracing &&
           A->LightingChannels.bChannel0 == B->LightingChannels.bChannel0 && A->LightingChannels.bChannel1 == B->LightingChannels.bChannel1 &&
           A->LightingChannels.bChannel2 == B->LightingChannels.bChannel2 && A->GetCollisionEnabled() == B->GetCollisionEnabled() &&
           A->GetCollisionProfileName() == B->GetCollisionProfileName() &&
           A->GetCollisionResponseToChannels() == B->GetCollisionResponseToChannels() &&
           A->GetCustomPrimitiveData().Data == B->GetCustomPrimitiveData().Data && A->ComponentTags == B->ComponentTags;
}
uint32 SetupHash(const UStaticMeshComponent* C)
{
    uint32 H = GetTypeHash(C->GetStaticMesh());
    for (int32 I = 0; I < C->GetNumMaterials(); ++I) H = HashCombineFast(H, GetTypeHash(C->GetMaterial(I)));
    return HashCombineFast(H, (uint32(C->CastShadow) << 1) | uint32(C->IsVisible()));
}
} // namespace

FSCInstancingStats FSCInstancing::Consolidate(ASCVehicle* Vehicle)
{
    FSCInstancingStats Stats;
    if (!Vehicle || !Vehicle->GetRootComponent()) return Stats;
    TArray<UStaticMeshComponent*> Parts;
    Vehicle->GetComponents(Parts);
    TMap<uint32, TArray<TArray<UStaticMeshComponent*>>> Groups;
    for (UStaticMeshComponent* C : Parts)
    {
        // Exactly a plain component (not an instanced one or a subclass), registered, rigid, and a leaf.
        if (!C || C->GetClass() != UStaticMeshComponent::StaticClass() || !C->IsRegistered() || !C->GetStaticMesh() ||
            !C->GetAttachChildren().IsEmpty() || UnderRig(C) || Emissive(C))
            continue;
        ++Stats.Components;
        TArray<TArray<UStaticMeshComponent*>>& Bucket = Groups.FindOrAdd(SetupHash(C));
        TArray<UStaticMeshComponent*>* Group = Bucket.FindByPredicate([C](const TArray<UStaticMeshComponent*>& G) { return SameSetup(G[0], C); });
        if (Group) Group->Add(C);
        else Bucket.Add({C});
    }
    const FTransform Root = Vehicle->GetRootComponent()->GetComponentTransform();
    for (auto& Pair : Groups)
        for (TArray<UStaticMeshComponent*>& Group : Pair.Value)
        {
            if (Group.Num() < 2) continue;
            const UStaticMeshComponent* First = Group[0];
            auto* ISM = NewObject<UInstancedStaticMeshComponent>(
                Vehicle, MakeUniqueObjectName(Vehicle, UInstancedStaticMeshComponent::StaticClass(), TEXT("Instanced")), RF_Transient);
            Vehicle->AddInstanceComponent(ISM);
            ISM->SetMobility(EComponentMobility::Movable); // the environment actor is moved onto the map floor after import
            ISM->SetupAttachment(Vehicle->GetRootComponent());
            ISM->SetStaticMesh(First->GetStaticMesh());
            for (int32 I = 0; I < First->GetNumMaterials(); ++I) ISM->SetMaterial(I, First->GetMaterial(I));
            ISM->SetVisibility(First->IsVisible());
            ISM->SetHiddenInGame(First->bHiddenInGame);
            ISM->CastShadow = First->CastShadow;
            ISM->bAffectDistanceFieldLighting = First->bAffectDistanceFieldLighting;
            ISM->bAffectDynamicIndirectLighting = First->bAffectDynamicIndirectLighting;
            ISM->bVisibleInRayTracing = First->bVisibleInRayTracing;
            ISM->LightingChannels = First->LightingChannels;
            ISM->ComponentTags = First->ComponentTags;
            ISM->SetCollisionProfileName(First->GetCollisionProfileName());
            ISM->SetCollisionEnabled(First->GetCollisionEnabled());
            ISM->SetCollisionResponseToChannels(First->GetCollisionResponseToChannels());
            const TArray<float>& Data = First->GetCustomPrimitiveData().Data;
            for (int32 I = 0; I < Data.Num(); ++I) ISM->SetCustomPrimitiveDataFloat(I, Data[I]);
            TArray<FTransform> Instances;
            Instances.Reserve(Group.Num());
            for (const UStaticMeshComponent* C : Group) Instances.Add(C->GetComponentTransform().GetRelativeTransform(Root));
            ISM->RegisterComponent();
            ISM->AddInstances(Instances, false, false);
            for (UStaticMeshComponent* C : Group) C->DestroyComponent();
            Stats.Consolidated += Group.Num();
            ++Stats.Instanced;
        }
    return Stats;
}
