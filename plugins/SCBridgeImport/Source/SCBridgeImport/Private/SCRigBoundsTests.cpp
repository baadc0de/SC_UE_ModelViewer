// SCBridge.Rig.PosedBounds: a rig's bounds follow its pose, and a leader-pose follower's follow its leader's bones. The rigs
// have no physics asset, so the engine's bounds are the bind-pose mesh bounds: a seated player's helmet (a follower of the
// body) kept its standing bounds about a metre above the head and was culled (USCRigComponent::CalcBounds). Away from the
// world origin, so world and local bounds differ (the renderer keeps both).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCRigComponent.h"
#include "SCSkeletonBuilder.h"
#include "SCMeshes.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCRigPosedBounds, "SCBridge.Rig.PosedBounds",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCRigPosedBounds::RunTest(const FString&)
{
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    AActor* Owner = World->SpawnActor<AActor>();
    auto* Root = NewObject<USceneComponent>(Owner, TEXT("Root"));
    Owner->SetRootComponent(Root);
    Root->RegisterComponent();
    Owner->SetActorLocation(FVector(100000, 50000, 3000)); // far from the origin
    auto MakeRig = [&](const TCHAR* Name) -> USCRigComponent* {
        FSCDerivedMesh Data;
        FSCRigNode Base;
        Base.SourceName = TEXT("Root");
        Data.RigNodes.Add(Base);
        FSCRigNode Head;
        Head.SourceName = TEXT("Head");
        Head.Parent = 0;
        Data.RigNodes.Add(Head);
        USkeleton* Skeleton = nullptr;
        USkeletalMesh* Mesh = FSCSkeletonBuilder::Build(Data, Owner, Skeleton);
        FString Error;
        if (!TestTrue(FString(Name) + TEXT(" rig builds: ") + Error, Mesh && FSCSkeletalMeshImporter::Build(Data, Mesh, Error))) return nullptr;
        auto* Rig = NewObject<USCRigComponent>(Owner, Name);
        Rig->SetupAttachment(Root);
        Owner->AddInstanceComponent(Rig);
        Rig->SetSkeletalMesh(Mesh);
        Rig->RegisterComponent();
        Rig->ResetPose();
        // Geometry of 20 cm around the last bone (the head) in the bind pose.
        const FVector BindHead = Rig->GetComponentSpaceTransforms().Last().GetLocation();
        Mesh->SetImportedBounds(FBoxSphereBounds(FBox(BindHead - FVector(10), BindHead + FVector(10))));
        return Rig;
    };
    USCRigComponent* Leader = MakeRig(TEXT("Body"));
    USCRigComponent* Follower = MakeRig(TEXT("Helmet"));
    if (Leader && Follower && TestTrue(TEXT("rig has a child bone"), Leader->LocalPose.Num() >= 2))
    {
        Follower->SetLeaderPoseComponent(Leader, true);
        // The head posed 5 m below its bind place: far outside the bind-pose bounds.
        Leader->LocalPose.Last().SetTranslation(Leader->LocalPose.Last().GetTranslation() + FVector(0, 0, -500));
        Leader->RefreshBoneTransforms();
        const FVector HeadLocal = Leader->GetComponentSpaceTransforms().Last().GetLocation();
        const FVector HeadWorld = Leader->GetComponentTransform().TransformPosition(HeadLocal);
        TestTrue(TEXT("the leader's bounds hold its posed head"), Leader->Bounds.GetBox().IsInsideOrOn(HeadWorld));
        TestTrue(TEXT("the follower's bounds hold the posed head"), Follower->Bounds.GetBox().IsInsideOrOn(HeadWorld));
        TestTrue(TEXT("the follower's local bounds hold it too (component space)"),
                 Follower->CalcBounds(FTransform::Identity).GetBox().IsInsideOrOn(HeadLocal));
        // Back in the bind pose the bounds shrink back with it (no stale growth).
        Leader->ResetPose();
        TestFalse(TEXT("bind pose: the old head position is outside the leader's bounds again"), Leader->Bounds.GetBox().IsInsideOrOn(HeadWorld));
        TestFalse(TEXT("bind pose: and outside the follower's"), Follower->Bounds.GetBox().IsInsideOrOn(HeadWorld));
    }
    World->DestroyWorld(false);
    return true;
}
#endif
