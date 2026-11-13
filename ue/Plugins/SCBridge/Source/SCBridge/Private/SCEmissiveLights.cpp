// Emissive surfaces vs analytic lights (experiment, development builds; owner request 2026-11-12).
// sc.Emissive.Report [MinNits] [PairCm]: every glowing material section of the vehicle (EmissiveStrength x the
//   luminance of EmissiveColor >= MinNits), split into contiguous parts, each fitted with a rectangle (principal axes in
//   its plane), with its area and the nearest authored light; a part is paired when an authored light lies within
//   PairCm of its rectangle.
// sc.Emissive.Lights <0|1> [MinNits] [PairCm]: 1 adds one unshadowed rect light per unpaired part, shaped like its rectangle (luminance x area
//   = candela, the game's emissive and light units are the same scale; sized and facing like the section, on the
//   section's lighting channels); 0 removes them. Not game data: in the game an emissive surface lights nothing.
// sc.Emissive.Lumen <0|1> [MinNits]: 1 gives Lumen the glow it cannot see: a hidden copy of every glowing section
//   that Lumen ignores (translucent / additive glow cards, or a component kept out of Lumen's scene), built with Lumen
//   mesh cards and drawn with the section's own material as an opaque instance (same parameters, so the same emission),
//   that affects indirect lighting only (bAffectIndirectLightingWhileHidden); the visible strips are unchanged. Parts are
//   clustered per 2 m cell of mesh space, built as one batch, and repeated rigid placements are instanced (LOOK-7).
//   Development (editor-host) builds only: the opaque base per master is a runtime material instance constant.
// The section's texture (emittance map / albedo, alpha gate) is not sampled: the luminance is the material's nominal
// one, an upper bound.
#include "SCEmissiveLights.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "SCVehicle.h"
#include "Components/RectLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "StaticMeshResources.h"
#include "SCInstancing.h"
#include "SCMeshImporter.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#if WITH_EDITOR
#include "Materials/MaterialInstanceConstant.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSCEmissive, Log, All);

