// Static mesh partitioning, worker-thread Chaos derivation, and game-thread UObject construction.
#include "SCMeshes.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "Util/ColorConstants.h"
#include "StaticMeshResources.h"
#include "Materials/Material.h"
#include "PhysicsEngine/BodySetup.h"
#include "Rendering/NaniteResources.h"
#include "UObject/StrongObjectPtrTemplates.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Chaos/ChaosArchive.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#if WITH_EDITOR
#include "StaticMeshCompiler.h"
#include "DistanceFieldAtlas.h"
#include "MeshCardBuild.h"
#endif

FSCDerivedMesh FSCMeshes::Select(const FSCDerivedMesh& Data, const TArray<FTriangleID>& Triangles)
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
void FSCMeshes::SplitTranslucent(FSCDerivedMesh& Data, const TSet<int32>& Slots, FSCDerivedMesh& Translucent)
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
bool FSCMeshes::DeriveCollision(FSCDerivedMesh& Data, FString& Error, bool bVertexIndexMap)
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
namespace
{
void CommitSource(UStaticMesh& Mesh)
{
#if WITH_EDITOR
    UStaticMesh::FCommitMeshDescriptionParams Commit;
    Commit.bMarkPackageDirty = false;
    Commit.bUseHashAsGuid = true;
    Mesh.CommitMeshDescription(0, Commit);
#endif
}

// Instantiate in three steps so that many meshes can build together (InstantiateBatch): configure (game thread), build,
// then collision and stats once the render data exists.
UStaticMesh* ConfigureMesh(const FSCDerivedMesh& Data, UObject* Outer, FName Name, bool bBuildCollision, UMaterialInterface* Material,
                           FSCMeshBuildStats& Stats, bool bCoarseProxy = false, FMeshDescription* DeferredSource = nullptr)
{
    check(IsInGameThread());
    UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, Name, RF_Transient);
    Stats.NaniteReason = TEXT("disabled:empty_geometry");
    Stats.Triangles = Data.Description.Triangles().Num();
    if (Stats.Triangles == 0) return Mesh;
    Mesh->bSupportRayTracing = true;
    for (int32 I = 0; I < Data.MaterialNames.Num(); ++I)
        Mesh->GetStaticMaterials().Add(
            FStaticMaterial(Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface), FName(*FString::Printf(TEXT("Slot_%d"), I))));
    Mesh->CreateBodySetup();
    auto* Body = Mesh->GetBodySetup();
    Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
    Body->bDoubleSidedGeometry = true;
    Body->bNeverNeedsCookedCollisionData = true; // Avoid the synchronous implicit cook in Build().
    if (bCoarseProxy)
    {
        Stats.NaniteReason = TEXT("proxy:deferred_detail");
        return Mesh;
    }
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
    if (DeferredSource)
        Mesh->CreateMeshDescription(0, MoveTemp(*DeferredSource));
    else
    {
        Mesh->CreateMeshDescription(0, Data.Description);
        CommitSource(*Mesh);
    }
    for (int32 I = 0; I < Data.MaterialNames.Num(); ++I)
    {
        FMeshSectionInfo Info(I);
        Info.bEnableCollision = bBuildCollision;
        Mesh->GetSectionInfoMap().Set(0, I, Info);
    }
#else
    Stats.NaniteReason = TEXT("disabled:non_editor_host");
