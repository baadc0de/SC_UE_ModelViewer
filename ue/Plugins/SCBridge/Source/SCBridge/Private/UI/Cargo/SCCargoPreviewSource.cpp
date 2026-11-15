// UI-PREVIEW DATA, NOT GAME DATA (WP-K1c): a synthetic cargo world for laying out the mobiGlas and the callouts while
// the scene has no cargo subsystem content (`sc.MobiGlas.Preview 1`). Grids are placed around the camera at the
// moment the preview starts; boxes, a drone dock and two drones fill them; Submit runs a timed fake job through every
// state so progress bars, callouts and refusals can be seen. Names and numbers here are illustrative only — the real
// values always come from the package via USCCargoSubsystem.
#include "SCCargoViewSource.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"

#define LOCTEXT_NAMESPACE "SCMobiGlasPreview"

namespace
{
constexpr double CellCm = 125, LiftPerDroneKg = 1000;
constexpr double OpenSeconds = 3, MoveSeconds = 12, CloseSeconds = 3;

FSCCargoRefusal Refusal(int32 BoxId, int32 SCU, const FText& Reason)
{
    FSCCargoRefusal R;
    R.BoxId = BoxId;
    R.SCU = SCU;
    R.Reason = Reason;
    return R;
}

struct FMove
{
    FSCCargoBoxView Box; // as carried
    int32 From = INDEX_NONE, To = INDEX_NONE;
    FIntVector Origin, Cells;
    TArray<int32> Drones;
};
struct FJob
{
    FSCCargoJobView View;
    TArray<FMove> Moves;
    double Phase = 0; // seconds into the current state
    TArray<int32> ClosedBefore; // destination grids to close again
};

class FPreviewSource final : public ISCCargoViewSource
{
public:
    explicit FPreviewSource(const FTransform& InFrame) : Frame(InFrame) { Build(); }

