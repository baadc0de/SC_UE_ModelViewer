#include "SCSkeletonBuilder.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "ReferenceSkeleton.h"
#include "StaticMeshAttributes.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Materials/Material.h"

FName FSCSkeletonBuilder::BoneName(const FSCDerivedMesh& Data, int32 Node)
{
    FString Name = Data.RigNodes[Node].SourceName;
    // glTF permits duplicate names, UE does not. Preserve source names separately for CRC routing.
    for (int32 I = 0; I < Node; ++I)
        if (Data.RigNodes[I].SourceName.Equals(Name, ESearchCase::IgnoreCase))
            return FName(*FString::Printf(TEXT("%s__node_%d"), *Name, Data.RigNodes[Node].GltfIndex));
    return FName(*Name);
}
USkeletalMesh* FSCSkeletonBuilder::Build(const FSCDerivedMesh& Data, UObject* Outer, USkeleton*& Skeleton)
{
    Skeleton = NewObject<USkeleton>(Outer, NAME_None, RF_Transient);
    auto* Mesh = NewObject<USkeletalMesh>(Outer, NAME_None, RF_Transient);
    FReferenceSkeleton Ref;
    {
    FReferenceSkeletonModifier Modify(Ref, Skeleton);
    Modify.Add(FMeshBoneInfo(TEXT("SC_RigRoot"), TEXT("SC_RigRoot"), INDEX_NONE), FTransform::Identity);
    for (int32 I = 0; I < Data.RigNodes.Num(); ++I)
    {
        const auto& Node = Data.RigNodes[I]; const FName Name = BoneName(Data, I);
        Modify.Add(FMeshBoneInfo(Name, Name.ToString(), Node.Parent + 1), Node.Local);
        auto* Socket = NewObject<USkeletalMeshSocket>(Skeleton, NAME_None, RF_Transient);
        Socket->SocketName = Name; Socket->BoneName = Name; Skeleton->Sockets.Add(Socket);
    }
    } // Finalize reference arrays before copying/merging.
    Mesh->SetRefSkeleton(Ref); Mesh->SetSkeleton(Skeleton);
    Skeleton->MergeAllBonesToBoneTree(Mesh); Mesh->CalculateInvRefMatrices();
    return Mesh;
}
bool FSCSkeletalMeshImporter::Build(const FSCDerivedMesh& Data, USkeletalMesh* Mesh, FString& Error)
{
    Mesh->AllocateResourceForRendering();
    auto* Render = Mesh->GetResourceForRendering();
    Render->NumInlinedLODs = 1; Render->NumNonOptionalLODs = 1;
    Render->CurrentFirstLODIdx = 0; Render->PendingFirstLODIdx = 0;
    Mesh->NeverStream = true;
    auto* LOD = new FSkeletalMeshLODRenderData(); Render->LODRenderData.Add(LOD);
    Mesh->AddLODInfo();
    for (int32 I = 0; I < Mesh->GetRefSkeleton().GetNum(); ++I)
    { LOD->RequiredBones.Add(I); LOD->ActiveBoneIndices.Add(I); }
    for (int32 Slot = 0; Slot < Data.MaterialNames.Num(); ++Slot)
        Mesh->GetMaterials().Add(FSkeletalMaterial(UMaterial::GetDefaultMaterial(MD_Surface), true, false,
            FName(*FString::Printf(TEXT("Slot_%d"), Slot))));
    TArray<FStaticMeshBuildVertex> Vertices;
    TArray<FSkinWeightInfo> Weights;
    TArray<uint32> Indices;
    FBox Bounds(ForceInit);
    FStaticMeshConstAttributes A(Data.Description);
    for (auto Group : Data.Description.PolygonGroups().GetElementIDs())
    {
        FSkelMeshRenderSection Section; Section.MaterialIndex = Group.GetValue();
        Section.BaseIndex = Indices.Num(); Section.BaseVertexIndex = Vertices.Num();
        Section.BoneMap = LOD->RequiredBones; Section.MaxBoneInfluences = 4;
        for (auto Triangle : Data.Description.GetPolygonGroupTriangles(Group))
        {
            for (auto Corner : Data.Description.GetTriangleVertexInstances(Triangle))
            {
                const auto Vertex = Data.Description.GetVertexInstanceVertex(Corner);
                const FSCVertexWeights* W = Data.SkinWeights.Find(Vertex);
                if (!W) { Error = TEXT("Soft triangle has no weights"); return false; }
                FStaticMeshBuildVertex V{}; V.Position = A.GetVertexPositions()[Vertex]; Bounds += FVector(V.Position);
                V.TangentX = A.GetVertexInstanceTangents()[Corner]; V.TangentZ = A.GetVertexInstanceNormals()[Corner];
                V.TangentY = FVector3f::CrossProduct(V.TangentZ, V.TangentX) * A.GetVertexInstanceBinormalSigns()[Corner];
                V.Color = FLinearColor(A.GetVertexInstanceColors()[Corner]).ToFColorSRGB();
                for (int32 UV = 0; UV < 2; ++UV) V.UVs[UV] = A.GetVertexInstanceUVs().Get(Corner, UV);
                FSkinWeightInfo Weight{}; int32 Sum = 0, Largest = 0;
                for (int32 K = 0; K < 4; ++K)
                {
                    const int32* Rig = Data.GltfToRig.Find(W->Joints[K]);
                    if (!Rig) { Error = TEXT("Joint is outside the scene hierarchy"); return false; }
                    Weight.InfluenceBones[K] = *Rig + 1;
                    Weight.InfluenceWeights[K] = FMath::RoundToInt(W->Weights[K] * 65535); Sum += Weight.InfluenceWeights[K];
                    if (W->Weights[K] > W->Weights[Largest]) Largest = K;
                }
                Weight.InfluenceWeights[Largest] += 65535 - Sum;
                Indices.Add(Vertices.Add(V)); Weights.Add(Weight);
            }
            ++Section.NumTriangles;
        }
        Section.NumVertices = Vertices.Num() - Section.BaseVertexIndex;
        if (Section.NumTriangles)
        {
            TMap<int32, TArray<int32>> Duplicates;
            Section.DuplicatedVerticesBuffer.Init(Section.NumVertices, Duplicates);
            LOD->RenderSections.Add(MoveTemp(Section));
        }
    }
    LOD->StaticVertexBuffers.PositionVertexBuffer.Init(Vertices);
    LOD->StaticVertexBuffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(true);
    LOD->StaticVertexBuffers.StaticMeshVertexBuffer.Init(Vertices, 2);
    LOD->StaticVertexBuffers.ColorVertexBuffer.Init(Vertices);
    LOD->SkinWeightVertexBuffer.SetNeedsCPUAccess(true);
    LOD->SkinWeightVertexBuffer.SetMaxBoneInfluences(4);
    LOD->SkinWeightVertexBuffer.SetUse16BitBoneIndex(true);
    LOD->SkinWeightVertexBuffer.SetUse16BitBoneWeight(true);
    LOD->SkinWeightVertexBuffer = Weights;
    LOD->MultiSizeIndexContainer.CreateIndexBuffer(sizeof(uint32));
    LOD->MultiSizeIndexContainer.CopyIndexBuffer(Indices);
    LOD->bStreamedDataInlined = true;
    Mesh->SetHasVertexColors(true);
    Mesh->SetImportedBounds(Bounds.IsValid ? FBoxSphereBounds(Bounds) : FBoxSphereBounds(FVector::ZeroVector, FVector(1), 1));
    Mesh->InitResources();
    return true;
}
