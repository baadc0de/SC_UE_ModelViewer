// FSCCargoRouteSearch (WP-K1): the route search behind FSCCargoRoute::Plan, spread over frames.
//  - Two fronts, one from the start and one from the goal, expanded in turn (weighted A* each, towards the other end);
//    the route is where they meet. A front that runs out of nodes has searched everything its end can reach: the end is
//    enclosed and the search stops there instead of flooding the other side to the expansion cap.
//  - Nodes are world-aligned half cells. A node steps a full cell in the open (26 neighbours); when one of those steps
//    is blocked (the shape overlaps there, or the step sweeps into something) it also steps half cells, so a passage
//    narrower than a cell (a doorway band between a lintel and stacked boxes) is found whatever the lattice's phase.
//  - The start and the goal are nodes of their own (off the lattice), joined to the lattice nodes around them.
#include "Cargo/SCCargoFlightPath.h"
#include "HAL/PlatformTime.h"

namespace
{
constexpr int64 StartKey = -1, GoalKey = -2; // the roots; lattice keys are positive
constexpr int32 KeyBias = 0x100000;
int64 Key(const FIntVector& C) { return (int64(C.X + KeyBias) << 42) | (int64(C.Y + KeyBias) << 21) | int64(C.Z + KeyBias); }
FIntVector Cell(int64 K) { return FIntVector(int32(K >> 42) - KeyBias, int32((K >> 21) & 0x1FFFFF) - KeyBias, int32(K & 0x1FFFFF) - KeyBias); }
constexpr double Weight = 1.6; // greedy: far fewer expansions in big open hangars, routes a little longer than shortest
} // namespace

const TCHAR* FSCCargoRoute::EndName(ESCCargoRouteEnd End)
{
    switch (End)
    {
    case ESCCargoRouteEnd::Direct: return TEXT("direct");
    case ESCCargoRouteEnd::Found: return TEXT("found");
    case ESCCargoRouteEnd::GoalBlocked: return TEXT("goal_blocked");
    case ESCCargoRouteEnd::StartEnclosed: return TEXT("start_enclosed");
    case ESCCargoRouteEnd::GoalEnclosed: return TEXT("goal_enclosed");
    case ESCCargoRouteEnd::Budget: return TEXT("budget");
    case ESCCargoRouteEnd::Smoothing: return TEXT("smoothing");
    default: return TEXT("none");
    }
}

FSCCargoRoute FSCCargoRoute::Plan(const FVector& Start, const FVector& Goal, const FSCCargoRouteQuery& Q)
{
    FSCCargoRouteSearch Search;
    Search.Begin(Start, Goal, Q);
    while (!Search.Step(1e9)) {}
    return Search.Result;
}

void FSCCargoRouteSearch::Begin(const FVector& InStart, const FVector& InGoal, const FSCCargoRouteQuery& InQuery)
{
    *this = FSCCargoRouteSearch();
    Start = InStart;
    Goal = InGoal;
    Q = InQuery;
    FineCm = Q.CellCm / 2;
    Phase = EPhase::Ends;
}

FVector FSCCargoRouteSearch::NodePos(int64 K) const
{
    return K == StartKey ? Start : K == GoalKey ? Goal : FVector(Cell(K)) * FineCm;
}

bool FSCCargoRouteSearch::CellBlocked(int64 K, const FIntVector& C, const FVector& W)
{
    if (const bool* B = Blocked.Find(K)) return *B;
    bool bBlocked;
    if (const bool* Shared = Q.CellCache ? Q.CellCache->Find(C) : nullptr) bBlocked = *Shared;
    else
    {
        bBlocked = Q.Blocked(W);
        if (Q.CellCache) Q.CellCache->Add(C, bBlocked);
    }
    Blocked.Add(K, bBlocked);
    return bBlocked;
}

bool FSCCargoRouteSearch::Step(double BudgetSeconds)
{
    const double Began = FPlatformTime::Seconds();
    const bool bDone = StepInner(Began + BudgetSeconds);
    const double Took = FPlatformTime::Seconds() - Began;
    Seconds += Took;
    MaxStepSeconds = FMath::Max(MaxStepSeconds, Took);
    ++Steps;
    return bDone;
}

