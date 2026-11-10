// A clip merged by name across rigs keeps each contributing animation's own rate, length and start pose.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCAnimImporter.h"
#include "SCSkeletonBuilder.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "UObject/Package.h"

namespace
{
// Synthetic rig: three sliding bones and one rotating bone with a non-identity bind.
FSCDerivedMesh TimingRig()
{
    FSCDerivedMesh Data;
    for (const TCHAR* Name : {TEXT("Long"), TEXT("Short"), TEXT("Fast"), TEXT("Anchored")})
    {
        FSCRigNode Node;
        Node.SourceName = Name;
        Data.RigNodes.Add(Node);
    }
    Data.RigNodes[3].Local = FTransform(FQuat(FVector::UpVector, 0.3));
    return Data;
}
FString QuatJson(FQuat X)
{
    return FString::Printf(TEXT("[%0.12f,%0.12f,%0.12f,%0.12f]"), X.W, X.X, X.Y, X.Z);
}
// Four animations of one clip name: 6 and 4 frames at 30 fps, 8 frames at 60 fps (with a spike between two clip
// frames), 2 frames with an authored start.
TSharedPtr<FJsonObject> TimingClip(const FQuat& Start, const FQuat& End)
{
    auto Hash = [](const TCHAR* Name) { return FSCAnimImporter::BoneHash(Name); };
    const FString Text = FString::Printf(
        TEXT("{\"fps\":30,\"frame_count\":6,\"start_rotation\":[1,0,0,0],\"sources\":[")
        TEXT("{\"skeleton_path\":\"a.chr\",\"fps\":30,\"frame_count\":6},{\"skeleton_path\":\"b.chr\",\"fps\":30,\"frame_count\":4},")
        TEXT("{\"skeleton_path\":\"c.chr\",\"fps\":60,\"frame_count\":8},")
        TEXT("{\"skeleton_path\":\"d.chr\",\"fps\":30,\"frame_count\":2,\"start_rotation\":%s}],\"bones\":{")
        TEXT("\"0x%X\":{\"source\":0,\"position\":[[0,0,0],[0,0,6]],\"position_time\":[0,6]},")
        TEXT("\"0x%X\":{\"source\":1,\"position\":[[0,0,0],[0,0,4]],\"position_time\":[0,4]},")
        TEXT("\"0x%X\":{\"source\":2,\"position\":[[0,0,0],[0,0,100],[0,0,0],[0,0,8]],\"position_time\":[0,1,2,8]},")
        TEXT("\"0x%X\":{\"source\":3,\"rotation\":[%s,%s],\"rotation_time\":[0,2]}}}"),
        *QuatJson(Start), Hash(TEXT("Long")), Hash(TEXT("Short")), Hash(TEXT("Fast")), Hash(TEXT("Anchored")), *QuatJson(Start),
        *QuatJson(End));
    TSharedPtr<FJsonObject> Json;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json);
    return Json;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRigSourceTiming, "SCBridge.Rig.ClipSourceTiming",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRigSourceTiming::RunTest(const FString&)
{
    const FSCDerivedMesh Data = TimingRig();
    const FQuat Start(FVector::ForwardVector, 0.4), End(FVector::ForwardVector, 0.9);
    const auto Json = TimingClip(Start, End);
    FSCImportedClip Clip;
    Clip.Fps = 30;
    Clip.LastFrame = 6;
    FSCClipBinding Binding;
    FString Error;
    if (!TestTrue(TEXT("derive merged clip"), FSCAnimImporter::DeriveBinding(Json, Data, TEXT("test.skin"), Clip, Binding, Error))) return false;
    if (!TestEqual(TEXT("four tracks"), Binding.Tracks.Num(), 4)) return false;
    const auto& Long = Binding.Tracks[0];
    const auto& Short = Binding.Tracks[1];
    const auto& Fast = Binding.Tracks[2];
    const auto& Anchored = Binding.Tracks[3];
    TestEqual(TEXT("longest animation spans the clip"), Long.Keys.Num(), 7);
    TestEqual(TEXT("shorter animation keeps its own length"), Short.Keys.Num(), 5);
    TestEqual(TEXT("faster animation keeps its own frames"), Fast.Keys.Num(), 9);
    TestEqual(TEXT("fast track rate"), Fast.Fps, 60.f);
    TestTrue(TEXT("faster animation keeps the key between two clip frames"),
             Fast.Keys[1].GetTranslation().Equals(FSCAnimImporter::SidecarPosition(FVector(0, 0, 100)), 1.e-6));
    TestEqual(TEXT("short track rate"), Short.Fps, 30.f);
    TestTrue(TEXT("short track ends on its own last key"),
             Short.Keys.Last().GetTranslation().Equals(FSCAnimImporter::SidecarPosition(FVector(0, 0, 4)), 1.e-6));
    TestTrue(TEXT("fast track ends on its own last key"),
             Fast.Keys.Last().GetTranslation().Equals(FSCAnimImporter::SidecarPosition(FVector(0, 0, 8)), 1.e-6));
    TestEqual(TEXT("anchored animation length"), Anchored.Keys.Num(), 3);
    TestTrue(TEXT("each animation anchors on its own start pose"),
             Anchored.Keys[0].GetRotation().Equals(Data.RigNodes[3].Local.GetRotation(), 1.e-5));
    TestTrue(TEXT("bind * inverse(own start) * sample"),
             Anchored.Keys[2].GetRotation().Equals(Data.RigNodes[3].Local.GetRotation() * FSCAnimImporter::SidecarRotation(Start).Inverse() *
                                                       FSCAnimImporter::SidecarRotation(End),
                                                   1.e-5));
    // Forward: every animation starts with the clip. Backward: every animation starts at its own end.
    auto Frame = [&](const FSCBoneTrack& Track, double At, bool bFromEnd) { return FSCAnimImporter::TrackFrame(Clip, Track, At, bFromEnd); };
    TestEqual(TEXT("forward start"), Frame(Short, 0, false), 0.);
    TestEqual(TEXT("forward holds own end"), Frame(Short, 5, false), 4.);
    TestEqual(TEXT("backward starts at own end"), Frame(Short, 6, true), 4.);
    TestEqual(TEXT("backward moves at once"), Frame(Short, 5, true), 3.);
    TestEqual(TEXT("backward reaches own start"), Frame(Short, 2, true), 0.);
    TestEqual(TEXT("backward holds own start"), Frame(Short, 0, true), 0.);
    TestEqual(TEXT("forward at own rate"), Frame(Fast, 2, false), 4.);
    TestEqual(TEXT("backward at own rate starts at own end"), Frame(Fast, 6, true), 8.);
    TestEqual(TEXT("backward at own rate"), Frame(Fast, 5, true), 6.);
    TestEqual(TEXT("longest track is the clip"), Frame(Long, 3.5, true), 3.5);
    // A channel must name a listed animation with a usable rate and length.
    auto Fails = [&](const TCHAR* Sources, const TCHAR* Channel) {
        const FString Text = FString::Printf(TEXT("{\"sources\":%s,\"bones\":{\"0x%X\":{%s\"position\":[[0,0,0],[0,0,1]]}}}"), Sources,
                                             FSCAnimImporter::BoneHash(TEXT("Long")), Channel);
        TSharedPtr<FJsonObject> Bad;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Bad);
        FSCImportedClip Other;
        Other.LastFrame = 1;
        FSCClipBinding Unused;
        FString Why;
        return !FSCAnimImporter::DeriveBinding(Bad, Data, TEXT("test.skin"), Other, Unused, Why) && !Why.IsEmpty();
    };
    TestTrue(TEXT("unknown source index"), Fails(TEXT("[{\"fps\":30,\"frame_count\":1}]"), TEXT("\"source\":1,")));
    TestTrue(TEXT("channel without a source"), Fails(TEXT("[{\"fps\":30,\"frame_count\":1}]"), TEXT("")));
    TestTrue(TEXT("source without a rate"), Fails(TEXT("[{\"frame_count\":1}]"), TEXT("\"source\":0,")));
    TestTrue(TEXT("source with a zero rate"), Fails(TEXT("[{\"fps\":0,\"frame_count\":1}]"), TEXT("\"source\":0,")));
    TestTrue(TEXT("source without a length"), Fails(TEXT("[{\"fps\":30}]"), TEXT("\"source\":0,")));
    const TCHAR* Two = TEXT("[{\"fps\":30,\"frame_count\":1},{\"fps\":30,\"frame_count\":1}]");
    TestFalse(TEXT("a listed source is accepted"), Fails(Two, TEXT("\"source\":1,")));
    TestTrue(TEXT("fractional source index"), Fails(Two, TEXT("\"source\":0.6,")));
    TestTrue(TEXT("negative source index"), Fails(Two, TEXT("\"source\":-0.2,")));
    TestTrue(TEXT("source index as text"), Fails(Two, TEXT("\"source\":\"1\",")));
    TestTrue(TEXT("source list that is not an array"), Fails(TEXT("{}"), TEXT("\"source\":0,")));
    TestTrue(TEXT("source list that is not an array, channel without a source"), Fails(TEXT("7"), TEXT("")));
    // A rate is a whole number of frames a second (contract), so none can vanish in the track's float or the
    // sequence's frame rate; an animation holds every key of its channels, and the clip every source.
    TestTrue(TEXT("source rate too small to store"), Fails(TEXT("[{\"fps\":1e-50,\"frame_count\":1}]"), TEXT("\"source\":0,")));
    TestTrue(TEXT("fractional source rate"), Fails(TEXT("[{\"fps\":29.5,\"frame_count\":1}]"), TEXT("\"source\":0,")));
    TestTrue(TEXT("source shorter than its channel"), Fails(TEXT("[{\"fps\":30,\"frame_count\":0}]"), TEXT("\"source\":0,")));
    TestTrue(TEXT("source shorter than its channel's times"),
             Fails(TEXT("[{\"fps\":30,\"frame_count\":1}]"), TEXT("\"source\":0,\"position_time\":[0,1.5],")));
    TestTrue(TEXT("clip shorter than its source"),
             Fails(TEXT("[{\"fps\":30,\"frame_count\":3}]"), TEXT("\"source\":0,\"position_time\":[0,3],")));
    TestTrue(TEXT("clip shorter than its faster source"),
             Fails(TEXT("[{\"fps\":60,\"frame_count\":3}]"), TEXT("\"source\":0,\"position_time\":[0,3],")));
    TestFalse(TEXT("a faster source that fits the clip"),
              Fails(TEXT("[{\"fps\":60,\"frame_count\":2}]"), TEXT("\"source\":0,\"position_time\":[0,2],")));
    {
        // Without a source list the clip's own length bounds the keys.
        const FString Text = FString::Printf(TEXT("{\"bones\":{\"0x%X\":{\"position\":[[0,0,0],[0,0,1],[0,0,2]]}}}"),
                                             FSCAnimImporter::BoneHash(TEXT("Long")));
        TSharedPtr<FJsonObject> Plain;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Plain);
        FSCImportedClip Other;
        Other.LastFrame = 1;
        FSCClipBinding Unused;
        FString Why;
        TestTrue(TEXT("clip shorter than its channel"),
                 !FSCAnimImporter::DeriveBinding(Plain, Data, TEXT("test.skin"), Other, Unused, Why) && !Why.IsEmpty());
        Other.LastFrame = 2;
        Why.Reset();
        TestTrue(TEXT("clip as long as its channel"), FSCAnimImporter::DeriveBinding(Plain, Data, TEXT("test.skin"), Other, Unused, Why));
    }
