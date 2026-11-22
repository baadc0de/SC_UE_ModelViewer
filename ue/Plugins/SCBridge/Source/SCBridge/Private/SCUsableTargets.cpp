// WP-I1-4: placed usables (beds, sofas, chairs: `usable_interactions[]`) under the crosshair. They are registered at import
// by their anchor (SCImportJob); the view ray finds one by the geometry it hits, as lift panels are found.
#include "SCVehicle.h"
#include "Engine/World.h"

const FSCUsableTarget* ASCVehicle::FindUsableTarget(FVector Eye, FVector Direction, float Reach, const AActor* Ignore,
                                               FVector* OutPoint) const
{
    if (UsableTargets.IsEmpty() || !GetWorld()) return nullptr;
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCUsableTarget), true, Ignore);
    if (!GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Direction.GetSafeNormal() * Reach, ECC_Visibility, Params) || Hit.GetActor() != this)
        return nullptr;
    for (const USceneComponent* C = Hit.GetComponent(); C; C = C->GetAttachParent())
        if (const FSCUsableTarget* Target = UsableTargets.Find(C))
        {
            if (OutPoint) *OutPoint = Hit.ImpactPoint;
            return Target;
        }
    return nullptr;
}