    virtual bool IsPreview() const override { return true; }
    virtual int32 GetRevision() const override { return Revision; }
    virtual float GetNearbyRadiusM() const override { return 50; }
    virtual TArray<FSCCargoGridView> GetGrids(const FVector& Viewer, float RadiusM) const override
    {
        TArray<FSCCargoGridView> Out;
        for (FSCCargoGridView G : Grids)
        {
            G.DistanceM = float(FVector::Dist(Viewer, G.WorldBounds.GetClosestPointTo(Viewer)) / 100);
            if (RadiusM <= 0 || G.DistanceM <= RadiusM) Out.Add(G);
        }
        Out.StableSort([](const FSCCargoGridView& A, const FSCCargoGridView& B) { return A.DistanceM < B.DistanceM; });
        return Out;
    }
    virtual TArray<FSCCargoCommodityView> GetCommodities(const TArray<int32>& Ids) const override
    {
        TMap<FName, FSCCargoCommodityView> By;
        for (const FSCCargoGridView& G : Grids)
            if (Ids.IsEmpty() || Ids.Contains(G.GridId))
                for (const FSCCargoBoxView& B : G.Boxes)
                {
                    if (B.State != ESCCargoBoxState::Stored) continue;
                    FSCCargoCommodityView& C = By.FindOrAdd(B.Commodity);
                    C.Commodity = B.Commodity;
                    C.Label = B.CommodityLabel;
                    C.TotalSCU += B.SCU;
                    C.BoxSizesSCU.AddUnique(B.SCU);
                }
        TArray<FSCCargoCommodityView> Out;
        for (auto& Pair : By) { Pair.Value.BoxSizesSCU.Sort(); Out.Add(Pair.Value); }
        Out.Sort([](const FSCCargoCommodityView& A, const FSCCargoCommodityView& B) { return A.TotalSCU > B.TotalSCU; });
        return Out;
    }
    virtual TArray<FSCCargoDroneView> GetDrones() const override { return Drones; }
    virtual TArray<FSCCargoJobView> GetJobs() const override
    {
        TArray<FSCCargoJobView> Out;
        for (int32 I = Jobs.Num() - 1; I >= 0; --I) Out.Add(Jobs[I].View);
        Out.StableSort([](const FSCCargoJobView& A, const FSCCargoJobView& B) { return Active(A.State) && !Active(B.State); });
        return Out;
    }
    virtual TArray<FSCCargoBoxView> GetMovingBoxes() const override
    {
        TArray<FSCCargoBoxView> Out;
        for (const FJob& J : Jobs)
            if (J.View.State == ESCCargoJobState::Moving && J.Moves.IsValidIndex(J.View.MovesDone)) Out.Add(J.Moves[J.View.MovesDone].Box);
        return Out;
    }
    virtual FSCCargoTransferPreview Preview(const FSCCargoTransferRequest& R) const override;
    virtual int32 Submit(const FSCCargoTransferRequest& R, FText& OutError) override;
    virtual bool Cancel(int32 JobId) override;
    virtual bool RequestGridAccess(int32 GridId, bool bOpen, FText& OutError) override;
    virtual void Advance(float Dt) override;

private:
    static bool Active(ESCCargoJobState S) { return S != ESCCargoJobState::Done && S != ESCCargoJobState::Cancelled && S != ESCCargoJobState::Failed; }
    FSCCargoGridView* Grid(int32 Id) { return Grids.FindByPredicate([Id](const FSCCargoGridView& G) { return G.GridId == Id; }); }
    const FSCCargoGridView* Grid(int32 Id) const { return Grids.FindByPredicate([Id](const FSCCargoGridView& G) { return G.GridId == Id; }); }
    FVector CellWorld(const FSCCargoGridView& G, const FVector& Cell) const { return G.LatticeToWorld.TransformPosition(Cell * CellCm); }
    void Place(FSCCargoGridView& G, FSCCargoBoxView& B) const
    {
        B.GridId = G.GridId;
        B.WorldCenter = CellWorld(G, FVector(B.Origin) + FVector(B.Cells) / 2);
        B.WorldBounds = FBox::BuildAABB(B.WorldCenter, FVector(B.Cells) * CellCm / 2);
    }
    void Recount(FSCCargoGridView& G) const
    {
        G.UsedSCU = 0;
        for (const FSCCargoBoxView& B : G.Boxes) G.UsedSCU += B.SCU;
        // Environment platforms are fixed open; ship plates retract only when empty (they carry cargo extended only).
        const bool bFixed = G.Kind == ESCCargoGridKind::Environment;
        // One mechanism per ship: every grid of the group must be empty to retract (preview stand-in for shared clips).
        int32 GroupLoad = 0;
        for (const FSCCargoGridView& O : Grids)
            if (O.GroupKey == G.GroupKey) GroupLoad += (O.GridId == G.GridId ? G.UsedSCU : O.UsedSCU) + O.ReservedSCU;
        const bool bLoaded = GroupLoad > 0;
        G.bCanClose = !bFixed && G.Access == ESCCargoGridAccess::Open && !bLoaded;
        G.CloseBlockedReason = bFixed || G.bCanClose ? FText()
            : bLoaded ? FText::Format(LOCTEXT("Loaded", "{0} carries {1} SCU; unload it before retracting."), G.GroupLabel, GroupLoad)
                      : LOCTEXT("NotExtended", "Not extended.");
    }
    void SetAccess(FSCCargoGridView& G, ESCCargoGridAccess Access)
    {
        G.Access = Access;
        Recount(G);
        ++Revision;
    }
    bool Fits(const FSCCargoGridView& G, const TArray<FIntVector>& Taken, const FIntVector& O, const FIntVector& C) const;
    bool FindSlot(const FSCCargoGridView& G, const TArray<FIntVector>& Taken, FIntVector Cells, FIntVector& Origin, FIntVector& Placed) const;
    TArray<FIntVector> Occupied(const FSCCargoGridView& G) const;
    void AddGrid(ESCCargoGridKind Kind, FName Group, const FText& GroupLabel, const FText& Label, FIntVector Cells, FIntVector MaxCells,
                 ESCCargoGridAccess Access, const FVector& LocalOrigin);
    void AddBox(int32 GridIndex, FName Commodity, const FText& Label, int32 SCU, FIntVector Origin, FIntVector Cells);
    void Build();

