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
//   that affects indirect lighting only (bAffectIndirectLightingWhileHidden); the visible strips are unchanged.
//   Development (editor-host) builds only: the opaque instance is a runtime material instance constant.
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
#include "StaticMeshResources.h"
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
    int32 Parts = 0, Triangles = 0, WithCards = 0;
    if (bOn && Vehicle)
    {
        TMap<UMaterialInterface*, UMaterialInterface*> Opaque;
        // One proxy per contiguous part: Lumen's card builder needs the geometry to cover its own bounds (scattered
        // thin strips in one mesh get no card at all).
        for (const FEmissiveSection& E : Collect(Vehicle, MinNits))
        {
            UStaticMeshComponent* C = E.Component;
            const UStaticMesh* Mesh = C->GetStaticMesh();
            const FStaticMeshLODResources& LOD = Mesh->GetRenderData()->LODResources[0];
            const FStaticMeshSection& Section = LOD.Sections[E.Section];
            UMaterialInterface* Material = C->GetMaterial(Section.MaterialIndex);
            const EBlendMode Blend = Material->GetBlendMode();
            if (C->bAffectDynamicIndirectLighting && (Blend == BLEND_Opaque || Blend == BLEND_Masked)) continue; // Lumen sees it
            const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
            const FStaticMeshVertexBuffer& Vertices = LOD.VertexBuffers.StaticMeshVertexBuffer;
            const FColorVertexBuffer& Colours = LOD.VertexBuffers.ColorVertexBuffer;
            if (!Vertices.GetTexCoordData()) continue;
            const bool bColours = Colours.GetNumVertices() == Positions.GetNumVertices() && Colours.GetVertexData();
            const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
            UMaterialInterface*& ProxyMaterial = Opaque.FindOrAdd(Material);
            if (!ProxyMaterial)
            {
                auto* Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage(), NAME_None, RF_Transient);
                Instance->SetParentEditorOnly(Material->GetMaterial());
                Instance->CopyMaterialUniformParametersEditorOnly(Material, true);
                Instance->BasePropertyOverrides.bOverride_BlendMode = true;
                Instance->BasePropertyOverrides.BlendMode = BLEND_Opaque;
                Instance->PostEditChange(); // compiles the opaque permutation
                ProxyMaterial = Instance;
            }
            FSCDerivedMesh Data;
            Data.bEnableNanite = true; // Lumen mesh cards (SCMeshBuild: cards with Nanite)
            Data.bHasTangents = false;
            Data.MaterialNames = {TEXT("Glow")};
            Data.MaterialSourceNames = {FString()};
            Data.MaterialFamilies = {FString()};
            FStaticMeshAttributes A(Data.Description);
            A.Register();
            const int32 UVs = FMath::Min<int32>(Vertices.GetNumTexCoords(), 2);
            A.GetVertexInstanceUVs().SetNumChannels(FMath::Max(UVs, 1));
            const FPolygonGroupID Group = Data.Description.CreatePolygonGroup();
            A.GetPolygonGroupMaterialSlotNames()[Group] = FName(TEXT("Slot_0"));
            for (int32 T : E.SourceTriangles)
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
                Data.Description.CreateTriangle(Group, Corners);
            }
            Data.bHasGeometry = Data.Description.Triangles().Num() > 0;
            if (!Data.bHasGeometry) continue;
            FSCMeshBuildStats Stats;
            FString Error;
            UStaticMesh* ProxyMesh = FSCMeshImporter::Instantiate(Data, GetTransientPackage(), NAME_None, false, nullptr, Stats, Error);
            if (!ProxyMesh) { UE_LOG(LogSCEmissive, Warning, TEXT("EMISSIVE_LUMEN %s: %s"), *GetNameSafe(Mesh), *Error); continue; }
            auto* Proxy = NewObject<UStaticMeshComponent>(Vehicle, NAME_None, RF_Transient);
            Proxy->SetMobility(EComponentMobility::Movable);
            Proxy->SetupAttachment(C);
            Proxy->SetStaticMesh(ProxyMesh);
            Proxy->SetMaterial(0, ProxyMaterial);
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
            ++Parts;
            Triangles += Stats.Triangles;
            WithCards += Stats.Cards > 0;
        }
    }
    UE_LOG(LogSCEmissive, Display, TEXT("EMISSIVE_LUMEN proxies=%d with_cards=%d triangles=%d min_nits=%.1f build_s=%.2f"), Parts, WithCards, Triangles, MinNits,
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
