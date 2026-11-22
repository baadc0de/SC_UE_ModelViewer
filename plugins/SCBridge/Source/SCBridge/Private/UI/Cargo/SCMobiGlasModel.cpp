// FSCMobiGlasModel: per-frame polling of the cargo views, structure stamps, selection and the transfer form.
#include "SCMobiGlasModel.h"
#include "SCCargoViewSource.h"
#include "SCViewerSubsystem.h"
#include "SCFirstPersonCharacter.h"

#define LOCTEXT_NAMESPACE "SCMobiGlas"

FSCMobiGlasModel::FSCMobiGlasModel(USCViewerSubsystem* InViewer) : Viewer(InViewer) {}

bool FSCMobiGlasModel::IsPreview() const { return Source && Source->IsPreview(); }
bool FSCMobiGlasModel::IsOpen() const { return SCCargoUI::IsMobiGlasOpen(Viewer.Get()); }
void FSCMobiGlasModel::SetOpen(bool bOpen) { SCCargoUI::SetMobiGlasOpen(Viewer.Get(), bOpen); }
bool FSCMobiGlasModel::IsActive(ESCCargoJobState S)
{
    return S != ESCCargoJobState::Done && S != ESCCargoJobState::Cancelled && S != ESCCargoJobState::Failed;
}

void FSCMobiGlasModel::Reset()
{
    Grids.Reset();
    Beacons.Reset();
    Jobs.Reset();
    Drones.Reset();
    MovingBoxes.Reset();
    SeenGridIds.Reset();
    SeenRevision = INDEX_NONE;
    ViewsAge = 1e6f;
    SelectedGrid = INDEX_NONE;
    Layer = 0;
    From = To = FSCGridPick();
    Commodity = NAME_None;
    Amount = 0;
    Commodities.Reset();
    PreviewResult = FSCCargoTransferPreview();
    SubmitError = FText();
    bPreviewDirty = true;
    ++Stamp;
}

void FSCMobiGlasModel::Refresh(float DeltaSeconds)
{
    if (LastFrame == GFrameCounter) return;
    LastFrame = GFrameCounter;
    UWorld* World = SCCargoUI::ViewerWorld();
    ISCCargoViewSource* Now = SCCargoUI::GetSource(World);
    if (Now != Source)
    {
        Source = Now;
        Reset();
    }
    if (!Source) return;
    Source->Advance(DeltaSeconds);
    FVector Eye = FVector::ZeroVector;
    // "Nearby" is around the player, not the camera (the mobiGlas live view orbits away from the player).
    if (const ASCFirstPersonCharacter* C = Viewer.IsValid() ? Viewer->GetFirstPerson() : nullptr) Eye = C->GetActorLocation();
    else SCCargoUI::ViewerLocation(World, Eye);
    // Per frame only what moves on its own: drones and boxes in the air (callouts). The rest is fetched when the
    // source's revision changes, and while the app is open also on a short timer (live distances, extending plates,
    // job progress); closed, the callouts need jobs and grids only at the revision steps (and a slow timer).
    Drones = Source->GetDrones();
    MovingBoxes = Source->GetMovingBoxes();
    const bool bOpen = IsOpen();
    const bool bChanged = Source->GetRevision() != SeenRevision;
    ViewsAge += DeltaSeconds;
    if (bOpen) Jobs = Source->GetJobs();
    if (bChanged || ViewsAge >= (bOpen ? OpenRefreshSeconds : ClosedRefreshSeconds) || bOpen != bWasOpen)
    {
        ViewsAge = 0;
        bWasOpen = bOpen;
        NearbyRadiusM = Source->GetNearbyRadiusM();
        Grids = Source->GetGrids(Eye, NearbyRadiusM);
        Beacons = Source->GetBeacons(Eye, NearbyRadiusM);
        if (!bOpen) Jobs = Source->GetJobs();
        TArray<int32> Ids;
        for (const FSCCargoGridView& G : Grids) Ids.Add(G.GridId);
        for (const FSCCargoBeaconView& B : Beacons) Ids.Add(B.BeaconId); // location ids: never equal to a grid's
        Ids.Sort(); // distance order changes as the player walks; rebuild only when the set changes
        if (bChanged || Ids != SeenGridIds)
        {
            SeenRevision = Source->GetRevision();
            SeenGridIds = Ids;
            ++Stamp;
            FixSelection();
            RefreshCommodities();
            bPreviewDirty = true;
        }
    }
    if (bPreviewDirty) RefreshPreview();
    // Route commands (sc.MobiGlas.*) act like the clicks they stand for.
    FSCMobiGlasRequests& Requests = FSCMobiGlasRequests::Get();
    if (Requests.SelectGrid) if (const FSCGridPick P = PickFor(*Requests.SelectGrid); P.IsSet()) SelectGrid(Resolve(P)[0]);
    if (Requests.From) if (const FSCGridPick P = PickFor(*Requests.From); P.IsSet()) SetFrom(P);
    if (Requests.To) if (const FSCGridPick P = PickFor(*Requests.To); P.IsSet()) SetTo(P);
    if (Requests.Layer) SetLayer(*Requests.Layer);
    if (Requests.Amount) SetAmount(*Requests.Amount);
    if (Requests.Amount) RefreshPreview(); // snap before a Confirm in the same frame
    if (Requests.bConfirm) Confirm();
    if (Requests.CancelJob) CancelJob(*Requests.CancelJob);
    if (Requests.AccessSelectedGroup)
        if (const FSCCargoGridView* G = FindGrid(SelectedGrid)) RequestAccess(Resolve(FSCGridPick{G->GroupKey}), *Requests.AccessSelectedGroup);
    Requests.SelectGrid.Reset();
    Requests.From.Reset();
    Requests.To.Reset();
    Requests.Layer.Reset();
    Requests.Amount.Reset();
    Requests.bConfirm = false;
    Requests.CancelJob.Reset();
    Requests.AccessSelectedGroup.Reset();
}

