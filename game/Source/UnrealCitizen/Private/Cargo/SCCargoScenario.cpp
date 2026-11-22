// FSCCargoScenario: JSON parsing of a cargo scenario file (see SCCargoScenario.h for the schema's meaning).
#include "Cargo/SCCargoScenario.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
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
double Num(const FObj& O, const TCHAR* Key, double Default = 0)
{
    double V = Default;
    if (O) O->TryGetNumberField(Key, V);
    return V;
}
FObj Child(const FObj& O, const TCHAR* Key)
{
    const FObj* C;
    return O && O->TryGetObjectField(Key, C) ? *C : nullptr;
}
/** "at": [x, y, z] metres, with "rot": [pitch, yaw, roll] or "yaw" degrees; unset without "at". */
TOptional<FTransform> Pose(const FObj& O)
{
    const TArray<TSharedPtr<FJsonValue>>* At;
    if (!O || !O->TryGetArrayField(TEXT("at"), At) || At->Num() < 3) return {};
    FRotator R(0, Num(O, TEXT("yaw")), 0);
    const TArray<TSharedPtr<FJsonValue>>* Rot;
    if (O->TryGetArrayField(TEXT("rot"), Rot) && Rot->Num() >= 3) R = FRotator((*Rot)[0]->AsNumber(), (*Rot)[1]->AsNumber(), (*Rot)[2]->AsNumber());
    return FTransform(R, FVector((*At)[0]->AsNumber(), (*At)[1]->AsNumber(), (*At)[2]->AsNumber()) * 100.0);
}
bool Selector(const FObj& O, FSCCargoGridSelector& Out, FString& Error)
{
    if (!O) { Error = TEXT("grid selector missing"); return false; }
    const FString Owner = Str(O, TEXT("owner"));
    if (Owner == TEXT("vehicle")) Out.Kind = ESCCargoGridKind::Vehicle;
    else if (Owner == TEXT("environment")) Out.Kind = ESCCargoGridKind::Environment;
    else { Error = TEXT("grid.owner must be \"vehicle\" or \"environment\""); return false; }
    Out.Index = int32(Num(O, TEXT("index"), 0));
    Out.Vehicle = int32(Num(O, TEXT("vehicle"), 0));
    Out.Match = Str(O, TEXT("match"));
    return true;
}
} // namespace

TArray<FString> FSCCargoScenario::Entities() const
{
    TArray<FString> Out;
    for (const FSCCargoStock& S : Stock)
        for (const FSCCargoStockEntry& E : S.Boxes) Out.AddUnique(E.Entity);
    for (const FLoose& L : Loose) Out.AddUnique(L.Entity);
    return Out;
}

bool FSCCargoScenario::Load(const FString& File, FSCCargoScenario& Out, FString& Error)
{
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *File)) { Error = TEXT("cannot read ") + File; return false; }
    if (!Parse(Text, Out, Error)) { Error = File + TEXT(": ") + Error; return false; }
    Out.Path = File;
    return true;
}

bool FSCCargoDroneDefinition::ApplyTractorRecord(const FString& RecordJson, FString& Error)
{
    TSharedPtr<FJsonValue> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RecordJson), Root) || !Root) { Error = TEXT("invalid record JSON"); return false; }
    // Depth-first: the first tractor-beam action object carrying the scenario's action name.
    TFunction<FObj(const TSharedPtr<FJsonValue>&)> Find = [&](const TSharedPtr<FJsonValue>& V) -> FObj {
        if (!V) return nullptr;
        if (V->Type == EJson::Array)
        {
            for (const TSharedPtr<FJsonValue>& E : V->AsArray())
                if (FObj F = Find(E)) return F;
            return nullptr;
        }
        if (V->Type != EJson::Object) return nullptr;
        const FObj O = V->AsObject();
        FString Type = Str(O, TEXT("__type"));
        if (Type.IsEmpty()) Type = Str(O, TEXT("_Type_"));
        if (Type == TEXT("SWeaponActionFireTractorBeamParams") && Str(O, TEXT("name")) == TractorAction) return O;
        for (const auto& Pair : O->Values)
            if (FObj F = Find(Pair.Value)) return F;
        return nullptr;
    };
    const FObj Action = Find(Root);
    if (!Action) { Error = FString::Printf(TEXT("no SWeaponActionFireTractorBeamParams named '%s' in the record"), *TractorAction); return false; }
    if (Num(Action, TEXT("maxForce")) <= 0 || Num(Action, TEXT("fullStrengthDistance")) <= 0)
    {
        Error = TEXT("the tractor action has no maxForce / fullStrengthDistance");
        return false;
    }
    MaxForceN = Num(Action, TEXT("maxForce"));
    MinDistanceM = Num(Action, TEXT("minDistance"));
    MaxDistanceM = Num(Action, TEXT("maxDistance"));
    FullStrengthDistanceM = Num(Action, TEXT("fullStrengthDistance"));
    MaxVolume = Num(Action, TEXT("maxVolume"));
    VolumeForceCoefficient = Num(Action, TEXT("volumeForceCoefficient"));
    if (const double Angle = Num(Action, TEXT("maxAngle")); Angle > 0) MaxAngleDeg = Angle;
    // Its effects (WP-K4c): references as the record writes them; absent fields leave them empty.
    auto First = [](const FObj& O, const TCHAR* Key) -> FObj {
        const TArray<TSharedPtr<FJsonValue>>* A;
        return O && O->TryGetArrayField(Key, A) && !A->IsEmpty() && (*A)[0]->Type == EJson::Object ? (*A)[0]->AsObject() : nullptr;
    };
    const FObj Group = Child(Action, TEXT("beamGroup"));
    BeamEffect = Str(Child(First(Group, TEXT("beamEffects")), TEXT("particleEffect")), TEXT("path"));
    HitEffectName = Str(Group, TEXT("hitEffectName"));
    MuzzleEffect = Str(Child(First(Action, TEXT("fireEffects")), TEXT("firstPerson")), TEXT("path"));
    if (const FObj S = Child(Action, TEXT("beamStrengthValues")))
    {
        Strengths.Stable = Num(S, TEXT("stable"));
        Strengths.Unstable = Num(S, TEXT("unstable"));
        Strengths.NoTarget = Num(S, TEXT("noTarget"));
        Strengths.ImpactControlling = Num(S, TEXT("impactControllingTarget"));
        Strengths.ImpactInvalid = Num(S, TEXT("impactInvalidTarget"));
        Strengths.BlendCoeff = Num(S, TEXT("beamEffectBlendCoeff"));
        Strengths.bSet = true;
    }
    TractorSource = TEXT("datacore");
    return true;
}

