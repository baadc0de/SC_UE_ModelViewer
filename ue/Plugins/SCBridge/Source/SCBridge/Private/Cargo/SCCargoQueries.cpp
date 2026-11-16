// USCCargoSubsystem's collision queries for flying shapes (split from SCCargoPrepare.cpp): a lone drone; a hanging load =
// the box with its margin, and over it the column the beams and the drones fly in (turned with the box); the collision
// they test (every object type the ship and hangar block with, complex collision, the game colliders included); each
// query's identity (what it tests, where the boxes at rest are) for the caches and searches that depend on it; the
// swept turn and descent onto a slot. The drones are no colliders: the space kept for other teams' drones (KeptSpace)
// is tested beside the world by every query. And what a lone drone decides from the others: when to leave its bay and
// when to stop short of a dock (FlightFree), when to hold short of its station (HoldShort), which loads to keep out
// of the way of (DroneLoads).
#include "Cargo/SCCargoSubsystem.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoGridComponent.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "SCViewerSubsystem.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "GameFramework/PlayerController.h"

using namespace SCCargoFlightTuning;

namespace
{
FCollisionObjectQueryParams Obstacles() { return SCCargoObstacles(); }
// Once per actor: what its colliding components are (profile, object type), the evidence of what the queries test.
void LogCollisionOnce(const AActor* Actor, TSet<const AActor*>& Logged)
{
    if (!Actor || Logged.Contains(Actor)) return;
    Logged.Add(Actor);
    TMap<FString, int32> Counts;
    Actor->ForEachComponent<UPrimitiveComponent>(false, [&](const UPrimitiveComponent* C) {
        if (!C->IsCollisionEnabled()) return;
        Counts.FindOrAdd(FString::Printf(TEXT("%s/%s%s"), *C->GetCollisionProfileName().ToString(),
                                         *UEnum::GetValueAsString(C->GetCollisionObjectType()), C->ComponentHasTag(TEXT("SCGameCollision")) ? TEXT("/game") : TEXT("")))++;
    });
    for (const auto& [Key, Count] : Counts) UE_LOG(LogSCCargo, Display, TEXT("CARGO_COLLISION actor=%s profile=%s components=%d"), *Actor->GetName(), *Key, Count);
}
} // namespace

FSCCargoRouteQuery USCCargoSubsystem::RouteQuery(const FVector& HalfExtent, const FVector& Offset, const AActor* Carried, double MinZ, double MaxZ, bool bIgnoreDocks,
                                                const FQuat& Rotation, bool bIgnoreFlying, const TArray<const UPrimitiveComponent*>* IgnoreComponents,
                                                const FSCCargoMove* Move, const ASCCargoDrone* Drone, TConstArrayView<FVector> Ends) const
{
    UWorld* World = GetWorld();
    static TSet<const AActor*> Logged;
    if (const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World))
    {
        LogCollisionOnce(Viewer->GetVehicle(), Logged);
        LogCollisionOnce(Viewer->GetEnvironment(), Logged);
    }
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCCargoRoute), /*bTraceComplex=*/true, Carried);
    uint32 Ignored = 0; // who is left out, for the identity (a sum: the order does not matter)
    auto IgnoreActor = [&](const AActor* A) {
        if (!A) return;
        Params.AddIgnoredActor(A);
        Ignored += GetTypeHash(A->GetUniqueID());
    };
    for (const TWeakObjectPtr<ASCCargoDrone>& D : State->Drones) IgnoreActor(D.Get());
    if (const APlayerController* PC = World->GetFirstPlayerController(); PC && PC->GetPawn()) IgnoreActor(PC->GetPawn());
    // Right of way between flying loads (lower box id first): a load plans around loads that give way to it (they wait
    // where they are) and through those it gives way to (it waits while they are near, the guard in TickMove). A box held
    // out of the way hovers in place and stays an obstacle; drone routes ignore flying loads.
    int32 CarriedId = INDEX_NONE;
    for (const auto& Pair : State->Boxes)
        if (Carried && Pair.Value.Actor.Get() == Carried) CarriedId = Pair.Value.Id;
    for (const auto& Pair : State->Boxes)
        if (SCCargoIsFlying(Pair.Value.State) && Pair.Value.Actor.IsValid() && (bIgnoreFlying || CarriedId == INDEX_NONE || Pair.Value.Id < CarriedId))
            IgnoreActor(Pair.Value.Actor.Get());
    if (bIgnoreDocks) // a drone leaves and enters its dock's bay
        for (const FSCCargoDock& D : State->Docks) IgnoreActor(D.Actor.Get());
    if (IgnoreComponents)
        for (const UPrimitiveComponent* C : *IgnoreComponents)
        {
            Params.AddIgnoredComponent(C);
            Ignored += GetTypeHash(C->GetUniqueID()) * 31;
        }
    const FCollisionObjectQueryParams Types = Obstacles();
    const FCollisionShape Shape = FCollisionShape::MakeBox(HalfExtent);
    const TSharedRef<FSCCargoHullRule> Hulls = HullRule(Params, HalfExtent, Rotation);
    // The drones are no colliders: the space kept for other teams' (their stations, where they hover, the ones over a
    // load this query takes as an obstacle) is tested beside the world. It is not part of the identity (it changes as
    // teams come and go): a plan stopped by it alone is tried again (State->KeptHits counts those), the guard sweeps
    // the route again as it is flown, and a replan samples its cells afresh.
    const TSharedRef<FSCCargoKeepOut> Kept = KeptSpace(Move ? Move : MoveOf(Carried), Drone, CarriedId, bIgnoreFlying, Ends);
    const TSharedRef<int64> KeptHits = State->KeptHits;
    FSCCargoRouteQuery Q;
    const FQuat R = Rotation;
    Q.Blocked = [World, Hulls, Types, Shape, Offset, R, Kept, KeptHits, HalfExtent](const FVector& P) {
        if (World->OverlapAnyTestByObjectType(P + Offset, R, Types, Shape, Hulls->For(Hulls->Touched(P + Offset)))) return true;
        if (!Kept->Hits(P + Offset, P + Offset, R, HalfExtent)) return false;
        ++*KeptHits;
        return true;
    };
    Q.Free = [World, Hulls, Types, Shape, Offset, R, Kept, KeptHits, HalfExtent](const FVector& A, const FVector& B) {
        if (World->SweepTestByObjectType(A + Offset, B + Offset, R, Types, Shape, Hulls->For(Hulls->Touched(A + Offset) | Hulls->Touched(B + Offset)))) return false;
        if (!Kept->Hits(A + Offset, B + Offset, R, HalfExtent)) return true;
        ++*KeptHits;
        return false;
    };
    Q.Describe = [World, Hulls, Types, Shape, Offset, R, Kept, HalfExtent](const FVector& A, const FVector& B) {
        FHitResult Hit;
        if (!World->SweepSingleByObjectType(Hit, A + Offset, B + Offset, R, Types, Shape, Hulls->For(Hulls->Touched(A + Offset) | Hulls->Touched(B + Offset))))
            return FString(Kept->Hits(A + Offset, B + Offset, R, HalfExtent) ? TEXT("space kept for another team's drone") : TEXT("none"));
        const UPrimitiveComponent* C = Hit.GetComponent();
        return FString::Printf(TEXT("%s/%s%s%s"), Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("?"), C ? *C->GetName() : TEXT("?"),
                               C && C->ComponentHasTag(TEXT("SCHullCollision")) ? TEXT("(hull collider)") : C && C->ComponentHasTag(TEXT("SCGameCollision")) ? TEXT("(game collider)") : TEXT(""),
                               Hit.bStartPenetrating ? TEXT("(start inside)") : TEXT(""));
    };
    Q.BlockedBy = [World, Hulls, Types, HalfExtent, Offset, R](const FVector& P, double Inflate) {
        return World->OverlapAnyTestByObjectType(P + Offset, R, Types, FCollisionShape::MakeBox(HalfExtent + FVector(Inflate)), Hulls->For(Hulls->Touched(P + Offset)));
    };
    Q.CellCm = 100;
    Q.MarginCm = 3000;
    Q.MaxExpansions = 60000;
    Q.bRefine = false; // full cells; a load's searches refine as their last resort (FSCCargoMove::bRefine)
    Q.MinZ = MinZ;
    Q.MaxZ = MaxZ;
    // What these callbacks test: the shape, its rotation, who it leaves out, where the boxes at rest are and the world
    // they stand in (vehicles, mechanism poses). Caches and searches are not shared across any change of these: a cell
    // blocked by a box is free once that box is gone, a route through a ramp's opening is none once the ramp is shut.
    const FRotator Rot = R.Rotator();
    Q.Identity = FString::Printf(TEXT("%d,%d,%d@%d,%d,%d/%d,%d,%d/%u/%d%d/%08x/%08x/%08x"), FMath::RoundToInt32(HalfExtent.X), FMath::RoundToInt32(HalfExtent.Y),
                                 FMath::RoundToInt32(HalfExtent.Z), FMath::RoundToInt32(Offset.X), FMath::RoundToInt32(Offset.Y), FMath::RoundToInt32(Offset.Z),
                                 FMath::RoundToInt32(Rot.Pitch), FMath::RoundToInt32(Rot.Yaw), FMath::RoundToInt32(Rot.Roll), Carried ? Carried->GetUniqueID() : 0,
                                 int32(bIgnoreDocks), int32(bIgnoreFlying), Ignored, StockAtRestHash(), WorldHash());
    return Q;
}

