// FSCCargoPlanner: lattice placement (yaw 0/90, gravity support, max item size, best fit) and transfer planning
// (eligible boxes, refusals with reasons, achievable amounts as packable subset sums of whole boxes).
#include "Cargo/SCCargoPlanner.h"

namespace
{
constexpr double ToleranceM = 1e-3; // 1 mm: authored sizes are exact multiples, floats are not

FVector Rotated(const FVector& S) { return FVector(S.Y, S.X, S.Z); }
double Volume(const FVector& S) { return S.X * S.Y * S.Z; }
bool Within(const FVector& S, const FVector& Max) { return S.X <= Max.X + ToleranceM && S.Y <= Max.Y + ToleranceM && S.Z <= Max.Z + ToleranceM; }
// Sides smallest first; an unauthored (zero) max side is no limit.
FVector Sorted(FVector V, bool bZeroIsUnlimited = false)
{
    for (int32 A = 0; A < 3; ++A)
        if (bZeroIsUnlimited && V[A] <= 0) V[A] = UE_BIG_NUMBER;
    if (V.X > V.Y) Swap(V.X, V.Y);
    if (V.Y > V.Z) Swap(V.Y, V.Z);
    if (V.X > V.Y) Swap(V.X, V.Y);
    return V;
}
// The authored max permitted item size holds no orientation (records put its long side in x, y or z alike): an item
// fits when its sides, sorted, are within the max's sides, sorted.
bool WithinMax(const FSCCargoLattice& L, const FVector& SizeM)
{
    return L.MaxItemM.IsNearlyZero() || Within(Sorted(SizeM), Sorted(L.MaxItemM, true));
}
bool Contains(const FSCCargoPlacement& P, const FIntVector& C)
{
    return C.X >= P.Origin.X && C.X < P.Origin.X + P.Cells.X && C.Y >= P.Origin.Y && C.Y < P.Origin.Y + P.Cells.Y && C.Z >= P.Origin.Z &&
           C.Z < P.Origin.Z + P.Cells.Z;
}
bool Excluded(const FSCCargoGridModel& G, const FSCCargoItem& I)
{
    return !I.SubType.IsEmpty() && G.Lattice.ExcludedSubTypes.ContainsByPredicate([&](const FString& S) { return S.Equals(I.SubType, ESearchCase::IgnoreCase); });
}
FString BoxName(int32 SCU) { return SCU > 0 ? FString::Printf(TEXT("%d SCU box"), SCU) : FString(TEXT("Item")); }
} // namespace

bool FSCCargoLattice::FromContainer(const FSCCargoContainerParams& C, FSCCargoLattice& Out, FString& Error)
{
    Out = FSCCargoLattice();
    Out.InteriorM = FromSC(C.InteriorDimensions);
    Out.CellM = FromSC(C.MinPermittedItemSize);
    Out.MaxItemM = FromSC(C.MaxPermittedItemSize);
    Out.ExcludedSubTypes = C.ExcludedItemSubTypes;
    if (Out.InteriorM.GetMin() <= 0) { Error = TEXT("container has no interior dimensions"); return false; }
    if (Out.CellM.GetMin() <= 0) { Error = TEXT("container has no minimum permitted item size (the lattice step)"); return false; }
    for (int32 A = 0; A < 3; ++A) Out.Cells[A] = FMath::FloorToInt(Out.InteriorM[A] / Out.CellM[A] + ToleranceM);
    if (Out.Cells.GetMin() <= 0) { Error = TEXT("minimum permitted item size exceeds the interior"); return false; }
    return true;
}
FIntVector FSCCargoLattice::CellsFor(const FVector& SizeM) const
{
    FIntVector Out;
    for (int32 A = 0; A < 3; ++A) Out[A] = FMath::Max(1, FMath::CeilToInt(SizeM[A] / CellM[A] - ToleranceM));
    return Out;
}

