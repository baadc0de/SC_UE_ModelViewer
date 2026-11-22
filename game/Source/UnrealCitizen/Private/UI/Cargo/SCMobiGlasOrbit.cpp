// FSCMobiGlasOrbit: view-target switch to the viewer's orbit pawn while the mobiGlas is open, and its drag / zoom.
#include "SCMobiGlasOrbit.h"
#include "SCFirstPersonCharacter.h"
#include "SCPlayerBody.h"
#include "SCVehicle.h"
#include "SCViewerPawn.h"
#include "SCViewerSubsystem.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

namespace
{
constexpr float BlendSeconds = 0.3f; // view-target blend on open / close (panel motion is 0.2 s)
constexpr float FramePitch = -12.f;  // the boom looks down at least this much (a low three-quarter view, below the hangar's
                                     // ceiling lights, which would otherwise dominate the exposure)
constexpr double BoomRadius = 40;     // spring-arm sweep radius (cm)
constexpr double MaxBoom = 4500;      // longest boom (cm); the focus may overfill the band rather than the camera go far
constexpr double MinBoom = 300, StepBack = 500; // shortest boom; how far beyond the known-good viewpoint the camera may go
constexpr double MinReach = 2000;     // ... but a viewpoint close to the focus does not hold the boom shorter than this
constexpr double WideFocus = 3000;    // a focus wider than this (cm) is a work area seen side-on
constexpr float WidePitch = -25.f;
}

ASCViewerPawn* FSCMobiGlasOrbit::Pawn() const
{
    if (OrbitPawn.IsValid()) return OrbitPawn.Get();
    return Viewer.IsValid() ? Viewer->GetViewerPawn() : nullptr; // orbit / fly modes: the possessed viewer pawn
}

void FSCMobiGlasOrbit::Begin(USCViewerSubsystem* InViewer, const FBox& Focus)
{
    if (bActive || !InViewer) return;
    Viewer = InViewer;
    bActive = true;
    ASCFirstPersonCharacter* C = InViewer->GetFirstPerson();
    UWorld* World = C ? C->GetWorld() : nullptr;
    APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
    if (!C || !PC)
    {
        if (ASCViewerPawn* P = Pawn(); P && P->GetCameraMode() == ESCCameraMode::Orbit && Focus.IsValid) Frame(P, Focus, P->GetActorLocation(), true);
        return;
    }
    // First person: view through the (un-possessed) viewer pawn the subsystem keeps for the inspection cameras.
    ASCViewerPawn* P = nullptr;
    for (TActorIterator<ASCViewerPawn> It(World); It && !P; ++It)
        if (!It->GetController()) P = *It;
    if (!P)
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        P = World->SpawnActor<ASCViewerPawn>(ASCViewerPawn::StaticClass(), C->Camera->GetComponentTransform(), Params);
        bSpawnedPawn = P != nullptr;
    }
    if (!P) { bActive = false; return; }
    OrbitPawn = P;
    Player = C;
    for (int32 Axis = 0; Axis < 3; ++Axis) P->SetMoveKey(Axis, 0);
    P->SetActorTickEnabled(true);
    P->SetCameraMode(ESCCameraMode::Orbit);
    // A three-quarter view of the focus from the player's side, swept like a spring arm (see Frame).
    const FVector Eye = C->Camera->GetComponentLocation();
    const FVector Centre = Focus.IsValid ? Focus.GetCenter() : C->GetActorLocation();
    // No pawn-side obstacle pull-in: it traces the hangar as one actor and dragged the camera into the hull; Frame's
    // own sweep keeps the boom clear (the subsystem sets the obstacle again whenever it changes camera mode).
    P->SetOrbitObstacle(nullptr);
    Frame(P, Focus.IsValid ? Focus : FBox::BuildAABB(Centre, FVector(150)), Eye, true);
    // The body seen from outside: third-person parts (head, helmet) on while orbiting.
    if (C->Body && C->Body->HasBody())
    {
        bPlayerWasThirdPerson = C->Body->IsThirdPerson();
        if (!bPlayerWasThirdPerson) C->Body->SetThirdPerson(true);
    }
    PlayerAt = C->GetActorLocation();
    PlayerView = PC->GetControlRotation();
    LastFocus = Focus;
    PC->SetViewTargetWithBlend(P, BlendSeconds, VTBlend_EaseInOut, 2);
    UE_LOG(LogTemp, Display, TEXT("MOBIGLAS_ORBIT begin focus=%s"), *Centre.ToCompactString());
}

