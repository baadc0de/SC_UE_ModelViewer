// SCRigAssets (SCRigAssets.h): a runtime rig mesh given an editor source model and the engine's build.
#include "SCRigAssets.h"
#include "SCMeshImporter.h"
#include "SCRigComponent.h"
#include "Animation/Skeleton.h"
#include "BoneWeights.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkeletalMeshAttributes.h"
#include "SkeletalMeshOperations.h"
#include "SkinnedAssetCompiler.h"
#include "StaticMeshAttributes.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCRigAssets, Log, All);

namespace
{
FName SlotName(int32 Slot)
{
    return FName(*FString::Printf(TEXT("Slot_%d"), Slot)); // FSCSkeletalMeshImporter's slot names
}
} // namespace

bool SCRigAssets::MakeSavable(USkeletalMesh* Mesh, const FSCDerivedMesh& Soft, FString& Error)
{
    if (!Mesh)
    {
        Error = TEXT("no rig mesh");
        return false;
    }
    const int32 Bones = Mesh->GetRefSkeleton().GetNum();
    // LOD 0's source: the soft triangles as derived, with skeletal attributes (skin weights) on top. A socket-only rig has
    // none: an empty description, its LOD keeps no geometry.
    FMeshDescription Description = Soft.bHasGeometry ? Soft.Description : FMeshDescription();
    FSkeletalMeshAttributes Attributes(Description);
    Attributes.Register(true);
    // Vertex colours as the runtime build stores them (FLinearColor::ToFColorSRGB): the engine's build converts without
    // gamma (ToFColor(false)), so the description carries the sRGB bytes as linear values.
    TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
    bool bAllBlack = true, bAllZero = true;
    for (const FVertexInstanceID Corner : Description.VertexInstances().GetElementIDs())
    {
        Colors[Corner] = FVector4f(FLinearColor(Colors[Corner]).ToFColorSRGB().ReinterpretAsLinear());
        bAllBlack &= FLinearColor(Colors[Corner]) == FLinearColor::Black;
        bAllZero &= Colors[Corner].IsNearlyZero();
    }
    // The runtime build always keeps the colours. The engine keeps them only when a corner is not opaque black (its LOD
    // build) and not near zero (its source model), and otherwise paints the mesh white: an all-black mask would light up.
    // One corner's red raised by 0.001 passes both tests and still stores byte 0 (0.001 * 255 rounds to 0).
    if (bAllBlack || bAllZero)
        for (const FVertexInstanceID Corner : Description.VertexInstances().GetElementIDs())
        {
            Colors[Corner].X += 0.001f;
            break;
        }
    TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
    for (const FPolygonGroupID Group : Description.PolygonGroups().GetElementIDs()) SlotNames[Group] = SlotName(Group.GetValue());
    FSkinWeightsVertexAttributesRef Weights = Attributes.GetVertexSkinWeights();
    for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
    {
        if (Description.GetVertexVertexInstanceIDs(Vertex).IsEmpty()) continue; // no triangle uses it
        const FSCVertexWeights* W = Soft.SkinWeights.Find(Vertex);
        if (!W)
        {
            Error = FString::Printf(TEXT("soft vertex %d has no weights"), Vertex.GetValue());
            return false;
        }
        TArray<UE::AnimationCore::FBoneWeight, TInlineAllocator<4>> Influences;
        for (int32 K = 0; K < 4; ++K)
        {
            if (W->Weights[K] <= 0.f) continue;
            const int32* Rig = Soft.GltfToRig.Find(W->Joints[K]);
            if (!Rig || *Rig + 1 >= Bones)
            {
                Error = FString::Printf(TEXT("soft vertex %d weights joint %d outside the skeleton"), Vertex.GetValue(), W->Joints[K]);
                return false;
            }
            Influences.Add(UE::AnimationCore::FBoneWeight(FBoneIndexType(*Rig + 1), W->Weights[K])); // SC_RigRoot is bone 0
        }
        if (Influences.IsEmpty()) Influences.Add(UE::AnimationCore::FBoneWeight(FBoneIndexType(0), 1.f));
        Weights.Set(Vertex, UE::AnimationCore::FBoneWeights::Create(Influences));
    }
    bool bInfluenceLimitHit = false;
    FSkeletalMeshOperations::ValidateAndFixInfluences(Description, bInfluenceLimitHit);
    // The bones, as the engine's conversion from a LOD model writes them (the raw reference skeleton in order).
    const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
    Attributes.ReserveNewBones(Ref.GetRawBoneNum());
    FSkeletalMeshAttributes::FBoneNameAttributesRef BoneNames = Attributes.GetBoneNames();
    FSkeletalMeshAttributes::FBoneParentIndexAttributesRef BoneParents = Attributes.GetBoneParentIndices();
    FSkeletalMeshAttributes::FBonePoseAttributesRef BonePoses = Attributes.GetBonePoses();
    for (int32 Bone = 0; Bone < Ref.GetRawBoneNum(); ++Bone)
    {
        const FBoneID Id = Attributes.CreateBone();
        BoneNames.Set(Id, Ref.GetRawRefBoneInfo()[Bone].Name);
        BoneParents.Set(Id, Ref.GetRawRefBoneInfo()[Bone].ParentIndex);
        BonePoses.Set(Id, Ref.GetRawRefBonePose()[Bone]);
    }

    // One imported LOD, built as authored: no reduction, no recomputed normals or tangents, full-precision UVs, and its
    // vertices and skin weights readable on the CPU (the rig's collision refit skins them there).
    FSkeletalMeshModel* Model = Mesh->GetImportedModel();
    Model->LODModels.Empty();
    Model->LODModels.Add(new FSkeletalMeshLODModel());
    if (Mesh->GetLODNum() == 0) Mesh->AddLODInfo();
    FSkeletalMeshLODInfo& Info = *Mesh->GetLODInfo(0);
    Info.bAllowCPUAccess = true;
    Info.ReductionSettings.NumOfTrianglesPercentage = 1.f;
    Info.ReductionSettings.NumOfVertPercentage = 1.f;
    Info.ReductionSettings.MaxDeviationPercentage = 0.f;
    Info.BuildSettings.bRecomputeNormals = false;
    Info.BuildSettings.bRecomputeTangents = false;
    Info.BuildSettings.bUseFullPrecisionUVs = true;
    Info.BuildSettings.bRemoveDegenerates = false;
    Info.BuildSettings.bUseHighPrecisionSkinWeights = true; // the runtime build's 16-bit weights
    Info.BuildSettings.BoneInfluenceLimit = 4;              // the runtime build's four influences, whatever the project default
    TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
    for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
    {
        Materials[Slot].MaterialSlotName = SlotName(Slot);
        Materials[Slot].ImportedMaterialSlotName = SlotName(Slot);
    }
    Mesh->NeverStream = true;
    Mesh->CreateMeshDescription(0, MoveTemp(Description));
    USkeletalMesh::FCommitMeshDescriptionParams Commit;
    Commit.bMarkPackageDirty = false;
    Mesh->CommitMeshDescription(0, Commit);
    Mesh->Build();
    FSkinnedAssetCompilingManager::Get().FinishCompilation(TArray<USkinnedAsset*>{Mesh});
    const FSkeletalMeshRenderData* Render = Mesh->GetResourceForRendering();
    if (!Render || Render->LODRenderData.IsEmpty())
    {
        Error = TEXT("the engine's build left no LOD");
        return false;
    }
    if (Mesh->GetRefSkeleton().GetNum() != Bones)
    {
        Error = FString::Printf(TEXT("the build changed the skeleton (%d bones, was %d): clip tracks index it"), Mesh->GetRefSkeleton().GetNum(), Bones);
        return false;
    }
    UE_LOG(LogSCRigAssets, Display, TEXT("RIG_ASSET mesh=%s bones=%d vertices=%d sections=%d influence_limit_hit=%d"), *Mesh->GetName(), Bones,
           Render->LODRenderData[0].GetNumVertices(), Render->LODRenderData[0].RenderSections.Num(), bInfluenceLimitHit);
    return true;
}

