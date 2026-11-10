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
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/CollisionProfile.h"
#include "Camera/CameraComponent.h"
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
    // Degenerate faces (review #11): collinear A(0,0) B(1,0) C(2,0) sampled at C returns C; coincident A=B with C.
    {
        FSCBlendSpace Line;
        Line.Id = TEXT("collinear");
        Line.Dimensions = {{TEXT("MoveSpeed"), 0, 2, 0, 2}, {TEXT("TravelAngle"), -1, 1, -1, 1}};
        for (float X : {0.f, 1.f, 2.f}) { FSCBlendExample E; E.Params = {X, 0.f}; Line.Examples.Add(E); }
        Line.Faces.Add({0, 1, 2});
        FSCBlendSampler::Weights(Line, {2.f, 0.f}, W);
        TestTrue(TEXT("collinear face sampled at C reproduces C"), W.Num() == 1 && W[0].Key == 2 && FMath::IsNearlyEqual(W[0].Value, 1.f));
        FSCBlendSampler::Weights(Line, {1.5f, 0.f}, W);
        float WB = 0, WC = 0;
        for (const auto& P : W) { if (P.Key == 1) WB = P.Value; if (P.Key == 2) WC = P.Value; }
        TestTrue(TEXT("collinear face between B and C interpolates B/C"), FMath::IsNearlyEqual(WB, 0.5f, 1e-4f) && FMath::IsNearlyEqual(WC, 0.5f, 1e-4f));
        Line.Examples[1].Params = {0.f, 0.f}; // A == B
        FSCBlendSampler::Weights(Line, {2.f, 0.f}, W);
        TestTrue(TEXT("coincident A=B: C still reachable"), W.Num() == 1 && W[0].Key == 2);
    }
    // Combined space (review #8): two components covering the same point; the choosing selector decides.
    {
        FSCBlendSpace Comb;
        Comb.Id = TEXT("comb");
        Comb.Dimensions = {{TEXT("TurnAngle"), -3, 3, -3, 3}};
        Comb.Components = {TEXT("narrow"), TEXT("wide")};
        auto Add = [&](float X, int32 C) { FSCBlendExample E; E.Params = {X}; E.Component = C; Comb.Examples.Add(E); };
        Add(-1, 0); Add(1, 0); Add(-3, 1); Add(3, 1);
        Comb.Faces = {{0, 1}, {2, 3}};
        FSCBlendSelector Sel; Sel.Name = TEXT("TurnAngle"); Sel.Dimension = 0; Sel.bChooseBlendSpace = true;
        Comb.Selectors.Add(Sel);
        FSCBlendSampler::Weights(Comb, {0.5f}, W);
        TestTrue(TEXT("selector: 0.5 inside the first component -> only its examples"), W.Num() > 0 && W.ContainsByPredicate([](const TPair<int32, float>& P) { return P.Key == 1; })
            && !W.ContainsByPredicate([](const TPair<int32, float>& P) { return P.Key >= 2; }));
        FSCBlendSampler::Weights(Comb, {2.f}, W);
        TestTrue(TEXT("selector: 2 only in the second component"), W.Num() > 0 && !W.ContainsByPredicate([](const TPair<int32, float>& P) { return P.Key < 2; }));
        TestEqual(TEXT("select component"), Comb.SelectComponent({2.f}), 1);
    }
    // TravelAngle seam (review #13): sticky on the side used last frame, unchanged away from it.
    TestTrue(TEXT("seam: -pi side kept"), FSCBlendSampler::StickySeamAngle(UE_PI - 0.01f, -UE_PI + 0.01f) < -UE_PI);
    TestTrue(TEXT("seam: +pi side kept"), FSCBlendSampler::StickySeamAngle(-UE_PI + 0.01f, UE_PI - 0.02f) > UE_PI);
    TestEqual(TEXT("seam: same side unchanged"), FSCBlendSampler::StickySeamAngle(UE_PI - 0.01f, UE_PI - 0.2f), UE_PI - 0.01f);
    TestEqual(TEXT("seam: far from pi unchanged"), FSCBlendSampler::StickySeamAngle(1.f, -UE_PI + 0.01f), 1.f);
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
        int32 Spaces = 0, Seams = 0, SeamBreaks = 0, Combined = 0;
        for (const auto& Pair : Parsed.BlendSpaces)
        {
            if (Pair.Value.Dimensions.Num() == 2 && CheckGrid(*this, Pair.Value, 24, Samples)) ++Spaces;
            Combined += !Pair.Value.Components.IsEmpty();
            TestTrue(Pair.Key.ToString() + TEXT(": selectors evaluable"), Pair.Value.Unsupported.IsEmpty());
            // TravelAngle seam (review #13): the clips and weights at +pi-eps and -pi+eps (nominal forward speed).
            const int32 AngleDim = Pair.Value.Dimension(TEXT("TravelAngle"));
            if (AngleDim == INDEX_NONE || Pair.Value.Dimensions.Num() != 2 || !FSCBlendSampler::CoversAngle(Pair.Value, UE_PI - 1e-3f) || !FSCBlendSampler::CoversAngle(Pair.Value, -UE_PI + 1e-3f)) continue;
            ++Seams;
            const float Speed = FSCBlendSampler::NominalForwardSpeed(Pair.Value);
            TMap<FName, float> Side[2];
            for (int32 K = 0; K < 2; ++K)
            {
                FSCBlendSampler::Weights(Pair.Value, FSCBlendSampler::LocomotionParams(Pair.Value, Speed, K ? -UE_PI + 1e-3f : UE_PI - 1e-3f), W);
                for (const auto& P : W) Side[K].FindOrAdd(Pair.Value.Examples[P.Key].Clip) += P.Value;
            }
            float Diff = 0;
            for (const auto& P : Side[0]) Diff += FMath::Abs(P.Value - Side[1].FindRef(P.Key));
            for (const auto& P : Side[1]) if (!Side[0].Contains(P.Key)) Diff += P.Value;
            if (Diff > 0.01f) { ++SeamBreaks; AddInfo(FString::Printf(TEXT("seam %s: clip weights differ by %.3f across +-pi"), *Pair.Key.ToString(), Diff)); }
        }
        AddInfo(FString::Printf(TEXT("kit seams checked=%d discontinuous=%d combined spaces=%d"), Seams, SeamBreaks, Combined));
        TestTrue(TEXT("kit combined spaces parsed with components"), Combined > 0);
        for (const FString& D : Parsed.Diagnostics) AddInfo(TEXT("kit diagnostic: ") + D);
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
    // Non-identity terminal key (review #12): 20 degrees about the sidecar X axis at the last frame; expected
    // pose computed independently as axis-angle in UE space applied before the base rotation.
    {
        const double S2 = FMath::Sin(FMath::DegreesToRadians(10.0)), C2 = FMath::Cos(FMath::DegreesToRadians(10.0));
        const FString Json2 = FString::Printf(TEXT("{\"name\":\"terminal_add\",\"fps\":30,\"frame_count\":10,\"duration\":0.3333333,\"additive\":true,")
            TEXT("\"bones\":{\"0x1\":{\"source_node_name\":\"b\",\"has_rotation\":true,\"has_position\":false,")
            TEXT("\"rotation\":[[1,0,0,0],[%f,%f,0,0]],\"rotation_time\":[0,10]}}}"), C2, S2);
        FSCPlayerClip Terminal;
        TestTrue(TEXT("parse terminal"), FSCPlayerClip::Parse(Json2, [](const FString& N) { return N == TEXT("b") ? 1 : INDEX_NONE; }, Terminal, Error));
        Pose = Base;
        Terminal.ApplyAdditive(Terminal.Duration, 1, Pose);
        const FQuat Delta = FSCAnimImporter::SidecarRotation(FQuat(S2, 0, 0, C2));
        const FVector Axis = Delta.GetRotationAxis();
        TestTrue(TEXT("terminal delta is 20 degrees"), FMath::IsNearlyEqual(FMath::RadiansToDegrees(Delta.GetAngle()), 20.0, 1e-3));
        const FQuat Expected = (FQuat(Axis, FMath::DegreesToRadians(20.0)) * Base[1].GetRotation()).GetNormalized();
        TestTrue(TEXT("terminal key applied at the last frame"), Pose[1].GetRotation().Equals(Expected, 1e-4));
        TestTrue(TEXT("half weight = half angle"), [&] { TArray<FTransform> P = Base; Terminal.ApplyAdditive(Terminal.Duration, 0.5f, P);
            return P[1].GetRotation().Equals((FQuat(Axis, FMath::DegreesToRadians(10.0)) * Base[1].GetRotation()).GetNormalized(), 1e-4); }());
    }
    // Layer clocks (review #7): a 2 s additive over a 0.5 s base keeps its own time, looped or held.
    {
        FSCMotionLayer L;
        L.Speed = 1; L.bLoop = true;
        TestTrue(TEXT("additive runs its own 2 s, not the base's phase"), FMath::IsNearlyEqual(FSCPlayerAnimator::LayerSeconds(L, 2.f, 1.25f), 1.25f));
        TestTrue(TEXT("looped past the end"), FMath::IsNearlyEqual(FSCPlayerAnimator::LayerSeconds(L, 2.f, 2.5f), 0.5f));
        L.Speed = 0.5f; L.StartTime = 0.2f;
        TestTrue(TEXT("speed and start time"), FMath::IsNearlyEqual(FSCPlayerAnimator::LayerSeconds(L, 2.f, 1.f), 0.7f));
        L.bLoop = false;
        TestTrue(TEXT("not looped: held at the end"), FMath::IsNearlyEqual(FSCPlayerAnimator::LayerSeconds(L, 2.f, 10.f), 2.f));
        L.Speed = 0;
        TestTrue(TEXT("zero speed holds the start time"), FMath::IsNearlyEqual(FSCPlayerAnimator::LayerSeconds(L, 2.f, 10.f), 0.2f));
    }
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
    // Queued swap across a long frame (review #10): the holster ends 0.1 s into a 0.5 s frame; the select starts
    // with that 0.1 s on its clock.
    M.Request(0);
    M.BeginDeselect(0.4f, 0);
    TestTrue(TEXT("holster ends inside the frame"), M.Tick(0.5f) == E::Holstered && FMath::IsNearlyEqual(M.GetOvershoot(), 0.1f, 1e-5f));
    M.BeginSelect(0, 0.3f, M.GetOvershoot());
    TestTrue(TEXT("select clock starts with the overshoot"), FMath::IsNearlyEqual(M.GetElapsed(), 0.1f, 1e-5f));
    TestTrue(TEXT("select ends after its remaining 0.2 s"), M.Tick(0.19f) == E::None && M.Tick(0.02f) == E::Selected);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCPlayerFirstPersonScale, "SCBridge.Player.FirstPersonScale", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCPlayerFirstPersonScale::RunTest(const FString&)
{
    using B = USCPlayerBodyComponent;
    TestEqual(TEXT("no hit: world scale"), B::FirstPersonScaleFor({70.f, 30.f}, {-1.f, -1.f}, 1.5f, 0.04f), 1.f);
    TestEqual(TEXT("hit beyond every probe: world scale"), B::FirstPersonScaleFor({70.f}, {80.f}, 1.5f, 0.04f), 1.f);
    // Muzzle 70 cm out, wall at 21.5 cm on its ray: 70 s <= 20.
    TestTrue(TEXT("muzzle in the wall: scaled to the wall minus margin"), FMath::IsNearlyEqual(B::FirstPersonScaleFor({70.f, 30.f}, {21.5f, -1.f}, 1.5f, 0.04f), 20.f / 70.f, 1e-5f));
    // The most constraining probe wins.
    TestTrue(TEXT("tightest probe"), FMath::IsNearlyEqual(B::FirstPersonScaleFor({70.f, 30.f}, {41.5f, 7.5f}, 1.5f, 0.04f), 6.f / 30.f, 1e-5f));
    TestEqual(TEXT("clamped to the minimum (eye at the wall)"), B::FirstPersonScaleFor({70.f}, {0.5f}, 1.5f, 0.04f), 0.04f);
    TestEqual(TEXT("zero-distance probe ignored"), B::FirstPersonScaleFor({0.f}, {0.f}, 1.5f, 0.04f), 1.f);
    return true;
}

