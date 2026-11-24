#include "SCGeometryStreaming.h"

#include "SCImporter.h"
#include "SCProcessedCache.h"
#include "SCProcessedMeshCache.h"
#include "SCVehicle.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/Material.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "StaticMeshAttributes.h"

namespace
{
// Viewer budgets. Below this triangle count another render asset usually costs more than it saves.
constexpr int32 MinimumTriangles = 2000;
constexpr int32 PendingTriangleBudget = 1200000;
constexpr double CellsAcrossLongestAxis = 64.0;
constexpr float CompatibleNormalDot = 0.96f;
constexpr double RoomPrefetchDistance = 800.0; // cm
constexpr double RelativeScreenError = 0.0015;
struct FClusterVertex
{
    FIntVector Cell;
    int32 Parent = INDEX_NONE;
    FPolygonGroupID Group{INDEX_NONE};
    FVertexID Output{INDEX_NONE};
    FVector3f Normal = FVector3f::ZeroVector;
    bool bSeam = false;
};

int32 FindRoot(TArray<FClusterVertex>& Vertices, int32 Index)
{
    while (Vertices[Index].Parent != Index)
    {
        Vertices[Index].Parent = Vertices[Vertices[Index].Parent].Parent;
        Index = Vertices[Index].Parent;
    }
    return Index;
}
}

bool FSCGeometryStreaming::CanDefer(const FSCDerivedMesh& Fine) const
{
    const int32 Triangles = Fine.Description.Triangles().Num();
    return Triangles >= MinimumTriangles && Triangles <= PendingTriangleBudget - PendingTriangles.load();
}

