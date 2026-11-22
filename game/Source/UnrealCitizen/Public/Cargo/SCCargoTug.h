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
// What is shared by a team is only the force. Until round 4 FSCCargoTug::Shares split it among the attach points and
// each drone chose its station along its share at the beam length the room allowed (FSCCargoTug::Station, RoomAhead;
// kept and tested, no longer flown: the team's stations swung together with the route's acceleration). Since round 5
// each beam gives an equal share at the box as a whole and the drone holds from a place fixed in the box's frame
// (FSCCargoEndGrip, FSCCargoGripSpot): the rest length gives its share. FSCCargoDroneBody is the drone's own inertia:
// thrust-limited (its thrust also carries the beam's reaction) and jerk-limited. On its way to its station it keeps
// its beam on the box by re-aiming it and paying it out or reeling it in.
//
// Keeping clear is each drone's own as well (FSCCargoDroneSteer: out of a load's way, apart from the other drones,
// slid along what its shape meets, in its bay until its way out is free), and so is flying out beside a teammate
// (Beside). What the planner adds is space: FSCCargoKeepOut
// is what route queries keep for the drones of other teams.
//
// Round 5: the beam holds to a point (any direction, up to the record's force); a drone may hold its box from its ends
// (FSCCargoEndGrip, the low way), beside it or over it, and re-picks its place now and then (FSCCargoGripPick).
#pragma once
#include "CoreMinimal.h"

struct UNREALCITIZEN_API FSCCargoBeam
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

struct UNREALCITIZEN_API FSCCargoTug
{
    /** Whether a load waits short of its destination's precise zone (round 5): while drones of other moves are in it
     *  (they have just set a box down there; inside, beside boxes standing flush, there is no room to step aside), it
     *  stops once its way into the zone is GateCm off or nearer (DistToZoneCm < 0: no zone ahead), never once inside. */
    static bool WaitsForZone(double DistToZoneCm, double GateCm, bool bInZone, bool bOccupied)
    {
        return bOccupied && !bInZone && DistToZoneCm >= 0 && DistToZoneCm <= GateCm;
    }
    /** Whether a lone drone holds short for a load planned before its move (HoldShort): not for one waiting at its gate
     *  (bLoadWaitsForZone), nor for one still outside its hold while the drone is in that hold or the load's way into it
     *  within the gate (bDroneInGate): that load waits for the drone instead (WaitsForZone counts it), and the drone goes
     *  on out. So neither waits on the other: the load waits only for drones in its gate, which never wait for it. */
    static bool DroneWaitsForLoad(bool bLoadWaitsForZone, bool bDroneInGate, bool bLoadInZone)
    {
        return !bLoadWaitsForZone && !(bDroneInGate && !bLoadInZone);
    }
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
        FVector HoverA = FVector::ZeroVector; // ... and how that accelerates (round 5: its route's; fed forward)
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
    /** How close a drone's emitter may come to the face its beam holds (cm): its hull's reach below the emitter (the
     *  emitter's height over the hull's bottom, none when the emitter is lowest) and the gap it keeps from its box. */
    static double HullHangCm(double EmitterZCm, double HullBottomZCm, double GapCm) { return FMath::Max(0.0, EmitterZCm - HullBottomZCm) + GapCm; }
    /** How far a drone's hull reaches above its emitter (cm): what a load's route keeps clear over the beam's hang for
     *  its drones (none when the emitter is highest). */
    static double HullAboveCm(double EmitterZCm, double HullTopZCm) { return FMath::Max(0.0, HullTopZCm - EmitterZCm); }
    /** The shortest beam (cm) a drone flies at a share needing StretchCm: the larger of the record's minimum rest length
     *  and the drone's hull hang (HullHangCm), plus that stretch. */
    static double ShortestCm(double RecordMinCm, double HullHangCm, double StretchCm) { return FMath::Max(RecordMinCm, HullHangCm) + StretchCm; }
};

