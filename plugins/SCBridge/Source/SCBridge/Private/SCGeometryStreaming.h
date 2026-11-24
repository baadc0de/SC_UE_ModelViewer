// Coarse render meshes and the view-driven queue for their full-detail replacements.
#pragma once
#include "CoreMinimal.h"
#include "SCMeshes.h"
#include "UObject/StrongObjectPtrTemplates.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include <atomic>

class ASCVehicle;
class FSCProcessedCache;
class UStaticMeshComponent;
class UWorld;

class FSCGeometryStreaming
{
public:
    ~FSCGeometryStreaming();
    /** Pure worker operation. Failure means the caller uses the exact variant as usual. */
    bool Reduce(const FSCDerivedMesh& Fine, FSCDerivedMesh& Coarse, double& Deviation) const;
    bool CanDefer(const FSCDerivedMesh& Fine) const;
    void Defer(ASCVehicle* Owner, UStaticMesh* Proxy, FSCDerivedMesh&& Fine, FString Label, double Deviation,
        bool bCollision, TSharedPtr<FSCProcessedCache, ESPMode::ThreadSafe> Cache, const FString& Key);
    void Attach(ASCVehicle* Owner);
    void QueueWrite(ASCVehicle* Owner, UStaticMesh* Mesh, const FSCDerivedMesh& Data, const FSCMeshBuildStats& Stats,
        TSharedPtr<FSCProcessedCache, ESPMode::ThreadSafe> Cache, const FString& Key);
    void Forget(ASCVehicle* Owner);
    void Tick(UWorld* World);
    void FlushForCapture(UWorld* World);
    void FinishWrites(UWorld* World);
    void PublishWrites();
    void DropWrites();
    void LogStatus(UWorld* World) const;

    /** Relative to the horizontal FOV. The room's own meshes can prefetch behind the player. */
    static double Priority(const FBox& Bounds, const FVector& Eye, const FVector& Forward,
        double Fov, double Deviation, bool bInsideRoom, bool bRendered);
private:
    struct FPending
    {
        TWeakObjectPtr<ASCVehicle> Owner;
        TWeakObjectPtr<UStaticMesh> Proxy;
        FSCDerivedMesh Fine;
        FString Label, Key;
        double Deviation = 0;
        bool bCollision = false, bAttached = false;
        TArray<TPair<TWeakObjectPtr<UStaticMeshComponent>, FString>> Components;
        TSharedPtr<FSCProcessedCache, ESPMode::ThreadSafe> Cache;
    };
    struct FWrite
    {
        TWeakObjectPtr<ASCVehicle> Owner;
        TStrongObjectPtr<UStaticMesh> Mesh;
        FString Key;
        FSCDerivedMesh Collision;
        FSCMeshBuildStats Stats;
        TSharedPtr<FSCProcessedCache, ESPMode::ThreadSafe> Cache;
    };
    struct FFineBuild
    {
        FPending Item;
        TStrongObjectPtr<UStaticMesh> Mesh;
        UE::Tasks::FTask SourceCommit;
        FSCMeshBuildStats Stats;
        double Started = 0;
        bool bSubmitted = false;
    };

    void BeginPromotion(int32 Index);
    void SubmitPromotion();
    void FinishPromotion(bool bWait);
    void WriteOne();
    TArray<FPending> Pending;
    TOptional<FFineBuild> InFlight;
    TArray<FWrite> Writes;
    TSet<TSharedPtr<FSCProcessedCache, ESPMode::ThreadSafe>> DirtyCaches;
    std::atomic<int32> PendingTriangles{0};
    int32 Promoted = 0, Failed = 0, Written = 0;
    FVector LastEye = FVector::ZeroVector;
    FRotator LastOrientation = FRotator::ZeroRotator;
    double LastFlush = 0, LastCameraMotion = 0;
};