    FTransform Frame;
    TArray<FSCCargoGridView> Grids;
    TArray<FSCCargoDroneView> Drones;
    TArray<FJob> Jobs;
    int32 Revision = 1, NextBox = 1, NextJob = 1;
    TMap<int32, double> Moving; // grid id -> seconds left of an extend / retract asked for directly
};

void FPreviewSource::AddGrid(ESCCargoGridKind Kind, FName Group, const FText& GroupLabel, const FText& Label, FIntVector Cells, FIntVector MaxCells,
                             ESCCargoGridAccess Access, const FVector& LocalOrigin)
{
    FSCCargoGridView& G = Grids.AddDefaulted_GetRef();
    G.GridId = Grids.Num();
    G.Kind = Kind;
    G.GroupKey = Group;
    G.GroupLabel = GroupLabel;
    G.GridLabel = Label;
    G.Cells = Cells;
    G.CellSizeM = FVector(CellCm / 100);
    G.MaxItemSizeM = FVector(MaxCells) * CellCm / 100;
    G.CapacitySCU = Cells.X * Cells.Y * Cells.Z;
    G.Access = Access;
    G.LatticeToWorld = FTransform(Frame.GetRotation(), Frame.TransformPosition(LocalOrigin));
    G.WorldBounds = FBox(ForceInit);
    for (int32 I = 0; I < 8; ++I) G.WorldBounds += CellWorld(G, FVector(I & 1 ? Cells.X : 0, I & 2 ? Cells.Y : 0, I & 4 ? Cells.Z : 0));
}

void FPreviewSource::AddBox(int32 GridIndex, FName Commodity, const FText& Label, int32 SCU, FIntVector Origin, FIntVector Cells)
{
    FSCCargoGridView& G = Grids[GridIndex];
    FSCCargoBoxView& B = G.Boxes.AddDefaulted_GetRef();
    B.BoxId = NextBox++;
    B.Commodity = Commodity;
    B.CommodityLabel = Label;
    B.SCU = SCU;
    B.Origin = Origin;
    B.Cells = Cells;
    B.SizeM = FVector(Cells) * CellCm / 100;
    B.MassKg = 150 + SCU * 75;
    Place(G, B);
    Recount(G);
}

void FPreviewSource::Build()
{
    const FText Hull = LOCTEXT("Hull", "MISC Hull A"), Elevator = LOCTEXT("Elevator", "Freight elevator");
    AddGrid(ESCCargoGridKind::Environment, "elevator", Elevator, LOCTEXT("Platform", "Platform"), {6, 8, 2}, {2, 8, 2}, ESCCargoGridAccess::Open, {700, -1250, 0});
    const TCHAR* Names[] = {TEXT("Grid 1 · top left"), TEXT("Grid 2 · top right"), TEXT("Grid 3 · bottom left"), TEXT("Grid 4 · bottom right")};
    const FVector Origins[] = {{900, 100, 280}, {900, 700, 280}, {900, 100, 0}, {900, 700, 0}};
    for (int32 I = 0; I < 4; ++I)
        AddGrid(ESCCargoGridKind::Vehicle, "hull_a", Hull, FText::FromString(Names[I]), {2, 4, 2}, {2, 4, 2}, ESCCargoGridAccess::Closed, Origins[I]);
    const FText Agri = LOCTEXT("Agri", "Agricultural supplies"), Med = LOCTEXT("Med", "Medical supplies"), Scrap = LOCTEXT("Scrap", "Scrap");
    AddBox(0, "agri", Agri, 32, {0, 0, 0}, {2, 8, 2});
    AddBox(0, "agri", Agri, 16, {2, 0, 0}, {2, 4, 2});
    AddBox(0, "agri", Agri, 16, {2, 4, 0}, {2, 4, 2});
    AddBox(0, "med", Med, 8, {4, 0, 0}, {2, 2, 2});
    AddBox(0, "med", Med, 8, {4, 2, 0}, {2, 2, 2});
    AddBox(0, "scrap", Scrap, 4, {4, 4, 0}, {2, 2, 1});
    Grids[1].BlockedCells.Add({0, 0, 0}); // drone dock module (1 SCU footprint) on grid 1
    for (int32 I = 0; I < 2; ++I)
    {
        FSCCargoDroneView& D = Drones.AddDefaulted_GetRef();
        D.DroneId = I + 1;
        D.Label = FText::Format(LOCTEXT("Drone", "Drone {0}"), I + 1);
        D.WorldLocation = CellWorld(Grids[1], FVector(0.5, 0.3 + 0.4 * I, 0.5));
        D.WorldBounds = FBox::BuildAABB(D.WorldLocation, FVector(40));
    }
    // History so the Transfers column has finished rows.
    Jobs.Reserve(16);
    FJob& Done = Jobs.AddDefaulted_GetRef();
    Done.View.JobId = NextJob++;
    Done.View.State = ESCCargoJobState::Done;
    Done.View.Summary = LOCTEXT("DoneSummary", "Medical supplies · 16 SCU · MISC Hull A → Freight elevator");
    Done.View.StateText = LOCTEXT("DoneState", "Done");
    Done.View.TotalSCU = Done.View.DoneSCU = 16;
    Done.View.Moves = Done.View.MovesDone = 1;
    Done.View.Progress = 1;
    FJob& Failed = Jobs.AddDefaulted_GetRef();
    Failed.View = Done.View;
    Failed.View.JobId = NextJob++;
    Failed.View.State = ESCCargoJobState::Failed;
    Failed.View.Summary = LOCTEXT("FailSummary", "Scrap · 4 SCU · Freight elevator → MISC Hull A");
    Failed.View.StateText = LOCTEXT("FailState", "Failed");
    Failed.View.FailureReason = LOCTEXT("FailReason", "Preview: the platform was lowered during the transfer.");
    Failed.View.DoneSCU = 0;
    Failed.View.TotalSCU = 4;
    Failed.View.MovesDone = 0;
    Failed.View.Progress = 0;
    for (FSCCargoGridView& G : Grids) Recount(G);
}

TArray<FIntVector> FPreviewSource::Occupied(const FSCCargoGridView& G) const
{
    TArray<FIntVector> Taken = G.BlockedCells;
    for (const FSCCargoBoxView& B : G.Boxes)
        for (int32 X = 0; X < B.Cells.X; ++X)
            for (int32 Y = 0; Y < B.Cells.Y; ++Y)
                for (int32 Z = 0; Z < B.Cells.Z; ++Z) Taken.Add(B.Origin + FIntVector(X, Y, Z));
    return Taken;
}

bool FPreviewSource::Fits(const FSCCargoGridView& G, const TArray<FIntVector>& Taken, const FIntVector& O, const FIntVector& C) const
{
    if (O.X + C.X > G.Cells.X || O.Y + C.Y > G.Cells.Y || O.Z + C.Z > G.Cells.Z) return false;
    for (int32 X = 0; X < C.X; ++X)
        for (int32 Y = 0; Y < C.Y; ++Y)
        {
            if (O.Z > 0 && !Taken.Contains(O + FIntVector(X, Y, -1))) return false; // rests fully on boxes
            for (int32 Z = 0; Z < C.Z; ++Z)
                if (Taken.Contains(O + FIntVector(X, Y, Z))) return false;
        }
    return true;
}

bool FPreviewSource::FindSlot(const FSCCargoGridView& G, const TArray<FIntVector>& Taken, FIntVector Cells, FIntVector& Origin, FIntVector& Placed) const
{
    for (int32 Turn = 0; Turn < 2; ++Turn, Swap(Cells.X, Cells.Y))
        for (int32 Z = 0; Z < G.Cells.Z; ++Z)
            for (int32 Y = 0; Y < G.Cells.Y; ++Y)
                for (int32 X = 0; X < G.Cells.X; ++X)
                    if (Fits(G, Taken, {X, Y, Z}, Cells)) { Origin = {X, Y, Z}; Placed = Cells; return true; }
    return false;
}

FSCCargoTransferPreview FPreviewSource::Preview(const FSCCargoTransferRequest& R) const
{
    FSCCargoTransferPreview Out;
    Out.RequestedSCU = R.AmountSCU;
    TArray<const FSCCargoBoxView*> Candidates;
    for (int32 Id : R.FromGrids)
        if (const FSCCargoGridView* G = Grid(Id); G && !R.ToGrids.Contains(Id))
            for (const FSCCargoBoxView& B : G->Boxes)
                if (B.State == ESCCargoBoxState::Stored && B.Commodity == R.Commodity) Candidates.Add(&B);
    Candidates.Sort([](const FSCCargoBoxView& A, const FSCCargoBoxView& B) { return A.SCU > B.SCU; });
    TArray<const FSCCargoGridView*> Dest;
    for (int32 Id : R.ToGrids)
        if (const FSCCargoGridView* G = Grid(Id); G && !R.FromGrids.Contains(Id)) Dest.Add(G);
    if (Candidates.IsEmpty() || Dest.IsEmpty())
    {
        Out.Refusals.Add(Refusal(INDEX_NONE, 0, Dest.IsEmpty() ? LOCTEXT("NoDest", "Choose a destination other than the source.")
                                                        : LOCTEXT("NoBoxes", "The source holds none of this commodity.")));
        return Out;
    }
    // Size check against the destinations' largest permitted item (yaw 90° allowed, no tipping).
    TArray<const FSCCargoBoxView*> Movable;
    for (const FSCCargoBoxView* B : Candidates)
    {
        const bool bFits = Dest.ContainsByPredicate([B](const FSCCargoGridView* G) {
            const FVector& M = G->MaxItemSizeM;
            return B->SizeM.Z <= M.Z + 1e-3 && ((B->SizeM.X <= M.X + 1e-3 && B->SizeM.Y <= M.Y + 1e-3) || (B->SizeM.X <= M.Y + 1e-3 && B->SizeM.Y <= M.X + 1e-3));
        });
        if (bFits) { Movable.Add(B); continue; }
        const double Long = FMath::Max(B->SizeM.X, B->SizeM.Y), Max = FMath::Max(Dest[0]->MaxItemSizeM.X, Dest[0]->MaxItemSizeM.Y);
        FNumberFormattingOptions OneDecimal;
        OneDecimal.SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1);
        Out.Refusals.Add(Refusal(B->BoxId, B->SCU, FText::Format(LOCTEXT("TooLong", "{0} SCU box is {1} m long; {2} grids take up to {3} m"), B->SCU,
                                                          FText::AsNumber(Long, &OneDecimal), Dest[0]->GroupLabel, FText::AsNumber(Max, &OneDecimal))));
    }
    // Every subset of movable boxes that the destinations can hold: the slider stops; the request's own subset: moves.
    const int32 N = FMath::Min(Movable.Num(), 10);
    TArray<FSCCargoMovePlan> Chosen;
    for (int32 Mask = 1; Mask < (1 << N); ++Mask)
    {
        int32 Sum = 0;
        for (int32 I = 0; I < N; ++I) Sum += Mask & (1 << I) ? Movable[I]->SCU : 0;
        if (Out.AchievableSCU.Contains(Sum) && Sum != R.AmountSCU) continue;
        if (Sum == R.AmountSCU && !Chosen.IsEmpty()) continue;
        TArray<TArray<FIntVector>> Taken;
        for (const FSCCargoGridView* G : Dest) Taken.Add(Occupied(*G));
        TArray<FSCCargoMovePlan> Moves;
        bool bAll = true;
        for (int32 I = 0; I < N && bAll; ++I)
        {
            if (!(Mask & (1 << I))) continue;
            bool bPlaced = false;
            for (int32 D = 0; D < Dest.Num() && !bPlaced; ++D)
            {
                FIntVector Origin, Cells;
                if (!FindSlot(*Dest[D], Taken[D], Movable[I]->Cells, Origin, Cells)) continue;
                for (int32 X = 0; X < Cells.X; ++X)
                    for (int32 Y = 0; Y < Cells.Y; ++Y)
                        for (int32 Z = 0; Z < Cells.Z; ++Z) Taken[D].Add(Origin + FIntVector(X, Y, Z));
                FSCCargoMovePlan& M = Moves.AddDefaulted_GetRef();
                M.BoxId = Movable[I]->BoxId;
                M.SCU = Movable[I]->SCU;
                M.FromGrid = Movable[I]->GridId;
                M.ToGrid = Dest[D]->GridId;
                M.Origin = Origin;
                M.Cells = Cells;
                M.bRotated = Cells.X != Movable[I]->Cells.X;
                M.DronesNeeded = FMath::CeilToInt32(Movable[I]->MassKg / LiftPerDroneKg);
                bPlaced = true;
            }
            bAll = bPlaced;
        }
        if (!bAll) continue;
        Out.AchievableSCU.AddUnique(Sum);
        if (Sum == R.AmountSCU) Chosen = Moves;
    }
    Out.AchievableSCU.Sort();
    Out.Moves = Chosen;
    for (const FSCCargoMovePlan& M : Chosen) Out.PlannedSCU += M.SCU;
    if (R.AmountSCU > 0 && Chosen.IsEmpty())
        Out.Refusals.Add(Refusal(INDEX_NONE, R.AmountSCU, FText::Format(LOCTEXT("NoCombo", "No set of whole boxes that fits makes {0} SCU."), R.AmountSCU)));
    Out.bValid = !Chosen.IsEmpty();
    Out.EstimatedSeconds = Chosen.IsEmpty() ? 0 : float(OpenSeconds + MoveSeconds * Chosen.Num() + CloseSeconds);
    return Out;
}

