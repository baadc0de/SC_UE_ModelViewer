#pragma once
#include "CoreMinimal.h"
#include "MeshDescription.h"
#include "Serialization/CustomVersion.h"
#include "SCPackageSource.h"

class UStaticMesh;
class UMaterialInterface;
struct SCBRIDGE_API FSCRigNode
{
    FString SourceName;
    int32 Parent = INDEX_NONE;
    int32 GltfIndex = INDEX_NONE;
    FTransform Local = FTransform::Identity;
    FMatrix World = FMatrix::Identity;
    bool bAnimate = true;
};
struct SCBRIDGE_API FSCVertexWeights
{
    int32 Joints[4] = {0,0,0,0}; // GLB node indices until skeleton remap
    float Weights[4] = {1,0,0,0};
};
/** Pure CPU data: safe to derive on a worker, cache without retaining UObjects. */
struct SCBRIDGE_API FSCDerivedMesh
{
    FMeshDescription Description;
    TArray<FSCRigNode> RigNodes; // parent-first, includes the coordinate wrapper
    TMap<int32, int32> GltfToRig;
    TMap<FTriangleID, int32> TriangleNodes;
    TMap<FVertexID, FSCVertexWeights> SkinWeights;
    int32 SocketNode = INDEX_NONE; // split meshes retain world-baked vertices, inverse bind on component

    TArray<FString> MaterialNames;
    TArray<FString> MaterialSourceNames, MaterialFamilies, Warnings;
    TSet<int32> BlendedSlots;
    bool bEnableNanite = true;
    bool bHasGeometry = false;
    TMap<FString, FMatrix> Nodes; // glTF world matrices, before host conversion
    FMatrix RootMatrix = FMatrix::Identity;
    FMatrix AttachmentRootMatrix = FMatrix::Identity;
    FString ContentHash;
    TArray<uint8> CollisionBytes; // Serialized Chaos BVH, no UObject references.
    FCustomVersionContainer CollisionVersions;
    double CollisionDeriveSeconds = 0;
    int32 DegenerateTriangles = 0;
    bool bHasTangents = true;
    bool bHasSkins = false;
};
struct SCBRIDGE_API FSCMeshBuildStats
{
    bool bNaniteEnabled = false;
    FString NaniteReason;
    int32 Triangles = 0, FallbackTriangles = 0, CollisionTriangles = 0, Clusters = 0, Cards = 0;
    bool bMeshCards = false, bDistanceField = false, bRayTracingGeometry = false, bCollision = false;
    double BuildSeconds = 0, RepresentationSeconds = 0, CollisionSeconds = 0;
};
class SCBRIDGE_API FSCMeshImporter
{
public:
    static constexpr uint32 DerivedFormatVersion = 3; // Include engine version/build settings in a processed-cache key.
    static bool Derive(const FSCPackageSource& Source, const FString& Path, FSCDerivedMesh& Out, FString& Error);
    static FSCDerivedMesh Select(const FSCDerivedMesh& Data, const TArray<FTriangleID>& Triangles);
    static void SplitTranslucent(FSCDerivedMesh& Data, const TSet<int32>& Slots, FSCDerivedMesh& Translucent);
    /** Worker-thread Chaos BVH. bVertexIndexMap stores the identity external->particle map that
     *  FTriangleMeshImplicitObject::UpdateVertices requires (refittable skinned collision only). */
    static bool DeriveCollision(FSCDerivedMesh& Data, FString& Error, bool bVertexIndexMap = false);
    static UStaticMesh* Instantiate(const FSCDerivedMesh& Data, UObject* Outer, FName Name,
        bool bBuildCollision, UMaterialInterface* Material, FSCMeshBuildStats& Stats, FString& Error);
    // glTF (right, up, backward), metres -> UE (forward, right, up), centimetres.
    static FVector Position(const FVector& V) { return FVector(-V.Z, V.X, V.Y) * 100.0; }
    static FMatrix Transform(const FMatrix& Gltf);
    static FMatrix CryToGltf();
    static FMatrix InstanceTransform(const FMatrix& Local, const FMatrix& AssetRoot, bool bSourceFrame, bool bCryBasis);
};
