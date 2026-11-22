// USCCargoGridComponent: lattice <-> world mapping and debug drawing of one cargo grid.
#include "Cargo/SCCargoGridComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"

// Verified on the Hull A cargo plates (extended: the lattice exactly covers the plate face) and the hangar freight elevator
// platform (WP-K1 verification): the container entity sits at the centre of the lattice floor; grid_pos_offset adds to it.
ESCCargoGridOrigin USCCargoGridComponent::OriginRule = ESCCargoGridOrigin::BottomCentre;

USCCargoGridComponent::USCCargoGridComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    SetMobility(EComponentMobility::Movable);
}

FTransform USCCargoGridComponent::LatticeToWorld() const
{
    const FVector Size = Lattice.InteriorM * 100.0;
    FVector Corner = FSCCargoLattice::FromSC(Record.Container.GridPosOffset) * 100.0;
    switch (OriginRule)
    {
    case ESCCargoGridOrigin::BottomCentre: Corner += FVector(-Size.X / 2, -Size.Y / 2, 0); break;
    case ESCCargoGridOrigin::Centre: Corner -= Size / 2; break;
    default: break;
    }
    return FTransform(Corner) * GetComponentTransform();
}

FTransform USCCargoGridComponent::SlotToWorld(const FSCCargoPlacement& Slot, const FVector& SizeM) const
{
    const FVector Cell = Lattice.CellM * 100.0;
    const FVector Footprint = FVector(Slot.Cells) * Cell;
    const FVector Local(Slot.Origin.X * Cell.X + Footprint.X / 2, Slot.Origin.Y * Cell.Y + Footprint.Y / 2, Slot.Origin.Z * Cell.Z + SizeM.Z * 50.0);
    const FQuat Yaw = Slot.bRotated ? FQuat(FVector::UpVector, UE_HALF_PI) : FQuat::Identity;
    return FTransform(Yaw, Local) * LatticeToWorld();
}

FBox USCCargoGridComponent::WorldBounds() const
{
    return FBox(FVector::ZeroVector, Lattice.InteriorM * 100.0).TransformBy(LatticeToWorld());
}

FSCCargoGridModel USCCargoGridComponent::Model() const
{
    FSCCargoGridModel M;
    M.GridId = GridId;
    M.Lattice = Lattice;
    M.Placed = Placed;
    M.Label = GridLabel.ToString();
    M.GroupLabel = GroupLabel.ToString();
    return M;
}

void USCCargoGridComponent::DebugDraw(float Seconds, bool bCells) const
{
    UWorld* World = GetWorld();
    if (!World) return;
    const FTransform L = LatticeToWorld();
    const FVector Size = Lattice.InteriorM * 100.0;
    const FColor Frame = Access == ESCCargoGridAccess::Open ? FColor(80, 220, 120) : FColor(230, 160, 40);
    DrawDebugBox(World, L.TransformPosition(Size / 2), Size / 2, L.GetRotation(), Frame, false, Seconds, 0, 2.f);
    // Origin marker (the entity's frame) and the lattice min corner: the origin convention is checked against these.
    const FTransform Entity = GetComponentTransform();
    DrawDebugCoordinateSystem(World, Entity.GetLocation(), Entity.Rotator(), 60.f, false, Seconds, 0, 3.f);
    DrawDebugPoint(World, L.GetLocation(), 14.f, FColor::Magenta, false, Seconds);
    if (bCells)
    {
        const FVector Cell = Lattice.CellM * 100.0;
        for (int32 X = 0; X <= Lattice.Cells.X; ++X)
            DrawDebugLine(World, L.TransformPosition(FVector(X * Cell.X, 0, 1)), L.TransformPosition(FVector(X * Cell.X, Size.Y, 1)), FColor(60, 160, 90), false, Seconds, 0, 0.5f);
        for (int32 Y = 0; Y <= Lattice.Cells.Y; ++Y)
            DrawDebugLine(World, L.TransformPosition(FVector(0, Y * Cell.Y, 1)), L.TransformPosition(FVector(Size.X, Y * Cell.Y, 1)), FColor(60, 160, 90), false, Seconds, 0, 0.5f);
    }
    for (const FSCCargoPlacement& P : Placed)
    {
        const FVector Cell = Lattice.CellM * 100.0;
        const FVector Min = FVector(P.Origin) * Cell, Max = FVector(P.Origin + P.Cells) * Cell;
        const FColor Color = Reserved.Contains(P.ItemId) ? FColor(90, 160, 255) : FColor(240, 240, 240);
        DrawDebugBox(World, L.TransformPosition((Min + Max) / 2), (Max - Min) / 2 - FVector(2), L.GetRotation(), Color, false, Seconds, 0, 1.f);
    }
}