namespace
{
struct FEmissiveSection
{
    UStaticMeshComponent* Component = nullptr;
    int32 Section = 0;
    double Nits = 0, AreaM2 = 0;
    FLinearColor Colour = FLinearColor::White;
    FVector Centre = FVector::ZeroVector, Normal = FVector::ZeroVector;
    FBox Bounds = FBox(ForceInit);
    double Width = 0, Height = 0; // fitted rectangle: Width along Axis (a strip's length), Height across, cm
    FVector Axis = FVector::ZeroVector;
    int32 Triangles = 0;
    FString Material;
    TArray<int32> SourceTriangles; // triangle indices within the LOD section
    double NearestLightCm = TNumericLimits<double>::Max();
    FString NearestLight;
};

TArray<TWeakObjectPtr<URectLightComponent>> GSpawned;

TArray<FEmissiveSection> Collect(ASCVehicle* Vehicle, double MinNits)
{
    TArray<FEmissiveSection> Out;
    TArray<UStaticMeshComponent*> Components;
    Vehicle->GetComponents(Components);
    for (UStaticMeshComponent* C : Components)
    {
        const UStaticMesh* Mesh = C ? C->GetStaticMesh() : nullptr;
        // Instanced components (FSCInstancing) hold only non-emissive parts; their single transform is not the parts'.
        if (C && C->IsA<UInstancedStaticMeshComponent>()) continue;
        if (!Mesh || !C->IsVisible() || C->ComponentHasTag(TEXT("SCGameCollision")) || !Mesh->GetRenderData() ||
            Mesh->GetRenderData()->LODResources.IsEmpty())
            continue;
        const FStaticMeshLODResources& LOD = Mesh->GetRenderData()->LODResources[0];
        const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
        const FStaticMeshVertexBuffer& Tangents = LOD.VertexBuffers.StaticMeshVertexBuffer;
        if (!Positions.GetVertexData() || !Tangents.GetTangentData()) continue; // no CPU copy (cooked)
        const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
        const FTransform& World = C->GetComponentTransform();
        for (int32 S = 0; S < LOD.Sections.Num(); ++S)
        {
            const FStaticMeshSection& Section = LOD.Sections[S];
            const UMaterialInterface* Material = C->GetMaterial(Section.MaterialIndex);
            float Strength = 0;
            FLinearColor Colour = FLinearColor::Black;
            if (!Material || !Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(TEXT("EmissiveStrength"))), Strength) ||
                !Material->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(TEXT("EmissiveColor"))), Colour))
                continue;
            const double Nits = double(Strength) * Colour.GetLuminance();
            if (Nits < MinNits || Section.NumTriangles == 0) continue;
            // Contiguous parts: triangles joined through shared corners (positions welded at 0.1 mm, so UV / normal
            // seams do not split a strip). One emitter per part, shaped like it.
            TArray<FVector> Corner;
            TArray<FVector> TriNormal;
            TArray<double> TriArea;
            Corner.Reserve(Section.NumTriangles * 3);
            for (uint32 T = 0; T < Section.NumTriangles; ++T)
            {
                FVector N = FVector::ZeroVector;
                for (int32 K = 0; K < 3; ++K)
                {
                    const uint32 Index = Indices[Section.FirstIndex + T * 3 + K];
                    Corner.Add(World.TransformPosition(FVector(Positions.VertexPosition(Index))));
                    N += World.TransformVectorNoScale(FVector(FVector3f(Tangents.VertexTangentZ(Index))));
                }
                const FVector* P = &Corner[T * 3];
                TriArea.Add(0.5 * FVector::CrossProduct(P[1] - P[0], P[2] - P[0]).Size());
                TriNormal.Add(N.GetSafeNormal());
            }
            TArray<int32> Parent;
            Parent.SetNum(Section.NumTriangles);
            for (int32 T = 0; T < Parent.Num(); ++T) Parent[T] = T;
            auto Find = [&](int32 X) { while (Parent[X] != X) X = Parent[X] = Parent[Parent[X]]; return X; };
            TMap<FIntVector, int32> Welded;
            for (int32 I = 0; I < Corner.Num(); ++I)
            {
                const FIntVector Key(FMath::RoundToInt(Corner[I].X * 100), FMath::RoundToInt(Corner[I].Y * 100), FMath::RoundToInt(Corner[I].Z * 100));
                if (const int32* Other = Welded.Find(Key)) Parent[Find(I / 3)] = Find(*Other / 3);
                else Welded.Add(Key, I);
            }
            TMap<int32, TArray<int32>> Parts;
            for (int32 T = 0; T < Parent.Num(); ++T)
                if (TriArea[T] > 0) Parts.FindOrAdd(Find(T)).Add(T);
            for (const auto& [Root, Tris] : Parts)
            {
                FEmissiveSection E;
                E.Component = C;
                E.Section = S;
                E.Nits = Nits;
                E.Colour = Colour / FMath::Max(Colour.GetMax(), 1e-6f);
                double Area = 0;
                FVector Centroid = FVector::ZeroVector, Normal = FVector::ZeroVector;
                for (int32 T : Tris)
                {
                    Area += TriArea[T];
                    Centroid += (Corner[T * 3] + Corner[T * 3 + 1] + Corner[T * 3 + 2]) / 3 * TriArea[T];
                    Normal += TriNormal[T] * TriArea[T];
                    for (int32 K = 0; K < 3; ++K) E.Bounds += Corner[T * 3 + K];
                }
                E.AreaM2 = Area / 1e4;
                E.Centre = Centroid / Area;
                E.Normal = Normal.GetSafeNormal();
                if (E.Normal.IsNearlyZero()) continue; // a closed shape (both sides) has no facing
                // Principal in-plane axes (2x2 covariance of the corners): a strip's length runs along Axis.
                FVector A = FVector::CrossProduct(E.Normal, FMath::Abs(E.Normal.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector).GetSafeNormal();
                FVector B = FVector::CrossProduct(E.Normal, A);
                double Sxx = 0, Syy = 0, Sxy = 0;
                for (int32 T : Tris)
                    for (int32 K = 0; K < 3; ++K)
                    {
                        const FVector D = Corner[T * 3 + K] - E.Centre;
                        const double X = FVector::DotProduct(D, A), Y = FVector::DotProduct(D, B);
                        Sxx += X * X; Syy += Y * Y; Sxy += X * Y;
                    }
                const double Angle = 0.5 * FMath::Atan2(2 * Sxy, Sxx - Syy);
                E.Axis = (A * FMath::Cos(Angle) + B * FMath::Sin(Angle)).GetSafeNormal();
                const FVector Across = FVector::CrossProduct(E.Normal, E.Axis);
                FVector2D Lo(TNumericLimits<double>::Max()), Hi(TNumericLimits<double>::Lowest());
                for (int32 T : Tris)
                    for (int32 K = 0; K < 3; ++K)
                    {
                        const FVector D = Corner[T * 3 + K] - E.Centre;
                        const FVector2D R(FVector::DotProduct(D, E.Axis), FVector::DotProduct(D, Across));
                        Lo = FVector2D(FMath::Min(Lo.X, R.X), FMath::Min(Lo.Y, R.Y));
                        Hi = FVector2D(FMath::Max(Hi.X, R.X), FMath::Max(Hi.Y, R.Y));
                    }
                E.Width = FMath::Max(1.0, Hi.X - Lo.X);   // along the strip
                E.Height = FMath::Max(1.0, Hi.Y - Lo.Y);  // across it
                E.Centre += E.Axis * (Lo.X + Hi.X) / 2 + Across * (Lo.Y + Hi.Y) / 2; // centre of the fitted rectangle
                E.Triangles = Tris.Num();
                E.SourceTriangles = Tris;
                E.Material = FString::Printf(TEXT("%s/%s"), *GetNameSafe(Material->GetMaterial()), *StaticEnum<EBlendMode>()->GetNameStringByValue(Material->GetBlendMode()));
                // Paired: an authored light within PairCm of the fitted rectangle (distance measured in its frame).
                for (const ULightComponent* Light : Vehicle->Lights)
                    if (Light)
                    {
                        const FVector D = Light->GetComponentLocation() - E.Centre;
                        const double X = FMath::Max(0.0, FMath::Abs(FVector::DotProduct(D, E.Axis)) - E.Width / 2);
                        const double Y = FMath::Max(0.0, FMath::Abs(FVector::DotProduct(D, Across)) - E.Height / 2);
                        const double Z = FVector::DotProduct(D, E.Normal);
                        const double Dist = FMath::Sqrt(X * X + Y * Y + Z * Z);
                        if (Dist < E.NearestLightCm) { E.NearestLightCm = Dist; E.NearestLight = Light->GetName(); }
                    }
                Out.Add(MoveTemp(E));
            }
        }
    }
    Out.Sort([](const FEmissiveSection& A, const FEmissiveSection& B) { return A.Nits * A.AreaM2 > B.Nits * B.AreaM2; });
    return Out;
}

