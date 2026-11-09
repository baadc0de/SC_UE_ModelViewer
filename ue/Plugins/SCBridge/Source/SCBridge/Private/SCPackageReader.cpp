#include "SCPackageReader.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/SecureHash.h"
#include "Misc/Paths.h"

namespace
{
using FReaderObject = TSharedPtr<FJsonObject>;
FString String(const FReaderObject& O, const TCHAR* Key) { FString S; if (O) O->TryGetStringField(Key, S); return S; }
FString Json(const FReaderObject& O) { FString S; FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&S)); return S; }
const TArray<TSharedPtr<FJsonValue>>& ReaderArray(const FReaderObject& O, const TCHAR* Key)
{
    static const TArray<TSharedPtr<FJsonValue>> Empty;
    const TArray<TSharedPtr<FJsonValue>>* A;
    return O && O->TryGetArrayField(Key, A) ? *A : Empty;
}
bool ReadJson(const FSCPackageSource& Source, const FString& Path, FReaderObject& Out, FString& Error, FString* Hash = nullptr)
{
    TArray<uint8> Bytes;
    if (!Source.ReadBytes(Path, Bytes, Error)) return false;
    if (Hash) *Hash = FSCPackageReader::Hash(Bytes);
    const FString Text(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()));
    if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) && Out) return true;
    Error = TEXT("Invalid JSON: ") + Path; return false;
}
bool Matrix(const FReaderObject& O, const TCHAR* Key, FMatrix& Out, FString& Error)
{
    Out = FMatrix::Identity;
    if (!O->HasField(Key)) return true;
    const auto& Columns = ReaderArray(O, Key);
    if (Columns.Num() != 4) { Error = TEXT("Expected 4x4 matrix: ") + FString(Key); return false; }
    for (int32 C = 0; C < 4; ++C)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values;
        if (!Columns[C]->TryGetArray(Values) || Values->Num() != 4) { Error = TEXT("Invalid matrix column"); return false; }
        for (int32 R = 0; R < 4; ++R)
        {
            double V;
            if (!(*Values)[R]->TryGetNumber(V) || !FMath::IsFinite(V)) { Error = TEXT("Non-finite matrix"); return false; }
            Out.M[C][R] = V; // UE row vectors: transpose of glTF column-vector matrix.
        }
    }
    if (!FMath::IsNearlyZero(Out.M[0][3]) || !FMath::IsNearlyZero(Out.M[1][3]) || !FMath::IsNearlyZero(Out.M[2][3]) || !FMath::IsNearlyEqual(Out.M[3][3], 1.0))
    { Error = TEXT("Non-affine matrix"); return false; }
    return true;
}
bool Instance(const FReaderObject& O, FSCSceneInstance& Out, FString& Error, const TCHAR* MatrixKey = TEXT("local_transform_sc"))
{
    if (!O) { Error = TEXT("Expected scene instance object"); return false; }
    Out.EntityName = String(O, TEXT("entity_name"));
    Out.ParentEntityName = String(O, TEXT("parent_entity_name"));
    Out.ParentNodeName = String(O, TEXT("parent_node_name"));
    Out.SourceTransformBasis = String(O, TEXT("source_transform_basis"));
    Out.PaletteId = String(O, TEXT("palette_id"));
    Out.bInvisible = String(O, TEXT("port_flags")).Contains(TEXT("invisible"), ESearchCase::IgnoreCase);
    for (auto Pair : {TPair<const TCHAR*, FString*>(TEXT("mesh_asset"), &Out.MeshAsset), {TEXT("material_sidecar"), &Out.MaterialSidecar}})
    {
        const FString Path = String(O, Pair.Key);
        if (!Path.IsEmpty() && !FSCPackageSource::Normalize(Path, *Pair.Value)) { Error = TEXT("Unsafe asset path: ") + Path; return false; }
    }
    if (!Out.SourceTransformBasis.IsEmpty() && Out.SourceTransformBasis != TEXT("gltf_y_up") && Out.SourceTransformBasis != TEXT("cryengine_z_up"))
    { Error = TEXT("Unsupported transform basis: ") + Out.SourceTransformBasis; return false; }
    return Matrix(O, MatrixKey, Out.LocalMatrix, Error);
}
}
FString FSCPackageReader::Hash(const TArray<uint8>& Bytes)
{
    FSHA1 H; H.Update(Bytes.GetData(), Bytes.Num()); H.Final(); uint8 Digest[20]; H.GetHash(Digest);
    return BytesToHex(Digest, 20);
}
TArray<FString> FSCSceneManifest::MeshAssets() const
{
    TSet<FString> Paths;
    auto Add = [&](const FSCSceneInstance& I) { if (!I.MeshAsset.IsEmpty()) Paths.Add(I.MeshAsset); };
    Add(Root); for (const auto& I : Children) Add(I);
    for (const auto& I : Interiors) for (const auto& P : I.Placements) Add(P);
    TArray<FString> Result = Paths.Array(); Result.Sort(); return Result;
}
bool FSCPackageReader::Read(const FSCPackageSource& Source, FSCSceneManifest& Out, FString& Error, const FString& ScenePath)
{
    Out = FSCSceneManifest(); Error.Reset();
    Out.ScenePath = ScenePath;
    if (Out.ScenePath.IsEmpty())
    {
        TArray<FString> Scenes;
        for (const FString& File : Source.ListFiles()) if (FPaths::GetCleanFilename(File) == TEXT("scene.json")) Scenes.Add(File);
        if (Scenes.Num() != 1) { Error = FString::Printf(TEXT("Expected one scene.json, found %d; select ScenePath"), Scenes.Num()); return false; }
        Out.ScenePath = Scenes[0];
    }
    FReaderObject Scene;
    if (!ReadJson(Source, Out.ScenePath, Scene, Error, &Out.ContentHash)) return false;
    double Version = 0;
    if (!Scene->TryGetNumberField(TEXT("version"), Version) || Version != 1 || String(Scene, TEXT("export_kind")) != TEXT("Decomposed"))
    { Error = TEXT("Expected decomposed scene version 1"); return false; }
    const FReaderObject* Root;
    if (!Scene->TryGetObjectField(TEXT("root_entity"), Root) || !Instance(*Root, Out.Root, Error)) return false;
    for (const auto& V : ReaderArray(Scene, TEXT("children")))
    {
        FSCSceneInstance I;
        if (!Instance(V->AsObject(), I, Error)) return false;
        if (I.SourceTransformBasis.IsEmpty()) { Error = TEXT("Child lacks resolved source_transform_basis"); return false; }
        Out.Children.Add(MoveTemp(I));
    }
    for (const auto& V : ReaderArray(Scene, TEXT("interiors")))
    {
        const auto O = V->AsObject(); FSCInterior I;
        if (!Instance(O, I.Container, Error, TEXT("container_transform"))) return false;
        I.Container.EntityName = String(O, TEXT("name"));
        I.Container.SourceTransformBasis = TEXT("cryengine_z_up");
        for (const auto& P : ReaderArray(O, TEXT("placements")))
        {
            FSCSceneInstance Placement;
            if (!Instance(P->AsObject(), Placement, Error, TEXT("transform"))) return false;
            Placement.EntityName = String(P->AsObject(), TEXT("cgf_path"));
            Placement.SourceTransformBasis = TEXT("cryengine_z_up");
            I.Placements.Add(MoveTemp(Placement));
        }
        Out.Interiors.Add(MoveTemp(I));
    }
    TSet<FString> Sidecars;
    auto Collect = [&](const FSCSceneInstance& I) { if (!I.MaterialSidecar.IsEmpty()) Sidecars.Add(I.MaterialSidecar); };
    Collect(Out.Root); for (const auto& I : Out.Children) Collect(I);
    for (const auto& I : Out.Interiors) for (const auto& P : I.Placements) Collect(P);
    TArray<FString> Sorted = Sidecars.Array(); Sorted.Sort();
    for (const FString& Path : Sorted)
    {
        FSCMaterialSet Set; Set.Path = Path; FReaderObject O;
        if (!ReadJson(Source, Path, O, Error, &Set.ContentHash)) return false;
        for (const auto& V : ReaderArray(O, TEXT("submaterials")))
        {
            const auto Sub = V->AsObject();
            if (!Sub) { Error = TEXT("Invalid submaterial"); return false; }
            FSCSubmaterial M; M.Index = Set.Submaterials.Num();
            double Index;
            if (Sub->TryGetNumberField(TEXT("index"), Index)) M.SourceIndex = int32(Index);
            M.Name = String(Sub, TEXT("submaterial_name")); M.ShaderFamily = String(Sub, TEXT("shader_family")); M.Json = Json(Sub);
            Set.Submaterials.Add(MoveTemp(M));
        }
        Out.Materials.Add(MoveTemp(Set));
    }
    const FString Directory = FPaths::GetPath(Out.ScenePath);
    const FString PalettesPath = Directory / TEXT("palettes.json");
    if (Source.Exists(PalettesPath))
    {
        FReaderObject O; if (!ReadJson(Source, PalettesPath, O, Error)) return false;
        for (const auto& V : ReaderArray(O, TEXT("palettes")))
        {
            if (!V->AsObject()) { Error = TEXT("Invalid palette"); return false; }
            FSCPalette P; P.Id = String(V->AsObject(), TEXT("id")); P.Json = Json(V->AsObject()); Out.Palettes.Add(MoveTemp(P));
        }
    }
    for (const auto& V : ReaderArray(*Root, TEXT("animations")))
    {
        if (!V->AsObject()) { Error = TEXT("Invalid clip record"); return false; }
        FSCClipRecord Clip; Clip.Name = String(V->AsObject(), TEXT("name")); Clip.Json = Json(V->AsObject());
        const FString Path = String(V->AsObject(), TEXT("sidecar"));
        if (!Path.IsEmpty() && !FSCPackageSource::Normalize(Directory / Path, Clip.Sidecar)) { Error = TEXT("Unsafe clip sidecar"); return false; }
        Out.Clips.Add(MoveTemp(Clip));
    }
    for (const FString& Path : Out.MeshAssets())
        if (!Path.EndsWith(TEXT(".glb")) || !Source.Exists(Path)) { Error = TEXT("Missing/unsupported mesh: ") + Path; return false; }
    return true;
}
