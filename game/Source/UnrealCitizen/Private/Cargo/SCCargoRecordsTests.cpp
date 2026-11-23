// Automation test of a cargo scenario's tractor record (SCBridge.Cargo.TractorRecord). The package reader's cargo fields
// (SCBridge.Cargo.ReaderFields) are the shared plugin's (SCCargoReaderTests.cpp).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Cargo/SCCargoScenario.h"

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