FSCCargoRouteQuery USCCargoSubsystem::ColumnQuery(const FSCCargoMove& M, const ASCCargoContainer* A, const FQuat& Rot, double MarginCm, bool bIgnoreFlying,
                                                 const TArray<const UPrimitiveComponent*>* Ignore,
                                                 double* OutTop) const
{
    // Over the box as it hangs at Rot: the beams and the drones, spread along the box's long side and turned with it. The
    // column lies along that spread, level (a box flown at a diagonal yaw keeps a column as narrow as the drones are).
    const FVector Size = A->SizeCm();
    const double TopZ = FMath::Abs(Rot.GetAxisX().Z) * Size.X / 2 + FMath::Abs(Rot.GetAxisY().Z) * Size.Y / 2 + FMath::Abs(Rot.GetAxisZ().Z) * Size.Z / 2;
    const FVector Spread = M.Offsets.Num() > 1 ? Rot.RotateVector(M.Offsets.Last()) : FVector::ZeroVector;
    const double Along = Spread.Size2D();
    const FQuat ColumnRot = Along > 1 ? FQuat(FVector::UpVector, FMath::Atan2(Spread.Y, Spread.X)) : FQuat::Identity;
    const double Above = M.HangCm + DroneHeightCm;
    const double Base = DroneHeightCm + MarginCm;
    if (OutTop) *OutTop = TopZ + Above;
    return RouteQuery(FVector(Along + Base, Base, Above / 2 + MarginCm - DroneClearanceCm / 2), FVector(0, 0, TopZ + Above / 2), A, -1e9, 1e9, false, ColumnRot, bIgnoreFlying,
                      Ignore, &M);
}

bool USCCargoSubsystem::PreciseZone(const FSCCargoMove& M, const ASCCargoContainer* A, FTransform& OutToLattice, FBox& OutZone) const
{
    const USCCargoGridComponent* To = M.Plan.Kind == ESCCargoMoveKind::Hold ? nullptr : FindGrid(M.Plan.ToGrid);
    if (!To || !To->bAccessRule || !A) return false;
    const FVector Size = A->SizeCm();
    const double Lead = Size.GetMax() / 2 + PreciseLeadCm; // the load's nose is PreciseLeadCm off the lattice when its centre enters
    OutToLattice = To->LatticeToWorld().Inverse();
    OutZone = FBox(FVector(-Lead, -Lead, 0), To->Lattice.InteriorM * 100.0 + FVector(Lead, Lead, Size.GetMax()));
    return true;
}

