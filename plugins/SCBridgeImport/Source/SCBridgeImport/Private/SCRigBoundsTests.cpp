// SCBridge.Rig.PosedBounds: a rig's bounds follow its pose, and a leader-pose follower's cover its leader's. The rigs have
// no physics asset, so the engine's bounds are the bind-pose mesh bounds: a seated player's helmet (a follower of the body)
// kept its standing bounds about a metre above the head and was culled (USCRigComponent::CalcBounds).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCRigComponent.h"
#include "SCSkeletonBuilder.h"
#include "SCMeshes.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRigPosedBounds, "SCBridge.Rig.PosedBounds",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRigPosedBounds::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    AActor* Owner = World->SpawnActor<AActor>();
    auto MakeRig = [&](const TCHAR* Name) -> USCRigComponent* {
        FSCDerivedMesh Data;
        FSCRigNode Root;
        Root.SourceName = TEXT("Root");
        Data.RigNodes.Add(Root);
        FSCRigNode Head;
        Head.SourceName = TEXT("Head");
        Head.Parent = 0;
        Data.RigNodes.Add(Head);
        USkeleton* Skeleton = nullptr;
        USkeletalMesh* Mesh = FSCSkeletonBuilder::Build(Data, Owner, Skeleton);
        FString Error;
        if (!TestTrue(FString(Name) + TEXT(" rig builds: ") + Error, Mesh && FSCSkeletalMeshImporter::Build(Data, Mesh, Error))) return nullptr;
        auto* Rig = NewObject<USCRigComponent>(Owner, Name);
        Owner->AddInstanceComponent(Rig);
        Rig->SetSkeletalMesh(Mesh);
        Rig->RegisterComponent();
        Rig->ResetPose();
        return Rig;
    };
    USCRigComponent* Leader = MakeRig(TEXT("Body"));
    USCRigComponent* Follower = MakeRig(TEXT("Helmet"));
    if (Leader && Follower && TestTrue(TEXT("rig has a child bone"), Leader->LocalPose.Num() >= 2))
    {
        Follower->SetLeaderPoseComponent(Leader, true);
        // The last bone (the head) posed 5 m below the root: far outside the bind-pose bounds.
        Leader->LocalPose.Last().SetTranslation(FVector(0, 0, -500));
        Leader->RefreshBoneTransforms();
        const FVector HeadWorld = Leader->GetComponentTransform().TransformPosition(Leader->GetComponentSpaceTransforms().Last().GetLocation());
        TestTrue(TEXT("the leader's bounds hold its posed head"), Leader->Bounds.GetBox().IsInsideOrOn(HeadWorld));
        TestTrue(TEXT("the follower's bounds hold the leader's posed head"), Follower->Bounds.GetBox().IsInsideOrOn(HeadWorld));
        // Back in the bind pose the bounds shrink back with it (no stale growth).
        Leader->ResetPose();
        TestFalse(TEXT("bind pose: the old head position is outside again"), Leader->Bounds.GetBox().IsInsideOrOn(HeadWorld));
    }
    World->DestroyWorld(false);
    return true;
}
#endif
