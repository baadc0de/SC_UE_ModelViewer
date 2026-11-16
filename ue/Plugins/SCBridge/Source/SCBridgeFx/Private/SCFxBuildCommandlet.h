// USCFxBuildCommandlet: builds the game's particle effects (read through the FFI) as Niagara systems with lightweight
// emitters under /Game/SC/FX (WP-K4c step 3). Usage and the mapping rules are in SCFxBuildCommandlet.cpp.
#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "SCFxBuildCommandlet.generated.h"

UCLASS()
class USCFxBuildCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    USCFxBuildCommandlet();
    virtual int32 Main(const FString& Params) override;
};
