// Pure rules of the SCFxBuild commandlet (WP-K4c, WP-E1 E1-6) that the SCBridge tests check without the engine's Niagara
// internals: the loop guard of looping lightweight emitters and the clamp form of a piecewise-linear strength curve.
#pragma once
#include "CoreMinimal.h"
#include <cmath>

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

/** The loop a looping emitter gets: the wanted one, stretched only where its live loops would not fit Capacity (an exact fit
 *  stays: E1-6 review). */
inline double LoopDuration(double Wanted, double MaxLifetime, int32 Capacity)
{
    return LiveLoops(MaxLifetime, Wanted) <= Capacity ? Wanted : FMath::Max(Wanted, MinLoopDuration(MaxLifetime, Capacity));
}

/**
 * Instantaneous bursts of a pulse (period Period) alive at once: one per period of lifetime, the expiry being exclusive (a burst
 * Lifetime old is gone), so 8 s of life at 0.25 s pulses keeps exactly 32 (E1-6 review: LiveLoops counts a rate loop's extra interval).
 */
inline int32 LiveBursts(double MaxLifetime, double Period)
{
    return Period > 0 ? FMath::Max(1, int32(FMath::CeilToDouble(MaxLifetime / Period - 1e-9))) : MAX_int32;
}

/**
 * The strengths at which a strength curve is classified (does it vary, is it zero everywhere): the configured knots, the domain's ends
 * 0 and 1, and every authored key inside [0, 1]. A piecewise-linear curve held at its ends takes its extremes on [0, 1] there, so a
 * curve that peaks between the knots (0.25 with knots 0, 0.5, 1) is neither constant nor invisible (E1-6 review).
 */
inline TArray<double> ClassificationStrengths(const TArray<double>& Knots, const TArray<double>& KeyTimes)
{
    TArray<double> Out = Knots;
    Out.Append({0.0, 1.0});
    for (double T : KeyTimes)
        if (T >= 0.0 && T <= 1.0) Out.Add(T);
    Out.Sort();
    TArray<double> Unique;
    for (double T : Out)
        if (Unique.IsEmpty() || !FMath::IsNearlyEqual(Unique.Last(), T, 1e-9)) Unique.Add(T);
    return Unique;
}

/**
 * The generated FX contract version (E1-6 review): SCFxBuild writes it as the system's user parameter SC.FxContract; the runtime warns
 * about a system without it or with an older one, and the showcase package refuses to cook stale systems. 2: E1-6 (stateless spawn-rate
 * expressions, clamp-sum strength curves, owner-scale sprite sizes; no SC.SV / SC.BR / SC.Scale2 runtime parameters).
 */
inline constexpr int32 FxContractVersion = 2;
inline const TCHAR* FxContractParameter = TEXT("SC.FxContract");

/**
 * The scale of a step's clamp term (a zero-width key interval at T): the expression evaluates in float, and (x - T) x Scale reaches 1
 * at the next float above T, so every strength above the key gets the later value and the key itself the earlier one (a strict step;
 * E1-6 review). Below 1e-30 (a step at strength 0) the scale stays representable and the step completes by 1e-30.
 */
inline double StepScale(double T)
{
    const float Tf = float(T);
    const float Next = FMath::Abs(Tf) < 1e-30f ? 1e-30f : std::nextafter(Tf, FLT_MAX);
    return 1.0 / (double(Next) - double(Tf));
}

/**
 * A piecewise-linear curve, held constant beyond its end keys (CryEngine's splines), as the sum the builder writes with stateless
 * clamp expressions: f(x) = Base + sum_k Rise_k * clamp((x - From_k) * Scale_k, 0, 1), one term per key interval. An interval of
 * zero width (a step) is a strict step: the earlier value at the key itself (as the builder's Eval), the later one from the next
 * float above it (StepScale).
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
    /** As the built stateless expression evaluates it: in float, x + (-From) then x Scale, clamped, x Rise, summed from Base. */
    float EvalFloat(float X) const
    {
        float V = float(Base);
        for (const FClampTerm& T : Terms) V = V + FMath::Clamp((X + float(-T.From)) * float(T.Scale), 0.0f, 1.0f) * float(T.Rise);
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
    for (int32 k = 1; k < Keys.Num(); ++k)
    {
        const double Span = Keys[k].X - Keys[k - 1].X, Rise = Keys[k].Y - Keys[k - 1].Y;
        if (Rise == 0) continue;
        S.Terms.Add(Span > 0 ? FClampTerm{Keys[k - 1].X, 1.0 / Span, Rise} : FClampTerm{double(float(Keys[k].X)), StepScale(Keys[k].X), Rise});
    }
    return S;
}
} // namespace SCFxRules
