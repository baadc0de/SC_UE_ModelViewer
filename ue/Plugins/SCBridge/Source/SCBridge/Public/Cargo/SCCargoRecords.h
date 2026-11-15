// Cargo records of a decomposed scene.json (WP-K1, K1a contract): `inventory_containers[]` on the vehicle and in
// interiors, `children[].port_path`, and the root item's inventory occupancy. Called by FSCPackageReader.
#pragma once
#include "CoreMinimal.h"
#include "Cargo/SCCargoTypes.h"

class FJsonObject;
struct FSCSceneManifest;

namespace SCCargoRecords
{
/** Port names root -> item (`port_path`), empty when absent. */
SCBRIDGE_API TArray<FString> PortPath(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key = TEXT("port_path"));
/** The resolved `container` object (an InventoryContainer record), verbatim; false when it is not an object. */
SCBRIDGE_API bool ParseContainer(const TSharedPtr<FJsonObject>& Object, FSCCargoContainerParams& Out, FString& Error);
/** `root_entity` occupancy fields; Out.bValid when the item has an inventory occupancy. */
SCBRIDGE_API void ParseOccupancy(const TSharedPtr<FJsonObject>& Root, FSCItemOccupancy& Out);
/** Owner's `inventory_containers[]` (the scene object: vehicle entries, INDEX_NONE; or interior Interior, its index in
 *  Out.Interiors) appended to Out.InventoryContainers. Warn(path, message). */
SCBRIDGE_API void AddContainers(const TSharedPtr<FJsonObject>& Owner, int32 Interior, FSCSceneManifest& Out,
                                TFunctionRef<void(const FString&, const FString&)> Warn);
/** "a/b/c" key of a port path (map keys on ASCVehicle). */
inline FString PortKey(const TArray<FString>& Path) { return FString::Join(Path, TEXT("/")); }
}
