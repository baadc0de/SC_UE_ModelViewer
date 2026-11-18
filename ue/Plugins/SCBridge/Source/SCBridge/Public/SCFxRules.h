// Pure rules of the SCFxBuild commandlet (WP-K4c, WP-E1 E1-6) that the SCBridge tests check without the engine's Niagara
// internals: the loop guard of looping lightweight emitters and the clamp form of a piecewise-linear strength curve.
#pragma once
#include "CoreMinimal.h"

namespace SCFxRules
{
/**
 * Spawn infos a looping lightweight emitter's spawn info keeps alive: one per loop whose particles may still live, i.e. the
 * loop being spawned plus ceil(MaxLifetime / LoopDuration) earlier ones (Niagara's stateless emitter draws at most
 * NiagaraStateless::MaxGpuSpawnInfos of them; the rest are dropped as "overflowed spawn infos").
 */
inline int32 LiveLoops(double MaxLifetime, double LoopDuration)
{
    return LoopDuration > 0 ? int32(FMath::CeilToDouble(MaxLifetime / LoopDuration - 1e-9)) + 1 : MAX_int32;
}

/** The shortest loop whose live loops fit Capacity spawn infos: MaxLifetime / (Capacity - 1), with a margin for rounding. */
inline double MinLoopDuration(double MaxLifetime, int32 Capacity)
{
    return Capacity > 1 ? MaxLifetime / double(Capacity - 1) * 1.001 : DBL_MAX;
}

/** The loop a looping emitter gets: the wanted one, stretched only where its live loops would not fit Capacity. */
inline double LoopDuration(double Wanted, double MaxLifetime, int32 Capacity)
{
    return FMath::Max(Wanted, MinLoopDuration(MaxLifetime, Capacity));
}

/**
 * A piecewise-linear curve, held constant beyond its end keys (CryEngine's splines), as the sum the builder writes with stateless
 * clamp expressions: f(x) = Base + sum_k Rise_k * clamp((x - From_k) * Scale_k, 0, 1), one term per key interval. An interval of
 * zero width (a step) gets a steep ramp from the key (the earlier value at the key itself, as the builder's Eval) to the later
 * value 1e-6 after it.
 */
struct FClampTerm
{
    double From = 0, Scale = 1, Rise = 0;
};
struct FClampSum
{
    double Base = 0;
    TArray<FClampTerm> Terms;
    double Eval(double X) const
    {
        double V = Base;
        for (const FClampTerm& T : Terms) V += T.Rise * FMath::Clamp((X - T.From) * T.Scale, 0.0, 1.0);
        return V;
    }
};
/** Keys (t, v) sorted by t; an empty list is the constant 1 (the builder's "no curve"). */
inline FClampSum ClampSum(const TArray<FVector2D>& Keys)
{
    FClampSum S;
    if (Keys.IsEmpty())
    {
        S.Base = 1;
        return S;
    }
    S.Base = Keys[0].Y;
    constexpr double StepScale = 1e6; // a zero-width interval: full rise within 1e-6 of the key
    for (int32 k = 1; k < Keys.Num(); ++k)
    {
        const double Span = Keys[k].X - Keys[k - 1].X, Rise = Keys[k].Y - Keys[k - 1].Y;
        if (Rise == 0) continue;
        S.Terms.Add(Span > 0 ? FClampTerm{Keys[k - 1].X, 1.0 / Span, Rise} : FClampTerm{Keys[k].X, StepScale, Rise});
    }
    return S;
}
} // namespace SCFxRules