bool FSCCargoScenario::Parse(const FString& Json, FSCCargoScenario& Out, FString& Error)
{
    Out = FSCCargoScenario();
    FObj Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root) { Error = TEXT("invalid JSON"); return false; }
    Out.Name = Str(Root, TEXT("name"));
    Out.Ship = Str(Root, TEXT("ship"));
    Out.Environment = Str(Root, TEXT("environment"));
    Out.bAtmosphere = !Out.Environment.IsEmpty();
    Root->TryGetBoolField(TEXT("atmosphere"), Out.bAtmosphere);
    if (!Str(Root, TEXT("player")).IsEmpty()) Out.Player = FName(*Str(Root, TEXT("player")));
    const TArray<TSharedPtr<FJsonValue>>* OwnerList;
    if (Root->TryGetArrayField(TEXT("owners"), OwnerList))
        for (const auto& V : *OwnerList)
            Out.Owners.Add({FName(*Str(V->AsObject(), TEXT("id"))), FText::FromString(Str(V->AsObject(), TEXT("label")))});
    // Space (WP-K3): no environment, no gravity, vacuum (unless the file says otherwise).
    Root->TryGetBoolField(TEXT("space"), Out.bSpace);
    Root->TryGetNumberField(TEXT("sun_lux"), Out.SunLux);
    if (Out.bSpace && !Root->HasField(TEXT("atmosphere"))) Out.bAtmosphere = false;
    Out.Seed = int32(Num(Root, TEXT("seed"), 1));
    const TArray<TSharedPtr<FJsonValue>>* VehicleList;
    if (Root->TryGetArrayField(TEXT("vehicles"), VehicleList))
        for (const auto& V : *VehicleList)
        {
            const FString Who = Str(V->AsObject(), TEXT("owner"));
            Out.VehicleOwners.Add(Who.IsEmpty() ? Out.Player : FName(*Who));
            Out.VehiclePoses.Add(Pose(V->AsObject()));
        }
    const TArray<TSharedPtr<FJsonValue>>* Things;
    if (Root->TryGetArrayField(TEXT("obstacles"), Things))
        for (const auto& V : *Things)
        {
            const TOptional<FTransform> At = Pose(V->AsObject());
            if (Str(V->AsObject(), TEXT("entity")).IsEmpty() || !At) { Error = TEXT("obstacles need entity and at"); return false; }
            Out.Obstacles.Add({Str(V->AsObject(), TEXT("entity")), *At});
        }
    if (Root->TryGetArrayField(TEXT("loose"), Things))
        for (const auto& V : *Things)
        {
            const FObj O = V->AsObject();
            FLoose L;
            L.Entity = Str(O, TEXT("entity"));
            L.Count = int32(Num(O, TEXT("count"), 1));
            L.Commodity = FName(*Str(O, TEXT("commodity")));
            L.CommodityLabel = FText::FromString(Str(O, TEXT("label")).IsEmpty() ? L.Commodity.ToString() : Str(O, TEXT("label")));
            if (!Str(O, TEXT("owner")).IsEmpty()) L.Owner = FName(*Str(O, TEXT("owner")));
            L.At = Pose(O);
            if (L.Entity.IsEmpty() || L.Commodity.IsNone()) { Error = TEXT("loose stock needs entity and commodity"); return false; }
            Out.Loose.Add(MoveTemp(L));
        }
    const FObj Drone = Child(Root, TEXT("drone"));
    const FObj Beam = Child(Drone, TEXT("tractor_beam"));
    const FObj Flight = Child(Drone, TEXT("flight"));
    if (!Beam || !Flight) { Error = TEXT("drone.tractor_beam and drone.flight are required"); return false; }
    FSCCargoDroneDefinition& D = Out.Drone;
    D.TractorRecord = Str(Beam, TEXT("record"));
    D.TractorRecordGuid = Str(Beam, TEXT("record_guid"));
    D.TractorAction = Str(Beam, TEXT("action"));
    D.Provenance = Str(Beam, TEXT("provenance"));
    D.MaxForceN = Num(Beam, TEXT("max_force"));
    D.MinDistanceM = Num(Beam, TEXT("min_distance"));
    D.MaxDistanceM = Num(Beam, TEXT("max_distance"));
    D.FullStrengthDistanceM = Num(Beam, TEXT("full_strength_distance"));
    D.MaxVolume = Num(Beam, TEXT("max_volume"));
    D.VolumeForceCoefficient = Num(Beam, TEXT("volume_force_coefficient"));
    if (const double Angle = Num(Beam, TEXT("max_angle")); Angle > 0) D.MaxAngleDeg = Angle;
    D.MaxSpeedMS = float(Num(Flight, TEXT("max_speed_m_s")));
    D.MaxAccelMS2 = float(Num(Flight, TEXT("max_accel_m_s2")));
    D.ClearanceMarginM = float(Num(Flight, TEXT("clearance_margin_m")));
    D.FormationSpacingM = float(Num(Flight, TEXT("formation_spacing_m")));
    D.MaxJerkMS3 = float(Num(Flight, TEXT("max_jerk_m_s3")));
    D.DroneMassKg = float(Num(Flight, TEXT("drone_mass_kg")));
    D.MinLoadAccelMS2 = float(Num(Flight, TEXT("min_load_accel_m_s2")));
    D.NormalClearanceM = float(Num(Flight, TEXT("normal_clearance_m")));
    D.SwayAmplitudeCm = float(Num(Flight, TEXT("sway_amplitude_cm"), D.SwayAmplitudeCm));
    D.SwayTiltDeg = float(Num(Flight, TEXT("sway_tilt_deg"), D.SwayTiltDeg));
    D.SwayFrequencyHz = float(Num(Flight, TEXT("sway_frequency_hz"), D.SwayFrequencyHz));
    if (D.MaxForceN <= 0 || D.FullStrengthDistanceM <= 0 || D.MaxSpeedMS <= 0 || D.MaxAccelMS2 <= 0 || D.MaxJerkMS3 <= 0)
    {
        Error = TEXT("drone needs tractor_beam.max_force, full_strength_distance and flight.max_speed_m_s, max_accel_m_s2, max_jerk_m_s3");
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* List;
    if (Root->TryGetArrayField(TEXT("docks"), List))
        for (const auto& V : *List)
        {
            FSCCargoDockSpec Dock;
            if (!Selector(Child(V->AsObject(), TEXT("grid")), Dock.Grid, Error)) return false;
            Dock.Drones = int32(Num(V->AsObject(), TEXT("drones")));
            Out.Docks.Add(Dock);
        }
    if (Root->TryGetArrayField(TEXT("stock"), List))
        for (const auto& V : *List)
        {
            FSCCargoStock Stock;
            if (!Selector(Child(V->AsObject(), TEXT("grid")), Stock.Grid, Error)) return false;
            const TArray<TSharedPtr<FJsonValue>>* Boxes;
            if (V->AsObject()->TryGetArrayField(TEXT("boxes"), Boxes))
                for (const auto& B : *Boxes)
                {
                    const FObj O = B->AsObject();
                    FSCCargoStockEntry E;
                    E.Entity = Str(O, TEXT("entity"));
                    E.Count = int32(Num(O, TEXT("count"), 1));
                    E.Commodity = FName(*Str(O, TEXT("commodity")));
                    if (!Str(O, TEXT("owner")).IsEmpty()) E.Owner = FName(*Str(O, TEXT("owner")));
                    E.CommodityLabel = FText::FromString(Str(O, TEXT("label")).IsEmpty() ? E.Commodity.ToString() : Str(O, TEXT("label")));
                    if (E.Entity.IsEmpty() || E.Commodity.IsNone()) { Error = TEXT("stock boxes need entity and commodity"); return false; }
                    const TArray<TSharedPtr<FJsonValue>>* Cell;
                    if (O->TryGetArrayField(TEXT("origin"), Cell) && Cell->Num() == 3)
                        E.Origin = FIntVector(int32((*Cell)[0]->AsNumber()), int32((*Cell)[1]->AsNumber()), int32((*Cell)[2]->AsNumber()));
                    O->TryGetBoolField(TEXT("rotated"), E.bRotated);
                    Stock.Boxes.Add(MoveTemp(E));
                }
            Out.Stock.Add(MoveTemp(Stock));
        }
    return true;
}
