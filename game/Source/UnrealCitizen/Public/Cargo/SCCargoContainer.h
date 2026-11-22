// A cargo box in the world (WP-K1): box collision from the item's occupancy dimensions, mass from its data, and the
// meshes of its package export (shared with every box of the same entity). Simulated while carried; attached to its
// grid with simulation off once placed. Its body's hits while carried go to the cargo subsystem (box contacts, WP-K4c).
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SCCargoContainer.generated.h"

class UBoxComponent;
class UStaticMeshComponent;
class ASCVehicle;
class USCCargoGridComponent;

UCLASS()
class UNREALCITIZEN_API ASCCargoContainer : public AActor
{
    GENERATED_BODY()
public:
    ASCCargoContainer();

    int32 BoxId = INDEX_NONE;
    UPROPERTY() TObjectPtr<UBoxComponent> Body;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Visuals;

    /** Occupancy in component axes (cm, X = SC y, Y = SC x) and mass (kg); collision and inertia follow. */
    void SetOccupancy(const FVector& SizeCm, double MassKg);
    /** Copies the visible static meshes of Template (the box package, imported once) onto the body: the item's frame
     *  sits at EntityOriginCm from the body centre (from the occupancy's local bounds); without a template, a basic cube
     *  of the occupancy size (logged by the caller). */
    void SetVisuals(const ASCVehicle* Template, const FVector& EntityOriginCm);
    /** Locked on a grid: simulation off, attached to Grid at World. */
    void Lock(USCCargoGridComponent* Grid, const FTransform& World);
    /** Carried: detached, simulated (sways and collides under the tractor beam). */
    void Release();
    FVector SizeCm() const;
    FBox Bounds() const;

protected:
    virtual void BeginPlay() override;

private:
    UFUNCTION()
    void OnBodyHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);
};