/** A tractor beam holds what it holds at a point (WP-K4 round 5, the multitool's tractor: SWeaponActionFireTractor-
 *  BeamParams): a spring-damper from the box to the hold point the drone keeps, with force in any direction up to the
 *  record's maxForce, inside its cone (maxAngle) and between its minDistance and fullStrengthDistance. A team's beams
 *  each give an equal share of what the route asks (weight, acceleration, drag), applied to the box as a whole; its
 *  rotation is the beams' angular hold (TugStep's attitude spring). So a drone may hold its box from wherever it fits:
 *  in front of it at an end, beside it, over it. All lengths cm in the box's frame (its long axis X or Y, Z up), forces N,
 *  accelerations m/s^2.
 *
 *  FSCCargoEndGrip is the geometry and force rule of holding a box from its ends, the low way to carry it: one drone at
 *  one end (a lone drone: the end that trails its route, so it pushes, pulling in reverse where the route doubles back),
 *  or a pair at both (one pushes, one pulls). A drone's hull, at any heading, clears the end face; its top is at the
 *  box's top; its beam, level or down, reaches the end face within the record's cone and distances. Larger teams are
 *  held over (no scenario sizes one; documented in WP-K4). */
struct UNREALCITIZEN_API FSCCargoEndGrip
{
    /** The drone as the grip sees it (level, cm): its hull's half size (square in plan: it covers every heading), how far
     *  the hull's vertical axis can be from the emitter in plan (the emitter's offset, any heading), and how far the
     *  hull reaches above and below the emitter. */
    struct FDrone
    {
        FVector HullHalf = FVector::ZeroVector;
        double EmitterPlanCm = 0, AboveCm = 0, BelowCm = 0;
    };
    struct FIn
    {
        FSCCargoBeam Beam;
        double MassKg = 0, GravityMS2 = 0;
        int32 Team = 0;
        FVector BoxHalfCm = FVector::ZeroVector;
        FDrone Drone;
        double GapCm = 2;        // kept between a hull and the box's face
        double InsetCm = 10;     // an attach point keeps this far from the face's edge
        double MinAccelMS2 = 0;  // the least acceleration a loaded team keeps (as RouteAccel)
        double MaxAngleDeg = 90; // the record's cone (maxAngle): how far below level the hold may point
    };
    bool bValid = false;
    FString Why; // why not, when not valid
    int32 Axis = 0; // the box's long axis: 0 X, 1 Y
    int32 Team = 0;
    double AngleRad = 0;   // the hold's angle below level at rest
    double BeamCm = 0, OutCm = 0, RiseCm = 0, AttachZCm = 0; // emitter to attach point; out from the face; above it; its height
    double ClearCm = 0;    // how far out from a face an emitter must be for the hull to clear it at any heading
    double ShareN = 0, ForceN = 0; // per beam at rest: the weight share (vertical) and the whole (the same)
    double MaxAngleDeg = 90, MinCm = 0, FullCm = 0; // the record's cone and distances, kept for the grip spots
    /** The largest route acceleration (any direction) the team takes with every beam within the record's force. */
    double MaxAccelMS2 = 0;
    double InsetCm = 10, GapCm = 2;
    FVector BoxHalf = FVector::ZeroVector;
    FDrone Drone;

    static double Sign(int32 End) { return End ? 1.0 : -1.0; }
    FVector AxisDir() const { return Axis ? FVector::RightVector : FVector::ForwardVector; }
    /** Drone End's attach point on its end face (End 0: the -axis face, 1: +axis) and its emitter's station. */
    FVector Attach(int32 End) const { return AxisDir() * (Sign(End) * BoxHalf[Axis]) + FVector(0, 0, AttachZCm); }
    FVector Station(int32 End) const { return Attach(End) + AxisDir() * (Sign(End) * OutCm) + FVector(0, 0, RiseCm); }
    /** Where a drone's hull can be with its emitter at StationCm, at any heading. */
    FBox HullAt(const FVector& StationCm) const;
    /** The box and both ends' drones' hulls on station. */
    FBox Shape() const;
    /** A beam's force for the team's NetN (the route's whole ask of the beams): an equal share, in any direction. */
    FVector Force(const FVector& NetN) const { return NetN / FMath::Max(Team, 1); }
    /** The hold's line on station (unit, box frame): from the attach point to the emitter. */
    FVector Line(int32 End) const { return (Station(End) - Attach(End)).GetSafeNormal(); }
    static FSCCargoEndGrip Plan(const FIn& In);
    /** The largest acceleration (m/s^2, its magnitude, any direction) with each beam's share within Beam's force. */
    double AccelLimit(const FSCCargoBeam& Beam, double MassKg, double GravityMS2) const;
    /** A drone's way from one station to another about the box (box frame), never through it: up to the higher of the
     *  two heights where it is, across at that height, down to where it goes. Over the box a station is higher than the
     *  hull's reach over its top; beyond an end face its hull clears that face; so the way does too. (Between two side
     *  or end spots low beside the box: the corner it passes is beyond both faces' reach.) */
    static TArray<FVector> SwapWay(const FVector& From, const FVector& To, double MinZ = -1e9);
    /** The height a hull clears the box's top at (its emitter's, box frame): a way between two places beside the box
     *  goes over it at least this high (SwapWay's MinZ), never round a corner it might cut. */
    double OverZ() const { return BoxHalf.Z + Drone.BelowCm + GapCm; }
    /** A drone's way round the box from one place to another (box frame): SwapWay, over the top (OverZ) only where the
     *  straight way across in plan would pass the box within the hull's reach below that height. */
    TArray<FVector> WayRound(const FVector& From, const FVector& To) const;
    /** The point of the box's surface nearest to P (box frame, P outside it), InsetCm in from the edges of its face. */
    FVector SurfaceNear(const FVector& P) const;
    /** The outward normal of the face P is furthest beyond (box frame): the face a drone there keeps off. */
    FVector FaceNear(const FVector& P) const;
};