const FSCCargoGridView* FSCMobiGlasModel::FindGrid(int32 GridId) const
{
    return Grids.FindByPredicate([GridId](const FSCCargoGridView& G) { return G.GridId == GridId; });
}
const FSCCargoBeaconView* FSCMobiGlasModel::FindBeacon(int32 BeaconId) const
{
    return Beacons.FindByPredicate([BeaconId](const FSCCargoBeaconView& B) { return B.BeaconId == BeaconId; });
}
const FSCCargoJobView* FSCMobiGlasModel::FindJob(int32 JobId) const
{
    return Jobs.FindByPredicate([JobId](const FSCCargoJobView& J) { return J.JobId == JobId; });
}

FText FSCMobiGlasModel::ShortGridName(const FSCCargoGridView& G) const
{
    FString Grid = G.GridLabel.ToString(), Head, Tail;
    if (Grid.Split(TEXT(" · "), &Head, &Tail)) Grid = Head;
    // A group of one grid (an elevator platform) is named by the group alone.
    const int32 InGroup = Grids.FilterByPredicate([&G](const FSCCargoGridView& O) { return O.GroupKey == G.GroupKey; }).Num();
    if (InGroup <= 1 || Grid.IsEmpty()) return GroupName(G.GroupKey);
    return FText::Format(LOCTEXT("ShortName", "{0} · {1}"), GroupName(G.GroupKey), FText::FromString(Grid));
}
FText FSCMobiGlasModel::ShortGridName(int32 GridId) const
{
    const FSCCargoGridView* G = FindGrid(GridId);
    if (const FSCCargoBeaconView* B = G ? nullptr : FindBeacon(GridId)) return B->Label; // a location id: a beacon's name
    return G ? ShortGridName(*G) : FText::Format(LOCTEXT("GridN", "Grid {0}"), GridId);
}
FText FSCMobiGlasModel::DescribeGrids(const TArray<int32>& GridIds) const
{
    if (GridIds.Num() == 1) return ShortGridName(GridIds[0]);
    const FSCCargoGridView* First = GridIds.IsEmpty() ? nullptr : FindGrid(GridIds[0]);
    return First ? GroupName(First->GroupKey) : FText();
}
int32 FSCMobiGlasModel::CommodityColour(FName Name)
{
    if (const int32* Found = Colours.Find(Name)) return *Found;
    return Colours.Add(Name, Colours.Num());
}