bool FSCCargoPlacement::Overlaps(const FSCCargoPlacement& O) const
{
    for (int32 A = 0; A < 3; ++A)
        if (Origin[A] >= O.Origin[A] + O.Cells[A] || O.Origin[A] >= Origin[A] + Cells[A]) return false;
    return true;
}
bool FSCCargoPlacement::Supports(const FSCCargoPlacement& O) const
{
    if (O.Origin.Z != Origin.Z + Cells.Z) return false;
    for (int32 A = 0; A < 2; ++A)
        if (Origin[A] >= O.Origin[A] + O.Cells[A] || O.Origin[A] >= Origin[A] + Cells[A]) return false;
    return true;
}

bool FSCCargoGridModel::IsFree(const FIntVector& Origin, const FIntVector& Cells, int32 IgnoreItem) const
{
    for (int32 A = 0; A < 3; ++A)
        if (Origin[A] < 0 || Origin[A] + Cells[A] > Lattice.Cells[A]) return false;
    FSCCargoPlacement Probe;
    Probe.Origin = Origin;
    Probe.Cells = Cells;
    for (const FSCCargoPlacement& P : Placed)
    {
        if (P.ItemId == IgnoreItem) continue;
        FSCCargoPlacement Kept = P;
        Kept.Origin -= FIntVector(P.KeepClear, P.KeepClear, 0);
        Kept.Cells += FIntVector(2 * P.KeepClear, 2 * P.KeepClear, 0);
        if (Kept.Overlaps(Probe)) return false;
    }
    return true;
}
bool FSCCargoGridModel::IsSupported(const FIntVector& Origin, const FIntVector& Cells, int32 IgnoreItem, const TSet<int32>* NoSupport) const
{
    if (Origin.Z == 0) return true;
    for (int32 X = Origin.X; X < Origin.X + Cells.X; ++X)
        for (int32 Y = Origin.Y; Y < Origin.Y + Cells.Y; ++Y)
        {
            const FIntVector Below(X, Y, Origin.Z - 1);
            if (!Placed.ContainsByPredicate([&](const FSCCargoPlacement& P) {
                    return P.ItemId != IgnoreItem && (!NoSupport || !NoSupport->Contains(P.ItemId)) && Contains(P, Below);
                }))
                return false;
        }
    return true;
}
TArray<int32> FSCCargoGridModel::ItemsOn(int32 ItemId) const
{
    TArray<int32> Out;
    if (const FSCCargoPlacement* Base = Find(ItemId))
        for (const FSCCargoPlacement& P : Placed)
            if (P.ItemId != ItemId && Base->Supports(P)) Out.Add(P.ItemId);
    return Out;
}
const FSCCargoPlacement* FSCCargoGridModel::Find(int32 ItemId) const
{
    return Placed.FindByPredicate([&](const FSCCargoPlacement& P) { return P.ItemId == ItemId; });
}

bool FSCCargoPlanner::FitsMaxItem(const FSCCargoLattice& Lattice, const FVector& SizeM, bool& bOutNeedsRotation, FString* OutWhy,
                                  const FString& GroupLabel, int32 SCU)
{
    // Upright (boxes face up) at yaw 0 or 90 inside the interior, and within the authored max in any orientation.
    const FVector& Interior = Lattice.InteriorM;
    bOutNeedsRotation = false;
    const bool bStraight = Within(SizeM, Interior), bTurned = Within(Rotated(SizeM), Interior);
    const bool bMax = WithinMax(Lattice, SizeM);
    if (bMax && (bStraight || bTurned))
    {
        bOutNeedsRotation = !bStraight;
        return true;
    }
    if (OutWhy)
    {
        const FString Who = GroupLabel.IsEmpty() ? FString(TEXT("the destination")) : GroupLabel;
        if (SizeM.Z > Interior.Z + ToleranceM)
            *OutWhy = FString::Printf(TEXT("%s is %.1f m tall; %s grids are %.1f m tall"), *BoxName(SCU), SizeM.Z, *Who, Interior.Z);
        else if (!bMax)
        {
            // Name the side, longest first, that exceeds the max's side of the same rank.
            const FVector Box = Sorted(SizeM), Max = Sorted(Lattice.MaxItemM, true);
            if (Box.Z > Max.Z + ToleranceM)
                *OutWhy = FString::Printf(TEXT("%s is %.1f m long; %s grids take up to %.1f m"), *BoxName(SCU), Box.Z, *Who, Max.Z);
            else if (Box.Y > Max.Y + ToleranceM)
                *OutWhy = FString::Printf(TEXT("%s is %.1f m wide; %s grids take up to %.1f m wide"), *BoxName(SCU), Box.Y, *Who, Max.Y);
            else
                *OutWhy = FString::Printf(TEXT("%s is %.1f m thick; %s grids take up to %.1f m thick"), *BoxName(SCU), Box.X, *Who, Max.X);
        }
        else
            *OutWhy = FString::Printf(TEXT("%s is %.1f x %.1f m; %s grids are %.1f x %.1f m"), *BoxName(SCU), SizeM.X, SizeM.Y, *Who, Interior.X, Interior.Y);
    }
    return false;
}