bool USCCargoSubsystem::InPreciseZone(const FSCCargoMove& M, const ASCCargoContainer* A, const FVector& P) const
{
    FTransform ToLattice;
    FBox Zone;
    return PreciseZone(M, A, ToLattice, Zone) && Zone.IsInsideOrOn(ToLattice.TransformPosition(P));
}

FSCCargoRouteQuery USCCargoSubsystem::LoadQuery(const FSCCargoMove& M, const ASCCargoContainer* A, bool bIgnoreFlying, const FQuat* Rotation) const
{
    // The box as it hangs (its pickup orientation, kept on the way, unless asked at another) with a margin, and over it
    // the column the beams and the drones fly in. Margins grow by the gusts' envelope in atmosphere. In the move's
    // precise zone (slow, no gusts) the margin is the placing one: a box goes in beside the boxes standing there.
    const FQuat Rot = Rotation ? *Rotation : M.FromRotation;
    double Top = 0;
    auto Shape = [&](double Margin, const TArray<const UPrimitiveComponent*>* Ignore) {
        const FSCCargoRouteQuery Box = RouteQuery(A->SizeCm() / 2 + FVector(Margin), FVector::ZeroVector, A, -1e9, 1e9, false, Rot, bIgnoreFlying, Ignore, &M);
        const FSCCargoRouteQuery Column = ColumnQuery(M, A, Rot, Margin, bIgnoreFlying, Ignore, &Top);
        FSCCargoRouteQuery Q = Box;
        Q.Blocked = [Box, Column](const FVector& P) { return Box.Blocked(P) || Column.Blocked(P); };
        Q.Free = [Box, Column](const FVector& A, const FVector& B) { return Box.Free(A, B) && Column.Free(A, B); };
        Q.BlockedBy = [Box, Column](const FVector& P, double Inflate) { return Box.BlockedBy(P, Inflate) || Column.BlockedBy(P, Inflate); };
        // Which part of the load is stopped: the box, or the drones and beams over it.
        Q.Describe = [Box, Column](const FVector& A, const FVector& B) {
            const FString Hit = Box.Describe(A, B);
            return Hit != TEXT("none") ? TEXT("box:") + Hit : TEXT("drones:") + Column.Describe(A, B);
        };
        Q.Identity = Box.Identity + TEXT("+") + Column.Identity;
        return Q;
    };
    FSCCargoRouteQuery Q = Shape(DroneClearanceCm / 2 + SwayEnvelopeCm(), nullptr);
    FTransform ToLattice;
    FBox Zone;
    if (PreciseZone(M, A, ToLattice, Zone))
    {
        // Precise: the placing margin to everything but the boxes standing at rest, which it may come flush with (boxes
        // stand flush on a lattice: the same 2 cm inside its size the descent onto a slot is swept with).
        TArray<const UPrimitiveComponent*> Stock;
        for (const auto& Pair : State->Boxes)
            if (const ASCCargoContainer* Other = Pair.Value.Actor.Get(); Other && Other != A && !SCCargoIsFlying(Pair.Value.State))
                Other->ForEachComponent<UPrimitiveComponent>(false, [&](const UPrimitiveComponent* C) { Stock.Add(C); });
        const FSCCargoRouteQuery Wide = Q, Structure = Shape(TurnMarginCm, &Stock), Flush = Shape(-FlushInsetCm, nullptr);
        auto In = [ToLattice, Zone](const FVector& P) { return Zone.IsInsideOrOn(ToLattice.TransformPosition(P)); };
        Q.Blocked = [Wide, Structure, Flush, In](const FVector& P) { return In(P) ? Structure.Blocked(P) || Flush.Blocked(P) : Wide.Blocked(P); };
        // A sweep crossing the zone's boundary is split there: each stretch with its own margin (the wide one up to the
        // boundary, the precise one inside), whichever way it is flown.
        auto Stretches = [ToLattice, Zone](const FVector& A, const FVector& B, FVector& OutIn, FVector& OutOut, bool& bBefore, bool& bAfter) {
            double Enter = 0, Leave = 1;
            if (!SCCargoSegmentInside(ToLattice.TransformPosition(A), ToLattice.TransformPosition(B), Zone, Enter, Leave)) return false;
            OutIn = FMath::Lerp(A, B, Enter);
            OutOut = FMath::Lerp(A, B, Leave);
            bBefore = Enter > 1e-6;
            bAfter = Leave < 1 - 1e-6;
            return true;
        };
        Q.Free = [Wide, Structure, Flush, Stretches](const FVector& A, const FVector& B) {
            FVector In, Out;
            bool bBefore = false, bAfter = false;
            if (!Stretches(A, B, In, Out, bBefore, bAfter)) return Wide.Free(A, B);
            return (!bBefore || Wide.Free(A, In)) && Structure.Free(In, Out) && Flush.Free(In, Out) && (!bAfter || Wide.Free(Out, B));
        };
        Q.BlockedBy = [Wide, Structure, Flush, In](const FVector& P, double Inflate) {
            return In(P) ? Structure.BlockedBy(P, Inflate) || Flush.BlockedBy(P, Inflate) : Wide.BlockedBy(P, Inflate);
        };
        Q.Describe = [Wide, Structure, Flush, Stretches](const FVector& A, const FVector& B) {
            FVector In, Out;
            bool bBefore = false, bAfter = false;
            if (!Stretches(A, B, In, Out, bBefore, bAfter)) return Wide.Describe(A, B);
            if (bBefore && !Wide.Free(A, In)) return Wide.Describe(A, In);
            if (!Structure.Free(In, Out)) return Structure.Describe(In, Out);
            if (!Flush.Free(In, Out)) return Flush.Describe(In, Out);
            return bAfter ? Wide.Describe(Out, B) : FString(TEXT("none"));
        };
        Q.Identity = FString::Printf(TEXT("%s|precise:%d:%s:%s"), *Wide.Identity, M.Plan.ToGrid, *Structure.Identity, *Flush.Identity);
    }
    // The search stays inside the environment's bounds (outside its shell is void, free for ever) and under its ceiling.
    const FBox World = EnvironmentBounds();
    if (World.IsValid) Q.MinZ = World.Min.Z;
    Q.MaxZ = CeilingZ() - Top - DroneClearanceCm;
    Q.bRefine = M.bRefine;
    if (M.bRefine) Q.Identity += TEXT("|fine");
    Q.CellCache = M.Cells(Q.Identity);
    return Q;
}

