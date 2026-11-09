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
const FSkeletalMeshLODRenderData& Lod0(USCRigComponent* Rig)
{
    return Rig->GetSkeletalMeshAsset()->GetResourceForRendering()->LODRenderData[0];
}
// Largest distance between live trimesh particles and the CPU skin of the same vertices (cm).
double RefitError(USCRigComponent* Rig)
{
    TArray<FVector3f> Particles;
    const auto& LOD = Lod0(Rig);
    if (!Rig->ReadCollisionVertices(Particles) || Particles.Num() != int32(LOD.GetNumVertices())) return TNumericLimits<double>::Max();
    const int32 Step = FMath::Max(1, Particles.Num() / 4000);
    double Worst = 0;
    for (int32 I = 0; I < Particles.Num(); I += Step)
        Worst = FMath::Max(Worst, double(FVector3f::Dist(Particles[I],
            USkinnedMeshComponent::GetSkinnedVertexPosition(Rig, I, LOD, *Rig->GetSkinWeightBuffer(0)))));
    return Worst;
}
struct FSkinSample { FVector Centre, Normal; };
TArray<FSkinSample> SampleTriangles(USCRigComponent* Rig, int32 Count)
{
    const auto& LOD = Lod0(Rig);
    TArray<uint32> Indices;
    LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
    TArray<FSkinSample> Out;
    const int32 Triangles = Indices.Num() / 3, Step = FMath::Max(1, Triangles / Count);
    for (int32 T = 0; T < Triangles && Out.Num() < Count; T += Step)
    {
        FVector V[3];
        for (int32 K = 0; K < 3; ++K)
            V[K] = Rig->GetComponentTransform().TransformPosition(FVector(USkinnedMeshComponent::GetSkinnedVertexPosition(
                Rig, Indices[T*3+K], LOD, *Rig->GetSkinWeightBuffer(0))));
        const FVector Cross = FVector::CrossProduct(V[1]-V[0], V[2]-V[0]);
        if (Cross.Size() < 1) continue; // < 0.5 cm^2: too small for a centred probe
        Out.Add({(V[0]+V[1]+V[2])/3, Cross.GetSafeNormal()});
    }
    return Out;
}
bool ProbeSurface(USCRigComponent* Rig, const FSkinSample& S, const FVector& Offset)
{
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCP1Probe), false);
    const FVector C = S.Centre + Offset;
    return Rig->LineTraceComponent(Hit, C + S.Normal*2, C - S.Normal*2, Params);
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
    int32 Soft = 0, SurfaceHits = 0, AttachedBodies = 0, Refitted = 0;
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
        TestTrue(TEXT("bind-pose body matches CPU skin"), RefitError(Rig) < 0.05);
        // A synthetic translation of the rig root moves every skinned vertex. The body must follow it
        // through the same refit the GPU host runs: particles equal the CPU skin, probes at the new
        // surface hit and probes at the vacated surface miss.
        if (Rig->LocalPose.Num() > 0)
        {
            const TArray<FSkinSample> Before = SampleTriangles(Rig, 64);
            const int32 RefitsBefore = Rig->GetCollisionRefits();
            const FVector Shift(0,0,137);
            Rig->LocalPose[0].AddToTranslation(Shift);
            Rig->RefreshBoneTransforms();
            const FVector WorldShift = Rig->GetComponentTransform().TransformVector(Shift);
            const double Error = RefitError(Rig);
            int32 NewHits = 0, OldMisses = 0;
            for (const FSkinSample& S : Before)
            {
                NewHits += ProbeSurface(Rig, S, WorldShift);
                OldMisses += !ProbeSurface(Rig, S, FVector::ZeroVector);
            }
            TestTrue(TEXT("pose change refits the body"), Rig->GetCollisionRefits() > RefitsBefore);
            TestTrue(FString::Printf(TEXT("refit particles follow the CPU skin (error %.3f cm)"), Error), Error < 0.05);
            TestTrue(FString::Printf(TEXT("moved surface is hit (%d/%d)"), NewHits, Before.Num()), !Before.IsEmpty() && NewHits >= Before.Num()*9/10);
            TestTrue(FString::Printf(TEXT("vacated surface is missed (%d/%d)"), OldMisses, Before.Num()), OldMisses >= Before.Num()*9/10);
            Rig->ResetPose();
            TestTrue(TEXT("reset pose refits back to bind"), RefitError(Rig) < 0.05);
            ++Refitted;
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
        const int32 Nearest = V->NearestDoor(Door.Rig->GetSocketLocation(Door.Joint),1);
        TestTrue(TEXT("nearest door at driven joint"), V->Doors.IsValidIndex(Nearest) &&
            V->Doors[Nearest].Rig->GetSocketLocation(V->Doors[Nearest].Joint).Equals(Door.Rig->GetSocketLocation(Door.Joint), 1));
        TMap<USCRigComponent*,TArray<FTransform>> OtherPoses;
        for (USCRigComponent* Rig : V->Rigs) if (Rig != Door.Rig) OtherPoses.Add(Rig,Rig->LocalPose);
        TArray<bool> OtherOpen;
        for (int32 J=0; J<V->Doors.Num(); ++J) OtherOpen.Add(V->IsDoorOpen(J));
        const bool UntargetedOpen = V->IsFragmentOpen(Door.Fragment);
        TestTrue(TEXT("fragment open"), V->ToggleDoor(I));
        V->Tick(60);
        TestTrue(TEXT("open state"), V->IsDoorOpen(I));
        for (int32 J=0; J<V->Doors.Num(); ++J)
            if (J != I) TestEqual(TEXT("other interactions keep their open state"), V->IsDoorOpen(J), OtherOpen[J]);
        TestEqual(TEXT("orbit toggle state unaffected by one door"), V->IsFragmentOpen(Door.Fragment), UntargetedOpen);
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
        {
            TestTrue(TEXT("deployed door skin hit"), TraceSkinnedSurface(Door.Rig));
            TestTrue(TEXT("deployed door body equals CPU skin"), RefitError(Door.Rig) < 0.05);
        }
        TestTrue(TEXT("fragment close"), V->ToggleDoor(I));
        V->Tick(60);
        TestFalse(TEXT("closed state"), V->IsDoorOpen(I));
        ++Toggles;
    }
    // Leave entrances deployed for the traversal. No asset/file-name matching.
    for (int32 I=0; I<V->Doors.Num(); ++I) { V->ToggleDoor(I); V->Tick(60); }
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Character = World->SpawnActor<ASCFirstPersonCharacter>(ASCFirstPersonCharacter::StaticClass(), FTransform::Identity, Params);
    TestTrue(TEXT("safe semantic spawn"), Character->Explore(V));
    // Start ranking: a generic DataCore Door never outranks seat access / seat hints; Selected is never ranked.
    const FName StartKind = V->SpawnPoints.IsValidIndex(Character->CurrentSpawn) ? V->SpawnPoints[Character->CurrentSpawn].Kind : NAME_None;
    const bool HasSeatHint = V->SpawnPoints.ContainsByPredicate([](const FSCSpawnPoint& P) {
        return P.Kind == FSCSpawnKind::SeatAccess || P.Kind == FSCSpawnKind::Seat; });
    TestFalse(TEXT("door does not outrank seat hints"), HasSeatHint && StartKind == FSCSpawnKind::Door);
    TestFalse(TEXT("doors are not entrances without data"), V->SpawnPoints.ContainsByPredicate([](const FSCSpawnPoint& P) {
        return P.Kind == FSCSpawnKind::Entrance; }));
    {
        const int32 Revision = V->SpawnRevision, Count = V->SpawnPoints.Num();
        const int32 First = V->SetSelectedSpawn(Character->GetActorTransform());
        const int32 Second = V->SetSelectedSpawn(Character->GetActorTransform());
        TestEqual(TEXT("selected spawn replaces"), V->SpawnPoints.Num(), Count + 1);
        TestTrue(TEXT("spawn list revision changes"), V->SpawnRevision > Revision && First == Second);
        TestFalse(TEXT("Selected is not a start kind"), FSCSpawnKind::StartOrder().Contains(FSCSpawnKind::Selected));
    }
    if (!V->Doors.IsEmpty())
    {
        const FTransform Saved = Character->GetActorTransform();
        // Exercise E's camera/reach selection, not just the actor's lower-level API.
        const FVector Joint = V->Doors[0].Rig->GetSocketLocation(V->Doors[0].Joint);
        Character->SetActorLocation(Joint-Character->Camera->GetRelativeLocation());
        const int32 Nearest = Character->FocusedInteraction();
        TestTrue(TEXT("door in interaction reach"), V->Doors.IsValidIndex(Nearest));
        if (V->Doors.IsValidIndex(Nearest))
        {
            const bool WasOpen = V->IsDoorOpen(Nearest);
            Character->HandleKey(EKeys::E,true);
            TestTrue(TEXT("E toggles nearest fragment"), V->IsDoorOpen(Nearest) != WasOpen);
            Character->HandleKey(EKeys::E,true);
            V->Tick(60);
        }
        Character->SetActorTransform(Saved,false,nullptr,ETeleportType::TeleportPhysics);
    }
    auto* Movement = Character->GetCharacterMovement();
    StepCharacter(Character,120);
    TestTrue(TEXT("start spawn settles on walkable floor"), Movement->IsMovingOnGround());
    // Walk from the first spawn (start ranking, then the rest) that has floor under a complete
    // capsule route; seats can sit in cockpits with no room to walk. Real movement sweeps.
    bool Walked = false;
    int32 FloorFrames = 0, WalkSpawn = INDEX_NONE;
    FVector Start = Character->GetActorLocation();
    TArray<int32> Order;
    for (FName Kind : FSCSpawnKind::StartOrder())
        for (int32 I=0; I<V->SpawnPoints.Num(); ++I) if (V->SpawnPoints[I].Kind == Kind) Order.Add(I);
    Order.Remove(Character->CurrentSpawn);
    Order.Insert(Character->CurrentSpawn, 0);
    for (int32 Spawn : Order)
    {
        if (Walked) break;
        if (Spawn != Character->CurrentSpawn || Order[0] != Spawn)
        {
            if (!Character->TeleportToSpawn(Spawn)) continue;
            StepCharacter(Character,120);
        }
        if (!Movement->IsMovingOnGround()) continue;
        Start = Character->GetActorLocation();
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
            FloorFrames = 0;
            for (int32 Frame=0; Frame<24; ++Frame)
            {
                StepCharacter(Character,1);
                FloorFrames += Movement->IsMovingOnGround();
            }
            Character->SetMoveInput(FVector::ZeroVector);
            Walked = FVector::Dist2D(Start,Character->GetActorLocation()) > 20;
            if (Walked) WalkSpawn = Spawn;
            else Character->SetActorLocation(Start,false,nullptr,ETeleportType::TeleportPhysics);
        }
    }
    UE_LOG(LogTemp, Display, TEXT("P1_WALK spawn=%d kind=%s"), WalkSpawn, V->SpawnPoints.IsValidIndex(WalkSpawn) ? *V->SpawnPoints[WalkSpawn].Kind.ToString() : TEXT("-"));
    TestTrue(TEXT("scripted walk makes progress"), Walked);
    TestTrue(TEXT("walk retains floor contact"), FloorFrames >= 20);
    TestEqual(TEXT("walk did not fall through and recover"), Character->RecoveryCount, 0);
    // Crouch/stand/jump need headroom: use the hull top (a seat spawn may only fit a crouched capsule).
    const int32 HullSpawn = V->SpawnPoints.IndexOfByPredicate([](const FSCSpawnPoint& P) { return P.Kind == FSCSpawnKind::Hull; });
    TestTrue(TEXT("hull spawn for the posture checks"), Character->TeleportToSpawn(HullSpawn));
    StepCharacter(Character,60);
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
    {
        // Exit refused inside geometry: centre the capsule on the floor surface under it.
        const FVector Here = Character->GetActorLocation();
        FHitResult Floor;
        FCollisionQueryParams Q(SCENE_QUERY_STAT(SCP1Noclip), false, Character);
        if (TestTrue(TEXT("floor under noclip test"), World->LineTraceSingleByChannel(Floor, Here, Here - FVector(0,0,400), ECC_Pawn, Q)))
        {
            Character->SetActorLocation(Floor.ImpactPoint);
            Character->ToggleNoclip();
            TestTrue(TEXT("noclip exit refused inside geometry"), Character->bNoclip);
            StepCharacter(Character, 30);
            TestTrue(TEXT("refusal stays visible"), Character->HUDText().Contains(TEXT("Cannot leave noclip")));
            StepCharacter(Character, 200);
            TestFalse(TEXT("refusal expires"), Character->HUDText().Contains(TEXT("Cannot leave noclip")));
            Character->SetActorLocation(Here);
        }
    }
    Character->ToggleNoclip();
    TestFalse(TEXT("noclip restores capsule"), Character->bNoclip);
    StepCharacter(Character, 60);
    {
        const FVector Before = Character->GetActorLocation();
        Character->HoldMoveInput(FVector(0,1,0), 0.5f, true);
        TestTrue(TEXT("held input runs"), Character->IsRunning());
        StepCharacter(Character, 40);
        TestFalse(TEXT("timed input released"), Character->IsHoldingMove() || Character->IsRunning());
        UE_LOG(LogTemp, Display, TEXT("P1_HOLD_MOVE moved_cm=%.1f"), FVector::Dist2D(Before, Character->GetActorLocation()));
    }
    Character->SetActorLocation(FVector(Start.X,Start.Y,V->ExplorationBounds().Min.Z-1000));
    Character->Tick(1.f/60);
    TestEqual(TEXT("fall recovery"), Character->RecoveryCount,1);
    UE_LOG(LogTemp, Display, TEXT("P1_VERIFY passed=%d soft=%d refitted=%d surface_hits=%d attached_bodies=%d seats=%d spawns=%d doors=%d toggles=%d walk=%d floor_frames=%d hits=%d recovery=%d import_s=%.3f"),
        !HasAnyErrors(),Soft,Refitted,SurfaceHits,AttachedBodies,ExpectedSeats,V->SpawnPoints.Num(),V->Doors.Num(),Toggles,Walked,FloorFrames,Character->CapsuleHits,Character->RecoveryCount,Result.TotalSeconds);
    Character->Destroy();
    FSCImporter::Unload(World);
    World->DestroyWorld(false);
    return !HasAnyErrors();
}
#endif