USkeletalMesh* SCRigAssets::SavableCopy(const USkeletalMesh* Mesh, const FSCDerivedMesh* Soft, FString& Error)
{
    if (!Mesh || !Mesh->GetSkeleton())
    {
        Error = TEXT("no rig mesh or skeleton");
        return nullptr;
    }
    USkeleton* Skeleton = DuplicateObject(Mesh->GetSkeleton(), GetTransientPackage());
    USkeletalMesh* Copy = nullptr;
    if (Soft)
    {
        Copy = NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Transient);
        Copy->SetRefSkeleton(Mesh->GetRefSkeleton());
        Copy->SetSkeleton(Skeleton);
        Copy->GetMaterials() = Mesh->GetMaterials();
        Copy->SetHasVertexColors(Mesh->GetHasVertexColors());
        Copy->SetImportedBounds(Mesh->GetImportedBounds());
        Copy->CalculateInvRefMatrices();
        if (!MakeSavable(Copy, *Soft, Error)) return nullptr;
        return Copy;
    }
    if (!Mesh->HasMeshDescription(0))
    {
        Error = TEXT("no rig source and no source model");
        return nullptr;
    }
    Copy = DuplicateObject(Mesh, GetTransientPackage());
    Copy->SetSkeleton(Skeleton);
    Copy->Build();
    FSkinnedAssetCompilingManager::Get().FinishCompilation(TArray<USkinnedAsset*>{Copy});
    const FSkeletalMeshRenderData* Render = Copy->GetResourceForRendering();
    if (!Render || Render->LODRenderData.IsEmpty())
    {
        Error = TEXT("the engine's build left no LOD");
        return nullptr;
    }
    return Copy;
}