int32 FPreviewSource::Submit(const FSCCargoTransferRequest& R, FText& OutError)
{
    const FSCCargoTransferPreview P = Preview(R);
    if (!P.bValid)
    {
        OutError = P.Refusals.IsEmpty() ? LOCTEXT("Nothing", "Nothing to move.") : P.Refusals.Last().Reason;
        return INDEX_NONE;
    }
    FJob& J = Jobs.AddDefaulted_GetRef();
    J.View.JobId = NextJob++;
    J.View.State = ESCCargoJobState::OpeningGrids;
    J.View.FromGrids = R.FromGrids;
    J.View.ToGrids = R.ToGrids;
    J.View.TotalSCU = P.PlannedSCU;
    J.View.Moves = P.Moves.Num();
    J.View.EtaSeconds = P.EstimatedSeconds;
    for (const FSCCargoMovePlan& M : P.Moves)
    {
        FSCCargoGridView* From = Grid(M.FromGrid);
        FSCCargoBoxView* Box = From ? From->Boxes.FindByPredicate([&M](const FSCCargoBoxView& B) { return B.BoxId == M.BoxId; }) : nullptr;
        if (!Box) continue;
        Box->State = ESCCargoBoxState::Reserved;
        Box->JobId = J.View.JobId;
        FMove& Move = J.Moves.AddDefaulted_GetRef();
        Move.Box = *Box;
        Move.From = M.FromGrid;
        Move.To = M.ToGrid;
        Move.Origin = M.Origin;
        Move.Cells = M.Cells;
        for (int32 D = 0; D < FMath::Min(M.DronesNeeded, Drones.Num()); ++D) Move.Drones.AddUnique(Drones[D].DroneId);
        if (FSCCargoGridView* To = Grid(M.ToGrid)) To->ReservedSCU += M.SCU;
        J.View.Drones.Append(Move.Drones);
    }
    const FSCCargoGridView* First = Grid(P.Moves[0].FromGrid);
    const FSCCargoGridView* Target = Grid(P.Moves[0].ToGrid);
    J.View.Summary = FText::Format(LOCTEXT("Summary", "{0} · {1} SCU · {2} → {3}"), J.Moves[0].Box.CommodityLabel, P.PlannedSCU,
                                   First ? First->GroupLabel : FText(), Target ? Target->GroupLabel : FText());
    for (int32 Id : R.ToGrids)
        if (FSCCargoGridView* G = Grid(Id); G && G->Access == ESCCargoGridAccess::Closed)
        {
            SetAccess(*G, ESCCargoGridAccess::Opening);
            J.ClosedBefore.Add(Id);
        }
    J.View.StateText = LOCTEXT("Opening", "Opening grids");
    for (FSCCargoGridView& G : Grids) Recount(G);
    ++Revision;
    return J.View.JobId;
}

