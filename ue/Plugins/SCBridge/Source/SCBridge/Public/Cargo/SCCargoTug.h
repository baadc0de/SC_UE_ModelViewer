// Tugging a box by tractor beams (WP-K1, hover rework). SI units throughout (m, s, kg, N). No engine objects: the
// subsystem feeds it poses and applies its forces (SCCargoTugStep.cpp); the tests run it against a point mass.
//
// A beam is a spring-damper between its anchor and the attach point on the box. The anchor is the point the beam
// holds: the spring's rest length out from the drone's emitter along the beam's aim. The drone aims it and sets the
// rest length from where its thrust holds it, so the anchor is where the drone means it; what moves the drone without
// its knowing (its gusts) moves the anchor with it.
// Its numbers come from the tractor record (FSCCargoDroneDefinition): the force is capped at MaxForceN up to the
// full-strength distance and falls to nothing at the maximum distance; the stiffness is MaxForceN / MinDistanceM (the
// beam gives its whole force stretched by its own minimum length: the record has no stiffness, this is the viewer's
// reading of it); the damping is critical for the mass the beam carries. The rest length is the drone's to set,
// within the record's [MinDistanceM, MaxDistanceM]: reeling in and paying out is each drone's own decision.
//
// What is shared by a team is only the force: FSCCargoTug::Shares splits the force the route asks for among the
// attach points (no moment about the box's centre). Each drone then decides for itself (FSCCargoTug::Station) how
// long its beam is, the rest length that gives its share, and where it hovers: as far out as its beam keeps full
// strength and the room around it allows, closer under a ceiling. FSCCargoDroneBody is the drone's own inertia:
// thrust-limited (its thrust also carries the beam's reaction) and jerk-limited. On its way to its station it keeps
// its beam on the box by re-aiming it and paying it out or reeling it in.
//
// Keeping clear is each drone's own as well (FSCCargoDroneSteer: out of a load's way, apart from the other drones,
// slid along what its shape meets, in its bay until its way out is free), and so is coming in ahead of a lintel
// (FSCCargoTug::RoomAhead), and flying out beside a teammate (Beside). What the planner adds is space: FSCCargoKeepOut
// is what route queries keep for the drones of other teams.
#pragma once
#include "CoreMinimal.h"

struct SCBRIDGE_API FSCCargoBeam
{
    double MaxForceN = 0, MinM = 0, FullM = 0, MaxM = 0;
    /** The force the beam can give at this length (emitter to attach point). */
    double CapN(double LengthM) const;
    double StiffnessNPerM() const;
    /** The stretch that gives ForceN. */
    double StretchM(double ForceN) const { return ForceN / FMath::Max(StiffnessNPerM(), 1e-9); }
    /** A rest length the record allows. */
    double ClampRestM(double RestM) const { return FMath::Clamp(RestM, MinM, FMath::Max(MinM, MaxM)); }
    /** The beam's pull on the box. ToAnchor: from the attach point to the anchor; Rate: the anchor's velocity less the
     *  attach point's; LengthM: emitter to attach point (for the cap); MassKg: what the beam carries; HoldN: the force
     *  the drone set its beam for (its share). Spring and critical damping around HoldN, taken implicitly over DtS
     *  (stable at any step; HoldN exactly when the stretch is the one it was set for), capped by CapN. */
    FVector ForceN(const FVector& ToAnchor, const FVector& Rate, double LengthM, double MassKg, double DtS, const FVector& HoldN) const;
};

struct SCBRIDGE_API FSCCargoTug
{
    /** Shares of a force applied at the box's centre for attach points at Along (m, along a line through the centre):
     *  they sum to 1 and leave no moment; the least-squares split (equal for points placed symmetrically). */
    static TArray<double> Shares(const TArray<double>& Along);
    /** The way a drone's beam pulls (the side of the box its station is on): along ShareN, the force its share of the
     *  load asks for (N); with under 1 N of it (zero g, the load at rest or drifting) towards the drone, the side it
     *  holds (AttachToDrone); with neither, up. */
    static FVector PullDirection(const FVector& ShareN, const FVector& AttachToDrone)
    {
        constexpr double MinForceN = 1.0; // next to nothing against a box's mass (viewer choice)
        if (ShareN.SizeSquared() >= MinForceN * MinForceN) return ShareN.GetSafeNormal();
        return AttachToDrone.GetSafeNormal(1e-6, FVector::UpVector);
    }

