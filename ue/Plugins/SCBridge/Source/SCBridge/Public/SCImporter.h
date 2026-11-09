#pragma once
#include "CoreMinimal.h"
#include "SCPackageSource.h"
#include "SCMeshImporter.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SCImporter.generated.h"
class ASCVehicle;
struct SCBRIDGE_API FSCImportOptions
{
    UWorld* World = nullptr;
    FString ScenePath;
    bool bBuildCollision = true;
    bool bImportMaterials = true; // false keeps WP-U1's placeholder (timing baseline / geometry-only checks)
    bool bReplacePrevious = true;
    FTransform ActorTransform = FTransform::Identity;
};
struct SCBRIDGE_API FSCImportResult
{
    TWeakObjectPtr<ASCVehicle> Vehicle;
    FString Error;
    TArray<FString> Warnings; // Each recoverable diagnostic includes its asset/instance path.
    TMap<FString, FSCMeshBuildStats> MeshStats;
    TMap<FString, FString> ContentHashes;
    double TotalSeconds = 0, MaterialSeconds = 0;
    int32 MeshCount = 0, InstanceCount = 0, TextureCount = 0, MaterialCount = 0;
};
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
    UFUNCTION(BlueprintCallable, Category="SC", meta=(WorldContext="WorldContext"))
    static ASCVehicle* ImportFolder(UObject* WorldContext, const FString& Folder, bool bBuildCollision = true);
    UFUNCTION(BlueprintCallable, Category="SC", meta=(WorldContext="WorldContext"))
    static bool VerifyPackage(UObject* WorldContext, const FString& Folder, const FString& Tar);
    UFUNCTION(BlueprintCallable, Category="SC", meta=(WorldContext="WorldContext"))
    static void UnloadPackage(UObject* WorldContext);
};
DECLARE_LOG_CATEGORY_EXTERN(LogSCImport, Log, All);
