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

bool FSCInstancing::UnderRig(const USceneComponent* C)
{
    // A rig's parts follow its bones, a lift's parts its moving anchor (WP-P5, tag SCLift): neither can be an instance of a
    // component fixed to the vehicle root.
    for (const USceneComponent* P = C->GetAttachParent(); P; P = P->GetAttachParent())
        if (P->IsA<USCRigComponent>() || P->ComponentHasTag(TEXT("SCLift"))) return true;
    return false;
}

bool FSCInstancing::UnderInteraction(const ASCVehicle* Vehicle, const USceneComponent* C)
{
    // A press finds its panel by walking up from the part it hits (FindLiftPanel, FindInteraction, the Garage kiosks): a
    // part under a lift panel's, a door pad's or a kiosk's anchor must stay a component there.
    for (const USceneComponent* P = C->GetAttachParent(); Vehicle && P; P = P->GetAttachParent())
    {
        USceneComponent* Key = const_cast<USceneComponent*>(P);
        if (Vehicle->LiftPanels.Contains(Key) || Vehicle->DoorControls.Contains(Key) || Vehicle->GarageKiosks.Contains(Key)) return true;
    }
    return false;
}

FSCInstancingStats FSCInstancing::Consolidate(ASCVehicle* Vehicle, TConstArrayView<UStaticMeshComponent*> Deferred)
{
    FSCInstancingStats Stats;
    if (!Vehicle || !Vehicle->GetRootComponent()) return Stats;
    TArray<UStaticMeshComponent*> Parts;
    Vehicle->GetComponents(Parts);
    const TSet<UStaticMeshComponent*> Pending(Deferred);
    TMap<uint32, TArray<TArray<UStaticMeshComponent*>>> Groups;
    for (UStaticMeshComponent* C : Parts)
    {
        // Only plain, rigid leaf components. An import can supply fully configured pieces before registration.
        if (!C || C->GetClass() != UStaticMeshComponent::StaticClass() || (!C->IsRegistered() && !Pending.Contains(C)) || !C->GetStaticMesh() ||
            !C->GetAttachChildren().IsEmpty() || FSCInstancing::UnderRig(C) || FSCInstancing::UnderInteraction(Vehicle, C) || Emissive(C) ||
            C->ComponentHasTag(FSCInstancing::KeepTag))
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
            ISM->AddInstances(Instances, false, false);
            ISM->RegisterComponent();
            for (UStaticMeshComponent* C : Group) C->DestroyComponent();
            Stats.Consolidated += Group.Num();
            ++Stats.Instanced;
        }
    return Stats;
}
