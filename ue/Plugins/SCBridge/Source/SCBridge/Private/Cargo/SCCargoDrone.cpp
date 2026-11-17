// ASCCargoDrone (flight, the step every move of it takes: clear of loads and drones, swept and slid; its look: the
// drone_art.json skeletal mesh with its clips, or the K1d stand-in with rotors; its heading; its tractor beam and the
// game's beam effects) and ASCCargoDroneDock (bays, a footprint of whole lattice cells).
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoSubsystem.h"
#include "SCCargoDroneAnim.h"
#include "SCCargoFx.h"
#include "SCFxStrength.h"
#include "SCCargoState.h"
#include "EngineUtils.h"
#include "AnimationRuntime.h"
#include "Animation/AnimSequence.h"
#include "Cargo/SCCargoFlightPath.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCCargoArt, Log, All);

namespace
{
// K1d art by soft path; the engine shape when the asset is missing (warned once per asset).
UStaticMesh* Art(const TCHAR* Name, const TCHAR* Fallback)
{
    const FString Path = FString::Printf(TEXT("/SCBridge/Cargo/Meshes/%s.%s"), Name, Name);
    if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet)) return Mesh;
    static TSet<FString> Warned;
    if (!Warned.Contains(Path))
    {
        Warned.Add(Path);
        UE_LOG(LogSCCargoArt, Warning, TEXT("CARGO_ART missing %s: using %s"), *Path, Fallback);
    }
    return LoadObject<UStaticMesh>(nullptr, Fallback);
}
UStaticMeshComponent* NewPart(AActor* Owner, USceneComponent* Parent, UStaticMesh* Mesh, FName Socket = NAME_None)
{
    auto* C = NewObject<UStaticMeshComponent>(Owner, NAME_None, RF_Transient);
    C->SetMobility(EComponentMobility::Movable);
    C->SetStaticMesh(Mesh);
    C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    C->SetupAttachment(Parent, Socket);
    Owner->AddInstanceComponent(C);
    C->RegisterComponent();
    return C;
}
} // namespace

int32 ASCCargoDrone::Overlaps = 0;
static constexpr double OwnBoxGapCm = 2; // kept between the hull and the face of the box it carries, when the box comes up to it

ASCCargoDrone::ASCCargoDrone()
{
    PrimaryActorTick.bCanEverTick = true;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent->SetMobility(EComponentMobility::Movable);
}

