// Synthetic animation basis, routing, skin buffer, and projector regressions.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCAnimImporter.h"
#include "SCSkeletonBuilder.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRigMath, "SCBridge.Rig.MathAndRouting",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRigMath::RunTest(const FString&)
{
    TestEqual(TEXT("standard CRC32 lower UTF8"), FSCAnimImporter::BoneHash(TEXT("123456789")), uint32(0xcbf43926));
    TestEqual(TEXT("source case is significant (exporter CRC32)"), FSCAnimImporter::BoneHash(TEXT("Canopy")), uint32(0x2C83BDF));
    const FQuat Q(FVector(1, 2, 3).GetSafeNormal(), 1.2);
    const FVector V(2, 3, 4);
    // Independent native Y-up fixture: the exporter swaps native (x,y,z) to (x,-z,y).
    const FVector Native(2, 3, 4);
    TestTrue(TEXT("sidecar undoes exporter swap before GLB conversion"),
             FSCAnimImporter::SidecarPosition(FVector(2, -4, 3)).Equals(FSCMeshImporter::Position(Native), 1.e-8));
    const FQuat NativeQ(FVector(1, 2, 3).GetSafeNormal(), 0.7);
    const FQuat SidecarQ(NativeQ.X, -NativeQ.Z, NativeQ.Y, NativeQ.W);
    const FQuat ExpectedQ(FSCMeshImporter::Transform(FTransform(NativeQ).ToMatrixWithScale()));
    TestTrue(TEXT("sidecar rotation agrees with independent native GLB matrix"),
             FSCAnimImporter::SidecarRotation(SidecarQ).AngularDistance(ExpectedQ) < 1.e-7);
    TestTrue(TEXT("rotation and position bases agree"), FSCAnimImporter::SidecarRotation(Q)
                                                            .RotateVector(FSCAnimImporter::SidecarPosition(V))
                                                            .Equals(FSCAnimImporter::SidecarPosition(Q.RotateVector(V)), 1.e-5));
    TestTrue(TEXT("hemisphere avoids zero quaternion"), FSCAnimImporter::LinearRotation(Q, Q * -1., 0.5).Equals(Q, 1.e-6));
    TestTrue(TEXT("duplicate hash routes rear"),
             FSCAnimImporter::ProvenanceScore(TEXT("Foot"), TEXT("gear/rear/left.skin"), TEXT("Foot"), TEXT("gear/rear/left.chr")) >
                 FSCAnimImporter::ProvenanceScore(TEXT("Foot"), TEXT("gear/rear/left.skin"), TEXT("Foot"), TEXT("gear/front/right.chr")));
    FSCDerivedMesh Data;
    FSCRigNode N;
    N.SourceName = TEXT("Root");
    N.Local = FTransform(FQuat(FVector::UpVector, 0.3));
    Data.RigNodes.Add(N);
    USkeleton* Skeleton;
    auto* Mesh = FSCSkeletonBuilder::Build(Data, GetTransientPackage(), Skeleton);
    TestEqual(TEXT("reference hierarchy"), Mesh->GetRefSkeleton().GetNum(), 2);
    TestNotNull(TEXT("named socket"), Skeleton->FindSocket(TEXT("Root")));
    FString Error;
    TestTrue(TEXT("runtime empty rig render data"), FSCSkeletalMeshImporter::Build(Data, Mesh, Error));
    const FQuat Start(FVector::ForwardVector, 0.4), End(FVector::ForwardVector, 0.9);
    auto QJson = [](FQuat X) { return FString::Printf(TEXT("[%0.12f,%0.12f,%0.12f,%0.12f]"), X.W, X.X, X.Y, X.Z); };
    const FString JsonText = FString::Printf(
        TEXT("{\"start_rotation\":%s,\"bones\":{\"0x%X\":{\"source_node_name\":\"Root\",\"rotation\":[%s,%s],\"rotation_time\":[0,2]}}}"),
        *QJson(Start), FSCAnimImporter::BoneHash(TEXT("Root")), *QJson(Start), *QJson(End * -1.));
    TSharedPtr<FJsonObject> Json;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), Json);
    FSCImportedClip Clip;
    Clip.Fps = 30;
    Clip.LastFrame = 2;
    TestTrue(*Error, FSCAnimImporter::Bind(Json, Data, TEXT("test.skin"), Mesh, GetTransientPackage(), Clip, Error));
    if (TestEqual(TEXT("one binding"), Clip.Bindings.Num(), 1))
    {
        const auto& Track = Clip.Bindings[0].Tracks[0];
        TestTrue(TEXT("start anchors to bind"), Track.Keys[0].GetRotation().Equals(N.Local.GetRotation(), 1.e-5));
        TestTrue(TEXT("bind * inverse(start) * sample"),
                 Track.Keys[2].GetRotation().Equals(N.Local.GetRotation() * FSCAnimImporter::SidecarRotation(Start).Inverse() *
                                                        FSCAnimImporter::SidecarRotation(End),
                                                    1.e-5));
        TestEqual(TEXT("uniform frames include endpoint"), Track.Keys.Num(), 3);
        TestTrue(TEXT("nonboomerang"), !Clip.bBoomerang);
        TestNotNull(TEXT("sequence object"), Clip.Bindings[0].Sequence.Get());
    }
    FSCImportedClip Still;
    Still.LastFrame = 0;
    TestTrue(TEXT("single-frame clip duplicates safely for the UE model"),
             FSCAnimImporter::Bind(Json, Data, TEXT("test.skin"), Mesh, GetTransientPackage(), Still, Error));
    return true;
}
#endif
#if WITH_DEV_AUTOMATION_TESTS
#include "StaticMeshAttributes.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SCLightImporter.h"
#include "Engine/World.h"
#include "Components/SpotLightComponent.h"
#include "Components/RectLightComponent.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Engine/Texture2D.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCSkinBuffers, "SCBridge.Rig.SkinBuffers",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCSkinBuffers::RunTest(const FString&)
{
    FSCDerivedMesh Data;
    Data.MaterialNames.Add(TEXT("Synthetic"));
    FSCRigNode Root;
    Root.SourceName = TEXT("Root");
    Data.RigNodes.Add(Root);
    FSCRigNode Child;
    Child.SourceName = TEXT("Tip");
    Child.Parent = 0;
    Child.Local = FTransform(FVector(0, 0, 100));
    Data.RigNodes.Add(Child);
    Data.GltfToRig.Add(0, 0);
    Data.GltfToRig.Add(1, 1);
    FStaticMeshAttributes A(Data.Description);
    A.Register();
    A.GetVertexInstanceUVs().SetNumChannels(2);
    const auto Group = Data.Description.CreatePolygonGroup();
    TArray<FVertexInstanceID> Corners;
    for (int32 I = 0; I < 3; ++I)
    {
        const auto V = Data.Description.CreateVertex();
        A.GetVertexPositions()[V] = FVector3f(I == 1 ? 100 : 0, I == 2 ? 100 : 0, 0);
        FSCVertexWeights W;
        W.Joints[0] = 0;
        W.Joints[1] = 1;
        W.Weights[0] = 0.25;
        W.Weights[1] = 0.75;
        Data.SkinWeights.Add(V, W);
        const auto VI = Data.Description.CreateVertexInstance(V);
        Corners.Add(VI);
        A.GetVertexInstanceNormals()[VI] = FVector3f(0, 0, 1);
        A.GetVertexInstanceTangents()[VI] = FVector3f(1, 0, 0);
        A.GetVertexInstanceBinormalSigns()[VI] = 1;
        A.GetVertexInstanceColors()[VI] = FVector4f(1, 1, 1, 1);
    }
    Data.Description.CreateTriangle(Group, Corners);
    USkeleton* Skeleton;
    auto* Mesh = FSCSkeletonBuilder::Build(Data, GetTransientPackage(), Skeleton);
    FString Error;
    if (TestTrue(*Error, FSCSkeletalMeshImporter::Build(Data, Mesh, Error)))
    {
        const auto* Render = Mesh->GetResourceForRendering();
        const auto& LOD = Render->LODRenderData[0];
        TestEqual(TEXT("resident LOD"), int32(Render->NumInlinedLODs), 1);
        TestEqual(TEXT("soft triangle"), int32(LOD.RenderSections[0].NumTriangles), 1);
        TestEqual(TEXT("three vertices"), int32(LOD.GetNumVertices()), 3);
        const auto W = LOD.SkinWeightVertexBuffer.GetVertexSkinWeights(0);
        const auto& Map = LOD.RenderSections[0].BoneMap;
        TestTrue(TEXT("joint remap preserves synthetic root offset (through the section bone map)"),
                 Map.IsValidIndex(W.InfluenceBones[1]) && int32(Map[W.InfluenceBones[1]]) == 2);
        TestTrue(TEXT("section bone map lists only weighting joints"), Map.Num() < Mesh->GetRefSkeleton().GetNum());
        TestEqual(TEXT("quantized weights sum to 65535"), int32(W.InfluenceWeights[0]) + int32(W.InfluenceWeights[1]), 65535);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRigLights, "SCBridge.Rig.Lights",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRigLights::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    auto* Vehicle = World->SpawnActor<ASCVehicle>();
    FSCSceneInstance Instance;
    for (const FString Kind : {TEXT("point"), TEXT("spot"), TEXT("area"), TEXT("ambient_proxy")})
        Instance.LightJson.Add(
            FString::Printf(TEXT("{\"name\":\"Synthetic\",\"semantic_light_kind\":\"%s\",\"transform_basis\":\"cryengine_z_up\","
                                 "\"position\":[1,2,3],\"direction_sc\":[0,1,0],\"color\":[0.2,0.4,0.6],\"intensity_candela_proxy\":25,"
                                 "\"radius_m\":4,\"inner_angle\":20,\"outer_angle\":30}"),
                            *Kind));
    FSCFolderPackageSource Source(TEXT("/tmp"));
    TArray<FString> Warnings;
    FSCLightImporter::Build(
        Source, Instance, Vehicle, Vehicle->GetRootComponent(), [](const FString&) -> USceneComponent* { return nullptr; }, Warnings);
    TestEqual(TEXT("ambient proxy excluded"), Vehicle->Lights.Num(), 3);
    TestEqual(TEXT("valid lights no warnings"), Warnings.Num(), 0);
    for (ULightComponent* Light : Vehicle->Lights)
    {
        TestEqual(TEXT("candela proxy 1:1"), Light->Intensity, 25.f);
        TestTrue(TEXT("Cry Z-up entity basis"), Light->GetRelativeLocation().Equals(FVector(200, 100, 300)));
        TestTrue(TEXT("spot/rect axis +X follows direction_sc"), Light->GetForwardVector().Equals(FVector(1, 0, 0), 1.e-5));
        const auto* Local = Cast<ULocalLightComponent>(Light);
        TestNotNull(TEXT("local light"), Local);
        if (Local) TestEqual(TEXT("radius metres -> cm"), Local->AttenuationRadius, 400.f);
        if (const auto* Spot = Cast<USpotLightComponent>(Light))
        {
            TestEqual(TEXT("outer is half-angle"), Spot->OuterConeAngle, 30.f);
            TestEqual(TEXT("inner is half-angle"), Spot->InnerConeAngle, 20.f);
        }
    }
    const FString Directory = FPaths::ProjectSavedDir() / TEXT("Import/RigTests");
    auto EXR = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")).CreateImageWrapper(EImageFormat::EXR);
    const float Pixels[] = {2, 1, 0.5, 1, 2, 1, 0.5, 1, 2, 1, 0.5, 1, 2, 1, 0.5, 1};
    if (!TestTrue(TEXT("synthetic HDR encoder"), EXR->SetRaw(Pixels, sizeof(Pixels), 2, 2, ERGBFormat::RGBAF, 32)))
    {
        Vehicle->Destroy();
        World->DestroyWorld(false);
        return false;
    }
    const auto& Bytes = EXR->GetCompressed();
    FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Bytes.GetData(), Bytes.Num()), *(Directory / TEXT("gobo.exr")));
    FSCSceneInstance HDR;
    HDR.LightJson.Add(TEXT(
        R"({"name":"HDR","semantic_light_kind":"spot","transform_basis":"cryengine_z_up","intensity_candela_proxy":1,"radius_m":2,"outer_angle":30,"projector_texture":"gobo.exr"})"));
    FSCFolderPackageSource HDRSource(Directory);
    for (int32 I = 0; I < 2; ++I)
        FSCLightImporter::Build(
            HDRSource, HDR, Vehicle, Vehicle->GetRootComponent(), [](const FString&) -> USceneComponent* { return nullptr; }, Warnings);
    TestEqual(TEXT("HDR gobos decode and bind"), Warnings.Num(), 0);
    TestEqual(TEXT("one texture shared by repeated gobos"), Vehicle->GoboTextures.Num(), 1);
    TestTrue(TEXT("one material shared by repeated gobos"), Vehicle->Lights.Last()->LightFunctionMaterial == Vehicle->Lights[Vehicle->Lights.Num() - 2]->LightFunctionMaterial);
    if (UTexture2D* Texture = Vehicle->GoboTextures.FindRef(TEXT("gobo.exr")))
    {
        TestFalse(TEXT("gobo is linear"), Texture->SRGB);
        TestTrue(TEXT("HDR format preserved"), Texture->GetPixelFormat() == PF_FloatRGBA || Texture->GetPixelFormat() == PF_A32B32G32R32F);
        TestTrue(TEXT("texture owned by import"), Texture->GetOuter() == Vehicle);
    }
    Vehicle->Destroy();
    World->DestroyWorld(false);
    return true;
}
#endif
