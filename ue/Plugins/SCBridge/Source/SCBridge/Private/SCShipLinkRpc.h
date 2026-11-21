// WP-M1 Ship Link routes and the state shared with the H1 journal.
#pragma once
#include "SCRpcJson.h"

class UWorld;
namespace SCShipLinkRpc
{
SCRpcJson::FObj Snapshot(UWorld* World);
SCRpcJson::FObj Command(UWorld* World, const FString& Route, const TSharedPtr<FJsonObject>& Body);
void Scan(UWorld* World, TSet<FString>& Found);
}
