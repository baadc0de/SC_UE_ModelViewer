// WP-O1-4b (the WP-U3 follow-up): a runtime rig mesh (FSCSkeletonBuilder + FSCSkeletalMeshImporter: render data only, no
// editor source model) made into a skeletal mesh that saves and loads as an asset — the prep step keeps a vehicle's rigs
// instead of baking them static, and Keep as assets keeps them.
#pragma once
#include "CoreMinimal.h"

class USkeletalMesh;
class USCRigComponent;
struct FSCDerivedMesh;

namespace SCRigAssets
{
/** Give Mesh an editor source model (LOD 0's mesh description from Soft: its triangles, normals, tangents, UVs, colours,
 *  and per-vertex skin weights to the rig's bones, SC_RigRoot + 1 + rig node like the runtime build) and rebuild it with
 *  the engine's skeletal mesh build. The reference skeleton, sockets, material slots and bone order (what clip tracks
 *  index) stay; LOD 0 keeps CPU-readable vertices and skin weights (USCRigComponent::RefitCollision). A socket-only rig
 *  (no soft geometry) gets an empty LOD 0. The description carries the reference skeleton's bones (as the engine's own
 *  source models do), so it is never empty: it saves, and a load reads it instead of recovering one. False with Error
 *  when the soft data cannot be described (a vertex without weights, a joint outside the skeleton) or the build leaves
 *  no LOD. */
bool MakeSavable(USkeletalMesh* Mesh, const FSCDerivedMesh& Soft, FString& Error);
/** A savable copy of a rig mesh with a copy of its skeleton (transient, in the transient package), Mesh left as it is
 *  (Keep as assets keeps copies). A runtime mesh (Soft: the import's rig source) is a new mesh with its reference
 *  skeleton, material slots and bounds, made savable from Soft (it is not duplicated: a duplicate of a mesh without a
 *  source model runs the editor's load fix-ups on LOD models it lacks); an asset (Soft null: it has a source model) is
 *  duplicated and built. Null with Error when it cannot be made. */
USkeletalMesh* SavableCopy(const USkeletalMesh* Mesh, const FSCDerivedMesh* Soft, FString& Error);
/** The rig's collision trimesh (serialized Chaos bytes and their custom versions) from Mesh's LOD 0 render data, in its
 *  vertex order (USCRigComponent::RefitCollision skins those vertices): the engine's build may reorder or weld the
 *  vertices the runtime build laid out per corner. False with Error when there is no geometry or no CPU copy. */
bool DeriveCollision(const USkeletalMesh* Mesh, TArray<uint8>& OutBytes, FCustomVersionContainer& OutVersions, FString& Error);
/** After MakeSavable: Rig's collision body derived again from its (built) mesh and put in place (saved with the rig), its
 *  physics state recreated. A rig that had a body keeps its collision settings; one without (its import-time collision
 *  failed) gets the import's policy. False with Error when the mesh has no geometry or the body does not build. */
bool RefreshCollision(USCRigComponent* Rig, FString& Error);
} // namespace SCRigAssets
