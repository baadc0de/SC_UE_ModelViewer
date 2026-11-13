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
    /** false: the actor is neither registered as the world's current import nor given exploration data
     *  (player kits are owned by the viewer subsystem, not the vehicle slot). */
    bool bPublish = true;
    /** true: every mesh with skin weights becomes a rig even without a binding clip (player kits are posed by
     *  USCPlayerBodyComponent, not by the clip catalogue). */
    bool bRigSkinnedMeshes = false;
    /** false: clip sidecars are not read or bound (player kits sample body clips themselves). */
    bool bImportClips = true;
    /** Environments: placements drawn only with VFX shaders (e.g. a hangar's landing hologram) are runtime state
     *  effects in the game (shown while a ship is delivered); they are not placed. */
    bool bHideEffectOnlyPlacements = false;
    /** Environments: identical rigid static parts (same mesh, materials, shadow/lighting flags, collision, channels and
     *  custom data, not under a rig) are drawn as one instanced component each; the render thread's per-primitive
     *  visibility cost dominated a hangar's frame (LOOK-6). Ships keep per-part components (hierarchy, focus). */
    bool bInstanceRepeatedParts = false;
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
    /** An object container package from a (shared) export root as an environment: effect-only placements hidden,
     *  repeated parts instanced. ScenePath selects the package's scene.json in a root holding several. */
    UFUNCTION(BlueprintCallable, Category="SC", meta=(WorldContext="WorldContext"))
    static ASCVehicle* ImportEnvironment(UObject* WorldContext, const FString& Folder, const FString& ScenePath);
    /** The door fragments' presented pose ("open", "closed" or "bind"), then the ship floor moved to FloorZ. */
    UFUNCTION(BlueprintCallable, Category="SC")
    static double PresentEnvironment(ASCVehicle* Environment, const FString& State = TEXT("open"), double FloorZ = -5.0);
    UFUNCTION(BlueprintCallable, Category="SC", meta=(WorldContext="WorldContext"))
    static bool VerifyPackage(UObject* WorldContext, const FString& Folder, const FString& Tar);
    UFUNCTION(BlueprintCallable, Category="SC", meta=(WorldContext="WorldContext"))
    static void UnloadPackage(UObject* WorldContext);
};
DECLARE_LOG_CATEGORY_EXTERN(LogSCImport, Log, All);