#endif
    return Mesh;
}
void BuildRuntime(UStaticMesh* Mesh, const FSCDerivedMesh& Data, bool bCoarseProxy = false)
{
    UStaticMesh::FBuildMeshDescriptionsParams Params;
    Params.bFastBuild = true;
    Params.bAllowCpuAccess = false;
    Params.bBuildSimpleCollision = false;
    if (bCoarseProxy)
    {
        Params.bCommitMeshDescription = false;
        auto& LOD = Params.PerLODOverrides.AddDefaulted_GetRef();
        LOD.bUseFullPrecisionUVs = true;
        LOD.bUseHighPrecisionTangentBasis = true;
    }
    Mesh->BuildFromMeshDescriptions({&Data.Description}, Params);
}
void AwaitRepresentations(UStaticMesh* Mesh, FSCMeshBuildStats& Stats)
{
    const double RepresentationStart = FPlatformTime::Seconds();
#if WITH_EDITOR
    if (GDistanceFieldAsyncQueue) GDistanceFieldAsyncQueue->BlockUntilBuildComplete(Mesh, false);
    if (GCardRepresentationAsyncQueue) GCardRepresentationAsyncQueue->BlockUntilBuildComplete(Mesh, false);
#endif
    Stats.RepresentationSeconds = FPlatformTime::Seconds() - RepresentationStart;
}
void SetCollisionReady(UBodySetup& Body)
{
    Body.bCreatedPhysicsMeshes = true;
    Body.bFailedToCreatePhysicsMeshes = false;
    Body.bHasCookedCollisionData = true;
    Body.bNeverNeedsCookedCollisionData = false;
}

bool FinishMesh(UStaticMesh* Mesh, const FSCDerivedMesh& Data, bool bBuildCollision, FSCMeshBuildStats& Stats,
                FString& Error, const UBodySetup* ProxyBody = nullptr)
{
    if (bBuildCollision)
    {
        auto* Body = Mesh->GetBodySetup();
        // The coarse mesh already owns the exact, immutable collision geometry.
        if (ProxyBody && ProxyBody->TriMeshGeometries.Num() == 1)
        {
            Body->TriMeshGeometries = ProxyBody->TriMeshGeometries;
            SetCollisionReady(*Body);
            Stats.CollisionTriangles = Body->TriMeshGeometries[0]->Elements().GetNumTriangles();
        }
        else if (!FSCMeshes::InstallCollision(Body, Data.CollisionBytes, Data.CollisionVersions, Stats.CollisionTriangles, Error))
            return false;
        Stats.CollisionSeconds = Data.CollisionDeriveSeconds;
        Stats.bCollision = true;
    }
    const auto* Render = Mesh->GetRenderData();
    if (!Render || Render->LODResources.IsEmpty())
    {
        Error = TEXT("Static mesh build produced no render data");
        return false;
    }
    const auto& LOD = Render->LODResources[0];
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
    return true;
}
} // namespace

bool FSCMeshes::InstallCollision(UBodySetup* Body, const TArray<uint8>& Bytes, const FCustomVersionContainer& Versions,
                                      int32& TriangleCount, FString& Error)
{
    if (!Body || Bytes.IsEmpty())
    {
        Error = TEXT("Collision requested without a body or DeriveCollision data");
        return false;
    }
    FMemoryReader Reader(Bytes);
    Reader.SetCustomVersions(Versions);
    Chaos::FChaosArchive Archive(Reader);
    Chaos::FTriangleMeshImplicitObjectPtr Geometry;
    Archive << Geometry;
    if (Reader.IsError() || !Geometry)
    {
        Error = TEXT("Cannot instantiate derived Chaos BVH");
        return false;
    }
    Body->TriMeshGeometries.Reset();
    Body->TriMeshGeometries.Add(Geometry);
    SetCollisionReady(*Body);
    TriangleCount = Geometry->Elements().GetNumTriangles();
    return true;
}

UStaticMesh* FSCMeshes::Instantiate(const FSCDerivedMesh& Data, UObject* Outer, FName Name, bool bBuildCollision,
                                          UMaterialInterface* Material, FSCMeshBuildStats& Stats, FString& Error, bool bCoarseProxy)
{
    UStaticMesh* Mesh = ConfigureMesh(Data, Outer, Name, bBuildCollision, Material, Stats, bCoarseProxy);
    if (Data.Description.Triangles().Num() == 0) return Mesh;
    const double Start = FPlatformTime::Seconds();
#if WITH_EDITOR
    if (bCoarseProxy) BuildRuntime(Mesh, Data, true);
    else
    {
        Mesh->Build(true);
        FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
    }
#else
    BuildRuntime(Mesh, Data);
#endif
    Stats.BuildSeconds = FPlatformTime::Seconds() - Start;
    if (!bCoarseProxy) AwaitRepresentations(Mesh, Stats);
    return FinishMesh(Mesh, Data, bBuildCollision, Stats, Error) ? Mesh : nullptr;
}