TArray<int32> FSCCargoGridModel::CoverOf(int32 ItemId) const
{
    TArray<int32> Out;
    for (TArray<int32> Frontier = ItemsOn(ItemId); !Frontier.IsEmpty();)
    {
        const int32 Id = Frontier.Pop();
        if (Out.Contains(Id)) continue;
        Out.Add(Id);
        Frontier.Append(ItemsOn(Id));
    }
    Out.Sort([&](int32 A, int32 B) { return Find(A)->Origin.Z > Find(B)->Origin.Z; });
    return Out;
}

bool FSCCargoPlanner::FindSlot(const FSCCargoGridModel& Grid, const FSCCargoItem& Item, FSCCargoPlacement& Out)
{
    return FindSlot(Grid, Item, Out, nullptr, nullptr);
}

bool FSCCargoPlanner::FindSlot(const FSCCargoGridModel& Grid, const FSCCargoItem& Item, FSCCargoPlacement& Out, const TSet<int32>* NoSupport,
                               const TFunction<bool(const FSCCargoPlacement&)>& Reject)
{
    if (Excluded(Grid, Item)) return false;
    if (!WithinMax(Grid.Lattice, Item.SizeM)) return false;
    struct FCandidate { FIntVector Origin, Cells; bool bRotated; int32 Contact; bool bAligned; };
    TOptional<FCandidate> Best;
    auto Occupied = [&](const FIntVector& C) {
        for (int32 A = 0; A < 3; ++A)
            if (C[A] < 0 || C[A] >= Grid.Lattice.Cells[A]) return true; // walls and floor count as contact
        return Grid.Placed.ContainsByPredicate([&](const FSCCargoPlacement& P) { return Contains(P, C); });
    };
    auto Contact = [&](const FIntVector& O, const FIntVector& S) {
        int32 N = 0;
        for (int32 Z = O.Z; Z < O.Z + S.Z; ++Z)
        {
            for (int32 Y = O.Y; Y < O.Y + S.Y; ++Y) N += Occupied({O.X - 1, Y, Z}) + Occupied({O.X + S.X, Y, Z});
            for (int32 X = O.X; X < O.X + S.X; ++X) N += Occupied({X, O.Y - 1, Z}) + Occupied({X, O.Y + S.Y, Z});
        }
        return N;
    };
    for (int32 Turn = 0; Turn < 2; ++Turn)
    {
        const bool bRotated = Turn == 1;
        if (bRotated && FMath::IsNearlyEqual(Item.SizeM.X, Item.SizeM.Y, ToleranceM)) break; // a square footprint turns into itself
        const FVector Size = bRotated ? Rotated(Item.SizeM) : Item.SizeM;
        if (!Within(Size, Grid.Lattice.InteriorM)) continue; // upright, at this yaw
        const FIntVector S = Grid.Lattice.CellsFor(Size); // per axis from the physical size as turned (cells may be unequal)
        // Lengthwise to the hold's entrance first, then the lowest layer, then the most contact.
        const bool bAligned = Grid.PreferredLongAxis == 0 || (Grid.PreferredLongAxis == 1 ? Size.X >= Size.Y - ToleranceM : Size.Y >= Size.X - ToleranceM);
        for (int32 Z = 0; Z + S.Z <= Grid.Lattice.Cells.Z; ++Z)
        {
            if (Best && Best->bAligned >= bAligned && Best->Origin.Z < Z) break; // lowest layer first
            for (int32 Y = 0; Y + S.Y <= Grid.Lattice.Cells.Y; ++Y)
                for (int32 X = 0; X + S.X <= Grid.Lattice.Cells.X; ++X)
                {
                    const FIntVector O(X, Y, Z);
                    if (!Grid.IsFree(O, S) || !Grid.IsSupported(O, S, INDEX_NONE, NoSupport)) continue;
                    if (Reject)
                    {
                        FSCCargoPlacement Candidate;
                        Candidate.ItemId = Item.Id;
                        Candidate.Origin = O;
                        Candidate.Cells = S;
                        Candidate.bRotated = bRotated;
                        if (Reject(Candidate)) continue;
                    }
                    const int32 C = Contact(O, S);
                    if (!Best || (bAligned != Best->bAligned ? bAligned : O.Z != Best->Origin.Z ? O.Z < Best->Origin.Z : C > Best->Contact))
                        Best = FCandidate{O, S, bRotated, C, bAligned};
                }
        }
    }
    if (!Best) return false;
    Out.ItemId = Item.Id;
    Out.Origin = Best->Origin;
    Out.Cells = Best->Cells;
    Out.bRotated = Best->bRotated;
    return true;
}

