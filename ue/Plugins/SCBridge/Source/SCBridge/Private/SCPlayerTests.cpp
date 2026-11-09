// WP-P2b null-RHI tests: blend-space weights over the whole parameter grid, additive composition at the last
// frame, the weapon state machine, and (with -SCPlayerPackage=<kit tar>) the imported rig, its player joints and
// weapon re-parenting between stow and hand ports.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCPlayerAnimator.h"
#include "SCPlayerBody.h"
#include "SCPlayerKit.h"
#include "SCFirstPersonCharacter.h"
#include "SCImportJob.h"
#include "SCPackageSource.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "Components/CapsuleComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SCAnimImporter.h"

namespace
{
// Visibly synthetic grid: speeds 0/2/4, angles -pi/0/pi, four quads.
FSCBlendSpace SyntheticSpace()
{
    FSCBlendSpace S;
    S.Id = TEXT("synthetic");
    S.Dimensions = {{TEXT("MoveSpeed"), 0, 4, 0, 4}, {TEXT("TravelAngle"), -UE_PI, UE_PI, -UE_PI, UE_PI}};
    for (int32 I = 0; I < 3; ++I)
        for (int32 J = 0; J < 3; ++J)
        {
            FSCBlendExample E;
            E.Clip = FName(*FString::Printf(TEXT("c%d%d"), I, J));
            E.Params = {2.f * I, UE_PI * (J - 1)};
            S.Examples.Add(E);
        }
    for (int32 I = 0; I < 2; ++I)
        for (int32 J = 0; J < 2; ++J)
            S.Faces.Add({I * 3 + J, (I + 1) * 3 + J, (I + 1) * 3 + J + 1, I * 3 + J + 1});
    return S;
}
bool CheckGrid(FAutomationTestBase& Test, const FSCBlendSpace& Space, int32 Steps, int32& Samples)
{
    if (Space.Dimensions.Num() != 2) return true;
    bool OK = true;
    TArray<TPair<int32, float>> W;
    const auto &A = Space.Dimensions[0], &B = Space.Dimensions[1];
    // The whole grid, one cell beyond the bounds on every side (clamped to the nearest face).
    for (int32 I = -1; I <= Steps + 1; ++I)
        for (int32 J = -1; J <= Steps + 1; ++J)
        {
            const float X = A.Min + (A.Max - A.Min) * I / Steps, Y = B.Min + (B.Max - B.Min) * J / Steps;
            FSCBlendSampler::Weights(Space, {X, Y}, W);
            float Sum = 0;
            bool Negative = false;
            for (const auto& P : W) { Sum += P.Value; Negative |= P.Value < 0; }
            ++Samples;
            if (W.IsEmpty() || Negative || !FMath::IsNearlyEqual(Sum, 1.f, 1e-4f))
            {
                Test.AddError(FString::Printf(TEXT("%s at (%g, %g): %d weights, sum %g, negative %d"), *Space.Id.ToString(), X, Y, W.Num(), Sum, Negative));
                OK = false;
            }
        }
    return OK;
}
TSharedPtr<FSCTarPackageSource> LoadPackage()
{
    FString Path;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SCPlayerPackage="), Path)) return nullptr;
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Path)) return nullptr;
    return MakeShared<FSCTarPackageSource>(MoveTemp(Bytes));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCPlayerBlendWeights, "SCBridge.Player.BlendWeights", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCPlayerBlendWeights::RunTest(const FString&)
{
    const FSCBlendSpace Space = SyntheticSpace();
    int32 Samples = 0;
    CheckGrid(*this, Space, 40, Samples);
    TArray<TPair<int32, float>> W;
    for (int32 E = 0; E < Space.Examples.Num(); ++E)
    {
        FSCBlendSampler::Weights(Space, Space.Examples[E].Params, W);
        const auto* Own = W.FindByPredicate([&](const TPair<int32, float>& P) { return P.Key == E; });
        TestTrue(FString::Printf(TEXT("example %d reproduces itself"), E), Own && Own->Value > 0.999f);
    }
    // Midway between two examples on an edge: half each.
    FSCBlendSampler::Weights(Space, {1.f, 0.f}, W);
    float A = 0, B = 0;
    for (const auto& P : W) { if (P.Key == 1) A = P.Value; if (P.Key == 4) B = P.Value; }
    TestTrue(TEXT("edge midpoint interpolates linearly"), FMath::IsNearlyEqual(A, 0.5f, 1e-4f) && FMath::IsNearlyEqual(B, 0.5f, 1e-4f));
    TestEqual(TEXT("locomotion input maps by dimension name"), FSCBlendSampler::LocomotionParams(Space, 3, -1), TArray<float>({3.f, -1.f}));
    TestEqual(TEXT("nominal forward speed = fastest forward example in the grid"), FSCBlendSampler::NominalForwardSpeed(Space), 4.f);
    // Real spaces from a kit, when one is given.
    if (TSharedPtr<FSCTarPackageSource> Package = LoadPackage())
    {
        TSharedPtr<FSCPlayerKit> Kit;
        TSharedPtr<FSCPlayerClipMap> Clips;
        int32 Failed = 0;
        FString Error;
        const FString Scene = FSCPlayerKit::FindRootScene(*Package);
        TArray<uint8> Bytes;
        FSCPlayerKit Parsed;
        TestTrue(TEXT("kit scene parses"), Package->ReadBytes(Scene, Bytes, Error) &&
            FSCPlayerKit::Parse(FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num())), FPaths::GetPath(Scene), Parsed, Error));
        int32 Spaces = 0;
        for (const auto& Pair : Parsed.BlendSpaces)
            if (Pair.Value.Dimensions.Num() == 2 && CheckGrid(*this, Pair.Value, 24, Samples)) ++Spaces;
        AddInfo(FString::Printf(TEXT("kit blend spaces checked=%d of %d"), Spaces, Parsed.BlendSpaces.Num()));
        TestTrue(TEXT("kit has 2-D blend spaces"), Spaces > 0);
    }
    AddInfo(FString::Printf(TEXT("samples=%d"), Samples));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCPlayerAdditive, "SCBridge.Player.AdditiveLastFrame", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCPlayerAdditive::RunTest(const FString&)
{
    // Synthetic sidecar: bone "b" rotates 30 degrees about X and moves 1 m at frame 0, identity at the last frame.
    const double S = FMath::Sin(FMath::DegreesToRadians(15.0)), C = FMath::Cos(FMath::DegreesToRadians(15.0));
    const FString Json = FString::Printf(TEXT("{\"name\":\"synthetic_add\",\"fps\":30,\"frame_count\":10,\"duration\":0.3333333,\"additive\":true,")
        TEXT("\"bones\":{\"0x1\":{\"source_node_name\":\"b\",\"has_rotation\":true,\"has_position\":true,")
        TEXT("\"rotation\":[[%f,%f,0,0],[1,0,0,0]],\"rotation_time\":[0,10],\"position\":[[1,0,0],[0,0,0]],\"position_time\":[0,10]}}}"), C, S);
    FSCPlayerClip Clip;
    FString Error;
    TestTrue(TEXT("parse"), FSCPlayerClip::Parse(Json, [](const FString& N) { return N == TEXT("b") ? 1 : INDEX_NONE; }, Clip, Error));
    TestTrue(TEXT("additive flag read from the asset"), Clip.bAdditive);
    TArray<FTransform> Base = {FTransform::Identity, FTransform(FRotator(10, 20, 30), FVector(5, 6, 7))};
    TArray<FTransform> Pose = Base;
    Clip.ApplyAdditive(Clip.Duration, 1, Pose);
    TestTrue(TEXT("last frame leaves the base pose unchanged"),
             Pose[1].GetRotation().Equals(Base[1].GetRotation(), 1e-5) && Pose[1].GetTranslation().Equals(Base[1].GetTranslation(), 1e-4));
    Pose = Base;
    Clip.ApplyAdditive(0, 1, Pose);
    TestTrue(TEXT("first frame changes the base"), !Pose[1].GetRotation().Equals(Base[1].GetRotation(), 1e-3));
    TestTrue(TEXT("translation adds"), FVector::Dist(Pose[1].GetTranslation(), Base[1].GetTranslation()) > 50);
    TestTrue(TEXT("delta composes in the parent's frame (delta * base)"),
             Pose[1].GetRotation().Equals((FQuat(FSCAnimImporter::SidecarRotation(FQuat(S, 0, 0, C))) * Base[1].GetRotation()).GetNormalized(), 1e-4));
    Pose = Base;
    Clip.ApplyAdditive(0, 0, Pose);
    TestTrue(TEXT("zero weight is a no-op"), Pose[1].Equals(Base[1], 1e-6));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCPlayerWeaponStates, "SCBridge.Player.WeaponStateMachine", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCPlayerWeaponStates::RunTest(const FString&)
{
    using E = FSCWeaponStateMachine::EEvent;
    FSCWeaponStateMachine M;
    TestEqual(TEXT("holster while unarmed does nothing"), M.Request(INDEX_NONE), ESCWeaponAction::None);
    TestEqual(TEXT("unarmed selects"), M.Request(0), ESCWeaponAction::Select);
    M.BeginSelect(0, 0.5f);
    TestEqual(TEXT("mid-select: other weapon ignored"), M.Request(1), ESCWeaponAction::None);
    TestEqual(TEXT("mid-select: holster ignored"), M.Request(INDEX_NONE), ESCWeaponAction::None);
    TestTrue(TEXT("still selecting"), M.Tick(0.25f) == E::None && M.GetState() == ESCWeaponState::Selecting);
    TestTrue(TEXT("select ends armed"), M.Tick(0.3f) == E::Selected && M.GetState() == ESCWeaponState::Armed && M.GetActive() == 0);
    TestEqual(TEXT("own key holsters"), M.Request(0), ESCWeaponAction::Deselect);
    TestEqual(TEXT("other key swaps through a deselect"), M.Request(1), ESCWeaponAction::Deselect);
    M.BeginDeselect(0.4f, 1);
    TestEqual(TEXT("mid-deselect: input ignored"), M.Request(0), ESCWeaponAction::None);
    TestTrue(TEXT("deselect ends unarmed with the swap queued"),
             M.Tick(0.5f) == E::Holstered && M.GetState() == ESCWeaponState::Unarmed && M.GetActive() == INDEX_NONE && M.GetQueued() == 1);
    M.BeginSelect(1, 0);
    TestTrue(TEXT("zero-length select arms on the next tick"), M.Tick(0) == E::Selected && M.GetActive() == 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCPlayerKitRig, "SCBridge.Player.KitRigAndWeapons", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCPlayerKitRig::RunTest(const FString&)
{
    TSharedPtr<FSCTarPackageSource> Package = LoadPackage();
    if (!TestTrue(TEXT("-SCPlayerPackage=<kit tar> readable"), Package.IsValid() && Package->IsValid())) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    World->InitializeActorsForPlay(FURL());
    World->BeginPlay();
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    const FString Scene = FSCPlayerKit::FindRootScene(*Package);
    FSCImportJob Job(*Package, USCPlayerBodyComponent::KitImportOptions(World, Scene), Package);
    while (!Job.Tick(true)) {}
    ASCVehicle* Kit = Job.GetResult().Vehicle.Get();
    if (!TestNotNull(TEXT("kit imported"), Kit)) return false;
    USCRigComponent* Leader = USCPlayerBodyComponent::FindLeader(Kit);
    if (!TestNotNull(TEXT("root rig"), Leader)) return false;
    TSharedPtr<FSCPlayerKit> Data;
    TSharedPtr<FSCPlayerClipMap> Clips;
    int32 Failed = 0;
    FString Error;
    TestTrue(TEXT("player block and clips load"), SCLoadPlayerKit(*Package, Scene, USCPlayerBodyComponent::BoneMap(Leader), Data, Clips, Failed, Error));
    if (!Data) return false;
    TestEqual(TEXT("no unreadable clip"), Failed, 0);
    TestTrue(TEXT("clips routed to the body skeleton"), Clips->Num() > 0);
    // Every joint the player block names exists on the leader rig.
    TArray<FString> Joints = {Data->CameraBone, Data->RootMotionBone};
    for (const auto& P : Data->HandPorts) Joints.Add(P.AttachBone);
    for (const auto& W : Data->Weapons) Joints.Add(W.StowPort.AttachBone);
    const FReferenceSkeleton& Ref = Leader->GetSkeletalMeshAsset()->GetRefSkeleton();
    for (const FString& J : Joints)
        TestTrue(FString::Printf(TEXT("rig has player joint %s"), *J), J.IsEmpty() || Ref.FindBoneIndex(FName(*J)) != INDEX_NONE);
    // Character + body, weapon re-parenting.
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ASCFirstPersonCharacter* C = World->SpawnActor<ASCFirstPersonCharacter>(ASCFirstPersonCharacter::StaticClass(), FTransform(FVector(0, 0, 500)), Params);
    if (!TestNotNull(TEXT("character"), C)) return false;
    TestTrue(TEXT("body attaches"), C->Body->Attach(Kit, Data, Clips, Error));
    TestTrue(TEXT("weapon slots"), C->Body->NumSlots() > 0);
    TestTrue(TEXT("head parts hidden in first person"), C->Body->NumHeadParts() > 0);
    TestTrue(TEXT("walk/run from blend-space examples"), C->GetProfile().WalkSpeed > 0 && C->GetProfile().RunSpeed > C->GetProfile().WalkSpeed);
    const FVector Floor = C->GetActorLocation() - FVector(0, 0, C->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
    const double EyeHeight = Leader->GetSocketLocation(FName(*Data->CameraBone)).Z - Floor.Z;
    AddInfo(FString::Printf(TEXT("camera joint height above the capsule floor: %.1f cm"), EyeHeight));
    TestTrue(TEXT("camera joint at standing eye height (feet on the capsule floor)"), EyeHeight > 140 && EyeHeight < 200);
    for (int32 Slot = 0; Slot < FMath::Min(2, C->Body->NumSlots()); ++Slot)
    {
        const FSCPlayerWeapon* W = C->Body->SlotWeapon(Slot);
        const FName Stow(*W->StowPort.AttachBone), Hand(*Data->HandPortFor(*W)->AttachBone);
        TestEqual(TEXT("stowed on the stow joint"), C->Body->WeaponParentBone(Slot), Stow);
        TestTrue(TEXT("select accepted"), C->Body->RequestSlot(Slot));
        TestEqual(TEXT("in hand from the start of the select"), C->Body->WeaponParentBone(Slot), Hand);
        TestFalse(TEXT("holster ignored mid-select"), C->Body->RequestSlot(INDEX_NONE));
        for (int32 Frame = 0; Frame < 300 && C->Body->GetWeaponState() != ESCWeaponState::Armed; ++Frame) C->Body->Step(1.f / 60);
        TestEqual(TEXT("armed"), C->Body->GetWeaponState(), ESCWeaponState::Armed);
        for (int32 Frame = 0; Frame < 30; ++Frame) C->Body->Step(1.f / 60);
        AddInfo(FString::Printf(TEXT("%s armed: %s"), *W->Entity, *C->Body->Describe()));
        TestTrue(TEXT("holster accepted"), C->Body->RequestSlot(Slot));
        TestEqual(TEXT("in hand during the deselect"), C->Body->WeaponParentBone(Slot), Hand);
        for (int32 Frame = 0; Frame < 300 && C->Body->GetWeaponState() != ESCWeaponState::Unarmed; ++Frame) C->Body->Step(1.f / 60);
        TestEqual(TEXT("unarmed"), C->Body->GetWeaponState(), ESCWeaponState::Unarmed);
        TestEqual(TEXT("back on the stow joint after the deselect"), C->Body->WeaponParentBone(Slot), Stow);
    }
    bool Finite = true;
    for (const FTransform& T : Leader->LocalPose) Finite &= !T.ContainsNaN();
    TestTrue(TEXT("pose finite"), Finite);
    C->Destroy();
    return true;
}
#endif
