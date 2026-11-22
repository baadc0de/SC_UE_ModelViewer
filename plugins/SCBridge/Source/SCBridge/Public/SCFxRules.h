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
 *  stays: E1-6 review). With a capacity of one no loop fits (a rate loop's interval overlaps the next one's while its particles
 *  live): the wanted loop is kept and the builder reports it (review fixes 3, Astra A8; it was stretched to an infinite loop). */
inline double LoopDuration(double Wanted, double MaxLifetime, int32 Capacity)
{
    if (Capacity < 2 || LiveLoops(MaxLifetime, Wanted) <= Capacity) return Wanted;
    return FMath::Max(Wanted, MinLoopDuration(MaxLifetime, Capacity));
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
 * expressions, clamp-sum strength curves, owner-scale sprite sizes; no SC.SV / SC.BR / SC.Scale2 runtime parameters). 3: review fixes 3
 * (strict steps as comparison expressions, strength curves classified on both sides of a step, light colour relative to a non-black
 * reference); systems built by version 2 lose those curves.
 */
inline constexpr int32 FxContractVersion = 3;
inline const TCHAR* FxContractParameter = TEXT("SC.FxContract");

/**
 * A piecewise-linear curve, held constant beyond its end keys (CryEngine's splines), as the sum the builder writes with stateless
 * expressions: f(x) = Base + sum_k Rise_k * term_k(x), one term per key interval. A ramp's term is clamp((x - From) * Scale, 0, 1)
 * (the fork's clamp expression); a zero-width interval (a step at From) is a strict step, the fork's comparison expression
 * x > From ? 1 : 0 (review fixes 3, Terra T5 / Astra A7: a clamp with a huge scale completed the step at 0 only by 1e-30, and a
 * denormal x - From flushes): the earlier value at the key itself (as the builder's Eval), the later one at every float above it.
 */
struct FClampTerm
{
    double From = 0, Scale = 1, Rise = 0;
    bool bStep = false;
};
struct FClampSum
{
    double Base = 0;
    TArray<FClampTerm> Terms;
    double Eval(double X) const
    {
        double V = Base;
        for (const FClampTerm& T : Terms) V += T.Rise * (T.bStep ? (X > T.From ? 1.0 : 0.0) : FMath::Clamp((X - T.From) * T.Scale, 0.0, 1.0));
        return V;
    }
    /** As the built stateless expression evaluates it: in float; a ramp x + (-From) then x Scale, clamped, x Rise; a step the
     *  comparison x > From (FNiagaraStatelessExpressionFloatGreaterThan) x Rise; summed from Base. */
    float EvalFloat(float X) const
    {
        float V = float(Base);
        for (const FClampTerm& T : Terms)
            V = V + (T.bStep ? (X > float(T.From) ? 1.0f : 0.0f) : FMath::Clamp((X + float(-T.From)) * float(T.Scale), 0.0f, 1.0f)) * float(T.Rise);
        return V;
    }
};
/** A strength curve as the builder reads it: keys [t, v0, v1, ...] sorted by t (stable, so coincident keys keep their order). */
using FKeys = TArray<TArray<double>>;

/** A key's value of a channel; a key with fewer channels repeats its last value. */
inline double KeyValue(const TArray<double>& Key, int32 Channel)
{
    return Key.IsValidIndex(Channel + 1) ? Key[Channel + 1] : Key.Last();
}

/** Piecewise linear, held at the end keys (CryEngine splines); at a time shared by coincident keys, the earlier key's value. An empty
 *  curve is the constant 1. */
inline double EvalKeys(const FKeys& Keys, double T, int32 Channel = 0)
{
    if (Keys.IsEmpty()) return 1.0;
    if (T <= Keys[0][0]) return KeyValue(Keys[0], Channel);
    for (int32 i = 1; i < Keys.Num(); ++i)
        if (T <= Keys[i][0])
        {
            const double Span = Keys[i][0] - Keys[i - 1][0];
            const double A = Span > 0 ? (T - Keys[i - 1][0]) / Span : 1.0;
            return FMath::Lerp(KeyValue(Keys[i - 1], Channel), KeyValue(Keys[i], Channel), A);
        }
    return KeyValue(Keys.Last(), Channel);
}

