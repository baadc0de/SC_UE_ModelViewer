// USCCargoSubsystem's collision queries for flying shapes (split from SCCargoPrepare.cpp): a lone drone; a hanging load =
// the box with its margin, and over it the column the beams and the drones fly in (turned with the box); the collision
// they test (every object type the ship and hangar block with, complex collision, the game colliders included); each
// query's identity (what it tests, where the boxes at rest are) for the caches and searches that depend on it; the
// swept turn and descent onto a slot.
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
                                                const FSCCargoMove* Move, const ASCCargoDrone* Drone) const
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
    const TSharedRef<FSCCargoHullRule> Hulls = HullRule(Params, HalfExtent, Rotation, Move ? Move : MoveOf(Carried), Drone);
    FSCCargoRouteQuery Q;
    const FQuat R = Rotation;
    Q.Blocked = [World, Hulls, Types, Shape, Offset, R](const FVector& P) {
        return World->OverlapAnyTestByObjectType(P + Offset, R, Types, Shape, Hulls->For(Hulls->Touched(P + Offset)));
    };
    Q.Free = [World, Hulls, Types, Shape, Offset, R](const FVector& A, const FVector& B) {
        return !World->SweepTestByObjectType(A + Offset, B + Offset, R, Types, Shape, Hulls->For(Hulls->Touched(A + Offset) | Hulls->Touched(B + Offset)));
    };
    Q.Describe = [World, Hulls, Types, Shape, Offset, R](const FVector& A, const FVector& B) {
        FHitResult Hit;
        if (!World->SweepSingleByObjectType(Hit, A + Offset, B + Offset, R, Types, Shape, Hulls->For(Hulls->Touched(A + Offset) | Hulls->Touched(B + Offset))))
            return FString(TEXT("none"));
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
    Q.Identity = FString::Printf(TEXT("%d,%d,%d@%d,%d,%d/%d,%d,%d/%u/%d%d/%08x/%08x/%08x/%08x"), FMath::RoundToInt32(HalfExtent.X), FMath::RoundToInt32(HalfExtent.Y),
                                 FMath::RoundToInt32(HalfExtent.Z), FMath::RoundToInt32(Offset.X), FMath::RoundToInt32(Offset.Y), FMath::RoundToInt32(Offset.Z),
                                 FMath::RoundToInt32(Rot.Pitch), FMath::RoundToInt32(Rot.Yaw), FMath::RoundToInt32(Rot.Roll), Carried ? Carried->GetUniqueID() : 0,
                                 int32(bIgnoreDocks), int32(bIgnoreFlying), Ignored, Hulls->Partners, StockAtRestHash(), WorldHash());
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
                      Ignore);
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
        const FSCCargoRouteQuery Box = RouteQuery(A->SizeCm() / 2 + FVector(Margin), FVector::ZeroVector, A, -1e9, 1e9, false, Rot, bIgnoreFlying, Ignore);
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
        const FSCCargoRouteQuery Box = RouteQuery(A->SizeCm() / 2 + FVector(Margin), FVector::ZeroVector, A, -1e9, 1e9, false, Q);
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
    const TSharedRef<FSCCargoHullRule> SlotHulls = HullRule(SlotParams, Half, Rotation, MoveOf(A));
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

FSCCargoRouteQuery USCCargoSubsystem::DroneQuery(const ASCCargoDrone* Drone) const
{
    const double R = (Drone ? Drone->GetComponentsBoundingBox(false).GetExtent().GetMax() : DroneHeightCm) + DroneClearanceCm + SwayEnvelopeCm();
    return RouteQuery(FVector(R), FVector::ZeroVector, nullptr, -1e9, CeilingZ() - R, true, FQuat::Identity, false, nullptr, nullptr, Drone);
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
