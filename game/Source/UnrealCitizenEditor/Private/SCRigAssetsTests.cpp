// WP-O1-4b: a runtime rig mesh made savable (SCRigAssets) saves, and loads back from disk — mesh and skeleton read from
// their files, not the objects still in memory — with its bone order, reference pose, sockets, geometry, the bone each
// vertex is weighted to, its vertex colours and CPU-readable skin weights; a rig component's collision, saved with it,
// restores into a body with one trimesh particle per render vertex.
#include "Misc/AutomationTest.h"
#include "SCMeshImporter.h"
#include "SCRigAssets.h"
#include "SCTextureSource.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Engine/Texture2D.h"
#include "Modules/ModuleManager.h"
#include "SCRigComponent.h"
#include "SCSkeletonBuilder.h"
#include "Animation/Skeleton.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkeletalMeshSourceModel.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/BodySetup.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshAttributes.h"
#include "UObject/LinkerLoad.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
const FLinearColor Tint(0.5f, 0.25f, 0.75f, 1.f);

/** Two rig nodes (Root, Arm 100 cm along X) and, with bSoft, one triangle in Colour: a corner on Root, one on Arm and
 *  one weighted half to each. */
FSCDerivedMesh SyntheticRig(bool bSoft, const FLinearColor& Colour)
{
    FSCDerivedMesh Data;
    FSCRigNode Root, Arm;
    Root.SourceName = TEXT("Root");
    Root.GltfIndex = 10;
    Arm.SourceName = TEXT("Arm");
    Arm.Parent = 0;
    Arm.GltfIndex = 11;
    Arm.Local = FTransform(FVector(100, 0, 0));
    Data.RigNodes = {Root, Arm};
    Data.GltfToRig = {{10, 0}, {11, 1}};
    Data.MaterialNames = {TEXT("Hull")};
    if (!bSoft) return Data;
    Data.bHasGeometry = true;
    FStaticMeshAttributes A(Data.Description);
    A.Register();
    A.GetVertexInstanceUVs().SetNumChannels(2);
    const FPolygonGroupID Group = Data.Description.CreatePolygonGroup();
    TArray<FVertexInstanceID> Corners;
    const FVector3f Points[3] = {FVector3f(0, 0, 0), FVector3f(0, 50, 0), FVector3f(100, 0, 0)};
    for (int32 I = 0; I < 3; ++I)
    {
        const FVertexID V = Data.Description.CreateVertex();
        A.GetVertexPositions()[V] = Points[I];
        const FVertexInstanceID C = Data.Description.CreateVertexInstance(V);
        A.GetVertexInstanceNormals()[C] = FVector3f(0, 0, 1);
        A.GetVertexInstanceTangents()[C] = FVector3f(1, 0, 0);
        A.GetVertexInstanceBinormalSigns()[C] = 1.f;
        A.GetVertexInstanceColors()[C] = FVector4f(Colour);
        Corners.Add(C);
        FSCVertexWeights W;
        for (int32& Joint : W.Joints) Joint = I == 2 ? 11 : 10; // weight 1 on the first, the rest unused
        if (I == 1)
        {
            W.Joints[1] = 11;
            W.Weights[0] = W.Weights[1] = 0.5f;
        }
        Data.SkinWeights.Add(V, W);
    }
    Data.Description.CreateTriangle(Group, Corners);
    return Data;
}