ASCVehicle* FindVehicle(UWorld* World)
{
    for (TActorIterator<ASCVehicle> It(World); It; ++It)
        if (!It->Lights.IsEmpty()) return *It;
    return nullptr;
}

void Report(UWorld* World, double MinNits, double PairCm, bool bLog)
{
    ASCVehicle* Vehicle = FindVehicle(World);
    if (!Vehicle) { UE_LOG(LogSCEmissive, Warning, TEXT("EMISSIVE no vehicle")); return; }
    const TArray<FEmissiveSection> Sections = Collect(Vehicle, MinNits);
    int32 Paired = 0;
    double AreaPaired = 0, AreaUnpaired = 0, FluxPaired = 0, FluxUnpaired = 0;
    for (const FEmissiveSection& E : Sections)
    {
        const bool bPaired = E.NearestLightCm <= PairCm;
        Paired += bPaired;
        (bPaired ? AreaPaired : AreaUnpaired) += E.AreaM2;
        (bPaired ? FluxPaired : FluxUnpaired) += E.Nits * E.AreaM2;
        if (bLog)
            UE_LOG(LogSCEmissive, Display, TEXT("EMISSIVE_PART mesh=%s section=%d tris=%d nits=%.1f area_m2=%.4f cd=%.1f size_cm=%.0fx%.0f centre=%s normal=%s nearest_light_cm=%.0f light=%s paired=%d material=%s"),
                   *GetNameSafe(E.Component->GetStaticMesh()), E.Section, E.Triangles, E.Nits, E.AreaM2, E.Nits * E.AreaM2, E.Width, E.Height,
                   *E.Centre.ToCompactString(), *E.Normal.ToCompactString(), E.NearestLightCm, *E.NearestLight, bPaired, *E.Material);
    }
    UE_LOG(LogSCEmissive, Display, TEXT("EMISSIVE_SUMMARY min_nits=%.1f pair_cm=%.0f parts=%d paired=%d unpaired=%d area_m2 paired=%.3f unpaired=%.3f cd paired=%.0f unpaired=%.0f lights=%d"),
           MinNits, PairCm, Sections.Num(), Paired, Sections.Num() - Paired, AreaPaired, AreaUnpaired, FluxPaired, FluxUnpaired, Vehicle->Lights.Num());
}