bool USCCargoSubsystem::TurnClear(const FSCCargoMove& M, const ASCCargoContainer* A, const FVector& FromP, const FQuat& FromQ, const FVector& ToP,
                                  const FQuat& ToQ, FString* OutWhy) const
{
    // The box at every step of the turn (and, in a level turn, its drones and beams over it).
    // No turn at all (it arrives as it will stand): the beam draws the swinging box to ToP; what matters is that it fits
    // there, flush with the boxes at rest beside it.
    const double Margin = FromQ.AngularDistance(ToQ) < FMath::DegreesToRadians(1.0) ? -FlushInsetCm : TurnMarginCm;
    const int32 Steps = FMath::Max(2, FMath::CeilToInt(FMath::RadiansToDegrees(FromQ.AngularDistance(ToQ)) / TurnStepDegrees));
    const bool bLevel = FromQ.GetUpVector().Z > 0.999 && ToQ.GetUpVector().Z > 0.999;
    for (int32 K = Margin < 0 ? Steps : 0; K <= Steps; ++K)
    {
        const double T = double(K) / Steps;
        const FQuat Q = FQuat::Slerp(FromQ, ToQ, T);
        const FVector P = FMath::Lerp(FromP, ToP, T);
        const FSCCargoRouteQuery Box = RouteQuery(A->SizeCm() / 2 + FVector(Margin), FVector::ZeroVector, A, -1e9, 1e9, false, Q, false, nullptr, &M);
        // The drones over it too, in a level turn (they fly over the box as it turns; in a turn onto a tilted lattice they
        // swing with the destination's normal, not with the box).
        const bool bBox = Box.Blocked(P);
        if (!bBox && (!bLevel || !ColumnQuery(M, A, Q, Margin, false, nullptr).Blocked(P))) continue;
        if (OutWhy)
            *OutWhy = FString::Printf(TEXT("turning %.0f deg hits %s%s"), FMath::RadiansToDegrees(FromQ.AngularDistance(Q)), bBox ? TEXT("") : TEXT("(the drones) "),
                                      *(bBox ? Box : ColumnQuery(M, A, Q, Margin, false, nullptr)).Describe(P, P + FVector(0, 0, 1)));
        return false;
    }
    return true;
}

bool USCCargoSubsystem::DescentClear(const ASCCargoContainer* A, const FQuat& Rotation, const FVector& From, const FVector& To, FString* OutWhy) const
{
    // What the box overlaps standing in its slot is the grid's own structure (a plate's rails, a floor lip): the planner
    // owns the slot, so only what lies on the way down counts.
    const FVector Half = A->SizeCm() / 2 - FVector(FlushInsetCm);
    TArray<FOverlapResult> InSlot;
    FCollisionQueryParams SlotParams(SCENE_QUERY_STAT(SCCargoSlot), /*bTraceComplex=*/true, A);
    const TSharedRef<FSCCargoHullRule> SlotHulls = HullRule(SlotParams, Half, Rotation);
    GetWorld()->OverlapMultiByObjectType(InSlot, To, Rotation, Obstacles(), FCollisionShape::MakeBox(Half), SlotHulls->At(To));
    TArray<const UPrimitiveComponent*> Own;
    for (const FOverlapResult& O : InSlot)
        if (O.GetComponent()) Own.Add(O.GetComponent());
    const FSCCargoRouteQuery Box = RouteQuery(Half, FVector::ZeroVector, A, -1e9, 1e9, false, Rotation, false, &Own);
    if (Box.Free(From, To)) return true;
    if (OutWhy) *OutWhy = Box.Describe(From, To);
    return false;
}

bool USCCargoSubsystem::SlotApproach(const FSCCargoMove& M, const ASCCargoContainer* A, const FTransform& Slot, const FVector& Normal, const FQuat& FromQ,
                                     double MinCm, FVector& OutTurn, FVector& OutHover, FString* OutWhy) const
{
    const FQuat ToQ = Slot.GetRotation();
    const FSCCargoRouteQuery Arriving = LoadQuery(M, A, false, &FromQ);
    const FSCCargoRouteQuery Turned = RouteQuery(A->SizeCm() / 2 + FVector(TurnMarginCm), FVector::ZeroVector, A, -1e9, 1e9, false, ToQ);
    FString Why = TEXT("no clear hover point");
    for (double Off = MinCm; Off <= MinCm + MaxApproachCm; Off += 25)
    {
        const FVector Hover = Slot.GetLocation() + Normal * Off;
        if (!DescentClear(A, ToQ, Hover, Slot.GetLocation(), &Why)) { Why = TEXT("the way down onto the slot hits ") + Why; continue; }
        // The turn at the hover point, else higher up (straight above it), then down to it turned.
        for (double Rise = 0; Rise <= MaxTurnRiseCm; Rise += 50)
        {
            const FVector Turn = Hover + FVector::UpVector * Rise;
            if (Arriving.Blocked(Turn)) continue;
            if (!TurnClear(M, A, Turn, FromQ, Turn, ToQ, &Why)) continue;
            if (Rise > 0 && !Turned.Free(Turn, Hover)) { Why = TEXT("the turned box's way down to the hover point is blocked"); continue; }
            OutTurn = Turn;
            OutHover = Hover;
            return true;
        }
    }
    if (OutWhy) *OutWhy = Why;
    return false;
}