/** A place a carrying drone holds its box from (WP-K4 round 5), in the box's frame: in front of an end face, beside a
 *  side face, or over the top face; its emitter's station and the hold's point on the box. */
struct UNREALCITIZEN_API FSCCargoGripSpot
{
    enum class EKind : uint8 { End, Side, Over };
    EKind Kind = EKind::End;
    int32 Face = 0; // End / Side: 0 the - face, 1 the + face (of the long / the other plan axis); Over: 0
    FVector Station = FVector::ZeroVector, Attach = FVector::ZeroVector;
    bool bPlanned = false; // the move's planned place (inside its route's shape: the fallback)
    /** The face's outward normal (box frame). */
    FVector FaceNormal(int32 LongAxis) const;
    bool SameFace(const FSCCargoGripSpot& O) const { return Kind == O.Kind && Face == O.Face; }
};

/** A drone searching for a good grip (WP-K4 round 5): every so often it re-picks where it holds its box from, among the
 *  viable places around it (the caller filters by the space there along the route ahead), weighted: ends mostly (in
 *  front or behind), sometimes over or beside; spread over each face (across it, in height, in reach), never where a
 *  teammate is, and rarely on a teammate's face (so a team's grips balance the load). Its numbers are drone_art.json's
 *  "grip" block (viewer choices). Pure. */
