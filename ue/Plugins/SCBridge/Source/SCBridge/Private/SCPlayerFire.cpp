// FSCPlayerFire / ASCShotFx: player weapon fire (see SCPlayerFire.h).
#include "SCPlayerFire.h"
#include "SCPlayerBody.h"
#include "SCRigComponent.h"
#include "SCVehicle.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCFire, Log, All);

namespace
{
// Engine basic shapes (cooked with the game: DefaultGame.ini DirectoriesToAlwaysCook) and the plugin's additive
// unlit master, whose EmissiveStrength is in the scene's cd/m2.
const TCHAR* const SphereMesh = TEXT("/Engine/BasicShapes/Sphere.Sphere");
const TCHAR* const CylinderMesh = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
const TCHAR* const GlowMaterial = TEXT("/SCBridge/Materials/M_SC_Emissive_Unlit.M_SC_Emissive_Unlit");
const FLinearColor FlashColour(1.0f, 0.62f, 0.3f);  // warm propellant flash (viewer look)
const FLinearColor TracerColour(1.0f, 0.85f, 0.55f); // pale streak, not a coloured beam

UPointLightComponent* MakeLight(AActor* Owner, const TCHAR* Name, float Candelas, float Radius)
{
    auto* Light = Owner->CreateDefaultSubobject<UPointLightComponent>(Name);
    Light->SetMobility(EComponentMobility::Movable); // placed at run time
    Light->SetIntensityUnits(ELightUnits::Candelas);
    Light->SetIntensity(Candelas);
    Light->SetAttenuationRadius(Radius);
    Light->SetLightColor(FlashColour);
    Light->SetCastShadows(false);
    Light->SetVisibility(false);
    return Light;
}
UStaticMeshComponent* MakeGlow(AActor* Owner, const TCHAR* Name, const TCHAR* Mesh)
{
    auto* Component = Owner->CreateDefaultSubobject<UStaticMeshComponent>(Name);
    Component->SetMobility(EComponentMobility::Movable);
    ConstructorHelpers::FObjectFinder<UStaticMesh> Found(Mesh);
    if (Found.Succeeded()) Component->SetStaticMesh(Found.Object);
    Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Component->SetCastShadow(false);
    Component->SetVisibility(false);
    return Component;
}
void Glow(UStaticMeshComponent* Mesh, const FLinearColor& Colour, float Strength)
{
    UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, GlowMaterial);
    if (!Base || !Mesh) return;
    UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, Mesh);
    Material->SetVectorParameterValue(TEXT("EmissiveColor"), Colour);
    Material->SetScalarParameterValue(TEXT("EmissiveStrength"), Strength);
    Mesh->SetMaterial(0, Material);
}
} // namespace

ASCShotFx::ASCShotFx()
{
    PrimaryActorTick.bCanEverTick = true;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
    GetRootComponent()->SetMobility(EComponentMobility::Movable);
    Flash = MakeLight(this, TEXT("Flash"), FlashCandelas, FlashRadius);
    Flash->SetupAttachment(GetRootComponent());
    FlashGlowMesh = MakeGlow(this, TEXT("FlashGlow"), SphereMesh);
    FlashGlowMesh->SetupAttachment(GetRootComponent());
    Tracer = MakeGlow(this, TEXT("Tracer"), CylinderMesh);
    Tracer->SetupAttachment(GetRootComponent());
    Impact = MakeLight(this, TEXT("Impact"), ImpactCandelas, FlashRadius);
    Impact->SetupAttachment(GetRootComponent());
}
void ASCShotFx::Launch(const FVector& Muzzle, const FVector& Target, bool bHit)
{
    From = Muzzle;
    To = Target;
    bHits = bHit;
    SetActorLocation(Muzzle);
    Flash->SetVisibility(true);
    // A small elongated glow along the barrel at the muzzle.
    const FVector Dir = (To - From).GetSafeNormal();
    FlashGlowMesh->SetWorldLocationAndRotation(Muzzle + Dir * 6, FRotationMatrix::MakeFromZ(Dir).Rotator());
    FlashGlowMesh->SetWorldScale3D(FVector(0.03, 0.03, 0.08));
    Glow(FlashGlowMesh, FlashColour, FlashGlow);
    FlashGlowMesh->SetVisibility(true);
    Glow(Tracer, TracerColour, TracerGlow);
    Impact->SetWorldLocation(To);
}
void ASCShotFx::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    Age += DeltaSeconds;
    if (Age > FlashSeconds)
    {
        Flash->SetVisibility(false);
        FlashGlowMesh->SetVisibility(false);
    }
    const FVector Line = To - From;
    const double Distance = Line.Size();
    const FVector Dir = Line.GetSafeNormal();
    if (!bLanded)
    {
        Flight += DeltaSeconds * TracerSpeed;
        const double Head = FMath::Min(double(Flight), Distance);
        const double Tail = FMath::Max(0.0, Head - TracerLength);
        if (Head - Tail > 1)
        {
            Tracer->SetWorldLocationAndRotation(From + Dir * ((Head + Tail) / 2), FRotationMatrix::MakeFromZ(Dir).Rotator());
            Tracer->SetWorldScale3D(FVector(TracerRadius * 2 / 100, TracerRadius * 2 / 100, (Head - Tail) / 100)); // shape: 100 cm
            Tracer->SetVisibility(true);
        }
        if (Flight >= Distance)
        {
            bLanded = true;
            Tracer->SetVisibility(false);
            Impact->SetVisibility(bHits);
            Flight = Age; // landing time
        }
    }
    else if (Age - Flight > ImpactSeconds)
        Destroy();
}