void SetLights(UWorld* World, bool bOn, double MinNits, double PairCm)
{
    for (const auto& Light : GSpawned)
        if (Light.IsValid()) Light->DestroyComponent();
    GSpawned.Reset();
    ASCVehicle* Vehicle = FindVehicle(World);
    if (!bOn || !Vehicle) { UE_LOG(LogSCEmissive, Display, TEXT("EMISSIVE_LIGHTS spawned=0")); return; }
    double Candela = 0;
    for (const FEmissiveSection& E : Collect(Vehicle, MinNits))
    {
        if (E.NearestLightCm <= PairCm) continue;
        auto* Light = NewObject<URectLightComponent>(Vehicle, NAME_None, RF_Transient);
        Light->SetMobility(EComponentMobility::Movable);
        Light->SetupAttachment(E.Component);
        Light->SetIntensityUnits(ELightUnits::Candelas);
        Light->SetIntensity(E.Nits * E.AreaM2);
        Light->SetLightColor(E.Colour);
        Light->SetSourceWidth(E.Width);
        Light->SetSourceHeight(E.Height);
        // Reach: where the on-axis illuminance falls to 0.1 lux (viewer choice), at least the section's own size.
        Light->SetAttenuationRadius(FMath::Clamp(FMath::Sqrt(E.Nits * E.AreaM2 / 0.1) * 100, FMath::Max(E.Width, E.Height), 2000.0));
        Light->SetCastShadows(false);
        Light->LightingChannels = E.Component->LightingChannels;
        Light->RegisterComponent();
        // Rect light: emits along +X, SourceWidth along Y, SourceHeight along Z.
        Light->SetWorldLocationAndRotation(E.Centre + E.Normal * 0.5, FRotationMatrix::MakeFromXY(E.Normal, E.Axis).Rotator());
        Light->ComponentTags.Add(TEXT("SCEmissiveExperiment"));
        GSpawned.Add(Light);
        Candela += E.Nits * E.AreaM2;
    }
    UE_LOG(LogSCEmissive, Display, TEXT("EMISSIVE_LIGHTS spawned=%d cd=%.0f min_nits=%.1f pair_cm=%.0f"), GSpawned.Num(), Candela, MinNits, PairCm);
}

TMap<TWeakObjectPtr<ASCVehicle>, TArray<TWeakObjectPtr<UStaticMeshComponent>>> GLumenProxyMap;

