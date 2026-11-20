// Live cargo spawning, pure part (SCCargoSpawn.h): box records from DataCore JSON, the catalogue by commodity, the
// spawn plan on grids (FSCCargoPlanner's packing) with its refusals, and what a clear may take.
#include "Cargo/SCCargoSpawn.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "SCCargoSpawn"

namespace
{
using FObj = TSharedPtr<FJsonObject>;
FString TypeOf(const FObj& O)
{
    FString T;
    if (O && !O->TryGetStringField(TEXT("__type"), T)) O->TryGetStringField(TEXT("_Type_"), T);
    return T;
}
FObj Child(const FObj& O, const TCHAR* Key)
{
    const FObj* C;
    return O && O->TryGetObjectField(Key, C) ? *C : nullptr;
}
double Num(const FObj& O, const TCHAR* Key, double Default = 0)
{
    double V = Default;
    if (O) O->TryGetNumberField(Key, V);
    return V;
}
/** Depth-first: the first object of Type under V. */
FObj FindTyped(const TSharedPtr<FJsonValue>& V, const FString& Type)
{
    if (!V) return nullptr;
    if (V->Type == EJson::Array)
    {
        for (const TSharedPtr<FJsonValue>& E : V->AsArray())
            if (FObj F = FindTyped(E, Type)) return F;
        return nullptr;
    }
    if (V->Type != EJson::Object) return nullptr;
    const FObj O = V->AsObject();
    if (TypeOf(O) == Type) return O;
    for (const auto& Pair : O->Values)
        if (FObj F = FindTyped(Pair.Value, Type)) return F;
    return nullptr;
}
/** A record reference: a GUID string (the FFI's shape) or a reference object (the CLI's): its name when it carries
 *  one, else its GUID. */
FString Reference(const TSharedPtr<FJsonValue>& V)
{
    if (!V) return FString();
    if (V->Type == EJson::String) return V->AsString();
    if (V->Type != EJson::Object) return FString();
    const FObj O = V->AsObject();
    FString S;
    for (const TCHAR* Key : {TEXT("__record_name"), TEXT("_RecordName_"), TEXT("__record_id"), TEXT("_RecordId_")})
        if (O->TryGetStringField(Key, S) && !S.IsEmpty()) return S;
    return FString();
}
FString SizesText(const TArray<const FSCCargoBoxRecord*>& Boxes)
{
    TArray<int32> S;
    for (const FSCCargoBoxRecord* B : Boxes) S.AddUnique(B->SCU);
    S.Sort();
    return FString::JoinBy(S, TEXT(", "), [](int32 V) { return FString::FromInt(V); });
}
FString WhereText(const TArray<FSCCargoGridModel>& Grids)
{
    if (Grids.Num() != 1) return FString::Printf(TEXT("%s (%d grids)"), Grids.IsEmpty() ? TEXT("the grids") : *Grids[0].GroupLabel, Grids.Num());
    const FSCCargoGridModel& G = Grids[0];
    return G.Label.IsEmpty() || G.GroupLabel.IsEmpty() ? (G.Label.IsEmpty() ? G.GroupLabel : G.Label) : G.GroupLabel + TEXT(" · ") + G.Label;
}
FSCCargoSpawnPreview Refuse(FSCCargoSpawnPreview P, const TCHAR* Key, const FString& Why)
{
    P.bValid = false;
    P.Refused = Key;
    P.Reason = FText::FromString(Why);
    P.Boxes.Reset();
    P.TotalSCU = 0;
    return P;
}
} // namespace

// ---- records --------------------------------------------------------------------------------------------------------