struct UNREALCITIZEN_API FSCCargoGripPick
{
    struct FCfg
    {
        float EndWeight = 0.75f, SideWeight = 0.1f, OverWeight = 0.15f; // the weights of the kinds of place (renormalised over the viable ones)
        float SameFaceWeight = 0.15f; // a teammate on that face already: its weight times this
        float Spread = 0.6f;          // across a face: up to this share of its half width either way
        float ExtraReach = 0.5f;      // out from a face: up to this share more than the hull's clearance (within full strength)
        float ExtraHeightCm = 30.f;   // up to this much higher than the lowest (ends, sides: the hull's top at the box's top; over: the shortest hang)
        float IntervalMinS = 3.f, IntervalMaxS = 7.f; // how long a drone keeps a place before it looks again (each its own draw)
        float LookaheadS = 3.f;       // a place is viable while the drone's hull fits there along this much of the route ahead
        float MoveSpeedScale = 0.35f; // it moves round the box to a new place at this share of its top speed
        // After its first grip, a re-pick moves it no further round the box than this (along its way there, WayRound): this
        // share of the box's longest side, within the two bounds (the owner: "when drones pick new positions aside from
        // initial, limit max distance to new position, so they don't shift too far").
        float MaxRepickMoveFrac = 0.3f, MaxRepickMoveMinCm = 150.f, MaxRepickMoveMaxCm = 300.f;
    };
    /** PerKind places of each kind round a box held like G (its geometry, the record's cone and distances), the over
     *  hang HangCm from the top face to the emitter (the shortest that holds). Every one clears the box. */
    static TArray<FSCCargoGripSpot> Candidates(const FSCCargoEndGrip& G, double HangCm, const FCfg& Cfg, FRandomStream& Rng, int32 PerKind);
    /** One of Viable by the weights (Teammates: where the others hold or are going; none nearer than KeepCm); -1: none. */
    static int32 Pick(TConstArrayView<FSCCargoGripSpot> Viable, TConstArrayView<FSCCargoGripSpot> Teammates, double KeepCm, const FCfg& Cfg, FRandomStream& Rng);
    /** Everywhere a team's drones can be while they search round a box held like G (box frame): the box and every place
     *  Candidates draws (over hanging HangCm), and the ways between them. What others keep clear of for it. */
    static FBox Reach(const FSCCargoEndGrip& G, double HangCm, const FCfg& Cfg);
    /** How far a re-pick may move a drone round a box whose longest side is LongestCm (cm, along its way). */
    static double MoveCap(const FCfg& Cfg, double LongestCm)
    {
        return FMath::Clamp(LongestCm * Cfg.MaxRepickMoveFrac, double(Cfg.MaxRepickMoveMinCm), double(FMath::Max(Cfg.MaxRepickMoveMaxCm, Cfg.MaxRepickMoveMinCm)));
    }
    /** The length of a way (cm). */
    static double WayLength(TConstArrayView<FVector> Way)
    {
        double L = 0;
        for (int32 K = 1; K < Way.Num(); ++K) L += FVector::Dist(Way[K - 1], Way[K]);
        return L;
    }
    /** How far ahead (s) a team looks for its route's end or a precise zone before its drones go back to their planned
     *  places: one at a time (Astra, round 6: a pair returning at once read as a relocation), each in about the lookahead,
     *  so the lookahead once for each drone still away from its place. */
    static double ReturnAheadS(const FCfg& Cfg, int32 AwayFromPlanned) { return double(Cfg.LookaheadS) * FMath::Max(1, AwayFromPlanned); }
    /** The next interval (s): the configured range, the drone's own draw. */
    static double Interval(const FCfg& Cfg, FRandomStream& Rng) { return FMath::Lerp(double(Cfg.IntervalMinS), double(FMath::Max(Cfg.IntervalMaxS, Cfg.IntervalMinS)), double(Rng.FRand())); }
};

/** Space kept for drones in route queries: where the drones of other teams hover or will take their stations (boxes,
 *  world axes). */
struct UNREALCITIZEN_API FSCCargoKeepOut
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
    /** A load's shape for RoutePasses from everything round its box centre it can take up (Reach, box frame: its box
     *  and its drones anywhere round it, FSCCargoGripPick::Reach): Half about the centre (symmetric, so it covers the
     *  reach on every side) and the column Above Half's top that covers the rest of the reach upwards. */
    static void LegShape(const FBox& Reach, FVector& OutHalf, double& OutAbove);
};

/** A drone's step through the world, its own decision each tick (any unit of length). It keeps clear of the other
 *  drones it can see (their positions and velocities now) and never passes through what its shape is swept against. */
struct UNREALCITIZEN_API FSCCargoDroneSteer
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
     *  hull (HullHalf, level) is over the face its beam holds (Face: that face's outward normal in the box's frame, an
     *  axis: +z over the box, +-x or +-y holding it from an end) and nearer to it than the hull reaches, out along that
     *  face's normal until it is not. A box in the air sways and lags its route, up to the drone on a short beam; the
     *  drone gives way at once. To itself when it is clear (or Face is zero: between faces, on its way round). */
    static FVector OffFace(const FVector& To, const FVector& HullHalf, const FTransform& Pose, const FVector& Half, const FVector& Face = FVector::UpVector);
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
struct UNREALCITIZEN_API FSCCargoDroneBody
{
    FVector P = FVector::ZeroVector, V = FVector::ZeroVector, A = FVector::ZeroVector; // m, m/s, m/s^2
    double MassKg = 1, MaxAccelMS2 = 0, MaxJerkMS3 = 0, MaxForceN = 0, GravityMS2 = 0;
    /** One step towards Hover (moving at HoverV, accelerating at HoverA: fed forward, so a station riding a route is
     *  followed without the lag a spring alone leaves, a / W^2: 1.2 m at 7.8 m/s^2; round 5); ReactionN is the beam's
     *  pull on the drone. */
    void Step(const FVector& Hover, const FVector& HoverV, const FVector& ReactionN, double DtS, const FVector& HoverA = FVector::ZeroVector);
    /** The acceleration left for manoeuvring along Dir while holding ReactionN (thrust limit m (g + a_max) + F_max). */
    double AccelLeft(const FVector& Dir, const FVector& ReactionN) const;
};