#if WITH_EDITOR
    // Editor sequences keep source timing: one per animation rate and length in the binding, keys verbatim. One
    // sequence at the clip rate would drop the faster animation's in-between keys and could not be played backwards
    // from each animation's own end.
    FSCDerivedMesh Buildable = Data;
    USkeleton* Skeleton;
    auto* Mesh = FSCSkeletonBuilder::Build(Buildable, GetTransientPackage(), Skeleton);
    TestTrue(TEXT("rig render data"), FSCSkeletalMeshImporter::Build(Buildable, Mesh, Error));
    FSCImportedClip Bound;
    Bound.Fps = 30;
    Bound.LastFrame = 6;
    if (TestTrue(TEXT("bind merged clip"), FSCAnimImporter::Bind(Json, Buildable, TEXT("test.skin"), Mesh, GetTransientPackage(), Bound, Error)) &&
        TestEqual(TEXT("one binding"), Bound.Bindings.Num(), 1))
    {
        const FSCClipBinding& Rig = Bound.Bindings[0];
        TestEqual(TEXT("one sequence per source timing"), Rig.Sequences.Num(), 4);
        for (const FSCBoneTrack& Track : Rig.Tracks)
        {
            const FName Bone = Mesh->GetRefSkeleton().GetBoneName(Track.Bone);
            int32 Holders = 0;
            for (const UAnimSequence* Sequence : Rig.Sequences)
            {
                const IAnimationDataModel* Model = Sequence->GetDataModel();
                if (!Model->IsValidBoneTrackName(Bone)) continue;
                ++Holders;
                TestEqual(Bone.ToString() + TEXT(": sequence at the animation's rate"), Model->GetFrameRate().AsDecimal(), double(Track.Fps));
                TestEqual(Bone.ToString() + TEXT(": sequence of the animation's length"), Model->GetNumberOfKeys(), Track.Keys.Num());
                TArray<FTransform> Keys;
                Model->GetBoneTrackTransforms(Bone, Keys);
                if (TestEqual(Bone.ToString() + TEXT(": sequence keeps the animation's frames"), Keys.Num(), Track.Keys.Num()))
                    for (int32 K = 0; K < Keys.Num(); ++K)
                        TestTrue(FString::Printf(TEXT("%s: key %d verbatim"), *Bone.ToString(), K), Keys[K].Equals(Track.Keys[K], 1.e-3));
            }
            TestEqual(Bone.ToString() + TEXT(": in exactly one sequence"), Holders, 1);
        }
    }