void ASCCargoDrone::PostInitializeComponents()
{
    Super::PostInitializeComponents();
    if (Hull || Art) return;
    if (FSCCargoDroneArt::Get().HasArt()) SetupArt();
    if (!Art) SetupStandIn();
    UE_LOG(LogTemp, Display, TEXT("CARGO_DRONE_SHAPE half=%s centre=%s radius_cm=%.0f look=%s"), *HullHalf.ToCompactString(), *HullCentre.ToCompactString(), HullRadius(),
           Art ? TEXT("art") : TEXT("stand-in"));
    Beam = NewPart(this, RootComponent, ::Art(TEXT("SM_TractorBeam"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
    Beam->SetCastShadow(false);
    Beam->SetVisibility(false);
    Light = NewObject<UPointLightComponent>(this, NAME_None, RF_Transient);
    const FName LightBone = FSCCargoDroneArt::Get().LightBone;
    if (Art) Light->SetupAttachment(Art, Art->GetBoneIndex(LightBone) != INDEX_NONE ? LightBone : NAME_None);
    else Light->SetupAttachment(Hull, Hull->DoesSocketExist(TEXT("Light_Status")) ? FName(TEXT("Light_Status")) : NAME_None);
    LeanScale = Art ? FSCCargoDroneArt::Get().ActorLeanScale : 1.f;
    Light->SetIntensityUnits(ELightUnits::Candelas);
    Light->SetIntensity(20.f);
    Light->SetAttenuationRadius(250.f);
    Light->SetLightColor(FLinearColor(0.3f, 0.8f, 1.f));
    Light->SetCastShadows(false);
    AddInstanceComponent(Light);
    Light->RegisterComponent();
    LastYaw = WantYaw = float(GetActorRotation().Yaw);
    if (!Art && Hull && Hull->DoesSocketExist(TEXT("Tractor"))) EmitterLocal = Hull->GetSocketTransform(TEXT("Tractor"), RTS_Actor).GetLocation();
}

double ASCCargoDrone::HullHangCm() const
{
    return FSCCargoTug::HullHangCm(EmitterLocal.Z, HullCentre.Z - HullHalf.Z, OwnBoxGapCm);
}

void ASCCargoDrone::SetupArt()
{
    const FSCCargoDroneArt& Cfg = FSCCargoDroneArt::Get();
    USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Cfg.Mesh, nullptr, LOAD_NoWarn | LOAD_Quiet);
    static bool bWarned = false;
    if (!Mesh)
    {
        if (!bWarned)
        {
            UE_LOG(LogSCCargoArt, Warning, TEXT("CARGO_DRONE_ART missing %s (drone_art.json): the stand-in drone"), *Cfg.Mesh);
        }
        bWarned = true;
        return;
    }
    const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
    const int32 CentreIndex = Ref.FindBoneIndex(Cfg.CentreBone);
    // The centre bone on the actor origin, the face turned to +X, native size x the art's scale.
    const FTransform CentreRef = CentreIndex != INDEX_NONE ? FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, CentreIndex) : FTransform::Identity;
    const FQuat Yaw(FVector::UpVector, FMath::DegreesToRadians(Cfg.YawOffsetDeg));
    const FTransform MeshToActor = FTransform(-CentreRef.GetLocation()) * FTransform(Yaw, FVector::ZeroVector, FVector(Cfg.Scale));
    Art = NewObject<USkeletalMeshComponent>(this, NAME_None, RF_Transient);
    Art->SetMobility(EComponentMobility::Movable);
    Art->SetSkeletalMesh(Mesh);
    Art->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Art->SetGenerateOverlapEvents(false);
    Art->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered; // a docked drone out of view costs nothing
    Art->SetAnimationMode(EAnimationMode::AnimationBlueprint, false);
    Art->SetAnimInstanceClass(USCCargoDroneAnimInstance::StaticClass());
    Art->SetupAttachment(RootComponent);
    Art->SetRelativeTransform(MeshToActor);
    AddInstanceComponent(Art);
    Art->RegisterComponent();
    // The actor carries the motion: the centre bone keeps its reference place (within body_bob_cm, in the mesh's own
    // units); the clips keep their rotations and moving parts.
    const double BobNative = Cfg.BodyBobCm / FMath::Max(Cfg.Scale, 1e-3);
    if (USCCargoDroneAnimInstance* Anim = Cast<USCCargoDroneAnimInstance>(Art->GetAnimInstance()))
    {
        Anim->YawLockBone = Cfg.YawLockBone;
        Anim->PlaceLockBone = Cfg.CentreBone;
        Anim->MaxBob = float(BobNative);
    }
    if (const int32 BeamIndex = Ref.FindBoneIndex(Cfg.BeamBone); BeamIndex != INDEX_NONE)
        EmitterLocal = MeshToActor.TransformPosition(FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, BeamIndex).GetLocation());
    // Material slots the art hides (a scanner fan, say), every LOD.
    TSet<int32> Hidden;
    const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
    for (int32 M = 0; M < Materials.Num(); ++M)
        if (Cfg.HiddenSlots.Contains(Materials[M].MaterialSlotName)) Hidden.Add(M);
    const FSkeletalMeshRenderData* Render = Mesh->GetResourceForRendering();
    if (Render)
        for (int32 L = 0; L < Render->LODRenderData.Num(); ++L)
            for (int32 S = 0; S < Render->LODRenderData[L].RenderSections.Num(); ++S)
                if (const int32 M = Render->LODRenderData[L].RenderSections[S].MaterialIndex; Hidden.Contains(M)) Art->ShowMaterialSection(M, S, false, L);
    // Its clips.
    for (const FString& Path : Cfg.AssetPaths())
    {
        if (Path == Cfg.Mesh) continue;
        if (UAnimSequence* Seq = LoadObject<UAnimSequence>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
        {
            Clips.Add(Path, Seq);
        }
        else
        {
            UE_LOG(LogSCCargoArt, Warning, TEXT("CARGO_DRONE_ART missing clip %s"), *Path);
        }
    }
    // The hull: the visible sections' vertices (reference pose), as the centre bone carries them through every clip,
    // in the actor's frame. Its collision shape covers it at every heading: level, square in plan, the radius of its
    // farthest corner from the origin (the heading turns it about the origin; HullCentre is world-aligned).
    FBox Vertices(ForceInit);
    int32 Sections = 0;
    if (Render && Render->LODRenderData.Num())
    {
        const FSkeletalMeshLODRenderData& L = Render->LODRenderData[0];
        const FPositionVertexBuffer& P = L.StaticVertexBuffers.PositionVertexBuffer;
        if (P.GetVertexData())
            for (const FSkelMeshRenderSection& S : L.RenderSections)
            {
                if (Hidden.Contains(S.MaterialIndex)) continue;
                ++Sections;
                for (uint32 V = S.BaseVertexIndex; V < S.BaseVertexIndex + S.NumVertices && V < P.GetNumVertices(); ++V) Vertices += FVector(P.VertexPosition(V));
            }
    }
    const TCHAR* Source = TEXT("vertices");
    if (!Vertices.IsValid)
    {
        Vertices = Mesh->GetImportedBounds().GetBox(); // no CPU vertex data: the imported bounds (every section)
        Source = TEXT("imported bounds");
    }
    FBox Local(ForceInit);
    double Radius = 0; // the farthest corner from the origin in plan (a turn about the origin keeps it)
    int32 Poses = 0;
    auto AddPose = [&](const FTransform& Delta) {
        for (int32 C = 0; C < 8; ++C)
        {
            const FVector Corner((C & 1) ? Vertices.Max.X : Vertices.Min.X, (C & 2) ? Vertices.Max.Y : Vertices.Min.Y, (C & 4) ? Vertices.Max.Z : Vertices.Min.Z);
            const FVector P = MeshToActor.TransformPosition(Delta.TransformPosition(Corner));
            Local += P;
            Radius = FMath::Max(Radius, P.Size2D());
        }
        ++Poses;
    };
    AddPose(FTransform::Identity);
    if (CentreIndex != INDEX_NONE)
        for (const auto& Pair : Clips)
        {
            const UAnimSequence* Seq = Pair.Value;
            const USkeleton* Skeleton = Seq ? Seq->GetSkeleton() : nullptr;
            if (!Skeleton) continue;
            const FReferenceSkeleton& SkelRef = Skeleton->GetReferenceSkeleton();
            for (int32 K = 0; K <= 8; ++K)
            {
                // The centre bone's component-space pose at this time (its parents' local poses chained).
                const double Time = Seq->GetPlayLength() * K / 8.0;
                FTransform Posed = FTransform::Identity;
                for (int32 B = SkelRef.FindBoneIndex(Cfg.CentreBone); B != INDEX_NONE; B = SkelRef.GetParentIndex(B))
                {
                    FTransform BoneLocal;
                    Seq->GetBoneTransform(BoneLocal, FSkeletonPoseBoneIndex(B), FAnimExtractContext(Time), false);
                    Posed = Posed * BoneLocal;
                }
                // As it renders: the centre bone held at its place (the anim instance's lock), within the bob.
                Posed.SetTranslation(CentreRef.GetTranslation() + (Posed.GetTranslation() - CentreRef.GetTranslation()).GetClampedToMaxSize(BobNative));
                // Mesh-space vertex -> as the posed centre bone carries it (the yaw-locked turn clips' yaw included:
                // conservative).
                AddPose(CentreRef.Inverse() * Posed);
            }
        }
    HullCentre = FVector(0, 0, Local.GetCenter().Z);
    HullHalf = FVector(Radius, Radius, Local.GetExtent().Z);
    UE_LOG(LogSCCargoArt, Display, TEXT("CARGO_DRONE_ART mesh=%s hull_from=%s sections=%d poses=%d local_min=%s local_max=%s (actor frame, face +X) plan_radius_cm=%.1f clips=%d"),
           *Cfg.Mesh, Source, Sections, Poses, *Local.Min.ToCompactString(), *Local.Max.ToCompactString(), Radius, Clips.Num());
}

void ASCCargoDrone::SetupStandIn()
{
    UStaticMesh* HullMesh = ::Art(TEXT("SM_CargoDrone"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    Hull = NewPart(this, RootComponent, HullMesh);
    const bool bArt = Hull->DoesSocketExist(TEXT("Tractor"));
    if (!bArt) Hull->SetRelativeScale3D(FVector(0.8, 0.8, 0.3));
    UStaticMesh* RotorMesh = ::Art(TEXT("SM_CargoDroneRotor"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    for (int32 I = 0; I < 4; ++I)
    {
        const FName Socket(*FString::Printf(TEXT("Fan_%d"), I));
        UStaticMeshComponent* Rotor = NewPart(this, Hull, RotorMesh, Hull->DoesSocketExist(Socket) ? Socket : NAME_None);
        if (!Hull->DoesSocketExist(Socket))
        {
            Rotor->SetRelativeLocation(FVector(I < 2 ? 45 : -45, I % 2 ? 45 : -45, 60));
            Rotor->SetRelativeScale3D(FVector(0.35, 0.35, 0.05));
        }
        Rotors.Add(Rotor);
    }
    // The collision shape: the hull's and the rotors' bounds, level, about the actor's origin.
    FBox Shape = Hull->Bounds.GetBox();
    for (const UStaticMeshComponent* Rotor : Rotors) Shape += Rotor->Bounds.GetBox();
    HullCentre = Shape.GetCenter() - GetActorLocation();
    HullHalf = Shape.GetExtent();
}

void ASCCargoDrone::FlyTo(const FVector& InTarget)
{
    Target = InTarget;
    bFlying = true;
}

bool ASCCargoDrone::Arrived(float ToleranceCm) const
{
    return FVector::Dist(GetActorLocation(), Target) <= ToleranceCm && Velocity.Size() < 40.f;
}

void ASCCargoDrone::Place(const FTransform& World)
{
    bFlying = false;
    Velocity = FVector::ZeroVector;
    Target = World.GetLocation();
    SetActorTransform(World);
    Trail.Reset();
    Travel = FVector::ZeroVector;
    LastYaw = WantYaw = float(World.Rotator().Yaw);
}

float ASCCargoDrone::Heading(float Yaw, float Dt)
{
    const FSCCargoDroneArt& Cfg = FSCCargoDroneArt::Get();
    // In its dock's corridor (in its bay, launching out of it, docking into it) it faces out of its bay.
    TOptional<float> BayYaw;
    if (InDockWay())
        if (const USCCargoSubsystem* Cargo = GetWorld() ? GetWorld()->GetSubsystem<USCCargoSubsystem>() : nullptr)
            if (FTransform BayPose; Cargo->DroneBay(this, BayPose)) BayYaw = float(BayPose.Rotator().Yaw);
    WantYaw = FSCCargoHeading::Target(Yaw, bByLoad ? LoadVelocity : Travel, Cfg.HoverSpeedCmS, BayYaw);
    return FSCCargoHeading::Turn(Yaw, WantYaw, Cfg.TurnRateDegS, Dt);
}

void ASCCargoDrone::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    const float Dt = FMath::Min(DeltaSeconds, 0.05f);
    FVector Accel = FVector::ZeroVector;
    if (bFlying && Dt > 0 && MaxSpeed > 0 && MaxAccel > 0)
    {
        // Speed toward the target, capped by what thrust can still brake to zero at it; thrust limits the change.
        const FVector To = Target - GetActorLocation();
        const double Dist = To.Size();
        const double Speed = FMath::Min<double>(MaxSpeed, FMath::Sqrt(2.0 * MaxAccel * 0.8 * Dist));
        const FVector Desired = Dist > 0.5 ? To / Dist * Speed : FVector::ZeroVector;
        const FVector Change = (Desired - Velocity).GetClampedToMaxSize(MaxAccel * Dt);
        Accel = Change / Dt;
        Velocity += Change;
        if (Dist < 0.5 && Velocity.SizeSquared() < 1) Velocity = FVector::ZeroVector;
        FRotator R = GetActorRotation();
        R.Yaw = Heading(float(R.Yaw), Dt);
        // Lean into the acceleration (visual only): a few degrees, in the drone's own frame.
        const FVector Local = FRotator(0, R.Yaw, 0).UnrotateVector(Accel) / FMath::Max(MaxAccel, 1.f);
        R.Pitch = FMath::FInterpTo(R.Pitch, float(-Local.X * 8.0 * LeanScale), Dt, 4.f);
        R.Roll = FMath::FInterpTo(R.Roll, float(Local.Y * 8.0 * LeanScale), Dt, 4.f);
        const FVector Before = GetActorLocation();
        Step(Before + Velocity * Dt, R, Dt);
        if (bBlocked) Velocity = (GetActorLocation() - Before) / Dt;
    }
    // Its travel (what the heading and the clips follow): its velocity, smoothed.
    const float Smooth = FSCCargoDroneArt::Get().SmoothingS;
    Travel = Smooth > 0 && Dt > 0 ? FMath::Lerp(Travel, Velocity, 1.f - FMath::Exp(-Dt / Smooth)) : Velocity;
    if (State == ESCCargoDroneState::Docked) Travel = FVector::ZeroVector;
    RotorAngle = FMath::Fmod(RotorAngle + Dt * (State == ESCCargoDroneState::Docked ? 0.f : 2400.f), 360.f);
    for (int32 I = 0; I < Rotors.Num(); ++I)
        if (Rotors[I]) Rotors[I]->SetRelativeRotation(FRotator(0, I % 2 ? RotorAngle : -RotorAngle, 0));
    Animate(Dt);
}

FString ASCCargoDrone::ClipName() const
{
    static const TCHAR* const Motions[] = {TEXT("Idle"), TEXT("Front"), TEXT("Back"), TEXT("Up"), TEXT("Down"), TEXT("Left"), TEXT("Right"), TEXT("TurnLeft"), TEXT("TurnRight"), TEXT("Scanning")};
    static const TCHAR* const Phases[] = {TEXT("Start"), TEXT("Loop"), TEXT("End")};
    return FString::Printf(TEXT("%s/%s"), Motions[int32(Motion.Motion)], Phases[int32(Motion.Phase)]);
}

UAnimSequence* ASCCargoDrone::ClipFor(ESCDroneMotion M, ESCDroneClipPhase P, int32 IdleIndex)
{
    const FSCCargoDroneArt& Cfg = FSCCargoDroneArt::Get();
    for (const FString& Path : {Cfg.Clip(M, P, IdleIndex), Cfg.Clip(M, ESCDroneClipPhase::Loop, IdleIndex)})
        if (const TObjectPtr<UAnimSequence>* Seq = Clips.Find(Path)) return *Seq;
    return nullptr;
}

void ASCCargoDrone::Animate(float Dt)
{
    USCCargoDroneAnimInstance* Anim = Art ? Cast<USCCargoDroneAnimInstance>(Art->GetAnimInstance()) : nullptr;
    if (!Anim || Dt <= 0) return;
    const FSCCargoDroneArt& Cfg = FSCCargoDroneArt::Get();
    const float Yaw = float(GetActorRotation().Yaw);
    YawRate = FMath::Lerp(YawRate, FRotator::NormalizeAxis(Yaw - LastYaw) / Dt, 1.f - FMath::Exp(-Dt / FMath::Max(Cfg.SmoothingS, 1e-3f)));
    LastYaw = Yaw;
    FSCCargoDroneMotion::FInput In;
    In.LocalVelocity = FRotator(0, Yaw, 0).UnrotateVector(Travel);
    In.YawRateDegS = State == ESCCargoDroneState::Docked ? 0.f : YawRate;
    In.bScanning = bScanning;
    const bool bFirst = !bAnimStarted;
    if (bFirst) Motion.IdleIndex = FMath::Max(DroneId, 0); // each drone its own idle first
    const bool bChanged = Motion.Step(In, Dt, Cfg, [this](ESCDroneMotion M, ESCDroneClipPhase P) {
        const UAnimSequence* Seq = ClipFor(M, P, Motion.IdleIndex);
        return Seq ? Seq->GetPlayLength() : 0.f;
    });
    if (!bChanged && !bFirst) return;
    bAnimStarted = true;
    const bool bTurn = Motion.Motion == ESCDroneMotion::TurnLeft || Motion.Motion == ESCDroneMotion::TurnRight;
    Anim->Play(ClipFor(Motion.Motion, Motion.Phase, Motion.IdleIndex), Motion.Loops(), bFirst ? 0.f : Cfg.BlendS, bTurn);
}

FVector ASCCargoDrone::Emitter() const
{
    if (Art)
    {
        const FName Bone = FSCCargoDroneArt::Get().BeamBone;
        if (Art->GetBoneIndex(Bone) != INDEX_NONE) return Art->GetBoneLocation(Bone);
        return Art->GetComponentLocation();
    }
    return Hull && Hull->DoesSocketExist(TEXT("Tractor")) ? Hull->GetSocketLocation(TEXT("Tractor")) : GetActorLocation() - FVector(0, 0, 20);
}

FVector ASCCargoDrone::EmitterForward() const
{
    if (Art) return GetActorQuat().RotateVector(FSCCargoDroneArt::Get().BeamAxis);
    if (Hull && Hull->DoesSocketExist(TEXT("Tractor"))) return Hull->GetSocketQuaternion(TEXT("Tractor")).GetForwardVector();
    return -GetActorUpVector();
}

void ASCCargoDrone::Gusts(float Dt, FVector2D& OutWobble)
{
    // Gusts: the controller holds the flight point within the sway (a wander of a few cm, a wobble of a few degrees).
    // Each drone's are its own process (its seed's spectrum, FSCCargoSway).
    SwayGainNow = FMath::FInterpTo(SwayGainNow, SwayGain, Dt, 1.5f);
    const double T = GetWorld() ? GetWorld()->GetTimeSeconds() : 0;
    const uint32 Seed = GetTypeHash(DroneId) ^ 0x5eed;
    SwayNow = SwayGainNow > 1e-3f ? FSCCargoSway::Offset(Seed, T, SwayAmplitudeCm * SwayGainNow, SwayFrequencyHz) : FVector::ZeroVector;
    OutWobble = SwayGainNow > 1e-3f ? FSCCargoSway::Tilt(Seed, T, SwayTiltDeg * SwayGainNow, SwayFrequencyHz) : FVector2D::ZeroVector;
}

void ASCCargoDrone::Step(const FVector& Want, const FRotator& Rotation, float Dt)
{
    const FVector From = GetActorLocation();
    UWorld* World = GetWorld();
    USCCargoSubsystem* Cargo = World ? World->GetSubsystem<USCCargoSubsystem>() : nullptr;
    // In its bay, and between the bay and its exit, the dock holds and moves it.
    if (!Cargo || State == ESCCargoDroneState::Docked || (bDockLeg && InDockWay()))
    {
        SetActorLocationAndRotation(Want, Rotation);
        KeptAt = Want;
        bBlocked = bOverlapping = false;
        return;
    }
    // No faster than twice its speed limit (a drone held up or turned aside catches up; it never jumps).
    const double MaxStep = MaxSpeed > 0 ? 2.0 * MaxSpeed * Dt : TNumericLimits<double>::Max();
    FVector To = From + (Want - From).GetClampedToMaxSize(MaxStep);
    const FVector Asked = To;
    // The loads other teams fly are not swept against the drones; a drone holding no load steps out of their way as
    // soon as it takes it to, also one hovering over its box at rest while its beams lock on (a load planned before it
    // came to its station may pass there). (A team's drones stay with their box in the air: loads keep apart by their
    // right of way, which reaches as far as the drones over them.)
    TArray<FSCCargoDroneSteer::FLoad, TInlineAllocator<8>> Loads;
    if (DodgesLoads(State)) Cargo->DroneLoads(this, Loads);
    if (!Loads.IsEmpty())
    {
        // With half the clearance a route keeps around its hull: a load in the air sways.
        const FVector Centre = FSCCargoDroneSteer::Evade(To + HullCentre, HullHalf + FVector(SCCargoFlightTuning::DroneClearanceCm / 2), Loads, MaxAccel);
        To = From + (Centre - HullCentre - From).GetClampedToMaxSize(MaxStep);
    }
    // Its own box in the air sways and lags its route, up to a drone on a short beam: off the face its beam holds.
    if (FTransform Own; CarryingBoxId != INDEX_NONE)
        if (FVector OwnHalf; Cargo->DroneOwnBox(this, Own, OwnHalf))
            To = From + (FSCCargoDroneSteer::OffFace(To + HullCentre, HullHalf + FVector(OwnBoxGapCm), Own, OwnHalf) - HullCentre - From).GetClampedToMaxSize(MaxStep);
    // The other drones in the air, as they are now: where, how fast, how big.
    TArray<FSCCargoDroneSteer::FOther, TInlineAllocator<16>> Others;
    for (TActorIterator<ASCCargoDrone> It(World); It; ++It)
    {
        if (*It == this || It->State == ESCCargoDroneState::Docked) continue;
        FSCCargoDroneSteer::FOther& O = Others.AddDefaulted_GetRef();
        O.P = It->GetActorLocation();
        O.V = It->GetVelocity();
        O.Radius = It->HullRadius();
    }
    const FVector Evaded = To;
    To = FSCCargoDroneSteer::Separate(From, To, HullRadius(), Others, Dt, MaxStep); // no further than any step goes
    // The surroundings and the boxes: swept with the hull's bounds, slid along what is met.
    FString By;
    FVector End = FSCCargoDroneSteer::Slide(From, To, [&](const FVector& A, const FVector& B, double& OutFraction, FVector& OutNormal) {
        return Cargo->DroneSweep(this, A, B, OutFraction, OutNormal, &By);
    }, bBlocked);
    // Held up: what by, once in a while (a drone that slides along something for a while says so once).
    if (const double Now = World->GetTimeSeconds(); bBlocked && Now - BlockedLogged > 2.0)
    {
        BlockedLogged = Now;
        UE_LOG(LogTemp, Display, TEXT("CARGO_DRONE_BLOCKED drone=%d state=%d at=%s wanted=%s by=%s"), DroneId, int32(State), *From.ToCompactString(), *To.ToCompactString(), *By);
    }
    // Slid into another drone's room: stay.
    for (const FSCCargoDroneSteer::FOther& O : Others)
        if (FVector::Dist(End, O.P) < HullRadius() + O.Radius && FVector::Dist(From, O.P) >= FVector::Dist(End, O.P)) End = From;
    SetActorLocationAndRotation(End, Rotation);
    if (FVector::Dist(KeptAt, End) > KeptSlackCm) KeptAt = End;
    if (Trail.IsEmpty() || FVector::Dist(Trail.Last(), End) >= TrailStepCm)
    {
        if (Trail.Num() >= TrailPoints) Trail.RemoveAt(0);
        Trail.Add(End);
    }
    // What it ended in, if anything (it should never): counted once each time it begins.
    FString What;
    for (const FSCCargoDroneSteer::FOther& O : Others)
        if (FVector::Dist(End, O.P) < HullRadius() + O.Radius) What = TEXT("a drone");
    if (What.IsEmpty()) Cargo->DroneOverlap(this, End, What);
    if (!What.IsEmpty() && !bOverlapping)
    {
        ++Overlaps;
        UE_LOG(LogTemp, Warning, TEXT("CARGO_DRONE_OVERLAP drone=%d with=%s at=%s state=%d overlaps=%d from=%s loads=%d evaded_cm=%.0f apart_cm=%.0f slid_cm=%.0f carrying=%d"), DroneId, *What,
               *End.ToCompactString(), int32(State), Overlaps, *From.ToCompactString(), Loads.Num(), FVector::Dist(Asked, Evaded), FVector::Dist(Evaded, To), FVector::Dist(To, End), CarryingBoxId);
    }
    bOverlapping = !What.IsEmpty();
}

void ASCCargoDrone::Lean(const FVector& Accel, const FVector2D& Wobble, float Dt, const FVector& At)
{
    const FVector Before = GetActorLocation();
    FRotator R = GetActorRotation();
    R.Yaw = Heading(float(R.Yaw), Dt);
    bByLoad = false; // set again by its task each tick it carries
    // Lean into the acceleration (visual): about 8 degrees at the empty drones' full thrust, plus the gusts' wobble.
    const FVector Local = FRotator(0, R.Yaw, 0).UnrotateVector(Accel) / FMath::Max(MaxAccel, 1.f);
    // With the art the clips lean the body themselves: the actor's share is the art's actor_lean_scale.
    R.Pitch = FMath::FInterpTo(R.Pitch, float(FMath::Clamp(-Local.X * 8.0 * LeanScale + Wobble.X, -15.0, 15.0)), Dt, 5.f);
    R.Roll = FMath::FInterpTo(R.Roll, float(FMath::Clamp(Local.Y * 8.0 * LeanScale + Wobble.Y, -15.0, 15.0)), Dt, 5.f);
    Step(At, R, Dt);
    Velocity = (GetActorLocation() - Before) / Dt;
}

void ASCCargoDrone::Pose(const FVector& Location, const FVector& Accel, float DeltaSeconds)
{
    bFlying = false;
    const float Dt = FMath::Max(DeltaSeconds, 1e-3f);
    const FVector SwayBefore = SwayNow;
    FVector2D Wobble;
    Gusts(Dt, Wobble);
    FVector At = Location;
    if (bBody)
    {
        // Coming off its own station (a tug just over): the body flies onto the formation point, then keeps it.
        const FVector Offset = GetActorLocation() - SwayBefore - Flight.P * 100.0; // actor origin from the body's point
        Flight.Step((Location - Offset) / 100.0, (Location - PoseTarget) / 100.0 / Dt, FVector::ZeroVector, Dt);
        At = Flight.P * 100.0 + Offset;
        if (FVector::Dist(At, Location) < 2.0) bBody = false;
    }
    PoseTarget = Location;
    Lean(Accel, Wobble, Dt, At + SwayNow);
}

void ASCCargoDrone::Tug(const FSCCargoTug::FStation& Station, const FVector& ReactionN, float DeltaSeconds)
{
    bFlying = false;
    const float Dt = FMath::Max(DeltaSeconds, 1e-3f);
    FVector2D Wobble;
    const FVector SwayBefore = SwayNow;
    Gusts(Dt, Wobble);
    // The body is the emitter less the gusts: they push the drone about the point its thrust holds.
    const FVector Offset = GetActorLocation() - Emitter();
    if (!bBody)
    {
        Flight.P = (Emitter() - SwayBefore) / 100.0;
        Flight.V = Velocity / 100.0;
        Flight.A = FVector::ZeroVector;
        bBody = true;
    }
    Flight.Step(Station.Hover, Station.HoverV, ReactionN, Dt);
    BeamRestM = Station.RestM;
    PoseTarget = Flight.P * 100.0 + Offset;
    const FVector Before = GetActorLocation();
    Lean(Flight.A * 100.0, Wobble, Dt, PoseTarget + SwayNow);
    // Held up or turned aside on the way: the body is where the drone is. Turned aside by its own steering (another
    // drone passing), its thrust's velocity is as it was: the step moved it, it gave it no speed. (Read as a
    // velocity, a step away from a drone flying in through the door a load was leaving by sent that load's two
    // drones 12 and 15 m off within a second.) Held up by something it met, it keeps of its velocity what lies
    // along the way it slid.
    if (const FVector Actual = GetActorLocation() - SwayNow - Offset; !Actual.Equals(Flight.P * 100.0, 0.5))
    {
        Flight.P = Actual / 100.0;
        if (bBlocked)
        {
            const FVector Went = (GetActorLocation() - Before).GetSafeNormal();
            Flight.V = Went * FMath::Max(FVector::DotProduct(Flight.V, Went), 0.0);
            Flight.A = FVector::ZeroVector;
        }
    }
}

void ASCCargoDrone::SetBeam(bool bOn, const FVector& Point, float WidthCm)
{
    if (!Beam) return;
    // Locking its beam onto a box (its scanning clip).
    bScanning = bOn && State == ESCCargoDroneState::Attaching;
    if (SetBeamFx(bOn, Point))
    {
        Beam->SetVisibility(false);
        return;
    }
    Beam->SetVisibility(bOn);
    if (!bOn || !Beam->GetStaticMesh()) return;
    // The beam mesh spans its bounds' Z; stretch that span from the emitter to the point, radius ~ a fifth of its length.
    const FBox B = Beam->GetStaticMesh()->GetBoundingBox();
    const FVector From = Emitter(), Dir = Point - From;
    const double Length = FMath::Max(Dir.Size(), 1.0), Span = FMath::Max(B.GetSize().Z, 1.0), Width = FMath::Max(B.GetSize().X, 1.0);
    const FQuat Along = FQuat::FindBetweenNormals(FVector::UpVector, Dir / Length);
    const FVector Scale(WidthCm / Width, WidthCm / Width, Length / Span);
    const FVector Base = From - Along.RotateVector(FVector(0, 0, B.Min.Z) * Scale);
    Beam->SetWorldTransform(FTransform(Along, Base, Scale));
}

bool ASCCargoDrone::SetBeamFx(bool bOn, const FVector& Point)
{
    UWorld* World = GetWorld();
    const USCCargoSubsystem* Cargo = World ? World->GetSubsystem<USCCargoSubsystem>() : nullptr;
    FSCCargoFx* Fx = Cargo ? Cargo->GetFx() : nullptr;
    UNiagaraSystem* BeamSystem = Fx ? Fx->Beam() : nullptr;
    if (!BeamSystem)
    {
        // Not built on this machine, or no archive: the K1d beam mesh (said once, by the first drone that beams).
        static bool bSaid = false;
        if (bOn && !bSaid)
        {
            bSaid = true;
            UE_LOG(LogSCCargoArt, Display, TEXT("CARGO_FX the drones draw the beam mesh: %s"), Fx ? *Fx->Missing() : TEXT("the tractor record is not read yet"));
        }
        return false;
    }
    auto Make = [this](TObjectPtr<UNiagaraComponent>& C, UNiagaraSystem* System) {
        if (C || !System) return;
        C = NewObject<UNiagaraComponent>(this, NAME_None, RF_Transient);
        C->SetAsset(System);
        C->SetAutoActivate(false);
        C->SetUsingAbsoluteLocation(true);
        C->SetUsingAbsoluteRotation(true);
        C->SetUsingAbsoluteScale(true);
        C->SetupAttachment(RootComponent);
        AddInstanceComponent(C);
        C->RegisterComponent();
    };
    Make(BeamFx, BeamSystem);
    Make(HitFx, Fx->Hit());
    Make(MuzzleFx, Fx->Muzzle());
    if (!bOn)
    {
        // killParticlesOnDeactivate in the data: the beam goes at once.
        for (UNiagaraComponent* C : {BeamFx.Get(), HitFx.Get(), MuzzleFx.Get()})
            if (C && C->IsActive()) C->DeactivateImmediate();
        bBeamOn = false;
        return true;
    }
    // Strength by the beam's state (FSCCargoDroneDefinition::FStrengths): on its box and holding it, stable; on its
    // box before it holds it (locking on, letting go), unstable. The hit: controlling its target when it holds it,
    // else the beam's. Shown strengths approach those at beamEffectBlendCoeff per second (from noTarget when it lights).
    const FSCCargoDroneDefinition::FStrengths& S = Fx->Strengths();
    const float Dt = World ? World->GetDeltaSeconds() : 0.f;
    const float BeamWant = S.bSet ? float(IsHolding() ? S.Stable : S.Unstable) : 1.f;
    const float HitWant = S.bSet ? float(IsHolding() ? S.ImpactControlling : S.Unstable) : 1.f;
    if (!bBeamOn)
    {
        BeamStrength = HitStrength = S.bSet ? float(S.NoTarget) : 1.f;
        bBeamOn = true;
    }
    const float Blend = S.bSet && S.BlendCoeff > 0 ? 1.f - FMath::Exp(-float(S.BlendCoeff) * Dt) : 1.f;
    BeamStrength += (BeamWant - BeamStrength) * Blend;
    HitStrength += (HitWant - HitStrength) * Blend;
    // The beam component at the emitter, its +X the emitter's forward; the end and tangent in its space (no scale).
    const FVector From = Emitter();
    const FQuat Along = FRotationMatrix::MakeFromX(EmitterForward()).ToQuat();
    const FVector End = Along.UnrotateVector(Point - From);
    const float Length = float(End.Size());
    BeamFx->SetWorldLocationAndRotation(From, Along);
    BeamFx->SetVariableVec3(TEXT("SC.BeamEnd"), End);
    BeamFx->SetVariableVec3(TEXT("SC.BeamStartTangent"), FVector(Length, 0, 0));
    BeamFx->SetVariableFloat(TEXT("SC.BeamLength"), Length);
    BeamFx->SetVariableVec2(TEXT("SC.BeamLength2"), FVector2D(Length, Length));
    SCFxStrength::Set(BeamFx, BeamStrength);
    if (!BeamFx->IsActive()) BeamFx->Activate(true);
    if (HitFx)
    {
        // The hit on the box, its emit axis back along the beam.
        HitFx->SetWorldLocationAndRotation(Point, FRotationMatrix::MakeFromX((From - Point).GetSafeNormal(1e-6, FVector::UpVector)).ToQuat());
        SCFxStrength::Set(HitFx, HitStrength);
        if (!HitFx->IsActive()) HitFx->Activate(true);
    }
    if (MuzzleFx)
    {
        MuzzleFx->SetWorldLocationAndRotation(From, Along);
        SCFxStrength::Set(MuzzleFx, BeamStrength);
        if (!MuzzleFx->IsActive()) MuzzleFx->Activate(true);
    }
    return true;
}

void ASCCargoDrone::Grab(UPrimitiveComponent* Box)
{
    Held = Box;
    BeamLengthM = 0; // not measured yet: no rate on the first step
    BeamForceN = FVector::ZeroVector;
}

void ASCCargoDrone::Drop()
{
    Held = nullptr;
    BeamForceN = FVector::ZeroVector;
}

// ---------------------------------------------------------------------------------------------------- dock

ASCCargoDroneDock::ASCCargoDroneDock()
{
    PrimaryActorTick.bCanEverTick = false;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent->SetMobility(EComponentMobility::Movable);
}

FIntVector ASCCargoDroneDock::MeshCells(const FVector& CellCm)
{
    const UStaticMesh* M = Art(TEXT("SM_CargoDroneDock"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    return M ? FSCCargoDockFootprint::Cells(M->GetBoundingBox().GetSize(), CellCm) : FIntVector(1);
}

void ASCCargoDroneDock::SetFootprint(const FIntVector& Cells, const FVector& CellCm)
{
    if (!Mesh) Mesh = NewPart(this, RootComponent, Art(TEXT("SM_CargoDroneDock"), TEXT("/Engine/BasicShapes/Cube.Cube")));
    if (!Mesh->GetStaticMesh()) return;
    // Scale the mesh uniformly so its footprint fills its cells; its bounds' bottom centre on the actor origin (the
    // footprint's floor centre). Its X lies along the slot's X (a turned slot turns the actor).
    const FBox B = Mesh->GetStaticMesh()->GetBoundingBox();
    const double Scale = FSCCargoDockFootprint::Scale(B.GetSize(), Cells, CellCm);
    Mesh->SetRelativeScale3D(FVector(Scale));
    Mesh->SetRelativeLocation(-FVector(B.GetCenter().X, B.GetCenter().Y, B.Min.Z) * Scale);
    Mesh->SetCollisionProfileName(TEXT("BlockAll"));
    // A neutral fill at each bay's light socket (the drones are dark under the bay's ceiling; WP-K4a review). A viewer
    // choice, not game data: 8 cd, 1.2 m reach, no shadows.
    for (int32 I = 0; I < NumBays(); ++I)
    {
        const FName Socket(*FString::Printf(TEXT("BayLight_%d"), I));
        if (!Mesh->DoesSocketExist(Socket) || BayLights.IsValidIndex(I)) continue;
        UPointLightComponent* L = NewObject<UPointLightComponent>(this, NAME_None, RF_Transient);
        L->SetupAttachment(Mesh, Socket);
        L->SetIntensityUnits(ELightUnits::Candelas);
        L->SetIntensity(8.f);
        L->SetAttenuationRadius(120.f);
        L->SetLightColor(FLinearColor(1.f, 0.96f, 0.9f));
        L->SetCastShadows(false);
        AddInstanceComponent(L);
        L->RegisterComponent();
        BayLights.Add(L);
    }
}

int32 ASCCargoDroneDock::NumBays() const
{
    int32 N = 0;
    while (Mesh && Mesh->DoesSocketExist(FName(*FString::Printf(TEXT("Bay_%d"), N)))) ++N;
    return N;
}

FTransform ASCCargoDroneDock::BayTransform(int32 Bay) const
{
    const FName Socket(*FString::Printf(TEXT("Bay_%d"), Bay));
    if (Mesh && Mesh->DoesSocketExist(Socket))
    {
        // It faces out of its bay: from the bay towards the bay's exit, level in the dock (the bays may open on any side).
        const FVector At = Mesh->GetSocketLocation(Socket), Up = GetActorUpVector();
        const FName ExitSocket(*FString::Printf(TEXT("BayExit_%d"), Bay));
        const FVector Out = Mesh->DoesSocketExist(ExitSocket) ? FVector::VectorPlaneProject(Mesh->GetSocketLocation(ExitSocket) - At, Up) : FVector::ZeroVector;
        return FTransform(Out.SizeSquared() > 1.0 ? FRotationMatrix::MakeFromXZ(Out, Up).ToQuat() : GetActorQuat(), At);
    }
    const FBox B = Mesh ? Mesh->Bounds.GetBox() : FBox(GetActorLocation(), GetActorLocation());
    return FTransform(GetActorRotation(), FVector(B.GetCenter().X, B.GetCenter().Y, B.Max.Z + 30 + Bay * 50));
}

FVector ASCCargoDroneDock::BayExit(int32 Bay) const
{
    const FName Socket(*FString::Printf(TEXT("BayExit_%d"), Bay));
    if (Mesh && Mesh->DoesSocketExist(Socket)) return Mesh->GetSocketLocation(Socket);
    return BayTransform(Bay).GetLocation() + FVector(0, 0, 100);
}
