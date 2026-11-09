// Static mesh partitioning, worker-thread Chaos derivation, and game-thread UObject construction.
#include "SCMeshImporter.h"
#include "SCPackageReader.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "Util/ColorConstants.h"
#include "StaticMeshResources.h"
#include "Materials/Material.h"
#include "PhysicsEngine/BodySetup.h"
#include "Rendering/NaniteResources.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Chaos/ChaosArchive.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#if WITH_EDITOR
#include "StaticMeshCompiler.h"
#include "DistanceFieldAtlas.h"
#include "MeshCardBuild.h"
#endif

FSCDerivedMesh FSCMeshImporter::Select(const FSCDerivedMesh& Data, const TArray<FTriangleID>& Triangles)
{
    FSCDerivedMesh Result;
    Result.MaterialNames = Data.MaterialNames;
    Result.MaterialSourceNames = Data.MaterialSourceNames;
    Result.MaterialFamilies = Data.MaterialFamilies;
    Result.ContentHash = Data.ContentHash;
    Result.bHasTangents = Data.bHasTangents;
    Result.bEnableNanite = Data.bEnableNanite;
    Result.SocketNode = Data.SocketNode;
    FMeshDescription& Selected = Result.Description;
    FStaticMeshAttributes Dst(Selected);
    Dst.Register();
    FStaticMeshConstAttributes Src(Data.Description);
    const int32 Channels = Src.GetVertexInstanceUVs().GetNumChannels();
    Dst.GetVertexInstanceUVs().SetNumChannels(Channels);
    TArray<FPolygonGroupID> Groups;
    for (int32 Slot = 0; Slot < Data.MaterialNames.Num(); ++Slot)
    {
        const auto Group = Selected.CreatePolygonGroup();
        Groups.Add(Group);
        Dst.GetPolygonGroupMaterialSlotNames()[Group] = FName(*FString::Printf(TEXT("Slot_%d"), Slot));
    }
    TMap<FVertexID, FVertexID> Vertices;
    for (auto Triangle : Triangles)
    {
        TArray<FVertexInstanceID, TInlineAllocator<3>> Corners;
        for (auto Original : Data.Description.GetTriangleVertexInstances(Triangle))
        {
            const auto Vertex = Data.Description.GetVertexInstanceVertex(Original);
            auto* Existing = Vertices.Find(Vertex);
            FVertexID NewVertex;
            if (Existing)
                NewVertex = *Existing;
            else
            {
                NewVertex = Selected.CreateVertex();
                Vertices.Add(Vertex, NewVertex);
                Dst.GetVertexPositions()[NewVertex] = Src.GetVertexPositions()[Vertex];
                if (const auto* W = Data.SkinWeights.Find(Vertex)) Result.SkinWeights.Add(NewVertex, *W);
            }
            const auto Corner = Selected.CreateVertexInstance(NewVertex);
            Corners.Add(Corner);
            Dst.GetVertexInstanceNormals()[Corner] = Src.GetVertexInstanceNormals()[Original];
            Dst.GetVertexInstanceTangents()[Corner] = Src.GetVertexInstanceTangents()[Original];
            Dst.GetVertexInstanceBinormalSigns()[Corner] = Src.GetVertexInstanceBinormalSigns()[Original];
            Dst.GetVertexInstanceColors()[Corner] = Src.GetVertexInstanceColors()[Original];
            for (int32 Channel = 0; Channel < Channels; ++Channel)
                Dst.GetVertexInstanceUVs().Set(Corner, Channel, Src.GetVertexInstanceUVs().Get(Original, Channel));
        }
        const auto NewTriangle = Selected.CreateTriangle(Groups[Data.Description.GetTrianglePolygonGroup(Triangle).GetValue()], Corners);
        Result.TriangleNodes.Add(NewTriangle, Data.TriangleNodes.FindRef(Triangle));
    }
    Result.bHasGeometry = !Triangles.IsEmpty();
    return Result;
}
void FSCMeshImporter::SplitTranslucent(FSCDerivedMesh& Data, const TSet<int32>& Slots, FSCDerivedMesh& Translucent)
{
    TArray<FTriangleID> Opaque, Blend;
    for (auto T : Data.Description.Triangles().GetElementIDs())
        (Slots.Contains(Data.Description.GetTrianglePolygonGroup(T).GetValue()) ? Blend : Opaque).Add(T);
    if (Blend.IsEmpty()) return;
    if (Opaque.IsEmpty())
    {
        Data.bEnableNanite = false;
        return;
    }
    Translucent.MaterialNames = Data.MaterialNames;
    Translucent.ContentHash = Data.ContentHash;
    Translucent.bHasTangents = Data.bHasTangents;
    Translucent.bEnableNanite = false;
    Translucent.bHasGeometry = true;
    Translucent = Select(Data, Blend);
    Translucent.bEnableNanite = false;
    auto OpaqueData = Select(Data, Opaque);
    Data.Description = MoveTemp(OpaqueData.Description);
    Data.TriangleNodes = MoveTemp(OpaqueData.TriangleNodes);
    Data.SkinWeights = MoveTemp(OpaqueData.SkinWeights);
    // CollisionBytes retain the independently derived full-source triangle BVH.
}
bool FSCMeshImporter::DeriveCollision(FSCDerivedMesh& Data, FString& Error, bool bVertexIndexMap)
{
    const double Start = FPlatformTime::Seconds();
    Data.CollisionBytes.Reset();
    if (Data.Description.Triangles().Num() == 0) return true;
    FStaticMeshConstAttributes Attributes(Data.Description);
    Chaos::FTriangleMeshImplicitObject::ParticlesType Particles;
    Particles.AddParticles(Data.Description.Vertices().Num());
    TMap<FVertexID, int32> VertexMap;
    for (auto V : Data.Description.Vertices().GetElementIDs())
    {
        const int32 Index = VertexMap.Num();
        VertexMap.Add(V, Index);
        Particles.SetX(Index, Attributes.GetVertexPositions()[V]);
    }
    TArray<Chaos::TVec3<int32>> Triangles;
    TArray<uint16> Materials;
    auto FaceMap = MakeUnique<TArray<int32>>();
    for (auto Triangle : Data.Description.Triangles().GetElementIDs())
    {
        const auto V = Data.Description.GetTriangleVertices(Triangle);
        // UStaticMesh::GetPhysicsTriMeshData sets bFlipNormals: Chaos uses the opposite winding.
        Triangles.Emplace(VertexMap[V[0]], VertexMap[V[2]], VertexMap[V[1]]);
        Materials.Add(uint16(Data.Description.GetTrianglePolygonGroup(Triangle).GetValue()));
        FaceMap->Add(Triangle.GetValue());
    }
    // Particles are created in vertex-element order, so external vertex i is particle i. Chaos
    // serializes a missing map as an empty array and loads it as a non-null empty map, which
    // UpdateVertices then indexes: a refittable mesh must carry the explicit identity map.
    TUniquePtr<TArray<int32>> VertexIndexMap;
    if (bVertexIndexMap)
    {
        VertexIndexMap = MakeUnique<TArray<int32>>();
        VertexIndexMap->Init(INDEX_NONE, VertexMap.Num());
        for (const auto& Pair : VertexMap)
        {
            if (!VertexIndexMap->IsValidIndex(Pair.Key.GetValue()))
            {
                Error = TEXT("Refittable collision needs compact vertex IDs");
                return false;
            }
            (*VertexIndexMap)[Pair.Key.GetValue()] = Pair.Value;
        }
    }
    Chaos::FTriangleMeshImplicitObjectPtr Geometry = new Chaos::FTriangleMeshImplicitObject(
        MoveTemp(Particles), MoveTemp(Triangles), MoveTemp(Materials), MoveTemp(FaceMap), MoveTemp(VertexIndexMap));
    FMemoryWriter Writer(Data.CollisionBytes);
    Chaos::FChaosArchive Archive(Writer);
    Archive << Geometry;
    Data.CollisionVersions = Writer.GetCustomVersions();
    Data.CollisionDeriveSeconds = FPlatformTime::Seconds() - Start;
    if (Writer.IsError())
    {
        Error = TEXT("Cannot serialize derived Chaos BVH");
        return false;
    }
    return true;
}
UStaticMesh* FSCMeshImporter::Instantiate(const FSCDerivedMesh& Data, UObject* Outer, FName Name, bool bBuildCollision,
                                          UMaterialInterface* Material, FSCMeshBuildStats& Stats, FString& Error)
{
    check(IsInGameThread());
    UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, Name, RF_Transient);
    Stats.NaniteReason = TEXT("disabled:empty_geometry");
    if (Data.Description.Triangles().Num() == 0) return Mesh;
    Mesh->bSupportRayTracing = true;
    for (int32 I = 0; I < Data.MaterialNames.Num(); ++I)
        Mesh->GetStaticMaterials().Add(
            FStaticMaterial(Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface), FName(*FString::Printf(TEXT("Slot_%d"), I))));
    Mesh->CreateBodySetup();
    auto* Body = Mesh->GetBodySetup();
    Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
    Body->bDoubleSidedGeometry = true;
    Body->bNeverNeedsCookedCollisionData = true; // Avoid the synchronous implicit cook in Build().
    const double Start = FPlatformTime::Seconds();
