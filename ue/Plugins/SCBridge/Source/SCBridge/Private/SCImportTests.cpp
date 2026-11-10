#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCPackageSource.h"
#include "SCPackageReader.h"
#include "SCMeshImporter.h"
#include "StaticMeshAttributes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "SCImporter.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "SCVehicle.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/Material.h"
#include "StaticMeshResources.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace
{
class FMemorySource : public FSCPackageSource
{
public:
    TMap<FString, TArray<uint8>> Files;
    void Add(const FString& Name, const FString& Text)
    { FTCHARToUTF8 Utf8(*Text); TArray<uint8> B; B.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); Files.Add(Name, MoveTemp(B)); }
    bool Exists(const FString& Path) const override { return Files.Contains(Path); }
    bool ReadBytes(const FString& Path, TArray<uint8>& Out, FString& Error) const override
    { if (auto* B = Files.Find(Path)) { Out = *B; return true; } Error = TEXT("Missing fixture"); return false; }
    TArray<FString> ListFiles(const FString& Prefix) const override
    { TArray<FString> R; for (const auto& P : Files) if (Prefix.IsEmpty() || P.Key.StartsWith(Prefix)) R.Add(P.Key); R.Sort(); return R; }
};
void Append32(TArray<uint8>& B, uint32 V) { B.Append(reinterpret_cast<uint8*>(&V), 4); }
FMemorySource Triangle()
{
    // Entirely synthetic: asymmetric metre-sized triangle, authored tangent, two UVs and colours.
    FMemorySource S; TArray<uint8> Bin;
    const float Values[] = {
        0,0,0, 2,0,0, 0,3,0, // positions
        0,0,1, 0,0,1, 0,0,1, // normals
        1,0,0,1, 1,0,0,1, 1,0,0,1, // tangent
        0,0, 1,0, 0,1, // UV0
        0.2f,0.3f, 0.4f,0.5f, 0.6f,0.7f, // UV1
        1,0,0,1, 0.25f,0.5f,0.75f,1, 0,0,1,1 // colors
    };
    Bin.Append(reinterpret_cast<const uint8*>(Values), sizeof(Values));
    const FString Json = TEXT(R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":216}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":48},{"buffer":0,"byteOffset":120,"byteLength":24},{"buffer":0,"byteOffset":144,"byteLength":24},{"buffer":0,"byteOffset":168,"byteLength":48}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},{"bufferView":3,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":4,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":5,"componentType":5126,"count":3,"type":"VEC4"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TANGENT":2,"TEXCOORD_0":3,"TEXCOORD_1":4,"COLOR_0":5}}]}],"nodes":[{"mesh":0,"name":"Synthetic"}],"scenes":[{"nodes":[0]}],"scene":0})");
    FTCHARToUTF8 UTF8(*Json); TArray<uint8> J; J.Append(reinterpret_cast<const uint8*>(UTF8.Get()), UTF8.Length());
    while (J.Num() % 4) J.Add(' ');
    TArray<uint8> B; Append32(B, 0x46546c67); Append32(B, 2); Append32(B, 28 + J.Num() + Bin.Num());
    Append32(B, J.Num()); Append32(B, 0x4e4f534a); B.Append(J); Append32(B, Bin.Num()); Append32(B, 0x004e4942); B.Append(Bin);
    S.Files.Add(TEXT("triangle.glb"), MoveTemp(B)); return S;
}
TArray<uint8> EditGLB(const TArray<uint8>& Bytes, TFunctionRef<void(TSharedPtr<FJsonObject>)> Edit)
{
    uint32 JsonSize; FMemory::Memcpy(&JsonSize, Bytes.GetData() + 12, 4);
    const FString Text(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + 20), JsonSize));
    TSharedPtr<FJsonObject> Object; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object); Edit(Object);
    FString Edited; FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Edited));
    FTCHARToUTF8 Utf8(*Edited); TArray<uint8> J; J.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    while (J.Num() % 4) J.Add(' ');
    TArray<uint8> B; Append32(B, 0x46546c67); Append32(B, 2); Append32(B, Bytes.Num() - JsonSize + J.Num());
    Append32(B, J.Num()); Append32(B, 0x4e4f534a); B.Append(J);
    B.Append(Bytes.GetData() + 20 + JsonSize, Bytes.Num() - 20 - JsonSize); return B;
}
void TarEntry(TArray<uint8>& Out, const FString& Name, const FString& Prefix, char Type, const FString& Payload, uint64 DeclaredSize = MAX_uint64)
{
    TArray<uint8> Header; Header.SetNumZeroed(512);
    auto Put = [&](int32 Offset, int32 Length, const FString& Text) { FTCHARToUTF8 U(*Text); FMemory::Memcpy(Header.GetData() + Offset, U.Get(), FMath::Min(Length, U.Length())); };
    Put(0,100,Name); Put(345,155,Prefix); Put(257,6,TEXT("ustar")); Header[156] = Type;
    FTCHARToUTF8 U(*Payload); uint64 Size = DeclaredSize == MAX_uint64 ? U.Length() : DeclaredSize;
    auto Oct = [](uint64 Value, int32 Digits) { FString R; do { R = FString::Chr(TCHAR('0' + (Value & 7))) + R; Value >>= 3; } while (Value); return FString::ChrN(FMath::Max(0, Digits - R.Len()), '0') + R; };
    Put(124,12,Oct(Size, 11));
    FMemory::Memset(Header.GetData() + 148, ' ', 8);
    uint32 Sum = 0; for (uint8 C : Header) Sum += C;
    Put(148,8,Oct(Sum, 6)); Header[154] = 0; Header[155] = ' ';
    Out.Append(Header); Out.Append(reinterpret_cast<const uint8*>(U.Get()), U.Length());
    while (Out.Num() % 512) Out.Add(0);
}
FString PaxPath(const FString& Path)
{
    const FString Body = TEXT("path=") + Path + TEXT("\n");
    int32 Length = FTCHARToUTF8(*Body).Length() + 2;
    for (;;) { const int32 Next = FTCHARToUTF8(*Body).Length() + FString::FromInt(Length).Len() + 1; if (Next == Length) break; Length = Next; }
    return FString::FromInt(Length) + TEXT(" ") + Body;
}

}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCReaderSparse, "SCBridge.Package.SparseAuthoredMaterialIndices", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCReaderSparse::RunTest(const FString&)
{
    FMemorySource Source;
    Source.Add(TEXT("scene.json"), TEXT(R"({"version":1,"export_kind":"Decomposed","root_entity":{"entity_name":"Synthetic","material_sidecar":"Data/material.materials.json"}})"));
    Source.Add(TEXT("Data/material.materials.json"), TEXT(R"({"submaterials":[{"index":1,"submaterial_name":"A"},{"index":10,"submaterial_name":"B"}]})"));
    FSCSceneManifest Scene; FString Error;
    const bool Read = FSCPackageReader::Read(Source, Scene, Error);
    TestTrue(*Error, Read);
    if (Scene.Materials.Num()) TestEqual(TEXT("Two ordinal slots despite sparse authored indices"), Scene.Materials[0].Submaterials.Num(), 2);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCPaths, "SCBridge.Package.PathsAndTar", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCPaths::RunTest(const FString&)
{
    FString P;
    TestTrue(TEXT("normalize"), FSCPackageSource::Normalize(TEXT("./Data\\Meshes//a.glb"), P));
    TestEqual(TEXT("canonical"), P, FString(TEXT("Data/Meshes/a.glb")));
    for (const FString Bad : {TEXT("../escape"), TEXT("Data/../../escape"), TEXT("/absolute"), TEXT("C:\\escape")}) TestFalse(*Bad, FSCPackageSource::Normalize(Bad, P));
    TArray<uint8> Empty; Empty.SetNumZeroed(1024); FSCTarPackageSource Tar(MoveTemp(Empty));
    TestTrue(TEXT("empty tar"), Tar.IsValid());
    TArray<uint8> Broken; Broken.Init(1,512); FSCTarPackageSource Bad(MoveTemp(Broken));
    TestFalse(TEXT("bad checksum"), Bad.IsValid()); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCBasis, "SCBridge.Mesh.BasisAndAttributes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCBasis::RunTest(const FString&)
{
    auto Source = Triangle(); FSCDerivedMesh D; FString Error;
    if (!TestTrue(TEXT("derive triangle"), FSCMeshImporter::Derive(Source, TEXT("triangle.glb"), D, Error))) { AddError(Error); return false; }
    FStaticMeshConstAttributes A(D.Description);
    TestEqual(TEXT("triangles"), D.Description.Triangles().Num(), 1);
    TestTrue(TEXT("forward -Z becomes +X centimetres"), FSCMeshImporter::Position(FVector(0,0,-2)).Equals(FVector(200,0,0)));
    TestTrue(TEXT("up +Y becomes +Z"), FSCMeshImporter::Position(FVector(0,3,0)).Equals(FVector(0,0,300)));
    TestEqual(TEXT("UV channels"), A.GetVertexInstanceUVs().GetNumChannels(), 2);
    const FVertexInstanceID VI(0);
    TestTrue(TEXT("normal"), A.GetVertexInstanceNormals()[VI].Equals(FVector3f(-1,0,0)));
    TestTrue(TEXT("tangent"), A.GetVertexInstanceTangents()[VI].Equals(FVector3f(0,1,0)));
    TestEqual(TEXT("binormal sign"), A.GetVertexInstanceBinormalSigns()[VI], 1.0f);
    TestTrue(TEXT("UV1"), A.GetVertexInstanceUVs().Get(VI,1).Equals(FVector2f(0.2f,0.3f)));
    TestTrue(TEXT("colour"), A.GetVertexInstanceColors()[VI].Equals(FVector4f(1,0,0,1)));
    const FMatrix W = FSCMeshImporter::CryToGltf();
    const FMatrix HelperMatrix = FTranslationMatrix(FVector(0,5,1)) * W;
    const FMatrix Child = FSCMeshImporter::InstanceTransform(FTranslationMatrix(FVector(0,2,0)), W, true, false);
    const FMatrix ChildWorld = FSCMeshImporter::Transform(W) * Child * FSCMeshImporter::Transform(HelperMatrix);
    TestTrue(TEXT("Resolved child offsets are relative to source helper, one wrapper only"), FVector(ChildWorld.TransformPosition(FVector::ZeroVector)).Equals(FVector(700,0,100), 0.001));
    const FMatrix Container = FSCMeshImporter::Transform(FTranslationMatrix(FVector(0,1,0)));
    const FMatrix Placement = FSCMeshImporter::InstanceTransform(FTranslationMatrix(FVector(2,3,4)), W, true, true);
    const FMatrix InteriorWorld = FSCMeshImporter::Transform(W) * Placement * Container * FSCMeshImporter::Transform(HelperMatrix);
    TestTrue(TEXT("Interior placement composes in source helper frame"), FVector(InteriorWorld.TransformPosition(FVector::ZeroVector)).Equals(FVector(900,200,500), 0.001));
    auto& Bytes = Source.Files[TEXT("triangle.glb")];
    FFileHelper::SaveArrayToFile(Bytes, *(FPaths::ProjectSavedDir() / TEXT("Import/synthetic.glb")));
    TArray<uint8> HelperBytes = Bytes;
    for (int32 I = 20; I + 8 < HelperBytes.Num(); ++I)
        if (FMemory::Memcmp(HelperBytes.GetData() + I, "\"mesh\":0", 8) == 0) { HelperBytes[I + 1] = 'x'; break; }
    Source.Files.Add(TEXT("helper.glb"), MoveTemp(HelperBytes));
    FSCDerivedMesh Helper;
    TestTrue(TEXT("Hierarchy-only GLB is valid"), FSCMeshImporter::Derive(Source, TEXT("helper.glb"), Helper, Error));
    TestEqual(TEXT("Helper nodes retained"), Helper.Nodes.Num(), 1);
    Source.Files[TEXT("triangle.glb")][8] = 0;
    TestFalse(TEXT("truncated header rejected"), FSCMeshImporter::Derive(Source, TEXT("triangle.glb"), D, Error));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCReviewTar, "SCBridge.Review.TarPaths", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCReviewTar::RunTest(const FString&)
{
    TArray<uint8> Bytes;
    TarEntry(Bytes, TEXT("Hull.glb"), TEXT("Data/Mesh"), '0', TEXT("upper"));
    TarEntry(Bytes, TEXT("hull.glb"), TEXT("Data/Mesh"), '0', TEXT("lower"));
    Bytes.AddZeroed(1024); FSCTarPackageSource Tar(MoveTemp(Bytes));
    TestTrue(TEXT("case-distinct ustar entries"), Tar.IsValid());
    FString Error; TArray<uint8> Read;
    TestTrue(TEXT("prefix/name read"), Tar.ReadBytes(TEXT("Data/Mesh/Hull.glb"), Read, Error));
    TestEqual(TEXT("exact case selects bytes"), Read.Num() ? int32(Read[0]) : -1, int32('u'));
    TestFalse(TEXT("wrong case absent"), Tar.Exists(TEXT("data/Mesh/Hull.glb")));
    const FString LongPath = TEXT("Data/") + FString::ChrN(180, 'a') + TEXT(".glb");
    TarEntry(Bytes, TEXT("PaxHeader"), TEXT(""), 'x', PaxPath(LongPath));
    TarEntry(Bytes, TEXT("placeholder"), TEXT(""), '0', TEXT("pax"));
    TarEntry(Bytes, TEXT("@LongLink"), TEXT(""), 'L', TEXT("Data/gnu.glb"));
    TarEntry(Bytes, TEXT("placeholder2"), TEXT(""), '0', TEXT("gnu"));
    Bytes.AddZeroed(1024); FSCTarPackageSource LongTar(MoveTemp(Bytes));
    TestTrue(TEXT("PAX path and GNU long-name records"), LongTar.IsValid());
    TestTrue(TEXT("PAX override"), LongTar.Exists(LongPath));
    TestTrue(TEXT("GNU override"), LongTar.Exists(TEXT("Data/gnu.glb")));
    TarEntry(Bytes, TEXT("huge"), TEXT(""), '0', TEXT(""), uint64(MAX_int32) + 1);
    FSCTarPackageSource Huge(MoveTemp(Bytes));
    TestTrue(TEXT("explicit 2 GiB size rejection"), Huge.GetError().Contains(TEXT("2 GiB")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCReviewPartial, "SCBridge.Review.PartialImport", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCReviewPartial::RunTest(const FString&)
{
    auto Source = Triangle();
    Source.Add(TEXT("broken.glb"), TEXT("not a GLB"));
    Source.Add(TEXT("invalid.materials.json"), TEXT("not JSON"));
    Source.Add(TEXT("scene.json"), TEXT(R"({"version":1,"export_kind":"Decomposed","root_entity":{"entity_name":"Root","mesh_asset":"triangle.glb"},"children":[{"entity_name":"MissingMaterial","parent_entity_name":"Root","mesh_asset":"triangle.glb","material_sidecar":"missing.materials.json","source_transform_basis":"gltf_y_up"},{"entity_name":"InvalidMaterial","parent_entity_name":"Root","mesh_asset":"triangle.glb","material_sidecar":"invalid.materials.json","source_transform_basis":"gltf_y_up"},{"entity_name":"Broken","parent_entity_name":"Root","mesh_asset":"broken.glb","source_transform_basis":"gltf_y_up"},{"entity_name":"Grandchild","parent_entity_name":"Broken","mesh_asset":"triangle.glb","source_transform_basis":"gltf_y_up"},{"entity_name":"Good","parent_entity_name":"Root","mesh_asset":"triangle.glb","source_transform_basis":"gltf_y_up"},{"entity_name":"Missing","parent_entity_name":"Root","mesh_asset":"missing.glb","source_transform_basis":"gltf_y_up"},{"entity_name":"Orphan","parent_entity_name":"Absent","mesh_asset":"triangle.glb","source_transform_basis":"gltf_y_up"},{"entity_name":"OrphanChild","parent_entity_name":"Orphan","mesh_asset":"triangle.glb","source_transform_basis":"gltf_y_up"}]})"));
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    FSCImportOptions Options; Options.World = World; Options.bBuildCollision = false;
    FSCImportResult Result;
    const bool OK = FSCImporter::Import(Source, Options, Result);
    TestTrue(TEXT("broken GLB and missing sidecar do not abort healthy instances"), OK);
    TestEqual(TEXT("root plus healthy sibling"), Result.InstanceCount, 2);
    TestTrue(TEXT("recoverable warnings returned to caller"), Result.Warnings.Num() >= 7);
    TestTrue(TEXT("no fatal error"), Result.Error.IsEmpty());
    if (Result.Vehicle.IsValid()) TestEqual(TEXT("unanimated assets do not create rigs"), Result.Vehicle->Rigs.Num(), 0);
    if (Result.Vehicle.IsValid())
    {
#if WITH_EDITOR
        TestTrue(TEXT("Nanite keeps explicit tangents"), Result.Vehicle->Meshes[0]->GetNaniteSettings().bExplicitTangents);
        TestEqual(TEXT("fallback fast path"), Result.Vehicle->Meshes[0]->GetNaniteSettings().FallbackRelativeError, 0.0f);
#endif
    }
    FSCImporter::Unload(World); World->DestroyWorld(false);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCReviewScale, "SCBridge.Review.SmallAndZeroScale", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCReviewScale::RunTest(const FString&)
{
    auto Source = Triangle();
    for (double Scale : {0.001, 0.0})
    {
        Source.Files.Add(TEXT("scaled.glb"), EditGLB(Source.Files[TEXT("triangle.glb")], [&](auto J)
        {
            TArray<TSharedPtr<FJsonValue>> S; for (int I=0; I<3; ++I) S.Add(MakeShared<FJsonValueNumber>(Scale));
            J->GetArrayField(TEXT("nodes"))[0]->AsObject()->SetArrayField(TEXT("scale"), S);
        }));
        FSCDerivedMesh Mesh; FString Error;
        TestTrue(TEXT("zero/small scale does not reject GLB"), FSCMeshImporter::Derive(Source, TEXT("scaled.glb"), Mesh, Error));
        TestEqual(TEXT("helper transform retained"), Mesh.Nodes.Num(), 1);
        TestEqual(TEXT("only truly degenerate geometry skipped"), Mesh.Description.Triangles().Num(), Scale == 0 ? 0 : 1);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCReviewMaterials, "SCBridge.Review.MaterialSplit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCReviewMaterials::RunTest(const FString&)
{
    auto Source = Triangle();
    Source.Files[TEXT("triangle.glb")] = EditGLB(Source.Files[TEXT("triangle.glb")], [](auto J)
    {
        TArray<TSharedPtr<FJsonValue>> Materials;
        for (const TCHAR* Name : {TEXT("Opaque"), TEXT("Glass"), TEXT("Decal"), TEXT("Blended")}) { auto M=MakeShared<FJsonObject>(); M->SetStringField(TEXT("name"),Name); Materials.Add(MakeShared<FJsonValueObject>(M)); }
        J->SetArrayField(TEXT("materials"),Materials);
        auto Mesh=J->GetArrayField(TEXT("meshes"))[0]->AsObject(); auto P=Mesh->GetArrayField(TEXT("primitives"))[0]->AsObject();
        TArray<TSharedPtr<FJsonValue>> Primitives;
        for (int32 Slot = 0; Slot < 4; ++Slot)
        {
            auto Primitive = MakeShared<FJsonObject>(); Primitive->Values = P->Values;
            Primitive->SetNumberField(TEXT("material"), Slot); Primitives.Add(MakeShared<FJsonValueObject>(Primitive));
        }
        Mesh->SetArrayField(TEXT("primitives"), Primitives);
    });
    Source.Add(TEXT("scene.json"), TEXT(R"({"version":1,"export_kind":"Decomposed","root_entity":{"entity_name":"Root","mesh_asset":"triangle.glb","material_sidecar":"mixed.json"}})"));
    Source.Add(TEXT("mixed.json"), TEXT(R"({"submaterials":[{"submaterial_name":"Opaque","shader_family":"Illum"},{"submaterial_name":"Glass","shader_family":"GlassPBR"},{"submaterial_name":"Decal","shader_family":"MeshDecal"},{"submaterial_name":"Blended","shader_family":"Illum","alpha_config":{"mode":"blended"}}]})"));
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    FSCImportOptions Options; Options.World=World; Options.bBuildCollision=false; FSCImportResult Result;
    TestTrue(TEXT("mixed material import"),FSCImporter::Import(Source,Options,Result));
    TestEqual(TEXT("source asset count"),Result.MeshCount,1);
    TestEqual(TEXT("source instance count"),Result.InstanceCount,1);
    if (Result.Vehicle.IsValid())
    {
        TestEqual(TEXT("separate opaque and translucent meshes"),Result.Vehicle->Meshes.Num(),2);
        if (Result.Vehicle->Meshes.Num() == 2)
        {
            TestEqual(TEXT("opaque triangle count"), Result.MeshStats[TEXT("triangle.glb")].Triangles, 1);
            TestEqual(TEXT("glass, decal, blended triangle count"), Result.MeshStats[TEXT("triangle.glb_Translucent")].Triangles, 3);
            for (auto Mesh : Result.Vehicle->Meshes) TestEqual(TEXT("original material slot count"), Mesh->GetStaticMaterials().Num(), 4);
            const auto& Sections = Result.Vehicle->Meshes[1]->GetRenderData()->LODResources[0].Sections;
            TestEqual(TEXT("all source sections retained"), Sections.Num(), 4);
            for (int32 I = 0; I < Sections.Num(); ++I)
            {
                TestEqual(TEXT("original translucent slot index"), Sections[I].MaterialIndex, uint32(I));
                TestEqual(TEXT("opaque slot has no translucent triangles"), Sections[I].NumTriangles, uint32(I == 0 ? 0 : 1));
            }
            // WP-U2: no glass/decal/blended section of the split mesh may render with an opaque material.
            for (UActorComponent* Component : Result.Vehicle->GetComponents())
                if (auto* C = Cast<UStaticMeshComponent>(Component); C && C->GetStaticMesh() == Result.Vehicle->Meshes[1])
                    for (int32 Slot = 1; Slot < 4; ++Slot)
                    {
                        UMaterialInterface* M = C->GetMaterial(Slot);
                        TestTrue(FString::Printf(TEXT("split slot %d is decal or translucent"), Slot), M
                            && (M->GetMaterial()->MaterialDomain == MD_DeferredDecal || IsTranslucentBlendMode(M->GetBlendMode())));
                    }
        }
#if WITH_EDITOR
        int32 Nanite=0; for (auto Mesh:Result.Vehicle->Meshes) Nanite+=Mesh->GetNaniteSettings().bEnabled;
        TestEqual(TEXT("only opaque enables Nanite"),Nanite,1);
        TestTrue(TEXT("explicit tangents"),Result.Vehicle->Meshes[0]->GetNaniteSettings().bExplicitTangents);
        TestEqual(TEXT("zero fallback error"),Result.Vehicle->Meshes[0]->GetNaniteSettings().FallbackRelativeError,0.0f);
#endif
    }
    FSCImporter::Unload(World); World->DestroyWorld(false); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFolderTarImport, "SCBridge.Package.FolderTarImport", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFolderTarImport::RunTest(const FString&)
{
    FString Folder, Tar;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SCPackage="), Folder) || !FParse::Value(FCommandLine::Get(), TEXT("SCVerifyTar="), Tar))
    { AddError(TEXT("FolderTarImport requires -SCPackage and -SCVerifyTar")); return false; }
    UWorld* World = nullptr;
    for (const auto& Context : GEngine->GetWorldContexts())
        if (Context.World() && (Context.WorldType == EWorldType::Editor || Context.World()->IsGameWorld())) World = Context.World();
    TestTrue(TEXT("Folder/tar asset counts, content hashes, collision, representations and GC"), FSCImporter::Verify(Folder, Tar, World));
    return true;
}
#endif
#include "SCImporter.h"
bool USCImportLibrary::RunUnitTests()
{
#if WITH_DEV_AUTOMATION_TESTS
    bool Passed = true;
    TArray<FString> Names = {TEXT("FSCReaderSparse"), TEXT("FSCPaths"), TEXT("FSCBasis"), TEXT("FSCReviewTar"), TEXT("FSCReviewPartial"), TEXT("FSCReviewScale"), TEXT("FSCReviewMaterials"),
        TEXT("FSCMaterialsFamilies"), TEXT("FSCMaterialsMasters"), TEXT("FSCMaterialsMasterParameters"), TEXT("FSCMaterialsTextures"), TEXT("FSCRigMath"), TEXT("FSCSkinBuffers"), TEXT("FSCRigLights"), TEXT("FSCRigPlayback"), TEXT("FSCRigCyclic"), TEXT("FSCLightDefaults"), TEXT("FSCLightAuthoredModel"),
        TEXT("FSCLightRoomVolumes"), TEXT("FSCLightAreaConfinement")};
    FString Folder;
    if (FParse::Value(FCommandLine::Get(), TEXT("SCPackage="), Folder)) Names.Append({TEXT("FSCMaterialsPackageRules"), TEXT("FSCFolderTarImport")});
    if (FParse::Param(FCommandLine::Get(), TEXT("SCFirstPersonTests"))) Names = {TEXT("FSCFirstPersonCollision")};
    if (FParse::Param(FCommandLine::Get(), TEXT("SCPlayerTests")))
    {
        Names = {TEXT("FSCPlayerBlendWeights"), TEXT("FSCPlayerAdditive"), TEXT("FSCPlayerWeaponStates"), TEXT("FSCPlayerFirstPersonScale")};
        if (FParse::Value(FCommandLine::Get(), TEXT("SCPlayerPackage="), Folder)) Names.Add(TEXT("FSCPlayerKitRig"));
    }
    for (const FString& Name : Names)
    {
        FAutomationTestFramework::Get().StartTestByName(Name, 0);
        if (!GIsAutomationTesting) return false;
        FAutomationTestExecutionInfo Info;
        const bool OK = FAutomationTestFramework::Get().StopTest(Info);
        UE_LOG(LogSCImport, Display, TEXT("UNIT_TEST name=%s passed=%d"), *Name, OK);
        for (const auto& Entry : Info.GetEntries()) UE_LOG(LogSCImport, Display, TEXT("UNIT_DETAIL %s"), *Entry.ToString());
        Passed &= OK;
    }
    return Passed;
#else
    return false;
#endif
}
