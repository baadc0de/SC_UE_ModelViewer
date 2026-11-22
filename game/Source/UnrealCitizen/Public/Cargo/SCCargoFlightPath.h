// Drone flight paths (WP-K1): obstacle-avoiding routes and their motion profile, pure C++ over world-query callbacks
// (automation-tested with synthetic obstacles, SCBridge.Cargo.Flight*).
//  - FSCCargoRoute::Plan: straight line when its sweep is free, else a two-ended A* (one front from the start, one from
//    the goal, meeting in between) over a 3D occupancy lattice sampled lazily with the flying shape (drones + hanging
//    box): full cells in the open, half cells from a node one of whose full steps is blocked (a passage narrower than
//    a cell is not missed by the lattice's phase). Shortcut by sweeps, then a centripetal Catmull-Rom spline that is
//    swept again (less smoothing when it clips something). A front that runs out of nodes proves its end enclosed.
//  - FSCCargoFlight: arclength speed profile along the samples: speed capped by curvature (lateral acceleration) and by
//    braking distance, acceleration rate-limited by jerk (an S-curve), so heavy loads ramp up and brake slowly.
#pragma once
#include "CoreMinimal.h"

struct UNREALCITIZEN_API FSCCargoRouteQuery
{
    /** True when the flying shape centred at P overlaps something. */
    TFunction<bool(const FVector& P)> Blocked;
    /** True when the shape sweeps from A to B without a hit. */
    TFunction<bool(const FVector& A, const FVector& B)> Free;
    /** True when the shape grown by InflateCm on every side, centred at P, overlaps something (clearance measurement). */
    TFunction<bool(const FVector& P, double InflateCm)> BlockedBy;
    /** What the shape hits sweeping from A to B (diagnostics). */
    TFunction<FString(const FVector& A, const FVector& B)> Describe;
    double CellCm = 125;          // occupancy lattice step in the open
    bool bRefine = true;          // half steps from nodes with a blocked full step
    double MarginCm = 2500;       // search box around start and goal, horizontally
    double MinZ = -1e9, MaxZ = 1e9; // vertical search limits (floor, ceiling)
    int32 MaxExpansions = 60000;  // both fronts together
    double SampleCm = 50;         // spline sample spacing
    /** Occupancy of world-aligned half cells (index = round(P / (CellCm / 2))) shared by searches with the same flying
     *  shape in the same surroundings: a cell sampled once is not sampled again. Optional. */
    TSharedPtr<TMap<FIntVector, bool>> CellCache;
    /** What the callbacks test (shape, rotation, what they ignore, where the still obstacles are): searches and cell
     *  caches are shared only between queries with the same identity. */
    FString Identity;
};

/** How a route search ended. */
enum class ESCCargoRouteEnd : uint8
{
    None,
    Direct,        // the straight line swept free
    Found,
    GoalBlocked,   // the shape does not fit at the goal
    StartEnclosed, // everything reachable from the start was searched: no way out of where it is
    GoalEnclosed,  // everything reachable from the goal was searched: no way in to it
    Budget,        // MaxExpansions reached with both ends still open: undecided
    Smoothing,     // a lattice path was found but no smoothed or straight-legged version of it swept free
};

/** One front of a search: what it reached (diagnostics of a failed search). */
struct UNREALCITIZEN_API FSCCargoRouteFront
{
    int32 Expansions = 0;
    FBox Reached = FBox(ForceInit);         // bounds of the expanded nodes
    FVector Nearest = FVector::ZeroVector;  // the expanded node nearest the other end
    int32 BlockedCells = 0, BlockedSteps = 0; // rejected neighbours: the shape overlaps there / its step there hits
};