UStaticMesh* FSCMeshes::PrepareFineBuild(FSCDerivedMesh& Data, const UStaticMesh& Proxy, FName Name,
    bool bBuildCollision, FSCMeshBuildStats& Stats, UE::Tasks::FTask& SourceCommit)
{
    UStaticMesh* Mesh = ConfigureMesh(Data, Proxy.GetOuter(), Name, bBuildCollision, nullptr, Stats, false, &Data.Description);
#if WITH_EDITOR
    // The source model and its bulk-data UObject already exist. Commit only on this worker
    // until it returns, leaving package dirtying disabled as required by the engine API.
    SourceCommit = UE::Tasks::Launch(UE_SOURCE_LOCATION, [OwnedMesh = TStrongObjectPtr<UStaticMesh>(Mesh)] {
        CommitSource(*OwnedMesh);
    });
#endif
    return Mesh;
}

void FSCMeshes::SubmitFineBuild(UStaticMesh* Mesh, const FSCDerivedMesh& Data)
{
    check(IsInGameThread());
#if WITH_EDITOR
    Mesh->Build(true);
#else
    BuildRuntime(Mesh, Data);
#endif
}

bool FSCMeshes::IsFineBuildReady(const UStaticMesh* Mesh)
{
#if WITH_EDITOR
    // Keep the mesh unbound until all three engine compilers have applied their results. The
    // game-mode compiler would finish any unfinished static mesh attached to a live component.
    return !Mesh->IsCompiling() &&
        (!GDistanceFieldAsyncQueue || GDistanceFieldAsyncQueue->GetNumOutstandingTasks(Mesh) == 0) &&
        (!GCardRepresentationAsyncQueue || GCardRepresentationAsyncQueue->GetNumOutstandingTasks(Mesh) == 0);
#else
    return true;
#endif
}

bool FSCMeshes::FinishFineBuild(UStaticMesh* Mesh, const FSCDerivedMesh& Data, UStaticMesh* Proxy,
    bool bBuildCollision, bool bWait, FSCMeshBuildStats& Stats, FString& Error)
{
#if WITH_EDITOR
    if (bWait)
    {
        const double Start = FPlatformTime::Seconds();
        FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
        Stats.BuildSeconds += FPlatformTime::Seconds() - Start;
        AwaitRepresentations(Mesh, Stats);
    }
#endif
    check(IsFineBuildReady(Mesh));
    return FinishMesh(Mesh, Data, bBuildCollision, Stats, Error, bBuildCollision ? Proxy->GetBodySetup() : nullptr);
}

