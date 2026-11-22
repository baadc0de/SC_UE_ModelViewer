// WP-M2 data the shared plugin keeps (WP-O1-2): which drawn panels are the hangar's vehicle kiosks (set at import,
// ASCVehicle::GarageKiosks) and the aim at one. What a kiosk does is the application's (UnrealCitizen: the Garage app).
#include "SCVehicle.h"
#include "SCPackageReader.h"
#include "Engine/World.h"

bool ASCVehicle::IsGarageKiosk(const FSCUiBinding& Binding)
{
    return Binding.IsOperatingPanel() && Binding.bOperated &&
        Binding.Buttons.ContainsByPredicate([](const FSCUiButton& B) { return B.Interaction == FName(TEXT("UseConsole")); }) &&
        Binding.StateTypes.ContainsByPredicate([](const FSCUiStateType& S) {
            return S.States.Contains(TEXT("VehicleList")) && S.States.Contains(TEXT("RetrievingVehicle"));
        });
}

bool ASCVehicle::AimGarageKiosk(const FVector& Eye, const FVector& Direction, float Reach, const AActor* Ignore) const
{
    if (GarageKiosks.IsEmpty() || !GetWorld()) return false;
    float Longest = Reach;
    for (const auto& Entry : GarageKiosks) Longest = FMath::Max(Longest, Entry.Value);
    FHitResult Hit;
    const FCollisionQueryParams Params(SCENE_QUERY_STAT(SCGarageKiosk), true, Ignore);
    if (!GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Direction.GetSafeNormal() * Longest, ECC_Visibility, Params) || Hit.GetActor() != this)
        return false;
    for (USceneComponent* Component = Hit.GetComponent(); Component; Component = Component->GetAttachParent())
        if (const float* KioskReach = GarageKiosks.Find(Component))
            return Hit.Distance <= FMath::Max(Reach, *KioskReach);
    return false;
}
