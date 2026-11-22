// SCBridge.ViewerExposure.LegacyClearMigration: the layout the engine fork's E1-4 review saved FPostProcessSettings with (one bool
// bOverriddenNoneAssetsClear for every asset, a BoolProperty tag), so the test can write tagged data as an old asset holds it and load it
// into the current FPostProcessSettings (tags match by name).
#pragma once
#include "CoreMinimal.h"
#include "SCLegacyPostProcessForTests.generated.h"

USTRUCT()
struct FSCLegacyOverriddenNoneClear
{
    GENERATED_BODY()

    FSCLegacyOverriddenNoneClear() : bOverriddenNoneAssetsClear(0) {}

    UPROPERTY()
    uint8 bOverriddenNoneAssetsClear : 1;
};
