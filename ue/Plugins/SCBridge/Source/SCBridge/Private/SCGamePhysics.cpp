// WP-P3: the game's own collision geometry. The exporter writes each node's CryPhysics colliders
// (StatObjPhysics chunk) as glTF node `extras.sc_physics`; this file turns one record into triangles in the
// node's frame and builds the collision-only mesh of an owner. Convex primitives are tessellated (UE's character
// sweeps run against the same trimesh path as the game's walk proxies).
#include "SCMeshImporter.h"
#include "StaticMeshAttributes.h"
#include "Dom/JsonObject.h"

namespace
{
constexpr int32 Segments = 16; // around an axis; rings per hemisphere / sphere stack: Segments / 4
bool Vec(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FVector& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!O->TryGetArrayField(Key, A) || A->Num() != 3) return false;
    for (int32 I = 0; I < 3; ++I) Out[I] = (*A)[I]->AsNumber();
    return !Out.ContainsNaN();
}
bool Num(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, double& Out)
{
    return O->TryGetNumberField(Key, Out) && FMath::IsFinite(Out);
}
/** Convex shape: wind every triangle so its normal points away from the shape centre. */
void Outward(TArray<FVector>& T, int32 From, const FVector& Centre)
{
    for (int32 I = From; I + 2 < T.Num(); I += 3)
    {
        const FVector N = FVector::CrossProduct(T[I + 1] - T[I], T[I + 2] - T[I]);
        if (FVector::DotProduct(N, (T[I] + T[I + 1] + T[I + 2]) / 3 - Centre) < 0) Swap(T[I + 1], T[I + 2]);
    }
}
void Quad(TArray<FVector>& T, const FVector& A, const FVector& B, const FVector& C, const FVector& D)
{
    T.Append({A, B, C, A, C, D});
}
/** Rings of a surface of revolution about Axis (unit) with an orthonormal U, V: Profile(k) = (radius, height). */
void Revolve(TArray<FVector>& T, const FVector& C, const FVector& Axis, const TArray<FVector2D>& Profile)
{
    FVector U, V;
    Axis.FindBestAxisVectors(U, V);
    auto Point = [&](const FVector2D& P, int32 S) {
        const double A = 2 * PI * S / Segments;
        return C + Axis * P.Y + (U * FMath::Cos(A) + V * FMath::Sin(A)) * P.X;
    };
    for (int32 K = 0; K + 1 < Profile.Num(); ++K)
        for (int32 S = 0; S < Segments; ++S)
        {
            const FVector A = Point(Profile[K], S), B = Point(Profile[K], S + 1), Cc = Point(Profile[K + 1], S + 1), D = Point(Profile[K + 1], S);
            if (Profile[K].X <= 0) T.Append({A, Cc, D});            // pole
            else if (Profile[K + 1].X <= 0) T.Append({A, B, Cc});   // pole
            else Quad(T, A, B, Cc, D);
        }
}
}

