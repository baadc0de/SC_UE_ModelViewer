#pragma once
#include "SCRpcJson.h"

class UWorld;
namespace SCGarageRpc
{
SCRpcJson::FObj Snapshot(UWorld* World, const FString& Entity = {}, bool bIncludeDefaultStored = false);
SCRpcJson::FObj Command(UWorld* World, const FString& Route, const TSharedPtr<FJsonObject>& Body);
}