FSCCargoRouteQuery USCCargoSubsystem::DroneQuery(const ASCCargoDrone* Drone, const FSCCargoMove* Move, TConstArrayView<FVector> Ends, bool bTight) const
{
    // Its hull's bounds (the shape its steps are swept with), and around them half the clearance and the gusts
    // (tight: the gusts alone).
    const FVector Half = (Drone ? Drone->HullHalf : FVector(DroneHeightCm)) + FVector((bTight ? 0.0 : DroneClearanceCm / 2) + SwayEnvelopeCm());
    FSCCargoRouteQuery Q = RouteQuery(Half, Drone ? Drone->HullCentre : FVector::ZeroVector, nullptr, -1e9, CeilingZ() - Half.Z, true, FQuat::Identity, false, nullptr, Move, Drone, Ends);
    // With an end inside its margin (tight): half steps where a full one is blocked, so it gets out of a place
    // narrower than a cell (beside a box on a plate).
    Q.bRefine = bTight;
    if (bTight) Q.Identity += TEXT("|fine");
    return Q;
}

TSharedRef<FSCCargoKeepOut> USCCargoSubsystem::KeptSpace(const FSCCargoMove* Move, const ASCCargoDrone* Drone, int32 CarriedBoxId, bool bIgnoreFlying,
                                                         TConstArrayView<FVector> Ends) const
{
    using EStep = FSCCargoMove::EStep;
    // The same for every query of one frame with these arguments (a plan builds hundreds of queries a tick: a slot's
    // approach tries hover points, each with the shapes of a turn). A route's own ends vary: not kept.
    const TTuple<const FSCCargoMove*, const ASCCargoDrone*, int32, bool> Key(Move, Drone, CarriedBoxId, bIgnoreFlying);
    if (State->KeptFrame != GFrameCounter)
    {
        State->KeptFrame = GFrameCounter;
        State->KeptByQuery.Reset();
    }
    if (Ends.IsEmpty())
        if (const TSharedRef<FSCCargoKeepOut>* Known = State->KeptByQuery.Find(Key)) return *Known;
    TSharedRef<FSCCargoKeepOut> Kept = MakeShared<FSCCargoKeepOut>();
    if (Ends.IsEmpty()) State->KeptByQuery.Add(Key, Kept);
    const bool bLoad = CarriedBoxId != INDEX_NONE;
    // A load's query keeps the other drones' hulls (its own shape carries its margin). A lone drone's keeps the
    // distance drones keep from each other (FSCCargoDroneSteer::Keep, about their centres): a route nearer to a
    // drone that stands would end at that distance from it, for as long as it stands. What the drone's own shape
    // (DroneQuery) reaches is taken off, since the query adds it again.
    const FVector Shape = Drone ? Drone->HullHalf + FVector(DroneClearanceCm / 2 + SwayEnvelopeCm()) : FVector::ZeroVector;
    auto Add = [&](const ASCCargoDrone* D, const FVector& At) {
        if (bLoad)
        {
            Kept->Boxes.Add(FBox::BuildAABB(At + D->HullCentre, D->HullHalf + FVector(ASCCargoDrone::KeptSlackCm)));
            return;
        }
        const double Keep = FSCCargoDroneSteer::Keep(Drone ? Drone->HullRadius() : D->HullRadius(), D->HullRadius()) + ASCCargoDrone::KeptSlackCm;
        const FBox Box = FBox::BuildAABB(At + D->HullCentre, (FVector(Keep) - Shape).ComponentMax(FVector::ZeroVector));
        // Not where this route has to begin or end (its own bay beside another's, its place beside another team's).
        for (const FVector& End : Ends)
            if (Box.ExpandBy(Shape).IsInsideOrOn(End + (Drone ? Drone->HullCentre : FVector::ZeroVector))) return;
        Kept->Boxes.Add(Box);
    };
    TSet<const ASCCargoDrone*> Stationed;
    for (const FSCCargoJob& Job : State->Jobs)
        for (const FSCCargoMove& O : Job.Moves)
        {
            // Its own move: a lone drone on its way out keeps clear of where its teammates will hover.
            const bool bOwn = &O == Move;
            if (bOwn && (bLoad || (O.Step != EStep::Waiting && O.Step != EStep::Outbound))) continue;
            bool bStations = bOwn; // where its drones will be, not where they are
            if (!bOwn) switch (O.Step)
            {
            // On their way out: their stations over the box are taken.
            case EStep::Outbound: bStations = true; break;
            // Hovering over a box at rest, or holding one aside.
            case EStep::Attaching: case EStep::Placing: case EStep::Holding: break;
            // Over a load in the air: with the load, when this query takes that as an obstacle (the right of way).
            case EStep::Lifting: case EStep::Transit: case EStep::Lowering:
                if (!bLoad || bIgnoreFlying || O.Plan.BoxId < CarriedBoxId) continue;
                break;
            default: continue;
            }
            for (int32 I = 0; I < O.Drones.Num(); ++I)
            {
                const ASCCargoDrone* D = FindDrone(*State, O.Drones[I]);
                if (!D || D == Drone) continue;
                Stationed.Add(D);
                if (!bStations) Add(D, D->GetKeptAt());
                else if (O.Stations.IsValidIndex(I)) Add(D, O.Stations[I]);
            }
        }
    // A lone drone's route also keeps clear of any other drone standing in the air (holding short, waiting its turn at
    // a dock): it stands there for now.
    if (!bLoad)
        for (const TWeakObjectPtr<ASCCargoDrone>& W : State->Drones)
            if (const ASCCargoDrone* D = W.Get(); D && D != Drone && D->State != ESCCargoDroneState::Docked && !Stationed.Contains(D) && D->GetVelocity().SizeSquared() < 100.0)
                Add(D, D->GetKeptAt());
    return Kept;
}