struct UNREALCITIZEN_API FSCCargoRoute
{
    TArray<FVector> Waypoints; // after shortcutting
    TArray<FVector> Samples;   // the spline, every ~SampleCm
    bool bDirect = false;      // the straight line was free
    bool bFound = false;       // false: no free route (nothing may fly Samples then)
    int32 Expansions = 0;
    ESCCargoRouteEnd End = ESCCargoRouteEnd::None;
    FSCCargoRouteFront FromStart, FromGoal;
    /** A found route's measured clearance (MinClearance of Samples) and hits, measured by the search itself. */
    double Clearance = 0;
    int32 Hits = 0;
    /** Plan from Start to Goal (both must be free); found routes sweep free sample to sample. */
    static FSCCargoRoute Plan(const FVector& Start, const FVector& Goal, const FSCCargoRouteQuery& Query);
    /** Centripetal Catmull-Rom through Points, sampled about every StepCm (the points themselves included). */
    static TArray<FVector> Spline(const TArray<FVector>& Points, double StepCm);
    static const TCHAR* EndName(ESCCargoRouteEnd End);
    double Length() const;
    /** Clearance of the flying shape along Samples (every ~StepCm): the least margin by which it could grow without
     *  touching anything (from 0, 10, 25, 50, 100, 200 cm; 200 = at least that), and OutHits: samples overlapping, plus
     *  consecutive samples whose sweep hits. Needs Query.BlockedBy and Query.Free. */
    static double MinClearance(const TArray<FVector>& Samples, const FSCCargoRouteQuery& Query, int32& OutHits, double StepCm = 100);
    /** A start out of the blocked space a load stopped in (a box placed since its route was planned): the nearest free
     *  point along each way out in turn, at most MaxCm away. Under gravity up first (the hangar convention); then out
     *  along OutNormal (the destination plate's normal), back the way it came (Back), then the remaining axes. True with
     *  OutStart = From when From is free; false (OutStart = From) when no way out is free within reach. */
    static bool EscapeStart(const FVector& From, const FSCCargoRouteQuery& Query, bool bGravity, const FVector& OutNormal, const FVector& Back,
                            FVector& OutStart, double MaxCm = 300, double StepCm = 25);
};

/** The clearance of a route, a sample at a time (FSCCargoRoute::MinClearance; a search measures its route so, within its
 *  frame budget). */
struct UNREALCITIZEN_API FSCCargoClearance
{
    double Min = 200, Since = 0;
    int32 Hits = 0;
    void Begin(double StepCm);
    void Sample(const TArray<FVector>& Samples, int32 I, const FSCCargoRouteQuery& Query, double StepCm);
};

/** The stretch of the segment A..B inside Box, as fractions of the segment (0 = A, 1 = B): false when none of it is.
 *  A query with one margin inside a zone and another outside splits a sweep there. */
UNREALCITIZEN_API bool SCCargoSegmentInside(const FVector& A, const FVector& B, const FBox& Box, double& OutEnter, double& OutLeave);

/** A route search spread over frames: Begin, then Step with a time budget until it returns true; Result is then what
 *  FSCCargoRoute::Plan returns for the same inputs (Plan runs one search to the end). */
struct UNREALCITIZEN_API FSCCargoRouteSearch
{
    FVector Start = FVector::ZeroVector, Goal = FVector::ZeroVector;
    FSCCargoRoute Result;
    // Cost: time spent in Step in all, in the longest single Step (the frame cost), and the number of Steps.
    double Seconds = 0, MaxStepSeconds = 0;
    int32 Steps = 0;

    void Begin(const FVector& InStart, const FVector& InGoal, const FSCCargoRouteQuery& InQuery);
    /** Search for about BudgetSeconds (at least one neighbour of a node, one stretch of a sweep); true once Result is final. */
    bool Step(double BudgetSeconds);
    bool IsDone() const { return Phase == EPhase::Done; }

private:
    enum class EPhase : uint8 { Ends, Search, Shortcut, Smooth, Clearance, Done };
    struct FOpen { double F; int64 K; bool operator<(const FOpen& O) const { return F < O.F; } };
    // A front: its root (the start or the goal itself, off the lattice) and the lattice nodes reached from it.
    struct FFront
    {
        FVector Root = FVector::ZeroVector;
        FIntVector Base = FIntVector::ZeroValue; // the root's half cell
        TMap<int64, double> G;                   // cost from the root (cm)
        TMap<int64, int64> Parent;
        TSet<int64> Closed;
        TArray<FOpen> Open;
        double NearestSq = TNumericLimits<double>::Max();
    };
    FSCCargoRouteQuery Q;
    EPhase Phase = EPhase::Done;
    FVector Lo = FVector::ZeroVector, Hi = FVector::ZeroVector;
    double FineCm = 50;
    FFront Fronts[2];
    TMap<int64, bool> Blocked;
    int32 Turn = 0;
    int64 MeetStart = 0, MeetGoal = 0; // where the fronts met: the node on the start's side and on the goal's
    bool bMet = false;
    // A long sweep taken a stretch at a time (a sweep across a hangar costs a frame by itself), and the steps after the
    // fronts met, resumable too: the shortcut (from each kept point, the farthest later point swept to freely), then the
    // smoothed or straight-legged samples swept one by one.
    FVector SweepA = FVector::ZeroVector, SweepB = FVector::ZeroVector;
    double SweepAt = -1; // < 0: none under way
    TArray<FVector> Raw, Short;
    int32 ShortI = 0, ShortJ = 0, Variant = 0, SampleI = 1;
    enum class ESweep : uint8 { Pending, Free, Hit };
    ESweep StepSweep(const FVector& A, const FVector& B, double Until);
    bool StepInner(double Until);
    bool StepShortcut(double Until);
    bool StepSmooth(double Until);
    bool StepClearance(double Until);
    FSCCargoClearance Clear;
    int32 ClearI = 0;
    void MakeSamples();
    FVector NodePos(int64 K) const;
    bool CellBlocked(int64 K, const FIntVector& C, const FVector& W);
    /** Expands the best open node of a front (resumable: Pending while its neighbours are still being tried); Exhausted
     *  when the front has none left (its end is enclosed). */
    enum class EExpand : uint8 { Expanded, Pending, Exhausted };
    bool bMid = false, bCurRoot = false, bObstructed = false; // an expansion under way: its node and how far it got
    int64 CurK = 0;
    FIntVector CurCell = FIntVector::ZeroValue;
    FVector CurFrom = FVector::ZeroVector;
    double CurCost = 0;
    int32 Pass = 0, Neighbour = 0;
    EExpand Expand(int32 Side, double Until);
    void BeginFinish();
};