#if WITH_EDITOR
    Mesh->bGenerateMeshDistanceField = Data.bEnableNanite;
    Mesh->GetNaniteSettings().bEnabled = Data.bEnableNanite;
    Mesh->GetNaniteSettings().bExplicitTangents = true;
    Mesh->GetNaniteSettings().FallbackTarget = ENaniteFallbackTarget::PercentTriangles;
    Mesh->GetNaniteSettings().FallbackPercentTriangles = 1.0f;
    Mesh->GetNaniteSettings().FallbackRelativeError = 0.0f;
    Stats.bNaniteEnabled = Data.bEnableNanite;
    Stats.NaniteReason = Data.bEnableNanite ? TEXT("accepted") : TEXT("skipped:translucent");
    auto& Model = Mesh->AddSourceModel();
    Model.BuildSettings.bRecomputeNormals = false;
    Model.BuildSettings.bRecomputeTangents = !Data.bHasTangents;
    Model.BuildSettings.bUseMikkTSpace = true;
    Model.BuildSettings.bGenerateLightmapUVs = false;
    Model.BuildSettings.bUseFullPrecisionUVs = true;
    Model.BuildSettings.bUseHighPrecisionTangentBasis = true;
    Model.BuildSettings.bRemoveDegenerates = false;
    Model.BuildSettings.MaxLumenMeshCards = Data.bEnableNanite ? 12 : 0;
    Mesh->CreateMeshDescription(0, Data.Description);
    UStaticMesh::FCommitMeshDescriptionParams Commit;
    Commit.bMarkPackageDirty = false;
    Commit.bUseHashAsGuid = true;
    Mesh->CommitMeshDescription(0, Commit);
    for (int32 I = 0; I < Data.MaterialNames.Num(); ++I)
    {
        FMeshSectionInfo Info(I);
        Info.bEnableCollision = bBuildCollision;
        Mesh->GetSectionInfoMap().Set(0, I, Info);
    }
    Mesh->Build(true);
    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
