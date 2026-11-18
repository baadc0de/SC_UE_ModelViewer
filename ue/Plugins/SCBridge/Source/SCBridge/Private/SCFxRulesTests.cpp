// SCBridge.Fx.*: the SCFxBuild rules that do not need Niagara's internals (SCFxRules.h, WP-E1 E1-6).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCFxRules.h"
#include "Stateless/NiagaraStatelessSpawnInfoCapacity.h" // NiagaraStateless::MaxGpuSpawnInfos (the fork's public constant)

namespace
{
// The builder's curve semantics (SCFxBuildCommandlet.cpp Eval): piecewise linear, the end keys held; at a key shared by two keys
// (a step) the later value.
double Reference(const TArray<FVector2D>& K, double X)
{
    if (K.IsEmpty()) return 1;
    if (X <= K[0].X) return K[0].Y;
    for (int32 i = 1; i < K.Num(); ++i)
        if (X <= K[i].X)
        {
            const double Span = K[i].X - K[i - 1].X;
            return FMath::Lerp(K[i - 1].Y, K[i].Y, Span > 0 ? (X - K[i - 1].X) / Span : 1.0);
        }
    return K.Last().Y;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFxLoopGuardTest, "SCBridge.Fx.LoopGuard",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFxLoopGuardTest::RunTest(const FString&)
{
    // The guard keeps every looping spawn info's live loops within the capacity, and leaves the wanted loop alone where it fits.
    for (const int32 Capacity : {8, 16, 32, int32(NiagaraStateless::MaxGpuSpawnInfos)})
        for (const double Life : {0.025, 0.05, 0.1, 0.6, 1.0, 1.4, 2.0, 7.0, 7.75, 8.0, 30.0})
            for (const double Wanted : {0.001, 0.01, 0.1, 0.25, 1.0, 5.0})
            {
                const double Loop = SCFxRules::LoopDuration(Wanted, Life, Capacity);
                const FString At = FString::Printf(TEXT("capacity %d life %g wanted %g -> loop %g"), Capacity, Life, Wanted, Loop);
                TestTrue(At + TEXT(": live loops fit"), SCFxRules::LiveLoops(Life, Loop) <= Capacity);
                if (SCFxRules::LiveLoops(Life, Wanted) * 1.0 <= Capacity - 1) TestEqual(At + TEXT(": not stretched"), Loop, Wanted);
                TestTrue(At + TEXT(": never shortened"), Loop >= Wanted);
            }
    // Live loops: the loop being spawned plus one per loop length of lifetime.
    TestEqual(TEXT("1 s life, 0.25 s loops"), SCFxRules::LiveLoops(1.0, 0.25), 5);
    TestEqual(TEXT("1 s life, 1 s loops"), SCFxRules::LiveLoops(1.0, 1.0), 2);
    TestEqual(TEXT("1 s life, 0.3 s loops"), SCFxRules::LiveLoops(1.0, 0.3), 5);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFxEngineCapacityTest, "SCBridge.Fx.EngineCapacity",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFxEngineCapacityTest::RunTest(const FString&)
{
    // The builder's guard is the engine's effective limit (E1-6 review): fx.Niagara.Stateless.MaxSpawnInfos, UE's 8 by default, which
    // the project's DefaultEngine.ini raises to the fork's compiled storage, 32.
    TestEqual(TEXT("compiled storage: 32"), int32(NiagaraStateless::MaxGpuSpawnInfos), 32);
    TestEqual(TEXT("UE's default limit: 8"), int32(NiagaraStateless::DefaultMaxSpawnInfos), 8);
    const int32 N = int32(NiagaraStateless::GetMaxSpawnInfos());
    TestEqual(TEXT("the project's limit (DefaultEngine.ini): the storage"), N, int32(NiagaraStateless::MaxGpuSpawnInfos));
    TestTrue(TEXT("capacity above UE 5.9's 8"), N > 8);
    TestTrue(TEXT("a multiple of 4 (scalar arrays pack four per float4)"), N % 4 == 0);
    TestTrue(TEXT("the guard is weaker than at UE's 8"), SCFxRules::MinLoopDuration(1.0, N) < SCFxRules::MinLoopDuration(1.0, 8));
    // The project's effects loop every 0.25 s (strength and beam length follow per loop): no stretch below (N - 1) / 4 s of life.
    TestEqual(TEXT("0.25 s loops up to (N - 1) x 0.25 s of life"), SCFxRules::LoopDuration(0.25, 0.25 * (N - 1) / 1.002, N), 0.25);
    TestTrue(TEXT("stretched beyond"), SCFxRules::LoopDuration(0.25, 0.25 * N, N) > 0.25);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFxClampSumTest, "SCBridge.Fx.ClampSum",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFxClampSumTest::RunTest(const FString&)
{
    // The clamp form of a strength curve equals the builder's piecewise-linear evaluation everywhere (also outside the keys).
    const TArray<TArray<FVector2D>> Curves = {
        {},
        {FVector2D(0.5, 2.0)},
        {FVector2D(0.06, 0.067), FVector2D(0.408, 0.106)},                                   // a game colour channel, 2 keys
        {FVector2D(0.0, 0.0), FVector2D(0.25, 1.0), FVector2D(0.5, 1.0), FVector2D(1.0, -0.5)}, // flat and falling parts
        {FVector2D(0.0, 1.0), FVector2D(0.3, 0.2), FVector2D(0.3, 0.8), FVector2D(0.7, 0.1), FVector2D(0.9, 0.5), FVector2D(1.0, 0.0)}, // a step, 6 keys
    };
    for (const TArray<FVector2D>& K : Curves)
    {
        const SCFxRules::FClampSum S = SCFxRules::ClampSum(K);
        double Worst = 0;
        for (int32 i = -50; i <= 1050; ++i)
        {
            const double X = i / 1000.0;
            Worst = FMath::Max(Worst, FMath::Abs(S.Eval(X) - Reference(K, X)));
        }
        TestTrue(FString::Printf(TEXT("%d keys: worst error %g"), K.Num(), Worst), Worst < 1e-9);
        TestTrue(TEXT("one term per rising or falling interval at most"), S.Terms.Num() <= FMath::Max(K.Num() - 1, 0));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFxReviewRulesTest, "SCBridge.Fx.ReviewRules",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFxReviewRulesTest::RunTest(const FString&)
{
    // E1-6 review: an exact fit is not stretched; instantaneous pulse bursts expire exclusively; a strict step through the float
    // expression; strength curves classified over their authored keys.
    TestEqual(TEXT("7.75 s of life at 0.25 s loops fits 32 spawn infos exactly: not stretched"), SCFxRules::LoopDuration(0.25, 7.75, 32), 0.25);
    TestTrue(TEXT("7.76 s does not fit: stretched"), SCFxRules::LoopDuration(0.25, 7.76, 32) > 0.25);
    TestEqual(TEXT("8 s of life at 0.25 s pulses: 32 bursts alive (a burst 8 s old is gone)"), SCFxRules::LiveBursts(8.0, 0.25), 32);
    TestEqual(TEXT("7.9 s: 32 bursts"), SCFxRules::LiveBursts(7.9, 0.25), 32);
    TestEqual(TEXT("8.1 s: 33 bursts"), SCFxRules::LiveBursts(8.1, 0.25), 33);

    const TArray<TArray<FVector2D>> Steps = {
        {FVector2D(0.0, 1.0), FVector2D(0.3, 0.2), FVector2D(0.3, 0.8), FVector2D(1.0, 0.0)},
        {FVector2D(0.5, 0.0), FVector2D(0.5, 1.0)},
        {FVector2D(0.0, 0.25), FVector2D(0.0, 0.75), FVector2D(1.0, 0.75)},
    };
    for (const TArray<FVector2D>& Authored : Steps)
    {
        const SCFxRules::FClampSum Sum = SCFxRules::ClampSum(Authored);
        TArray<FVector2D> K = Authored; // the reference at the keys as the float expression holds them
        for (FVector2D& P : K) P.X = double(float(P.X));
        for (int32 i = 1; i < K.Num(); ++i)
        {
            if (K[i].X != K[i - 1].X) continue;
            const float T = float(K[i].X);
            const float Next = T == 0.0f ? 1e-30f : std::nextafter(T, FLT_MAX);
            TestEqual(FString::Printf(TEXT("step at %g: the earlier value at the key"), T), Sum.EvalFloat(T), float(Reference(K, T)), 1e-6f);
            TestEqual(FString::Printf(TEXT("step at %g: the later value at the next float"), T), Sum.EvalFloat(Next), float(Reference(K, Next)), 1e-6f);
        }
        double Worst = 0;
        for (int32 i = -50; i <= 1050; ++i)
        {
            const float X = float(i / 1000.0);
            Worst = FMath::Max(Worst, double(FMath::Abs(Sum.EvalFloat(X) - float(Reference(K, X)))));
        }
        TestTrue(FString::Printf(TEXT("%d keys with a step: float expression within 1e-5 everywhere (worst %g)"), K.Num(), Worst), Worst < 1e-5);
    }

    // A count curve that peaks at 0.25 and is 0 at the configured strengths 0, 0.5 and 1.
    const TArray<FVector2D> Peak = {FVector2D(0.0, 0.0), FVector2D(0.25, 3.0), FVector2D(0.5, 0.0), FVector2D(1.0, 0.0)};
    TArray<double> KeyTimes;
    for (const FVector2D& P : Peak) KeyTimes.Add(P.X);
    const TArray<double> At = SCFxRules::ClassificationStrengths({0.0, 0.5, 1.0}, KeyTimes);
    double Max = 0;
    for (double S : At) Max = FMath::Max(Max, Reference(Peak, S));
    TestEqual(TEXT("classified at the peak: its maximum is seen"), Max, 3.0);
    TestTrue(TEXT("the knots stay"), At.Contains(0.0) && At.Contains(0.5) && At.Contains(1.0));
    TestFalse(TEXT("keys outside [0, 1] are not strengths"), SCFxRules::ClassificationStrengths({0.5}, {-1.0, 2.0}).Contains(2.0));
    return true;
}
#endif
