// Parses the K1a cargo fields of scene.json into FSCSceneManifest (inventory containers, port paths, item occupancy).
// Values are kept as authored (metres, SC axes); nothing is defaulted: a missing field stays zero/empty.
#include "Cargo/SCCargoRecords.h"
#include "SCSceneManifest.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace
{
using FObj = TSharedPtr<FJsonObject>;
FString Str(const FObj& O, const TCHAR* Key)
{
    FString S;
    if (O) O->TryGetStringField(Key, S);
    return S;
}
const TArray<TSharedPtr<FJsonValue>>& Arr(const FObj& O, const TCHAR* Key)
{
    static const TArray<TSharedPtr<FJsonValue>> Empty;
    const TArray<TSharedPtr<FJsonValue>>* A;
    return O && O->TryGetArrayField(Key, A) ? *A : Empty;
}
FString ToJson(const FObj& O)
{
    FString S;
    if (O) FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&S));
    return S;
}
// A vector authored as [x, y, z] or {x, y, z}.
bool Vec(const FObj& O, const TCHAR* Key, FVector& Out)
{
    Out = FVector::ZeroVector;
    if (!O || !O->HasField(Key)) return false;
    const auto& A = Arr(O, Key);
    if (A.Num() == 3)
    {
        Out = FVector(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber());
        return true;
    }
    const FObj* V;
    if (!O->TryGetObjectField(Key, V)) return false;
    double X = 0, Y = 0, Z = 0;
    const bool bOK = (*V)->TryGetNumberField(TEXT("x"), X) & (*V)->TryGetNumberField(TEXT("y"), Y) & (*V)->TryGetNumberField(TEXT("z"), Z);
    Out = FVector(X, Y, Z);
    return bOK;
}
// glTF-style column-major 4x4 (the placements' `transform` and children's `local_transform_sc`) as a UE row-vector matrix.
bool Matrix(const FObj& O, const TCHAR* Key, FMatrix& Out, FString& Error)
{
    Out = FMatrix::Identity;
    if (!O->HasField(Key)) return true;
    const auto& Columns = Arr(O, Key);
    if (Columns.Num() != 4) { Error = FString::Printf(TEXT("expected a 4x4 %s"), Key); return false; }
    for (int32 C = 0; C < 4; ++C)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values;
        if (!Columns[C]->TryGetArray(Values) || Values->Num() != 4) { Error = FString::Printf(TEXT("invalid %s column"), Key); return false; }
        for (int32 R = 0; R < 4; ++R)
        {
            double V;
            if (!(*Values)[R]->TryGetNumber(V) || !FMath::IsFinite(V)) { Error = FString::Printf(TEXT("non-finite %s"), Key); return false; }
            Out.M[C][R] = V;
        }
    }
    return true;
}
TArray<FString> Strings(const FObj& O, const TCHAR* Key)
{
    TArray<FString> Out;
    for (const auto& V : Arr(O, Key))
    {
        if (V->Type == EJson::String) Out.Add(V->AsString());
        else if (V->Type == EJson::Object) Out.Add(ToJson(V->AsObject()));
    }
    return Out;
}
} // namespace

