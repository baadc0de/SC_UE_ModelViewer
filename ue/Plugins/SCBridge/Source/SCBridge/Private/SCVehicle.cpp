#include "SCVehicle.h"
#include "Components/SceneComponent.h"
ASCVehicle::ASCVehicle()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("VehicleRoot"));
    RootComponent->SetMobility(EComponentMobility::Movable);
}
