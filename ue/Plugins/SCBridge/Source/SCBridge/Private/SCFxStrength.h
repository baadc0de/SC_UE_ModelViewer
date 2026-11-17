// The game effects' strength at run time (WP-K4c, SCFxBuild runtime contract): SC.Strength and the EmitterStrength
// curves a built system exposes as SC.SK<i>.T / .V (up to four keys: strengths, values; fewer repeat the last), each
// evaluated piecewise linear at that strength into the bound SC.SV<i>. Shared by the drones' beams and sc.Fx.Spawn.
#pragma once
#include "CoreMinimal.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"

namespace SCFxStrength
{
struct FCurve
{
    FName Value; // SC.SV<i> (as SetVariable takes it: without "User.")
    FVector4f T, V;
};

/** The curves a system exposes (read once per system from its user parameter defaults). */
inline const TArray<FCurve>& Curves(const UNiagaraSystem* System)
{
    static TMap<TObjectKey<UNiagaraSystem>, TArray<FCurve>> Cache;
    if (const TArray<FCurve>* Known = Cache.Find(System)) return *Known;
    TArray<FCurve>& Out = Cache.Add(System);
    if (!System) return Out;
    const FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
    TArray<FNiagaraVariable> Vars;
    Store.GetParameters(Vars);
    for (int32 i = 0;; ++i)
    {
        const FName TName(*FString::Printf(TEXT("User.SC.SK%d.T"), i)), VName(*FString::Printf(TEXT("User.SC.SK%d.V"), i));
        const FNiagaraVariable* TV = Vars.FindByPredicate([&](const FNiagaraVariable& X) { return X.GetName() == TName; });
        const FNiagaraVariable* VV = Vars.FindByPredicate([&](const FNiagaraVariable& X) { return X.GetName() == VName; });
        if (!TV || !VV) break;
        FCurve& C = Out.AddDefaulted_GetRef();
        C.Value = FName(*FString::Printf(TEXT("SC.SV%d"), i));
        C.T = Store.GetParameterValue<FVector4f>(*TV);
        C.V = Store.GetParameterValue<FVector4f>(*VV);
    }
    return Out;
}

/** Piecewise linear through (T[k], V[k]), constant beyond the ends. */
inline float Eval(const FCurve& C, float S)
{
    if (S <= C.T[0]) return C.V[0];
    for (int32 k = 1; k < 4; ++k)
        if (S <= C.T[k])
        {
            const float Span = C.T[k] - C.T[k - 1];
            return Span > 0 ? FMath::Lerp(C.V[k - 1], C.V[k], (S - C.T[k - 1]) / Span) : C.V[k];
        }
    return C.V[3];
}

/** Set SC.Strength and every curve of the component's system at that strength. */
inline void Set(UNiagaraComponent* Component, float Strength)
{
    if (!Component) return;
    Component->SetVariableFloat(TEXT("SC.Strength"), Strength);
    for (const FCurve& C : Curves(Component->GetAsset())) Component->SetVariableFloat(C.Value, Eval(C, Strength));
}
} // namespace SCFxStrength