bool USCCargoSubsystem::ClearStart(const ASCCargoDrone* D, const FSCCargoRouteQuery& Query, FVector& InOutStart, TArray<FVector>* OutVia) const
{
    // A drone hovering close to something (beside the box it has just set down on a plate) stands inside its own
    // routing margin there. Its route begins a short straight step away: the nearest point, level or upright or
    // between, where its routing shape is free and that its hull sweeps to freely. From a pocket with no such
    // point: back along the way it came (its trail, each stretch swept with the hull as things stand now) to the
    // first place where the shape is free.
    if (OutVia) OutVia->Reset();
    if (!Query.Blocked(InOutStart)) return true;
    const FVector From = InOutStart;
    for (double Off = 25; Off <= MaxApproachCm; Off += 25)
        for (int32 X = -1; X <= 1; ++X)
            for (int32 Y = -1; Y <= 1; ++Y)
                for (int32 Z = -1; Z <= 1; ++Z)
                {
                    if (!X && !Y && !Z) continue;
                    const FVector P = From + FVector(X, Y, Z).GetSafeNormal() * Off;
                    double Fraction = 1;
                    FVector Normal;
                    if (Query.Blocked(P) || DroneSweep(D, From, P, Fraction, Normal)) continue;
                    InOutStart = P;
                    return true;
                }
    TArray<FVector> Via;
    FVector At = From;
    for (int32 I = D->Trail.Num() - 1; I >= 0; --I)
    {
        const FVector P = D->Trail[I];
        double Fraction = 1;
        FVector Normal;
        if (DroneSweep(D, At, P, Fraction, Normal)) break; // that way is shut now
        if (!Query.Blocked(P))
        {
            InOutStart = P;
            if (OutVia) *OutVia = MoveTemp(Via);
            return true;
        }
        Via.Add(P);
        At = P;
    }
    return false;
}

TArray<FVector> USCCargoSubsystem::DockWay(const ASCCargoDrone* D) const
{
    const FSCCargoDock* Dock = D ? State->Docks.FindByPredicate([&](const FSCCargoDock& K) { return K.ItemId == D->DockItem; }) : nullptr;
    const ASCCargoDroneDock* Actor = Dock ? Dock->Actor.Get() : nullptr;
    if (!Actor) return {};
    // The corridor's end as it was last planned for this drone (before any plan: straight off the bay's exit).
    const FVector Exit = Actor->BayExit(D->Bay);
    return {Actor->BayTransform(D->Bay).GetLocation(), Exit, D->bDockClear ? D->DockClear : Exit};
}

double USCCargoSubsystem::FlightFree(const ASCCargoDrone* D, const FSCCargoFlight& F) const
{
    if (!D || F.Points.Num() < 2) return -1;
    const double Radius = D->HullRadius();
    TArray<FSCCargoDroneSteer::FOther, TInlineAllocator<16>> Others;
    if (D->State == ESCCargoDroneState::Docked)
    {
        // In its bay: its way out (bay, exit, clear of the dock's grid) must be free of every drone in the air, where
        // it is and where it flies in the time the way out takes. Two bays nearer than the kept distance (the dock's
        // geometry) so launch one after the other.
        const int32 Legs = FMath::Min(F.Points.Num(), 3);
        const TConstArrayView<FVector> Way(F.Points.GetData(), Legs);
        const double Length = F.S[Legs - 1];
        for (const TWeakObjectPtr<ASCCargoDrone>& W : State->Drones)
            if (const ASCCargoDrone* O = W.Get(); O && O != D && O->State != ESCCargoDroneState::Docked)
            {
                FSCCargoDroneSteer::FOther& Other = Others.AddDefaulted_GetRef();
                Other.P = O->GetActorLocation();
                Other.V = O->GetVelocity();
                Other.Radius = O->HullRadius();
            }
        return FSCCargoDroneSteer::FreeAlong(Way, Radius, Others, FSCCargoFlight::Seconds(Length, F.MaxSpeed, F.MaxAccel)) < Length - 1e-3 ? 0.0 : -1.0;
    }
    if (D->State == ESCCargoDroneState::Launching) return -1; // it is in its own corridor: the others wait for it
    // In the air: short of the dock corridor another drone is using. A drone launching has it; of two docking, the
    // lower id (the other waits the kept distance away until that one is in its bay).
    for (const TWeakObjectPtr<ASCCargoDrone>& W : State->Drones)
    {
        const ASCCargoDrone* O = W.Get();
        if (!O || O == D) continue;
        const bool bUsing = O->State == ESCCargoDroneState::Launching ||
                            (O->State == ESCCargoDroneState::Docking && (D->State != ESCCargoDroneState::Docking || O->DroneId < D->DroneId));
        if (!bUsing) continue;
        const TArray<FVector> Way = DockWay(O);
        for (int32 I = 0; I + 1 < Way.Num(); ++I)
        {
            FSCCargoDroneSteer::FOther& Other = Others.AddDefaulted_GetRef();
            Other.P = Way[I];
            Other.V = Way[I + 1] - Way[I]; // with a lead of 1 s: the corridor's leg
            Other.Radius = O->HullRadius();
        }
    }
    if (Others.IsEmpty()) return -1;
    // As far ahead as it takes to brake, and the kept distance beyond.
    const double Look = F.Speed * F.Speed / FMath::Max(2 * 0.7 * F.MaxAccel, 1.0) + FSCCargoDroneSteer::Keep(Radius, Radius);
    TArray<FVector, TInlineAllocator<64>> Ahead;
    for (double S = F.Dist; S < F.Dist + Look + GuardStretchCm && S <= F.Length() + GuardStretchCm; S += GuardStretchCm) Ahead.Add(F.At(FMath::Min(S, F.Length())));
    const double Free = FSCCargoDroneSteer::FreeAlong(Ahead, Radius, Others, 1.0);
    return Free < Look ? F.Dist + Free : -1.0;
}

