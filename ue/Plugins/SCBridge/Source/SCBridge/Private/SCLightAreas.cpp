// Room-volume parsing, containment and the two-channel light confinement (see SCLightAreas.h, WP-U3b §12).
#include "SCLightAreas.h"
#include "SCVehicle.h"
#include "SCImporter.h"
#include "SCLightImporter.h"
#include "SCRigComponent.h"
#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "MeshDescription.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "UObject/UObjectIterator.h"
namespace
{
// Trimesh containment, the exporter's room_containment.rs: parity of the triangles a +Z ray crosses.
// Signed area of the directed edge A -> B about the origin and whether the origin is to its left; false for an
// edge with no extent. An origin exactly on the edge's line is placed as if it sat an infinitesimal (e, e^2)
// further along +X, +Y. The area is evaluated in one vertex order whichever way the edge is traversed, so the
// two triangles sharing an edge always get opposite sides.
bool EdgeSide(const FVector& A, const FVector& B, double& Area, bool& bLeft)
{
    const bool bReversed = B.X < A.X || (B.X == A.X && B.Y < A.Y);
    const FVector& First = bReversed ? B : A;
    const FVector& Second = bReversed ? A : B;
    const double Forward = First.X * Second.Y - First.Y * Second.X;
    Area = bReversed ? -Forward : Forward;
    if (Area != 0) { bLeft = Area > 0; return true; }
    if (A.Y != B.Y) { bLeft = A.Y > B.Y; return true; }
    if (A.X != B.X) { bLeft = B.X > A.X; return true; }
    return false;
}
// Whether the +Z ray from the origin crosses triangle ABC: the origin is on the same side of all three edges
// seen along the ray, and the surface lies ahead. No edge ever answers "on the edge", so a ray through an edge
// or a vertex of a closed soup counts once per side, and a face seen edge-on is never crossed.
bool UpwardRayCrosses(const FVector& A, const FVector& B, const FVector& C)
{
    double AB, BC, CA;
    bool bAB, bBC, bCA;
    if (!EdgeSide(A, B, AB, bAB) || !EdgeSide(B, C, BC, bBC) || !EdgeSide(C, A, CA, bCA) || bAB != bBC || bBC != bCA) return false;
    const double Area = AB + BC + CA; // each edge weights the opposite corner
    return Area != 0 && (BC * A.Z + CA * B.Z + AB * C.Z) / Area > 0;
}
bool Vector3(const TSharedPtr<FJsonValue>& Value, FVector& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!Value || !Value->TryGetArray(A) || A->Num() != 3) return false;
    double V[3];
    for (int32 I = 0; I < 3; ++I)
        if (!(*A)[I]->TryGetNumber(V[I]) || !FMath::IsFinite(V[I])) return false;
    Out = FVector(V[0], V[1], V[2]);
    return true;
}
bool Vectors(const TSharedPtr<FJsonObject>& J, const TCHAR* Key, TArray<FVector>& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!J->TryGetArrayField(Key, A)) return false;
    Out.Reset(A->Num());
    for (const auto& V : *A)
        if (!Vector3(V, Out.AddDefaulted_GetRef())) return false;
    return true;
}
}

bool FSCRoomVolume::Parse(const FString& Json, FSCRoomVolume& Out, FString& Error)
{
    Out = FSCRoomVolume();
    TSharedPtr<FJsonObject> J;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), J) || !J) { Error = TEXT("room volume is not a JSON object"); return false; }
    J->TryGetStringField(TEXT("room"), Out.Room);
    J->TryGetStringField(TEXT("room_name"), Out.RoomName);
    J->TryGetStringField(TEXT("source"), Out.Source);
    // Column-major 4x4 (segment -> container). UE matrices use row vectors, so column C is row C.
    const TArray<TSharedPtr<FJsonValue>>* Columns;
    FMatrix ToContainer = FMatrix::Identity;
    if (!J->TryGetArrayField(TEXT("transform"), Columns) || Columns->Num() != 4) { Error = TEXT("room volume transform is not 4x4"); return false; }
    for (int32 C = 0; C < 4; ++C)
    {
        const TArray<TSharedPtr<FJsonValue>>* Column;
        if (!(*Columns)[C]->TryGetArray(Column) || Column->Num() != 4) { Error = TEXT("room volume transform is not 4x4"); return false; }
        for (int32 R = 0; R < 4; ++R)
        {
            double V;
            if (!(*Column)[R]->TryGetNumber(V) || !FMath::IsFinite(V)) { Error = TEXT("room volume transform is not numeric"); return false; }
            ToContainer.M[C][R] = V;
        }
    }
    if (FMath::IsNearlyZero(ToContainer.Determinant(), 1e-12)) { Error = TEXT("room volume transform is singular"); return false; }
    Out.FromContainer = ToContainer.Inverse();
    FString Kind;
    J->TryGetStringField(TEXT("kind"), Kind);
    if (Kind == TEXT("box"))
    {
        Out.Shape = EShape::Box;
        if (!Vector3(J->TryGetField(TEXT("box_min")), Out.Min) || !Vector3(J->TryGetField(TEXT("box_max")), Out.Max))
        { Error = TEXT("room volume box needs box_min and box_max"); return false; }
    }
    else if (Kind == TEXT("prism"))
    {
        Out.Shape = EShape::Prism;
        if (!Vectors(J, TEXT("points"), Out.Points) || Out.Points.Num() < 3) { Error = TEXT("room volume prism needs a polygon"); return false; }
        if (!J->TryGetNumberField(TEXT("height"), Out.Height) || !FMath::IsFinite(Out.Height)) { Error = TEXT("room volume prism needs a height"); return false; }
    }
    else if (Kind == TEXT("trimesh"))
    {
        Out.Shape = EShape::Trimesh;
        if (!Vectors(J, TEXT("vertices"), Out.Points) || Out.Points.IsEmpty() || Out.Points.Num() % 3 != 0)
        { Error = TEXT("room volume trimesh needs three vertices per triangle"); return false; }
    }
    else { Error = TEXT("unsupported room volume kind '") + Kind + TEXT("'"); return false; }
    return true;
}

