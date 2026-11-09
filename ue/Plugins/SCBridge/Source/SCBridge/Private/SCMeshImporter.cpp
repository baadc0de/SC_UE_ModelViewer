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

namespace
{
using FObject = TSharedPtr<FJsonObject>;
const TArray<TSharedPtr<FJsonValue>>& Array(const FObject& O, const TCHAR* Key)
{
    static const TArray<TSharedPtr<FJsonValue>> Empty;
    const TArray<TSharedPtr<FJsonValue>>* A;
    return O && O->TryGetArrayField(Key, A) ? *A : Empty;
}
int32 Int(const FObject& O, const TCHAR* Key, int32 Default = 0)
{
    int32 V; return O && O->TryGetNumberField(Key, V) ? V : Default;
}
FObject ObjectAt(const FObject& O, const TCHAR* Key, int32 Index)
{
    const auto& A = Array(O, Key); return A.IsValidIndex(Index) ? A[Index]->AsObject() : nullptr;
}
uint32 U32(const uint8* P) { uint32 V; FMemory::Memcpy(&V, P, 4); return V; }
struct FAccessor
{
    const uint8* Data = nullptr;
    int32 Count = 0, Components = 0, Type = 0, ComponentSize = 0, Stride = 0;
    bool Normalized = false;
    double Get(int32 Index, int32 Component) const
    {
        const uint8* P = Data + int64(Index) * Stride + Component * ComponentSize;
        switch (Type)
        {
        case 5126: { float V; FMemory::Memcpy(&V, P, 4); return V; }
        case 5125: return U32(P);
        case 5123: { uint16 V; FMemory::Memcpy(&V, P, 2); return Normalized ? V / 65535.0 : V; }
        case 5122: { int16 V; FMemory::Memcpy(&V, P, 2); return Normalized ? FMath::Max(-1.0, V / 32767.0) : V; }
        case 5121: return Normalized ? *P / 255.0 : *P;
        case 5120: { int8 V = *P; return Normalized ? FMath::Max(-1.0, V / 127.0) : V; }
        default: return 0;
        }
    }
    FVector Vector(int32 I) const { return FVector(Get(I, 0), Get(I, 1), Get(I, 2)); }
};
struct FGlb
{
    FObject Json;
    const uint8* Bin = nullptr;
    int64 BinSize = 0;
    FString Error;
    bool Accessor(int32 Index, FAccessor& A)
    {
        const auto O = ObjectAt(Json, TEXT("accessors"), Index);
        if (!O || O->HasField(TEXT("sparse"))) { Error = TEXT("Missing or unsupported sparse accessor"); return false; }
        const auto View = ObjectAt(Json, TEXT("bufferViews"), Int(O, TEXT("bufferView"), -1));
        if (!View || Int(View, TEXT("buffer")) != 0) { Error = TEXT("Missing/external bufferView"); return false; }
        FString Type; O->TryGetStringField(TEXT("type"), Type);
        A.Components = Type == TEXT("SCALAR") ? 1 : Type == TEXT("VEC2") ? 2 : Type == TEXT("VEC3") ? 3 : Type == TEXT("VEC4") ? 4 : Type == TEXT("MAT4") ? 16 : 0;
        A.Type = Int(O, TEXT("componentType")); A.Count = Int(O, TEXT("count"));
        A.ComponentSize = A.Type == 5120 || A.Type == 5121 ? 1 : A.Type == 5122 || A.Type == 5123 ? 2 : A.Type == 5125 || A.Type == 5126 ? 4 : 0;
        A.Stride = Int(View, TEXT("byteStride"), A.Components * A.ComponentSize);
        O->TryGetBoolField(TEXT("normalized"), A.Normalized);
        const int64 ViewOffset = Int(View, TEXT("byteOffset")), ViewSize = Int(View, TEXT("byteLength")), Offset = Int(O, TEXT("byteOffset"));
        const int64 Needed = int64(A.Count - 1) * A.Stride + A.Components * A.ComponentSize;
        if (!A.Components || !A.ComponentSize || A.Count <= 0 || A.Stride < A.Components * A.ComponentSize || Offset < 0 || ViewOffset < 0 || ViewSize < 0
            || ViewOffset + ViewSize > BinSize || Needed < 0 || Offset + Needed > ViewSize)
        { Error = TEXT("Accessor exceeds buffer bounds or has invalid layout"); return false; }
        A.Data = Bin + ViewOffset + Offset;
        return true;
    }
    bool Parse(const TArray<uint8>& Bytes)
    {
        if (Bytes.Num() < 20 || U32(Bytes.GetData()) != 0x46546c67 || U32(Bytes.GetData() + 4) != 2 || U32(Bytes.GetData() + 8) != uint32(Bytes.Num()))
        { Error = TEXT("Invalid GLB 2 header"); return false; }
        for (int64 Offset = 12; Offset + 8 <= Bytes.Num();)
        {
            const uint32 Size = U32(Bytes.GetData() + Offset), Type = U32(Bytes.GetData() + Offset + 4);
            Offset += 8;
            if (Offset + Size > Bytes.Num()) { Error = TEXT("Truncated GLB chunk"); return false; }
            if (Type == 0x4e4f534a)
            {
                const FString Text(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset), Size));
                if (Json || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json)) { Error = TEXT("Invalid GLB JSON"); return false; }
            }
            else if (Type == 0x004e4942) { Bin = Bytes.GetData() + Offset; BinSize = Size; }
            Offset += Size;
        }
        if (!Json || !Bin) { Error = TEXT("GLB needs JSON and BIN chunks"); return false; }
        if (!Array(Json, TEXT("extensionsRequired")).IsEmpty()) { Error = TEXT("Required glTF extensions are unsupported"); return false; }
        return true;
    }
    bool NodeMatrix(const FObject& N, FMatrix& M)
    {
        M = FMatrix::Identity;
        const auto& Values = Array(N, TEXT("matrix"));
        if (!Values.IsEmpty())
        {
            if (Values.Num() != 16) return false;
            for (int32 I = 0; I < 16; ++I) M.M[I / 4][I % 4] = Values[I]->AsNumber();
        }
        else
        {
            FVector T(0), S(1); FQuat Q = FQuat::Identity;
            const auto& Translation = Array(N, TEXT("translation")); const auto& Scale = Array(N, TEXT("scale")); const auto& Rotation = Array(N, TEXT("rotation"));
            if (Translation.Num() == 3) T = FVector(Translation[0]->AsNumber(), Translation[1]->AsNumber(), Translation[2]->AsNumber());
            if (Scale.Num() == 3) S = FVector(Scale[0]->AsNumber(), Scale[1]->AsNumber(), Scale[2]->AsNumber());
            if (Rotation.Num() == 4) Q = FQuat(Rotation[0]->AsNumber(), Rotation[1]->AsNumber(), Rotation[2]->AsNumber(), Rotation[3]->AsNumber()).GetNormalized();
            M = FTransform(Q, T, S).ToMatrixWithScale();
        }
        return !M.ContainsNaN();
    }
};
}
FMatrix FSCMeshImporter::CryToGltf()
{
    return FMatrix(FPlane(1,0,0,0), FPlane(0,0,-1,0), FPlane(0,1,0,0), FPlane(0,0,0,1));
}
FMatrix FSCMeshImporter::Transform(const FMatrix& Gltf)
{
    const FMatrix Basis(FPlane(0,100,0,0), FPlane(0,0,100,0), FPlane(-100,0,0,0), FPlane(0,0,0,1));
    return Basis.Inverse() * Gltf * Basis;
}
FMatrix FSCMeshImporter::InstanceTransform(const FMatrix& Local, const FMatrix& AssetRoot, bool bSourceFrame, bool bCryBasis)
{
    if (bSourceFrame) return Transform((AssetRoot.Determinant() != 0 ? AssetRoot.Inverse() : FMatrix::Identity) * Local);
    return Transform(bCryBasis ? CryToGltf().Inverse() * Local * CryToGltf() : Local);
}
bool FSCMeshImporter::Derive(const FSCPackageSource& Source, const FString& Path, FSCDerivedMesh& Out, FString& Error)
{
    Out = FSCDerivedMesh(); TArray<uint8> Bytes;
    if (!Source.ReadBytes(Path, Bytes, Error)) return false;
    Out.ContentHash = FSCPackageReader::Hash(Bytes);
    FGlb G;
    if (!G.Parse(Bytes)) { Error = Path + TEXT(": ") + G.Error; return false; }
    Out.bHasSkins = !Array(G.Json, TEXT("skins")).IsEmpty();
    for (const auto& M : Array(G.Json, TEXT("materials")))
    {
        FString Name, SourceName, Family, Alpha; const auto O = M->AsObject();
        if (O)
        {
            O->TryGetStringField(TEXT("name"), Name); O->TryGetStringField(TEXT("alphaMode"), Alpha);
            const FObject* Extras;
            if (O->TryGetObjectField(TEXT("extras"), Extras))
            {
                (*Extras)->TryGetStringField(TEXT("source_name"), SourceName);
                (*Extras)->TryGetStringField(TEXT("shader"), Family);
            }
        }
        if (Alpha.Equals(TEXT("BLEND"), ESearchCase::IgnoreCase)) Out.BlendedSlots.Add(Out.MaterialNames.Num());
        Out.MaterialNames.Add(Name); Out.MaterialSourceNames.Add(SourceName); Out.MaterialFamilies.Add(Family);
    }
    int32 DefaultSlot = INDEX_NONE;
    FStaticMeshAttributes A(Out.Description); A.Register();
    auto Positions = A.GetVertexPositions(); auto Normals = A.GetVertexInstanceNormals(); auto Tangents = A.GetVertexInstanceTangents();
    auto Signs = A.GetVertexInstanceBinormalSigns(); auto UVs = A.GetVertexInstanceUVs(); auto Colors = A.GetVertexInstanceColors();
    UVs.SetNumChannels(2);
    TArray<FPolygonGroupID> Groups;
    for (int32 I = 0; I < Out.MaterialNames.Num(); ++I)
    {
        auto Group = Out.Description.CreatePolygonGroup(); Groups.Add(Group);
        A.GetPolygonGroupMaterialSlotNames()[Group] = FName(*FString::Printf(TEXT("Slot_%d"), I));
    }
    struct FBindCheck { int32 Joint; FMatrix InverseBind; FMatrix MeshWorld; };
    TArray<FBindCheck> BindChecks;
    TSet<int32> CheckedSkins;
    TSet<int32> Active, Visited;
    TFunction<bool(int32, const FMatrix&, int32)> Visit;
    Visit = [&](int32 Index, const FMatrix& Parent, int32 ParentRig)
    {
        const auto Node = ObjectAt(G.Json, TEXT("nodes"), Index);
        if (!Node || Active.Contains(Index) || Visited.Contains(Index)) { Error = TEXT("Invalid/cyclic/multiply-parented GLB node"); return false; }
        Active.Add(Index); Visited.Add(Index);
        FMatrix Local;
        if (!G.NodeMatrix(Node, Local)) { Error = TEXT("Invalid node matrix"); return false; }
        const FMatrix World = Local * Parent;
        FString NodeName;
        if (Node->TryGetStringField(TEXT("name"), NodeName) && !NodeName.IsEmpty()) Out.Nodes.FindOrAdd(NodeName) = World;
        FSCRigNode RigNode;
        RigNode.SourceName = NodeName.IsEmpty() ? FString::Printf(TEXT("Node_%d"), Index) : NodeName;
        RigNode.Parent = ParentRig; RigNode.GltfIndex = Index;
        RigNode.Local = FTransform(Transform(Local)); RigNode.World = World;
        // Exported skin helper aliases are identity children of an identically named owner.
        // Only that owner receives channels; driving both would apply the rotation twice.
        for (int32 Ancestor = ParentRig; Ancestor != INDEX_NONE; Ancestor = Out.RigNodes[Ancestor].Parent)
            if (Out.RigNodes[Ancestor].SourceName.Equals(RigNode.SourceName, ESearchCase::IgnoreCase)) RigNode.bAnimate = false;
        const int32 RigIndex = Out.RigNodes.Add(RigNode); Out.GltfToRig.Add(Index, RigIndex);
        const int32 MeshIndex = Int(Node, TEXT("mesh"), -1);
        const FVector Scale = World.GetScaleVector();
        const double ScaleProduct = Scale.X * Scale.Y * Scale.Z;
        const bool bDegenerate = ScaleProduct == 0 || FMath::Abs(World.Determinant() / ScaleProduct) <= 1.e-8;
        if (MeshIndex >= 0 && bDegenerate) Out.Warnings.Add(TEXT("Degenerate node: geometry skipped, transform retained: ") + NodeName);
        if (MeshIndex >= 0 && !bDegenerate)
        {
            const auto Mesh = ObjectAt(G.Json, TEXT("meshes"), MeshIndex);
            if (!Mesh) { Error = TEXT("Invalid mesh index"); return false; }
            const FMatrix NormalMatrix = World.Inverse().GetTransposed();
            for (const auto& Value : Array(Mesh, TEXT("primitives")))
            {
                const auto Primitive = Value->AsObject(); const FObject* Attributes;
                if (!Primitive || Int(Primitive, TEXT("mode"), 4) != 4 || !Primitive->TryGetObjectField(TEXT("attributes"), Attributes))
                { Error = TEXT("Only triangle primitives are supported"); return false; }
                FAccessor P, N, T, C, U0, U1, Indices, Joints, Weights;
                if (!G.Accessor(Int(*Attributes, TEXT("POSITION"), -1), P) || P.Components != 3) { Error = G.Error; return false; }
                auto Optional = [&](const TCHAR* Key, FAccessor& Access, int32 Min, int32 Max)
                {
                    if (!(*Attributes)->HasField(Key)) return true;
                    if (!G.Accessor(Int(*Attributes, Key), Access) || Access.Count != P.Count || Access.Components < Min || Access.Components > Max)
                    { Error = TEXT("Invalid vertex attribute: ") + FString(Key) + TEXT(" ") + G.Error; return false; }
                    return true;
                };
                if (!Optional(TEXT("NORMAL"), N, 3, 3) || !Optional(TEXT("TANGENT"), T, 4, 4) || !Optional(TEXT("COLOR_0"), C, 3, 4)
                    || !Optional(TEXT("TEXCOORD_0"), U0, 2, 2) || !Optional(TEXT("TEXCOORD_1"), U1, 2, 2)) return false;
                const int32 SkinIndex = Int(Node, TEXT("skin"), -1);
                const FObject Skin = ObjectAt(G.Json, TEXT("skins"), SkinIndex);
                const auto& JointNodes = Array(Skin, TEXT("joints"));
                if (SkinIndex >= 0 && (!Skin || !Optional(TEXT("JOINTS_0"), Joints, 4, 4) || !Optional(TEXT("WEIGHTS_0"), Weights, 4, 4)
                    || !Joints.Data || !Weights.Data || Joints.Normalized || (Joints.Type != 5121 && Joints.Type != 5123)))
                { Error = TEXT("Invalid skin joints/weights"); return false; }
                // Validate inverse-bind accessors. The exporter rebases each skin into its mesh node frame.
                if (Skin && Skin->HasField(TEXT("inverseBindMatrices")))
                {
                    FAccessor IBM;
                    if (!G.Accessor(Int(Skin, TEXT("inverseBindMatrices")), IBM) || IBM.Components != 16 || IBM.Count != JointNodes.Num())
                    { Error = TEXT("Invalid inverse bind matrices"); return false; }
                    if (!CheckedSkins.Contains(SkinIndex))
                    {
                        CheckedSkins.Add(SkinIndex);
                        for (int32 Joint = 0; Joint < IBM.Count; ++Joint)
                        {
                            FBindCheck Check; Check.Joint = int32(JointNodes[Joint]->AsNumber()); Check.MeshWorld = World;
                            for (int32 K = 0; K < 16; ++K) Check.InverseBind.M[K/4][K%4] = IBM.Get(Joint,K);
                            if (Check.InverseBind.ContainsNaN()) { Error = TEXT("Non-finite inverse bind"); return false; }
                            BindChecks.Add(Check);
                        }
                    }
                }
                Out.bHasTangents &= T.Data != nullptr;
                const bool bIndexed = Primitive->HasField(TEXT("indices"));
                if (bIndexed && (!G.Accessor(Int(Primitive, TEXT("indices")), Indices) || Indices.Components != 1 || Indices.Normalized
                    || (Indices.Type != 5121 && Indices.Type != 5123 && Indices.Type != 5125))) { Error = TEXT("Invalid triangle indices"); return false; }
                const int32 Count = bIndexed ? Indices.Count : P.Count;
                int32 Material = Int(Primitive, TEXT("material"), INDEX_NONE);
                if (Material == INDEX_NONE)
                {
                    if (DefaultSlot == INDEX_NONE)
                    {
                        DefaultSlot = Out.MaterialNames.Add(TEXT("Default"));
                        Out.MaterialSourceNames.Add(TEXT("")); Out.MaterialFamilies.Add(TEXT(""));
                        auto Group = Out.Description.CreatePolygonGroup(); Groups.Add(Group);
                        A.GetPolygonGroupMaterialSlotNames()[Group] = FName(*FString::Printf(TEXT("Slot_%d"), DefaultSlot));
                        Out.Warnings.Add(FString::Printf(TEXT("Primitive has no material; using default slot %d"), DefaultSlot));
                    }
                    Material = DefaultSlot;
                }
                if (Count % 3 || !Groups.IsValidIndex(Material)) { Error = TEXT("Invalid triangle/material count"); return false; }
                TMap<int32, FVertexID> Vertices;
                for (int32 I = 0; I < Count; I += 3)
                {
                    int32 Id[3]; FVector Pos[3];
                    for (int32 K = 0; K < 3; ++K)
                    {
                        const double Raw = bIndexed ? Indices.Get(I + K, 0) : I + K;
                        if (Raw < 0 || Raw >= P.Count) { Error = TEXT("Index outside POSITION accessor"); return false; }
                        Id[K] = int32(Raw); Pos[K] = Position(World.TransformPosition(P.Vector(Id[K])));
                        if (Pos[K].ContainsNaN()) { Error = TEXT("Non-finite vertex"); return false; }
                    }
                    // UE uses clockwise front faces. The host basis already reflects handedness;
                    // only an additional mirrored node reverses corners (Interchange baseline).
                    if (World.Determinant() < 0) { Swap(Id[1], Id[2]); Swap(Pos[1], Pos[2]); }
                    const FVector FaceNormal = FVector::CrossProduct(Pos[2] - Pos[0], Pos[1] - Pos[0]).GetSafeNormal();
                    if (FaceNormal.IsNearlyZero()) { ++Out.DegenerateTriangles; continue; }
                    TArray<FVertexInstanceID, TInlineAllocator<3>> Corners;
                    for (int32 K = 0; K < 3; ++K)
                    {
                        const int32 J = Id[K]; FVertexID* Existing = Vertices.Find(J); FVertexID Vertex;
                        if (Existing) Vertex = *Existing;
                        else
                        {
                            Vertex = Out.Description.CreateVertex(); Positions[Vertex] = FVector3f(Pos[K]); Vertices.Add(J, Vertex);
                            if (Skin)
                            {
                                FSCVertexWeights W; double Sum = 0;
                                for (int32 L = 0; L < 4; ++L)
                                {
                                    const double Joint = Joints.Get(J, L), Weight = Weights.Get(J, L);
                                    if (!FMath::IsFinite(Joint) || Joint != FMath::FloorToDouble(Joint) || !JointNodes.IsValidIndex(int32(Joint)) || !FMath::IsFinite(Weight) || Weight < 0)
                                    { Error = TEXT("Invalid skin influence"); return false; }
                                    W.Joints[L] = int32(JointNodes[int32(Joint)]->AsNumber()); W.Weights[L] = Weight; Sum += Weight;
                                }
                                if (Sum <= SMALL_NUMBER) { Error = TEXT("Zero skin weight sum"); return false; }
                                for (float& Weight : W.Weights) Weight /= Sum;
                                Out.SkinWeights.Add(Vertex, W);
                            }
                        }
                        const FVertexInstanceID VI = Out.Description.CreateVertexInstance(Vertex); Corners.Add(VI);
                        const FVector Normal = N.Data ? Position(NormalMatrix.TransformVector(N.Vector(J))).GetSafeNormal() : FaceNormal;
                        Normals[VI] = FVector3f(Normal);
                        Tangents[VI] = T.Data ? FVector3f(Position(World.TransformVector(T.Vector(J))).GetSafeNormal()) : FVector3f::ZeroVector;
                        Signs[VI] = T.Data ? float(FMath::Sign(World.Determinant()) * T.Get(J, 3)) : 1;
                        Colors[VI] = C.Data ? FVector4f(C.Get(J, 0), C.Get(J, 1), C.Get(J, 2), C.Components == 4 ? C.Get(J, 3) : 1) : FVector4f(1,1,1,1);
                        // Match Interchange: counter the static-mesh builder's linear-to-sRGB vertex-colour packing.
                        UE::Geometry::LinearColors::SRGBToLinear(Colors[VI]);
                        UVs.Set(VI, 0, U0.Data ? FVector2f(U0.Get(J, 0), U0.Get(J, 1)) : FVector2f::ZeroVector);
                        UVs.Set(VI, 1, U1.Data ? FVector2f(U1.Get(J, 0), U1.Get(J, 1)) : FVector2f::ZeroVector);
                    }
                    Out.TriangleNodes.Add(Out.Description.CreateTriangle(Groups[Material], Corners), RigIndex);
                }
            }
        }
        for (const auto& Child : Array(Node, TEXT("children"))) if (!Visit(int32(Child->AsNumber()), World, RigIndex)) return false;
        Active.Remove(Index); return true;
    };
    const auto Scene = ObjectAt(G.Json, TEXT("scenes"), Int(G.Json, TEXT("scene")));
    const auto& Roots = Array(Scene, TEXT("nodes"));
    if (Roots.IsEmpty()) { Error = TEXT("GLB has no scene roots"); return false; }
    if (Roots.Num() == 1)
    {
        const auto Root = ObjectAt(G.Json, TEXT("nodes"), int32(Roots[0]->AsNumber()));
        // Capture the external coordinate wrapper structurally, never by asset/node name.
        if (Root && !Root->HasField(TEXT("mesh")) && !G.NodeMatrix(Root, Out.RootMatrix)) { Error = TEXT("Invalid root matrix"); return false; }
    }
    for (const auto& Root : Roots) if (!Visit(int32(Root->AsNumber()), FMatrix::Identity, INDEX_NONE)) return false;
    for (const auto& Check : BindChecks)
    {
        const int32* Joint = Out.GltfToRig.Find(Check.Joint);
        if (!Joint || !(Check.InverseBind * Out.RigNodes[*Joint].World).Equals(Check.MeshWorld, 0.002))
        {
            Error = TEXT("Skin inverse bind disagrees with node reference pose; skeleton sidecar required (WP-R6)");
            return false;
        }
    }
    Out.AttachmentRootMatrix = Out.RootMatrix;
    if (Roots.Num() == 1)
    {
        const auto Root = ObjectAt(G.Json, TEXT("nodes"), int32(Roots[0]->AsNumber()));
        const auto& Children = Array(Root, TEXT("children"));
        if (!Root->HasField(TEXT("mesh")) && Children.Num() == 1)
        {
            FMatrix Content;
            if (G.NodeMatrix(ObjectAt(G.Json, TEXT("nodes"), int32(Children[0]->AsNumber())), Content))
                Out.AttachmentRootMatrix = Content * Out.RootMatrix;
        }
    }
    if (!Out.bHasTangents && Out.Description.Triangles().Num() > 0)
    {
        TMap<FVertexInstanceID, FVector4f> Authored;
        for (auto VI : Out.Description.VertexInstances().GetElementIDs())
            if (!Tangents[VI].IsNearlyZero()) Authored.Add(VI, FVector4f(Tangents[VI], Signs[VI]));
        FStaticMeshOperations::ComputeTriangleTangentsAndNormals(Out.Description);
        FStaticMeshOperations::ComputeMikktTangents(Out.Description, true);
        for (const auto& P : Authored) { Tangents[P.Key] = FVector3f(P.Value); Signs[P.Key] = P.Value.W; }
        Out.bHasTangents = true;
    }
    Out.bHasGeometry = Out.Description.Triangles().Num() > 0;
    // Hierarchy-only assets are valid attachments; retain their helper nodes.
    return true;
}
FSCDerivedMesh FSCMeshImporter::Select(const FSCDerivedMesh& Data, const TArray<FTriangleID>& Triangles)
{
        FSCDerivedMesh Result;
        Result.MaterialNames = Data.MaterialNames; Result.MaterialSourceNames = Data.MaterialSourceNames;
        Result.MaterialFamilies = Data.MaterialFamilies; Result.ContentHash = Data.ContentHash;
        Result.bHasTangents = Data.bHasTangents; Result.bEnableNanite = Data.bEnableNanite;
        Result.SocketNode = Data.SocketNode;
        FMeshDescription& Selected = Result.Description;
        FStaticMeshAttributes Dst(Selected); Dst.Register();
        FStaticMeshConstAttributes Src(Data.Description);
        const int32 Channels = Src.GetVertexInstanceUVs().GetNumChannels();
        Dst.GetVertexInstanceUVs().SetNumChannels(Channels);
        TArray<FPolygonGroupID> Groups;
        for (int32 Slot = 0; Slot < Data.MaterialNames.Num(); ++Slot)
        {
            const auto Group = Selected.CreatePolygonGroup(); Groups.Add(Group);
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
                if (Existing) NewVertex = *Existing;
                else
                {
                    NewVertex = Selected.CreateVertex(); Vertices.Add(Vertex, NewVertex);
                    Dst.GetVertexPositions()[NewVertex] = Src.GetVertexPositions()[Vertex];
                    if (const auto* W = Data.SkinWeights.Find(Vertex)) Result.SkinWeights.Add(NewVertex, *W);
                }
                const auto Corner = Selected.CreateVertexInstance(NewVertex); Corners.Add(Corner);
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
    Result.bHasGeometry = !Triangles.IsEmpty(); return Result;
}
void FSCMeshImporter::SplitTranslucent(FSCDerivedMesh& Data, const TSet<int32>& Slots, FSCDerivedMesh& Translucent)
{
    TArray<FTriangleID> Opaque, Blend;
    for (auto T : Data.Description.Triangles().GetElementIDs())
        (Slots.Contains(Data.Description.GetTrianglePolygonGroup(T).GetValue()) ? Blend : Opaque).Add(T);
    if (Blend.IsEmpty()) return;
    if (Opaque.IsEmpty()) { Data.bEnableNanite = false; return; }
    Translucent.MaterialNames = Data.MaterialNames;
    Translucent.ContentHash = Data.ContentHash; Translucent.bHasTangents = Data.bHasTangents;
    Translucent.bEnableNanite = false; Translucent.bHasGeometry = true;
    Translucent = Select(Data, Blend); Translucent.bEnableNanite = false;
    auto OpaqueData = Select(Data, Opaque);
    Data.Description = MoveTemp(OpaqueData.Description);
    Data.TriangleNodes = MoveTemp(OpaqueData.TriangleNodes);
    Data.SkinWeights = MoveTemp(OpaqueData.SkinWeights);
    // CollisionBytes retain the independently derived full-source triangle BVH.
}
bool FSCMeshImporter::DeriveCollision(FSCDerivedMesh& Data, FString& Error)
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
        const int32 Index = VertexMap.Num(); VertexMap.Add(V, Index);
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
    Chaos::FTriangleMeshImplicitObjectPtr Geometry = new Chaos::FTriangleMeshImplicitObject(
        MoveTemp(Particles), MoveTemp(Triangles), MoveTemp(Materials), MoveTemp(FaceMap));
    FMemoryWriter Writer(Data.CollisionBytes);
    Chaos::FChaosArchive Archive(Writer);
    Archive << Geometry;
    Data.CollisionVersions = Writer.GetCustomVersions();
    Data.CollisionDeriveSeconds = FPlatformTime::Seconds() - Start;
    if (Writer.IsError()) { Error = TEXT("Cannot serialize derived Chaos BVH"); return false; }
    return true;
}
UStaticMesh* FSCMeshImporter::Instantiate(const FSCDerivedMesh& Data, UObject* Outer, FName Name,
    bool bBuildCollision, UMaterialInterface* Material, FSCMeshBuildStats& Stats, FString& Error)
{
    check(IsInGameThread());
    UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, Name, RF_Transient);
    Stats.NaniteReason = TEXT("disabled:empty_geometry");
    if (Data.Description.Triangles().Num() == 0) return Mesh;
    Mesh->bSupportRayTracing = true;
    for (int32 I = 0; I < Data.MaterialNames.Num(); ++I)
        Mesh->GetStaticMaterials().Add(FStaticMaterial(Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface), FName(*FString::Printf(TEXT("Slot_%d"), I))));
    Mesh->CreateBodySetup(); auto* Body = Mesh->GetBodySetup();
    Body->CollisionTraceFlag = CTF_UseComplexAsSimple; Body->bDoubleSidedGeometry = true;
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
    UStaticMesh::FCommitMeshDescriptionParams Commit; Commit.bMarkPackageDirty = false; Commit.bUseHashAsGuid = true;
    Mesh->CommitMeshDescription(0, Commit);
    for (int32 I = 0; I < Data.MaterialNames.Num(); ++I)
    {
        FMeshSectionInfo Info(I); Info.bEnableCollision = bBuildCollision;
        Mesh->GetSectionInfoMap().Set(0, I, Info);
    }
    Mesh->Build(true);
    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