bool FSCGeometryStreaming::Reduce(const FSCDerivedMesh& Fine, FSCDerivedMesh& Coarse, double& Deviation) const
{
    if (!CanDefer(Fine)) return false;
    const FMeshDescription& Source = Fine.Description;
    const FBox Bounds = Source.ComputeBoundingBox();
    const double CellSize = Bounds.GetSize().GetMax() / CellsAcrossLongestAxis;
    if (!FMath::IsFinite(CellSize) || CellSize <= UE_SMALL_NUMBER) return false;

    const FStaticMeshConstAttributes In(Source);
    const auto Points = In.GetVertexPositions();
    const auto Normals = In.GetVertexInstanceNormals();
    TArray<FClusterVertex> Vertices;
    Vertices.SetNum(Source.Vertices().GetArraySize());
    for (const FVertexID Vertex : Source.Vertices().GetElementIDs())
    {
        auto& Cell = Vertices[Vertex.GetValue()];
        const FVector Local = (FVector(Points[Vertex]) - Bounds.Min) / CellSize;
        Cell.Cell = FIntVector(FMath::FloorToInt(Local.X), FMath::FloorToInt(Local.Y), FMath::FloorToInt(Local.Z));
        Cell.Parent = Vertex.GetValue();
    }
    for (const FTriangleID Face : Source.Triangles().GetElementIDs())
    {
        const FPolygonGroupID Group = Source.GetTrianglePolygonGroup(Face);
        for (const FVertexInstanceID Corner : Source.GetTriangleVertexInstances(Face))
        {
            auto& Vertex = Vertices[Source.GetVertexInstanceVertex(Corner).GetValue()];
            const FVector3f Normal = Normals[Corner];
            if (Vertex.Group.GetValue() == INDEX_NONE)
            {
                Vertex.Group = Group;
                Vertex.Normal = Normal;
            }
            else Vertex.bSeam |= Vertex.Group != Group || FVector3f::DotProduct(Vertex.Normal, Normal) < CompatibleNormalDot;
        }
    }
    // Collapse connected edges only. Disconnected surfaces, material boundaries, and sharp normals keep their vertices.
    for (const FTriangleID Face : Source.Triangles().GetElementIDs())
    {
        const auto Corners = Source.GetTriangleVertices(Face);
        for (int32 Corner = 0; Corner < 3; ++Corner)
        {
            const int32 A = FindRoot(Vertices, Corners[Corner].GetValue());
            const int32 B = FindRoot(Vertices, Corners[(Corner + 1) % 3].GetValue());
            if (A != B && Vertices[A].Cell == Vertices[B].Cell && Vertices[A].Group == Vertices[B].Group &&
                !Vertices[A].bSeam && !Vertices[B].bSeam &&
                FVector3f::DotProduct(Vertices[A].Normal, Vertices[B].Normal) >= CompatibleNormalDot)
                Vertices[FMath::Max(A, B)].Parent = FMath::Min(A, B);
        }
    }
    TArray<FTriangleID> Kept;
    TSet<FPolygonGroupID> SourceGroups, KeptGroups;
    for (const FTriangleID Face : Source.Triangles().GetElementIDs())
    {
        const FPolygonGroupID Group = Source.GetTrianglePolygonGroup(Face);
        SourceGroups.Add(Group);
        const auto Corners = Source.GetTriangleVertices(Face);
        const int32 A = FindRoot(Vertices, Corners[0].GetValue());
        const int32 B = FindRoot(Vertices, Corners[1].GetValue());
        const int32 C = FindRoot(Vertices, Corners[2].GetValue());
        if (A != B && B != C && A != C)
        {
            Kept.Add(Face);
            KeptGroups.Add(Group);
        }
    }
    if (Kept.IsEmpty() || Kept.Num() > Source.Triangles().Num() * 0.8 || SourceGroups.Num() != KeptGroups.Num())
        return false;

    FMeshDescription& Output = Coarse.Description;
    FStaticMeshAttributes Out(Output);
    Out.Register();
    const int32 Channels = In.GetVertexInstanceUVs().GetNumChannels();
    Out.GetVertexInstanceUVs().SetNumChannels(Channels);
    for (const FPolygonGroupID Group : Source.PolygonGroups().GetElementIDs())
    {
        Output.CreatePolygonGroupWithID(Group);
        Out.GetPolygonGroupMaterialSlotNames()[Group] = In.GetPolygonGroupMaterialSlotNames()[Group];
    }
    Deviation = 0.0;
    for (const FVertexID Vertex : Source.Vertices().GetElementIDs())
    {
        const int32 Root = FindRoot(Vertices, Vertex.GetValue());
        Deviation = FMath::Max(Deviation, double((Points[Vertex] - Points[FVertexID(Root)]).Size()));
    }
    for (const FTriangleID Face : Kept)
    {
        TArray<FVertexInstanceID, TInlineAllocator<3>> Corners;
        for (FVertexInstanceID Original : Source.GetTriangleVertexInstances(Face))
        {
            const int32 Root = FindRoot(Vertices, Source.GetVertexInstanceVertex(Original).GetValue());
            auto& Cluster = Vertices[Root];
            if (Cluster.Output.GetValue() == INDEX_NONE)
            {
                Cluster.Output = Output.CreateVertex();
                Out.GetVertexPositions()[Cluster.Output] = Points[FVertexID(Root)];
            }
            const FVertexInstanceID Corner = Output.CreateVertexInstance(Cluster.Output);
            Corners.Add(Corner);
            Out.GetVertexInstanceNormals()[Corner] = Normals[Original];
            Out.GetVertexInstanceTangents()[Corner] = In.GetVertexInstanceTangents()[Original];
            Out.GetVertexInstanceBinormalSigns()[Corner] = In.GetVertexInstanceBinormalSigns()[Original];
            Out.GetVertexInstanceColors()[Corner] = In.GetVertexInstanceColors()[Original];
            for (int32 Channel = 0; Channel < Channels; ++Channel)
                Out.GetVertexInstanceUVs().Set(Corner, Channel, In.GetVertexInstanceUVs().Get(Original, Channel));
        }
        Output.CreateTriangle(Source.GetTrianglePolygonGroup(Face), Corners);
    }
    Coarse.MaterialNames = Fine.MaterialNames;
    Coarse.SocketNode = Fine.SocketNode;
    Coarse.CollisionBytes = Fine.CollisionBytes;
    Coarse.CollisionVersions = Fine.CollisionVersions;
    Coarse.bHasGeometry = true;
    Coarse.bHasTangents = Fine.bHasTangents;
    Coarse.bEnableNanite = Fine.bEnableNanite;
    return true;
}

