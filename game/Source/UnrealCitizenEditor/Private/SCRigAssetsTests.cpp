// WP-O1-4b: a runtime rig mesh made savable (SCRigAssets) saves, and loads back from disk — mesh and skeleton read from
// their files, not the objects still in memory — with its bone order, reference pose, sockets, geometry, the bone each
// vertex is weighted to, its vertex colours and CPU-readable skin weights; a rig component's collision, saved with it,
// restores into a body with one trimesh particle per render vertex.
#include "Misc/AutomationTest.h"
#include "SCMeshImporter.h"
#include "SCRigAssets.h"
#include "SCRigComponent.h"
#include "SCSkeletonBuilder.h"
#include "Animation/Skeleton.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
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

/** Two rig nodes (Root, Arm 100 cm along X) and, with bSoft, one triangle in Tint: two corners on Root, one on Arm. */
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
        A.GetVertexInstanceColors()[C] = FVector4f(Tint);
        Corners.Add(C);
        FSCVertexWeights W;
        for (int32& Joint : W.Joints) Joint = I == 2 ? 11 : 10; // weight 1 on the first, the rest unused
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

void CheckRig(FAutomationTestBase& Test, bool bSoft)
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

    const FSCDerivedMesh Data = SyntheticRig(bSoft);
    USkeleton* Skeleton = nullptr;
    USkeletalMesh* Mesh = FSCSkeletonBuilder::Build(Data, GetTransientPackage(), Skeleton);
    FString Error;
    if (!Test.TestTrue(TEXT("runtime build: ") + Error, FSCSkeletalMeshImporter::Build(Data, Mesh, Error))) return;
    if (!Test.TestTrue(TEXT("made savable: ") + Error, SCRigAssets::MakeSavable(Mesh, Data, Error))) return;
    // A rig component using it, its collision in the built vertex order (kept for the save on an editor host).
    USCRigComponent* Rig = NewObject<USCRigComponent>(GetTransientPackage());
    Rig->SetSkeletalMesh(Mesh);
    if (bSoft && !Test.TestTrue(TEXT("rig collision: ") + Error, SCRigAssets::RefreshCollision(Rig, Error))) return;

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
    Test.TestTrue(TEXT("sockets on the skeleton"), Loaded->GetSkeleton() && Loaded->GetSkeleton()->Sockets.Num() == 2);
    const FSkeletalMeshRenderData* Render = Loaded->GetResourceForRendering();
    if (!Test.TestTrue(TEXT("LOD 0"), Render && Render->LODRenderData.Num() == 1)) return;
    const FSkeletalMeshLODRenderData& LOD = Render->LODRenderData[0];
    Test.TestEqual(TEXT("vertices"), int32(LOD.GetNumVertices()), bSoft ? 3 : 0);
    if (!bSoft) return;

    Test.TestTrue(TEXT("skin weights readable on the CPU"), LOD.SkinWeightVertexBuffer.GetNeedsCPUAccess());
    Test.TestTrue(TEXT("the material slot"), Loaded->GetMaterials().Num() == 1 && Loaded->GetMaterials()[0].MaterialSlotName == TEXT("Slot_0"));
    // Each vertex weighted to its bone (the corner at X 100 to Arm, the others to Root), in the runtime build's colours.
    const FColor Expected = Tint.ToFColorSRGB();
    for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
        for (uint32 V = Section.BaseVertexIndex; V < Section.BaseVertexIndex + Section.NumVertices; ++V)
        {
            const FSkinWeightInfo W = LOD.SkinWeightVertexBuffer.GetVertexSkinWeights(V);
            int32 Strongest = 0;
            for (int32 K = 1; K < MAX_TOTAL_INFLUENCES; ++K)
                if (W.InfluenceWeights[K] > W.InfluenceWeights[Strongest]) Strongest = K;
            const int32 Bone = Section.BoneMap.IsValidIndex(W.InfluenceBones[Strongest]) ? Section.BoneMap[W.InfluenceBones[Strongest]] : INDEX_NONE;
            const bool bArm = LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(V).X > 50.f;
            Test.TestEqual(FString::Printf(TEXT("vertex %u's bone"), V), Bone, bArm ? 2 : 1);
            Test.TestEqual(FString::Printf(TEXT("vertex %u's colour"), V), LOD.StaticVertexBuffers.ColorVertexBuffer.VertexColor(V), Expected);
        }
    // The saved rig: its body is not saved, its collision bytes are; restored, one trimesh particle per render vertex.
    USCRigComponent* LoadedRig = LoadObject<USCRigComponent>(nullptr, *FString::Printf(TEXT("%s%s_RigReload.%s_Rig"), *Folder, *Name, *Name));
    if (!Test.TestNotNull(TEXT("the rig loads"), LoadedRig)) return;
    Opened.Add(LoadedRig->GetPackage());
    Test.TestFalse(TEXT("no saved body"), LoadedRig->HasCollisionBody());
    LoadedRig->SetSkeletalMesh(Loaded);
    Test.TestTrue(TEXT("its body restores from the saved bytes"), LoadedRig->RestoreSavedCollision());
    const UBodySetup* Body = LoadedRig->GetBodySetup();
    Test.TestTrue(TEXT("one trimesh particle per render vertex"), Body && Body->TriMeshGeometries.Num() == 1 && Body->TriMeshGeometries[0] &&
                                                               Body->TriMeshGeometries[0]->Particles().Size() == LOD.GetNumVertices());
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