void FSCMobiGlasOrbit::End()
{
    if (!bActive) return;
    bActive = false;
    ASCFirstPersonCharacter* C = Player.Get();
    ASCViewerPawn* P = OrbitPawn.Get();
    if (C)
    {
        if (C->Body && C->Body->HasBody() && !bPlayerWasThirdPerson) C->Body->SetThirdPerson(false);
        if (APlayerController* PC = C->GetWorld()->GetFirstPlayerController()) PC->SetViewTargetWithBlend(C, BlendSeconds, VTBlend_EaseInOut, 2);
    }
    if (P)
    {
        if (bSpawnedPawn) P->Destroy();
        else P->SetActorTickEnabled(false); // as the subsystem leaves its inspection pawn in first person
    }
    OrbitPawn.Reset();
    Player.Reset();
    bSpawnedPawn = false;
    UE_LOG(LogTemp, Display, TEXT("MOBIGLAS_ORBIT end"));
}

void FSCMobiGlasOrbit::Refocus(const FBox& Focus)
{
    if (Focus.IsValid) LastFocus = Focus;
    ASCViewerPawn* P = bActive ? Pawn() : nullptr;
    if (P && Focus.IsValid && P->GetCameraMode() == ESCCameraMode::Orbit) Frame(P, Focus, P->GetActorLocation(), false);
}
void FSCMobiGlasOrbit::Rotate(const FVector2D& Pixels)
{
    if (ASCViewerPawn* P = bActive ? Pawn() : nullptr) P->RotateDrag(Pixels);
}
void FSCMobiGlasOrbit::Pan(const FVector2D& Fraction)
{
    if (ASCViewerPawn* P = bActive ? Pawn() : nullptr) P->PanDrag(Fraction);
}
void FSCMobiGlasOrbit::Zoom(float Notches)
{
    if (ASCViewerPawn* P = bActive ? Pawn() : nullptr) P->Wheel(Notches);
}
void FSCMobiGlasOrbit::Sync()
{
    ASCFirstPersonCharacter* C = bActive ? Player.Get() : nullptr;
    ASCViewerPawn* P = OrbitPawn.Get();
    APlayerController* PC = C ? C->GetWorld()->GetFirstPlayerController() : nullptr;
    if (!C || !P || !PC) return;
    const FVector At = C->GetActorLocation();
    const FRotator View = PC->GetControlRotation();
    if (FVector::DistSquared(At, PlayerAt) < 5 * 5 && View.Equals(PlayerView, 0.5f)) return;
    // Moved from outside: show that viewpoint, orbiting the point ahead (at the focus distance).
    PlayerAt = At;
    PlayerView = View;
    const FVector Eye = At + FVector(0, 0, C->BaseEyeHeight);
    const double Pivot = LastFocus.IsValid ? FMath::Max(200.0, FVector::Dist(Eye, LastFocus.GetCenter())) : 1000.0;
    P->SetView(Eye, View, Pivot);
    UE_LOG(LogTemp, Display, TEXT("MOBIGLAS_ORBIT retarget eye=%s yaw=%.1f pitch=%.1f"), *Eye.ToCompactString(), View.Yaw, View.Pitch);
}
void FSCMobiGlasOrbit::Frame(ASCViewerPawn* P, const FBox& Focus, const FVector& From, bool bSnap)
{
    // The boom runs from the focus towards a viewpoint known to be inside the room (the player's eye on open, the
    // current camera on re-focus): hangar shells often carry no collision a sweep could stop at, so the camera never
    // goes further from the focus than that viewpoint (plus a step back) — never out through the roof or under the
    // floor. Within that, the distance fits the focus into the live band (the panels cover the rest), and a sweep on the
    // visibility and pawn channels from the focus' edge pulls the camera in front of a wall. Finally the view pitches
    // down so the focus sits in the band rather than at the screen centre.
    const double HalfH = FMath::DegreesToRadians(45.0), HalfV = FMath::Atan(FMath::Tan(HalfH) * 9.0 / 16.0);
    // Before the panels are first laid out the band is unknown: the layout's share (header ~8 %, panels from ~47 %).
    const FVector2D Use = Band.Y - Band.X > 0.05 && Band.X >= 0 ? Band : FVector2D(0.08, 0.47);
    const double Span = FMath::Clamp(Use.Y - Use.X, 0.2, 1.0), BandCentre = (Use.X + Use.Y) / 2;
    const double BandHalfV = FMath::Atan(Span * FMath::Tan(HalfV));
    const FVector Centre = Focus.GetCenter(), Size = Focus.GetSize();
    const double Across = FMath::Max(Size.X, Size.Y);
    FRotator Rot = (Centre - From).Rotation();
    if (Across > WideFocus)
    {
        // A wide work area (ship and elevator) runs along a diagonal of its box: look across it — perpendicular to a
        // diagonal — from the side nearer the room's middle (the environment's origin), so both ends show and the
        // walls stay behind the work instead of between the camera and it.
        const AActor* Room = Viewer.IsValid() ? Viewer->GetEnvironment() : nullptr;
        const FVector Middle = Room ? Room->GetActorLocation() : From;
        TArray<FVector2D> Axes = {FVector2D(-Size.Y, Size.X).GetSafeNormal(), FVector2D(Size.Y, Size.X).GetSafeNormal()};
        if (WorkAxis && !WorkAxis->IsNearlyZero()) Axes = {FVector2D(-WorkAxis->Y, WorkAxis->X).GetSafeNormal()}; // across the actual route
        double Nearest = TNumericLimits<double>::Max();
        for (const FVector2D& A : Axes)
            for (double Sign : {1.0, -1.0})
            {
                const FVector Look(A.X * Sign, A.Y * Sign, 0);
                const double ToMiddle = FVector::Dist2D(Centre - Look * FMath::Min(Across, MaxBoom), Middle);
                if (ToMiddle < Nearest) { Nearest = ToMiddle; Rot = FRotator(WidePitch, Look.Rotation().Yaw, 0); }
            }
    }
    Rot.Pitch = FMath::Clamp(Rot.Pitch, -45.f, FramePitch); // from above, never straight down onto the floor
    Rot.Roll = 0;
    const double Pr = FMath::DegreesToRadians(FMath::Abs(Rot.Pitch));
    const double HalfHeight = 0.5 * (Size.Z * FMath::Cos(Pr) + Across * FMath::Sin(Pr)), HalfWidth = 0.5 * Across;
    const bool bWide = Across > WideFocus;
    const double Depth = 0.5 * (bWide ? FMath::Min(Size.X, Size.Y) : Across) * FMath::Cos(Pr); // the near side is this much closer
    const double Fit = FMath::Max(HalfHeight / FMath::Tan(BandHalfV), HalfWidth / FMath::Tan(HalfH)) + Depth;
    const double Reach = bWide ? MaxBoom : FMath::Max(FVector::Dist(From, Centre) + StepBack, MinReach);
    double Distance = FMath::Clamp(Fit, MinBoom, FMath::Max(MinBoom, FMath::Min(Reach, MaxBoom)));
    // Spring arm: from the focus' edge (cargo, drones and the platform inside the focus are not obstacles).
    UWorld* World = P->GetWorld();
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SCMobiGlasBoom), false, P);
    if (ASCFirstPersonCharacter* C = Player.Get()) Params.AddIgnoredActor(C);
    if (Viewer.IsValid() && Viewer->GetVehicle()) Params.AddIgnoredActor(Viewer->GetVehicle());
    const double Edge = FMath::Min(Distance * 0.8, Focus.GetExtent().Size() + 100.0);
    FString HitName = TEXT("none");
    for (ECollisionChannel Channel : {ECC_Visibility, ECC_Pawn})
    {
        FHitResult Hit;
        if (World && World->SweepSingleByChannel(Hit, Centre - Rot.Vector() * Edge, Centre - Rot.Vector() * Distance, FQuat::Identity, Channel,
                                                 FCollisionShape::MakeSphere(BoomRadius), Params))
        {
            Distance = FMath::Max(MinBoom, Edge + Hit.Distance - BoomRadius);
            HitName = Hit.GetComponent() ? Hit.GetComponent()->GetName() : FString(TEXT("?"));
        }
    }
    const FVector Camera = Centre - Rot.Vector() * Distance;
    const double Lift = FMath::RadiansToDegrees(FMath::Atan((0.5 - BandCentre) * 2 * FMath::Tan(HalfV)));
    const FRotator View(FMath::Clamp(Rot.Pitch - float(Lift), -89.f, 89.f), Rot.Yaw, 0);
    P->SetView(Camera, View, Distance);
    UE_LOG(LogTemp, Display, TEXT("MOBIGLAS_ORBIT frame centre=%s from=%s camera=%s yaw=%.0f pitch=%.0f distance_cm=%.0f fit_cm=%.0f hit=%s band=%.2f..%.2f snap=%d"),
           *Centre.ToCompactString(), *From.ToCompactString(), *Camera.ToCompactString(), View.Yaw, View.Pitch, Distance, Fit, *HitName, Use.X, Use.Y, bSnap);
}
