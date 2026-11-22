// FSCCargoPlanner::PlanDigOut: reaching a covered box. Its cover (what rests on it, transitively) comes off top first:
// the lowest covering boxes are held aside by drones and put back after the box beneath is out (as many as the drones
// left over from the box's own team can hold at once); the rest are relocated for good to free cells of the source grids
// that are not over the box. A covering box that cannot move (a fixed module, another job's) stops the dig.
#include "Cargo/SCCargoPlanner.h"

bool FSCCargoPlanner::ReturnSlot(const FSCCargoGridModel& Grid, const FSCCargoItem& Item, const FSCCargoPlacement& Planned, FSCCargoPlacement& Out)
{
    FSCCargoGridModel Now = Grid;
    Now.Placed.RemoveAll([&](const FSCCargoPlacement& P) { return P.ItemId == Item.Id; });
    if (Now.IsFree(Planned.Origin, Planned.Cells) && Now.IsSupported(Planned.Origin, Planned.Cells))
    {
        Out = Planned;
        Out.ItemId = Item.Id;
        return true;
    }
    if (!FindSlot(Now, Item, Out)) return false;
    Out.ItemId = Item.Id;
    return true;
}

bool FSCCargoPlanner::PlanDigOut(const FSCCargoPlanInput& In, TArray<FSCCargoGridModel>& Sources, int32 GridIndex, int32 Target, int32 TargetTeam,
                                 TFunctionRef<int32(const FSCCargoItem&)> TeamFor, TArray<FSCCargoMovePlan>& OutMoves, FString& Why)
{
    FSCCargoGridModel& Grid = Sources[GridIndex];
    const FSCCargoPlacement* TargetSlot = Grid.Find(Target);
    const FSCCargoItem* TargetItem = In.Items.Find(Target);
    if (!TargetSlot || !TargetItem) { Why = TEXT("the box is not on its grid"); return false; }
    const TArray<int32> Cover = Grid.CoverOf(Target); // top first
    TArray<FSCCargoMovePlan> Moves;
    if (!Cover.IsEmpty())
    {
        for (const int32 Id : Cover)
        {
            const FSCCargoItem* Item = In.Items.Find(Id);
            if (!Item || !Item->bMovable || In.Busy.Contains(Id))
            {
                Why = FString::Printf(TEXT("%d SCU box on %s is covered by %s"), TargetItem->SCU, *Grid.Label,
                                      !Item || !Item->bMovable ? TEXT("a fixed module") : TEXT("a box another transfer is moving"));
                return false;
            }
            if (TeamFor(*Item) == 0 || TeamFor(*Item) > In.DronesAvailable)
            {
                Why = FString::Printf(TEXT("%d SCU box on %s is covered by a box the drones cannot lift"), TargetItem->SCU, *Grid.Label);
                return false;
            }
        }
        // Hold from the bottom of the cover up while drones remain beside the box's own team; relocate the rest.
        TSet<int32> Held;
        int32 Free = In.DronesAvailable - TargetTeam;
        for (int32 I = Cover.Num() - 1; I >= 0; --I)
        {
            const int32 Team = TeamFor(In.Items[Cover[I]]);
            if (Team > Free) break;
            Free -= Team;
            Held.Add(Cover[I]);
        }
        // Cells over the box (any height) stay clear: that is its lift corridor along the grid normal.
        const FSCCargoPlacement Column = *TargetSlot;
        auto OverTarget = [Column](const FSCCargoPlacement& P) {
            for (int32 A = 0; A < 2; ++A)
                if (P.Origin[A] >= Column.Origin[A] + Column.Cells[A] || Column.Origin[A] >= P.Origin[A] + P.Cells[A]) return false;
            return true;
        };
        TSet<int32> NoSupport(Cover);
        NoSupport.Add(Target);
        for (const int32 Id : Cover) // top first: what is relocated comes off before what is held under it
        {
            const FSCCargoPlacement Old = *Grid.Find(Id);
            FSCCargoMovePlan M;
            M.BoxId = Id;
            M.SCU = In.Items[Id].SCU;
            M.FromGrid = Grid.GridId;
            M.DronesNeeded = TeamFor(In.Items[Id]);
            M.ForBoxId = Target;
            if (Held.Contains(Id))
            {
                M.Kind = ESCCargoMoveKind::Hold;
                M.ToGrid = Grid.GridId;
                M.Origin = Old.Origin;
                M.Cells = Old.Cells;
                M.bRotated = Old.bRotated;
                Moves.Add(M);
                continue;
            }
            Grid.Placed.RemoveAll([&](const FSCCargoPlacement& P) { return P.ItemId == Id; });
            bool bPlaced = false;
            for (int32 G = 0; G < Sources.Num() && !bPlaced; ++G)
            {
                const int32 Index = G == 0 ? GridIndex : (G <= GridIndex ? G - 1 : G); // its own grid first
                FSCCargoPlacement Slot;
                const bool bSame = Index == GridIndex;
                if (FindSlot(Sources[Index], In.Items[Id], Slot, &NoSupport, [&](const FSCCargoPlacement& P) { return bSame && OverTarget(P); }))
                {
                    Sources[Index].Placed.Add(Slot);
                    M.Kind = ESCCargoMoveKind::Relocate;
                    M.ToGrid = Sources[Index].GridId;
                    M.Origin = Slot.Origin;
                    M.Cells = Slot.Cells;
                    M.bRotated = Slot.bRotated;
                    bPlaced = true;
                }
            }
            if (!bPlaced)
            {
                Why = FString::Printf(TEXT("%d SCU box on %s is covered by %d box%s: not enough drones to hold them and no free cell to move them to"),
                                      TargetItem->SCU, *Grid.Label, Cover.Num(), Cover.Num() == 1 ? TEXT("") : TEXT("es"));
                return false;
            }
            Moves.Add(M);
        }
    }
    Grid.Placed.RemoveAll([&](const FSCCargoPlacement& P) { return P.ItemId == Target; });
    // Put back, lowest first: into its own cell when that still stands on something once the box beneath is gone,
    // else into the best free cell (usually where the dug-out box was).
    TArray<FSCCargoMovePlan*> Holds;
    for (FSCCargoMovePlan& M : Moves)
        if (M.Kind == ESCCargoMoveKind::Hold) Holds.Add(&M);
    for (const FSCCargoMovePlan* H : Holds) Grid.Placed.RemoveAll([&](const FSCCargoPlacement& P) { return P.ItemId == H->BoxId; });
    for (int32 I = Holds.Num() - 1; I >= 0; --I)
    {
        FSCCargoMovePlan& H = *Holds[I];
        FSCCargoPlacement Slot;
        Slot.ItemId = H.BoxId;
        Slot.Origin = H.Origin;
        Slot.Cells = H.Cells;
        Slot.bRotated = H.bRotated;
        if (!Grid.IsFree(Slot.Origin, Slot.Cells) || !Grid.IsSupported(Slot.Origin, Slot.Cells))
        {
            if (!FindSlot(Grid, In.Items[H.BoxId], Slot))
            {
                Why = FString::Printf(TEXT("%d SCU box on %s: no cell to put its cover back"), TargetItem->SCU, *Grid.Label);
                return false;
            }
            H.Origin = Slot.Origin;
            H.Cells = Slot.Cells;
            H.bRotated = Slot.bRotated;
        }
        Grid.Placed.Add(Slot);
    }
    OutMoves.Append(Moves);
    return true;
}
