// WP-O1-4b: a runtime rig mesh made savable (SCRigAssets) saves, and loads back under another package name with its
// skeleton, sockets, bone order, geometry and CPU-readable skin weights.
#include "Misc/AutomationTest.h"
#include "SCMeshImporter.h"
#include "SCRigAssets.h"
#include "SCRigComponent.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "PhysicsEngine/BodySetup.h"
#include "SCSkeletonBuilder.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
/** Two rig nodes (Root, Arm 100 cm along X) and, with bSoft, one triangle: two corners on Root, one on Arm. */
FSCDerivedMesh SyntheticRig(bool bSoft)
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
        A.GetVertexInstanceColors()[C] = FVector4f(1, 1, 1, 1);
        Corners.Add(C);
        FSCVertexWeights W;
        for (int32& Joint : W.Joints) Joint = I == 2 ? 11 : 10; // weight 1 on the first, the rest unused
        Data.SkinWeights.Add(V, W);
    }
    Data.Description.CreateTriangle(Group, Corners);
    return Data;
}

/** Into packages of their own under /Game/SCTests, saved; returns the mesh's package file. */
FString SaveRig(USkeletalMesh* Mesh, const FString& Name)
{
    USkeleton* Skeleton = Mesh->GetSkeleton();
    auto Persist = [](UObject* Object, const FString& PackageName) {
        UPackage* Package = CreatePackage(*PackageName);
        Object->Rename(*FPackageName::GetShortName(PackageName), Package, REN_DontCreateRedirectors | REN_NonTransactional);
        Object->ClearFlags(RF_Transient);
        Object->SetFlags(RF_Public | RF_Standalone);
        ForEachObjectWithOuter(Object, [](UObject* Inner) { Inner->ClearFlags(RF_Transient); });
        return Package;
    };
    UPackage* SkeletonPackage = Persist(Skeleton, TEXT("/Game/SCTests/") + Name + TEXT("_Skeleton"));
    UPackage* MeshPackage = Persist(Mesh, TEXT("/Game/SCTests/") + Name);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    const FString SkeletonFile = FPackageName::LongPackageNameToFilename(SkeletonPackage->GetName(), FPackageName::GetAssetPackageExtension());
    const FString MeshFile = FPackageName::LongPackageNameToFilename(MeshPackage->GetName(), FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(SkeletonPackage, Skeleton, *SkeletonFile, Args) || !UPackage::SavePackage(MeshPackage, Mesh, *MeshFile, Args))
        return FString();
    return MeshFile;
}

bool CheckRig(FAutomationTestBase& Test, bool bSoft)
{
    const FString Name = FString::Printf(TEXT("Rig_%s_%s"), bSoft ? TEXT("Soft") : TEXT("Sockets"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    const FSCDerivedMesh Data = SyntheticRig(bSoft);
    USkeleton* Skeleton = nullptr;
    USkeletalMesh* Mesh = FSCSkeletonBuilder::Build(Data, GetTransientPackage(), Skeleton);
    FString Error;
    if (!Test.TestTrue(TEXT("runtime build: ") + Error, FSCSkeletalMeshImporter::Build(Data, Mesh, Error))) return false;
    if (!Test.TestTrue(TEXT("made savable: ") + Error, SCRigAssets::MakeSavable(Mesh, Data, Error))) return false;
    const FString File = SaveRig(Mesh, Name);
    if (!Test.TestFalse(TEXT("saved"), File.IsEmpty())) return false;
    // Loaded back as another package: what the file holds, not the object still in memory.
    const FString Copy = FPaths::GetPath(File) / Name + TEXT("_Reload") + FPackageName::GetAssetPackageExtension();
    IFileManager::Get().Copy(*Copy, *File);
    USkeletalMesh* Loaded = LoadObject<USkeletalMesh>(nullptr, *FString::Printf(TEXT("/Game/SCTests/%s_Reload.%s"), *Name, *Name));
    const bool bLoaded = Test.TestNotNull(TEXT("loads"), Loaded);
    if (bLoaded)
    {
        const FReferenceSkeleton& Ref = Loaded->GetRefSkeleton();
        Test.TestEqual(TEXT("bones: SC_RigRoot, then the rig nodes in order"), Ref.GetNum(), 3);
        if (Ref.GetNum() == 3)
        {
            Test.TestEqual(TEXT("bone 0"), Ref.GetBoneName(0), FName(TEXT("SC_RigRoot")));
            Test.TestEqual(TEXT("bone 2 (clip tracks index it)"), Ref.GetBoneName(2), FName(TEXT("Arm")));
            Test.TestEqual(TEXT("the reference pose"), Ref.GetRefBonePose()[2].GetLocation(), FVector(100, 0, 0));
        }
        Test.TestTrue(TEXT("sockets on the skeleton"), Loaded->GetSkeleton() && Loaded->GetSkeleton()->Sockets.Num() == 2);
        const FSkeletalMeshRenderData* Render = Loaded->GetResourceForRendering();
        Test.TestTrue(TEXT("LOD 0"), Render && Render->LODRenderData.Num() == 1);
        if (Render && Render->LODRenderData.Num() == 1)
        {
            const FSkeletalMeshLODRenderData& LOD = Render->LODRenderData[0];
            Test.TestEqual(TEXT("vertices"), int32(LOD.GetNumVertices()), bSoft ? 3 : 0);
            if (bSoft)
            {
                Test.TestTrue(TEXT("skin weights readable on the CPU"), LOD.SkinWeightVertexBuffer.GetNeedsCPUAccess());
                Test.TestTrue(TEXT("the material slot"), Loaded->GetMaterials().Num() == 1 && Loaded->GetMaterials()[0].MaterialSlotName == TEXT("Slot_0"));
                // The rig's collision in the built vertex order, and a rig body from it.
                TArray<uint8> Bytes;
                FCustomVersionContainer Versions;
                FString CollisionError;
                if (Test.TestTrue(TEXT("collision derived: ") + CollisionError, SCRigAssets::DeriveCollision(Loaded, Bytes, Versions, CollisionError)))
                {
                    USCRigComponent* Rig = NewObject<USCRigComponent>(GetTransientPackage());
                    Rig->SetSkeletalMesh(Loaded);
                    Test.TestTrue(TEXT("a rig body from it"), Rig->InitializeCollision(Bytes, Versions));
                    const UBodySetup* Body = Rig->GetBodySetup();
                    Test.TestTrue(TEXT("one trimesh particle per render vertex"), Body && Body->TriMeshGeometries.Num() == 1 && Body->TriMeshGeometries[0] &&
                                                                       Body->TriMeshGeometries[0]->Particles().Size() == LOD.GetNumVertices());
                    Test.TestFalse(TEXT("kept for a save (editor host)"), Rig->SavedCollision.IsEmpty());
                }
            }
        }
    }
    for (const FString& F : {File, Copy, FPaths::GetPath(File) / Name + TEXT("_Skeleton") + FPackageName::GetAssetPackageExtension()})
        IFileManager::Get().Delete(*F, false, true, true);
    return bLoaded;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRigAssetsTest, "SCBridge.Prepare.RigAssets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRigAssetsTest::RunTest(const FString&)
{
    CheckRig(*this, true);
    CheckRig(*this, false);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
