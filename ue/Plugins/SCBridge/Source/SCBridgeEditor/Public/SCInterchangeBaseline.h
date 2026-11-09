#pragma once
#include "CoreMinimal.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SCInterchangeBaseline.generated.h"

/** Validation-only pipeline; the runtime importer never depends on Interchange. */
UCLASS()
class USCInterchangePipeline : public UInterchangeGenericAssetsPipeline
{
    GENERATED_BODY()
public:
    USCInterchangePipeline();
};
UCLASS()
class USCInterchangeBaseline : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="SC|Verification")
    static bool ImportGLB(const FString& Filename);
};
