#include "SCVehicle.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
ASCVehicle::ASCVehicle()
{
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("VehicleRoot"));
    RootComponent->SetMobility(EComponentMobility::Movable);
}
void ASCVehicle::SetEngineGlow(float Strength)
{
    const FSCEngineGlow& Glow = Manifest.EngineGlow;
    const float Value = Glow.MaxStrength > Glow.MinStrength ? FMath::Clamp(Strength, Glow.MinStrength, Glow.MaxStrength) : Strength;
    for (UMaterialInstanceDynamic* MID : EngineGlowMaterials) if (MID) MID->SetScalarParameterValue(TEXT("EmissiveStrength"), Value);
}