void FSCMobiGlasModel::SelectGrid(int32 GridId)
{
    if (SelectedGrid != GridId) AccessError = FText();
    if (SelectedGrid != GridId) Layer = 0;
    SelectedGrid = GridId;
}

void FSCMobiGlasModel::FixSelection()
{
    if (!FindGrid(SelectedGrid) && !FindBeacon(SelectedGrid)) SelectGrid(Grids.IsEmpty() ? INDEX_NONE : Grids[0].GridId);
    if (const FSCCargoGridView* G = FindGrid(SelectedGrid)) Layer = FMath::Clamp(Layer, 0, FMath::Max(0, G->Cells.Z - 1));
    const TArray<FSCGridPick> Options = PickOptions(), Sources = PickOptions(true);
    auto Valid = [&Options](const FSCGridPick& P) { return Options.Contains(P); };
    if (From.IsBeacon() && !Sources.Contains(From)) From = FSCGridPick(); // the beacon is gone (its last box picked up)
    if ((!From.IsBeacon() && !Valid(From)) || !Valid(To))
    {
        // Default: from the environment (elevator) to the vehicle as a whole.
        auto Pick = [&](ESCCargoGridKind Kind, const FSCGridPick& Avoid) {
            for (const FSCGridPick& P : Options)
            {
                const TArray<int32> Ids = Resolve(P);
                const FSCCargoGridView* G = Ids.IsEmpty() ? nullptr : FindGrid(Ids[0]);
                if (G && G->Kind == Kind && !(P == Avoid) && (P.IsGroup() || Resolve(FSCGridPick{G->GroupKey}).Num() <= 1)) return P;
            }
            for (const FSCGridPick& P : Options) if (!(P == Avoid)) return P;
            return FSCGridPick();
        };
        if (!From.IsBeacon() && !Valid(From)) From = Pick(ESCCargoGridKind::Environment, To);
        if (!Valid(To)) To = Pick(ESCCargoGridKind::Vehicle, From);
    }
}

TArray<FSCGridPick> FSCMobiGlasModel::PickOptions(bool bAsSource) const
{
    // Groups in order of their nearest grid; a group of several grids is offered whole, then each grid.
    TArray<FSCGridPick> Out;
    TArray<FName> Groups;
    for (const FSCCargoGridView& G : Grids) Groups.AddUnique(G.GroupKey);
    for (FName Group : Groups)
    {
        // Other owners' grids are never a source or a destination.
        const TArray<int32> Ids = Resolve(FSCGridPick{Group}).FilterByPredicate([this](int32 Id) { return FindGrid(Id)->bMine; });
        if (Ids.Num() > 1 && IsGroupMine(Group)) Out.Add(FSCGridPick{Group});
        for (int32 Id : Ids) Out.Add(FSCGridPick{NAME_None, Id});
    }
    // Loose cargo (WP-K2): our beacons, nearest first, as a source only (nothing is set down at a beacon yet).
    if (bAsSource)
        for (const FSCCargoBeaconView& B : Beacons)
            if (B.bMine && B.TotalSCU > 0) Out.Add(FSCGridPick{NAME_None, INDEX_NONE, B.BeaconId});
    return Out;
}
FText FSCMobiGlasModel::PickLabel(const FSCGridPick& P) const
{
    if (!P.IsSet()) return LOCTEXT("PickNone", "Choose…");
    if (P.IsBeacon())
    {
        const FSCCargoBeaconView* B = FindBeacon(P.BeaconId);
        return B ? FText::Format(LOCTEXT("PickBeacon", "{0} · {1} {1}|plural(one=box,other=boxes)"), B->Label, B->Boxes.Num()) : LOCTEXT("PickBeaconGone", "Loose cargo (gone)");
    }
    if (P.IsGroup())
    {
        const TArray<int32> Ids = Resolve(P);
        const FSCCargoGridView* G = Ids.IsEmpty() ? nullptr : FindGrid(Ids[0]);
        return FText::Format(LOCTEXT("PickGroup", "{0} · all {1} grids"), G ? GroupName(G->GroupKey) : FText::FromName(P.Group), Ids.Num());
    }
    return ShortGridName(P.GridId);
}
TArray<int32> FSCMobiGlasModel::Resolve(const FSCGridPick& P) const
{
    TArray<int32> Ids;
    if (P.IsBeacon())
    {
        if (FindBeacon(P.BeaconId)) Ids.Add(P.BeaconId); // a location id the planner takes as a source
    }
    else if (P.IsGroup())
    {
        for (const FSCCargoGridView& G : Grids) if (G.GroupKey == P.Group) Ids.Add(G.GridId);
    }
    else if (P.GridId != INDEX_NONE && FindGrid(P.GridId))
        Ids.Add(P.GridId);
    return Ids;
}

