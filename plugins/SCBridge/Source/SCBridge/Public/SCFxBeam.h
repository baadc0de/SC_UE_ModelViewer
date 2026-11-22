// A guided beam's length at run time (WP-K4c, SCFxBuild runtime contract): SC.BeamLength / SC.BeamLength2. The beam
// emitters' spawn rates are expressions of SC.BeamLength in the built system (WP-E1 E1-6: the fork evaluates spawn-rate
// expressions; before it, the runtime set each beam emitter's SC.BR<i>), so the beam holds the game's EmitPerDistance at any
// length. Shared by the drones' beams and sc.Fx.Spawn.
#pragma once
#include "CoreMinimal.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

namespace SCFxBeam
{
/** Sets the beam's length (cm). */
inline void SetLength(UNiagaraComponent* C, float LengthCm)
{
    if (!C) return;
    C->SetVariableFloat(TEXT("SC.BeamLength"), LengthCm);
    C->SetVariableVec2(TEXT("SC.BeamLength2"), FVector2D(LengthCm, LengthCm));
}
} // namespace SCFxBeam
