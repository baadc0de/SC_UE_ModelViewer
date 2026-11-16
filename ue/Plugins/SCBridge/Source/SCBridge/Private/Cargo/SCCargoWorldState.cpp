// USCCargoSubsystem: the world a cargo route is planned in (split from SCCargoQueries.cpp). Which vehicles' own (hull)
// colliders a query leaves out where (FSCCargoHullRule, WP-P3), the hashes of that world and of the stock at rest that
// a query's identity carries, and dropping what a move keeps between ticks when either changes.
#include "Cargo/SCCargoSubsystem.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoContainer.h"
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoGridComponent.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "SCViewerSubsystem.h"

uint32 FSCCargoHullRule::Touched(const FVector& Centre) const
{
    // The shape is in a vehicle's grid when an upright axis through it (its middle, the ends of its long side) touches
    // the grid's volume: the rule the player's capsule follows (ASCVehicle::IsInPhysicsGrid(Bottom, Top)).
    uint32 Mask = 0;
    for (int32 I = 0; I < Hulls.Num(); ++I)
    {
        const ASCVehicle* V = Hulls[I].Vehicle.Get();
        if (!V) continue;
        for (const FVector& Probe : Probes)
        {
            const FVector P = Centre + Probe;
            const TTuple<uint32, FIntVector, int32> Key(V->GetUniqueID(), FIntVector(FMath::RoundToInt32(P.X / 25), FMath::RoundToInt32(P.Y / 25), FMath::RoundToInt32(P.Z / 25)),
                                                        FMath::RoundToInt32(HalfZ));
            bool bIn;
            if (const bool* Known = Memo ? Memo->Find(Key) : nullptr) bIn = *Known;
            else
            {
                bIn = V->IsInPhysicsGrid(P - FVector(0, 0, HalfZ), P + FVector(0, 0, HalfZ));
                if (Memo) Memo->Add(Key, bIn);
            }
            if (bIn)
            {
                Mask |= 1u << I;
                break;
            }
        }
    }
    return Mask;
}

const FCollisionQueryParams& FSCCargoHullRule::For(uint32 Mask) const
{
    if (!Mask) return All;
    if (FMath::IsPowerOfTwo(Mask)) return Hulls[FMath::FloorLog2(Mask)].Without;
    return None; // in more than one vehicle's grid (grids overlapping): none of the hulls
}

const FSCCargoMove* USCCargoSubsystem::MoveOf(const AActor* Box) const
{
    if (!Box) return nullptr;
    for (const auto& Pair : State->Boxes)
        if (Pair.Value.Actor.Get() == Box)
            for (const FSCCargoJob& Job : State->Jobs)
                for (const FSCCargoMove& M : Job.Moves)
                    if (M.Plan.BoxId == Pair.Key && M.Step != FSCCargoMove::EStep::Done && M.Step != FSCCargoMove::EStep::Failed) return &M;
    return nullptr;
}

TSharedRef<FSCCargoHullRule> USCCargoSubsystem::HullRule(const FCollisionQueryParams& Base, const FVector& HalfExtent, const FQuat& Rotation) const
{
    // A vehicle's own colliders (the root entity's game collision: its hull as the game collides it with the world
    // outside) act only on what is outside the vehicle's physics grid (ASCVehicle::HullColliders, WP-P3). A cargo query
    // follows that per position: a vehicle's hull is left out where the shape is in that vehicle's grid and blocks
    // everywhere else, for every vehicle alike. The hull's caps over its openings (triangles without a surface, class
    // `no_surface`) are no colliders (WP-P4), so the way through an open ramp is free although the grid's volume ends
    // short of it; what fills an opening (the ramp, a door) blocks with its own colliders in its pose.
    TSharedRef<FSCCargoHullRule> Rule = MakeShared<FSCCargoHullRule>();
    Rule->All = Rule->None = Base;
    WorldHash(); // a changed world forgets the grid answers kept in State->GridMemo
    Rule->Memo = State->GridMemo;
    Rule->HalfZ = FMath::Abs(Rotation.GetAxisX().Z) * HalfExtent.X + FMath::Abs(Rotation.GetAxisY().Z) * HalfExtent.Y + FMath::Abs(Rotation.GetAxisZ().Z) * HalfExtent.Z;
    Rule->Probes.Add(FVector::ZeroVector);
    const int32 Long = HalfExtent.X >= HalfExtent.Y ? (HalfExtent.X >= HalfExtent.Z ? 0 : 2) : (HalfExtent.Y >= HalfExtent.Z ? 1 : 2);
    FVector End = FVector::ZeroVector;
    End[Long] = HalfExtent[Long];
    if (HalfExtent[Long] > 2 * FMath::Min3(HalfExtent.X, HalfExtent.Y, HalfExtent.Z))
    {
        Rule->Probes.Add(Rotation.RotateVector(End));
        Rule->Probes.Add(Rotation.RotateVector(-End));
    }
    if (const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld()))
        for (const ASCVehicle* V : Viewer->GetVehicles())
        {
            if (!V || V->HullColliders.IsEmpty()) continue;
            if (Rule->Hulls.Num() >= 32) continue;
            FSCCargoHullRule::FHull& Hull = Rule->Hulls.AddDefaulted_GetRef();
            Hull.Vehicle = V;
            Hull.Without = Base;
            for (const UPrimitiveComponent* C : V->HullColliders)
            {
                Rule->None.AddIgnoredComponent(C);
                Hull.Without.AddIgnoredComponent(C);
            }
        }
    return Rule;
}