double USCCargoSubsystem::HoldShort(const FSCCargoMove& M, const ASCCargoDrone* D, const FSCCargoFlight& F) const
{
    using EStep = FSCCargoMove::EStep;
    if (!D || F.Points.Num() < 2) return -1;
    // The loads planned before this move that have yet to fly: each one's way from where it is (its lift, its route,
    // its way into a hold, each at the rotation it is flown at), its box and the column its drones fly in.
    struct FLeg
    {
        TArray<FVector> Points;
        FQuat Rotation;
        FVector Half;
        double Above;
    };
    TArray<FLeg> Legs;
    const double Pad = D->HullHalf.GetMax() + SwayEnvelopeCm();
    const FVector Station = F.Points.Last() + D->HullCentre;
    for (const FSCCargoJob& Job : State->Jobs)
        for (const FSCCargoMove& O : Job.Moves)
        {
            if (&O == &M || O.PlannedAt <= 0 || O.PlannedAt > M.PlannedAt || (O.PlannedAt == M.PlannedAt && O.MoveId > M.MoveId)) continue;
            const FSCCargoBox* Box = State->Boxes.Find(O.Plan.BoxId);
            const ASCCargoContainer* A = Box ? Box->Actor.Get() : nullptr;
            if (!A) continue;
            const int32 Before = Legs.Num();
            auto Add = [&](TArray<FVector> Points, const FQuat& Rotation, bool bTurn = false) {
                if (Points.IsEmpty()) return;
                // Turned there (a level turn): the cylinder about its upright axis.
                const FVector Half = A->SizeCm() / 2;
                Legs.Add({MoveTemp(Points), bTurn ? FQuat::Identity : Rotation, bTurn ? FVector(Half.Size2D(), Half.Size2D(), Half.Z) : Half, O.HangCm + DroneHeightCm});
            };
            const bool bQuarter = !O.PickRotation.Equals(O.FromRotation, 1e-4); // turned at the top of its lift
            auto Rest = [](const FSCCargoFlight& Flight) {
                TArray<FVector> Points = {Flight.Position()};
                for (int32 I = 0; I < Flight.Points.Num(); ++I)
                    if (Flight.S[I] > Flight.Dist) Points.Add(Flight.Points[I]);
                return Points;
            };
            switch (O.Step)
            {
            case EStep::Outbound: case EStep::Attaching:
                if (O.TransitPath.IsEmpty()) continue;
                Add({A->GetActorLocation(), O.TransitPath[0]}, O.PickRotation);
                if (bQuarter) Add({O.TransitPath[0]}, O.PickRotation, true);
                Add(O.TransitPath, O.FromRotation);
                Add(O.SecondPath, O.SecondRotation);
                break;
            case EStep::Lifting:
                Add(Rest(O.Carry), O.PickRotation);
                if (bQuarter && !O.TransitPath.IsEmpty()) Add({O.TransitPath[0]}, O.PickRotation, true);
                Add(O.TransitPath, O.FromRotation);
                Add(O.SecondPath, O.SecondRotation);
                break;
            case EStep::Transit:
                if (O.bStalled) continue; // it has no route from where it is; it plans round what is there then
                Add(Rest(O.Carry), O.FromRotation);
                Add(O.SecondPath, O.SecondRotation);
                break;
            case EStep::Lowering: Add(Rest(O.Carry), O.ToRotation); break;
            default: continue;
            }
            // Of this load's way: up to where it is past the station (what it flies after that does not keep the drone
            // waiting; the two may well fly the same corridor in opposite directions). None of it when it never is there.
            int32 LastLeg = INDEX_NONE, LastAt = 0;
            for (int32 K = Before; K < Legs.Num(); ++K)
                if (int32 At = 0; FSCCargoKeepOut::RoutePasses(Legs[K].Points, Legs[K].Rotation, Legs[K].Half, Legs[K].Above, Station, Pad, &At))
                {
                    LastLeg = K;
                    LastAt = At;
                }
            if (LastLeg == INDEX_NONE) Legs.SetNum(Before);
            else
            {
                Legs.SetNum(LastLeg + 1);
                Legs[LastLeg].Points.SetNum(FMath::Min(LastAt + 2, Legs[LastLeg].Points.Num()));
            }
        }
    // And the drones of other teams within the kept distance of the station: where they hover now (attached to a box
    // beside this one, setting one down), or will before this drone, when their move was planned first.
    const double Keep = FSCCargoDroneSteer::Keep(D->HullRadius(), D->HullRadius()) + ASCCargoDrone::KeptSlackCm;
    TArray<FVector, TInlineAllocator<8>> Taken;
    for (const FSCCargoJob& Job : State->Jobs)
        for (const FSCCargoMove& O : Job.Moves)
        {
            if (&O == &M || O.Step == EStep::Waiting || O.Step == EStep::Returning || O.Step == EStep::Done || O.Step == EStep::Failed) continue;
            const bool bFirst = O.PlannedAt > 0 && (O.PlannedAt < M.PlannedAt || (O.PlannedAt == M.PlannedAt && O.MoveId < M.MoveId));
            for (int32 I = 0; I < O.Drones.Num(); ++I)
            {
                const ASCCargoDrone* Other = FindDrone(*State, O.Drones[I]);
                if (!Other) continue;
                if (O.Step == EStep::Outbound && !bFirst) continue;
                const FVector At = (O.Step == EStep::Outbound ? (O.Stations.IsValidIndex(I) ? O.Stations[I] : Other->GetActorLocation()) : Other->GetActorLocation()) + Other->HullCentre;
                if (FVector::DistSquared(At, Station) < Keep * Keep) Taken.Add(At);
            }
        }
    if (Legs.IsEmpty() && Taken.IsEmpty()) return -1; // the station is free and no load has yet to fly through it: on
    auto Passed = [&](const FVector& P, double Extra) {
        for (const FVector& At : Taken)
            if (FVector::DistSquared(At, P) < Keep * Keep) return true;
        for (const FLeg& Leg : Legs)
            if (FSCCargoKeepOut::RoutePasses(Leg.Points, Leg.Rotation, Leg.Half, Leg.Above, P, Pad + Extra)) return true;
        return false;
    };
    // Back along its way to the last point clear of them (by the kept distance more): there it waits, and where it
    // is when it is past that point but not yet in a load's way. Already in one (it came before the load was
    // planned, or the load took a new route): on to its station; the load's own guard finds it there.
    int32 I = F.Points.Num() - 1;
    while (I >= 0 && Passed(F.Points[I] + D->HullCentre, Keep)) --I;
    if (I < 0) return 0;
    return F.Dist <= F.S[I] || !Passed(F.Position() + D->HullCentre, 0) ? F.S[I] : -1.0;
}

