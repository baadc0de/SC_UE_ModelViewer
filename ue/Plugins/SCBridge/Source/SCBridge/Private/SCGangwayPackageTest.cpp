// Optional actual-package test: compare authored free travel with contact on a tilted pad in a headless game world.
#include "SCVehicle.h"
#include "SCImporter.h"
#include "SCRigComponent.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "SCPackageSource.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/ScopeExit.h"
#include "PhysicsEngine/BodySetup.h"

namespace
{
// Keep the part of an actual posed collision triangle over a rectangular support. A linear height field
// reaches its minimum at a vertex of this clipped polygon, including newly introduced edge intersections.
void ClipFootprint(TArray<FVector>& Poly, const FBox& Box)
{
    for (int32 Axis = 0; Axis < 2; ++Axis)
        for (int32 Sign : {-1, 1})
        {
            if (Poly.IsEmpty()) return;
            const double Edge = Sign < 0 ? Box.Min[Axis] : Box.Max[Axis];
            TArray<FVector> Inside;
            FVector Previous = Poly.Last();
            double A = (Previous[Axis] - Edge) * Sign;
            for (const FVector P : Poly)
            {
                const double B = (P[Axis] - Edge) * Sign;
                if ((A <= 0) != (B <= 0)) Inside.Add(FMath::Lerp(Previous, P, A / (A - B)));
                if (B <= 0) Inside.Add(P);
                Previous = P;
                A = B;
            }
            Poly = MoveTemp(Inside);
        }
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCGangwayPackage, "SCBridge.Gangway.PackageContact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCGangwayPackage::RunTest(const FString&)
{
    FString Folder;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SCGangwayPackage="), Folder))
    {
        AddInfo(TEXT("Supply -SCGangwayPackage=<export root> for the package-level ground-contact test"));
        return true;
    }
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    World->InitializeActorsForPlay(FURL());
    World->BeginPlay();
    FSCFolderPackageSource Source(Folder);
    FSCImportOptions Options;
    Options.World = World;
    FSCImportResult Result;
    if (!TestTrue(TEXT("package imports"), FSCImporter::Import(Source, Options, Result))) return false;
    ASCVehicle* V = Result.Vehicle.Get();
    if (!TestNotNull(TEXT("vehicle"), V)) return false;
    int32 ContactCount = 0;
    for (int32 I = 0; I < V->ShipDoors.Num(); ++I)
    {
        const auto& Door = V->ShipDoors[I];
        if (!Door.bRamp) continue;
        const auto* Rig = V->Doors[I].Rig.Get();
        if (!TestTrue(TEXT("gangway carries a measurable walking surface"), Rig && !Door.GroundProbes.IsEmpty())) continue;
        const FQuat Tilt = FRotator(0,0,4).Quaternion();
        const FVector Normal = Tilt.RotateVector(FVector::UpVector);
        // Measure Chaos collision independently from the runtime probe reduction and predictive bone sampler.
        auto Lowest = [&](FVector* OutPoint = nullptr, const FBox* Footprint = nullptr) {
            double Min = TNumericLimits<double>::Max();
            TArray<USceneComponent*> Parts;
            Rig->GetChildrenComponents(true, Parts);
            for (USceneComponent* Part : Parts)
            {
                auto* Mesh = Cast<UStaticMeshComponent>(Part);
                if (!Mesh || !Mesh->IsQueryCollisionEnabled() || Mesh->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block) continue;
                UBodySetup* Body = Mesh->GetBodySetup();
                if (!Body) continue;
                for (const auto& Geometry : Body->TriMeshGeometries)
                    for (int32 T = 0; Geometry && T < Geometry->Elements().GetNumTriangles(); ++T)
                    {
                        Chaos::FTriangle Triangle;
                        int32 A,B,C;
                        Geometry->GetTransformedTriangle(T, Chaos::FRigidTransform3::Identity, Triangle, A, B, C);
                        TArray<FVector> Poly;
                        for (int32 K = 0; K < 3; ++K) Poly.Add(Mesh->GetComponentTransform().TransformPosition(FVector(Triangle[K])));
                        if (Footprint) ClipFootprint(Poly, *Footprint);
                        for (const FVector WorldPoint : Poly)
                        {
                            const double Height = Footprint ? WorldPoint.Z : FVector::DotProduct(Normal, WorldPoint);
                            if (Height >= Min) continue;
                            Min = Height;
                            if (OutPoint) *OutPoint = WorldPoint;
                        }
                    }
            }
            return Min;
        };
        auto Move = [&](bool bOpen) {
            if (!V->PlayDoor(I, bOpen)) return false;
            for (int32 Tick = 0; Tick < 150 && V->IsDoorPlaying(I); ++Tick) V->Tick(0.2f);
            return V->IsDoorOpen(I) == bOpen && !V->IsDoorPlaying(I);
        };
        if (!TestTrue(TEXT("close before measurement"), Move(false))) continue;
        const double Closed = Lowest();
        if (!TestTrue(TEXT("deploy to authored endpoint over empty ground"), Move(true))) continue;
        const double Free = Lowest();
        AddInfo(FString::Printf(TEXT("gangway=%d closed=%.2f free=%.2f sets=%d"), I, Closed, Free, Door.GroundProbes.Num()));
        if (!TestTrue(TEXT("opening lowers the ramp surface"), Free < Closed - 1.0)) continue;
        if (!TestTrue(TEXT("close clear gangway"), Move(false))) continue;
        const double Surface = FMath::Lerp(Free, Closed, 0.2);
        AActor* Pad = World->SpawnActor<AActor>();
        UBoxComponent* Box = NewObject<UBoxComponent>(Pad);
        Pad->SetRootComponent(Box);
        Box->SetMobility(EComponentMobility::Movable);
        Box->SetBoxExtent(FVector(20000,20000,20));
        Box->SetCollisionProfileName(TEXT("BlockAll"));
        Box->SetWorldTransform(FTransform(Tilt, Normal * (Surface - 20.)));
        Box->RegisterComponent();
        TestTrue(TEXT("opening stops in a stable open state"), Move(true));
        FVector ContactPoint = FVector::ZeroVector;
        const double Clearance = Lowest(&ContactPoint) - Surface;
        AddInfo(FString::Printf(TEXT("gangway=%d tilt=4deg surface=%.2f contact_clearance_cm=%.3f"), I, Surface, Clearance));
        TestTrue(TEXT("actual surface rests on the high side of the tilted pad"), Clearance >= -0.5 && Clearance <= 1.0);
        TestTrue(TEXT("close from the partial stroke"), Move(false));
        TestTrue(TEXT("closed geometry returns to reference"), FMath::IsNearlyEqual(Lowest(), Closed, 0.5));
        // A small raised patch at one edge; the opposite side has no supporting ground.
        const FVector Half(55,55,20);
        const FVector Top = ContactPoint + FVector(0,0,10);
        Box->SetBoxExtent(Half);
        Box->SetWorldTransform(FTransform(Top - FVector(0,0,Half.Z)), false, nullptr, ETeleportType::TeleportPhysics);
        const FBox Footprint(Top - Half, Top + Half);
        TestTrue(TEXT("opening detects the isolated step"), Move(true));
        const double StepClearance = Lowest(nullptr, &Footprint) - Top.Z;
        AddInfo(FString::Printf(TEXT("gangway=%d one_sided_step_clearance_cm=%.3f"), I, StepClearance));
        TestTrue(TEXT("supported probes stay above the local step"), StepClearance >= -0.5 && StepClearance <= 1.0);
        TestTrue(TEXT("close from one-sided support"), Move(false));
        Pad->Destroy();
        ++ContactCount;
    }
    TestTrue(TEXT("exercised a lowering gangway"), ContactCount > 0);
    return true;
}
