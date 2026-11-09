// Worker-thread full-resolution collision in the same vertex order as the CPU skin buffer, and its
// per-instance refit from the CPU skinned pose.
#include "SCSkeletonBuilder.h"
#include "SCRigComponent.h"
#include "StaticMeshAttributes.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "Physics/PhysicsInterfaceCore.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Chaos/ImplicitObjectScaled.h"
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
    if (!FSCMeshImporter::DeriveCollision(Expanded, Error, /*bVertexIndexMap*/ true)) return false;
    Data.CollisionBytes = MoveTemp(Expanded.CollisionBytes);
    Data.CollisionVersions = Expanded.CollisionVersions;
    Data.CollisionDeriveSeconds = Expanded.CollisionDeriveSeconds;
    return true;
}
bool USCRigComponent::InitializeCollision(const FSCDerivedMesh& Data)
{
    if (Data.CollisionBytes.IsEmpty()) return false; // socket-only rigs have no surface
    const USkeletalMesh* Mesh = GetSkeletalMeshAsset();
    const FSkeletalMeshRenderData* Render = Mesh ? Mesh->GetResourceForRendering() : nullptr;
    if (!Render || Render->LODRenderData.IsEmpty()) return false;
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
    // The cooked particles are the bind pose; only bones that weight a vertex can deform it.
    const FSkeletalMeshLODRenderData& LOD = Render->LODRenderData[0];
    const auto& Ref = Mesh->GetRefSkeleton();
    TBitArray<> Used(false, Ref.GetNum());
    for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
        for (uint32 V = 0; V < Section.NumVertices; ++V)
        {
            const FSkinWeightInfo W = LOD.SkinWeightVertexBuffer.GetVertexSkinWeights(Section.BaseVertexIndex + V);
            for (int32 K = 0; K < MAX_TOTAL_INFLUENCES; ++K)
                if (W.InfluenceWeights[K] && Section.BoneMap.IsValidIndex(W.InfluenceBones[K]))
                    Used[Section.BoneMap[W.InfluenceBones[K]]] = true;
        }
    SkinBones.Reset();
    for (TConstSetBitIterator<> It(Used); It; ++It) SkinBones.Add(It.GetIndex());
    RefitPose.SetNum(Ref.GetNum());
    for (int32 I = 0; I < Ref.GetNum(); ++I)
    {
        const int32 Parent = Ref.GetParentIndex(I);
        RefitPose[I] = Parent == INDEX_NONE ? Ref.GetRefBonePose()[I] : Ref.GetRefBonePose()[I] * RefitPose[Parent];
    }
    bEnablePerPolyCollision = true;
    SetCollisionProfileName(TEXT("BlockAll"));
    SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    UE_LOG(LogTemp, Display, TEXT("SKIN_COLLISION owner=%s vertices=%u skin_bones=%d"), *OwnerEntity, LOD.GetNumVertices(), SkinBones.Num());
    return true;
}
bool USCRigComponent::SkinPoseChanged() const
{
    const TArray<FTransform>& Space = GetComponentSpaceTransforms();
    for (int32 Bone : SkinBones)
        if (!Space.IsValidIndex(Bone) || !RefitPose.IsValidIndex(Bone) || !Space[Bone].Equals(RefitPose[Bone], 1.e-4))
            return true;
    return false;
}
bool USCRigComponent::RefitCollision()
{
    // Before the body exists (registration, InitAnim) there is nothing to refit: OnCreatePhysicsState
    // catches up with the pose present at that time.
    if (!CollisionBody || bRefitUnavailable || !BodyInstance.IsValidBodyInstance() || !SkinPoseChanged()) return false;
    const USkeletalMesh* Mesh = GetSkeletalMeshAsset();
    const FSkeletalMeshRenderData* Render = Mesh ? Mesh->GetResourceForRendering() : nullptr;
    const FSkinWeightVertexBuffer* Weights = GetSkinWeightBuffer(0);
    if (!Render || Render->LODRenderData.IsEmpty() || !Weights) return false;
    const FSkeletalMeshLODRenderData& LOD = Render->LODRenderData[0];
    // CPU skinning reads the CPU copies kept by FSCSkeletalMeshImporter::Build. Refuse loudly
    // (once) instead of reading released buffers if a host dropped them.
    if (!LOD.StaticVertexBuffers.PositionVertexBuffer.GetVertexData() || !Weights->GetNeedsCPUAccess() ||
        !const_cast<FSkinWeightVertexBuffer*>(Weights)->GetDataVertexBuffer()->GetWeightData())
    {
        bRefitUnavailable = true;
        UE_LOG(LogTemp, Warning, TEXT("SKIN_COLLISION owner=%s refit unavailable: no CPU skin data"), *OwnerEntity);
        return false;
    }
    const double Start = FPlatformTime::Seconds();
    TArray<FMatrix44f> RefToLocals;
    TArray<FVector3f> Positions;
    CacheRefToLocalMatrices(RefToLocals);
    ComputeSkinnedPositions(this, Positions, RefToLocals, LOD, *Weights);
    BodyInstance.UpdateTriMeshVertices(UE::LWC::ConvertArrayType<FVector>(Positions));
    RefitPose = GetComponentSpaceTransforms();
    ++CollisionRefits;
    UE_LOG(LogTemp, Verbose, TEXT("SKIN_COLLISION_REFIT owner=%s vertices=%d seconds=%.4f"), *OwnerEntity, Positions.Num(),
           FPlatformTime::Seconds() - Start);
    return true;
}
bool USCRigComponent::ReadCollisionVertices(TArray<FVector3f>& Out) const
{
    Out.Reset();
    if (!CollisionBody || !BodyInstance.IsValidBodyInstance()) return false;
    FPhysicsCommand::ExecuteRead(BodyInstance.GetPhysicsActor(), [&](const FPhysicsActorHandle&) {
        TArray<FPhysicsShapeHandle> Shapes;
        BodyInstance.GetAllShapes_AssumesLocked(Shapes);
        for (const FPhysicsShapeHandle& Shape : Shapes)
        {
            using namespace Chaos;
            const FImplicitObject* Implicit = Shape.Shape->GetGeometry();
            const FTriangleMeshImplicitObject* Mesh = nullptr;
            if (IsInstanced(Implicit->GetType()))
                Mesh = Implicit->GetObjectChecked<TImplicitObjectInstanced<FTriangleMeshImplicitObject>>().GetInstancedObject();
            else if (IsScaled(Implicit->GetType()))
                Mesh = Implicit->GetObjectChecked<TImplicitObjectScaled<FTriangleMeshImplicitObject>>().GetUnscaledObject();
            else if (Implicit->GetType() == ImplicitObjectType::TriangleMesh)
                Mesh = &Implicit->GetObjectChecked<FTriangleMeshImplicitObject>();
            if (!Mesh) continue;
            // The identity external->particle map (DeriveCollision) makes particle i external vertex i.
            const auto& Particles = Mesh->Particles();
            Out.Reserve(Particles.Size());
            for (uint32 I = 0; I < Particles.Size(); ++I) Out.Add(FVector3f(Particles.GetX(I)));
            break;
        }
    });
    return !Out.IsEmpty();
}
UBodySetup* USCRigComponent::GetBodySetup()
{
    return CollisionBody ? CollisionBody.Get() : Super::GetBodySetup();
}
void USCRigComponent::OnCreatePhysicsState()
{
    if (!CollisionBody) { Super::OnCreatePhysicsState(); return; }
    // Use our worker-cooked body; the stock skeletal path would synchronously recook the mesh and
    // discard the full-resolution BVH. A new body starts from the cooked bind pose: catch up.
    UPrimitiveComponent::OnCreatePhysicsState();
    const auto& Ref = GetSkeletalMeshAsset()->GetRefSkeleton();
    RefitPose.SetNum(Ref.GetNum());
    for (int32 I = 0; I < Ref.GetNum(); ++I)
    {
        const int32 Parent = Ref.GetParentIndex(I);
        RefitPose[I] = Parent == INDEX_NONE ? Ref.GetRefBonePose()[I] : Ref.GetRefBonePose()[I] * RefitPose[Parent];
    }
    RefitCollision();
}
void USCRigComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
    if (!CollisionBody) { Super::OnUpdateTransform(UpdateTransformFlags, Teleport); return; }
    // The skeletal per-poly path only moves the body when a render mesh object exists (not under
    // null RHI). Move our single body like any primitive, identically in every host.
    Super::OnUpdateTransform(UpdateTransformFlags | EUpdateTransformFlags::SkipPhysicsUpdate, Teleport);
    if (IsPhysicsStateCreated() && !(UpdateTransformFlags & EUpdateTransformFlags::SkipPhysicsUpdate))
        BodyInstance.SetBodyTransform(GetComponentTransform(), Teleport);
}
FBodyInstance* USCRigComponent::GetBodyInstance(FName BoneName, bool bGetWelded, int32 Index) const
{
    return CollisionBody ? const_cast<FBodyInstance*>(&BodyInstance) : Super::GetBodyInstance(BoneName, bGetWelded, Index);
}
bool USCRigComponent::HasValidPhysicsState() const
{
    return CollisionBody ? BodyInstance.IsValidBodyInstance() : Super::HasValidPhysicsState();
}