/** Motion along a sampled path: arclength, speed and acceleration with speed / acceleration / jerk limits. */
struct UNREALCITIZEN_API FSCCargoFlight
{
    TArray<FVector> Points;
    TArray<double> S;    // arclength at each point (cm)
    TArray<double> VCap; // speed cap at each point: curvature and braking to rest at the end
    double MaxSpeed = 0, MaxAccel = 0, MaxJerk = 0; // cm/s, cm/s^2, cm/s^3
    double Dist = 0, Speed = 0, Accel = 0;
    double SpeedLimit = 0; // > 0: a lower speed cap for now (reached at the acceleration and jerk limits)
    double StopAt = -1;    // >= 0: brake to rest by this arclength (the stretch beyond is not known free); < 0: none
    double HoldAt = -1;    // >= 0: the flight ends at this arclength for now, as at the path's end (never beyond it); < 0: none

    /** Start at rest on Path (at least two points). */
    void Start(const TArray<FVector>& Path, double InMaxSpeed, double InMaxAccel, double InMaxJerk);
    /** Advance by Dt; true once at the end and at rest. */
    bool Step(double Dt);
    bool Done() const { return Points.Num() < 2 || (Dist >= Length() - 0.5 && Speed < 1); }
    /** Started on a route (even one of zero length: a one-point Start is a flight that has arrived); an unstarted flight
     *  is "no route yet": its drone hovers where it is and plans again. */
    bool HasRoute() const { return Points.Num() >= 2; }
    double Length() const { return S.IsEmpty() ? 0 : S.Last(); }
    FVector Position() const { return At(Dist); }
    FVector Tangent() const;
    FVector At(double Distance) const;
    double CapAt(double Distance) const;
    /** Seconds to fly a path of Length at rest-to-rest with these limits (trapezoid; ETA). */
    static double Seconds(double Length, double MaxSpeed, double MaxAccel);
};

/** Gusts in atmosphere: smooth band-limited noise, three sines per axis with weights (0.55, 0.3, 0.15). Each Seed (a
 *  drone) has its own process: its components' frequencies are FrequencyHz x (1, 1.73, 2.91), each shifted by up to
 *  half an octave either way, with its own phases and 0.7 to 1 of the amplitude, so teammates sway independently.
 *  Offset: per axis within +-AmplitudeCm (vertical half); Tilt (pitch, roll degrees) within +-TiltDeg. */
struct UNREALCITIZEN_API FSCCargoSway
{
    static FVector Offset(uint32 Seed, double T, double AmplitudeCm, double FrequencyHz);
    static FVector2D Tilt(uint32 Seed, double T, double TiltDeg, double FrequencyHz);
};

/** Acceleration a load allows: (N F - m g) / (N m_drone + m), capped by the empty drones' own limit. */
UNREALCITIZEN_API double SCCargoLoadedAccel(int32 Drones, double LiftForceN, double LoadKg, double DroneKg, double GravityMS2, double EmptyAccelMS2);
