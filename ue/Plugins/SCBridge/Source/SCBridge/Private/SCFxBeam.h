// A guided beam's length at run time (WP-K4c, SCFxBuild runtime contract): SC.BeamLength / SC.BeamLength2, and each beam
// emitter's spawn rate SC.BR<i> = SC.BK<i> (its particles per cm of beam per second, a default of the built system) x the
// length, so the beam holds the game's EmitPerDistance at any length. Shared by the drones' beams and sc.Fx.Spawn.
#pragma once
#include "CoreMinimal.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

namespace SCFxBeam
{
struct FRate
{
    FName Rate; // SC.BR<i> (as SetVariable takes it: without "User.")
    float PerCm = 0;
};

/** The beam emitters' rates a system exposes (read once per system from its user parameter defaults). */
inline const TArray<FRate>& Rates(const UNiagaraSystem* System)
{
    static TMap<TObjectKey<UNiagaraSystem>, TArray<FRate>> Cache;
    if (const TArray<FRate>* Known = Cache.Find(System)) return *Known;
    TArray<FRate>& Out = Cache.Add(System);
    if (!System) return Out;
    const FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
    TArray<FNiagaraVariable> Vars;
    Store.GetParameters(Vars);
    for (int32 i = 0;; ++i)
    {
        const FName KName(*FString::Printf(TEXT("User.SC.BK%d"), i));
        const FNiagaraVariable* K = Vars.FindByPredicate([&](const FNiagaraVariable& X) { return X.GetName() == KName; });
        if (!K) break;
        Out.Add({FName(*FString::Printf(TEXT("SC.BR%d"), i)), Store.GetParameterValue<float>(*K)});
    }
    return Out;
}

/** Sets the beam's length (cm) and the spawn rates that follow it. */
inline void SetLength(UNiagaraComponent* C, float LengthCm)
{
    if (!C) return;
    C->SetVariableFloat(TEXT("SC.BeamLength"), LengthCm);
    C->SetVariableVec2(TEXT("SC.BeamLength2"), FVector2D(LengthCm, LengthCm));
    for (const FRate& R : Rates(C->GetAsset())) C->SetVariableFloat(R.Rate, R.PerCm * LengthCm);
}
} // namespace SCFxBeam