#else
    Stats.NaniteReason = TEXT("disabled:non_editor_host");
    UStaticMesh::FBuildMeshDescriptionsParams Params;
    Params.bFastBuild = true;
    Params.bAllowCpuAccess = false;
    Params.bBuildSimpleCollision = false;
    Mesh->BuildFromMeshDescriptions({&Data.Description}, Params);
#endif
    Stats.BuildSeconds = FPlatformTime::Seconds() - Start;
    const double RepresentationStart = FPlatformTime::Seconds();
#if WITH_EDITOR
    if (GDistanceFieldAsyncQueue) GDistanceFieldAsyncQueue->BlockUntilBuildComplete(Mesh, false);
    if (GCardRepresentationAsyncQueue) GCardRepresentationAsyncQueue->BlockUntilBuildComplete(Mesh, false);
#endif
    Stats.RepresentationSeconds = FPlatformTime::Seconds() - RepresentationStart;
    if (bBuildCollision)
    {
        if (Data.CollisionBytes.IsEmpty())
        {
            Error = TEXT("Collision requested without DeriveCollision data");
            return nullptr;
        }
        FMemoryReader Reader(Data.CollisionBytes);
        Reader.SetCustomVersions(Data.CollisionVersions);
        Chaos::FChaosArchive Archive(Reader);
        Chaos::FTriangleMeshImplicitObjectPtr Geometry;
        Archive << Geometry;
        if (Reader.IsError() || !Geometry)
        {
            Error = TEXT("Cannot instantiate derived Chaos BVH");
            return nullptr;
        }
        Body->TriMeshGeometries.Add(Geometry);
        Body->bCreatedPhysicsMeshes = true;
        Body->bFailedToCreatePhysicsMeshes = false;
        Body->bHasCookedCollisionData = true;
        Body->bNeverNeedsCookedCollisionData = false;
        Stats.CollisionSeconds = Data.CollisionDeriveSeconds;
        Stats.bCollision = true;
        Stats.CollisionTriangles = Geometry->Elements().GetNumTriangles();
    }
    const auto* Render = Mesh->GetRenderData();
    if (!Render || Render->LODResources.IsEmpty())
    {
        Error = TEXT("Static mesh build produced no render data");
        return nullptr;
    }
    const auto& LOD = Render->LODResources[0];
    Stats.Triangles = Data.Description.Triangles().Num();
    Stats.FallbackTriangles = LOD.GetNumTriangles();
    Stats.Clusters = Render->HasValidNaniteData() ? Render->NaniteResourcesPtr->NumClusters : 0;
    Stats.bDistanceField = LOD.DistanceFieldData != nullptr;
    Stats.bRayTracingGeometry = LOD.RayTracingGeometry != nullptr || (Render->RayTracingProxy && !Render->RayTracingProxy->LODs.IsEmpty() &&
                                                                      Render->RayTracingProxy->LODs[0].RayTracingGeometry);
#if WITH_EDITOR
    if (Stats.bNaniteEnabled && !Stats.Clusters) Stats.NaniteReason = TEXT("disabled:no_clusters");
    Stats.bMeshCards = LOD.CardRepresentationData != nullptr;
    Stats.Cards = LOD.CardRepresentationData ? LOD.CardRepresentationData->MeshCardsBuildData.CardBuildData.Num() : 0;
#endif
    return Mesh;
}