void FSCMobiGlasModel::SetFrom(const FSCGridPick& P)
{
    From = P;
    RefreshCommodities();
    bPreviewDirty = true;
    SubmitError = FText();
}
void FSCMobiGlasModel::SetTo(const FSCGridPick& P)
{
    if (P.IsBeacon()) return; // loose cargo is a source only
    To = P;
    bPreviewDirty = true;
    SubmitError = FText();
}
void FSCMobiGlasModel::SetCommodity(FName InCommodity)
{
    Commodity = InCommodity;
    Amount = 0; // snaps to the largest achievable amount on the next preview
    bPreviewDirty = true;
    SubmitError = FText();
}
void FSCMobiGlasModel::SetAmount(int32 SCU)
{
    if (SCU == Amount) return;
    Amount = SCU;
    bPreviewDirty = true;
    SubmitError = FText();
}

void FSCMobiGlasModel::RefreshCommodities()
{
    // What we can move from the source: our own stored boxes (other owners' boxes on a shared grid, the freight
    // elevator, are not offered), aggregated from the grid views so it agrees with what the panels show.
    Commodities.Reset();
    for (int32 Id : Resolve(From))
    {
        const FSCCargoGridView* G = FindGrid(Id);
        const FSCCargoBeaconView* Beacon = G ? nullptr : FindBeacon(Id);
        const TArray<FSCCargoBoxView>* Boxes = G ? &G->Boxes : Beacon ? &Beacon->Boxes : nullptr;
        if (Boxes)
            for (const FSCCargoBoxView& B : *Boxes)
            {
                if (!B.bMine || (B.State != ESCCargoBoxState::Stored && B.State != ESCCargoBoxState::Loose)) continue;
                FSCCargoCommodityView* C = Commodities.FindByPredicate([&B](const FSCCargoCommodityView& V) { return V.Commodity == B.Commodity; });
                if (!C)
                {
                    C = &Commodities.AddDefaulted_GetRef();
                    C->Commodity = B.Commodity;
                    C->Label = B.CommodityLabel;
                }
                C->TotalSCU += B.SCU;
                C->BoxSizesSCU.AddUnique(B.SCU);
            }
    }
    for (FSCCargoCommodityView& C : Commodities) C.BoxSizesSCU.Sort();
    Commodities.Sort([](const FSCCargoCommodityView& A, const FSCCargoCommodityView& B) { return A.TotalSCU > B.TotalSCU; });
    for (const FSCCargoCommodityView& C : Commodities) CommodityColour(C.Commodity);
    if (!Commodities.ContainsByPredicate([this](const FSCCargoCommodityView& C) { return C.Commodity == Commodity; }))
    {
        Commodity = Commodities.IsEmpty() ? NAME_None : Commodities[0].Commodity;
        Amount = 0;
    }
}

