// Dev console commands to preview the game effects built by SCFxBuild (WP-K4c): sc.Fx.Spawn / sc.Fx.Clear.
//   sc.Fx.Spawn <asset path> [x y z] [every=<s>] [dist=|right=|up=<cm>] [pitch=|yaw=|roll=<deg>] [rel=1] [scale=<s>] [sx=<s>]
// spawns a Niagara system at x y z (cm), or dist cm (default 300) in front of the camera, offset right / up. The
// system's +X is the game emitter's forward (the emission axis, the beam direction); by default it points up
// (pitch=90), as an impact on a floor; rel=1 makes the angles camera-relative. every=<s> re-triggers it every s
// real seconds (bursts are short); sx scales X only. The SCFxBuild runtime contract: strength=<0..1> sets
// User.SC.Strength; len=<cm> sets a guided beam's User.SC.BeamLength / BeamEnd / BeamStartTangent (the beam leaves
// along +X), bend=<deg> turns its end about the component's Z so the beam curves.
#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "HAL/IConsoleManager.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogSCFxPreview, Log, All);

namespace
{
TArray<TWeakObjectPtr<UNiagaraComponent>> GSpawned;
TArray<FTSTicker::FDelegateHandle> GTickers;

void Clear()
{
    for (const FTSTicker::FDelegateHandle& T : GTickers) FTSTicker::RemoveTicker(T);
    GTickers.Reset();
    int32 Removed = 0;
    for (const TWeakObjectPtr<UNiagaraComponent>& C : GSpawned)
        if (C.IsValid())
        {
            C->DestroyComponent();
            ++Removed;
        }
    GSpawned.Reset();
    UE_LOG(LogSCFxPreview, Display, TEXT("SC_FX_CLEAR removed=%d"), Removed);
}

void Spawn(const TArray<FString>& Args, UWorld* World)
{
    if (!World || Args.IsEmpty())
    {
        UE_LOG(LogSCFxPreview, Warning, TEXT("usage: sc.Fx.Spawn <asset path> [x y z] [every=s] [dist=cm] [pitch=|yaw=|roll=deg] [scale=s] [sx=s]"));
        return;
    }
    FString Path = Args[0];
    if (!Path.Contains(TEXT("."))) Path += TEXT(".") + FPaths::GetBaseFilename(Path);
    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Path);
    if (!System)
    {
        UE_LOG(LogSCFxPreview, Warning, TEXT("SC_FX_SPAWN no Niagara system %s"), *Path);
        return;
    }
    TArray<double> Numbers;
    TMap<FString, double> Keys;
    for (int32 i = 1; i < Args.Num(); ++i)
    {
        FString K, V;
        if (Args[i].Split(TEXT("="), &K, &V)) Keys.Add(K.ToLower(), FCString::Atod(*V));
        else if (Args[i].IsNumeric()) Numbers.Add(FCString::Atod(*Args[i]));
    }
    auto Key = [&Keys](const TCHAR* Name, double Default) {
        const double* V = Keys.Find(Name);
        return V ? *V : Default;
    };
    APlayerController* PC = World->GetFirstPlayerController();
    const bool bCamera = PC && PC->PlayerCameraManager;
    const FRotator CamRot = bCamera ? PC->PlayerCameraManager->GetCameraRotation() : FRotator::ZeroRotator;
    FVector At;
    if (Numbers.Num() >= 3) At = FVector(Numbers[0], Numbers[1], Numbers[2]);
    else if (bCamera)
    {
        // dist ahead, right / up across the view (cm).
        const FRotationMatrix M(CamRot);
        At = PC->PlayerCameraManager->GetCameraLocation() + M.GetUnitAxis(EAxis::X) * Key(TEXT("dist"), 300.0) +
             M.GetUnitAxis(EAxis::Y) * Key(TEXT("right"), 0.0) + M.GetUnitAxis(EAxis::Z) * Key(TEXT("up"), 0.0);
    }
    else
    {
        UE_LOG(LogSCFxPreview, Warning, TEXT("SC_FX_SPAWN no camera: pass x y z"));
        return;
    }
    // rel=1: pitch / yaw / roll relative to the camera (yaw=90 lays a beam's +X across the view, left to right).
    FRotator Rot(Key(TEXT("pitch"), 90.0), Key(TEXT("yaw"), 0.0), Key(TEXT("roll"), 0.0));
    if (Key(TEXT("rel"), 0.0) != 0) Rot = FRotator(CamRot.Quaternion() * Rot.Quaternion());
    const double Uniform = Key(TEXT("scale"), 1.0);
    const FVector Scale(Key(TEXT("sx"), Uniform), Uniform, Uniform);
    UNiagaraComponent* C = UNiagaraFunctionLibrary::SpawnSystemAtLocation(World, System, At, Rot, Scale, /*bAutoDestroy*/ false,
                                                                          /*bAutoActivate*/ true, ENCPoolMethod::None, /*bPreCullCheck*/ false);
    if (!C)
    {
        UE_LOG(LogSCFxPreview, Warning, TEXT("SC_FX_SPAWN %s failed"), *Path);
        return;
    }
    if (const double* Strength = Keys.Find(TEXT("strength"))) C->SetVariableFloat(TEXT("SC.Strength"), float(*Strength));
    if (const double* Len = Keys.Find(TEXT("len")))
    {
        const double Bend = FMath::DegreesToRadians(Key(TEXT("bend"), 0.0));
        C->SetVariableFloat(TEXT("SC.BeamLength"), float(*Len));
        C->SetVariableVec2(TEXT("SC.BeamLength2"), FVector2D(*Len, *Len));
        C->SetVariableVec3(TEXT("SC.BeamEnd"), FVector(FMath::Cos(Bend), FMath::Sin(Bend), 0.0) * *Len);
        C->SetVariableVec3(TEXT("SC.BeamStartTangent"), FVector(*Len, 0.0, 0.0));
    }
    GSpawned.Add(C);
    const double Every = Key(TEXT("every"), 0.0);
    if (Every > 0)
    {
        TWeakObjectPtr<UNiagaraComponent> Weak(C);
        GTickers.Add(FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float) {
            if (!Weak.IsValid()) return false;
            Weak->ResetSystem();
            return true;
        }), float(Every)));
    }
    UE_LOG(LogSCFxPreview, Display, TEXT("SC_FX_SPAWN %s at=%s rot=%s scale=%s every=%.2f"), *Path, *At.ToCompactString(), *Rot.ToCompactString(),
           *Scale.ToCompactString(), Every);
}

FAutoConsoleCommandWithWorldAndArgs GSCFxSpawn(
    TEXT("sc.Fx.Spawn"),
    TEXT("sc.Fx.Spawn <asset path> [x y z] [every=s] [dist=cm] [pitch=|yaw=|roll=deg] [scale=s] [sx=s]: preview a Niagara system (WP-K4c)"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Spawn));
FAutoConsoleCommand GSCFxClear(TEXT("sc.Fx.Clear"), TEXT("Remove the systems sc.Fx.Spawn placed"), FConsoleCommandDelegate::CreateStatic(&Clear));
} // namespace
