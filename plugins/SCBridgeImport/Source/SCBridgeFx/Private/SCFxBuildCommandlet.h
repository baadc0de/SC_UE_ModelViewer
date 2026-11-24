// USCFxBuildCommandlet: builds the game's particle effects (read through the FFI) as Niagara systems with lightweight
// emitters under /Game/SC/FX (WP-K4c step 3). Usage and the mapping rules are in SCFxBuildCommandlet.cpp.
#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "StructUtils/InstancedStruct.h"
#include "SCFxBuildCommandlet.generated.h"

/** The stateless float expression the builder writes for a piecewise-linear strength curve (keys (t, v) sorted by t) of the input X
 *  (SCFxRules::ClampSum: clamp terms for ramps, comparison terms for steps). For the tests (SCBridge.Fx.ExpressionValidation). */
FInstancedStruct SCFxPiecewiseLinearExpression(const TArray<FVector2D>& Keys, const FInstancedStruct& X);

UCLASS()
class USCFxBuildCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    USCFxBuildCommandlet();
    virtual int32 Main(const FString& Params) override;
};
