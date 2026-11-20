#pragma once
#include "SCRpcJson.h"

class UWorld;
namespace SCGarageRpc
{
SCRpcJson::FObj Snapshot(UWorld* World);
SCRpcJson::FObj Command(UWorld* World, const FString& Route, const TSharedPtr<FJsonObject>& Body);
}