uint32 USCCargoSubsystem::WorldHash() const
{
    // The collision world a route is planned in, apart from the cargo: which vehicles there are, where they and the
    // environment stand, and the pose of every rig (a ramp, a door, a plate: whoever moved it). Per frame.
    if (State->WorldHashFrame == GFrameCounter) return State->WorldHashValue;
    const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld());
    uint32 Hash = Viewer ? GetTypeHash(Viewer->GetVehiclesRevision()) : 0;
    auto Mix = [&Hash](double Value) { Hash = FCrc::MemCrc32(&Value, sizeof(Value), Hash); };
    auto Transform = [&Mix](const FTransform& T) {
        const FQuat R = T.GetRotation();
        const FVector P = T.GetTranslation(), S = T.GetScale3D();
        for (const double Value : {R.X, R.Y, R.Z, R.W, P.X, P.Y, P.Z, S.X, S.Y, S.Z}) Mix(Value);
    };
    auto Actor = [&](const ASCVehicle* V) {
        if (!V) return;
        Hash = HashCombineFast(Hash, GetTypeHash(V->GetUniqueID()));
        Transform(V->GetActorTransform());
        for (const USCRigComponent* Rig : V->Rigs)
        {
            if (!Rig) continue;
            Hash = HashCombineFast(Hash, GetTypeHash(Rig->GetUniqueID()) + Rig->GetCollisionRefits());
            for (const FTransform& Bone : Rig->LocalPose) Transform(Bone);
        }
    };
    if (Viewer)
    {
        for (const ASCVehicle* V : Viewer->GetVehicles()) Actor(V);
        Actor(Viewer->GetEnvironment());
    }
    if (Hash != State->WorldHashValue || !State->GridMemo) State->GridMemo = MakeShared<FSCCargoHullRule::FMemo>();
    State->WorldHashFrame = GFrameCounter;
    State->WorldHashValue = Hash;
    return Hash;
}

void USCCargoSubsystem::RefreshMove(FSCCargoMove& M) const
{
    // What a move keeps between ticks (searches under way or answered, sampled cells, turn points) was worked out in
    // the world as it stood: a ramp that moved, a ship that came or went, a box that was set down make it worthless.
    const uint32 Stamp = HashCombineFast(WorldHash(), StockAtRestHash());
    if (M.WorldStamp == Stamp) return;
    if (M.WorldStamp != 0 && !M.Searches.IsEmpty())
        UE_LOG(LogSCCargo, Display, TEXT("CARGO_WORLD_CHANGED move=%d box=%d searches_dropped=%d cell_caches_dropped=%d"), M.MoveId, M.Plan.BoxId, M.Searches.Num(),
               M.CellCaches.Num());
    M.Searches.Empty();
    M.CellCaches.Empty();
    M.TurnPoints.Empty();
    M.WorldStamp = Stamp;
}

uint32 USCCargoSubsystem::StockAtRestHash() const
{
    if (State->StockHashFrame == GFrameCounter) return State->StockHash;
    uint32 Hash = 0;
    for (const auto& Pair : State->Boxes)
    {
        const ASCCargoContainer* A = Pair.Value.Actor.Get();
        // At rest: on a grid or set down. A box in the air is not (its right of way is another matter), nor one held aside
        // (it keeps station with a small drift: still an obstacle, never a still one).
        if (!A || SCCargoIsFlying(Pair.Value.State) || Pair.Value.State == ESCCargoBoxState::Held) continue;
        const FVector P = A->GetActorLocation();
        const FRotator R = A->GetActorRotation();
        // Order-independent (the map's order is not): a sum of per-box hashes.
        // To 5 cm and 1 degree: a box coming to rest where it was let down does not count as moving for long.
        Hash += HashCombineFast(HashCombineFast(GetTypeHash(Pair.Key), GetTypeHash(FIntVector(FMath::RoundToInt(P.X / 5), FMath::RoundToInt(P.Y / 5), FMath::RoundToInt(P.Z / 5)))),
                                GetTypeHash(FIntVector(FMath::RoundToInt(R.Pitch), FMath::RoundToInt(R.Yaw), FMath::RoundToInt(R.Roll))));
    }
    State->StockHashFrame = GFrameCounter;
    State->StockHash = Hash;
    return Hash;
}