namespace
{
/** Real attachment of a weapon anchor: the hand joint on the leader, or the stow frame (body joint or owner helper). */
bool AttachedTo(const USceneComponent* Anchor, const USCRigComponent* Leader, FName Joint)
{
    return Anchor && Anchor->GetAttachParent() &&
        ((Anchor->GetAttachParent() == Leader && Anchor->GetAttachSocketName() == Joint) ||
         (Anchor->GetAttachParent() != Leader && Anchor->GetAttachSocketName().IsNone()));
}
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
        const USceneComponent* Anchor = Kit->InstanceAnchors.FindRef(W->ChildId);
        TestEqual(TEXT("stowed on the stow joint"), C->Body->WeaponParentBone(Slot), Stow);
        TestTrue(TEXT("anchor really attached to the stow frame"), AttachedTo(Anchor, Leader, Stow));
        TestTrue(TEXT("select accepted"), C->Body->RequestSlot(Slot));
        TestEqual(TEXT("in hand from the start of the select"), C->Body->WeaponParentBone(Slot), Hand);
        TestTrue(TEXT("anchor really attached to the hand joint"), Anchor && Anchor->GetAttachParent() == Leader && Anchor->GetAttachSocketName() == Hand);
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
        TestTrue(TEXT("anchor really back on the stow frame"), AttachedTo(Anchor, Leader, Stow) && Anchor->GetAttachSocketName() != Hand);
    }
    // Queued swap with long frames (review #10): animator one-shot time and state machine clock agree.
    if (C->Body->NumSlots() > 1)
    {
        C->Body->RequestSlot(0);
        for (int32 Frame = 0; Frame < 300 && C->Body->GetWeaponState() != ESCWeaponState::Armed; ++Frame) C->Body->Step(1.f / 60);
        C->Body->RequestSlot(1); // deselect 0, then select 1
        bool bChecked = false;
        for (int32 Frame = 0; Frame < 40 && C->Body->GetWeaponState() != ESCWeaponState::Armed; ++Frame)
        {
            C->Body->Step(0.25f);
            if (C->Body->GetWeaponState() == ESCWeaponState::Selecting && C->Body->GetAnimator().OneShotProgress() >= 0)
            {
                const float Animated = C->Body->GetAnimator().OneShotProgress() * C->Body->GetAnimator().OneShotDuration();
                TestTrue(FString::Printf(TEXT("swap: one-shot %.3f s = state clock %.3f s"), Animated, C->Body->GetWeaponElapsed()),
                         FMath::IsNearlyEqual(Animated, C->Body->GetWeaponElapsed(), 1e-3f));
                bChecked = true;
            }
        }
        TestTrue(TEXT("swap reached the select"), bChecked && C->Body->GetActiveSlot() == 1);
        C->Body->RequestSlot(1);
        for (int32 Frame = 0; Frame < 300 && C->Body->GetWeaponState() != ESCWeaponState::Unarmed; ++Frame) C->Body->Step(1.f / 60);
    }
    // First person: the camera sits at the camera joint (or swept back from it), head parts hidden, body visible.
    C->Body->SetThirdPerson(false);
    C->Body->Step(1.f / 60);
    TestTrue(TEXT("camera at the camera joint"), FVector::Dist(C->Camera->GetComponentLocation(), C->Body->HeadLocation()) < 5);
    TestEqual(TEXT("no wall in reach: world-space body"), C->Body->GetFirstPersonScale(), 1.f);
    TestFalse(TEXT("no wall in reach: no first-person primitives"), C->Body->AreFirstPersonPrimitivesActive());
    // Weapon drawn, a wall 10 cm ahead of the capsule: the body is scaled toward the eye, then released in third person.
    if (C->Body->NumSlots() > 0)
    {
        C->Body->RequestSlot(0);
        for (int32 Frame = 0; Frame < 300 && C->Body->GetWeaponState() != ESCWeaponState::Armed; ++Frame) C->Body->Step(1.f / 60);
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        AStaticMeshActor* Wall = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(),
            FTransform(FRotator::ZeroRotator, C->GetActorLocation() + C->GetActorForwardVector() * (C->GetCapsuleComponent()->GetScaledCapsuleRadius() + 10 + 50), FVector(1, 4, 4)), Params);
        if (TestNotNull(TEXT("wall"), Wall) && Cube)
        {
            Wall->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
            Wall->GetStaticMeshComponent()->SetStaticMesh(Cube);
            Wall->GetStaticMeshComponent()->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
            C->Body->Step(1.f / 60);
            AddInfo(FString::Printf(TEXT("wall 10 cm ahead: first-person scale %.3f"), C->Body->GetFirstPersonScale()));
            TestTrue(TEXT("wall in reach: scaled toward the eye"), C->Body->GetFirstPersonScale() < 1.f);
            TestTrue(TEXT("wall in reach: first-person primitives"), C->Body->AreFirstPersonPrimitivesActive());
            TestTrue(TEXT("camera scale enabled"), C->Camera->bEnableFirstPersonScale && FMath::IsNearlyEqual(C->Camera->FirstPersonScale, C->Body->GetFirstPersonScale()));
            C->Body->SetThirdPerson(true);
            TestEqual(TEXT("third person: world-space body"), C->Body->GetFirstPersonScale(), 1.f);
            TestFalse(TEXT("third person: no first-person primitives"), C->Body->AreFirstPersonPrimitivesActive());
            C->Body->SetThirdPerson(false);
            Wall->Destroy();
        }
    }
    bool Finite = true;
    for (const FTransform& T : Leader->LocalPose) Finite &= !T.ContainsNaN();
    TestTrue(TEXT("pose finite"), Finite);
    C->Destroy();
    return true;
}
#endif
