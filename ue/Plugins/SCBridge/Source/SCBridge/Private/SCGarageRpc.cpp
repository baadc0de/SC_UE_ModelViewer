#include "SCGarageRpc.h"

#if SC_WITH_RPC
#include "SCGarageSubsystem.h"
#include "Engine/World.h"
#include "UI/Cargo/SCCargoViewSource.h"

using namespace SCRpcJson;

namespace
{
FObj Reply(const FString& Why = {})
{
    FObj Out = MakeShared<FJsonObject>();
    Out->SetBoolField(TEXT("ok"), Why.IsEmpty());
    if (!Why.IsEmpty()) Out->SetStringField(TEXT("error"), Why);
    return Out;
}

FString Text(const TSharedPtr<FJsonObject>& Body, const TCHAR* Key)
{
    FString Out;
    if (Body) Body->TryGetStringField(Key, Out);
    return Out;
}

bool ShipId(const TSharedPtr<FJsonObject>& Body, int32& Out)
{
    double Value = -1;
    if (!Body || !Body->TryGetNumberField(TEXT("ship"), Value) || !FMath::IsFinite(Value) ||
        Value < 0 || Value > MAX_int32 || FMath::TruncToDouble(Value) != Value) return false;
    Out = int32(Value);
    return true;
}
}

FObj SCGarageRpc::Snapshot(UWorld* World, const FString& Entity, bool bIncludeDefaultStored)
{
    auto* Garage = USCGarageSubsystem::Get(World);
    TArray<TSharedPtr<FJsonValue>> Fleet;
    if (Garage)
        for (const FSCGarageRow& Row : Garage->Rows(Entity, bIncludeDefaultStored))
        {
            const FObj Info = MakeShared<FJsonObject>();
            Info->SetStringField(TEXT("entity"), Row.Entity);
            Info->SetStringField(TEXT("label"), Row.DisplayName);
            Info->SetStringField(TEXT("state"), FSCGaragePolicy::StatusName(Row.Status));
            if (!Row.Role.IsEmpty()) Info->SetStringField(TEXT("role"), Row.Role);
            if (!Row.Size.IsEmpty()) Info->SetStringField(TEXT("size"), Row.Size);
            if (!Row.Error.IsEmpty()) Info->SetStringField(TEXT("reason"), Row.Error);
            if (Row.Ship != INDEX_NONE) Info->SetNumberField(TEXT("ship"), Row.Ship);
            if (Row.Status == ESCGarageStatus::Retrieving) Info->SetField(TEXT("progress"), Number(Row.Progress));
            if (Row.Status == ESCGarageStatus::InHangar)
            {
                Info->SetField(TEXT("distance_m"), Number(Row.DistanceM));
                Info->SetStringField(TEXT("paint"), Row.Paint);
                if (!Row.PaintOptions.IsEmpty()) Info->SetField(TEXT("paints"), Strings(Row.PaintOptions));
            }
            Fleet.Add(MakeShared<FJsonValueObject>(Info));
        }
    const FObj Out = MakeShared<FJsonObject>();
    Out->SetArrayField(TEXT("fleet"), Fleet);
    if (World == SCCargoUI::ViewerWorld())
        Out->SetBoolField(TEXT("ui_ready"), FSCMobiGlasRequests::Get().bGarageReady);
    return Out;
}

FObj SCGarageRpc::Command(UWorld* World, const FString& Route, const TSharedPtr<FJsonObject>& Body)
{
    USCGarageSubsystem* Garage = USCGarageSubsystem::Get(World);
    if (!Garage) return Reply(TEXT("The garage is unavailable"));
    if (Route == TEXT("fleet"))
    {
        Garage->RequestCatalog();
        FObj Out = Reply();
        Out->SetObjectField(TEXT("garage"), Snapshot(World, Text(Body, TEXT("entity")), true));
        return Out;
    }

    if (Route == TEXT("retrieve"))
    {
        const FString Position = Text(Body, TEXT("at")).ToLower();
        if (!Position.IsEmpty() && Position != TEXT("bay") && Position != TEXT("floor"))
            return Reply(TEXT("at must be bay or floor"));
        bool bConfirmed = false;
        if (Body) Body->TryGetBoolField(TEXT("confirm_store"), bConfirmed);
        const FSCGarageOrder R = Garage->Retrieve(Text(Body, TEXT("entity")),
            Position == TEXT("floor") ? ESCGaragePlacement::Floor : ESCGaragePlacement::Bay, bConfirmed);
        FObj Out = Reply(R.Refusal);
        if (R.Ship != INDEX_NONE) Out->SetNumberField(TEXT("ship"), R.Ship);
        if (R.Occupant != INDEX_NONE) Out->SetNumberField(TEXT("occupant"), R.Occupant);
        if (R.bConfirmStore) Out->SetBoolField(TEXT("confirm_store"), true);
        return Out;
    }

    int32 Id = INDEX_NONE;
    if (Route == TEXT("store"))
        return Reply(ShipId(Body, Id) ? Garage->Store(Id) : FString(TEXT("ship is required")));

    const FString Palette = Text(Body, TEXT("paint"));
    return Reply(ShipId(Body, Id) ? Garage->Paint(Id, Palette) : Garage->PaintEntity(Text(Body, TEXT("entity")), Palette));
}
#endif
