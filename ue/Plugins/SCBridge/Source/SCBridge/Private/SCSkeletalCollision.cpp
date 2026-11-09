// Worker-thread full-resolution collision in the same vertex order as the CPU skin buffer.
#include "SCSkeletonBuilder.h"
#include "SCRigComponent.h"
#include "StaticMeshAttributes.h"
#include "PhysicsEngine/BodySetup.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Chaos/ChaosArchive.h"
#include "Serialization/MemoryReader.h"

bool FSCSkeletalMeshImporter::DeriveCollision(FSCDerivedMesh& Data, FString& Error)
{
    // The skeletal builder emits one vertex per corner, grouped by material. Chaos vertex
    // indices must match ComputeSkinnedPositions exactly, including duplicate positions.
    FSCDerivedMesh Expanded;
    FStaticMeshAttributes Dst(Expanded.Description);
    Dst.Register();
    FStaticMeshConstAttributes Src(Data.Description);
    for (auto Group : Data.Description.PolygonGroups().GetElementIDs())
    {
        const auto G = Expanded.Description.CreatePolygonGroup();
        for (auto Triangle : Data.Description.GetPolygonGroupTriangles(Group))
        {
            TArray<FVertexInstanceID, TInlineAllocator<3>> Corners;
            for (auto Corner : Data.Description.GetTriangleVertexInstances(Triangle))
            {
                const auto V = Expanded.Description.CreateVertex();
                Dst.GetVertexPositions()[V] = Src.GetVertexPositions()[Data.Description.GetVertexInstanceVertex(Corner)];
                Corners.Add(Expanded.Description.CreateVertexInstance(V));
            }
            Expanded.Description.CreateTriangle(G, Corners);
        }
    }
    if (!FSCMeshImporter::DeriveCollision(Expanded, Error)) return false;
    Data.CollisionBytes = MoveTemp(Expanded.CollisionBytes);
    Data.CollisionVersions = Expanded.CollisionVersions;
    Data.CollisionDeriveSeconds = Expanded.CollisionDeriveSeconds;
    return true;
}
bool USCRigComponent::InitializeCollision(const FSCDerivedMesh& Data)
{
    if (Data.CollisionBytes.IsEmpty()) return false; // socket-only rigs have no surface
    CollisionBody = NewObject<UBodySetup>(this, NAME_None, RF_Transient);
    CollisionBody->CollisionTraceFlag = CTF_UseComplexAsSimple;
    CollisionBody->bDoubleSidedGeometry = true;
    FMemoryReader Reader(Data.CollisionBytes);
    Reader.SetCustomVersions(Data.CollisionVersions);
    Chaos::FChaosArchive Archive(Reader);
    Chaos::FTriangleMeshImplicitObjectPtr Geometry;
    Archive << Geometry;
    if (Reader.IsError() || !Geometry) { CollisionBody = nullptr; return false; }
    // A separate deformable BVH per component: shared meshes can have different poses.
    CollisionBody->TriMeshGeometries.Add(Geometry);
    CollisionBody->bCreatedPhysicsMeshes = true;
    CollisionBody->bHasCookedCollisionData = true;
    CollisionBody->bNeverNeedsCookedCollisionData = true;
    bEnablePerPolyCollision = true;
    SetCollisionProfileName(TEXT("BlockAll"));
    SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    return true;
}
UBodySetup* USCRigComponent::GetBodySetup()
{
    return CollisionBody ? CollisionBody.Get() : Super::GetBodySetup();
}
void USCRigComponent::OnCreatePhysicsState()
{
    if (!CollisionBody) { Super::OnCreatePhysicsState(); return; }
    // Use our worker-cooked body; the stock skeletal path would synchronously recook
    // the mesh and discard the full-resolution BVH. Queries still use UE per-poly skinning.
    UPrimitiveComponent::OnCreatePhysicsState();
}

FBodyInstance* USCRigComponent::GetBodyInstance(FName BoneName, bool bGetWelded, int32 Index) const
{
    return CollisionBody ? const_cast<FBodyInstance*>(&BodyInstance) : Super::GetBodyInstance(BoneName, bGetWelded, Index);
}
bool USCRigComponent::HasValidPhysicsState() const
{
    return CollisionBody ? BodyInstance.IsValidBodyInstance() : Super::HasValidPhysicsState();
}