bool FPreviewSource::Cancel(int32 JobId)
{
    FJob* J = Jobs.FindByPredicate([JobId](const FJob& X) { return X.View.JobId == JobId; });
    if (!J || !Active(J->View.State)) return false;
    for (int32 M = J->View.MovesDone; M < J->Moves.Num(); ++M)
    {
        const FMove& Move = J->Moves[M];
        if (FSCCargoGridView* To = Grid(Move.To)) To->ReservedSCU -= Move.Box.SCU;
        if (FSCCargoGridView* From = Grid(Move.From))
        {
            FSCCargoBoxView* Box = From->Boxes.FindByPredicate([&Move](const FSCCargoBoxView& B) { return B.BoxId == Move.Box.BoxId; });
            if (!Box) { Box = &From->Boxes.Add_GetRef(Move.Box); Place(*From, *Box); Recount(*From); } // carried box goes back
            Box->State = ESCCargoBoxState::Stored;
            Box->JobId = INDEX_NONE;
        }
    }
    for (int32 Id : J->ClosedBefore)
        if (FSCCargoGridView* G = Grid(Id); G && G->bCanClose) SetAccess(*G, ESCCargoGridAccess::Closed);
    for (FSCCargoDroneView& D : Drones) if (D.JobId == JobId) { D.State = ESCCargoDroneState::Docked; D.JobId = D.CarryingBoxId = INDEX_NONE; }
    J->View.State = ESCCargoJobState::Cancelled;
    J->View.StateText = LOCTEXT("Cancelled", "Cancelled");
    J->View.EtaSeconds = 0;
    ++Revision;
    return true;
}