UPackage* Persist(UObject* Object, const FString& PackageName)
{
    UPackage* Package = CreatePackage(*PackageName);
    Object->Rename(*FPackageName::GetShortName(PackageName), Package, REN_DontCreateRedirectors | REN_NonTransactional);
    Object->ClearFlags(RF_Transient);
    Object->SetFlags(RF_Public | RF_Standalone);
    ForEachObjectWithOuter(Object, [](UObject* Inner) { Inner->ClearFlags(RF_Transient); });
    return Package;
}
bool Save(UPackage* Package, UObject* Asset)
{
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    return UPackage::SavePackage(Package, Asset, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
}
/** Out of the way in memory, so a load of its name reads the file. */
void Forget(UPackage* Package)
{
    Package->Rename(*FString::Printf(TEXT("/Temp/SCTestsForgotten_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)), nullptr,
                    REN_DontCreateRedirectors | REN_NonTransactional);
}

void CheckRig(FAutomationTestBase& Test, bool bSoft, const FLinearColor& Colour = Tint)
{
    const FString Name = FString::Printf(TEXT("Rig_%s_%s"), bSoft ? TEXT("Soft") : TEXT("Sockets"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    const FString Folder = TEXT("/Game/SCTests/");
    TArray<FString> Files;
    for (const TCHAR* Suffix : {TEXT(""), TEXT("_Skeleton"), TEXT("_Rig"), TEXT("_Reload"), TEXT("_RigReload")})
        Files.Add(FPackageName::LongPackageNameToFilename(Folder + Name + Suffix, FPackageName::GetAssetPackageExtension()));
    TArray<TWeakObjectPtr<UPackage>> Opened; // packages loaded from the test's files: their loaders keep the files open
    ON_SCOPE_EXIT
    {
        for (const TWeakObjectPtr<UPackage>& Package : Opened)
            if (Package.IsValid()) ResetLoaders(Package.Get());
        for (const FString& F : Files) IFileManager::Get().Delete(*F, false, true, true);
        IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Files[0]), false, false); // only when empty
    };

    const FSCDerivedMesh Data = SyntheticRig(bSoft, Colour);
    USkeleton* RuntimeSkeleton = nullptr;
    USkeletalMesh* Runtime = FSCSkeletonBuilder::Build(Data, GetTransientPackage(), RuntimeSkeleton);
    FString Error;
    if (!Test.TestTrue(TEXT("runtime build: ") + Error, FSCSkeletalMeshImporter::Build(Data, Runtime, Error))) return;
    // Keep as assets' path: a savable copy of the runtime mesh (MakeSavable on a new mesh), the runtime one untouched.
    USkeletalMesh* Mesh = SCRigAssets::SavableCopy(Runtime, &Data, Error);
    if (!Test.TestNotNull(TEXT("savable copy: ") + Error, Mesh)) return;
    USkeleton* Skeleton = Mesh->GetSkeleton();
    Test.TestTrue(TEXT("a copy, on a copy of the skeleton"), Mesh != Runtime && Skeleton && Skeleton != RuntimeSkeleton);
    Test.TestFalse(TEXT("the runtime mesh keeps no mesh description"), Runtime->HasMeshDescription(0));
    // The source model is saved as written (a description with the bones even without geometry), never recovered at load.
    const FMeshDescriptionBulkData* Bulk = Mesh->GetNumSourceModels() ? Mesh->GetSourceModel(0).GetMeshDescriptionBulkData() : nullptr;
    Test.TestTrue(TEXT("LOD 0's source model has a payload"), Bulk && !Bulk->IsEmpty());
    // A rig component using it, its collision in the built vertex order (kept for the save on an editor host).
    USCRigComponent* Rig = NewObject<USCRigComponent>(GetTransientPackage());
    Rig->SetSkeletalMesh(Mesh);
    if (bSoft && !Test.TestTrue(TEXT("rig collision: ") + Error, SCRigAssets::RefreshCollision(Rig, Error))) return;
    // Saved in a pose (a hangar prepared open): the Arm 30 cm up.
    Rig->LocalPose = Mesh->GetRefSkeleton().GetRefBonePose();
    if (Rig->LocalPose.Num() == 3) Rig->LocalPose[2].AddToTranslation(FVector(0, 0, 30));

    UPackage* SkeletonPackage = Persist(Skeleton, Folder + Name + TEXT("_Skeleton"));
    UPackage* MeshPackage = Persist(Mesh, Folder + Name);
    UPackage* RigPackage = Persist(Rig, Folder + Name + TEXT("_Rig"));
    Opened.Append({SkeletonPackage, MeshPackage, RigPackage}); // a saved package's bulk data reads from its file
    if (!Test.TestTrue(TEXT("saved"), Save(SkeletonPackage, Skeleton) && Save(MeshPackage, Mesh) && Save(RigPackage, Rig))) return;
    // Loaded back as other packages: the mesh and the rig from copies of their files, the skeleton from its own file (the
    // objects still in memory are moved out of the way).
    IFileManager::Get().Copy(*Files[3], *Files[0]);
    IFileManager::Get().Copy(*Files[4], *Files[2]);
    Forget(SkeletonPackage);
    USkeletalMesh* Loaded = LoadObject<USkeletalMesh>(nullptr, *FString::Printf(TEXT("%s%s_Reload.%s"), *Folder, *Name, *Name));
    if (!Test.TestNotNull(TEXT("the mesh loads"), Loaded)) return;
    Opened.Add(Loaded->GetPackage());
    if (Loaded->GetSkeleton()) Opened.Add(Loaded->GetSkeleton()->GetPackage());
    Test.TestTrue(TEXT("its skeleton was read from its file"), Loaded->GetSkeleton() && Loaded->GetSkeleton() != Skeleton);

    const FReferenceSkeleton& Ref = Loaded->GetRefSkeleton();
    Test.TestEqual(TEXT("bones: SC_RigRoot, then the rig nodes in order"), Ref.GetNum(), 3);
    if (Ref.GetNum() == 3)
    {
        Test.TestEqual(TEXT("bone 0"), Ref.GetBoneName(0), FName(TEXT("SC_RigRoot")));
        Test.TestEqual(TEXT("bone 2 (clip tracks index it)"), Ref.GetBoneName(2), FName(TEXT("Arm")));
        Test.TestEqual(TEXT("the reference pose"), Ref.GetRefBonePose()[2].GetLocation(), FVector(100, 0, 0));
    }
    // A socket per rig node, on its bone.
    const USkeleton* LoadedSkeleton = Loaded->GetSkeleton();
    for (const TCHAR* Node : {TEXT("Root"), TEXT("Arm")})
    {
        const TObjectPtr<USkeletalMeshSocket>* Socket = LoadedSkeleton ? LoadedSkeleton->Sockets.FindByPredicate([&](const USkeletalMeshSocket* S) {
            return S && S->SocketName == Node;
        }) : nullptr;
        Test.TestTrue(FString::Printf(TEXT("socket %s on its bone"), Node), Socket && (*Socket)->BoneName == Node && Ref.FindBoneIndex(Node) != INDEX_NONE);
    }
    const FSkeletalMeshRenderData* Render = Loaded->GetResourceForRendering();
    if (!Test.TestTrue(TEXT("LOD 0"), Render && Render->LODRenderData.Num() == 1)) return;
    const FSkeletalMeshLODRenderData& LOD = Render->LODRenderData[0];
    Test.TestEqual(TEXT("vertices"), int32(LOD.GetNumVertices()), bSoft ? 3 : 0);
    if (!bSoft) return;

    Test.TestTrue(TEXT("skin weights readable on the CPU"), LOD.SkinWeightVertexBuffer.GetNeedsCPUAccess());
    Test.TestTrue(TEXT("the material slot"), Loaded->GetMaterials().Num() == 1 && Loaded->GetMaterials()[0].MaterialSlotName == TEXT("Slot_0"));
    // Each vertex weighted to its bones (the corner at X 100 to Arm, the one at Y 50 half to each, at 16-bit precision,
    // the third to Root), in the runtime build's colours (black ones too: the engine would paint an all-black mesh white).
    const FColor Expected = Colour.ToFColorSRGB();
    int32 Blended = 0;
    for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
        for (uint32 V = Section.BaseVertexIndex; V < Section.BaseVertexIndex + Section.NumVertices; ++V)
        {
            const FSkinWeightInfo W = LOD.SkinWeightVertexBuffer.GetVertexSkinWeights(V);
            TMap<int32, int32> Bones; // bone -> weight (of 65535)
            for (int32 K = 0; K < MAX_TOTAL_INFLUENCES; ++K)
                if (W.InfluenceWeights[K] && Section.BoneMap.IsValidIndex(W.InfluenceBones[K]))
                    Bones.FindOrAdd(Section.BoneMap[W.InfluenceBones[K]]) += W.InfluenceWeights[K];
            const FVector3f P = LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(V);
            if (P.Y > 25.f)
            {
                ++Blended;
                Test.TestTrue(FString::Printf(TEXT("vertex %u: half to Root, half to Arm at 16 bits"), V),
                              Bones.Num() == 2 && FMath::Abs(Bones.FindRef(1) - 32768) <= 1 && FMath::Abs(Bones.FindRef(2) - 32768) <= 1);
            }
            else
                Test.TestTrue(FString::Printf(TEXT("vertex %u's bone"), V), Bones.Num() == 1 && Bones.Contains(P.X > 50.f ? 2 : 1));
            Test.TestEqual(FString::Printf(TEXT("vertex %u's colour"), V), LOD.StaticVertexBuffers.ColorVertexBuffer.VertexColor(V), Expected);
        }
    Test.TestEqual(TEXT("one blended vertex"), Blended, 1);
    // The saved rig, loaded through its own reference to the mesh (both packages out of memory, so they are read from
    // their files) and registered in a world: its body is not saved, its collision bytes are; it restores them when it
    // registers, and the body follows a posed bone.
    Forget(MeshPackage);
    Forget(RigPackage);
    USCRigComponent* LoadedRig = LoadObject<USCRigComponent>(nullptr, *FString::Printf(TEXT("%s%s_RigReload.%s_Rig"), *Folder, *Name, *Name));
    if (!Test.TestNotNull(TEXT("the rig loads"), LoadedRig)) return;
    Opened.Add(LoadedRig->GetPackage());
    USkeletalMesh* RigMesh = LoadedRig->GetSkeletalMeshAsset();
    if (!Test.TestTrue(TEXT("its mesh resolves from the file"), RigMesh && RigMesh != Mesh && RigMesh->GetName() == Name)) return;
    Opened.Add(RigMesh->GetPackage());
    Test.TestFalse(TEXT("no saved body"), LoadedRig->HasCollisionBody());
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT
    {
        if (LoadedRig->IsRegistered()) LoadedRig->UnregisterComponent();
        World->DestroyWorld(false);
    };
    // Registered as the engine may (the reference pose set at init, without our bone refresh): it stands in its saved pose.
    LoadedRig->bUseRefPoseOnInitAnim = true;
    LoadedRig->RegisterComponentWithWorld(World);
    if (!Test.TestTrue(TEXT("its body restored when it registered"), LoadedRig->HasCollisionBody())) return;
    const UBodySetup* Body = LoadedRig->GetBodySetup();
    Test.TestTrue(TEXT("one trimesh particle per render vertex"), Body && Body->TriMeshGeometries.Num() == 1 && Body->TriMeshGeometries[0] &&
                                                               Body->TriMeshGeometries[0]->Particles().Size() == LOD.GetNumVertices());
    Test.TestEqual(TEXT("its skin bones saved, not scanned at load (Root, Arm)"), LoadedRig->GetSkinBones(), TArray<int32>({1, 2}));
    const FVector ArmAt = LoadedRig->GetSocketLocation(TEXT("Arm"));
    Test.TestTrue(FString::Printf(TEXT("the saved pose at registration: the Arm socket at %s"), *ArmAt.ToString()), ArmAt.Equals(FVector(100, 0, 30), 0.1));
    TArray<FVector3f> Saved;
    if (!Test.TestTrue(TEXT("the restored body reads"), LoadedRig->ReadCollisionVertices(Saved))) return;
    for (const FVector3f& P : Saved) // the fixture's bind positions lie at Z 0
        Test.TestTrue(FString::Printf(TEXT("restored particle at %s in the saved pose"), *P.ToString()),
                      FMath::IsNearlyEqual(P.Z, P.X > 50.f ? 30.f : P.Y > 25.f ? 15.f : 0.f, 0.1f));
    TArray<FVector3f> Before, After;
    LoadedRig->ResetPose();
    if (!Test.TestTrue(TEXT("the live body reads"), LoadedRig->ReadCollisionVertices(Before))) return;
    LoadedRig->LocalPose[2].AddToTranslation(FVector(0, 0, 30)); // Arm up 30 cm
    LoadedRig->RefreshBoneTransforms();
    LoadedRig->ReadCollisionVertices(After);
    // Each particle, known by its bind position, where its weights put it: the Arm corner up 30 cm, the blended one 15,
    // the Root one where it was.
    if (!Test.TestEqual(TEXT("the posed body has every particle"), After.Num(), Before.Num())) return;
    for (int32 I = 0; I < Before.Num(); ++I)
    {
        const FVector3f Rise(0, 0, Before[I].X > 50.f ? 30.f : Before[I].Y > 25.f ? 15.f : 0.f);
        Test.TestTrue(FString::Printf(TEXT("particle at %s posed"), *Before[I].ToString()), After[I].Equals(Before[I] + Rise, 0.1f));
    }
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRigAssetsTest, "SCBridge.Prepare.RigAssets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRigAssetsTest::RunTest(const FString&)
{
    CheckRig(*this, true);
    CheckRig(*this, true, FLinearColor::Black);              // an opaque black mask
    CheckRig(*this, true, FLinearColor(0.f, 0.f, 0.f, 0.f)); // transparent black
    CheckRig(*this, false);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#if WITH_DEV_AUTOMATION_TESTS
// A float (HDR) texture, as an EXR projector gobo imports, keeps its pixels as a float source through a save and a load.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCTextureSourceTest, "SCBridge.Prepare.TextureSource", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCTextureSourceTest::RunTest(const FString&)
{
    auto EXR = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")).CreateImageWrapper(EImageFormat::EXR);
    const float Pixels[] = {2, 1, 0.5, 1, 2, 1, 0.5, 1, 2, 1, 0.5, 1, 2, 1, 0.5, 1};
    if (!TestTrue(TEXT("synthetic HDR encoder"), EXR && EXR->SetRaw(Pixels, sizeof(Pixels), 2, 2, ERGBFormat::RGBAF, 32))) return false;
    const TArray64<uint8>& Compressed = EXR->GetCompressed();
    UTexture2D* Texture = FImageUtils::ImportBufferAsTexture2D(TArray<uint8>(Compressed.GetData(), int32(Compressed.Num())));
    if (!TestNotNull(TEXT("the EXR imports"), Texture)) return false;
    FString Error;
    if (!TestTrue(TEXT("a source: ") + Error, SCTextureSource::Add(Texture, Error))) return false;
    TestTrue(TEXT("a float source, compressed as HDR"), Texture->Source.IsHDR(Texture->Source.GetFormat()) && Texture->CompressionSettings == TC_HDR);

    const FString PackageName = FString::Printf(TEXT("/Game/SCTests/Gobo_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    const FString File = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    UPackage* Package = Persist(Texture, PackageName);
    TWeakObjectPtr<UPackage> Loaded;
    ON_SCOPE_EXIT
    {
        ResetLoaders(Package);
        if (Loaded.IsValid()) ResetLoaders(Loaded.Get());
        IFileManager::Get().Delete(*File, false, true, true);
        IFileManager::Get().DeleteDirectory(*FPaths::GetPath(File), false, false);
    };
    if (!TestTrue(TEXT("saved"), Save(Package, Texture))) return false;
    Forget(Package);
    UTexture2D* Reloaded = LoadObject<UTexture2D>(nullptr, *(PackageName + TEXT(".") + FPackageName::GetShortName(PackageName)));
    if (!TestNotNull(TEXT("the texture loads"), Reloaded)) return false;
    Loaded = Reloaded->GetPackage();
    TArray64<uint8> Mip;
    if (!TestTrue(TEXT("its source reads"), Reloaded->Source.GetMipData(Mip, 0) && Reloaded->Source.IsHDR(Reloaded->Source.GetFormat()))) return false;
    // The first pixel's red, 2.0, in the source's float layout.
    const float Red = Reloaded->Source.GetFormat() == TSF_RGBA16F ? float(reinterpret_cast<const FFloat16*>(Mip.GetData())[0])
                                                                   : reinterpret_cast<const float*>(Mip.GetData())[0];
    TestEqual(TEXT("an HDR value survives"), Red, 2.f);
    return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
