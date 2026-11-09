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
bool FSCPackageReader::Read(const FSCPackageSource& Source, FSCSceneManifest& Out, FString& Error, const FString& ScenePath, TArray<FString>* Warnings)
{
    Out = FSCSceneManifest(); Error.Reset();
    auto Warn = [&](const FString& Path, const FString& Message) { if (Warnings) Warnings->Add(Path + TEXT(": ") + Message); };
    auto Object = [](const TSharedPtr<FJsonValue>& V) -> FReaderObject { return V && V->Type == EJson::Object ? V->AsObject() : nullptr; };
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
    if (!Scene->TryGetObjectField(TEXT("root_entity"), Root)) { Error = TEXT("Scene has no root_entity object"); return false; }
    FString Detail;
    if (!Instance(*Root, Out.Root, Detail)) { Out.Root.ImportWarning = Detail; Warn(TEXT("root_entity"), Detail); }
    for (const auto& V : ReaderArray(Scene, TEXT("children")))
    {
        FSCSceneInstance I;
        if (!Instance(Object(V), I, Detail)) I.ImportWarning = Detail;
        if (I.SourceTransformBasis.IsEmpty()) I.ImportWarning = TEXT("Child lacks resolved source_transform_basis");
        if (!I.ImportWarning.IsEmpty()) Warn(I.EntityName, I.ImportWarning);
        Out.Children.Add(MoveTemp(I));
    }
    for (const auto& V : ReaderArray(Scene, TEXT("interiors")))
    {
        const auto O = Object(V); FSCInterior I;
        if (!Instance(O, I.Container, Detail, TEXT("container_transform"))) { Warn(TEXT("interior"), Detail); continue; }
        I.Container.EntityName = String(O, TEXT("name"));
        I.Container.SourceTransformBasis = TEXT("cryengine_z_up");
        for (const auto& P : ReaderArray(O, TEXT("placements")))
        {
            FSCSceneInstance Placement;
            if (!Instance(Object(P), Placement, Detail, TEXT("transform"))) { Warn(TEXT("placement"), Detail); continue; }
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
        if (!ReadJson(Source, Path, O, Detail, &Set.ContentHash) || !O->HasTypedField<EJson::Array>(TEXT("submaterials")))
        { Warn(Path, Detail.IsEmpty() ? TEXT("Missing submaterials array") : Detail); Detail.Reset(); continue; }
        for (const auto& V : ReaderArray(O, TEXT("submaterials")))
        {
            const auto Sub = Object(V);
            FSCSubmaterial M; M.Index = Set.Submaterials.Num();
            if (!Sub) { Warn(Path, TEXT("Invalid submaterial slot; using placeholder")); Set.Submaterials.Add(MoveTemp(M)); continue; }
            double Index;
            if (Sub->TryGetNumberField(TEXT("index"), Index)) M.SourceIndex = int32(Index);
            M.Name = String(Sub, TEXT("submaterial_name")); M.ShaderFamily = String(Sub, TEXT("shader_family")); M.Json = Json(Sub);
            const FReaderObject* Alpha;
            if (Sub->TryGetObjectField(TEXT("alpha_config"), Alpha))
            {
                (*Alpha)->TryGetBoolField(TEXT("blended"), M.bBlended);
                FString Mode = String(*Alpha, TEXT("mode"));
                M.bBlended |= Mode.Equals(TEXT("blend"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("blended"), ESearchCase::IgnoreCase);
            }
            else { FString AlphaMode = String(Sub, TEXT("alpha_config")); M.bBlended = AlphaMode.Equals(TEXT("blended"), ESearchCase::IgnoreCase) || AlphaMode.Equals(TEXT("blend"), ESearchCase::IgnoreCase); }
            Set.Submaterials.Add(MoveTemp(M));
        }
        Out.Materials.Add(MoveTemp(Set));
    }
    auto ValidateSidecar = [&](FSCSceneInstance& I)
    {
        if (!I.MaterialSidecar.IsEmpty() && !Out.Materials.ContainsByPredicate([&](const FSCMaterialSet& M) { return M.Path == I.MaterialSidecar; }))
            I.ImportWarning = TEXT("Unavailable material sidecar: ") + I.MaterialSidecar;
    };
    ValidateSidecar(Out.Root); for (auto& I : Out.Children) ValidateSidecar(I);
    for (auto& I : Out.Interiors) for (auto& P : I.Placements) ValidateSidecar(P);
    const FString Directory = FPaths::GetPath(Out.ScenePath);
    const FString PalettesPath = Directory / TEXT("palettes.json");
    if (Source.Exists(PalettesPath))
    {
        FReaderObject O; if (!ReadJson(Source, PalettesPath, O, Detail)) Warn(PalettesPath, Detail);
        for (const auto& V : ReaderArray(O, TEXT("palettes")))
        {
            if (!Object(V)) { Warn(PalettesPath, TEXT("Invalid palette")); continue; }
            FSCPalette P; P.Id = String(V->AsObject(), TEXT("id")); P.Json = Json(V->AsObject()); Out.Palettes.Add(MoveTemp(P));
        }
    }
    for (const auto& V : ReaderArray(*Root, TEXT("animations")))
    {
        if (!Object(V)) { Warn(Out.ScenePath, TEXT("Invalid clip record")); continue; }
        FSCClipRecord Clip; Clip.Name = String(V->AsObject(), TEXT("name")); Clip.Json = Json(V->AsObject());
        const FString Path = String(V->AsObject(), TEXT("sidecar"));
        if (!Path.IsEmpty() && !FSCPackageSource::Normalize(Directory / Path, Clip.Sidecar)) { Warn(Out.ScenePath, TEXT("Unsafe clip sidecar")); continue; }
        Out.Clips.Add(MoveTemp(Clip));
    }
    // Mesh existence/format failures are recovered per asset by FSCImporter.
    return true;
}