void FSCMobiGlasModel::RefreshPreview()
{
    bPreviewDirty = false;
    ++PreviewStamp;
    PreviewResult = FSCCargoTransferPreview();
    if (!Source || !From.IsSet() || !To.IsSet() || Commodity.IsNone()) return;
    PreviewResult = Source->Preview(Request());
    const TArray<int32> Stops = PreviewResult.AchievableSCU;
    if (Stops.IsEmpty() || Stops.Contains(Amount)) return;
    // Snap to the nearest achievable amount (the largest one when nothing was chosen yet) and plan that.
    int32 Best = Stops.Last();
    if (Amount > 0)
        for (int32 S : Stops) if (FMath::Abs(S - Amount) < FMath::Abs(Best - Amount)) Best = S;
    Amount = Best;
    PreviewResult = Source->Preview(Request());
}

FSCCargoTransferRequest FSCMobiGlasModel::Request() const
{
    FSCCargoTransferRequest R;
    R.FromGrids = Resolve(From);
    R.ToGrids = Resolve(To);
    R.Commodity = Commodity;
    R.AmountSCU = Amount;
    return R;
}

bool FSCMobiGlasModel::Confirm()
{
    SubmitError = FText();
    if (!Source) return false;
    const int32 Job = Source->Submit(Request(), SubmitError);
    if (Job == INDEX_NONE && SubmitError.IsEmpty()) SubmitError = LOCTEXT("SubmitFailed", "The transfer could not be started.");
    bPreviewDirty = true;
    UE_LOG(LogTemp, Display, TEXT("MOBIGLAS_SUBMIT job=%d amount=%d error=\"%s\""), Job, Amount, *SubmitError.ToString());
    return Job != INDEX_NONE;
}

bool FSCMobiGlasModel::CancelJob(int32 JobId)
{
    return Source && Source->Cancel(JobId);
}

FSCMobiGlasModel::FAccessControl FSCMobiGlasModel::GetAccessControl(const TArray<int32>& GridIds) const
{
    FAccessControl C;
    TArray<const FSCCargoGridView*> Views;
    for (int32 Id : GridIds)
        if (const FSCCargoGridView* G = FindGrid(Id)) Views.Add(G);
    if (Views.IsEmpty()) return C;
    for (const FSCCargoGridView* G : Views)
    {
        // Fixed-open grids (no open / close) and mixed states get no control.
        if (G->Access == ESCCargoGridAccess::Open && !G->bCanClose && G->CloseBlockedReason.IsEmpty()) return C;
        if (G->Access != Views[0]->Access) return C;
    }
    C.bShown = true;
    const ESCCargoGridAccess A = Views[0]->Access;
    C.bExtend = A == ESCCargoGridAccess::Closed || A == ESCCargoGridAccess::Opening;
    if (A == ESCCargoGridAccess::Opening || A == ESCCargoGridAccess::Closing)
    {
        C.Reason = A == ESCCargoGridAccess::Opening ? LOCTEXT("ExtendingNow", "Extending…") : LOCTEXT("RetractingNow", "Retracting…");
        return C;
    }
    C.bEnabled = true;
    if (!C.bExtend)
        for (const FSCCargoGridView* G : Views)
            if (!G->bCanClose)
            {
                C.bEnabled = false;
                C.Reason = G->CloseBlockedReason;
                break;
            }
    return C;
}

bool FSCMobiGlasModel::RequestAccess(const TArray<int32>& GridIds, bool bOpen)
{
    AccessError = FText();
    if (!Source) return false;
    bool bAll = true;
    for (int32 Id : GridIds)
    {
        FText Error;
        if (!Source->RequestGridAccess(Id, bOpen, Error))
        {
            bAll = false;
            if (AccessError.IsEmpty()) AccessError = Error;
        }
    }
    UE_LOG(LogTemp, Display, TEXT("MOBIGLAS_ACCESS open=%d grids=%d ok=%d error=\"%s\""), bOpen, GridIds.Num(), bAll, *AccessError.ToString());
    return bAll;
}