bool FSCMeshImporter::PhysicsBlocksActor(const TSharedPtr<FJsonObject>& Shape)
{
    FString Class;
    if (Shape) Shape->TryGetStringField(TEXT("class"), Class);
    return Class != TEXT("interaction") && Class != TEXT("tires_only") && Class != TEXT("grid_volume");
}
bool FSCMeshImporter::PhysicsShapeTriangles(const TSharedPtr<FJsonObject>& Shape, TArray<FVector>& Out,
    TFunctionRef<bool(int32, TArray<FVector>&)> MeshTriangles, FString& Error)
{
    FString Kind;
    Shape->TryGetStringField(TEXT("kind"), Kind);
    const int32 From = Out.Num();
    FVector C, Axis, H;
    double R = 0, HalfHeight = 0;
    if (Kind == TEXT("trimesh"))
    {
        // scene.json `physics_grid` carries its vertices and triangles inline; a mesh asset's record names a glTF mesh.
        const TArray<TSharedPtr<FJsonValue>>* Vertices;
        const TArray<TSharedPtr<FJsonValue>>* Triangles;
        if (Shape->TryGetArrayField(TEXT("vertices"), Vertices))
        {
            if (!Shape->TryGetArrayField(TEXT("triangles"), Triangles)) { Error = TEXT("trimesh: inline vertices without triangles"); return false; }
            TArray<FVector> Points;
            for (const auto& V : *Vertices)
            {
                const TArray<TSharedPtr<FJsonValue>>* P;
                if (!V || !V->TryGetArray(P) || P->Num() != 3) { Error = TEXT("trimesh: malformed vertex"); return false; }
                Points.Add(FVector((*P)[0]->AsNumber(), (*P)[1]->AsNumber(), (*P)[2]->AsNumber()));
            }
            for (const auto& T : *Triangles)
            {
                const TArray<TSharedPtr<FJsonValue>>* I;
                if (!T || !T->TryGetArray(I) || I->Num() != 3) { Error = TEXT("trimesh: malformed triangle"); return false; }
                for (int32 K = 0; K < 3; ++K)
                {
                    const int32 Index = int32((*I)[K]->AsNumber());
                    if (!Points.IsValidIndex(Index)) { Error = TEXT("trimesh: triangle index out of range"); return false; }
                    Out.Add(Points[Index]);
                }
            }
            return true;
        }
        double Mesh = -1;
        if (!Num(Shape, TEXT("mesh"), Mesh) || !MeshTriangles(int32(Mesh), Out)) { Error = TEXT("trimesh: invalid mesh"); return false; }
        return true;
    }
    if (Kind == TEXT("box"))
    {
        const TArray<TSharedPtr<FJsonValue>>* Axes;
        if (!Vec(Shape, TEXT("center"), C) || !Vec(Shape, TEXT("half_size"), H) || !Shape->TryGetArrayField(TEXT("axes"), Axes) || Axes->Num() != 3)
        { Error = TEXT("box: needs center, half_size, axes"); return false; }
        FVector A[3];
        for (int32 I = 0; I < 3; ++I)
        {
            const TArray<TSharedPtr<FJsonValue>>* Row;
            if (!(*Axes)[I]->TryGetArray(Row) || Row->Num() != 3) { Error = TEXT("box: axes are 3 rows of 3"); return false; }
            for (int32 J = 0; J < 3; ++J) A[I][J] = (*Row)[J]->AsNumber(); // rows: the box axes in node space
        }
        auto P = [&](int32 X, int32 Y, int32 Z) { return C + A[0] * (X * H.X) + A[1] * (Y * H.Y) + A[2] * (Z * H.Z); };
        Quad(Out, P(-1, -1, -1), P(-1, 1, -1), P(1, 1, -1), P(1, -1, -1)); Quad(Out, P(-1, -1, 1), P(1, -1, 1), P(1, 1, 1), P(-1, 1, 1));
        Quad(Out, P(-1, -1, -1), P(1, -1, -1), P(1, -1, 1), P(-1, -1, 1)); Quad(Out, P(-1, 1, -1), P(-1, 1, 1), P(1, 1, 1), P(1, 1, -1));
        Quad(Out, P(-1, -1, -1), P(-1, -1, 1), P(-1, 1, 1), P(-1, 1, -1)); Quad(Out, P(1, -1, -1), P(1, 1, -1), P(1, 1, 1), P(1, -1, 1));
    }
    else if (Kind == TEXT("sphere"))
    {
        if (!Vec(Shape, TEXT("center"), C) || !Num(Shape, TEXT("radius"), R) || R <= 0) { Error = TEXT("sphere: needs center, radius > 0"); return false; }
        TArray<FVector2D> Profile;
        for (int32 K = 0; K <= Segments / 2; ++K) { const double A = PI * K / (Segments / 2); Profile.Add({R * FMath::Sin(A), -R * FMath::Cos(A)}); }
        Revolve(Out, C, FVector::UnitZ(), Profile);
    }
    else if (Kind == TEXT("cylinder") || Kind == TEXT("capsule"))
    {
        if (!Vec(Shape, TEXT("center"), C) || !Vec(Shape, TEXT("axis"), Axis) || !Num(Shape, TEXT("radius"), R) ||
            !Num(Shape, TEXT("half_height"), HalfHeight) || R <= 0 || HalfHeight < 0 || !Axis.Normalize())
        { Error = Kind + TEXT(": needs center, axis, radius > 0, half_height >= 0"); return false; }
        TArray<FVector2D> Profile;
        if (Kind == TEXT("cylinder")) Profile = {{0, -HalfHeight}, {R, -HalfHeight}, {R, HalfHeight}, {0, HalfHeight}};
        else
        {
            const int32 Rings = Segments / 4; // hemispheres about the segment ends
            for (int32 K = 0; K <= Rings; ++K) { const double A = HALF_PI * K / Rings; Profile.Add({R * FMath::Sin(A), -HalfHeight - R * FMath::Cos(A)}); }
            for (int32 K = 0; K <= Rings; ++K) { const double A = HALF_PI * K / Rings; Profile.Add({R * FMath::Cos(A), HalfHeight + R * FMath::Sin(A)}); }
        }
        Revolve(Out, C, Axis, Profile);
    }
    else { Error = TEXT("unknown collider kind: ") + Kind; return false; }
    Outward(Out, From, C);
    return true;
}

