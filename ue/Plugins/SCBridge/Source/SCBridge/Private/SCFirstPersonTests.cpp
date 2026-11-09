// Headless physics acceptance on the real import: skin queries, pose changes, capsule movement and safety.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "SCImporter.h"
#include "SCVehicle.h"
#include "SCRigComponent.h"
#include "SCFirstPersonCharacter.h"
#include "SCFirstPersonCollisionComparison.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "PhysicsEngine/BodySetup.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Engine/SkeletalMesh.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/SpotLightComponent.h"

namespace
{
bool TraceSkinnedSurface(USCRigComponent* Rig)
{
    const auto& LOD = Rig->GetSkeletalMeshAsset()->GetResourceForRendering()->LODRenderData[0];
    TArray<uint32> Indices;
    LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCP1Skin), false);
    for (int32 I = 0; I + 2 < Indices.Num(); I += 3)
    {
        FVector V[3];
        for (int32 K = 0; K < 3; ++K)
            V[K] = Rig->GetComponentTransform().TransformPosition(FVector(USkinnedMeshComponent::GetSkinnedVertexPosition(
                Rig, Indices[I+K], LOD, *Rig->GetSkinWeightBuffer(0))));
        const FVector Normal = FVector::CrossProduct(V[1]-V[0],V[2]-V[0]).GetSafeNormal();
        if (Normal.IsNearlyZero()) continue;
        const FVector Centre = (V[0]+V[1]+V[2])/3;
        FHitResult Hit;
        if (Rig->LineTraceComponent(Hit, Centre+Normal*10, Centre-Normal*10, Params) &&
            FVector::Dist(Hit.ImpactPoint,Centre)<1) return true;
    }
    return false;
}
void StepCharacter(ASCFirstPersonCharacter* Character, int32 Steps)
{
    for (int32 I=0; I<Steps; ++I)
    {
        Character->Tick(1.f/60);
        Character->GetCharacterMovement()->TickComponent(1.f/60, LEVELTICK_All, nullptr);
    }
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFirstPersonCollision, "SCBridge.FirstPerson.SkeletalCollision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFirstPersonCollision::RunTest(const FString&)
{
    FString Package;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SCPackage="), Package)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    World->InitializeActorsForPlay(FURL());
    World->BeginPlay();
    FSCFolderPackageSource Source(Package);
    FSCImportOptions Options;
    Options.World = World;
    FSCImportResult Result;
    TestTrue(TEXT("import"), FSCImporter::Import(Source, Options, Result));
    ASCVehicle* V = Result.Vehicle.Get();
    if (!TestNotNull(TEXT("vehicle"), V)) { World->DestroyWorld(false); return false; }
    TestTrue(TEXT("collision ready"), V->bCollisionReady);
    int32 Soft = 0, SurfaceHits = 0, AttachedBodies = 0;
    for (USCRigComponent* Rig : V->Rigs)
    {
        const auto* Data = Rig->GetSkeletalMeshAsset()->GetResourceForRendering();
        if (!Data || Data->LODRenderData.IsEmpty() || !Data->LODRenderData[0].GetNumVertices()) continue;
        ++Soft;
        TestTrue(TEXT("soft geometry has per-poly collision"), Rig->bEnablePerPolyCollision);
        TestTrue(TEXT("soft geometry blocks capsule"), Rig->IsCollisionEnabled() && Rig->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block);
        TestTrue(TEXT("body cooked"), Rig->GetBodySetup() && Rig->GetBodySetup()->bCreatedPhysicsMeshes);
        const bool Hit = TraceSkinnedSurface(Rig);
        TestTrue(TEXT("simple query hits skinned surface"), Hit);
        SurfaceHits += Hit;
        // An arbitrary synthetic translation verifies that collision is refit, not left in bind pose.
        if (Rig->LocalPose.Num() > 1)
        {
            Rig->LocalPose[0].AddToTranslation(FVector(0,0,137));
            Rig->RefreshBoneTransforms();
            TestTrue(TEXT("query follows changed skin pose"), TraceSkinnedSurface(Rig));
            Rig->ResetPose();
        }
    }
    TInlineComponentArray<UStaticMeshComponent*> Static(V);
    for (const auto* C : Static)
        if (Cast<USCRigComponent>(C->GetAttachParent()) && C->IsCollisionEnabled()) ++AttachedBodies;
    TestTrue(TEXT("animated geometry has query bodies"), Soft + AttachedBodies > 0);
    TestTrue(TEXT("legacy-compatible hull spawn"), V->SpawnPoints.ContainsByPredicate([](const FSCSpawnPoint& P) { return P.Kind == TEXT("Hull"); }));
    const int32 ExpectedSeats = V->Manifest.Children.FilterByPredicate([](const FSCSceneInstance& I) { return I.ItemType == TEXT("Seat"); }).Num();
    TestEqual(TEXT("DataCore seat spawn count"), V->SpawnPoints.FilterByPredicate([](const FSCSpawnPoint& P) { return P.Kind == TEXT("Seat"); }).Num(), ExpectedSeats);
    int32 Toggles = 0;
    for (int32 I=0; I<V->Doors.Num(); ++I)
    {
        auto& Door = V->Doors[I];
        TestEqual(TEXT("nearest door at driven joint"), V->NearestDoor(Door.Rig->GetSocketLocation(Door.Joint),1), I);
        TMap<USCRigComponent*,TArray<FTransform>> OtherPoses;
        for (USCRigComponent* Rig : V->Rigs) if (Rig != Door.Rig) OtherPoses.Add(Rig,Rig->LocalPose);
        TestTrue(TEXT("fragment open"), V->ToggleDoor(I));
        V->Tick(60);
        TestTrue(TEXT("open state"), Door.bOpen);
        for (const auto& Other : OtherPoses)
        {
            bool Same = Other.Value.Num() == Other.Key->LocalPose.Num();
            for (int32 Bone=0; Same && Bone<Other.Value.Num(); ++Bone)
                Same &= Other.Value[Bone].Equals(Other.Key->LocalPose[Bone]);
            TestTrue(TEXT("door interaction leaves other rig instances unchanged"),Same);
        }
        TestTrue(TEXT("deployed door exact collision (convex comparison)"), SCCompareDoorCollision(V,Door.Rig,Door.Joint));
        const auto* Render = Door.Rig->GetSkeletalMeshAsset()->GetResourceForRendering();
        if (Render && !Render->LODRenderData.IsEmpty() && Render->LODRenderData[0].GetNumVertices())
            TestTrue(TEXT("deployed door skin hit"), TraceSkinnedSurface(Door.Rig));
        TestTrue(TEXT("fragment close"), V->ToggleDoor(I));
        V->Tick(60);
        TestFalse(TEXT("closed state"), Door.bOpen);
        ++Toggles;
    }
    // Leave entrances deployed for the traversal. No asset/file-name matching.
    for (int32 I=0; I<V->Doors.Num(); ++I) { V->ToggleDoor(I); V->Tick(60); }
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Character = World->SpawnActor<ASCFirstPersonCharacter>(ASCFirstPersonCharacter::StaticClass(), FTransform::Identity, Params);
    TestTrue(TEXT("safe semantic spawn"), Character->Explore(V));
    if (!V->Doors.IsEmpty())
    {
        const FTransform Saved = Character->GetActorTransform();
        // Exercise E's camera/reach selection, not just the actor's lower-level API.
        const FVector Joint = V->Doors[0].Rig->GetSocketLocation(V->Doors[0].Joint);
        Character->SetActorLocation(Joint-Character->Camera->GetRelativeLocation());
        const int32 Nearest = V->NearestDoor(Character->Camera->GetComponentLocation());
        TestTrue(TEXT("door in interaction reach"), V->Doors.IsValidIndex(Nearest));
        if (V->Doors.IsValidIndex(Nearest))
        {
            const bool WasOpen = V->Doors[Nearest].bOpen;
            Character->HandleKey(EKeys::E,true);
            TestTrue(TEXT("E toggles nearest fragment"), V->Doors[Nearest].bOpen != WasOpen);
            Character->HandleKey(EKeys::E,true);
            V->Tick(60);
        }
        Character->SetActorTransform(Saved,false,nullptr,ETeleportType::TeleportPhysics);
    }
    auto* Movement = Character->GetCharacterMovement();
    StepCharacter(Character,120);
    const FVector Start = Character->GetActorLocation();
    TestTrue(TEXT("spawn settles on walkable floor"), Movement->IsMovingOnGround());
    // Pick a local heading with floor under the complete capsule route, then perform actual movement sweeps.
    bool Walked = false;
    int32 FloorFrames = 0;
    for (int32 Direction=0; Direction<8 && !Walked; ++Direction)
    {
        const FVector Heading = FRotator(0,Direction*45,0).Vector();
        bool HasFloor = true;
        for (int32 N=1; N<=6; ++N)
        {
            FHitResult Hit;
            const FVector P = Start + Heading * N * 25;
            FCollisionQueryParams Q(SCENE_QUERY_STAT(SCP1Walk),false,Character);
            HasFloor &= World->LineTraceSingleByChannel(Hit,P,P-FVector(0,0,160),ECC_Pawn,Q) && Hit.ImpactNormal.Z >= Movement->GetWalkableFloorZ();
        }
        if (!HasFloor) continue;
        Character->SetActorRotation(Heading.Rotation());
        Character->SetMoveInput(FVector(1,0,0));
        for (int32 Frame=0; Frame<24; ++Frame)
        {
            StepCharacter(Character,1);
            FloorFrames += Movement->IsMovingOnGround();
        }
        Character->SetMoveInput(FVector::ZeroVector);
        Walked = FVector::Dist2D(Start,Character->GetActorLocation()) > 20;
        if (!Walked) Character->SetActorLocation(Start,false,nullptr,ETeleportType::TeleportPhysics);
    }
    TestTrue(TEXT("scripted walk makes progress"), Walked);
    TestTrue(TEXT("walk retains floor contact"), FloorFrames >= 20);
    TestEqual(TEXT("walk did not fall through and recover"), Character->RecoveryCount, 0);
    Character->HandleKey(EKeys::LeftControl,true);
    StepCharacter(Character,2);
    TestEqual(TEXT("crouched capsule"), Character->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(),44.f);
    Character->HandleKey(EKeys::LeftControl,false);
    StepCharacter(Character,2);
    Character->HandleKey(EKeys::SpaceBar,true);
    StepCharacter(Character,1);
    TestTrue(TEXT("jump has upward velocity"), Movement->Velocity.Z > 0);
    Character->HandleKey(EKeys::SpaceBar,false);
    StepCharacter(Character,120);
    Character->SetMoveInput(FVector(1,0,0));
    Character->Tick(0); // establish last position
    Character->Tick(2.1f); // deterministically model input blocked by geometry
    TestTrue(TEXT("blocked input offers noclip"), Character->bStuck);
    Character->ClearInput();
    Character->HandleKey(EKeys::F,true);
    TestTrue(TEXT("flashlight toggles"), Character->Flashlight->IsVisible());
    Character->ToggleNoclip();
    TestTrue(TEXT("noclip disables capsule"), Character->bNoclip && !Character->GetActorEnableCollision());
    Character->ToggleNoclip();
    TestFalse(TEXT("noclip restores capsule"), Character->bNoclip);
    Character->SetActorLocation(FVector(Start.X,Start.Y,V->ExplorationBounds().Min.Z-1000));
    Character->Tick(1.f/60);
    TestEqual(TEXT("fall recovery"), Character->RecoveryCount,1);
    UE_LOG(LogTemp, Display, TEXT("P1_VERIFY passed=%d soft=%d surface_hits=%d attached_bodies=%d seats=%d spawns=%d doors=%d toggles=%d walk=%d floor_frames=%d hits=%d recovery=%d import_s=%.3f"),
        !HasAnyErrors(),Soft,SurfaceHits,AttachedBodies,ExpectedSeats,V->SpawnPoints.Num(),V->Doors.Num(),Toggles,Walked,FloorFrames,Character->CapsuleHits,Character->RecoveryCount,Result.TotalSeconds);
    Character->Destroy();
    FSCImporter::Unload(World);
    World->DestroyWorld(false);
    return !HasAnyErrors();
}
#endif