void FSCGeometryStreaming::Defer(ASCVehicle* Owner, UStaticMesh* Proxy, FSCDerivedMesh&& Fine, FString Label,
    double Deviation, bool bCollision, TSharedPtr<FSCProcessedCache, ESPMode::ThreadSafe> Cache, const FString& Key)
{
    PendingTriangles += Fine.Description.Triangles().Num();
    auto& Item = Pending.AddDefaulted_GetRef();
    Item.Owner = Owner;
    Item.Proxy = Proxy;
    Item.Fine = MoveTemp(Fine);
    Item.Label = MoveTemp(Label);
    Item.Key = Key;
    Item.Deviation = Deviation;
    Item.bCollision = bCollision;
    Item.Cache = MoveTemp(Cache);
}

void FSCGeometryStreaming::Attach(ASCVehicle* Owner)
{
    TMap<UStaticMesh*, int32> ByMesh;
    for (int32 Index = 0; Index < Pending.Num(); ++Index)
        if (Pending[Index].Owner.Get() == Owner && !Pending[Index].bAttached)
            if (auto* Proxy = Pending[Index].Proxy.Get()) ByMesh.Add(Proxy, Index);
    int32 Inside = 0, Outside = 0;
    Owner->ForEachComponent<UStaticMeshComponent>(false, [&](UStaticMeshComponent* Component) {
        if (const int32* Index = ByMesh.Find(Component->GetStaticMesh()))
        {
            const FString Room = Owner->LightRoomAt(Component->Bounds.Origin);
            ++(Room.IsEmpty() ? Outside : Inside);
            Pending[*Index].Components.Emplace(Component, Room);
        }
    });
    int32 Translucent = 0, Rigged = 0, Triangles = 0;
    for (const auto& Pair : ByMesh)
    {
        auto& Item = Pending[Pair.Value];
        Item.bAttached = true;
        Translucent += !Item.Fine.bEnableNanite;
        Rigged += Item.Fine.SocketNode != INDEX_NONE;
        Triangles += Item.Fine.Description.Triangles().Num();
    }
    UE_LOG(LogSCImport, Display,
        TEXT("GEOMETRY_STREAM_ATTACH entity=%s variants=%d triangles=%d room_components=%d outside_components=%d translucent=%d rigged=%d"),
        *Owner->Manifest.Root.EntityName, ByMesh.Num(), Triangles, Inside, Outside, Translucent, Rigged);
}

double FSCGeometryStreaming::Priority(const FBox& Bounds, const FVector& Eye, const FVector& Forward,
    double Fov, double Deviation, bool bInsideRoom, bool bRendered)
{
    if (!Bounds.IsValid) return 0;
    const double Distance = FMath::Sqrt(Bounds.ComputeSquaredDistanceToPoint(Eye));
    const FVector Offset = Bounds.GetCenter() - Eye;
    const double Facing = FVector::DotProduct(Offset.GetSafeNormal(), Forward);
    const double Screen = Deviation / FMath::Max(1.0, Distance);
    if (bInsideRoom && Distance <= RoomPrefetchDistance) return 2.0 + Screen;
    if (!bRendered || Facing < -0.1 || Screen < RelativeScreenError * FMath::Tan(FMath::DegreesToRadians(Fov * 0.5)))
        return 0;
    return 1.0 + Screen;
}

FSCGeometryStreaming::~FSCGeometryStreaming()
{
    if (InFlight.IsSet() && InFlight->SourceCommit.IsValid()) InFlight->SourceCommit.Wait();
}

void FSCGeometryStreaming::BeginPromotion(int32 Index)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(SCGeometryPrepare);
    CSV_SCOPED_TIMING_STAT_EXCLUSIVE(SCGeometryPrepare);
    check(!InFlight.IsSet());
    FPending& Item = InFlight.Emplace().Item;
    Item = MoveTemp(Pending[Index]);
    Pending.RemoveAtSwap(Index);
    UStaticMesh* Proxy = Item.Proxy.Get();
    if (!Item.Owner.IsValid() || !Proxy || Item.Components.IsEmpty())
    {
        PendingTriangles -= Item.Fine.Description.Triangles().Num();
        InFlight.Reset();
        return;
    }
    InFlight->Started = FPlatformTime::Seconds();
    InFlight->Mesh.Reset(FSCMeshes::PrepareFineBuild(Item.Fine, *Proxy,
        MakeUniqueObjectName(Proxy->GetOuter(), UStaticMesh::StaticClass(), TEXT("Fine")), Item.bCollision,
        InFlight->Stats, InFlight->SourceCommit));
    UE_LOG(LogSCImport, Display, TEXT("GEOMETRY_STREAM_PREPARE path=%s ms=%.2f"),
        *Item.Label, (FPlatformTime::Seconds() - InFlight->Started) * 1000);
}

