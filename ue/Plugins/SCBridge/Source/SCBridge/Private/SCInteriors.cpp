// Interior object containers as units (Galaxy-class ships): experiment commands that take a container out of the
// rendered and/or ray-traced scene, to measure what streaming containers by camera position would save.
// sc.Interior.Show <name|*> <0|1>: hide / restore every primitive under the matching container anchors.
// sc.Interior.RayTracing <name|*> <0|1>: drop / restore them from the ray-tracing scene (still rasterised).
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "SCVehicle.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCInteriors, Log, All);

namespace
{
// Imported state of a primitive before the first change, restored on 1.
TMap<TWeakObjectPtr<UPrimitiveComponent>, TPair<bool, bool>> GRestore;

void Apply(UWorld* World, const TArray<FString>& Args, bool bRayTracing)
{
    if (Args.Num() < 2) { UE_LOG(LogSCInteriors, Warning, TEXT("usage: <name|*> <0|1>")); return; }
    const bool bOn = FCString::Atoi(*Args[1]) != 0;
    int32 Containers = 0, Primitives = 0;
    for (TActorIterator<ASCVehicle> It(World); It; ++It)
        for (USceneComponent* Anchor : It->InteriorAnchors)
        {
            if (!Anchor || (Args[0] != TEXT("*") && !Anchor->GetName().Contains(Args[0]))) continue;
            ++Containers;
            TArray<USceneComponent*> Children;
            Anchor->GetChildrenComponents(true, Children);
            for (USceneComponent* C : Children)
                if (auto* P = Cast<UPrimitiveComponent>(C))
                {
                    const TPair<bool, bool>& Original = GRestore.FindOrAdd(P, TPair<bool, bool>(P->IsVisible(), bool(P->bVisibleInRayTracing)));
                    if (bRayTracing)
                    {
                        P->bVisibleInRayTracing = bOn && Original.Value;
                        P->MarkRenderStateDirty();
                    }
                    else
                        P->SetVisibility(bOn && Original.Key);
                    ++Primitives;
                }
        }
    UE_LOG(LogSCInteriors, Display, TEXT("INTERIOR_%s match=%s on=%d containers=%d primitives=%d"), bRayTracing ? TEXT("RT") : TEXT("SHOW"), *Args[0], bOn,
           Containers, Primitives);
}

FAutoConsoleCommandWithWorldAndArgs GSCInteriorShow(
    TEXT("sc.Interior.Show"), TEXT("sc.Interior.Show <name|*> <0|1>: hide / restore the primitives of interior containers (experiment)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) { Apply(World, Args, false); }));
FAutoConsoleCommandWithWorldAndArgs GSCInteriorRayTracing(
    TEXT("sc.Interior.RayTracing"), TEXT("sc.Interior.RayTracing <name|*> <0|1>: drop / restore interior containers from ray tracing (experiment)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) { Apply(World, Args, true); }));
}
