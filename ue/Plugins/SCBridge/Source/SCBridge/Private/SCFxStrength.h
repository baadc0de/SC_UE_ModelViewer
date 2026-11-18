// The game effects' strength at run time (WP-K4c, SCFxBuild runtime contract): SC.Strength. A built system evaluates its
// EmitterStrength curves itself, as stateless expressions in SC.Strength (WP-E1 E1-6: the fork's clamp expression makes the
// curves exact; before it, the runtime evaluated them into bound SC.SV<i> floats). Shared by the drones' beams and sc.Fx.Spawn.
#pragma once
#include "CoreMinimal.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

namespace SCFxStrength
{
/** Set the component's SC.Strength (0..1). */
inline void Set(UNiagaraComponent* Component, float Strength)
{
    if (!Component) return;
    Component->SetVariableFloat(TEXT("SC.Strength"), Strength);
}
} // namespace SCFxStrength
