// ASCCargoDrone (kinematic flight, rotors, tractor beam, soft physics-handle hold) and ASCCargoDroneDock (bays).
#include "Cargo/SCCargoDrone.h"
#include "Cargo/SCCargoFlightPath.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/PhysicsHandleComponent.h"

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

ASCCargoDrone::ASCCargoDrone()
{
    PrimaryActorTick.bCanEverTick = true;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent->SetMobility(EComponentMobility::Movable);
    Handle = CreateDefaultSubobject<UPhysicsHandleComponent>(TEXT("Handle"));
    // Soft hold (acceleration-mode drive, so the same for any mass): the box lags ~a/k behind the beam, sways at about
    // 1 Hz and settles (a tractor beam, not a rigid arm). Its sag under gravity, g/k, is lifted out by the carrier.
    Handle->bSoftLinearConstraint = true;
    Handle->bSoftAngularConstraint = true;
    Handle->LinearStiffness = HoldStiffness;
    Handle->LinearDamping = 7.f;
    Handle->AngularStiffness = 60.f;
    Handle->AngularDamping = 14.f;
    Handle->bInterpolateTarget = false;
}

void ASCCargoDrone::PostInitializeComponents()
{
    Super::PostInitializeComponents();
    if (Hull) return;
    UStaticMesh* HullMesh = Art(TEXT("SM_CargoDrone"), TEXT("/Engine/BasicShapes/Cube.Cube"));
    Hull = NewPart(this, RootComponent, HullMesh);
    const bool bArt = Hull->DoesSocketExist(TEXT("Tractor"));
    if (!bArt) Hull->SetRelativeScale3D(FVector(0.8, 0.8, 0.3));
    UStaticMesh* RotorMesh = Art(TEXT("SM_CargoDroneRotor"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
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
    Beam = NewPart(this, RootComponent, Art(TEXT("SM_TractorBeam"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
    Beam->SetCastShadow(false);
    Beam->SetVisibility(false);
    Light = NewObject<UPointLightComponent>(this, NAME_None, RF_Transient);
    Light->SetupAttachment(Hull, Hull->DoesSocketExist(TEXT("Light_Status")) ? FName(TEXT("Light_Status")) : NAME_None);
    Light->SetIntensityUnits(ELightUnits::Candelas);
    Light->SetIntensity(20.f);
    Light->SetAttenuationRadius(250.f);
    Light->SetLightColor(FLinearColor(0.3f, 0.8f, 1.f));
    Light->SetCastShadows(false);
    AddInstanceComponent(Light);
    Light->RegisterComponent();
}

void ASCCargoDrone::FlyTo(const FVector& InTarget, float Yaw, bool bInFace)
{
    Target = InTarget;
    TargetYaw = Yaw;
    bFace = bInFace;
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
        if (bFace) R.Yaw = FMath::FixedTurn(R.Yaw, TargetYaw, 90.f * Dt);
        // Lean into the acceleration (visual only): a few degrees, in the drone's own frame.
        const FVector Local = FRotator(0, R.Yaw, 0).UnrotateVector(Accel) / FMath::Max(MaxAccel, 1.f);
        R.Pitch = FMath::FInterpTo(R.Pitch, float(-Local.X * 8.0), Dt, 4.f);
        R.Roll = FMath::FInterpTo(R.Roll, float(Local.Y * 8.0), Dt, 4.f);
        SetActorLocationAndRotation(GetActorLocation() + Velocity * Dt, R);
    }
    RotorAngle = FMath::Fmod(RotorAngle + Dt * (State == ESCCargoDroneState::Docked ? 0.f : 2400.f), 360.f);
    for (int32 I = 0; I < Rotors.Num(); ++I)
        if (Rotors[I]) Rotors[I]->SetRelativeRotation(FRotator(0, I % 2 ? RotorAngle : -RotorAngle, 0));
}

FVector ASCCargoDrone::Emitter() const
{
    return Hull && Hull->DoesSocketExist(TEXT("Tractor")) ? Hull->GetSocketLocation(TEXT("Tractor")) : GetActorLocation() - FVector(0, 0, 20);
}

void ASCCargoDrone::Pose(const FVector& Location, const FVector& Accel, float Yaw, float DeltaSeconds)
{
    bFlying = false;
    const float Dt = FMath::Max(DeltaSeconds, 1e-3f);
    // Gusts: the controller holds the flight point within the sway (a wander of a few cm, a wobble of a few degrees).
    SwayGainNow = FMath::FInterpTo(SwayGainNow, SwayGain, Dt, 1.5f);
    const double T = GetWorld() ? GetWorld()->GetTimeSeconds() : 0;
    const uint32 Seed = GetTypeHash(DroneId) ^ 0x5eed;
    SwayNow = SwayGainNow > 1e-3f ? FSCCargoSway::Offset(Seed, T, SwayAmplitudeCm * SwayGainNow, SwayFrequencyHz) : FVector::ZeroVector;
    const FVector2D Wobble = SwayGainNow > 1e-3f ? FSCCargoSway::Tilt(Seed, T, SwayTiltDeg * SwayGainNow, SwayFrequencyHz) : FVector2D::ZeroVector;
    const FVector At = Location + SwayNow;
    Velocity = (At - GetActorLocation()) / Dt;
    FRotator R = GetActorRotation();
    R.Yaw = FMath::FixedTurn(R.Yaw, Yaw, 120.f * Dt);
    // Lean into the acceleration (visual): about 8 degrees at the empty drones' full thrust, plus the gusts' wobble.
    const FVector Local = FRotator(0, R.Yaw, 0).UnrotateVector(Accel) / FMath::Max(MaxAccel, 1.f);
    R.Pitch = FMath::FInterpTo(R.Pitch, float(FMath::Clamp(-Local.X * 8.0 + Wobble.X, -15.0, 15.0)), Dt, 5.f);
    R.Roll = FMath::FInterpTo(R.Roll, float(FMath::Clamp(Local.Y * 8.0 + Wobble.Y, -15.0, 15.0)), Dt, 5.f);
    SetActorLocationAndRotation(At, R);
}

void ASCCargoDrone::SetBeam(bool bOn, const FVector& Point, float WidthCm)
{
    if (!Beam) return;
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

void ASCCargoDrone::Grab(UPrimitiveComponent* Box, const FVector& At, const FRotator& Rotation)
{
    if (!Box) return;
    Handle->GrabComponentAtLocationWithRotation(Box, NAME_None, At, Rotation);
    Handle->SetTargetLocationAndRotation(At, Rotation);
}

void ASCCargoDrone::Hold(const FVector& At, const FRotator& Rotation) { Handle->SetTargetLocationAndRotation(At, Rotation); }

void ASCCargoDrone::Drop() { Handle->ReleaseComponent(); }

bool ASCCargoDrone::IsHolding() const { return Handle->GetGrabbedComponent() != nullptr; }

// ---------------------------------------------------------------------------------------------------- dock

ASCCargoDroneDock::ASCCargoDroneDock()
{
    PrimaryActorTick.bCanEverTick = false;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent->SetMobility(EComponentMobility::Movable);
}

void ASCCargoDroneDock::SetFootprint(const FVector& CellCm)
{
    if (!Mesh) Mesh = NewPart(this, RootComponent, Art(TEXT("SM_CargoDroneDock"), TEXT("/Engine/BasicShapes/Cube.Cube")));
    if (!Mesh->GetStaticMesh()) return;
    // Scale the mesh's footprint to one lattice cell; its bounds' bottom centre on the actor origin (the cell floor).
    const FBox B = Mesh->GetStaticMesh()->GetBoundingBox();
    const double Scale = CellCm.X / FMath::Max(B.GetSize().X, 1.0);
    Mesh->SetRelativeScale3D(FVector(Scale));
    Mesh->SetRelativeLocation(-FVector(B.GetCenter().X, B.GetCenter().Y, B.Min.Z) * Scale);
    Mesh->SetCollisionProfileName(TEXT("BlockAll"));
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
    if (Mesh && Mesh->DoesSocketExist(Socket)) return FTransform(GetActorRotation(), Mesh->GetSocketLocation(Socket));
    const FBox B = Mesh ? Mesh->Bounds.GetBox() : FBox(GetActorLocation(), GetActorLocation());
    return FTransform(GetActorRotation(), FVector(B.GetCenter().X, B.GetCenter().Y, B.Max.Z + 30 + Bay * 50));
}

FVector ASCCargoDroneDock::BayExit(int32 Bay) const
{
    const FName Socket(*FString::Printf(TEXT("BayExit_%d"), Bay));
    if (Mesh && Mesh->DoesSocketExist(Socket)) return Mesh->GetSocketLocation(Socket);
    return BayTransform(Bay).GetLocation() + FVector(0, 0, 100);
}