FSCDerivedMesh FSCMeshImporter::SelectPhysics(const FSCDerivedMesh& Data, int32 Owner)
{
    FSCDerivedMesh Result;
    Result.MaterialNames = {TEXT("GamePhysics")};
    Result.MaterialSourceNames = {FString()};
    Result.MaterialFamilies = {FString()};
    Result.ContentHash = Data.ContentHash;
    Result.bHasTangents = false;
    Result.bEnableNanite = false;
    Result.SocketNode = Owner;
    FStaticMeshAttributes A(Result.Description);
    A.Register();
    A.GetVertexInstanceUVs().SetNumChannels(1);
    const FPolygonGroupID Group = Result.Description.CreatePolygonGroup();
    A.GetPolygonGroupMaterialSlotNames()[Group] = FName(TEXT("Slot_0"));
    TMap<int32, FVertexID> Vertices;
    for (int32 T = 0; T < Data.PhysicsTriangles.Num(); ++T)
    {
        if (Owner != INDEX_NONE && Data.PhysicsTriangleNodes[T] != Owner) continue;
        const FIntVector& Tri = Data.PhysicsTriangles[T];
        const FVector3f P[3] = {Data.PhysicsPositions[Tri.X], Data.PhysicsPositions[Tri.Y], Data.PhysicsPositions[Tri.Z]};
        const FVector3f N = FVector3f::CrossProduct(P[2] - P[0], P[1] - P[0]).GetSafeNormal();
        TArray<FVertexInstanceID, TInlineAllocator<3>> Corners;
        for (int32 K = 0; K < 3; ++K)
        {
            const int32 Source = Tri[K];
            FVertexID* Existing = Vertices.Find(Source);
            const FVertexID Vertex = Existing ? *Existing : Vertices.Add(Source, Result.Description.CreateVertex());
            A.GetVertexPositions()[Vertex] = P[K];
            const FVertexInstanceID VI = Result.Description.CreateVertexInstance(Vertex);
            A.GetVertexInstanceNormals()[VI] = N;
            A.GetVertexInstanceTangents()[VI] = FVector3f::ZeroVector;
            A.GetVertexInstanceBinormalSigns()[VI] = 1;
            A.GetVertexInstanceColors()[VI] = FVector4f(1, 1, 1, 1);
            A.GetVertexInstanceUVs().Set(VI, 0, FVector2f::ZeroVector);
            Corners.Add(VI);
        }
        Result.Description.CreateTriangle(Group, Corners);
    }
    Result.bHasGeometry = Result.Description.Triangles().Num() > 0;
    return Result;
}