TArray<int32> FSCMobiGlasModel::NeedExtending(const TArray<int32>& GridIds) const
{
    TArray<int32> Out;
    for (int32 Id : GridIds)
        if (const FSCCargoGridView* G = FindGrid(Id); G && G->Access != ESCCargoGridAccess::Open) Out.Add(Id);
    return Out;
}

#undef LOCTEXT_NAMESPACE
FSCGridPick FSCMobiGlasModel::PickFor(const FString& Selector) const
{
    const FString S = Selector.TrimStartAndEnd();
    // Beacons (loose cargo, WP-K2): "beacon" / "loose" is the nearest of ours, else by id or text in a beacon's name.
    const bool bBeacon = S.Equals(TEXT("beacon"), ESearchCase::IgnoreCase) || S.Equals(TEXT("loose"), ESearchCase::IgnoreCase);
    for (const FSCCargoBeaconView& B : Beacons) // nearest first
        if (bBeacon ? B.bMine : S.IsNumeric() ? FCString::Atoi(*S) == B.BeaconId : B.Label.ToString().Contains(S))
            return FSCGridPick{NAME_None, INDEX_NONE, B.BeaconId};
    if (bBeacon) return FSCGridPick();
    if (S.IsNumeric()) return FindGrid(FCString::Atoi(*S)) ? FSCGridPick{NAME_None, FCString::Atoi(*S)} : FSCGridPick();
    const bool bShip = S.Equals(TEXT("ship"), ESearchCase::IgnoreCase) || S.Equals(TEXT("vehicle"), ESearchCase::IgnoreCase);
    const bool bEnv = S.Equals(TEXT("elevator"), ESearchCase::IgnoreCase) || S.Equals(TEXT("environment"), ESearchCase::IgnoreCase);
    TArray<int32> Ids;
    for (const FSCCargoGridView& G : Grids) // nearest first
    {
        const bool bMatch = bShip ? G.Kind == ESCCargoGridKind::Vehicle
                          : bEnv  ? G.Kind == ESCCargoGridKind::Environment
                                  : G.GridLabel.ToString().Contains(S) || G.GroupLabel.ToString().Contains(S);
        if (bMatch && (Ids.IsEmpty() || (bShip || bEnv ? FindGrid(Ids[0])->GroupKey == G.GroupKey : true))) Ids.Add(G.GridId);
    }
    if (Ids.IsEmpty()) return FSCGridPick();
    const FName Group = FindGrid(Ids[0])->GroupKey;
    const TArray<int32> Whole = Resolve(FSCGridPick{Group});
    if (Ids.Num() > 1 && Whole.Num() > 1 && Ids.Num() == Whole.Num()) return FSCGridPick{Group};
    return FSCGridPick{NAME_None, Ids[0]};
}

FText FSCMobiGlasModel::GroupName(FName GroupKey) const
{
    // Ships of one type in one hangar share a label: number them in a stable order ("MISC Hull A · 2").
    const FSCCargoGridView* First = Grids.FindByPredicate([GroupKey](const FSCCargoGridView& G) { return G.GroupKey == GroupKey; });
    if (!First) return FText::FromName(GroupKey);
    TArray<FName> Same;
    for (const FSCCargoGridView& G : Grids)
        if (G.GroupLabel.EqualTo(First->GroupLabel)) Same.AddUnique(G.GroupKey);
    if (Same.Num() <= 1) return HumanGroup(First->GroupLabel);
    Same.Sort(FNameLexicalLess());
    return FText::Format(NSLOCTEXT("SCMobiGlas", "GroupN", "{0} · {1}"), HumanGroup(First->GroupLabel), Same.IndexOfByKey(GroupKey) + 1);
}

bool FSCMobiGlasModel::IsGroupMine(FName GroupKey) const
{
    return !Grids.ContainsByPredicate([GroupKey](const FSCCargoGridView& G) { return G.GroupKey == GroupKey && !G.bMine; });
}