bool SCRigAssets::DeriveCollision(const USkeletalMesh* Mesh, TArray<uint8>& OutBytes, FCustomVersionContainer& OutVersions, FString& Error)
{
    const FSkeletalMeshRenderData* Render = Mesh ? Mesh->GetResourceForRendering() : nullptr;
    if (!Render || Render->LODRenderData.IsEmpty())
    {
        Error = TEXT("no LOD 0");
        return false;
    }
    const FSkeletalMeshLODRenderData& LOD = Render->LODRenderData[0];
    const FPositionVertexBuffer& Positions = LOD.StaticVertexBuffers.PositionVertexBuffer;
    if (!LOD.GetNumVertices() || !Positions.GetVertexData())
    {
        Error = TEXT("LOD 0 has no CPU-readable vertices");
        return false;
    }
    // One description vertex per render vertex, in render order; the triangles of every section as the index buffer has
    // them, in a polygon group of the section's material slot (the trimesh keeps it per face: physical materials). The
    // derived trimesh then indexes the render vertices the refit skins.
    FSCDerivedMesh Expanded;
    FStaticMeshAttributes Attributes(Expanded.Description);
    Attributes.Register();
    TArray<FVertexID> Vertices;
    Vertices.Reserve(Positions.GetNumVertices());
    for (uint32 I = 0; I < Positions.GetNumVertices(); ++I)
    {
        const FVertexID V = Expanded.Description.CreateVertex();
        Attributes.GetVertexPositions()[V] = Positions.VertexPosition(I);
        Vertices.Add(V);
    }
    TArray<uint32> Indices;
    LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
    for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
    {
        const FPolygonGroupID Group(Section.MaterialIndex);
        while (!Expanded.Description.IsPolygonGroupValid(Group)) Expanded.Description.CreatePolygonGroup();
        for (uint32 Triangle = 0; Triangle < Section.NumTriangles; ++Triangle)
        {
            TArray<FVertexInstanceID, TInlineAllocator<3>> Corners;
            for (uint32 K = 0; K < 3; ++K)
            {
                const uint32 Index = Indices[Section.BaseIndex + Triangle * 3 + K];
                if (!Vertices.IsValidIndex(int32(Index)))
                {
                    Error = TEXT("an index outside LOD 0's vertices");
                    return false;
                }
                Corners.Add(Expanded.Description.CreateVertexInstance(Vertices[Index]));
            }
            Expanded.Description.CreateTriangle(Group, Corners);
        }
    }
    if (!FSCMeshImporter::DeriveCollision(Expanded, Error, /*bVertexIndexMap*/ true)) return false;
    OutBytes = MoveTemp(Expanded.CollisionBytes);
    OutVersions = Expanded.CollisionVersions;
    return true;
}

bool SCRigAssets::RefreshCollision(USCRigComponent* Rig, FString& Error)
{
    const USkeletalMesh* Mesh = Rig ? Rig->GetSkeletalMeshAsset() : nullptr;
    TArray<uint8> Bytes;
    FCustomVersionContainer Versions;
    if (!DeriveCollision(Mesh, Bytes, Versions, Error)) return false;
    const bool bHadBody = Rig->HasCollisionBody();
    // The import sets the pawn response after its own collision step, whether that built a body or not (a rig with the
    // game's colliders ignores the pawn): a first body keeps it.
    const ECollisionResponse Pawn = Rig->GetCollisionResponseToChannel(ECC_Pawn);
    Rig->SavedCollision.Reset();
    if (!Rig->InitializeCollision(Bytes, Versions, /*bConfigure*/ !bHadBody))
    {
        Error = TEXT("the collision body does not build");
        return false;
    }
    if (!bHadBody && Pawn == ECR_Ignore) Rig->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
    if (Rig->IsRegistered()) Rig->RecreatePhysicsState();
    return true;
}