bool SCCargoSpawn::ParseBoxRecord(const FString& Json, FSCCargoBoxRecord& Out, FString& OutCommodityRef, bool& bOutGroup, FString& Why)
{
    TSharedPtr<FJsonValue> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root) { Why = TEXT("invalid record JSON"); return false; }
    const FObj Attach = FindTyped(Root, TEXT("SAttachableComponentParams"));
    const FObj Def = Child(Attach, TEXT("AttachDef"));
    if (!Def) { Why = TEXT("no SAttachableComponentParams.AttachDef"); return false; }
    // Whole standard cargo units only: the eighth-SCU hand boxes carry their volume in another unit.
    const FObj Volume = Child(Def, TEXT("inventoryOccupancyVolume"));
    double Scu = 0;
    if (!Volume || !Volume->TryGetNumberField(TEXT("standardCargoUnits"), Scu) || Scu < 1 - 1e-3 || FMath::Abs(Scu - FMath::RoundToDouble(Scu)) > 1e-3)
    {
        Why = TEXT("its occupancy is not a whole number of standard cargo units");
        return false;
    }
    const FObj Dims = Child(Def, TEXT("inventoryOccupancyDimensions"));
    const FVector D(Num(Dims, TEXT("x")), Num(Dims, TEXT("y")), Num(Dims, TEXT("z")));
    if (D.GetMin() <= 0) { Why = TEXT("no inventoryOccupancyDimensions"); return false; }
    // The resource it holds: the default composition's first entry, else the first group it takes.
    const FObj Container = FindTyped(Root, TEXT("ResourceContainer"));
    if (!Container) { Why = TEXT("no ResourceContainer: it holds no commodity"); return false; }
    FString Ref;
    bOutGroup = false;
    const TArray<TSharedPtr<FJsonValue>>* List;
    if (Container->TryGetArrayField(TEXT("defaultComposition"), List))
        for (const TSharedPtr<FJsonValue>& E : *List)
            if (E && E->Type == EJson::Object && (Ref = Reference(E->AsObject()->TryGetField(TEXT("entry")))).Len()) break;
    if (Ref.IsEmpty() && Container->TryGetArrayField(TEXT("inclusiveGroups"), List))
        for (const TSharedPtr<FJsonValue>& E : *List)
            if ((Ref = Reference(E)).Len()) { bOutGroup = true; break; }
    if (Ref.IsEmpty()) { Why = TEXT("its ResourceContainer names no resource or group"); return false; }
    Out.SCU = int32(FMath::RoundToDouble(Scu));
    Out.DimensionsSC = D;
    Def->TryGetStringField(TEXT("SubType"), Out.SubType);
    OutCommodityRef = Ref;
    return true;
}

FString SCCargoSpawn::RecordShortName(const FString& Json)
{
    TSharedPtr<FJsonObject> Root;
    FString Name;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root) return Name;
    if (!Root->TryGetStringField(TEXT("__record_name"), Name)) Root->TryGetStringField(TEXT("_RecordName_"), Name);
    FString Type, Short;
    return Name.Split(TEXT("."), &Type, &Short) ? Short : Name;
}

// ---- catalogue ------------------------------------------------------------------------------------------------------

void FSCCargoBoxCatalog::Add(const FSCCargoBoxRecord& Record) { Records.Add(Record); }

void FSCCargoBoxCatalog::Finish()
{
    // A commodity's label: the display name most of its boxes share, else its key. A name that boxes of several
    // commodities carry names none of them (a localisation placeholder: the game's own text for "no name yet").
    TMap<FString, TSet<FName>> Owners;
    for (const FSCCargoBoxRecord& R : Records)
        if (!R.DisplayName.IsEmpty()) Owners.FindOrAdd(R.DisplayName).Add(R.Commodity);
    TMap<FName, TMap<FString, int32>> Names;
    for (const FSCCargoBoxRecord& R : Records)
        if (!R.DisplayName.IsEmpty() && Owners[R.DisplayName].Num() == 1) ++Names.FindOrAdd(R.Commodity).FindOrAdd(R.DisplayName);
    TMap<FName, FString> Labels;
    for (const FSCCargoBoxRecord& R : Records)
    {
        if (Labels.Contains(R.Commodity)) continue;
        FString Best = R.Commodity.ToString();
        int32 Most = 0;
        if (const TMap<FString, int32>* N = Names.Find(R.Commodity))
            for (const auto& [Name, Count] : *N)
                if (Count > Most || (Count == Most && Name < Best)) { Best = Name; Most = Count; }
        Labels.Add(R.Commodity, Best);
    }
    // Two commodities under one name (a localisation shared by a raw and a refined resource) are told apart by key.
    TMap<FString, int32> Uses;
    for (const auto& [Key, Label] : Labels) ++Uses.FindOrAdd(Label);
    for (auto& [Key, Label] : Labels)
        if (Uses[Label] > 1) Label = FString::Printf(TEXT("%s · %s"), *Label, *Key.ToString());
    for (FSCCargoBoxRecord& R : Records) R.CommodityLabel = FText::FromString(Labels[R.Commodity]);
    Records.Sort([](const FSCCargoBoxRecord& A, const FSCCargoBoxRecord& B) {
        return A.Commodity != B.Commodity ? A.Commodity.LexicalLess(B.Commodity) : A.SCU != B.SCU ? A.SCU > B.SCU : A.Entity < B.Entity;
    });
}