FSCCargoRouteSearch::ESweep FSCCargoRouteSearch::StepSweep(const FVector& A, const FVector& B, double Until)
{
    // A stretch at a time: the stretches together are the whole sweep, and the first that hits ends it.
    constexpr double StretchCm = 300;
    if (SweepAt < 0 || !SweepA.Equals(A, 0.0) || !SweepB.Equals(B, 0.0))
    {
        SweepA = A;
        SweepB = B;
        SweepAt = 0;
    }
    const double Length = FVector::Dist(A, B);
    const FVector Dir = (B - A).GetSafeNormal();
    do
    {
        const double Next = FMath::Min(SweepAt + StretchCm, Length);
        if (!Q.Free(A + Dir * SweepAt, Next >= Length ? B : A + Dir * Next))
        {
            SweepAt = -1;
            return ESweep::Hit;
        }
        SweepAt = Next;
        if (SweepAt < Length && FPlatformTime::Seconds() > Until) return ESweep::Pending;
    } while (SweepAt < Length);
    SweepAt = -1;
    return ESweep::Free;
}

bool FSCCargoRouteSearch::StepInner(double Until)
{
    if (Phase == EPhase::Done) return true;
    FSCCargoRoute& R = Result;
    if (Phase == EPhase::Ends)
    {
        const ESweep Straight = StepSweep(Start, Goal, Until);
        if (Straight == ESweep::Pending) return false;
        if (Straight == ESweep::Free)
        {
            R.bDirect = R.bFound = true;
            R.End = ESCCargoRouteEnd::Direct;
            R.Waypoints = {Start, Goal};
            R.Samples = FSCCargoRoute::Spline(R.Waypoints, Q.SampleCm);
            Phase = EPhase::Clearance;
            return StepClearance(Until);
        }
        if (Q.Blocked(Goal)) // the shape does not fit at the goal: nothing to search for
        {
            R.End = ESCCargoRouteEnd::GoalBlocked;
            R.Samples = {Start, Goal};
            Phase = EPhase::Done;
            return true;
        }
        Lo = FVector(FMath::Min(Start.X, Goal.X) - Q.MarginCm, FMath::Min(Start.Y, Goal.Y) - Q.MarginCm, FMath::Max(Q.MinZ, FMath::Min(Start.Z, Goal.Z) - Q.MarginCm));
        Hi = FVector(FMath::Max(Start.X, Goal.X) + Q.MarginCm, FMath::Max(Start.Y, Goal.Y) + Q.MarginCm, FMath::Min(Q.MaxZ, FMath::Max(Start.Z, Goal.Z) + Q.MarginCm));
        auto Root = [&](FFront& F, const FVector& P, const FVector& Other, int64 K) {
            F.Root = P;
            F.Base = FIntVector(FMath::RoundToInt32(P.X / FineCm), FMath::RoundToInt32(P.Y / FineCm), FMath::RoundToInt32(P.Z / FineCm));
            F.G.Add(K, 0);
            F.Open.HeapPush({FVector::Dist(P, Other) * Weight, K});
        };
        Root(Fronts[0], Start, Goal, StartKey);
        Root(Fronts[1], Goal, Start, GoalKey);
        Phase = EPhase::Search;
        if (FPlatformTime::Seconds() > Until) return false;
    }
    if (Phase == EPhase::Search)
    {
        while (!bMet && (bMid || R.Expansions < Q.MaxExpansions))
        {
            const EExpand Done = Expand(Turn, Until);
            if (Done == EExpand::Pending) return false; // the rest of this node's neighbours next frame
            if (Done == EExpand::Exhausted)
            {
                R.End = Turn == 0 ? ESCCargoRouteEnd::StartEnclosed : ESCCargoRouteEnd::GoalEnclosed;
                break;
            }
            Turn ^= 1;
            if (!bMet && R.Expansions < Q.MaxExpansions && FPlatformTime::Seconds() > Until) return false; // the rest next frame
        }
        if (!bMet)
        {
            if (R.End == ESCCargoRouteEnd::None) R.End = ESCCargoRouteEnd::Budget;
            R.Waypoints = {Start, Goal};
            R.Samples = FSCCargoRoute::Spline(R.Waypoints, Q.SampleCm);
            Phase = EPhase::Done;
            return true;
        }
        BeginFinish();
        if (FPlatformTime::Seconds() > Until) return false;
    }
    if (Phase == EPhase::Shortcut && !StepShortcut(Until)) return false;
    if (Phase == EPhase::Smooth && !StepSmooth(Until)) return false;
    if (Phase == EPhase::Clearance && !StepClearance(Until)) return false;
    return true;
}

