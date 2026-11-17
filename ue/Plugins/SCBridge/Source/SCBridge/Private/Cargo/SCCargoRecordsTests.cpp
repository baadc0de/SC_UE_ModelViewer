// Automation test of the K1a cargo fields in FSCPackageReader (SCBridge.Cargo.Reader*): a synthetic scene.json with
// children port paths, vehicle and interior inventory containers and a root occupancy. Values are synthetic.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCPackageReader.h"
#include "SCPackageSource.h"
#include "Cargo/SCCargoScenario.h"

namespace
{
class FCargoFixtureSource : public FSCPackageSource
{
public:
    TMap<FString, FString> Files;
    bool Exists(const FString& Path) const override { return Files.Contains(Path); }
    bool ReadBytes(const FString& Path, TArray<uint8>& Out, FString& Error) const override
    {
        const FString* Text = Files.Find(Path);
        if (!Text) { Error = TEXT("missing fixture ") + Path; return false; }
        FTCHARToUTF8 Utf8(**Text);
        Out.Reset();
        Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        return true;
    }
    TArray<FString> ListFiles(const FString& Prefix) const override
    {
        TArray<FString> R;
        for (const auto& P : Files) if (Prefix.IsEmpty() || P.Key.StartsWith(Prefix)) R.Add(P.Key);
        return R;
    }
};
const TCHAR* SyntheticScene = TEXT(R"({"version":3,"export_kind":"Decomposed",
  "root_entity":{"entity_name":"SyntheticBox","item_type":"Cargo","item_sub_type":"Synthetic","display_name":"Synthetic crate",
    "inventory_occupancy":{"dimensions":[1.5,3,1.5],"volume":7,"volume_unit":"SyntheticUnit","local_bounds_min":[-0.75,-1.5,0],"local_bounds_max":[0.75,1.5,1.5]},"standard_cargo_units":7,"mass":1234},
  "children":[
    {"entity_name":"SyntheticPlate","parent_entity_name":"SyntheticBox","parent_node_name":"hp_plate_a","port_path":["hp_plate_a"],"source_transform_basis":"gltf_y_up"},
    {"entity_name":"SyntheticPlate","parent_entity_name":"SyntheticBox","parent_node_name":"hp_plate_b","port_path":["hp_plate_b"],"source_transform_basis":"gltf_y_up"}],
  "inventory_containers":[
    {"entity_name":"SyntheticGrid","entity_class_guid":"00000000-0000-0000-0000-000000000001","port_path":["hp_plate_b","hp_grid"],
     "parent_port_path":["hp_plate_b"],"parent_node_name":"hp_grid_node",
     "local_transform_sc":[[1,0,0,0],[0,1,0,0],[0,0,1,0],[0.5,0,0,1]],
     "container":{"record":"InventoryContainer.Synthetic","record_guid":"00000000-0000-0000-0000-000000000002","inventory_type":"InventoryOpenContainerType",
       "interior_dimensions":[3,6,3],"grid_cell_size":0.01,"grid_pos_offset":[0,0,0],"min_permitted_item_size":{"x":1.5,"y":1.5,"z":1.5},
       "max_permitted_item_size":[3,6,3],"is_external_container":true,"excluded_item_sub_types":["Excluded"],"capacity":0}}],
  "interiors":[{"name":"SyntheticRoom","placements":[],
    "inventory_containers":[{"entity_id":"42","entity_class":"SyntheticPlatformGrid","entity_class_guid":"00000000-0000-0000-0000-000000000003",
      "transform":[[0,1,0,0],[-1,0,0,0],[0,0,1,0],[2,3,4,1]],"links":["41"],
      "container":{"record":"InventoryContainer.SyntheticPlatform","interior_dimensions":[6,4.5,3],"min_permitted_item_size":[1.5,1.5,1.5],
        "max_permitted_item_size":[0,0,0],"is_external_container":false}}]}]})");
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoReaderTest, "SCBridge.Cargo.ReaderFields", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCCargoReaderTest::RunTest(const FString&)
{
    FCargoFixtureSource Source;
    Source.Files.Add(TEXT("scene.json"), SyntheticScene);
    FSCSceneManifest M;
    FString Error;
    TArray<FString> Warnings;
    TestTrue(*Error, FSCPackageReader::Read(Source, M, Error, TEXT(""), &Warnings));
    TestEqual(TEXT("children keep their port paths"), M.Children.Num(), 2);
    if (M.Children.Num() == 2) TestEqual(TEXT("port path of the second plate"), M.Children[1].PortPath, TArray<FString>{TEXT("hp_plate_b")});
    TestEqual(TEXT("one vehicle and one interior container"), M.InventoryContainers.Num(), 2);
    if (M.InventoryContainers.Num() == 2)
    {
        const FSCInventoryContainerRecord& V = M.InventoryContainers[0];
        TestEqual(TEXT("vehicle entry: no interior"), V.InteriorIndex, int32(INDEX_NONE));
        TestEqual(TEXT("parent port path"), V.ParentPortPath, TArray<FString>{TEXT("hp_plate_b")});
        TestEqual(TEXT("parent node"), V.ParentNodeName, FString(TEXT("hp_grid_node")));
        TestEqual(TEXT("local transform (column-major, metres)"), V.LocalMatrix.GetOrigin(), FVector(0.5, 0, 0));
        TestEqual(TEXT("interior dimensions"), V.Container.InteriorDimensions, FVector(3, 6, 3));
        TestEqual(TEXT("min item size from an {x,y,z} object"), V.Container.MinPermittedItemSize, FVector(1.5));
        TestTrue(TEXT("external"), V.Container.bExternalContainer);
        TestEqual(TEXT("excluded sub types"), V.Container.ExcludedItemSubTypes, TArray<FString>{TEXT("Excluded")});
        TestTrue(TEXT("unknown fields survive in the raw json"), V.Container.Json.Contains(TEXT("capacity")));
        const FSCInventoryContainerRecord& I = M.InventoryContainers[1];
        TestEqual(TEXT("interior entry index"), I.InteriorIndex, 0);
        TestEqual(TEXT("entity id"), I.EntityId, FString(TEXT("42")));
        TestEqual(TEXT("links"), I.Links, TArray<FString>{TEXT("41")});
        TestEqual(TEXT("interior transform"), I.LocalMatrix.GetOrigin(), FVector(2, 3, 4));
        TestEqual(TEXT("zero max item stays zero"), I.Container.MaxPermittedItemSize, FVector::ZeroVector);
    }
    TestTrue(TEXT("root occupancy"), M.RootOccupancy.bValid);
    TestEqual(TEXT("occupancy dimensions"), M.RootOccupancy.Dimensions, FVector(1.5, 3, 1.5));
    TestEqual(TEXT("standard cargo units"), M.RootOccupancy.StandardCargoUnits, 7);
    TestEqual(TEXT("mass"), M.RootOccupancy.Mass, 1234.0);
    TestEqual(TEXT("volume unit as authored"), M.RootOccupancy.VolumeUnit, FString(TEXT("SyntheticUnit")));
    TestEqual(TEXT("volume"), M.RootOccupancy.Volume, 7.0);
    TestEqual(TEXT("local bounds"), M.RootOccupancy.LocalBoundsMax, FVector(0.75, 1.5, 1.5));
    TestEqual(TEXT("display name"), M.RootOccupancy.DisplayName, FString(TEXT("Synthetic crate")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCCargoTractorRecordTest, "SCBridge.Cargo.TractorRecord", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCCargoTractorRecordTest::RunTest(const FString&)
{
    // A synthetic record in sc_datacore_record's shape: two tractor actions, the scenario names one of them.
    const FString Record = TEXT(R"({"__record_name":"EntityClassDefinition.synthetic_tool","__type":"EntityClassDefinition","Components":[
        {"__type":"SSomething","fireActions":[{"__type":"SWeaponActionFireTractorBeamParams","name":"Other","maxForce":9,"fullStrengthDistance":9}]},
        {"__type":"SWeapon","aimAction":{"conditionalWeaponActions":[{"weaponAction":{"__type":"SWeaponActionFireTractorBeamParams","name":"Beam",
         "minForce":10,"maxForce":1000,"minDistance":0.25,"maxDistance":40,"fullStrengthDistance":2,"maxVolume":7,"volumeForceCoefficient":0.5,
         "fireEffects":[{"__type":"SWeaponParticleEffectParams","firstPerson":{"__type":"GlobalResourceParticle","path":"g1|lib/a.muzzle"}}],
         "beamGroup":{"__type":"SBeamGroupParams","hitEffectName":"Custom_Hit","beamEffects":[{"__type":"SBeamEffectParams","particleEffect":{"__type":"GlobalResourceParticle","path":"g2|lib/a.beam"}}]},
         "beamStrengthValues":{"__type":"SWeaponActionFireTractorBeamStrengthValueParams","noTarget":0.1,"stable":0.9,"unstable":0.4,"impactInvalidTarget":0.2,"impactControllingTarget":0.8,"beamEffectBlendCoeff":3}}}]}}]})");
    FSCCargoDroneDefinition D;
    D.TractorAction = TEXT("Beam");
    D.MaxForceN = 1; // the scenario's copy
    FString Error;
    TestTrue(TEXT("the named action is found"), D.ApplyTractorRecord(Record, Error));
    TestEqual(TEXT("force from the record"), D.MaxForceN, 1000.0);
    TestEqual(TEXT("distances"), FVector(D.MinDistanceM, D.MaxDistanceM, D.FullStrengthDistanceM), FVector(0.25, 40, 2));
    TestEqual(TEXT("volume fields kept"), FVector2D(D.MaxVolume, D.VolumeForceCoefficient), FVector2D(7, 0.5));
    TestEqual(TEXT("source"), D.TractorSource, FString(TEXT("datacore")));
    TestEqual(TEXT("beam effect"), D.BeamEffect, FString(TEXT("g2|lib/a.beam")));
    TestEqual(TEXT("muzzle effect"), D.MuzzleEffect, FString(TEXT("g1|lib/a.muzzle")));
    TestEqual(TEXT("hit effect"), D.HitEffectName, FString(TEXT("Custom_Hit")));
    TestTrue(TEXT("strengths read"), D.Strengths.bSet);
    TestEqual(TEXT("strengths"), FVector(D.Strengths.Stable, D.Strengths.Unstable, D.Strengths.NoTarget), FVector(0.9, 0.4, 0.1));
    TestEqual(TEXT("impact strengths and blend"), FVector(D.Strengths.ImpactControlling, D.Strengths.ImpactInvalid, D.Strengths.BlendCoeff), FVector(0.8, 0.2, 3));
    FSCCargoDroneDefinition Missing;
    Missing.TractorAction = TEXT("Nope");
    Missing.MaxForceN = 5;
    TestFalse(TEXT("an action the record lacks is an error"), Missing.ApplyTractorRecord(Record, Error));
    TestEqual(TEXT("the scenario's values stand"), Missing.MaxForceN, 5.0);
    return true;
}
#endif