const FSCCargoBoxRecord* FSCCargoBoxCatalog::FindEntity(const FString& Entity) const
{
    return Records.FindByPredicate([&](const FSCCargoBoxRecord& R) { return R.Entity.Equals(Entity, ESearchCase::IgnoreCase); });
}

FName FSCCargoBoxCatalog::FindCommodity(const FString& Name, FString& Why) const
{
    const FString Wanted = Name.TrimStartAndEnd();
    if (Wanted.IsEmpty()) { Why = TEXT("no commodity named"); return NAME_None; }
    for (const FSCCargoBoxRecord& R : Records)
        if (R.Commodity.ToString().Equals(Wanted, ESearchCase::IgnoreCase)) return R.Commodity;
    for (const FSCCargoBoxRecord& R : Records)
        if (R.CommodityLabel.ToString().Equals(Wanted, ESearchCase::IgnoreCase)) return R.Commodity;
    TArray<FName> Hits;
    for (const FSCCargoBoxRecord& R : Records)
        if (R.Commodity.ToString().Contains(Wanted) || R.CommodityLabel.ToString().Contains(Wanted)) Hits.AddUnique(R.Commodity);
    if (Hits.Num() == 1) return Hits[0];
    if (Hits.IsEmpty())
        Why = FString::Printf(TEXT("no commodity or box entity \"%s\" in the game's box records (%d records)"), *Wanted, Records.Num());
    else
    {
        Hits.Sort([](FName A, FName B) { return A.LexicalLess(B); });
        TArray<FString> Shown;
        for (int32 I = 0; I < FMath::Min(Hits.Num(), 6); ++I) Shown.Add(LabelOf(Hits[I]).ToString());
        Why = FString::Printf(TEXT("\"%s\" matches %d commodities: %s%s"), *Wanted, Hits.Num(), *FString::Join(Shown, TEXT(", ")), Hits.Num() > 6 ? TEXT(", ...") : TEXT(""));
    }
    return NAME_None;
}

TArray<const FSCCargoBoxRecord*> FSCCargoBoxCatalog::BoxesOf(FName Commodity) const
{
    // Every record (variants of one size may differ in dimensions or sub type: the plan takes the first that fits).
    TArray<const FSCCargoBoxRecord*> Out;
    for (const FSCCargoBoxRecord& R : Records) // sorted by SCU, largest first, then entity
        if (R.Commodity == Commodity) Out.Add(&R);
    return Out;
}

FText FSCCargoBoxCatalog::LabelOf(FName Commodity) const
{
    const FSCCargoBoxRecord* R = Records.FindByPredicate([&](const FSCCargoBoxRecord& X) { return X.Commodity == Commodity; });
    return R ? R->CommodityLabel : FText::FromName(Commodity);
}

TArray<FSCCargoSpawnCommodity> FSCCargoBoxCatalog::Commodities() const
{
    TArray<FSCCargoSpawnCommodity> Out;
    for (const FSCCargoBoxRecord& R : Records)
    {
        FSCCargoSpawnCommodity* C = Out.FindByPredicate([&](const FSCCargoSpawnCommodity& X) { return X.Commodity == R.Commodity; });
        if (!C)
        {
            C = &Out.AddDefaulted_GetRef();
            C->Commodity = R.Commodity;
            C->Label = R.CommodityLabel;
        }
        if (!C->SizesSCU.Contains(R.SCU))
        {
            C->SizesSCU.Add(R.SCU);
            C->SizesM.Add(R.DimensionsSC);
        }
    }
    for (FSCCargoSpawnCommodity& C : Out)
    {
        // Ascending sizes, dimensions alongside.
        TArray<int32> Order;
        for (int32 I = 0; I < C.SizesSCU.Num(); ++I) Order.Add(I);
        Order.Sort([&](int32 A, int32 B) { return C.SizesSCU[A] < C.SizesSCU[B]; });
        TArray<int32> S;
        TArray<FVector> M;
        for (const int32 I : Order) { S.Add(C.SizesSCU[I]); M.Add(C.SizesM[I]); }
        C.SizesSCU = S;
        C.SizesM = M;
    }
    Out.Sort([](const FSCCargoSpawnCommodity& A, const FSCCargoSpawnCommodity& B) { return A.Label.ToString() < B.Label.ToString(); });
    return Out;
}

// ---- plan -----------------------------------------------------------------------------------------------------------