bool FSCCargoPlanner::Pack(TArray<FSCCargoGridModel>& Grids, const TArray<FSCCargoItem>& Items, TArray<FSCCargoPlacement>& OutPlacements,
                           TArray<int32>& OutGrid)
{
    OutPlacements.SetNum(Items.Num());
    OutGrid.Init(INDEX_NONE, Items.Num());
    TArray<int32> Order;
    for (int32 I = 0; I < Items.Num(); ++I) Order.Add(I);
    Order.StableSort([&](int32 A, int32 B) { return Volume(Items[A].SizeM) > Volume(Items[B].SizeM) + 1e-9; });
    for (const int32 I : Order)
    {
        bool bPlaced = false;
        for (int32 G = 0; G < Grids.Num() && !bPlaced; ++G)
            if (FindSlot(Grids[G], Items[I], OutPlacements[I]))
            {
                Grids[G].Placed.Add(OutPlacements[I]);
                OutGrid[I] = G;
                bPlaced = true;
            }
        if (!bPlaced) return false;
    }
    return true;
}

int32 FSCCargoPlanner::TeamSize(const FSCCargoPlanInput& In, const FSCCargoItem& Item, int32 MaxDrones, int32* OutByForce, int32* OutByVolume)
{
    const int32 ByVolume = In.BeamMaxVolume > 0 && Item.Volume > 0 ? FMath::CeilToInt(Item.Volume / In.BeamMaxVolume - 1e-9) : 1;
    // Force with the reserve, for teams of at least the volume's size (a bigger team has more lift, never less).
    int32 ByForce = DronesFor(Item.MassKg, In.GravityMS2, In.LiftForceN);
    if (In.MinLoadAccelMS2 > 0 || In.DroneMassKg > 0)
        ByForce = DronesFor(Item.MassKg, In.GravityMS2, In.LiftForceN, In.DroneMassKg, In.MinLoadAccelMS2, FMath::Max(MaxDrones, ByVolume));
    if (OutByForce) *OutByForce = ByForce;
    if (OutByVolume) *OutByVolume = ByVolume;
    if (ByForce == 0) return 0;
    const int32 Team = FMath::Max(ByForce, ByVolume); // may exceed MaxDrones: the caller refuses with the count
    if (In.MinLoadAccelMS2 > 0 || In.DroneMassKg > 0)
        return (Team * In.LiftForceN - Item.MassKg * In.GravityMS2) / FMath::Max(Team * In.DroneMassKg + Item.MassKg, 1e-6) >= In.MinLoadAccelMS2 ? Team : 0;
    return Team;
}