TArray<UStaticMesh*> FSCMeshes::InstantiateBatch(const TArray<const FSCDerivedMesh*>& Data, UObject* Outer, TArray<FSCMeshBuildStats>& Stats,
                                                       TArray<FString>& Errors)
{
    TArray<UStaticMesh*> Meshes, ToBuild;
    Stats.SetNum(Data.Num());
    Errors.SetNum(Data.Num());
    for (int32 I = 0; I < Data.Num(); ++I)
    {
        Meshes.Add(ConfigureMesh(*Data[I], Outer, NAME_None, false, nullptr, Stats[I]));
        if (Data[I]->Description.Triangles().Num() > 0) ToBuild.Add(Meshes.Last());
    }
    const double Start = FPlatformTime::Seconds();
#if WITH_EDITOR
    UStaticMesh::BatchBuild(ToBuild, true); // asynchronous: the compiling manager builds them in parallel
    FStaticMeshCompilingManager::Get().FinishCompilation(ToBuild);
#else
    for (int32 I = 0; I < Data.Num(); ++I)
        if (Data[I]->Description.Triangles().Num() > 0) BuildRuntime(Meshes[I], *Data[I]);
#endif
    const double BuildSeconds = FPlatformTime::Seconds() - Start;
    for (int32 I = 0; I < Data.Num(); ++I)
    {
        if (Data[I]->Description.Triangles().Num() == 0) continue;
        Stats[I].BuildSeconds = BuildSeconds / FMath::Max(1, ToBuild.Num());
        AwaitRepresentations(Meshes[I], Stats[I]);
        if (!FinishMesh(Meshes[I], *Data[I], false, Stats[I], Errors[I])) Meshes[I] = nullptr;
    }
    return Meshes;
}

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFineBuildTest, "SCBridge.Import.GeometryStreaming.FineBuild",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSCFineBuildTest::RunTest(const FString&)
{
    FSCDerivedMesh Data;
    Data.MaterialNames.Add(TEXT("Synthetic"));
    Data.bHasGeometry = true;
    Data.bEnableNanite = false;
    Data.bHasTangents = true;
    FStaticMeshAttributes Attributes(Data.Description);
    Attributes.Register();
    Attributes.GetVertexInstanceUVs().SetNumChannels(1);
    const auto Group = Data.Description.CreatePolygonGroup();
    TArray<FVertexInstanceID> Corners;
    for (const FVector3f Position : {FVector3f(0, -100, -100), FVector3f(0, 100, -100), FVector3f(0, 0, 100)})
    {
        const auto Vertex = Data.Description.CreateVertex();
        Attributes.GetVertexPositions()[Vertex] = Position;
        const auto Corner = Data.Description.CreateVertexInstance(Vertex);
        Attributes.GetVertexInstanceNormals()[Corner] = FVector3f(1, 0, 0);
        Attributes.GetVertexInstanceTangents()[Corner] = FVector3f(0, 1, 0);
        Attributes.GetVertexInstanceBinormalSigns()[Corner] = 1;
        Corners.Add(Corner);
    }
    Data.Description.CreateTriangle(Group, Corners);
    FString Error;
    if (!TestTrue(*Error, FSCMeshes::DeriveCollision(Data, Error))) return false;
    FSCMeshBuildStats ProxyStats, FineStats;
    UStaticMesh* Proxy = FSCMeshes::Instantiate(Data, GetTransientPackage(), NAME_None, true, nullptr, ProxyStats, Error, true);
    if (!TestNotNull(*Error, Proxy)) return false;
    UE::Tasks::FTask SourceCommit;
    UStaticMesh* Fine = FSCMeshes::PrepareFineBuild(Data, *Proxy, NAME_None, true, FineStats, SourceCommit);
    SourceCommit.Wait();
    FSCMeshes::SubmitFineBuild(Fine, Data);
    if (!TestTrue(*Error, FSCMeshes::FinishFineBuild(Fine, Data, Proxy, true, true, FineStats, Error))) return false;
    TestTrue(TEXT("capture waits for all fine work"), FSCMeshes::IsFineBuildReady(Fine));
    TestEqual(TEXT("fine render triangles"), FineStats.Triangles, 1);
    auto* FineBody = Fine->GetBodySetup();
    auto* ProxyBody = Proxy->GetBodySetup();
    if (!TestEqual(TEXT("exact fine collision shapes"), FineBody->TriMeshGeometries.Num(), 1)) return false;
    TestTrue(TEXT("fine build reuses the exact proxy collision"), FineBody->TriMeshGeometries[0] == ProxyBody->TriMeshGeometries[0]);
    ProxyBody->ClearPhysicsMeshes();
    TestEqual(TEXT("recycled proxy releases only its own collision reference"), FineBody->TriMeshGeometries[0]->Elements().GetNumTriangles(), 1);
    return !HasAnyErrors();
}
#endif