namespace
{
/** Packs Items onto Grids as FSCCargoPlanner::Pack does (largest first, each the best free, supported slot over the
 *  grids in order), with the items in NoSupport bearing nothing. Out: per item its slot and grid index. */
bool PackItems(TArray<FSCCargoGridModel>& Grids, const TArray<FSCCargoItem>& Items, const TSet<int32>& NoSupport, TArray<FSCCargoPlacement>& OutSlots,
               TArray<int32>& OutGrid, int32* OutPlaced = nullptr)
{
    OutSlots.SetNum(Items.Num());
    OutGrid.Init(INDEX_NONE, Items.Num());
    TArray<int32> Order;
    for (int32 I = 0; I < Items.Num(); ++I) Order.Add(I);
    Order.StableSort([&](int32 A, int32 B) {
        const FVector &SA = Items[A].SizeM, &SB = Items[B].SizeM;
        return SA.X * SA.Y * SA.Z > SB.X * SB.Y * SB.Z + 1e-9;
    });
    int32 Placed = 0;
    for (const int32 I : Order)
    {
        bool bPlaced = false;
        for (int32 G = 0; G < Grids.Num() && !bPlaced; ++G)
            if (FSCCargoPlanner::FindSlot(Grids[G], Items[I], OutSlots[I], NoSupport.IsEmpty() ? nullptr : &NoSupport, nullptr))
            {
                Grids[G].Placed.Add(OutSlots[I]);
                OutGrid[I] = G;
                bPlaced = true;
            }
        if (!bPlaced)
        {
            if (OutPlaced) *OutPlaced = Placed;
            return false;
        }
        ++Placed;
    }
    if (OutPlaced) *OutPlaced = Placed;
    return true;
}
} // namespace

