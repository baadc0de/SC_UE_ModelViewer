// USCCargoSubsystem::LogSections (sc.Cargo.Section): what surrounds a grid as the route queries see it, as text.
#include "Cargo/SCCargoSubsystem.h"
#include "SCCargoState.h"
#include "Cargo/SCCargoGridComponent.h"
#include "SCVehicle.h"
#include "SCViewerSubsystem.h"

void USCCargoSubsystem::LogSections(int32 GridId, double ReachM, double StepM) const
{
    // What is around a grid, as the route queries see it: a probe cube sampled on two sections through the lattice's
    // middle, each along a lattice axis out to ReachM beyond it on both sides: upright (rows top to bottom, from the
    // lattice's top + ReachM / 2 down to its floor - ReachM / 2) and level at mid height. '#' the probe overlaps
    // something, '.' free, '+' free inside the lattice, ':' free inside a vehicle's physics grid.
    const USCCargoGridComponent* G = FindGrid(GridId);
    if (!G) return;
    const double Step = FMath::Max(StepM, 0.25) * 100.0, Reach = ReachM * 100.0;
    const FSCCargoRouteQuery Probe = RouteQuery(FVector(Step / 2 - 5), FVector::ZeroVector, nullptr, -1e9, 1e9);
    const FTransform Frame = G->LatticeToWorld();
    const FVector Interior = G->Lattice.InteriorM * 100.0, Mid = Interior / 2;
    UE_LOG(LogSCCargo, Display, TEXT("CARGO_SECTION grid=%d step_m=%.2f reach_m=%.1f interior_m=%s origin_world=%s x_world=%s y_world=%s"), GridId, Step / 100, ReachM,
           *G->Lattice.InteriorM.ToCompactString(), *Frame.GetLocation().ToCompactString(), *Frame.GetUnitAxis(EAxis::X).ToCompactString(),
           *Frame.GetUnitAxis(EAxis::Y).ToCompactString());
    // ':' free and inside a vehicle's physics grid (there that vehicle's hull colliders are left out of cargo queries).
    TArray<ASCVehicle*> Vehicles;
    if (const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(GetWorld())) Vehicles = Viewer->GetVehicles();
    auto InGrid = [&Vehicles](const FVector& P) {
        for (const ASCVehicle* V : Vehicles)
            if (V && V->IsInPhysicsGrid(P)) return true;
        return false;
    };
    auto Section = [&](const TCHAR* Name, int32 Along, int32 Across) {
        // Local coordinates: U along the lattice axis Along (0 = x, 1 = y); V along Across (2 = z upright, else the other axis).
        const double U0 = -Reach, U1 = Interior[Along] + Reach;
        const double VReach = Across == 2 ? Reach / 2 : Reach;
        const double V0 = -VReach, V1 = Interior[Across] + VReach;
        for (double V = V1; V >= V0; V -= Step)
        {
            FString Row;
            for (double U = U0; U <= U1; U += Step)
            {
                FVector Local = Mid;
                Local[Along] = U;
                Local[Across] = V;
                const bool bInside = U >= 0 && U <= Interior[Along] && V >= 0 && V <= Interior[Across];
                const FVector World = Frame.TransformPosition(Local);
                Row.AppendChar(Probe.Blocked(World) ? TEXT('#') : bInside ? TEXT('+') : InGrid(World) ? TEXT(':') : TEXT('.'));
            }
            UE_LOG(LogSCCargo, Display, TEXT("CARGO_SECTION grid=%d %s %s=%7.2f |%s|"), GridId, Name, Across == 2 ? TEXT("z_m") : TEXT("v_m"), V / 100, *Row);
        }
    };
    Section(TEXT("upright_along_x"), 0, 2);
    Section(TEXT("upright_along_y"), 1, 2);
    Section(TEXT("level_x_by_y"), 0, 1);
}
