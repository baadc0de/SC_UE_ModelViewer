// Regression coverage for playback lifetime, sparse rig refresh, scope arbitration, and cyclic positions.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "SCAnimImporter.h"
#include "SCSkeletonBuilder.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRigPlayback, "SCBridge.Rig.PlaybackLifecycle",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRigPlayback::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    auto* Vehicle = World->SpawnActor<ASCVehicle>();
    Vehicle->ClipCatalog = NewObject<USCClipCatalog>(Vehicle);
    for (int32 I = 0; I < 2; ++I)
    {
        FSCDerivedMesh Data;
        FSCRigNode Node;
        Node.SourceName = TEXT("Bone");
        Data.RigNodes.Add(Node);
        USkeleton* Skeleton;
        auto* Mesh = FSCSkeletonBuilder::Build(Data, Vehicle, Skeleton);
        FString Error;
        TestTrue(TEXT("empty socket rig"), FSCSkeletalMeshImporter::Build(Data, Mesh, Error));
        auto* Rig = NewObject<USCRigComponent>(Vehicle);
        Vehicle->AddInstanceComponent(Rig);
        Rig->SetSkeletalMesh(Mesh);
        Rig->RegisterComponent();
        Rig->ResetPose();
        Vehicle->Rigs.Add(Rig);
        Vehicle->SkeletalMeshes.Add(Mesh);
        FSCImportedClip Clip;
        Clip.Id = I == 0 ? TEXT("A") : TEXT("B");
        Clip.LastFrame = 2;
        Clip.Fps = 1;
        FSCClipBinding Binding;
        Binding.Mesh = Mesh;
        FSCBoneTrack Track;
        Track.Bone = 1;
        Track.bPosition = true;
        Track.Fps = 1;
        for (int32 Frame = 0; Frame <= 2; ++Frame)
            Track.Keys.Add(FTransform(FVector(Frame * 10, 0, 0)));
        Binding.Tracks.Add(Track);
        Clip.Bindings.Add(Binding);
        Vehicle->ClipCatalog->Clips.Add(Clip);
    }
    TestTrue(TEXT("start forward"), Vehicle->PlayClip(TEXT("A")));
    const uint64 Initial = Vehicle->PoseEvaluations;
    Vehicle->Tick(0);
    TestEqual(TEXT("zero delta keeps clip active"), Vehicle->Playback.Num(), 1);
    TestEqual(TEXT("zero delta does not refresh rigs"), Vehicle->PoseEvaluations, Initial);
    Vehicle->Tick(1);
    TestEqual(TEXT("only bound rig refreshed"), Vehicle->PoseEvaluations, Initial + 1);
    TestTrue(TEXT("one second sample"), Vehicle->Rigs[0]->LocalPose[1].GetTranslation().Equals(FVector(10, 0, 0)));
    Vehicle->Tick(1);
    TestEqual(TEXT("finished removed from active list"), Vehicle->Playback.Num(), 0);
    const uint64 Finished = Vehicle->PoseEvaluations;
    for (int32 I = 0; I < 120; ++I)
        Vehicle->Tick(1.f / 60);
    const uint64 IdleEvaluations = Vehicle->PoseEvaluations - Finished;
    TestEqual(TEXT("120 idle ticks cost zero rig evaluations"), Vehicle->PoseEvaluations, Finished);
    TestTrue(TEXT("endpoint held"), Vehicle->Rigs[0]->LocalPose[1].GetTranslation().Equals(FVector(20, 0, 0)));
    TestFalse(TEXT("idle actor tick disabled"), Vehicle->IsActorTickEnabled());
    TestTrue(TEXT("play disjoint rig"), Vehicle->PlayClip(TEXT("B")));
    Vehicle->Tick(1);
    TestTrue(TEXT("disjoint playback preserves held endpoint"), Vehicle->Rigs[0]->LocalPose[1].GetTranslation().Equals(FVector(20, 0, 0)));
    Vehicle->ResetPose();
    auto& Overlap = Vehicle->ClipCatalog->Clips[1].Bindings[0];
    const auto OtherMesh = Overlap.Mesh;
    Overlap.Mesh = Vehicle->SkeletalMeshes[0];
    Vehicle->PlayClip(TEXT("A"));
    Vehicle->SnapClip(TEXT("B"), true);
    const uint64 BeforeHiddenAdvance = Vehicle->PoseEvaluations;
    Vehicle->Tick(1);
    TestTrue(TEXT("newer held layer wins over older active layer"),
             Vehicle->Rigs[0]->LocalPose[1].GetTranslation().Equals(FVector(20, 0, 0)));
    TestEqual(TEXT("unchanged composed pose does not move socket children"), Vehicle->PoseEvaluations, BeforeHiddenAdvance);
    Overlap.Mesh = OtherMesh;
    Vehicle->ResetPose();
    auto Fragment = [](const TCHAR* Guid, const TCHAR* Tag, float Speed, float Weight) {
        FSCClipFragment F;
        F.Fragment = TEXT("Door");
        F.OptionId = Guid;
        F.Tags.Add(TEXT("Broad"));
        F.FragmentTags.Add(Tag);
        F.Scopes.Add(TEXT("DoorScope"));
        F.Speed = Speed;
        F.OptionWeight = Weight;
        return F;
    };
    auto& A = Vehicle->ClipCatalog->Clips[0];
    A.Fragments.Add(Fragment(TEXT("open"), TEXT("Open"), 1, 1));
    A.Fragments.Add(Fragment(TEXT("close"), TEXT("Close"), -1, 1));
    Vehicle->ClipCatalog->Clips[1].Fragments.Add(Fragment(TEXT("disabled"), TEXT("Open"), 1, 0));
    TestTrue(TEXT("broad tag picks a scope option"), Vehicle->PlayFragment(TEXT("Door"), TEXT("Broad")));
    TestEqual(TEXT("broad tag does not play opposite options together"), Vehicle->Playback.Num(), 1);
    TestEqual(TEXT("zero-weight option cannot win"), Vehicle->Playback[0].Clip, 0);
    Vehicle->ResetPose();
    TestTrue(TEXT("interact opens"), Vehicle->ToggleClip(TEXT("Door")));
    TestEqual(TEXT("open direction"), Vehicle->Playback[0].Speed, 1.f);
    TestTrue(TEXT("interact closes"), Vehicle->ToggleClip(TEXT("Door")));
    TestEqual(TEXT("close direction"), Vehicle->Playback[0].Speed, -1.f);
    Vehicle->Tick(0);
    TestEqual(TEXT("zero delta keeps reverse clip active"), Vehicle->Playback.Num(), 1);
    Vehicle->Tick(3);
    TestEqual(TEXT("reverse stops at zero"), Vehicle->Playback.Num(), 0);
    TestTrue(TEXT("reverse endpoint"), Vehicle->Rigs[0]->LocalPose[1].GetTranslation().IsNearlyZero());
    Vehicle->ResetPose();
    auto& OtherOption = Vehicle->ClipCatalog->Clips[1].Fragments[0];
    OtherOption.OptionWeight = 1;
    OtherOption.Scopes = {TEXT("OtherScope")};
    Vehicle->PlayFragment(TEXT("Door"), TEXT("Open"));
    TestEqual(TEXT("disjoint scopes play together"), Vehicle->Playback.Num(), 2);
    Vehicle->ResetPose();
    OtherOption.Scopes = {TEXT("DoorScope")};
    OtherOption.OptionWeight = 1000;
    int32 WeightedWins = 0;
    for (int32 I = 0; I < 32; ++I)
    {
        Vehicle->ResetPose();
        Vehicle->PlayFragment(TEXT("Door"), TEXT("Open"));
        TestEqual(TEXT("one alternative per scope"), Vehicle->Playback.Num(), 1);
        WeightedWins += Vehicle->Playback[0].Clip == 1;
    }
    TestTrue(TEXT("relative option weights affect selection"), WeightedWins >= 28);
    Vehicle->ResetPose();
    UE_LOG(LogTemp, Display, TEXT("PLAYBACK_REGRESSION idle_ticks=120 idle_evaluations=%llu active_after_finish=%d weighted_wins=%d/32"),
           IdleEvaluations, Vehicle->Playback.Num(), WeightedWins);
    Vehicle->Destroy();
    World->DestroyWorld(false);
    return true;
}
// WP-P5: two door geometries that share bone names (the C2's hold and top lift doors, CRUS_Starlifter_Lift_Doors.cga and
// _Lift_Doors_Top.cga: the hold clip moves the cage too, 80 frames; the top clip 50) bind both clips to both rigs. A door
// rig plays the clip authored for its own geometry (the clip's owner), never the other door's at random.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRigClipOwner, "SCBridge.Rig.FragmentPrefersOwnClip",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRigClipOwner::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    auto* Vehicle = World->SpawnActor<ASCVehicle>();
    Vehicle->ClipCatalog = NewObject<USCClipCatalog>(Vehicle);
    const TCHAR* Owners[] = {TEXT("Objects/Synthetic/Lift_Doors.cga"), TEXT("Objects/Synthetic/Lift_Doors_Top.cga")};
    for (int32 I = 0; I < 2; ++I)
    {
        FSCDerivedMesh Data;
        FSCRigNode Node;
        Node.SourceName = TEXT("Door_Lift_Bottom");
        Data.RigNodes.Add(Node);
        USkeleton* Skeleton;
        auto* Mesh = FSCSkeletonBuilder::Build(Data, Vehicle, Skeleton);
        FString Error;
        TestTrue(TEXT("rig"), FSCSkeletalMeshImporter::Build(Data, Mesh, Error));
        auto* Rig = NewObject<USCRigComponent>(Vehicle);
        Vehicle->AddInstanceComponent(Rig);
        Rig->SetSkeletalMesh(Mesh);
        Rig->OwnerEntity = FString(TEXT("Data/")) + Owners[I]; // interior placements are named by their cgf_path
        Rig->RegisterComponent();
        Rig->ResetPose();
        Vehicle->Rigs.Add(Rig);
        Vehicle->SkeletalMeshes.Add(Mesh);
    }
    for (int32 I = 0; I < 2; ++I)
    {
        FSCImportedClip Clip;
        Clip.Id = I == 0 ? TEXT("hold_open") : TEXT("top_open");
        Clip.Owner = Owners[I];
        Clip.LastFrame = I == 0 ? 80 : 50;
        Clip.Fps = 30;
        for (USkeletalMesh* Mesh : Vehicle->SkeletalMeshes) // the shared bone binds the clip to both geometries
        {
            FSCClipBinding Binding;
            Binding.Mesh = Mesh;
            FSCBoneTrack Track;
            Track.Bone = 1;
            Track.bPosition = true;
            Track.Fps = 30;
            Track.Keys = {FTransform(FVector::ZeroVector), FTransform(FVector(0, -100, 0))};
            Binding.Tracks.Add(Track);
            Clip.Bindings.Add(Binding);
        }
        FSCClipFragment F;
        F.Fragment = TEXT("SCItemDoor");
        F.FragmentTags.Add(TEXT("Open"));
        F.Scopes = {TEXT("SCItemDoor"), TEXT("SyncedEntity"), TEXT("DoorUser")};
        F.Speed = 1;
        F.OptionWeight = 1;
        F.AnimationNames.Add(Clip.Id);
        F.OptionId = Clip.Id.ToString(); // two options of one fragment (the real fragments differ by their JSON)
        Clip.Fragments.Add(F);
        Vehicle->ClipCatalog->Clips.Add(Clip);
    }
    int32 Own[2] = {0, 0};
    for (int32 Try = 0; Try < 16; ++Try)
        for (int32 R = 0; R < 2; ++R)
        {
            Vehicle->ResetPose();
            TestTrue(TEXT("plays"), Vehicle->PlayFragmentAt(TEXT("SCItemDoor"), TEXT("Open"), Vehicle->Rigs[R]));
            Own[R] += Vehicle->Playback.Num() == 1 && Vehicle->Playback[0].Clip == R;
        }
    TestEqual(TEXT("the hold door plays its own clip"), Own[0], 16);
    TestEqual(TEXT("the top door plays its own clip"), Own[1], 16);
    Vehicle->Destroy();
    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRigCyclic, "SCBridge.Rig.CyclicPositionAnchor",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRigCyclic::RunTest(const FString&)
{
    FSCDerivedMesh Data;
    FSCRigNode Node;
    Node.SourceName = TEXT("Bone");
    Node.Local = FTransform(FVector(10, 20, 30));
    Data.RigNodes.Add(Node);
    const FString Text =
        FString::Printf(TEXT("{\"bones\":{\"0x%X\":{\"position\":[[1,2,3],[2,4,6],[1,2,3]]}}}"), FSCAnimImporter::BoneHash(TEXT("Bone")));
    TSharedPtr<FJsonObject> Json;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json);
    FSCImportedClip Clip;
    Clip.LastFrame = 2;
    Clip.bCyclic = true;
    FSCClipBinding Binding;
    FString Error;
    if (!TestTrue(TEXT("derive cyclic"), FSCAnimImporter::DeriveBinding(Json, Data, TEXT("test.glb"), Clip, Binding, Error))) return false;
    TestTrue(TEXT("first at bind"), Binding.Tracks[0].Keys[0].GetTranslation().Equals(FVector(10, 20, 30)));
    TestTrue(TEXT("middle bind plus delta"), Binding.Tracks[0].Keys[1].GetTranslation().Equals(FVector(210, 120, 330)));
    TestTrue(TEXT("last back at bind"), Binding.Tracks[0].Keys[2].GetTranslation().Equals(FVector(10, 20, 30)));
    Clip.bCyclic = false;
    TestTrue(TEXT("derive absolute"), FSCAnimImporter::DeriveBinding(Json, Data, TEXT("test.glb"), Clip, Binding, Error));
    TestTrue(TEXT("noncyclic stays absolute"), Binding.Tracks[0].Keys[0].GetTranslation().Equals(FVector(200, 100, 300)));
    return true;
}
#endif