void SetLumenProxies(ASCVehicle* Vehicle, bool bOn, double MinNits)
{
    if (TArray<TWeakObjectPtr<UStaticMeshComponent>>* Old = GLumenProxyMap.Find(Vehicle))
        for (const auto& Proxy : *Old)
            if (Proxy.IsValid()) Proxy->DestroyComponent();
    GLumenProxyMap.Remove(Vehicle);
    TArray<TWeakObjectPtr<UStaticMeshComponent>>& GLumenProxies = GLumenProxyMap.FindOrAdd(Vehicle);
#if WITH_EDITOR
    const double Start = FPlatformTime::Seconds();
    int32 Parts = 0, ClusterCount = 0, Triangles = 0, WithCards = 0, Built = 0, Instanced = 0;
    double CollectSeconds = 0, DeriveSeconds = 0, BuildSeconds = 0;
    if (bOn && Vehicle)
    {
        TMap<UMaterialInterface*, UMaterialInterface*> Opaque;
        TMap<UMaterial*, UMaterialInstanceConstant*> OpaqueBase;
        // Proxies are clusters of contiguous parts: Lumen's card builder needs the geometry to cover its own bounds
        // (scattered thin strips across a large mesh get no card at all), but one mesh per part does not scale (a large
        // ship has ~16k glowing parts, each a primitive). The parts of one source section inside one ClusterCm cell of
        // mesh space share a proxy mesh (up to 12 cards each); a cluster is the same in every placement of its mesh, so
        // its proxy mesh is built once, and the rigid placements of one proxy are drawn as instances.
        constexpr double ClusterCm = 200.0; // a cell budget (not game data): small enough for cards to cover a cluster
        struct FCluster { const FEmissiveSection* First = nullptr; TArray<int32> Triangles; };
        TMap<FString, FCluster> Clusters; // per component, section and mesh-space cell
        const TArray<FEmissiveSection> Sections = Collect(Vehicle, MinNits); // outlives the clusters that point into it
        for (const FEmissiveSection& E : Sections)
        {
            UStaticMeshComponent* C = E.Component;
            const FStaticMeshSection& Section = C->GetStaticMesh()->GetRenderData()->LODResources[0].Sections[E.Section];
            const UMaterialInterface* Material = C->GetMaterial(Section.MaterialIndex);
            const EBlendMode Blend = Material->GetBlendMode();
            if (C->bAffectDynamicIndirectLighting && (Blend == BLEND_Opaque || Blend == BLEND_Masked)) continue; // Lumen sees it
            if (E.SourceTriangles.IsEmpty()) continue;
            const FVector Local = C->GetComponentTransform().InverseTransformPosition(E.Centre) / ClusterCm;
            FCluster& Cluster = Clusters.FindOrAdd(FString::Printf(TEXT("%p/%d/%d,%d,%d"), C, E.Section, FMath::FloorToInt32(Local.X),
                                                                   FMath::FloorToInt32(Local.Y), FMath::FloorToInt32(Local.Z)));
            if (!Cluster.First) Cluster.First = &E;
            Cluster.Triangles.Append(E.SourceTriangles);
            ++Parts;
        }
        ClusterCount = Clusters.Num();
        const double DeriveStart = FPlatformTime::Seconds();
        CollectSeconds = DeriveStart - Start;
        // Pass 1: one derived mesh per distinct cluster (same mesh, section and triangles in every placement).
        struct FPlacement { UStaticMeshComponent* Component; UMaterialInterface* Material; int32 Mesh; };
        TArray<FPlacement> Placements;
        TArray<FSCDerivedMesh> Derived;
        TMap<FString, int32> MeshIndex;
        for (auto& [ClusterKey, Cluster] : Clusters)
        {
            const FEmissiveSection& E = *Cluster.First;
            Cluster.Triangles.Sort();
            UStaticMeshComponent* C = E.Component;
            const UStaticMesh* Mesh = C->GetStaticMesh();
            const FStaticMeshLODResources& LOD = Mesh->GetRenderData()->LODResources[0];
            const FStaticMeshSection& Section = LOD.Sections[E.Section];
            UMaterialInterface* Material = C->GetMaterial(Section.MaterialIndex);
            const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
            const FStaticMeshVertexBuffer& Vertices = LOD.VertexBuffers.StaticMeshVertexBuffer;
            const FColorVertexBuffer& Colours = LOD.VertexBuffers.ColorVertexBuffer;
            if (!Vertices.GetTexCoordData()) continue;
            UMaterialInterface*& ProxyMaterial = Opaque.FindOrAdd(Material);
            if (!ProxyMaterial)
            {
                // The opaque permutation is compiled once per master (a constant instance with the blend override); each
                // glowing material is a dynamic instance of it with the material's own parameters (no compile).
                UMaterial* Master = Material->GetMaterial();
                UMaterialInstanceConstant*& Base = OpaqueBase.FindOrAdd(Master);
                if (!Base)
                {
                    Base = NewObject<UMaterialInstanceConstant>(GetTransientPackage(), NAME_None, RF_Transient);
                    Base->SetParentEditorOnly(Master);
                    Base->BasePropertyOverrides.bOverride_BlendMode = true;
                    Base->BasePropertyOverrides.BlendMode = BLEND_Opaque;
                    Base->PostEditChange(); // compiles the opaque permutation
                }
                UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Base, Vehicle);
                Instance->CopyMaterialUniformParameters(Material);
                ProxyMaterial = Instance;
            }
            const FString MeshKey = FString::Printf(TEXT("%p/%d/%d/%d/%d"), Mesh, E.Section, Cluster.Triangles[0], Cluster.Triangles.Last(),
                                                    Cluster.Triangles.Num());
            if (const int32* Existing = MeshIndex.Find(MeshKey))
            {
                Placements.Add({C, ProxyMaterial, *Existing});
                continue;
            }
            const bool bColours = Colours.GetNumVertices() == Positions.GetNumVertices() && Colours.GetVertexData();
            const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
            FSCDerivedMesh& Data = Derived.AddDefaulted_GetRef();
            Data.bEnableNanite = true; // Lumen mesh cards (SCMeshBuild: cards with Nanite)
            Data.bHasTangents = false;
            Data.MaterialNames = {TEXT("Glow")};
            Data.MaterialSourceNames = {FString()};
            Data.MaterialFamilies = {FString()};
            FStaticMeshAttributes A(Data.Description);
            A.Register();
            const int32 UVs = FMath::Min<int32>(Vertices.GetNumTexCoords(), 2);
            A.GetVertexInstanceUVs().SetNumChannels(FMath::Max(UVs, 1));
            const FPolygonGroupID PolygonGroup = Data.Description.CreatePolygonGroup();
            A.GetPolygonGroupMaterialSlotNames()[PolygonGroup] = FName(TEXT("Slot_0"));
            for (int32 T : Cluster.Triangles)
            {
                TArray<FVertexInstanceID, TInlineAllocator<3>> Corners;
                for (int32 K = 0; K < 3; ++K)
                {
                    const uint32 Index = Indices[Section.FirstIndex + T * 3 + K];
                    const FVertexID Vertex = Data.Description.CreateVertex();
                    A.GetVertexPositions()[Vertex] = Positions.VertexPosition(Index);
                    const FVertexInstanceID VI = Data.Description.CreateVertexInstance(Vertex);
                    A.GetVertexInstanceNormals()[VI] = FVector3f(Vertices.VertexTangentZ(Index));
                    A.GetVertexInstanceTangents()[VI] = FVector3f(Vertices.VertexTangentX(Index));
                    A.GetVertexInstanceBinormalSigns()[VI] = 1;
                    A.GetVertexInstanceColors()[VI] = bColours ? FVector4f(FLinearColor(Colours.VertexColor(Index))) : FVector4f(1, 1, 1, 1);
                    for (int32 UV = 0; UV < UVs; ++UV) A.GetVertexInstanceUVs().Set(VI, UV, Vertices.GetVertexUV(Index, UV));
                    Corners.Add(VI);
                }
                Data.Description.CreateTriangle(PolygonGroup, Corners);
            }
            Data.bHasGeometry = Data.Description.Triangles().Num() > 0;
            if (!Data.bHasGeometry) { Derived.Pop(); continue; }
            MeshIndex.Add(MeshKey, Derived.Num() - 1);
            Placements.Add({C, ProxyMaterial, Derived.Num() - 1});
        }
        // Pass 2: build every proxy mesh together (the editor compiles them in parallel).
        DeriveSeconds = FPlatformTime::Seconds() - DeriveStart;
        const double BuildStart = FPlatformTime::Seconds();
        TArray<const FSCDerivedMesh*> ToBuild;
        for (const FSCDerivedMesh& D : Derived) ToBuild.Add(&D);
        TArray<FSCMeshBuildStats> BuildStats;
        TArray<FString> Errors;
        const TArray<UStaticMesh*> ProxyMeshes = FSCMeshImporter::InstantiateBatch(ToBuild, GetTransientPackage(), BuildStats, Errors);
        for (int32 I = 0; I < ProxyMeshes.Num(); ++I)
        {
            if (!ProxyMeshes[I]) { UE_LOG(LogSCEmissive, Warning, TEXT("EMISSIVE_LUMEN proxy %d: %s"), I, *Errors[I]); continue; }
            ++Built;
            Triangles += BuildStats[I].Triangles;
            WithCards += BuildStats[I].Cards > 0;
        }
        BuildSeconds = FPlatformTime::Seconds() - BuildStart;
        // Pass 3: rigid placements as instances, parts under a rig as components that follow it.
        struct FGroup { UStaticMesh* Mesh; UMaterialInterface* Material; FLightingChannels Channels; TArray<FTransform> Instances; };
        TMap<FString, FGroup> Groups;
        USceneComponent* Root = Vehicle->GetRootComponent();
        for (const FPlacement& Placement : Placements)
        {
            UStaticMesh* ProxyMesh = ProxyMeshes[Placement.Mesh];
            if (!ProxyMesh) continue;
            UStaticMeshComponent* C = Placement.Component;
            if (!FSCInstancing::UnderRig(C))
            {
                const FLightingChannels& L = C->LightingChannels;
                FGroup& Group = Groups.FindOrAdd(FString::Printf(TEXT("%p/%p/%d%d%d"), ProxyMesh, Placement.Material, L.bChannel0, L.bChannel1, L.bChannel2),
                                                 FGroup{ProxyMesh, Placement.Material, L, {}});
                Group.Instances.Add(C->GetComponentTransform().GetRelativeTransform(Root->GetComponentTransform()));
                continue;
            }
            auto* Proxy = NewObject<UStaticMeshComponent>(Vehicle, NAME_None, RF_Transient);
            Proxy->SetMobility(EComponentMobility::Movable);
            Proxy->SetupAttachment(C);
            Proxy->SetStaticMesh(ProxyMesh);
            Proxy->SetMaterial(0, Placement.Material);
            Proxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Proxy->CastShadow = false;
            Proxy->bAffectDynamicIndirectLighting = true;
            Proxy->bAffectDistanceFieldLighting = true;
            Proxy->bAffectIndirectLightingWhileHidden = true; // in Lumen's scene (indirect rays only), never drawn
            Proxy->SetHiddenInGame(true);
            Proxy->LightingChannels = C->LightingChannels;
            Proxy->ComponentTags.Add(TEXT("SCEmissiveExperiment"));
            Proxy->RegisterComponent();
            GLumenProxies.Add(Proxy);
        }
        for (auto& [Key, Group] : Groups)
        {
            auto* Proxy = NewObject<UInstancedStaticMeshComponent>(Vehicle, NAME_None, RF_Transient);
            Proxy->SetMobility(EComponentMobility::Movable); // the actor is moved onto the floor after import
            Proxy->SetupAttachment(Root);
            Proxy->SetStaticMesh(Group.Mesh);
            Proxy->SetMaterial(0, Group.Material);
            Proxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Proxy->CastShadow = false;
            Proxy->bAffectDynamicIndirectLighting = true;
            Proxy->bAffectDistanceFieldLighting = true;
            Proxy->bAffectIndirectLightingWhileHidden = true;
            Proxy->SetHiddenInGame(true);
            Proxy->LightingChannels = Group.Channels;
            Proxy->ComponentTags.Add(TEXT("SCEmissiveExperiment"));
            Proxy->RegisterComponent();
            Proxy->AddInstances(Group.Instances, false, false);
            GLumenProxies.Add(Proxy);
            ++Instanced;
        }
    }
    UE_LOG(LogSCEmissive, Display, TEXT("EMISSIVE_LUMEN parts=%d clusters=%d meshes=%d with_cards=%d triangles=%d instanced_components=%d components=%d min_nits=%.1f collect_s=%.2f derive_s=%.2f mesh_build_s=%.2f build_s=%.2f"),
           Parts, ClusterCount, Built, WithCards, Triangles, Instanced, GLumenProxies.Num(), MinNits, CollectSeconds, DeriveSeconds, BuildSeconds,
           FPlatformTime::Seconds() - Start);