bool FSCRoomVolume::Contains(const FVector& ContainerPointM) const
{
    const FVector P = FromContainer.TransformPosition(ContainerPointM);
    if (P.ContainsNaN()) return false;
    switch (Shape)
    {
    case EShape::Box:
        return P.X >= Min.X && P.X <= Max.X && P.Y >= Min.Y && P.Y <= Max.Y && P.Z >= Min.Z && P.Z <= Max.Z;
    case EShape::Prism:
    {
        double Base = TNumericLimits<double>::Max();
        for (const FVector& Q : Points) Base = FMath::Min(Base, Q.Z);
        if (P.Z < Base || P.Z > Base + Height) return false;
        bool bInside = false; // even-odd rule on the xy polygon
        for (int32 I = 0; I < Points.Num(); ++I)
        {
            const FVector& A = Points[I];
            const FVector& B = Points[(I + 1) % Points.Num()];
            if ((A.Y > P.Y) != (B.Y > P.Y) && P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y) + A.X) bInside = !bInside;
        }
        return bInside;
    }
    case EShape::Trimesh:
    {
        int32 Crossings = 0; // an odd count is inside a closed soup
        for (int32 I = 0; I + 2 < Points.Num(); I += 3)
            Crossings += UpwardRayCrosses(Points[I] - P, Points[I + 1] - P, Points[I + 2] - P);
        return Crossings % 2 == 1;
    }
    }
    return false;
}

bool FSCLightArea::Contains(const FVector& World) const
{
    const USceneComponent* A = Anchor.Get();
    if (!A) return false;
    const FVector Point = AnchorToContainer.TransformPosition(A->GetComponentTransform().InverseTransformPosition(World));
    for (const FSCRoomVolume& Volume : Volumes)
        if (Volume.Contains(Point)) return true;
    return false;
}

TArray<FVector3f> FSCLightAreas::Sample(const FMeshDescription& Description)
{
    TArray<FVector3f> Out;
    const int32 Count = Description.Vertices().Num();
    if (Count == 0) return Out;
    const auto Positions = FStaticMeshConstAttributes(Description).GetVertexPositions();
    const int32 Wanted = FMath::Min(Count, MaxSamples);
    Out.Reserve(Wanted);
    int32 Index = 0;
    for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
    {
        // Sample K is vertex floor(K * Count / Wanted): an even stride over the vertex order.
        if (Out.Num() < Wanted && Index == int32(int64(Out.Num()) * Count / Wanted)) Out.Add(Positions[Vertex]);
        ++Index;
    }
    return Out;
}

void FSCLightAreas::Register(ASCVehicle* Vehicle, USceneComponent* Anchor, const TArray<FString>& VolumeJson, bool bHelperFrame,
                             TArray<FString>& Warnings)
{
    if (!Vehicle || !Anchor || VolumeJson.IsEmpty()) return;
    FSCLightArea Area;
    Area.Anchor = Anchor;
    // The same container -> anchor mapping the light importer applies to light positions (metres -> cm).
    const FVector X = FSCLightImporter::ContainerBasis(FVector(1, 0, 0), bHelperFrame) * 100;
    const FVector Y = FSCLightImporter::ContainerBasis(FVector(0, 1, 0), bHelperFrame) * 100;
    const FVector Z = FSCLightImporter::ContainerBasis(FVector(0, 0, 1), bHelperFrame) * 100;
    Area.AnchorToContainer = FMatrix(FPlane(X, 0), FPlane(Y, 0), FPlane(Z, 0), FPlane(0, 0, 0, 1)).Inverse();
    for (const FString& Text : VolumeJson)
    {
        FString Error;
        FSCRoomVolume Volume;
        if (FSCRoomVolume::Parse(Text, Volume, Error))
            Area.Volumes.Add(MoveTemp(Volume));
        else
            Warnings.Add(Anchor->GetName() + TEXT(": ") + Error);
    }
    if (!Area.Volumes.IsEmpty()) Vehicle->LightAreas.Add(MoveTemp(Area));
}