    struct FStation
    {
        FVector Dir = FVector::UpVector; // the beam: from the attach point to the emitter
        double LengthM = 0, RestM = 0;   // the beam's length at its share, the rest length the drone sets
        FVector Hover = FVector::ZeroVector, HoverV = FVector::ZeroVector; // where the emitter is to be, and how it moves
        FVector AnchorWant = FVector::ZeroVector; // where the anchor is to be: the stretch for the share beyond the attach point's place
        /** The rest length the drone sets from where its thrust holds it (Body): its distance to where the anchor is to
         *  be, within the record's range. On station that is RestM. */
        double RestAt(const struct FSCCargoBeam& Beam, const FVector& Body) const;
        /** The anchor as it is: that rest length along the aim from Body, out from where the emitter really is. */
        FVector Anchor(const struct FSCCargoBeam& Beam, const FVector& Body, const FVector& Emitter) const;
        /** Keeps the drone's shape (ShapeHalf, level, its centre ShapeAt from the emitter) off its own box: beyond
         *  the plane of the face its attach point lies on (Attach, outward normal FaceN). Over a box hanging
         *  upright that holds at any beam length; beside the face of a box hanging on its side (carried off an
         *  upright plate as it stood) the hover point moves out from the face by the shape's reach, and the beam
         *  slants. */
        void KeepOffFace(const FVector& FaceN, const FVector& Attach, const FVector& ShapeAt, const FVector& ShapeHalf);
    };
    /** A drone's own station for giving ShareN (world, on the box) with RoomM free along the beam from where the attach
     *  point should be (WantAttach, moving at WantV): the beam along its share, as long as it keeps full strength and
     *  the room allows, never with a rest length under the record's minimum; the emitter that far out. */
    static FStation Station(const FSCCargoBeam& Beam, const FVector& ShareN, double RoomM, const FVector& WantAttach, const FVector& WantV);

    /** The room a drone allows itself along its beam now, looking along the route it flies: RoomM[k] is the room over
     *  the route k * StepM on from its first stretch, which lies BehindM behind the drone (its hull's rear; its
     *  front meets a stretch that much before the drone is over it, so a stretch counts as k StepM - 2 BehindM away,
     *  the nearest as here). The route can bring it there no sooner than t(k): from SpeedMS, at AccelMS2 up to
     *  MaxSpeedMS. A drone comes in onto a shorter beam as a critically damped body at RateHz (its
     *  thrust's swing rate, jerk / acceleration, after one such swing): a beam LongestM long is within SlackM of
     *  room[k] after ComeInS(LongestM - room[k]). So it takes the room of every stretch it could reach within the
     *  time it needs to come in for it, and the least of those: it tucks in before a lintel, not under it, and no
     *  earlier than it has to. */
    static double RoomAhead(TConstArrayView<double> RoomM, double StepM, double SpeedMS, double AccelMS2, double MaxSpeedMS, double RateHz, double SlackM, double LongestM, double BehindM = 0);
    /** The time that body needs to come in by ExcessM to within SlackM: x with (1 + x) e^-x = slack / excess, one
     *  swing more, over the rate. */
    static double ComeInS(double ExcessM, double RateHz, double SlackM);
    /** How far ahead that look has to reach for a beam that can be RangeM longer than the room (m). */
    static double LookAheadM(double RangeM, double SpeedMS, double AccelMS2, double MaxSpeedMS, double RateHz, double SlackM);
};

/** Space kept for drones in route queries: where the drones of other teams hover or will take their stations (boxes,
 *  world axes). */
struct SCBRIDGE_API FSCCargoKeepOut
{
    TArray<FBox> Boxes;
    bool IsEmpty() const { return Boxes.IsEmpty(); }
    /** Whether a box of Half at Rotation, its centre swept from A to B, meets any of it. A kept box is taken by its
     *  bounds in the swept box's frame (exact for a level box turned about the vertical but for the kept box's own
     *  corners). */
    bool Hits(const FVector& A, const FVector& B, const FQuat& Rotation, const FVector& Half) const;
    /** Whether a load flown along Route (its box centre; Half: half its size at Rotation; its drones in the column
     *  Above its top) passes through Point, taken PadCm larger on every side. OutLast: the last leg of Route on which
     *  it does (the load is past the point once it has flown that leg). */
    static bool RoutePasses(TConstArrayView<FVector> Route, const FQuat& Rotation, const FVector& Half, double Above, const FVector& Point, double PadCm, int32* OutLast = nullptr);
};

/** A drone's step through the world, its own decision each tick (any unit of length). It keeps clear of the other
 *  drones it can see (their positions and velocities now) and never passes through what its shape is swept against. */