FSCCargoSpawnPreview SCCargoSpawn::Plan(const TArray<FSCCargoGridModel>& Grids, const TArray<const FSCCargoBoxRecord*>& InBoxes, const FSCCargoSpawnRequest& Request,
                                        int32 FirstItemId, const TSet<int32>& NoSupport)
{
    FSCCargoSpawnPreview P;
    if (InBoxes.IsEmpty()) return Refuse(P, TEXT("commodity"), TEXT("no box of that commodity in the game data"));
    if (Grids.IsEmpty()) return Refuse(P, TEXT("grid"), TEXT("no grid to put the boxes on"));
    TArray<const FSCCargoBoxRecord*> Boxes = InBoxes;
    Boxes.StableSort([](const FSCCargoBoxRecord& A, const FSCCargoBoxRecord& B) { return A.SCU > B.SCU; });
    P.Commodity = Boxes[0]->Commodity;
    P.CommodityLabel = Boxes[0]->CommodityLabel;
    const FString Label = P.CommodityLabel.ToString(), Where = WhereText(Grids);
    auto Item = [](const FSCCargoBoxRecord* B, int32 Id) {
        FSCCargoItem I;
        I.Id = Id;
        I.SizeM = B->SizeM();
        I.SCU = B->SCU;
        I.SubType = B->SubType;
        I.Commodity = B->Commodity;
        return I;
    };
    // A box fits when some grid takes it: within its max item and interior, upright, and not an excluded sub type.
    auto Fits = [&](const FSCCargoBoxRecord* B, FString* Why) {
        for (const FSCCargoGridModel& G : Grids)
        {
            bool bTurn = false;
            FString W;
            const bool bExcluded = !B->SubType.IsEmpty() &&
                G.Lattice.ExcludedSubTypes.ContainsByPredicate([&](const FString& S) { return S.Equals(B->SubType, ESearchCase::IgnoreCase); });
            if (bExcluded) W = FString::Printf(TEXT("%s grids do not take %s items"), *G.GroupLabel, *B->SubType);
            else if (FSCCargoPlanner::FitsMaxItem(G.Lattice, B->SizeM(), bTurn, &W, G.GroupLabel, B->SCU)) return true;
            if (Why && Why->IsEmpty()) *Why = W;
        }
        return false;
    };
    // One box per size: the first variant of it that fits (largest size first).
    TArray<const FSCCargoBoxRecord*> Candidates;
    FString FirstWhy;
    for (const FSCCargoBoxRecord* B : Boxes)
    {
        if (Request.SizeSCU > 0 && B->SCU != Request.SizeSCU) continue;
        if (Candidates.ContainsByPredicate([&](const FSCCargoBoxRecord* C) { return C->SCU == B->SCU; })) continue;
        FString Why;
        if (Fits(B, &Why)) Candidates.Add(B);
        else if (FirstWhy.IsEmpty() || Request.SizeSCU <= 0) FirstWhy = Why; // the smallest size's reason, or the requested one's
    }
    if (Request.SizeSCU > 0 && !Boxes.ContainsByPredicate([&](const FSCCargoBoxRecord* B) { return B->SCU == Request.SizeSCU; }))
        return Refuse(P, TEXT("size"), FString::Printf(TEXT("no %d SCU box of %s in the game data (sizes %s SCU)"), Request.SizeSCU, *Label, *SizesText(Boxes)));
    if (Candidates.IsEmpty()) return Refuse(P, TEXT("size"), FirstWhy);
    // The box sets that make the request, preferred first. By amount: whole boxes, depth-first over the counts of each
    // size from the largest down (largest first, then an exact fit with fewer large boxes; bounded). By count: the count
    // of each size, largest first.
    TArray<TArray<const FSCCargoBoxRecord*>> Sets;
    constexpr int32 MaxSets = 48, MaxSteps = 20000;
    bool bTooMany = false;
    if (Request.AmountSCU > 0)
    {
        if (Request.SizeSCU > 0 && Request.AmountSCU % Request.SizeSCU != 0)
            return Refuse(P, TEXT("amount"), FString::Printf(TEXT("%d SCU is not a whole number of %d SCU boxes"), Request.AmountSCU, Request.SizeSCU));
        TArray<int32> Counts;
        Counts.SetNumZeroed(Candidates.Num());
        int32 Steps = 0;
        TFunction<void(int32, int32, int32)> Walk = [&](int32 I, int32 Left, int32 Used) {
            if (Sets.Num() >= MaxSets || ++Steps > MaxSteps) return;
            if (Left == 0)
            {
                TArray<const FSCCargoBoxRecord*>& Set = Sets.AddDefaulted_GetRef();
                for (int32 K = 0; K < Candidates.Num(); ++K)
                    for (int32 N = 0; N < Counts[K]; ++N) Set.Add(Candidates[K]);
                return;
            }
            if (I >= Candidates.Num()) return;
            for (int32 N = Left / Candidates[I]->SCU; N >= 0; --N)
            {
                if (Used + N > MaxBoxes) { bTooMany = true; continue; }
                Counts[I] = N;
                Walk(I + 1, Left - N * Candidates[I]->SCU, Used + N);
                Counts[I] = 0;
                if (Sets.Num() >= MaxSets || Steps > MaxSteps) return;
            }
        };
        Walk(0, Request.AmountSCU, 0);
        if (Sets.IsEmpty() && bTooMany)
            return Refuse(P, TEXT("amount"), FString::Printf(TEXT("%d SCU takes more than %d boxes: one spawn puts down at most %d"), Request.AmountSCU, MaxBoxes, MaxBoxes));
        if (Sets.IsEmpty())
            return Refuse(P, TEXT("amount"), FString::Printf(TEXT("%d SCU cannot be made of whole %s boxes that fit %s (sizes %s SCU)"), Request.AmountSCU, *Label,
                                                             *Where, *SizesText(Candidates)));
    }
    else
    {
        const int32 Count = FMath::Max(1, Request.Count);
        if (Count > MaxBoxes)
            return Refuse(P, TEXT("amount"), FString::Printf(TEXT("%d boxes: one spawn puts down at most %d"), Count, MaxBoxes));
        for (const FSCCargoBoxRecord* B : Candidates)
        {
            TArray<const FSCCargoBoxRecord*> Set;
            Set.Init(B, Count);
            Sets.Add(MoveTemp(Set));
        }
    }
    // The first set that packs onto the grids as they are.
    for (const TArray<const FSCCargoBoxRecord*>& Set : Sets)
    {
        TArray<FSCCargoGridModel> Work = Grids;
        TArray<FSCCargoItem> Items;
        for (int32 I = 0; I < Set.Num(); ++I) Items.Add(Item(Set[I], FirstItemId + I));
        TArray<FSCCargoPlacement> Slots;
        TArray<int32> GridOf;
        if (!PackItems(Work, Items, NoSupport, Slots, GridOf)) continue;
        P.bValid = true;
        for (int32 I = 0; I < Set.Num(); ++I)
        {
            FSCCargoSpawnBoxPlan& B = P.Boxes.AddDefaulted_GetRef();
            B.Entity = Set[I]->Entity;
            B.SCU = Set[I]->SCU;
            B.GridId = Grids[GridOf[I]].GridId;
            B.Origin = Slots[I].Origin;
            B.Cells = Slots[I].Cells;
            B.bRotated = Slots[I].bRotated;
            P.TotalSCU += B.SCU;
        }
        // Bottom up: a box is put down after the ones it rests on.
        P.Boxes.StableSort([](const FSCCargoSpawnBoxPlan& A, const FSCCargoSpawnBoxPlan& B) { return A.Origin.Z < B.Origin.Z; });
        return P;
    }
    // Full: how many of the first (preferred) set do fit.
    const TArray<const FSCCargoBoxRecord*>& First = Sets[0];
    TArray<FSCCargoGridModel> Work = Grids;
    TArray<FSCCargoItem> Items;
    for (int32 I = 0; I < First.Num(); ++I) Items.Add(Item(First[I], FirstItemId + I));
    TArray<FSCCargoPlacement> Slots;
    TArray<int32> GridOf;
    int32 Fit = 0;
    PackItems(Work, Items, NoSupport, Slots, GridOf, &Fit);
    int32 FirstScu = 0;
    for (const FSCCargoBoxRecord* B : First) FirstScu += B->SCU;
    const bool bOneSize = !First.ContainsByPredicate([&](const FSCCargoBoxRecord* B) { return B->SCU != First[0]->SCU; });
    FString Why;
    if (Fit == 0) Why = FString::Printf(TEXT("no room on %s for a %d SCU box of %s"), *Where, First[0]->SCU, *Label);
    else if (bOneSize) Why = FString::Printf(TEXT("only %d of %d %d SCU boxes of %s fit on %s"), Fit, First.Num(), First[0]->SCU, *Label, *Where);
    else Why = FString::Printf(TEXT("no room on %s for %d SCU of %s: %d of its %d boxes fit"), *Where, FirstScu, *Label, Fit, First.Num());
    P = Refuse(P, TEXT("full"), Why);
    P.FitCount = Fit;
    return P;
}