void FSCPlayerFire::Reset()
{
    for (FWeaponFire& W : Weapons)
        if (USCRigComponent* Rig = W.Rig.Get(); Rig && W.BindPose.Num())
        {
            Rig->LocalPose = W.BindPose;
            Rig->RefreshBoneTransforms();
        }
    Weapons.Reset();
    Firing = Stopping = FPlaying();
    Posed = INDEX_NONE;
    bTrigger = bWasFiring = false;
    ShotClock = 0;
}
void FSCPlayerFire::Attach(ASCVehicle* Kit, const FSCPlayerKit& Data)
{
    Reset();
    Weapons.SetNum(Data.Weapons.Num());
    for (int32 I = 0; I < Data.Weapons.Num(); ++I)
    {
        const FSCPlayerWeapon& Weapon = Data.Weapons[I];
        FWeaponFire& W = Weapons[I];
        W.Weapon = &Weapon;
        W.Anchor = Kit->InstanceAnchors.FindRef(Weapon.ChildId);
        if (!Weapon.FireActions.IsEmpty())
            if (const FSCInstanceHelpers* Helpers = Kit->InstanceHelpers.Find(Weapon.ChildId))
                for (const auto& Pair : Helpers->Frames)
                    if (Pair.Key.Equals(Weapon.FireActions[0].FireHelper, ESearchCase::IgnoreCase))
                    {
                        // Weapon helpers point their +Y along the line, as the sight helpers do (SightAxes: X along).
                        W.Launch = USCPlayerBodyComponent::SightAxes() * Pair.Value;
                        W.bLaunch = true;
                    }
        // The weapon's own rig: the kit rig of the weapon entity that carries the most of its fire clips' joints.
        int32 BestRouted = 0;
        for (USCRigComponent* Rig : Kit->Rigs)
        {
            if (!Rig || !Rig->OwnerEntity.Equals(Weapon.Entity, ESearchCase::IgnoreCase) || !Rig->GetSkeletalMeshAsset()) continue;
            const FReferenceSkeleton& Ref = Rig->GetSkeletalMeshAsset()->GetRefSkeleton();
            TMap<FName, TSharedPtr<FSCPlayerClip>> Clips;
            int32 Routed = 0;
            for (const auto& [Name, Json] : Weapon.FireClipJson)
            {
                auto Clip = MakeShared<FSCPlayerClip>();
                FString Error;
                if (!FSCPlayerClip::Parse(Json, [&](const FString& Joint) { return Ref.FindBoneIndex(FName(*Joint)); }, *Clip, Error)) continue;
                Clip->Name = Name;
                Routed += Clip->Tracks.Num();
                Clips.Add(Name, Clip);
            }
            if (!W.Rig.IsValid() || Routed > BestRouted)
            {
                W.Rig = Rig;
                W.Clips = MoveTemp(Clips);
                W.BindPose = Ref.GetRefBonePose();
                BestRouted = Routed;
            }
        }
        UE_LOG(LogSCFire, Display, TEXT("PLAYER_FIRE weapon=%s actions=%d fragments=%d rig=%s clips=%d routed_tracks=%d launch=%s"),
               *Weapon.Entity, Weapon.FireActions.Num(), Weapon.FireFragments.Num(), W.Rig.IsValid() ? *W.Rig->GetName() : TEXT("<none>"),
               W.Clips.Num(), BestRouted, W.bLaunch ? *Weapon.FireActions[0].FireHelper : TEXT("<none>"));
    }
}
void FSCPlayerFire::Pose(FWeaponFire& W, const FPlaying& Main, const FPlaying& Tail)
{
    USCRigComponent* Rig = W.Rig.Get();
    if (!Rig || W.BindPose.IsEmpty()) return;
    TArray<FTransform> Local = W.BindPose;
    for (const FPlaying* Playing : {&Main, &Tail})
        if (Playing->Fragment)
            for (const FSCWeaponFragment::FLayer& Layer : Playing->Fragment->Layers)
                if (const TSharedPtr<FSCPlayerClip>* Found = W.Clips.Find(Layer.Clip))
                {
                    const FSCPlayerClip& Clip = **Found;
                    float Time = Playing->Time * Layer.Speed;
                    Time = Layer.bLoop && Clip.Duration > 0 ? FMath::Fmod(Time, Clip.Duration) : FMath::Min(Time, Clip.Duration);
                    if (Clip.bAdditive) Clip.ApplyAdditive(Time, 1, Local);
                    else Clip.Sample(Time, Local);
                }
    Rig->LocalPose = MoveTemp(Local);
    Rig->RefreshBoneTransforms();
}
void FSCPlayerFire::Shoot(FWeaponFire& W, const FVector& AimOrigin, const FVector& AimDirection, UWorld* World, const TArray<const AActor*>& Ignore)
{
    ++Shots;
    USceneComponent* Anchor = W.Anchor.Get();
    if (!World || !Anchor || !W.bLaunch) return;
    const FVector Muzzle = (W.Launch * Anchor->GetComponentTransform()).GetLocation();
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCShot), true);
    for (const AActor* Actor : Ignore) Params.AddIgnoredActor(Actor);
    FHitResult Hit;
    const FVector End = AimOrigin + AimDirection * Range;
    const bool bHit = World->LineTraceSingleByChannel(Hit, AimOrigin, End, ECC_Visibility, Params);
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if (ASCShotFx* Fx = World->SpawnActor<ASCShotFx>(ASCShotFx::StaticClass(), FTransform(Muzzle), Spawn))
        Fx->Launch(Muzzle, bHit ? Hit.ImpactPoint : End, bHit);
}
FVector2D FSCPlayerFire::Step(float DeltaSeconds, int32 WeaponIndex, bool bAiming, const FVector& AimOrigin, const FVector& AimDirection,
                              UWorld* World, const TArray<const AActor*>& Ignore)
{
    FVector2D Kick = FVector2D::ZeroVector;
    if (Posed != INDEX_NONE && Posed != WeaponIndex && Weapons.IsValidIndex(Posed))
    {
        if (USCRigComponent* Rig = Weapons[Posed].Rig.Get())
        {
            Rig->LocalPose = Weapons[Posed].BindPose;
            Rig->RefreshBoneTransforms();
        }
        Posed = INDEX_NONE;
        Firing = Stopping = FPlaying();
    }
    FWeaponFire* W = Weapons.IsValidIndex(WeaponIndex) ? &Weapons[WeaponIndex] : nullptr;
    const FSCFireAction* Action = W && W->Weapon && !W->Weapon->FireActions.IsEmpty() ? &W->Weapon->FireActions[0] : nullptr;
    if (!Action)
    {
        bWasFiring = false;
        return Kick;
    }
    // A rapid action fires while held at its rate; any other kind fires once per press.
    const bool bAutomatic = Action->Kind.Contains(TEXT("Rapid"));
    const float Interval = Action->FireRate > 0 ? 60.f / Action->FireRate : 0.f;
    auto Shot = [&] {
        Shoot(*W, AimOrigin, AimDirection, World, Ignore);
        Kick += FVector2D(KickPitch, FMath::FRandRange(-KickYaw, KickYaw));
    };
    if (bTrigger && !bWasFiring)
    {
        Firing = {W->Weapon->FindFireFragment(Action->FireFragment, bAiming), 0};
        Stopping = FPlaying();
        ShotClock = 0;
        Shot();
        ShotClock = Interval;
    }
    else if (bTrigger && bAutomatic && Interval > 0)
    {
        ShotClock -= DeltaSeconds;
        for (int32 N = 0; ShotClock <= 0 && N < MaxShotsPerStep; ++N)
        {
            Shot();
            ShotClock += Interval;
        }
        ShotClock = FMath::Max(ShotClock, 0.f);
    }
    if (!bTrigger && bWasFiring && bAutomatic)
    {
        Firing = FPlaying();
        Stopping = {Action->StopFragment.IsNone() ? nullptr : W->Weapon->FindFireFragment(Action->StopFragment, bAiming), 0};
    }
    // The view comes back by what the shots kicked it once the trigger is released.
    Recoil += Kick;
    if (!bTrigger && !Recoil.IsZero())
    {
        const FVector2D Back = Recoil.GetSafeNormal() * FMath::Min(double(KickRecovery * DeltaSeconds), Recoil.Size());
        Recoil -= Back;
        Kick -= Back;
    }
    Firing.Time += DeltaSeconds;
    Stopping.Time += DeltaSeconds;
    Pose(*W, Firing, Stopping);
    Posed = WeaponIndex;
    bWasFiring = bTrigger;
    return Kick;
}
FString FSCPlayerFire::Describe() const
{
    return FString::Printf(TEXT("shots=%d trigger=%d firing=%s stopping=%s"), Shots, bTrigger,
                           Firing.Fragment ? *Firing.Fragment->Fragment.ToString() : TEXT("-"),
                           Stopping.Fragment ? *Stopping.Fragment->Fragment.ToString() : TEXT("-"));
}