namespace
{
/** Generic word expansions for abbreviated position tokens in port names (no asset names). */
FString ExpandWord(const FString& W)
{
    static const TMap<FString, FString> Words = {{TEXT("bot"), TEXT("bottom")}, {TEXT("btm"), TEXT("bottom")}, {TEXT("l"), TEXT("left")},
                                                 {TEXT("r"), TEXT("right")},    {TEXT("lft"), TEXT("left")},    {TEXT("rgt"), TEXT("right")},
                                                 {TEXT("fwd"), TEXT("front")},  {TEXT("ctr"), TEXT("centre")}, {TEXT("mid"), TEXT("middle")},
                                                 {TEXT("t"), TEXT("top")},      {TEXT("b"), TEXT("bottom")}};
    const FString Lower = W.ToLower();
    const FString* Found = Words.Find(Lower);
    return Found ? *Found : Lower;
}
TArray<FString> Words(const FString& S)
{
    TArray<FString> Out;
    S.Replace(TEXT("_"), TEXT(" ")).ParseIntoArrayWS(Out);
    return Out;
}
} // namespace

FText FSCMobiGlasModel::GridName(const FSCCargoGridView& G) const
{
    // "Grid 2 · hardpoint cargoplate bot left" -> "Grid 2 · bottom left": the words every sibling grid shares are dropped,
    // abbreviations expanded. The view's own label stays available (tooltip).
    FString Head, Tail;
    if (!G.GridLabel.ToString().Split(TEXT(" · "), &Head, &Tail)) return G.GridLabel;
    // Words in every sibling's label (the port-path prefix) say nothing about this grid.
    TArray<FString> Mine = Words(Tail), Common = Mine;
    bool bSiblings = false;
    for (const FSCCargoGridView& O : Grids)
    {
        FString OH, OT;
        if (O.GroupKey != G.GroupKey || O.GridId == G.GridId || !O.GridLabel.ToString().Split(TEXT(" · "), &OH, &OT)) continue;
        bSiblings = true;
        const TArray<FString> Theirs = Words(OT);
        Common.RemoveAll([&Theirs](const FString& W) { return !Theirs.ContainsByPredicate([&W](const FString& T) { return T.Equals(W, ESearchCase::IgnoreCase); }); });
    }
    if (bSiblings) Mine.RemoveAll([&Common](const FString& W) { return Common.Contains(W); });
    if (Mine.IsEmpty()) return FText::FromString(Head);
    FString Position;
    for (const FString& W : Mine) Position += (Position.IsEmpty() ? TEXT("") : TEXT(" ")) + ExpandWord(W);
    return FText::FromString(Head + TEXT(" · ") + Position);
}

FText FSCMobiGlasModel::HumanGroup(const FText& Label)
{
    // "Hangar Freight Elevator Cargo Grid Large" -> "Hangar Freight Elevator · Large": every entry of this app is a cargo
    // grid, so those words say nothing; a trailing size word becomes a qualifier. Other labels pass unchanged.
    TArray<FString> W = Words(Label.ToString());
    const int32 Before = W.Num();
    W.RemoveAll([](const FString& X) { return X.Equals(TEXT("cargo"), ESearchCase::IgnoreCase) || X.Equals(TEXT("grid"), ESearchCase::IgnoreCase); });
    if (W.IsEmpty() || W.Num() == Before) return Label;
    static const TCHAR* Sizes[] = {TEXT("tiny"), TEXT("small"), TEXT("medium"), TEXT("large"), TEXT("xl"), TEXT("xxl")};
    FString Size;
    for (const TCHAR* S : Sizes)
        if (W.Num() > 1 && W.Last().Equals(S, ESearchCase::IgnoreCase)) Size = W.Pop();
    FString Out = FString::Join(W, TEXT(" "));
    if (!Size.IsEmpty()) Out += TEXT(" · ") + Size;
    return FText::FromString(Out);
}
