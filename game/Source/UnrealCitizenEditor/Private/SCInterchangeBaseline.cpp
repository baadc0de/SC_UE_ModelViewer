#include "SCInterchangeBaseline.h"
#include "InterchangeGenericMeshPipeline.h"
#include "InterchangeGenericMaterialPipeline.h"
#include "InterchangeGenericTexturePipeline.h"
#include "InterchangeGenericAnimationPipeline.h"
#include "InterchangeManager.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "StaticMeshCompiler.h"
#include "Rendering/NaniteResources.h"
#include "StaticMeshAttributes.h"
#include "SCMeshImporter.h"
#include "SCPackageSource.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

USCInterchangePipeline::USCInterchangePipeline()
{
    MeshPipeline->bBuildNanite = true;
    MeshPipeline->NaniteTriangleThreshold = 0;
    MeshPipeline->CombineStaticMeshesBehavior = EInterchangeCombineStaticMeshesBehavior::All;
    MeshPipeline->bImportSkeletalMeshes = false;
    MeshPipeline->bCollision = false;
    MaterialPipeline->bImportMaterials = false;
    MaterialPipeline->TexturePipeline->bImportTextures = false;
    AnimationPipeline->bImportAnimations = false;
    CommonMeshesProperties->bRecomputeNormals = false;
    CommonMeshesProperties->bRecomputeTangents = false;
    // Interchange maps (x,y,z)->(x,z,y). Our ship-facing convention is a +90 degree yaw.
    ImportOffsetRotation = FRotator(0,90,0);
}
bool USCInterchangeBaseline::ImportGLB(const FString& Filename)
{
    auto* Source = UInterchangeManager::CreateSourceData(Filename);
    FImportAssetParameters Parameters; Parameters.bIsAutomated = true;
    TStrongObjectPtr<USCInterchangePipeline> Pipeline(NewObject<USCInterchangePipeline>());
    Parameters.OverridePipelines.Add(FSoftObjectPath(Pipeline.Get()));
    TArray<UObject*> Objects;
    const double Start = FPlatformTime::Seconds();
    const bool bOK = UInterchangeManager::GetInterchangeManagerScripted()->ImportAsset(TEXT("/Temp/SCBaseline/") + FGuid::NewGuid().ToString(EGuidFormats::Digits), Source, Parameters, Objects);
    FStaticMeshCompilingManager::Get().FinishAllCompilation();
    int32 Meshes = 0;
    FSCDerivedMesh Direct; FString Error; FSCFolderPackageSource Folder(FPaths::GetPath(Filename));
    const bool bDerived = FSCMeshImporter::Derive(Folder, FPaths::GetCleanFilename(Filename), Direct, Error);
    FBox DirectBounds(ForceInit);
    if (bDerived)
    {
        FStaticMeshConstAttributes A(Direct.Description);
        for (auto V : Direct.Description.Vertices().GetElementIDs()) DirectBounds += FVector(A.GetVertexPositions()[V]);
    }
    bool bMatch = bDerived;
    for (auto* Object : Objects)
    {
        if (auto* Mesh = Cast<UStaticMesh>(Object))
        {
            ++Meshes; const auto* R = Mesh->GetRenderData();
            const FMeshDescription* D = Mesh->GetMeshDescription(0);
            const FBox Bounds = Mesh->GetBoundingBox();
            bool bBounds = Bounds.Min.Equals(DirectBounds.Min, 0.1) && Bounds.Max.Equals(DirectBounds.Max, 0.1);
            bMatch &= bBounds;
            if (D && D->Triangles().Num() == 1 && Direct.Description.Triangles().Num() == 1)
            {
                FStaticMeshConstAttributes Imported(*D), Own(Direct.Description);
                const auto IC = D->GetTriangleVertexInstances(FTriangleID(0));
                const auto DC = Direct.Description.GetTriangleVertexInstances(FTriangleID(0));
                bool bAttributes = true;
                for (int32 K = 0; K < 3; ++K)
                {
                    bAttributes &= Imported.GetVertexPositions()[D->GetVertexInstanceVertex(IC[K])].Equals(Own.GetVertexPositions()[Direct.Description.GetVertexInstanceVertex(DC[K])], 0.01f);
                    bAttributes &= Imported.GetVertexInstanceNormals()[IC[K]].Equals(Own.GetVertexInstanceNormals()[DC[K]], 0.001f);
                    bAttributes &= Imported.GetVertexInstanceTangents()[IC[K]].Equals(Own.GetVertexInstanceTangents()[DC[K]], 0.001f);
                    bAttributes &= FMath::IsNearlyEqual(Imported.GetVertexInstanceBinormalSigns()[IC[K]], Own.GetVertexInstanceBinormalSigns()[DC[K]]);
                    bAttributes &= Imported.GetVertexInstanceUVs().Get(IC[K], 0).Equals(Own.GetVertexInstanceUVs().Get(DC[K], 0), 0.001f);
                    bAttributes &= Imported.GetVertexInstanceUVs().Get(IC[K], 1).Equals(Own.GetVertexInstanceUVs().Get(DC[K], 1), 0.001f);
                    bAttributes &= Imported.GetVertexInstanceColors()[IC[K]].Equals(Own.GetVertexInstanceColors()[DC[K]], 0.001f);
                }
                UE_LOG(LogTemp, Display, TEXT("SC_BASELINE synthetic_attributes_match=%d imported_sign=%.1f direct_sign=%.1f"), bAttributes,
                    Imported.GetVertexInstanceBinormalSigns()[IC[0]], Own.GetVertexInstanceBinormalSigns()[DC[0]]);
                bMatch &= bAttributes;
            }
            UE_LOG(LogTemp, Display, TEXT("SC_BASELINE mesh=%s triangles=%d nanite=%d uv_channels=%d colours=%d bounds_cm=%s direct_bounds_match=%d seconds=%.4f"),
                *Mesh->GetName(), D ? D->Triangles().Num() : 0, R && R->HasValidNaniteData(),
                R && !R->LODResources.IsEmpty() ? R->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() : 0,
                R && !R->LODResources.IsEmpty() ? R->LODResources[0].VertexBuffers.ColorVertexBuffer.GetNumVertices() : 0,
                *Bounds.GetSize().ToString(), bBounds, FPlatformTime::Seconds() - Start);
        }
        Object->ClearFlags(RF_Public | RF_Standalone); Object->SetFlags(RF_Transient); Object->GetOutermost()->SetDirtyFlag(false);
    }
    UE_LOG(LogTemp, Display, TEXT("SC_BASELINE complete=%d meshes=%d direct_match=%d error=%s"), bOK, Meshes, bMatch, *Error);
    return bOK && Meshes > 0 && bMatch;
}