void FSCLightAreas::SetChannels(UPrimitiveComponent* Component, bool bInside)
{
    if (Component) Component->SetLightingChannels(!bInside, bInside, false);
}

void FSCLightAreas::Apply(ASCVehicle* Vehicle)
{
    if (!Vehicle) return;
    int32 Volumes = 0;
    for (const FSCLightArea& Area : Vehicle->LightAreas) Volumes += Area.Volumes.Num();
    if (Volumes == 0)
    {
        UE_LOG(LogSCImport, Display, TEXT("LIGHT_AREAS volumes=0"));
        return;
    }
    // Lights: confined to their side when authored area-only; everything else is the game's AffectsEverything.
    int32 LightsInside = 0, LightsOutside = 0, LightsEverywhere = 0;
    for (FSCImportedLightState& State : Vehicle->LightStates)
    {
        ULightComponent* Light = State.Component;
        if (!Light) continue;
        State.bInsideArea = Vehicle->IsInsideLightArea(Light->GetComponentLocation());
        if (!State.bAreaOnly)
        {
            Light->SetLightingChannels(true, true, false);
            ++LightsEverywhere;
        }
        else
        {
            Light->SetLightingChannels(!State.bInsideArea, State.bInsideArea, false);
            ++(State.bInsideArea ? LightsInside : LightsOutside);
        }
        UE_LOG(LogSCImport, Verbose, TEXT("LIGHT_AREA_LIGHT name=%s owner=%s area_only=%d inside=%d"), *State.Name, *State.OwnerEntity,
               State.bAreaOnly, State.bInsideArea);
    }
    // Surfaces: the game gates per pixel; a component takes the side most of its sampled vertices are on.
    int32 MeshesInside = 0, MeshesOutside = 0;
    TInlineComponentArray<UPrimitiveComponent*> Primitives(Vehicle);
    for (UPrimitiveComponent* Component : Primitives)
    {
        const UObject* Asset = nullptr;
        if (const auto* Static = Cast<UStaticMeshComponent>(Component)) Asset = Static->GetStaticMesh();
        else if (const auto* Skinned = Cast<USkinnedMeshComponent>(Component)) Asset = Skinned->GetSkinnedAsset();
        const TArray<FVector3f>* Samples = Asset ? Vehicle->MeshSamples.Find(Asset) : nullptr;
        int32 Inside = 0, Total = 0;
        if (Samples && !Samples->IsEmpty())
        {
            const FTransform& ToWorld = Component->GetComponentTransform();
            for (const FVector3f& Sample : *Samples)
                Inside += Vehicle->IsInsideLightArea(ToWorld.TransformPosition(FVector(Sample)));
            Total = Samples->Num();
        }
        else
        {
            Inside = Vehicle->IsInsideLightArea(Component->Bounds.Origin);
            Total = 1;
        }
        const bool bInside = 2 * Inside > Total;
        SetChannels(Component, bInside);
        ++(bInside ? MeshesInside : MeshesOutside);
        UE_LOG(LogSCImport, Verbose, TEXT("LIGHT_AREA_MESH component=%s asset=%s inside=%d/%d side=%s"), *Component->GetName(),
               Component->ComponentTags.Num() ? *Component->ComponentTags[0].ToString() : *GetNameSafe(Asset), Inside, Total,
               bInside ? TEXT("in") : TEXT("out"));
    }
    // Lights that belong to no vehicle (hangar panels, sun) are not area lights: they reach both sides.
    int32 Environment = 0;
    for (TObjectIterator<ULightComponent> It; It; ++It)
    {
        ULightComponent* Light = *It;
        if (Light->GetWorld() != Vehicle->GetWorld() || Cast<ASCVehicle>(Light->GetOwner()) || Light->LightingChannels.bChannel1) continue;
        Light->SetLightingChannels(Light->LightingChannels.bChannel0, true, Light->LightingChannels.bChannel2);
        ++Environment;
    }
    UE_LOG(LogSCImport, Display,
           TEXT("LIGHT_AREAS volumes=%d lights_inside=%d lights_outside=%d lights_everywhere=%d meshes_inside=%d meshes_outside=%d environment_lights=%d"),
           Volumes, LightsInside, LightsOutside, LightsEverywhere, MeshesInside, MeshesOutside, Environment);
}