void FSCGeometryStreaming::SubmitPromotion()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(SCGeometrySubmit);
    CSV_SCOPED_TIMING_STAT_EXCLUSIVE(SCGeometrySubmit);
    FFineBuild& Build = InFlight.GetValue();
    const double Start = FPlatformTime::Seconds();
    check(!Build.SourceCommit.IsValid() || Build.SourceCommit.IsCompleted());
    Build.SourceCommit = {};
    FSCMeshes::SubmitFineBuild(Build.Mesh.Get(), Build.Item.Fine);
    Build.bSubmitted = true;
    Build.Stats.BuildSeconds = FPlatformTime::Seconds() - Start;
    UE_LOG(LogSCImport, Display, TEXT("GEOMETRY_STREAM_SUBMIT path=%s ms=%.2f compiling=%d"),
        *Build.Item.Label, Build.Stats.BuildSeconds * 1000, Build.Mesh->IsCompiling());
}

void FSCGeometryStreaming::FinishPromotion(bool bWait)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(SCGeometryApply);
    CSV_SCOPED_TIMING_STAT_EXCLUSIVE(SCGeometryApply);
    const double Start = FPlatformTime::Seconds();
    if (bWait && !InFlight->bSubmitted)
    {
        if (InFlight->SourceCommit.IsValid()) InFlight->SourceCommit.Wait();
        SubmitPromotion();
    }
    FFineBuild Build = MoveTemp(InFlight.GetValue());
    InFlight.Reset();
    const FPending& Item = Build.Item;
    PendingTriangles -= Build.Stats.Triangles;
    ASCVehicle* Owner = Item.Owner.Get();
    UStaticMesh* Proxy = Item.Proxy.Get();
    if (!Owner || !Proxy) return;
    UStaticMesh* Fine = Build.Mesh.Get();
    FString Error;
    if (!FSCMeshes::FinishFineBuild(Fine, Item.Fine, Proxy, Item.bCollision, bWait, Build.Stats, Error))
    {
        ++Failed;
        UE_LOG(LogSCImport, Warning, TEXT("GEOMETRY_STREAM_FAILED path=%s error=%s"), *Item.Label, *Error);
        return;
    }
    for (const auto& Binding : Item.Components)
        if (auto* Component = Binding.Key.Get(); Component && Component->GetStaticMesh() == Proxy)
            Component->SetStaticMesh(Fine);
    for (auto& Mesh : Owner->Meshes)
        if (Mesh == Proxy) Mesh = Fine;
    Owner->SourcePaths.Remove(Proxy);
    Owner->SourcePaths.Add(Fine, Item.Label);
    TArray<FVector3f> Samples;
    if (Owner->MeshSamples.RemoveAndCopyValue(Proxy, Samples))
        Owner->MeshSamples.Add(Fine, MoveTemp(Samples));
    QueueWrite(Owner, Fine, Item.Fine, Build.Stats, Item.Cache, Item.Key);
    ++Promoted;
    const double End = FPlatformTime::Seconds();
    UE_LOG(LogSCImport, Display, TEXT("GEOMETRY_STREAM_PROMOTE entity=%s path=%s triangles=%d components=%d apply_ms=%.2f elapsed_ms=%.2f waited=%d pending=%d"),
        *Owner->Manifest.Root.EntityName, *Item.Label, Build.Stats.Triangles, Item.Components.Num(),
        (End - Start) * 1000, (End - Build.Started) * 1000, bWait, Pending.Num());
}

