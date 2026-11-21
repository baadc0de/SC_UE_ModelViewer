// Ground contact of rigid ramp probes sampled along an authored clip.
#pragma once
#include "CoreMinimal.h"

namespace SCGangwayGround
{
inline bool IsRampRole(const FString& Type, const FString& Subtype, const FString& Port)
{
    auto Typed = [](const FString& Value) { return Value.Equals(TEXT("Ramp"), ESearchCase::IgnoreCase) ||
        Value.Equals(TEXT("Gangway"), ESearchCase::IgnoreCase); };
    if (Typed(Type) || Typed(Subtype)) return true;
    TArray<FString> Tokens;
    const TCHAR* Delimiters[] = {TEXT("_"), TEXT("-")};
    Port.ParseIntoArray(Tokens, Delimiters, UE_ARRAY_COUNT(Delimiters), true);
    return Tokens.ContainsByPredicate(Typed);
}
using FPose = TFunctionRef<void(double, TArray<FVector>&)>;
using FTrace = TFunctionRef<TOptional<double>(FVector, FVector)>;

// Traverse keys in playback order. Subdivide curved motion until the probe chord is within one millimetre.
inline TOptional<double> LimitFrame(double From, double To, FPose Pose, FTrace Trace,
    TFunctionRef<bool(const FBox&)> MayHit = [](const FBox&) { return true; })
{
    if (From == To) return {};
    TArray<FVector> Start;
    Pose(From, Start);
    auto Interval = [&](auto&& Self, double A, double B, const TArray<FVector>& P, const TArray<FVector>& Q,
                        int32 Depth) -> TOptional<double> {
        if (P.IsEmpty() || P.Num() != Q.Num()) return {};
        const double Mid = (A + B) * 0.5;
        TArray<FVector> M;
        Pose(Mid, M);
        if (M.Num() != P.Num()) return {};
        for (int32 I = 0; Depth < 12 && I < P.Num(); ++I)
            if (!M[I].Equals((P[I] + Q[I]) * 0.5, 0.1))
            {
                if (auto Hit = Self(Self, A, Mid, P, M, Depth + 1); Hit.IsSet()) return Hit;
                return Self(Self, Mid, B, M, Q, Depth + 1);
            }
        TOptional<double> First;
        FBox Bounds(ForceInit);
        for (int32 I = 0; I < P.Num(); ++I)
            if (Q[I].Z < P[I].Z - UE_SMALL_NUMBER) { Bounds += P[I]; Bounds += Q[I]; }
        if (!Bounds.IsValid || !MayHit(Bounds.ExpandBy(0.2))) return {};
        for (int32 I = 0; I < P.Num(); ++I)
        {
            if (Q[I].Z >= P[I].Z - UE_SMALL_NUMBER) continue;
            const auto Hit = Trace(P[I], Q[I]);
            if (Hit.IsSet() && (!First.IsSet() || Hit.GetValue() < First.GetValue())) First = Hit;
        }
        return First.IsSet() ? TOptional<double>(FMath::Lerp(A, B, First.GetValue())) : TOptional<double>{};
    };
    for (double A = From; A != To;)
    {
        const double B = To > From ? FMath::Min(To, FMath::FloorToDouble(A) + 1.)
                                   : FMath::Max(To, FMath::CeilToDouble(A) - 1.);
        TArray<FVector> End;
        Pose(B, End);
        if (auto Hit = Interval(Interval, A, B, Start, End, 0); Hit.IsSet()) return Hit;
        Start = MoveTemp(End);
        A = B;
    }
    return {};
}
}