#else
    Stats.NaniteReason = TEXT("disabled:non_editor_host");
    UStaticMesh::FBuildMeshDescriptionsParams Params;
    Params.bFastBuild = true; Params.bAllowCpuAccess = false; Params.bBuildSimpleCollision = false;
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
        if (Data.CollisionBytes.IsEmpty()) { Error = TEXT("Collision requested without DeriveCollision data"); return nullptr; }
        FMemoryReader Reader(Data.CollisionBytes);
        Reader.SetCustomVersions(Data.CollisionVersions);
        Chaos::FChaosArchive Archive(Reader);
        Chaos::FTriangleMeshImplicitObjectPtr Geometry;
        Archive << Geometry;
        if (Reader.IsError() || !Geometry) { Error = TEXT("Cannot instantiate derived Chaos BVH"); return nullptr; }
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
    if (!Render || Render->LODResources.IsEmpty()) { Error = TEXT("Static mesh build produced no render data"); return nullptr; }
    const auto& LOD = Render->LODResources[0];
    Stats.Triangles = Data.Description.Triangles().Num(); Stats.FallbackTriangles = LOD.GetNumTriangles();
    Stats.Clusters = Render->HasValidNaniteData() ? Render->NaniteResourcesPtr->NumClusters : 0;
    Stats.bDistanceField = LOD.DistanceFieldData != nullptr; Stats.bRayTracingGeometry = LOD.RayTracingGeometry != nullptr
        || (Render->RayTracingProxy && !Render->RayTracingProxy->LODs.IsEmpty() && Render->RayTracingProxy->LODs[0].RayTracingGeometry);
#if WITH_EDITOR
    if (Stats.bNaniteEnabled && !Stats.Clusters) Stats.NaniteReason = TEXT("disabled:no_clusters");
    Stats.bMeshCards = LOD.CardRepresentationData != nullptr;
    Stats.Cards = LOD.CardRepresentationData ? LOD.CardRepresentationData->MeshCardsBuildData.CardBuildData.Num() : 0;
#endif
    return Mesh;
}