void FSCGeometryStreaming::QueueWrite(ASCVehicle* Owner, UStaticMesh* Mesh, const FSCDerivedMesh& Data,
    const FSCMeshBuildStats& Stats, TSharedPtr<FSCProcessedCache, ESPMode::ThreadSafe> Cache, const FString& Key)
{
    if (!Cache || Key.IsEmpty() || Stats.Triangles <= 0) return;
    auto& Write = Writes.AddDefaulted_GetRef();
    Write.Owner = Owner;
    Write.Mesh.Reset(Mesh);
    Write.Key = Key;
    Write.Stats = Stats;
    Write.Cache = MoveTemp(Cache);
    Write.Collision.CollisionBytes = Data.CollisionBytes;
    Write.Collision.CollisionVersions = Data.CollisionVersions;
}

void FSCGeometryStreaming::Forget(ASCVehicle* Owner)
{
    if (!Owner) return;
    Pending.RemoveAllSwap([&](const FPending& Item) {
        if (Item.Owner.Get(true) != Owner) return false;
        PendingTriangles -= Item.Fine.Description.Triangles().Num();
        return true;
    });
    if (InFlight.IsSet() && InFlight->Item.Owner.Get(true) == Owner)
    {
        if (InFlight->SourceCommit.IsValid()) InFlight->SourceCommit.Wait();
        PendingTriangles -= InFlight->Stats.Triangles;
        InFlight.Reset();
    }
    Writes.RemoveAll([Owner](const FWrite& Write) { return Write.Owner.Get(true) == Owner; });
}

void FSCGeometryStreaming::WriteOne()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(SCGeometryCacheWrite);
    CSV_SCOPED_TIMING_STAT_EXCLUSIVE(SCGeometryCacheWrite);
    FWrite Write = Writes.Pop(EAllowShrinking::No);
    if (UStaticMesh* Mesh = Write.Mesh.Get(); Mesh &&
        SCProcessedMeshCache::Store(*Write.Cache, Write.Key, *Mesh, Write.Collision, Write.Stats))
    {
        DirtyCaches.Add(Write.Cache);
        ++Written;
    }
}

void FSCGeometryStreaming::Tick(UWorld* World)
{
    if (!World || IsEngineExitRequested()) return;
    // A single fine build bounds memory and leaves worker capacity for rendering and streaming.
    // Cache package saves can drain the engine's compiler queues, so keep them out of this interval.
    if (InFlight.IsSet())
    {
        if (!InFlight->bSubmitted)
        {
            if (!InFlight->SourceCommit.IsValid() || InFlight->SourceCommit.IsCompleted()) SubmitPromotion();
        }
        else if (FSCMeshes::IsFineBuildReady(InFlight->Mesh.Get()))
            FinishPromotion(false);
        return;
    }
    for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
        if (!Pending[Index].Owner.IsValid() || (Pending[Index].bAttached && Pending[Index].Components.IsEmpty()))
        {
            PendingTriangles -= Pending[Index].Fine.Description.Triangles().Num();
            Pending.RemoveAtSwap(Index);
        }
    double Best = 0;
    int32 Candidate = INDEX_NONE;
    const APlayerController* PC = World->GetFirstPlayerController();
    const APlayerCameraManager* Camera = PC ? PC->PlayerCameraManager : nullptr;
    if (Camera)
    {
        const FVector Eye = Camera->GetCameraLocation();
        const FRotator Orientation = Camera->GetCameraRotation();
        const FVector Forward = Orientation.Vector();
        if (!Eye.Equals(LastEye, 5.0) || !Orientation.Equals(LastOrientation, 0.5))
        {
            LastEye = Eye;
            LastOrientation = Orientation;
            LastCameraMotion = FPlatformTime::Seconds();
        }
        const double Fov = Camera->GetFOVAngle();
        const float Now = World->GetTimeSeconds();
        TMap<const ASCVehicle*, FString> Rooms;
        for (int32 Index = 0; Index < Pending.Num(); ++Index)
        {
            const FPending& Item = Pending[Index];
            const ASCVehicle* Owner = Item.Owner.Get();
            if (!Item.bAttached || !Owner || Owner->GetWorld() != World) continue;
            const FString& Room = Rooms.Contains(Owner) ? Rooms.FindChecked(Owner) : Rooms.Add(Owner, Owner->LightRoomAt(Eye));
            for (const auto& Binding : Item.Components)
            {
                const auto* Component = Binding.Key.Get();
                if (!Component || !Component->IsVisible() || Component->bHiddenInGame) continue;
                const float LastOnScreen = Component->GetLastRenderTimeOnScreen();
                const bool bRendered = LastOnScreen > 0 && Now - LastOnScreen < 0.25f;
                const double Scale = Component->GetComponentScale().GetAbsMax();
                const double Score = Priority(Component->Bounds.GetBox(), Eye, Forward, Fov, Item.Deviation * Scale,
                    !Room.IsEmpty() && Room == Binding.Value, bRendered);
                if (Score > Best) { Best = Score; Candidate = Index; }
            }
        }
    }
    if (Candidate != INDEX_NONE) BeginPromotion(Candidate);
    else if (!Writes.IsEmpty() && FPlatformTime::Seconds() - LastCameraMotion > 0.25) WriteOne();
    if (!DirtyCaches.IsEmpty() && (Writes.IsEmpty() || FPlatformTime::Seconds() - LastFlush > 2.0))
    {
        PublishWrites();
        LastFlush = FPlatformTime::Seconds();
    }
}