TArray<FString> SCCargoRecords::PortPath(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
{
    TArray<FString> Out;
    for (const auto& V : Arr(Object, Key))
        if (V->Type == EJson::String) Out.Add(V->AsString());
    return Out;
}

bool SCCargoRecords::ParseContainer(const TSharedPtr<FJsonObject>& O, FSCCargoContainerParams& Out, FString& Error)
{
    Out = FSCCargoContainerParams();
    if (!O) { Error = TEXT("no container object"); return false; }
    Out.Json = ToJson(O);
    Out.Record = Str(O, TEXT("record"));
    Out.RecordGuid = Str(O, TEXT("record_guid"));
    Out.InventoryType = Str(O, TEXT("inventory_type"));
    Vec(O, TEXT("interior_dimensions"), Out.InteriorDimensions);
    Vec(O, TEXT("min_permitted_item_size"), Out.MinPermittedItemSize);
    Vec(O, TEXT("max_permitted_item_size"), Out.MaxPermittedItemSize);
    Vec(O, TEXT("grid_pos_offset"), Out.GridPosOffset);
    double Cell = 0;
    const FObj* CellObject;
    if (O->TryGetNumberField(TEXT("grid_cell_size"), Cell)) Out.GridCellSize = float(Cell);
    else if (O->TryGetObjectField(TEXT("grid_cell_size"), CellObject) && (*CellObject)->TryGetNumberField(TEXT("centimeters"), Cell))
        Out.GridCellSize = float(Cell / 100.0); // the record's own unit object
    O->TryGetBoolField(TEXT("is_external_container"), Out.bExternalContainer);
    Out.ExcludedItemSubTypes = Strings(O, TEXT("excluded_item_sub_types"));
    if (Out.InteriorDimensions.IsNearlyZero()) { Error = TEXT("container without interior_dimensions"); return false; }
    return true;
}

void SCCargoRecords::ParseOccupancy(const TSharedPtr<FJsonObject>& Root, FSCItemOccupancy& Out)
{
    Out = FSCItemOccupancy();
    if (!Root) return;
    const FObj* Occupancy;
    if (Root->TryGetObjectField(TEXT("inventory_occupancy"), Occupancy))
    {
        Out.bValid = Vec(*Occupancy, TEXT("dimensions"), Out.Dimensions) && !Out.Dimensions.IsNearlyZero();
        const FObj* Volume;
        double V = 0;
        if ((*Occupancy)->TryGetNumberField(TEXT("volume"), V)) Out.Volume = V;
        else if ((*Occupancy)->TryGetObjectField(TEXT("volume"), Volume))
        {
            // {value, unit} or a single {<unit>: value} pair, as authored.
            if ((*Volume)->TryGetNumberField(TEXT("value"), V)) { Out.Volume = V; Out.VolumeUnit = Str(*Volume, TEXT("unit")); }
            else
                for (const auto& Pair : (*Volume)->Values)
                    if (Pair.Value->TryGetNumber(V)) { Out.Volume = V; Out.VolumeUnit = Pair.Key; break; }
        }
        if (Out.VolumeUnit.IsEmpty()) Out.VolumeUnit = Str(*Occupancy, TEXT("volume_unit"));
        Vec(*Occupancy, TEXT("local_bounds_min"), Out.LocalBoundsMin);
        Vec(*Occupancy, TEXT("local_bounds_max"), Out.LocalBoundsMax);
    }
    double N = 0;
    if (Root->TryGetNumberField(TEXT("standard_cargo_units"), N)) Out.StandardCargoUnits = FMath::RoundToInt(N);
    if (Root->TryGetNumberField(TEXT("mass"), N)) Out.Mass = N;
    Out.ItemType = Str(Root, TEXT("item_type"));
    Out.ItemSubType = Str(Root, TEXT("item_sub_type"));
    Out.DisplayName = Str(Root, TEXT("display_name"));
}

void SCCargoRecords::AddContainers(const TSharedPtr<FJsonObject>& Owner, int32 Interior, FSCSceneManifest& Out,
                                   TFunctionRef<void(const FString&, const FString&)> Warn)
{
    for (const auto& V : Arr(Owner, TEXT("inventory_containers")))
    {
        const FObj O = V->Type == EJson::Object ? V->AsObject() : nullptr;
        FSCInventoryContainerRecord R;
        FString Error;
        const FObj* Container;
        if (!O || !O->TryGetObjectField(TEXT("container"), Container) || !ParseContainer(*Container, R.Container, Error))
        {
            Warn(TEXT("inventory_containers"), Error.IsEmpty() ? TEXT("entry without a container object") : Error);
            continue;
        }
        R.EntityName = Str(O, TEXT("entity_name"));
        R.EntityClass = Str(O, TEXT("entity_class"));
        R.EntityClassGuid = Str(O, TEXT("entity_class_guid"));
        R.EntityId = Str(O, TEXT("entity_id"));
        R.PortPath = PortPath(O);
        R.ParentPortPath = PortPath(O, TEXT("parent_port_path"));
        R.ParentNodeName = Str(O, TEXT("parent_node_name"));
        R.Links = Strings(O, TEXT("links"));
        R.InteriorIndex = Interior;
        if (!Matrix(O, Interior == INDEX_NONE ? TEXT("local_transform_sc") : TEXT("transform"), R.LocalMatrix, Error))
        {
            Warn(R.EntityName.IsEmpty() ? R.EntityClass : R.EntityName, Error);
            continue;
        }
        Out.InventoryContainers.Add(MoveTemp(R));
    }
}