FSCCargoRouteSearch::EExpand FSCCargoRouteSearch::Expand(int32 Side, double Until)
{
    FFront& A = Fronts[Side];
    const FFront& B = Fronts[1 - Side];
    FSCCargoRouteFront& Stats = Side == 0 ? Result.FromStart : Result.FromGoal;
    auto Met = [&](int64 Mine, int64 Theirs) {
        bMet = true;
        bMid = false;
        MeetStart = Side == 0 ? Mine : Theirs;
        MeetGoal = Side == 0 ? Theirs : Mine;
    };
    if (!bMid)
    {
        FOpen Cur;
        do
        {
            if (A.Open.IsEmpty()) return EExpand::Exhausted;
            A.Open.HeapPop(Cur, EAllowShrinking::No);
        } while (A.Closed.Contains(Cur.K));
        A.Closed.Add(Cur.K);
        ++Result.Expansions;
        ++Stats.Expansions;
        CurK = Cur.K;
        bCurRoot = CurK < 0;
        CurCell = bCurRoot ? A.Base : Cell(CurK);
        CurFrom = NodePos(CurK);
        CurCost = A.G[CurK];
        Stats.Reached += CurFrom;
        if (const double D = FVector::DistSquared(CurFrom, B.Root); D < A.NearestSq)
        {
            A.NearestSq = D;
            Stats.Nearest = CurFrom;
        }
        // The other end itself, from the nodes around it.
        if (const FIntVector D = CurCell - B.Base; FMath::Max3(FMath::Abs(D.X), FMath::Abs(D.Y), FMath::Abs(D.Z)) <= 2 && Q.Free(CurFrom, B.Root))
        {
            Met(CurK, Side == 0 ? GoalKey : StartKey);
            return EExpand::Expanded;
        }
        bMid = true;
        bObstructed = false;
        Pass = Neighbour = 0;
    }
    // Its neighbours, a full cell away; when one of those steps is blocked, half a cell away too. One neighbour can cost
    // several sweeps of a large shape: the expansion itself is resumable (Pass, Neighbour).
    for (; Pass < 2; ++Pass, Neighbour = 0)
    {
        if (Pass == 1 && !(bObstructed && Q.bRefine)) break;
        const int32 Stride = Pass == 0 ? 2 : 1;
        for (; Neighbour < 27; ++Neighbour)
        {
            const FIntVector Offset(Neighbour / 9 - 1, (Neighbour / 3) % 3 - 1, Neighbour % 3 - 1);
            if (Offset == FIntVector::ZeroValue && !bCurRoot) continue; // a root's own half cell is its first lattice node
            const FIntVector N = CurCell + Offset * Stride;
            const FVector W = FVector(N) * FineCm;
            if (W.X < Lo.X || W.Y < Lo.Y || W.Z < Lo.Z || W.X > Hi.X || W.Y > Hi.Y || W.Z > Hi.Z) continue;
            const int64 NK = Key(N);
            if (A.Closed.Contains(NK)) continue;
            if (CellBlocked(NK, N, W))
            {
                bObstructed = true;
                ++Stats.BlockedCells;
            }
            else if (const double Cost = CurCost + FVector::Dist(CurFrom, W), *Old = A.G.Find(NK); !Old || *Old > Cost)
            {
                // The step itself must sweep free (two free cells can sit on either side of a thin plate or a corner).
                if (!Q.Free(CurFrom, W))
                {
                    bObstructed = true;
                    ++Stats.BlockedSteps;
                }
                else
                {
                    A.G.Add(NK, Cost);
                    A.Parent.Add(NK, CurK);
                    A.Open.HeapPush({Cost + FVector::Dist(W, B.Root) * Weight, NK});
                    if (B.G.Contains(NK))
                    {
                        Met(NK, NK);
                        return EExpand::Expanded;
                    }
                }
            }
            if (FPlatformTime::Seconds() > Until)
            {
                ++Neighbour;
                return EExpand::Pending;
            }
        }
    }
    bMid = false;
    return EExpand::Expanded;
}