bool FPreviewSource::RequestGridAccess(int32 GridId, bool bOpen, FText& OutError)
{
    FSCCargoGridView* G = Grid(GridId);
    if (!G) { OutError = LOCTEXT("NoGrid", "No such grid."); return false; }
    if (G->Kind == ESCCargoGridKind::Environment) { OutError = LOCTEXT("FixedGrid", "This grid does not extend or retract."); return false; }
    if (!bOpen && !G->bCanClose) { OutError = G->CloseBlockedReason; return false; }
    if (G->Access == ESCCargoGridAccess::Opening || G->Access == ESCCargoGridAccess::Closing)
    {
        OutError = LOCTEXT("Busy", "The grid is already moving.");
        return false;
    }
    if ((G->Access == ESCCargoGridAccess::Open) == bOpen) return true;
    for (FSCCargoGridView& O : Grids) // the whole ship moves
        if (O.GroupKey == G->GroupKey && (O.Access == ESCCargoGridAccess::Open) != bOpen)
        {
            SetAccess(O, bOpen ? ESCCargoGridAccess::Opening : ESCCargoGridAccess::Closing);
            Moving.Add(O.GridId, OpenSeconds);
        }
    return true;
}

void FPreviewSource::Advance(float Dt)
{
    for (auto It = Moving.CreateIterator(); It; ++It)
        if ((It.Value() -= Dt) <= 0)
        {
            if (FSCCargoGridView* G = Grid(It.Key()))
                SetAccess(*G, G->Access == ESCCargoGridAccess::Opening ? ESCCargoGridAccess::Open : ESCCargoGridAccess::Closed);
            It.RemoveCurrent();
        }
    for (FJob& J : Jobs)
    {
        if (!Active(J.View.State)) continue;
        J.Phase += Dt;
        FSCCargoJobView& V = J.View;
        if (V.State == ESCCargoJobState::OpeningGrids && J.Phase >= OpenSeconds)
        {
            for (int32 Id : J.ClosedBefore) if (FSCCargoGridView* G = Grid(Id)) SetAccess(*G, ESCCargoGridAccess::Open);
            V.State = ESCCargoJobState::Moving;
            J.Phase = 0;
            ++Revision;
        }
        if (V.State == ESCCargoJobState::Moving && J.Moves.IsValidIndex(V.MovesDone))
        {
            FMove& M = J.Moves[V.MovesDone];
            FSCCargoGridView* From = Grid(M.From);
            FSCCargoGridView* To = Grid(M.To);
            if (From && From->Boxes.RemoveAll([&M](const FSCCargoBoxView& B) { return B.BoxId == M.Box.BoxId; }) > 0) { Recount(*From); ++Revision; }
            const double T = FMath::Clamp(J.Phase / MoveSeconds, 0.0, 1.0);
            M.Box.GridId = INDEX_NONE;
            M.Box.State = T < 0.15 ? ESCCargoBoxState::Lifting : T < 0.85 ? ESCCargoBoxState::InTransit : ESCCargoBoxState::Placing;
            const FVector A = M.Box.WorldCenter, B = To ? CellWorld(*To, FVector(M.Origin) + FVector(M.Cells) / 2) : A;
            const double Travel = FMath::Clamp((T - 0.15) / 0.7, 0.0, 1.0);
            const double Lift = T < 0.15 ? T / 0.15 : T > 0.85 ? (1 - T) / 0.15 : 1;
            const FVector At = FMath::Lerp(From ? CellWorld(*From, FVector(M.Box.Origin) + FVector(M.Box.Cells) / 2) : A, B, Travel) + FVector(0, 0, 220 * Lift);
            M.Box.WorldCenter = At;
            M.Box.WorldBounds = FBox::BuildAABB(At, M.Box.SizeM * 50);
            for (FSCCargoDroneView& D : Drones)
            {
                const int32 Slot = M.Drones.IndexOfByKey(D.DroneId);
                if (Slot == INDEX_NONE) continue;
                D.JobId = V.JobId;
                D.CarryingBoxId = M.Box.BoxId;
                D.State = T < 0.15 ? ESCCargoDroneState::Attaching : T < 0.85 ? ESCCargoDroneState::Carrying : ESCCargoDroneState::Releasing;
                D.WorldLocation = At + FVector(0, (Slot - 0.5 * (M.Drones.Num() - 1)) * 90, M.Box.SizeM.Z * 50 + 120);
                D.WorldBounds = FBox::BuildAABB(D.WorldLocation, FVector(40));
            }
            V.Progress = float((V.MovesDone + T) / FMath::Max(1, V.Moves));
            V.EtaSeconds = float((1 - T) * MoveSeconds + (V.Moves - V.MovesDone - 1) * MoveSeconds + CloseSeconds);
            V.StateText = FText::Format(LOCTEXT("Moving", "Moving box {0} of {1}"), V.MovesDone + 1, V.Moves);
            if (T >= 1 && To)
            {
                FSCCargoBoxView Placed = M.Box;
                Placed.State = ESCCargoBoxState::Stored;
                Placed.JobId = INDEX_NONE;
                Placed.Origin = M.Origin;
                Placed.Cells = M.Cells;
                Place(*To, To->Boxes.Add_GetRef(Placed));
                Recount(*To);
                To->ReservedSCU -= M.Box.SCU;
                V.DoneSCU += M.Box.SCU;
                ++V.MovesDone;
                J.Phase = 0;
                ++Revision;
            }
            if (V.MovesDone >= V.Moves)
            {
                V.State = ESCCargoJobState::ClosingGrids;
                V.StateText = LOCTEXT("Closing", "Closing grids");
                for (int32 Id : J.ClosedBefore) if (FSCCargoGridView* G = Grid(Id); G && G->bCanClose) SetAccess(*G, ESCCargoGridAccess::Closing);
                for (int32 I = 0; I < Drones.Num(); ++I)
                    if (Drones[I].JobId == V.JobId)
                    {
                        Drones[I].State = ESCCargoDroneState::Docked;
                        Drones[I].JobId = Drones[I].CarryingBoxId = INDEX_NONE;
                        Drones[I].WorldLocation = CellWorld(Grids[1], FVector(0.5, 0.3 + 0.4 * I, 0.5));
                        Drones[I].WorldBounds = FBox::BuildAABB(Drones[I].WorldLocation, FVector(40));
                    }
            }
        }
        if (V.State == ESCCargoJobState::ClosingGrids && J.Phase >= CloseSeconds)
        {
            for (int32 Id : J.ClosedBefore) if (FSCCargoGridView* G = Grid(Id); G && G->Access == ESCCargoGridAccess::Closing) SetAccess(*G, ESCCargoGridAccess::Closed);
            V.State = ESCCargoJobState::Done;
            V.StateText = LOCTEXT("Done", "Done");
            V.Progress = 1;
            V.EtaSeconds = 0;
            ++Revision;
        }
    }
    for (FSCCargoGridView& G : Grids) Recount(G); // reservations and loads change what may retract
}
} // namespace

TUniquePtr<ISCCargoViewSource> SCCargoUI::MakePreviewSource(UWorld* World)
{
    // Around the camera at this moment: floor 1.6 m under the eye, yaw only.
    FTransform Frame = FTransform::Identity;
    APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
    if (PC && PC->PlayerCameraManager)
        Frame = FTransform(FRotator(0, PC->PlayerCameraManager->GetCameraRotation().Yaw, 0), PC->PlayerCameraManager->GetCameraLocation() - FVector(0, 0, 160));
    return MakeUnique<FPreviewSource>(Frame);
}

#undef LOCTEXT_NAMESPACE