int32 FSCCargoPlanner::DronesFor(double MassKg, double GravityMS2, double LiftForceN, double DroneKg, double MinAccelMS2, int32 MaxDrones)
{
    if (LiftForceN <= 0 || GravityMS2 <= 0) return 0;
    for (int32 N = FMath::Max(1, DronesFor(MassKg, GravityMS2, LiftForceN)); N <= FMath::Max(MaxDrones, 1); ++N)
        if ((N * LiftForceN - MassKg * GravityMS2) / FMath::Max(N * DroneKg + MassKg, 1e-6) >= MinAccelMS2) return N;
    return 0;
}

bool FSCCargoPlanner::Substitute(const FSCCargoPlanInput& In, const FSCCargoMovePlan& Failed, const TSet<int32>& Excluded,
                                 TFunctionRef<bool(int32 BoxId, FString& Why)> CanPick, FSCCargoMovePlan& Out, TArray<FString>& OutWhy)
{
    const FSCCargoItem* Original = In.Items.Find(Failed.BoxId);
    if (!Original) return false;
    struct FCandidate { int32 Id, Grid; };
    TArray<FCandidate> Candidates;
    for (int32 G = 0; G < In.Grids.Num(); ++G)
        if (In.FromGrids.Contains(In.Grids[G].GridId))
            for (const FSCCargoPlacement& P : In.Grids[G].Placed)
            {
                const FSCCargoItem* I = In.Items.Find(P.ItemId);
                if (I && I->bMovable && I->Commodity == Original->Commodity && P.ItemId != Failed.BoxId && !In.Busy.Contains(P.ItemId) &&
                    !Excluded.Contains(P.ItemId) && I->SCU <= Original->SCU) // never more than was asked for
                    Candidates.Add({P.ItemId, G});
            }
    // The same size first (the transferred amount stays), then the largest smaller box (the job is partly done).
    Candidates.Sort([&](const FCandidate& A, const FCandidate& B) {
        const FSCCargoItem &IA = In.Items[A.Id], &IB = In.Items[B.Id];
        const bool SA = IA.SCU == Original->SCU, SB = IB.SCU == Original->SCU;
        return SA != SB ? SA : IA.SCU != IB.SCU ? IA.SCU > IB.SCU : A.Id < B.Id;
    });
    const int32 MaxTeam = In.DronesAvailable;
    for (const FCandidate& C : Candidates)
    {
        const FSCCargoItem& Item = In.Items[C.Id];
        const int32 Drones = TeamSize(In, Item, MaxTeam);
        if (Drones == 0 || Drones > MaxTeam) continue;
        FString Why;
        if (!In.Grids[C.Grid].ItemsOn(C.Id).IsEmpty())
        {
            OutWhy.Add(FString::Printf(TEXT("%d SCU box on %s is covered by another box"), Item.SCU, *In.Grids[C.Grid].Label));
            continue;
        }
        if (!CanPick(C.Id, Why)) { OutWhy.Add(Why); continue; }
        for (const int32 To : In.ToGrids)
        {
            const FSCCargoGridModel* Dest = In.Grids.FindByPredicate([&](const FSCCargoGridModel& G) { return G.GridId == To; });
            FSCCargoPlacement Slot;
            if (!Dest || !FindSlot(*Dest, Item, Slot)) continue;
            Out = Failed;
            Out.BoxId = C.Id;
            Out.SCU = Item.SCU;
            Out.FromGrid = In.Grids[C.Grid].GridId;
            Out.ToGrid = To;
            Out.Origin = Slot.Origin;
            Out.Cells = Slot.Cells;
            Out.bRotated = Slot.bRotated;
            Out.DronesNeeded = Drones;
            return true;
        }
    }
    return false;
}

int32 FSCCargoPlanner::DronesFor(double MassKg, double GravityMS2, double LiftForceN)
{
    if (LiftForceN <= 0 || GravityMS2 <= 0) return 0;
    return FMath::Max(1, FMath::CeilToInt(MassKg * GravityMS2 / LiftForceN - 1e-9));
}