void FSCCargoRouteSearch::BeginFinish()
{
    // Start .. the meeting node along the start front's parents, then on to the goal along the goal front's.
    Raw.Reset();
    for (int64 K = MeetStart;; K = Fronts[0].Parent[K])
    {
        Raw.Add(NodePos(K));
        if (K == StartKey) break;
    }
    Algo::Reverse(Raw);
    for (int64 K = MeetGoal;; K = Fronts[1].Parent[K])
    {
        if (K != MeetStart) Raw.Add(NodePos(K));
        if (K == GoalKey) break;
    }
    Short = {Raw[0]};
    ShortI = 0;
    ShortJ = Raw.Num() - 1;
    Phase = EPhase::Shortcut;
}

bool FSCCargoRouteSearch::StepShortcut(double Until)
{
    // From each kept point, the farthest later point the shape sweeps to freely (the next one always does: that step was
    // swept when it was taken).
    while (ShortI + 1 < Raw.Num())
    {
        if (ShortJ > ShortI + 1)
        {
            const ESweep S = StepSweep(Raw[ShortI], Raw[ShortJ], Until);
            if (S == ESweep::Pending) return false;
            if (S == ESweep::Hit)
            {
                --ShortJ;
                if (FPlatformTime::Seconds() > Until) return false;
                continue;
            }
        }
        Short.Add(Raw[ShortJ]);
        ShortI = ShortJ;
        ShortJ = Raw.Num() - 1;
        if (FPlatformTime::Seconds() > Until && ShortI + 1 < Raw.Num()) return false;
    }
    Variant = 0;
    MakeSamples();
    Phase = EPhase::Smooth;
    return FPlatformTime::Seconds() <= Until ? StepSmooth(Until) : false;
}

void FSCCargoRouteSearch::MakeSamples()
{
    // The spline must sweep free too; if it cuts a corner, smooth the denser lattice path, else keep straight legs: the
    // shortcut ones, else the lattice steps themselves.
    FSCCargoRoute& R = Result;
    R.Waypoints = Variant % 2 == 0 ? Short : Raw;
    if (Variant < 2) R.Samples = FSCCargoRoute::Spline(R.Waypoints, Q.SampleCm);
    else
    {
        R.Samples.Reset();
        for (int32 I = 0; I + 1 < R.Waypoints.Num(); ++I)
        {
            const int32 Count = FMath::Max(1, FMath::CeilToInt32(FVector::Dist(R.Waypoints[I], R.Waypoints[I + 1]) / Q.SampleCm));
            for (int32 K = 0; K < Count; ++K) R.Samples.Add(FMath::Lerp(R.Waypoints[I], R.Waypoints[I + 1], double(K) / Count));
        }
        R.Samples.Add(R.Waypoints.Last());
    }
    SampleI = 1;
}

bool FSCCargoRouteSearch::StepSmooth(double Until)
{
    FSCCargoRoute& R = Result;
    for (;;)
    {
        bool bHit = false;
        for (; SampleI < R.Samples.Num() && !bHit; ++SampleI)
        {
            bHit = !Q.Free(R.Samples[SampleI - 1], R.Samples[SampleI]);
            if (!bHit && SampleI + 1 < R.Samples.Num() && FPlatformTime::Seconds() > Until)
            {
                ++SampleI;
                return false;
            }
        }
        if (!bHit)
        {
            R.bFound = true;
            R.End = ESCCargoRouteEnd::Found;
            break;
        }
        if (++Variant > 3)
        {
            R.bFound = false;
            R.End = ESCCargoRouteEnd::Smoothing;
            break;
        }
        MakeSamples();
        if (FPlatformTime::Seconds() > Until) return false;
    }
    Phase = R.bFound ? EPhase::Clearance : EPhase::Done;
    return true;
}

bool FSCCargoRouteSearch::StepClearance(double Until)
{
    // The found route's proof, within the budget like the search: every sample swept from the one before and, every
    // metre, the shape grown until it touches (FSCCargoRoute::MinClearance, a sample at a time).
    FSCCargoRoute& R = Result;
    constexpr double EveryCm = 100;
    if (ClearI == 0) Clear.Begin(EveryCm);
    while (ClearI < R.Samples.Num())
    {
        Clear.Sample(R.Samples, ClearI, Q, EveryCm);
        if (++ClearI < R.Samples.Num() && FPlatformTime::Seconds() > Until) return false;
    }
    R.Clearance = Clear.Min;
    R.Hits = Clear.Hits;
    Phase = EPhase::Done;
    return true;
}
