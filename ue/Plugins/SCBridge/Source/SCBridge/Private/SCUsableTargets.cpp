// WP-I1-4: placed usables (beds, sofas, chairs: `usable_interactions[]`) under the crosshair. They are registered at import
// by their anchor with their interaction points (SCImportJob); the view ray finds one by the geometry it hits, as lift
// panels are found, and the point the player uses is the nearest one in reach of where the ray hits.
#include "SCVehicle.h"
#include "Engine/World.h"

const FSCUsableTarget* ASCVehicle::FindUsableTarget(FVector Eye, FVector Direction, float Reach, const AActor* Ignore, int32* OutPoint) const
{
    if (OutPoint) *OutPoint = INDEX_NONE;
    if (UsableTargets.IsEmpty() || !GetWorld()) return nullptr;
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCUsableTarget), true, Ignore);
    if (!GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Direction.GetSafeNormal() * Reach, ECC_Visibility, Params) || Hit.GetActor() != this)
        return nullptr;
    for (const USceneComponent* C = Hit.GetComponent(); C; C = C->GetAttachParent())
        if (const FSCUsableTarget* Target = UsableTargets.Find(C))
        {
            // The point: located ones within their radius of the eye, nearest the hit; none located: the first.
            int32 Best = INDEX_NONE;
            double BestDistance = UE_BIG_NUMBER;
            bool bAnyLocated = false;
            for (int32 I = 0; I < Target->Points.Num(); ++I)
            {
                const FSCUsablePoint& P = Target->Points[I];
                if (!P.bLocated) continue;
                bAnyLocated = true;
                const FVector World = C->GetComponentTransform().TransformPosition(P.Local);
                if (P.RadiusCm > 0 && FVector::Dist(Eye, World) > P.RadiusCm) continue;
                // The point's `AngleConstraint.viewAngleLimit` (as the seat accesses read it): within half of it of the view.
                if (P.ViewAngleLimit > 0 && P.ViewAngleLimit < 180 &&
                    FVector::DotProduct(Direction.GetSafeNormal(), (World - Eye).GetSafeNormal()) < FMath::Cos(FMath::DegreesToRadians(P.ViewAngleLimit / 2)))
                    continue;
                if (const double D = FVector::Dist(Hit.ImpactPoint, World); D < BestDistance)
                {
                    Best = I;
                    BestDistance = D;
                }
            }
            if (!bAnyLocated && !Target->Points.IsEmpty()) Best = 0;
            if (Best == INDEX_NONE) return nullptr;
            if (OutPoint) *OutPoint = Best;
            return Target;
        }
    return nullptr;
}
