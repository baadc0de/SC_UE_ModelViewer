// ASCCargoContainer: occupancy box body, shared package meshes, carried (simulated) / locked (attached) states.
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoGridComponent.h"
#include "SCVehicle.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

ASCCargoContainer::ASCCargoContainer()
{
    PrimaryActorTick.bCanEverTick = false;
    Body = CreateDefaultSubobject<UBoxComponent>(TEXT("Body"));
    Body->SetMobility(EComponentMobility::Movable);
    Body->SetCollisionProfileName(TEXT("PhysicsActor"));
    Body->SetLinearDamping(0.2f);
    Body->SetAngularDamping(2.0f);
    Body->SetHiddenInGame(true);
    RootComponent = Body;
}

void ASCCargoContainer::SetOccupancy(const FVector& SizeCm, double MassKg)
{
    Body->SetBoxExtent(SizeCm / 2, true);
    if (MassKg > 0) Body->SetMassOverrideInKg(NAME_None, float(MassKg), true);
}

FVector ASCCargoContainer::SizeCm() const { return Body->GetUnscaledBoxExtent() * 2; }

FBox ASCCargoContainer::Bounds() const { return Body->Bounds.GetBox(); }

void ASCCargoContainer::SetVisuals(const ASCVehicle* Template, const FVector& EntityOriginCm)
{
    for (UStaticMeshComponent* V : Visuals)
        if (V) V->DestroyComponent();
    Visuals.Reset();
    const FVector Size = SizeCm();
    struct FPart { UStaticMesh* Mesh; TArray<UMaterialInterface*> Materials; FTransform Relative; };
    TArray<FPart> Parts;
    FBox Extent(ForceInit);
    if (Template)
    {
        const FTransform Root = Template->GetActorTransform();
        Template->ForEachComponent<UStaticMeshComponent>(false, [&](const UStaticMeshComponent* C) {
            if (!C->GetStaticMesh() || C->IsA<UInstancedStaticMeshComponent>() || !C->IsVisible() || C->ComponentHasTag(TEXT("SCGameCollision"))) return;
            FPart P{C->GetStaticMesh(), {}, C->GetComponentTransform().GetRelativeTransform(Root)};
            for (int32 I = 0; I < C->GetNumMaterials(); ++I) P.Materials.Add(C->GetMaterial(I));
            Extent += P.Mesh->GetBoundingBox().TransformBy(P.Relative);
            Parts.Add(MoveTemp(P));
        });
    }
    const bool bPackage = !Parts.IsEmpty();
    if (!bPackage)
    {
        // No package meshes: the occupancy itself, drawn as a cube.
        if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
        {
            Parts.Add({Cube, {}, FTransform(FQuat::Identity, FVector::ZeroVector, Size / 100.0)});
            Extent = Cube->GetBoundingBox().TransformBy(Parts[0].Relative);
        }
    }
    // Package meshes are in the item's frame, placed where the occupancy's local bounds put it; the cube is centred.
    const FVector Offset = bPackage ? EntityOriginCm : Extent.IsValid ? -Extent.GetCenter() : FVector::ZeroVector;
    for (const FPart& P : Parts)
    {
        auto* C = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
        C->SetMobility(EComponentMobility::Movable);
        C->SetStaticMesh(P.Mesh);
        for (int32 I = 0; I < P.Materials.Num(); ++I) C->SetMaterial(I, P.Materials[I]);
        C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        C->SetupAttachment(Body);
        C->SetRelativeTransform(P.Relative * FTransform(Offset));
        AddInstanceComponent(C);
        C->RegisterComponent();
        Visuals.Add(C);
    }
}

void ASCCargoContainer::Lock(USCCargoGridComponent* Grid, const FTransform& World)
{
    Body->SetSimulatePhysics(false);
    Body->SetPhysicsLinearVelocity(FVector::ZeroVector);
    Body->SetPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
    SetActorTransform(World, false, nullptr, ETeleportType::TeleportPhysics);
    if (Grid) AttachToComponent(Grid, FAttachmentTransformRules::KeepWorldTransform);
}

void ASCCargoContainer::Release()
{
    DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    Body->SetSimulatePhysics(true);
    Body->WakeAllRigidBodies();
}