#else
    UE_LOG(LogSCEmissive, Warning, TEXT("EMISSIVE_LUMEN needs a development editor-host build"));
#endif
}

double Arg(const TArray<FString>& Args, int32 I, double Default) { return Args.IsValidIndex(I) ? FCString::Atod(*Args[I]) : Default; }

FAutoConsoleCommandWithWorldAndArgs GSCEmissiveReport(
    TEXT("sc.Emissive.Report"), TEXT("sc.Emissive.Report [MinNits=1] [PairCm=50] [log sections 0|1]: glowing sections vs authored lights"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        Report(World, Arg(Args, 0, 1), Arg(Args, 1, 50), Arg(Args, 2, 1) != 0);
    }));
FAutoConsoleCommandWithWorldAndArgs GSCEmissiveLumen(
    TEXT("sc.Emissive.Lumen"), TEXT("sc.Emissive.Lumen <0|1> [MinNits=1]: hidden opaque copies of glowing sections Lumen cannot see"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        SetLumenProxies(FindVehicle(World), Arg(Args, 0, 1) != 0, Arg(Args, 1, 1));
    }));
FAutoConsoleCommandWithWorldAndArgs GSCEmissiveLights(
    TEXT("sc.Emissive.Lights"), TEXT("sc.Emissive.Lights <0|1> [MinNits=1] [PairCm=50]: experimental rect lights on unpaired glowing sections"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        SetLights(World, Arg(Args, 0, 1) != 0, Arg(Args, 1, 1), Arg(Args, 2, 50));
    }));
}

void FSCEmissive::SetLumen(ASCVehicle* Vehicle, bool bOn, double MinNits) { SetLumenProxies(Vehicle, bOn, MinNits); }
