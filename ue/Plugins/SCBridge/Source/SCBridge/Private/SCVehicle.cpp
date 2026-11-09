#include "SCVehicle.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "SCMaterialBuilder.h"
ASCVehicle::ASCVehicle()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("VehicleRoot"));
    RootComponent->SetMobility(EComponentMobility::Movable);
}
void ASCVehicle::SetEngineGlow(float Strength)
{
    const float Value = FSCMaterialBuilder::EngineGlowToEmissive(Manifest.EngineGlow, Strength);
    for (UMaterialInstanceDynamic* MID : EngineGlowMaterials) if (MID) MID->SetScalarParameterValue(TEXT("EmissiveStrength"), Value);
}