TArray<int32> SCCargoSpawn::Clearable(const FSCCargoGridModel& Grid, const TSet<int32>& Candidates)
{
    // What stays (not a candidate) keeps everything beneath it, however deep.
    TSet<int32> Keep;
    for (const FSCCargoPlacement& P : Grid.Placed)
        if (!Candidates.Contains(P.ItemId)) Keep.Add(P.ItemId);
    for (bool bChanged = true; bChanged;)
    {
        bChanged = false;
        for (const FSCCargoPlacement& P : Grid.Placed)
        {
            if (Keep.Contains(P.ItemId)) continue;
            if (Grid.Placed.ContainsByPredicate([&](const FSCCargoPlacement& K) { return Keep.Contains(K.ItemId) && P.Supports(K); }))
            {
                Keep.Add(P.ItemId);
                bChanged = true;
            }
        }
    }
    TArray<int32> Out;
    for (const FSCCargoPlacement& P : Grid.Placed)
        if (Candidates.Contains(P.ItemId) && !Keep.Contains(P.ItemId)) Out.AddUnique(P.ItemId);
    return Out;
}

#undef LOCTEXT_NAMESPACE

// ---- console words --------------------------------------------------------------------------------------------------

bool SCCargoSpawn::ParseWhole(const FString& Text, int32& Out)
{
    if (Text.IsEmpty() || Text.Len() > 6) return false;
    for (const TCHAR C : Text)
        if (!FChar::IsDigit(C)) return false;
    Out = FCString::Atoi(*Text);
    return Out > 0;
}

SCCargoSpawn::EAmountWord SCCargoSpawn::ParseAmountWord(const FString& Word, int32& OutScu, int32& OutCount)
{
    OutScu = OutCount = 0;
    // A word that holds a digit and only number characters (or an x before them) is meant as an amount.
    const bool bCount = Word.Len() > 1 && (Word[0] == TEXT('x') || Word[0] == TEXT('X'));
    const FString Number = bCount ? Word.RightChop(1) : Word;
    bool bDigit = false, bNumberish = !Number.IsEmpty();
    for (const TCHAR C : Number)
    {
        bDigit |= FChar::IsDigit(C);
        bNumberish &= FChar::IsDigit(C) || C == TEXT('.') || C == TEXT(',') || C == TEXT('-') || C == TEXT('+');
    }
    if (!bDigit || !bNumberish) return EAmountWord::NotAmount;
    int32 Value = 0;
    if (!ParseWhole(Number, Value)) return EAmountWord::Invalid;
    (bCount ? OutCount : OutScu) = Value;
    return EAmountWord::Amount;
}