bool USCCargoSubsystem::DroneOwnBox(const ASCCargoDrone* Drone, FTransform& OutPose, FVector& OutHalf) const
{
    const FSCCargoBox* Box = Drone && Drone->CarryingBoxId != INDEX_NONE ? State->Boxes.Find(Drone->CarryingBoxId) : nullptr;
    const ASCCargoContainer* A = Box ? Box->Actor.Get() : nullptr;
    if (!A || !SCCargoIsFlying(Box->State)) return false;
    OutPose = FTransform(A->GetActorQuat(), A->GetActorLocation());
    OutHalf = A->SizeCm() / 2;
    return true;
}

void USCCargoSubsystem::DroneLoads(const ASCCargoDrone* Drone, TArray<FSCCargoDroneSteer::FLoad, TInlineAllocator<8>>& Out) const
{
    using EStep = FSCCargoMove::EStep;
    for (const auto& Pair : State->Boxes)
    {
        const FSCCargoBox& Box = Pair.Value;
        const ASCCargoContainer* A = Box.Actor.Get();
        if (!A || (!SCCargoIsFlying(Box.State) && Box.State != ESCCargoBoxState::Held) || (Drone && Drone->CarryingBoxId == Box.Id)) continue;
        const FSCCargoMove* M = MoveOf(A);
        // Its own team's box between the steps that carry it (attached, releasing): its own.
        if (M && Drone && M->Drones.Contains(Drone->DroneId)) continue;
        FSCCargoDroneSteer::FLoad& Load = Out.AddDefaulted_GetRef();
        Load.Pose = FTransform(A->GetActorQuat(), A->GetActorLocation());
        Load.Half = A->SizeCm() / 2;
        // Being turned (at the top of its lift, at a hold's entrance, as it is caught over its slot): it sweeps the
        // cylinder about its upright axis, or, tilting, the sphere about its centre.
        if (M && M->bTurning)
        {
            const bool bLevel = FMath::Abs(A->GetActorQuat().GetUpVector().Z) > 0.999 && !M->bTilting;
            const double Round = bLevel ? Load.Half.Size2D() : Load.Half.Size();
            Load.Half = FVector(Round, Round, bLevel ? Load.Half.Z : Round);
            Load.Pose.SetRotation(FQuat::Identity);
        }
        // As its route flies it (it is moved by its route on the lift and the descent, and follows it in between).
        if (M && (M->Step == EStep::Lifting || M->Step == EStep::Transit || M->Step == EStep::Lowering) && !M->bStalled) Load.V = M->Carry.Tangent() * M->Carry.Speed;
    }
}


bool USCCargoSubsystem::EarlyTurnPoint(FSCCargoMove& M, const ASCCargoContainer* A, const USCCargoGridComponent* From, const FVector& Lifted, const FQuat& ToQ,
                                       FVector& OutPoint) const
{
    // A level turn only (a box hanging level, onto a level lattice): a box coming off a plate on its side turns when caught.
    constexpr double Level = 0.999;
    if (M.PickRotation.GetUpVector().Z < Level || ToQ.GetUpVector().Z < Level) return false;
    // Answered once per situation (the box, how it hangs, the stock at rest, where it was lifted).
    const FSCCargoRouteQuery Load = LoadQuery(M, A, false, &M.PickRotation);
    const FString Key = FString::Printf(TEXT("%s>%s@%s"), *Load.Identity, *ToQ.Rotator().ToCompactString(), *Lifted.ToCompactString());
    if (const TOptional<FVector>* Known = M.TurnPoints.Find(Key))
    {
        if (Known->IsSet()) OutPoint = Known->GetValue();
        return Known->IsSet();
    }
    TOptional<FVector>& Answer = M.TurnPoints.Add(Key);
    // At the top of the lift, else the nearest point over the grid (its lattice's steps, at the lifted height) the load
    // reaches in a straight sweep as it was picked.
    const FTransform Frame = From->LatticeToWorld();
    const FVector Cell = From->Lattice.CellM * 100.0, Interior = From->Lattice.InteriorM * 100.0;
    const FVector Local = Frame.InverseTransformPosition(Lifted);
    TArray<FVector> Candidates;
    for (int32 I = FMath::CeilToInt32(-Local.X / Cell.X); Local.X + I * Cell.X <= Interior.X; ++I)
        for (int32 J = FMath::CeilToInt32(-Local.Y / Cell.Y); Local.Y + J * Cell.Y <= Interior.Y; ++J)
            Candidates.Add(Frame.TransformPosition(Local + FVector(I * Cell.X, J * Cell.Y, 0)));
    if (Candidates.IsEmpty()) Candidates.Add(Lifted); // lifted outside its lattice's footprint
    Candidates.Sort([&](const FVector& P, const FVector& Q) { return FVector::DistSquared(P, Lifted) < FVector::DistSquared(Q, Lifted); });
    int32 Tried = 0;
    for (const FVector& P : Candidates)
    {
        ++Tried;
        if (Load.Blocked(P) || (!P.Equals(Lifted, 1.0) && !Load.Free(Lifted, P)) || !TurnClear(M, A, P, M.PickRotation, P, ToQ)) continue;
        Answer = OutPoint = P;
        break;
    }
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_TURN_POINT move=%d box=%d found=%d at=%s from_lift_m=%.1f tried=%d of %d"), M.MoveId, M.Plan.BoxId, Answer.IsSet(),
           *Answer.Get(Lifted).ToCompactString(), FVector::Dist(Answer.Get(Lifted), Lifted) / 100, Tried, Candidates.Num());
    return Answer.IsSet();
}
