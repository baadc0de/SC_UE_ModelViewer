// WP-O1-2: what an application adds to the RPC harness (WP-H1, USCRpcSubsystem): its routes, its sections of /state and
// the state log, what keeps the scene busy or moving, the anomalies it scans for, its /catalog entries and its /wait
// conditions. UnrealCitizen registers cargo, Ship Link and Garage this way; the harness itself knows none of them.
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UWorld;

/** The harness, as a route sees it. */
class ISCRpcHost
{
public:
    using FReply = TFunction<void(const TSharedPtr<FJsonObject>& Body)>;
    using FPoll = TFunction<TSharedPtr<FJsonObject>(bool bTimedOut)>;
    virtual ~ISCRpcHost() = default;
    virtual UWorld* World() const = 0;
    /** Answer the request being routed later, instead of through its Reply: Poll runs every tick until it returns a body
     *  (bTimedOut on its last run, after TimeoutS). The request keeps the scene busy ("rpc:<Path>") meanwhile. Call it
     *  from the route itself. */
    virtual void Defer(const FString& Path, double TimeoutS, FPoll Poll) = 0;
};

class ISCRpcExtension
{
public:
    /** A route: Body is the request's JSON; Reply answers it, or the route defers its answer (Host.Defer). A body with
     *  "ok": false is an error. */
    using FRoute = TFunction<void(const TSharedPtr<FJsonObject>& Body, ISCRpcHost& Host, const ISCRpcHost::FReply& Reply)>;
    virtual ~ISCRpcExtension() = default;
    /** Its routes by path ("/cargo/spawn"). */
    virtual void Routes(TMap<FString, FRoute>& Out) = 0;
    /** Its sections of the scene snapshot (/state and the log's records). */
    virtual void Snapshot(UWorld* World, FJsonObject& State) const {}
    /** What keeps the scene from idle (Reasons) and how fast its things move (cm/s, for settling). */
    virtual void Activity(UWorld* World, TArray<FString>& Reasons, double& MaxSpeedCmS) const {}
    /** Anomalies in a snapshot (its own sections or the world): keys of what is wrong now. */
    virtual void Scan(UWorld* World, const FJsonObject& State, TSet<FString>& Found) const {}
    /** Its entries of /catalog. */
    virtual void Catalog(UWorld* World, FJsonObject& Out) const {}
    /** /wait until=Until: false when this extension does not answer that condition; else bReached (or OutError) and the
     *  fields to add to the answer. */
    virtual bool Wait(UWorld* World, const FString& Until, const TSharedPtr<FJsonObject>& Body, bool& bReached, FJsonObject& Out,
                      FString& OutError) const { return false; }
};

namespace SCRpcExtensions
{
/** Register before the harness starts (module startup); routes are bound when its HTTP server starts. */
SCBRIDGE_API void Add(FName Name, TSharedRef<ISCRpcExtension> Extension);
SCBRIDGE_API void Remove(FName Name);
SCBRIDGE_API TArray<TSharedRef<ISCRpcExtension>> All();
} // namespace SCRpcExtensions
