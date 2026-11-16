// RPC harness (WP-H1): a localhost HTTP/JSON server for scripted, exploratory testing of the running viewer (load and
// spawn ships, add and remove cargo, transfers, camera, screenshots, console), and a state log: NDJSON records of the
// scene (vehicles, grids, boxes with pose / velocity, drones with beam forces, jobs), activity, idle / settled events,
// warnings and anomalies, rotated by size. Off unless -SCRpc[=port] or -SCStateLog[=path]; absent from Shipping and Wasm
// builds (SC_WITH_RPC). The API and the log's records are described in docs/work-packages/WP-H1-rpc-harness.md.
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SCRpcSubsystem.generated.h"

UCLASS()
class SCBRIDGE_API USCRpcSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

private:
    TSharedPtr<class FSCRpcServer> Server;
};
