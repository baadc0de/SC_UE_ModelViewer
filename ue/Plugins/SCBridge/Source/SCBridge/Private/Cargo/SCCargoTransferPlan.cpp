// FSCCargoPlanner::Plan: which boxes of a commodity can move from the source grids to the destination grids (whole
// boxes only, no repacking), the refusals with their reasons, and the achievable amounts (subset sums that pack and
// whose covered boxes can be dug out: PlanDigOut). Boxes with the least cover go first.
#include "Cargo/SCCargoPlanner.h"

namespace
{
constexpr int32 MaxPackEvaluations = 4000; // bound on subset trials per plan (each trial packs the destination)
constexpr int32 MaxTrialsPerAmount = 48;

struct FCandidate
{
    int32 Id = INDEX_NONE;
    int32 Grid = INDEX_NONE; // index into Input.Grids
    int32 Z = 0;
    int32 SCU = 0;
    int32 Cover = 0; // boxes resting on it, transitively
};
FText Reason(const FString& S) { return FText::FromString(S); }
} // namespace

FSCCargoTransferPreview FSCCargoPlanner::Plan(const FSCCargoPlanInput& In)
{
    FSCCargoTransferPreview Out;
    Out.RequestedSCU = In.AmountSCU;
    auto Refuse = [&](int32 Box, int32 SCU, const FString& Why) {
        FSCCargoRefusal R;
        R.BoxId = Box;
        R.SCU = SCU;
        R.Reason = Reason(Why);
        Out.Refusals.Add(MoveTemp(R));
    };
    auto GridIndex = [&](int32 Id) { return In.Grids.IndexOfByPredicate([&](const FSCCargoGridModel& G) { return G.GridId == Id; }); };
    TArray<int32> Sources, Targets;
    for (const int32 Id : In.FromGrids)
        if (const int32 G = GridIndex(Id); G != INDEX_NONE && !In.ToGrids.Contains(Id)) Sources.AddUnique(G);
    for (const int32 Id : In.ToGrids)
        if (const int32 G = GridIndex(Id); G != INDEX_NONE) Targets.AddUnique(G);
    const FString Commodity = In.CommodityLabel.IsEmpty() ? In.Commodity.ToString() : In.CommodityLabel;
    if (Sources.IsEmpty() || Targets.IsEmpty())
    {
        Refuse(INDEX_NONE, 0, Sources.IsEmpty() ? TEXT("Choose a source grid other than the destination") : TEXT("Choose a destination grid"));
        return Out;
    }
    const FString DestLabel = In.Grids[Targets[0]].GroupLabel;
    // Drones a box needs: by force (lift with the scenario's reserve) and by volume (beams per box), whichever is more.
    auto TeamFor = [&](const FSCCargoItem& Item) { return TeamSize(In, Item, In.DronesAvailable); };

    // Boxes of the commodity on the sources that are free to move, and their refusals.
    TArray<FCandidate> Pass;
    TMap<int32, int32> GridOfItem;
    for (const int32 G : Sources)
        for (const FSCCargoPlacement& P : In.Grids[G].Placed)
        {
            const FSCCargoItem* Item = In.Items.Find(P.ItemId);
            if (!Item || !Item->bMovable || Item->Commodity != In.Commodity) continue;
            GridOfItem.Add(P.ItemId, G);
            if (In.Busy.Contains(P.ItemId)) continue; // another job carries it
            FString Why;
            bool bRotate = false;
            const bool bExcluded = !Targets.ContainsByPredicate([&](int32 T) {
                return !In.Grids[T].Lattice.ExcludedSubTypes.ContainsByPredicate([&](const FString& S) { return S.Equals(Item->SubType, ESearchCase::IgnoreCase); });
            });
            if (bExcluded)
            {
                Refuse(P.ItemId, Item->SCU, FString::Printf(TEXT("%d SCU box (%s) is not accepted by %s"), Item->SCU, *Item->SubType, *DestLabel));
                continue;
            }
            bool bFits = false;
            for (const int32 T : Targets) bFits |= FitsMaxItem(In.Grids[T].Lattice, Item->SizeM, bRotate);
            if (!bFits)
            {
                // The reason against the most permissive destination grid.
                int32 Widest = Targets[0];
                for (const int32 T : Targets)
                    if (In.Grids[T].Lattice.MaxItemM.Size() > In.Grids[Widest].Lattice.MaxItemM.Size()) Widest = T;
                FitsMaxItem(In.Grids[Widest].Lattice, Item->SizeM, bRotate, &Why, In.Grids[Widest].GroupLabel, Item->SCU);
                Refuse(P.ItemId, Item->SCU, Why);
                continue;
            }
            const int32 Drones = TeamFor(*Item);
            if (Drones == 0 && In.DronesAvailable > 0 && In.LiftForceN > 0)
            {
                Refuse(P.ItemId, Item->SCU, FString::Printf(TEXT("%d SCU box weighs %.1f t: even %d drones cannot lift it with a working reserve"),
                                                            Item->SCU, Item->MassKg / 1000.0, In.DronesAvailable));
                continue;
            }
            if (Drones == 0 || In.DronesAvailable <= 0)
            {
                Refuse(P.ItemId, Item->SCU, In.DronesAvailable <= 0 ? TEXT("No cargo drones are docked on the grids") : TEXT("The drones' lift force is unknown"));
                continue;
            }
            if (Drones > In.DronesAvailable)
            {
                int32 ByForce = 0, ByVolume = 0;
                TeamSize(In, *Item, In.DronesAvailable, &ByForce, &ByVolume);
                Refuse(P.ItemId, Item->SCU, ByVolume > ByForce
                    ? FString::Printf(TEXT("%d SCU box needs %d drones (a beam takes %.0f SCU), %d available"), Item->SCU, Drones, In.BeamMaxVolume, In.DronesAvailable)
                    : FString::Printf(TEXT("%d SCU box weighs %.1f t: it needs %d drones, %d available"), Item->SCU, Item->MassKg / 1000.0, Drones,
                                      In.DronesAvailable));
                continue;
            }
            // Covered: the dig-out must be possible on its own (enough drones to hold or room to relocate the cover);
            // covering boxes of the same commodity may simply be carried first, so they are taken as gone here.
            const TArray<int32> Cover = In.Grids[G].CoverOf(P.ItemId);
            if (!Cover.IsEmpty())
            {
                TArray<FSCCargoGridModel> Trial;
                for (const int32 S : Sources) Trial.Add(In.Grids[S]);
                Trial[Sources.IndexOfByKey(G)].Placed.RemoveAll([&](const FSCCargoPlacement& C) {
                    const FSCCargoItem* I = In.Items.Find(C.ItemId);
                    return Cover.Contains(C.ItemId) && I && I->bMovable && I->Commodity == In.Commodity && !In.Busy.Contains(C.ItemId);
                });
                TArray<FSCCargoMovePlan> Dig;
                if (!PlanDigOut(In, Trial, Sources.IndexOfByKey(G), P.ItemId, Drones, TeamFor, Dig, Why))
                {
                    Refuse(P.ItemId, Item->SCU, Why);
                    continue;
                }
            }
            Pass.Add({P.ItemId, G, P.Origin.Z, Item->SCU, Cover.Num()});
        }
    if (GridOfItem.IsEmpty())
    {
        Refuse(INDEX_NONE, 0, FString::Printf(TEXT("No %s on the source grids"), *Commodity));
        return Out;
    }
    // Least digging first, then top-down (removal order), larger first within a layer.
    Pass.Sort([](const FCandidate& A, const FCandidate& B) {
        return A.Cover != B.Cover ? A.Cover < B.Cover : A.Z != B.Z ? A.Z > B.Z : A.SCU != B.SCU ? A.SCU > B.SCU : A.Id < B.Id;
    });
    TArray<FSCCargoGridModel> Dest;
    for (const int32 T : Targets) Dest.Add(In.Grids[T]);
    int32 Evaluations = 0;
    auto TryPack = [&](const TArray<int32>& Chosen, TArray<FSCCargoMovePlan>* Moves) {
        ++Evaluations;
        TArray<FSCCargoGridModel> Grids = Dest;
        TArray<FSCCargoItem> Items;
        for (const int32 C : Chosen) Items.Add(In.Items[Pass[C].Id]);
        TArray<FSCCargoPlacement> Slots;
        TArray<int32> GridOf;
        if (!Pack(Grids, Items, Slots, GridOf)) return false;
        // Dig-outs in the chosen (removal) order on working copies of the sources: what an earlier box leaves is gone.
        TArray<FSCCargoGridModel> Working;
        for (const int32 S : Sources) Working.Add(In.Grids[S]);
        TArray<TArray<FSCCargoMovePlan>> Digs;
        for (const int32 C : Chosen)
        {
            FString Ignored;
            if (!PlanDigOut(In, Working, Sources.IndexOfByKey(Pass[C].Grid), Pass[C].Id, TeamFor(In.Items[Pass[C].Id]), TeamFor, Digs.AddDefaulted_GetRef(), Ignored))
                return false;
        }
        if (Moves)
            for (int32 I = 0; I < Chosen.Num(); ++I)
            {
                Moves->Append(Digs[I]);
                FSCCargoMovePlan M;
                M.BoxId = Items[I].Id;
                M.SCU = Items[I].SCU;
                M.FromGrid = In.Grids[Pass[Chosen[I]].Grid].GridId;
                M.ToGrid = Grids[GridOf[I]].GridId;
                M.Origin = Slots[I].Origin;
                M.Cells = Slots[I].Cells;
                M.bRotated = Slots[I].bRotated;
                M.DronesNeeded = TeamFor(Items[I]);
                M.ForBoxId = M.BoxId;
                Moves->Add(M);
            }
        return true;
    };
    // Subsets summing to Amount (depth-first in candidate order) that pack and dig out; the first one wins.
    auto Find = [&](int32 Amount, TArray<int32>& Chosen) {
        int32 Trials = 0;
        TArray<int32> Suffix;
        Suffix.SetNumZeroed(Pass.Num() + 1);
        for (int32 I = Pass.Num() - 1; I >= 0; --I) Suffix[I] = Suffix[I + 1] + Pass[I].SCU;
        TFunction<bool(int32, int32)> Walk = [&](int32 I, int32 Left) -> bool {
            if (Left == 0) return Trials++ < MaxTrialsPerAmount && Evaluations < MaxPackEvaluations && TryPack(Chosen, nullptr);
            if (I >= Pass.Num() || Suffix[I] < Left || Trials >= MaxTrialsPerAmount || Evaluations >= MaxPackEvaluations) return false;
            if (Pass[I].SCU <= Left)
            {
                Chosen.Add(I);
                if (Walk(I + 1, Left - Pass[I].SCU)) return true;
                Chosen.Pop();
            }
            return Walk(I + 1, Left);
        };
        return Walk(0, Amount);
    };
    int32 Total = 0;
    for (const FCandidate& C : Pass) Total += C.SCU;
    TBitArray<> Reachable(false, Total + 1);
    Reachable[0] = true;
    for (const FCandidate& C : Pass)
        for (int32 S = Total; S >= C.SCU; --S) Reachable[S] = Reachable[S] || Reachable[S - C.SCU];
    TMap<int32, TArray<int32>> Subsets;
    for (int32 S = 1; S <= Total; ++S)
    {
        TArray<int32> Chosen;
        if (Reachable[S] && Find(S, Chosen))
        {
            Out.AchievableSCU.Add(S);
            Subsets.Add(S, Chosen);
        }
    }
    if (Evaluations >= MaxPackEvaluations)
        Refuse(INDEX_NONE, 0, FString::Printf(TEXT("Amount search stopped after %d trials; larger amounts may be possible"), Evaluations));
    if (Out.AchievableSCU.IsEmpty())
    {
        if (!Pass.IsEmpty()) Refuse(INDEX_NONE, 0, FString::Printf(TEXT("No room on %s for any %s box"), *DestLabel, *Commodity));
        return Out;
    }
    if (In.AmountSCU <= 0) return Out; // the slider's stops only
    int32 Amount = 0;
    for (const int32 S : Out.AchievableSCU)
        if (S <= In.AmountSCU) Amount = S;
    if (Amount != In.AmountSCU)
        Refuse(INDEX_NONE, 0, Amount > 0 ? FString::Printf(TEXT("%d SCU cannot be made of whole boxes that fit; %d SCU planned"), In.AmountSCU, Amount)
                                         : FString::Printf(TEXT("The smallest amount that fits is %d SCU"), Out.AchievableSCU[0]));
    if (Amount <= 0) return Out;
    TryPack(Subsets[Amount], &Out.Moves);
    // Removal order: top-down per source (Pass order), which Subsets already follow.
    for (const FSCCargoMovePlan& M : Out.Moves)
        if (M.Kind == ESCCargoMoveKind::Carry) Out.PlannedSCU += M.SCU;
    Out.bValid = !Out.Moves.IsEmpty();
    return Out;
}