/**
 * The values a strength curve's channel (x Scale) is classified by (does it vary, is it zero everywhere): the curve at the
 * ClassificationStrengths and each key's own value inside [0, 1]. Review fixes 3 (Terra T3 / Astra A6): at a time shared by
 * coincident keys Eval gives the earlier key, so (0,0) (0.5,0) (0.5,1) (1,0) sampled at times only is 0 everywhere though it is
 * positive just above 0.5; the keys' own values take both sides of the step.
 */
inline TArray<double> ClassificationValues(const FKeys& Keys, const TArray<double>& Knots, int32 Channel = 0, double Scale = 1.0)
{
    TArray<double> KeyTimes;
    for (const TArray<double>& K : Keys) KeyTimes.Add(K[0]);
    TArray<double> Y;
    for (double S : ClassificationStrengths(Knots, KeyTimes)) Y.Add(EvalKeys(Keys, S, Channel) * Scale);
    for (const TArray<double>& K : Keys)
        if (K[0] >= 0.0 && K[0] <= 1.0) Y.Add(KeyValue(K, Channel) * Scale);
    return Y;
}

/** A colour key's own RGB (times the base colour). */
inline FLinearColor KeyColour(const TArray<double>& Key, const FLinearColor& Base)
{
    return FLinearColor(Base.R * float(KeyValue(Key, 0)), Base.G * float(KeyValue(Key, 1)), Base.B * float(KeyValue(Key, 2)), 1.f);
}

/**
 * The colour a light's strength curve is relative to: its strength, RGB (the base colour times the curve's channels) and luminance.
 * Strength 1 where the colour is not black there; else (review fixes 3, Terra T4 / Astra A6: a colour bright at 0.5 and black at 1
 * made the whole relative luminance curve 0) the brightest of the curve at the classification strengths and the keys' own colours
 * inside [0, 1]. Black everywhere: luminance 0 (the light is invisible).
 */
struct FLightColourReference
{
    double Strength = 1;
    FLinearColor Rgb = FLinearColor::Black;
    double Luminance = 0;
};
inline FLightColourReference LightColourReference(const FKeys& ColourKeys, const FLinearColor& Base, const TArray<double>& Knots)
{
    const auto At = [&](double S) {
        return FLinearColor(Base.R * float(EvalKeys(ColourKeys, S, 0)), Base.G * float(EvalKeys(ColourKeys, S, 1)), Base.B * float(EvalKeys(ColourKeys, S, 2)), 1.f);
    };
    FLightColourReference R;
    R.Rgb = At(1.0);
    R.Luminance = R.Rgb.GetLuminance();
    if (R.Luminance > 0) return R;
    const auto Consider = [&R](double S, const FLinearColor& C) {
        const double L = C.GetLuminance();
        if (L > R.Luminance)
        {
            R.Strength = S;
            R.Rgb = C;
            R.Luminance = L;
        }
    };
    TArray<double> KeyTimes;
    for (const TArray<double>& K : ColourKeys) KeyTimes.Add(K[0]);
    for (double S : ClassificationStrengths(Knots, KeyTimes)) Consider(S, At(S));
    for (const TArray<double>& K : ColourKeys)
        if (K[0] >= 0.0 && K[0] <= 1.0) Consider(K[0], KeyColour(K, Base));
    return R;
}

/** The colour curve's luminance relative to the reference's, one key per colour key, each from that key's own channels (review fixes 3:
 *  coincident keys keep their step; Eval at the key's time gave both the earlier key's colour). */
inline TArray<FVector2D> RelativeLuminanceKeys(const FKeys& ColourKeys, const FLinearColor& Base, double ReferenceLuminance)
{
    TArray<FVector2D> Out;
    for (const TArray<double>& K : ColourKeys)
        Out.Add(FVector2D(K[0], ReferenceLuminance > 0 ? KeyColour(K, Base).GetLuminance() / ReferenceLuminance : 0.0));
    return Out;
}

/** Whether a looping spawn info's live loops fit Capacity spawn infos. */
inline bool LoopsFit(double MaxLifetime, double LoopDuration, int32 Capacity)
{
    return LiveLoops(MaxLifetime, LoopDuration) <= Capacity;
}

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
        S.Terms.Add(Span > 0 ? FClampTerm{Keys[k - 1].X, 1.0 / Span, Rise, false} : FClampTerm{double(float(Keys[k].X)), 0.0, Rise, true});
    }
    return S;
}
} // namespace SCFxRules