void FSCGeometryStreaming::FlushForCapture(UWorld* World)
{
    if (InFlight.IsSet()) FinishPromotion(true);
    for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
        if (const ASCVehicle* Owner = Pending[Index].Owner.Get(); Owner && Owner->GetWorld() == World && Pending[Index].bAttached)
        {
            BeginPromotion(Index);
            if (InFlight.IsSet()) FinishPromotion(true);
        }
}

void FSCGeometryStreaming::FinishWrites(UWorld* World)
{
    while (!Writes.IsEmpty() && !IsEngineExitRequested()) WriteOne();
    PublishWrites();
    LogStatus(World);
}

void FSCGeometryStreaming::PublishWrites()
{
    for (const auto& Cache : DirtyCaches) Cache->Flush();
    DirtyCaches.Reset();
}

void FSCGeometryStreaming::DropWrites()
{
    Writes.Reset();
    for (auto& Item : Pending) Item.Key.Empty();
    if (InFlight.IsSet()) InFlight->Item.Key.Empty();
}

void FSCGeometryStreaming::LogStatus(UWorld* World) const
{
    UE_LOG(LogSCImport, Display, TEXT("GEOMETRY_STREAM_STATUS pending=%d pending_triangles=%d promoted=%d failed=%d writes_pending=%d written=%d in_flight=%d"),
        Pending.Num(), PendingTriangles.load(), Promoted, Failed, Writes.Num(), Written, InFlight.IsSet());
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCGeometryPriorityTest, "SCBridge.Import.GeometryStreaming.Priority",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSCGeometryPriorityTest::RunTest(const FString&)
{
    const FVector Eye = FVector::ZeroVector;
    const FVector Forward = FVector::ForwardVector;
    const FBox Near(FVector(400, -50, -50), FVector(500, 50, 50));
    const FBox Behind(FVector(-500, -50, -50), FVector(-400, 50, 50));
    const FBox Far(FVector(100000, -50, -50), FVector(100100, 50, 50));
    TestEqual(TEXT("another room occluded in front"), FSCGeometryStreaming::Priority(Near, Eye, Forward, 90, 5, false, false), 0.0);
    TestEqual(TEXT("distant geometry remains coarse"), FSCGeometryStreaming::Priority(Far, Eye, Forward, 90, 5, false, true), 0.0);
    TestEqual(TEXT("recently seen geometry behind the camera"), FSCGeometryStreaming::Priority(Behind, Eye, Forward, 90, 5, false, true), 0.0);
    TestTrue(TEXT("visible nearby geometry requests detail"), FSCGeometryStreaming::Priority(Near, Eye, Forward, 90, 5, false, true) > 0);
    TestTrue(TEXT("nearby detail in the occupied room prefetches out of view"),
        FSCGeometryStreaming::Priority(Behind, Eye, Forward, 90, 5, true, false) > 0);
    return true;
}
#endif