struct SCBRIDGE_API FSCCargoDroneSteer
{
    struct FOther
    {
        FVector P = FVector::ZeroVector, V = FVector::ZeroVector; // where it is, how it moves
        double Radius = 0;                                        // its hull's bounding radius
    };
    /** The distance two drones keep between their centres: their hulls' radii and half of the smaller again. */
    static double Keep(double RadiusA, double RadiusB) { return RadiusA + RadiusB + 0.5 * FMath::Min(RadiusA, RadiusB); }
    /** The step From -> To changed so that the drone ends no nearer than Keep to where each other drone is now or
     *  will be after DtS (the stretch it flies in that time). One that it is closing on is passed on the right (both
     *  drones follow the same rule, so they turn away from each other); the push is out of the other's way, level
     *  when the two are level. Returns the new end point, no further from From than MaxStep: a drone turns aside
     *  at its own pace, it is not thrown (over a few steps it is the kept distance away). */
    static FVector Separate(const FVector& From, const FVector& To, double Radius, TConstArrayView<FOther> Others, double DtS, double MaxStep = TNumericLimits<double>::Max());
    /** A sweep of the drone's shape: true when blocked, with the fraction of A -> B flown free and the surface normal. */
    using FSweep = TFunctionRef<bool(const FVector& A, const FVector& B, double& OutFraction, FVector& OutNormal)>;
    /** The step From -> To against Sweep: flown as far as it is free; what is left slides along the surface it met
     *  (its part across the normal), as far as that is free. Never beyond a hit. bOutBlocked: something was met. */
    static FVector Slide(const FVector& From, const FVector& To, FSweep Sweep, bool& bOutBlocked);
    /** How far along Path (from its first point: where the drone is) the drone can fly before it comes within Keep of
     *  one of Others, each taken along the stretch it flies in LeadS (a drone in its dock's corridor: the corridor).
     *  The path's length when none is in the way; 0 when the drone would have to be elsewhere already. A drone waits
     *  there: in its bay (two bays nearer than the kept distance launch one after the other) or short of a dock. */
    static double FreeAlong(TConstArrayView<FVector> Path, double Radius, TConstArrayView<FOther> Others, double LeadS);
    /** A teammate's way out to the same box: from From to To beside Route (the route of the teammate planned first),
     *  Apart to its side all the way between (the side To lies on; level, across the route as it goes), eased away
     *  from it over RampCm after From and back onto To over as much before it. It keeps the route's shape, so it
     *  is as smooth; the caller sweeps it. Empty for a route of less than two points. */
    static TArray<FVector> Beside(TConstArrayView<FVector> Route, const FVector& From, const FVector& To, double Apart, double RampCm);
    /** Where the drone's centre goes instead of To to be off its own box (Pose, Half), the box it carries: when its
     *  hull (HullHalf, level) is over the face its beam holds (the box's own +z) and nearer to it than the hull
     *  reaches, out along that face's normal until it is not. A box in the air sways and lags its route, up to
     *  the drone on a short beam; the drone gives way at once. To itself when it is clear. */
    static FVector OffFace(const FVector& To, const FVector& HullHalf, const FTransform& Pose, const FVector& Half);
    /** A box in the air that is not the drone's own: its pose, half size and velocity. */
    struct FLoad
    {
        FTransform Pose = FTransform::Identity;
        FVector Half = FVector::ZeroVector, V = FVector::ZeroVector;
    };
    /** Where the drone goes instead of To so that its hull (HullHalf, level) is clear of every load, where it is and
     *  where it will be by the time the drone can be out of its way: sideways (level, across the load's velocity:
     *  over a load its own drones fly, under it there is seldom room) by the shortest way, which takes it 2 sqrt(way / Accel) from rest to rest (and a quarter of a second to notice);
     *  a load further off than it flies in that time is left to come nearer. Any side of a load that stands. To
     *  itself when it is clear. */
    static FVector Evade(const FVector& To, const FVector& HullHalf, TConstArrayView<FLoad> Loads, double Accel);
};

/** A drone as a body: flies to a hover point that itself moves, with what thrust is left after the beam's reaction and
 *  its weight, and a limited rate of change of acceleration. */
struct SCBRIDGE_API FSCCargoDroneBody
{
    FVector P = FVector::ZeroVector, V = FVector::ZeroVector, A = FVector::ZeroVector; // m, m/s, m/s^2
    double MassKg = 1, MaxAccelMS2 = 0, MaxJerkMS3 = 0, MaxForceN = 0, GravityMS2 = 0;
    /** One step towards Hover (moving at HoverV); ReactionN is the beam's pull on the drone. */
    void Step(const FVector& Hover, const FVector& HoverV, const FVector& ReactionN, double DtS);
    /** The acceleration left for manoeuvring along Dir while holding ReactionN (thrust limit m (g + a_max) + F_max). */
    double AccelLeft(const FVector& Dir, const FVector& ReactionN) const;
};