#endif
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRigSourcePlayback, "SCBridge.Rig.ClipSourcePlayback",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRigSourcePlayback::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    auto* Vehicle = World->SpawnActor<ASCVehicle>();
    Vehicle->ClipCatalog = NewObject<USCClipCatalog>(Vehicle);
    FSCDerivedMesh Data = TimingRig();
    USkeleton* Skeleton;
    auto* Mesh = FSCSkeletonBuilder::Build(Data, Vehicle, Skeleton);
    FString Error;
    TestTrue(TEXT("rig render data"), FSCSkeletalMeshImporter::Build(Data, Mesh, Error));
    auto* Rig = NewObject<USCRigComponent>(Vehicle);
    Vehicle->AddInstanceComponent(Rig);
    Rig->SetSkeletalMesh(Mesh);
    Rig->RegisterComponent();
    Rig->ResetPose();
    Vehicle->Rigs.Add(Rig);
    Vehicle->SkeletalMeshes.Add(Mesh);
    // One clip second per clip frame. Bones 1..3 run 6 s, 4 s and (at two frames a second) 4 s.
    FSCImportedClip Clip;
    Clip.Id = TEXT("Gear");
    Clip.Fps = 1;
    Clip.LastFrame = 6;
    FSCClipBinding Binding;
    Binding.Mesh = Mesh;
    auto Track = [&](int32 Bone, int32 Last, float Fps) {
        FSCBoneTrack T;
        T.Bone = Bone;
        T.bPosition = true;
        T.Fps = Fps;
        for (int32 Frame = 0; Frame <= Last; ++Frame)
            T.Keys.Add(FTransform(FVector(Frame * 10, 0, 0)));
        Binding.Tracks.Add(T);
    };
    Track(1, 6, 1);
    Track(2, 4, 1);
    Track(3, 8, 2);
    Clip.Bindings.Add(Binding);
    for (float Speed : {1.f, -1.f})
    {
        FSCClipFragment F;
        F.Fragment = TEXT("Gear");
        F.OptionId = Speed > 0 ? TEXT("deploy") : TEXT("retract");
        F.FragmentTags.Add(Speed > 0 ? TEXT("Open") : TEXT("Close"));
        F.Scopes.Add(TEXT("GearScope"));
        F.Speed = Speed;
        Clip.Fragments.Add(F);
    }
    Vehicle->ClipCatalog->Clips.Add(Clip);
    auto X = [&](int32 Bone) { return Rig->LocalPose[Bone].GetTranslation().X; };
    TestTrue(TEXT("deploy"), Vehicle->ToggleClip(TEXT("Gear")));
    Vehicle->Tick(1);
    TestEqual(TEXT("forward: long after 1 s"), X(1), 10.);
    TestEqual(TEXT("forward: short starts with the clip"), X(2), 10.);
    TestEqual(TEXT("forward: fast runs at its own rate"), X(3), 20.);
    Vehicle->Tick(5);
    TestEqual(TEXT("deployed"), Vehicle->Playback.Num(), 0);
    TestEqual(TEXT("forward: long end"), X(1), 60.);
    TestEqual(TEXT("forward: short holds its own end"), X(2), 40.);
    TestEqual(TEXT("forward: fast holds its own end"), X(3), 80.);
    TestTrue(TEXT("retract"), Vehicle->ToggleClip(TEXT("Gear")));
    Vehicle->Tick(1);
    TestEqual(TEXT("backward: long after 1 s"), X(1), 50.);
    TestEqual(TEXT("backward: short moves at once"), X(2), 30.);
    TestEqual(TEXT("backward: fast moves at once"), X(3), 60.);
    Vehicle->Tick(3);
    TestEqual(TEXT("backward: long after 4 s"), X(1), 20.);
    TestEqual(TEXT("backward: short is done after its own length"), X(2), 0.);
    TestEqual(TEXT("backward: fast is done after its own length"), X(3), 0.);
    Vehicle->Tick(2);
    TestEqual(TEXT("retracted"), Vehicle->Playback.Num(), 0);
    TestEqual(TEXT("backward: all at start"), X(1) + X(2) + X(3), 0.);
    Vehicle->Destroy();
    World->DestroyWorld(false);
    return true;
}
#endif
