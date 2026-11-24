#pragma once
#include "CoreMinimal.h"
#include "SCPackageSource.h"
#include "SCMeshes.h"
#include "SCImportLog.h"
#include "SCImport.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SCImporter.generated.h"
class ASCVehicle;
class SCBRIDGE_API FSCImporter
{
public:
    // Game-thread synchronous API over FSCImportJob (SCImportJob.h), which hosts drive incrementally.
    // Parsing, mesh derivation and pure Chaos BVH derivation run on workers.
    // Only publishes the new actor after all readable assets and resolvable attachments are assembled.
    static bool Import(FSCPackageSource& Source, FSCImportOptions Options, FSCImportResult& Result);
    /** Registers Vehicle as the world's current import, destroying the previous one when bReplacePrevious. */
    static void Publish(UWorld* World, ASCVehicle* Vehicle, bool bReplacePrevious);
    static void Unload(UWorld* World);
    static bool Verify(const FString& Folder, const FString& Tar, UWorld* World);
};
UCLASS()
class SCBRIDGE_API USCImportLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="SC|Verification")
    static bool RunUnitTests();
    UFUNCTION(BlueprintCallable, Category="SC|Verification")
    static bool VerifyRig(ASCVehicle* Vehicle, const FString& PoseReference = TEXT(""), FName PoseClip = NAME_None);
    /** ScenePath selects the package in a shared export root holding several (empty: the sole scene.json). */
    UFUNCTION(BlueprintCallable, Category="SC", meta=(WorldContext="WorldContext"))
    static ASCVehicle* ImportFolder(UObject* WorldContext, const FString& Folder, bool bBuildCollision = true, const FString& ScenePath = TEXT(""));
    /** An object container package from a (shared) export root as an environment: effect-only placements hidden,
     *  repeated parts instanced. ScenePath selects the package's scene.json in a root holding several. */
    UFUNCTION(BlueprintCallable, Category="SC", meta=(WorldContext="WorldContext"))
    static ASCVehicle* ImportEnvironment(UObject* WorldContext, const FString& Folder, const FString& ScenePath);
    /** ImportFolder and ImportEnvironment over any package source (WP-O1-4: the game's prep step imports the archive's
     *  export as it comes, without writing it to disk). */
    static ASCVehicle* ImportVehicleFrom(UWorld* World, FSCPackageSource& Source, bool bBuildCollision = true, const FString& ScenePath = TEXT(""));
    static ASCVehicle* ImportEnvironmentFrom(UWorld* World, FSCPackageSource& Source, const FString& ScenePath);
    UFUNCTION(BlueprintCallable, Category="SC", meta=(WorldContext="WorldContext"))
    static bool VerifyPackage(UObject* WorldContext, const FString& Folder, const FString& Tar);
    UFUNCTION(BlueprintCallable, Category="SC", meta=(WorldContext="WorldContext"))
    static void UnloadPackage(UObject* WorldContext);
};
