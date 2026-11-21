// Ship Link controls authored exhaust effects without changing the ship's movement.
#include "SCThrusterPreview.h"
#include "SCVehicle.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "SCFxContract.h"
#include "SCFxNames.h"
#include "SCMaterialBuilder.h"
#include "SCRigComponent.h"
#include "SCStarEngine.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Paths.h"
#include "SCViewerSubsystem.h"

namespace
{
void Stop(FSCThrusterEmitter& Emitter)
{
    if (!Emitter.Component) return;
    Emitter.Component->DeactivateImmediate();
    Emitter.Component->DestroyComponent();
    Emitter.Component = nullptr;
}

bool Start(FSCThrusterEmitter& Emitter, float Strength)
{
    if (!IsValid(Emitter.Mount)) return false;
    if (!IsValid(Emitter.Component))
    {
        const FString Path = SCFxNames::SystemPathForReference(Emitter.Source.Reference);
        UNiagaraSystem* Asset = Path.IsEmpty() ? nullptr : LoadObject<UNiagaraSystem>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
        if (!Asset)
        {
            UE_LOG(LogTemp, Warning, TEXT("THRUSTER_FX_MISSING reference=%s path=%s"), *Emitter.Source.Reference, *Path);
            return false;
        }
        SCFxContract::Check(Asset);
        const FTransform& Offset = Emitter.Source.Offset;
        Emitter.Component = UNiagaraFunctionLibrary::SpawnSystemAttached(
            Asset, Emitter.Mount, Emitter.Socket, Offset.GetLocation(), Offset.Rotator(), Offset.GetScale3D(),
            EAttachLocation::KeepRelativeOffset, false, ENCPoolMethod::None, false, false);
        if (Emitter.Component)
            UE_LOG(LogTemp, Verbose, TEXT("THRUSTER_FX_MOUNT reference=%s socket=%s origin=%s positive_axis=%s scale=%s"),
                *Emitter.Source.Reference, *Emitter.Socket.ToString(), *Emitter.Component->GetComponentLocation().ToCompactString(),
                *Emitter.Component->GetForwardVector().ToCompactString(), *Emitter.Component->GetComponentScale().ToCompactString());
    }
    if (!Emitter.Component) return false;
    Emitter.Component->SetVariableFloat(TEXT("User.SC.Strength"), Strength);
    if (!Emitter.Component->IsActive()) Emitter.Component->Activate(true);
    return true;
}

const TCHAR* ModeName(ESCThrusterPreviewMode Mode)
{
    switch (Mode)
    {
    case ESCThrusterPreviewMode::Off: return TEXT("off");
    case ESCThrusterPreviewMode::Minimum: return TEXT("minimum");
    case ESCThrusterPreviewMode::Full: return TEXT("full");
    case ESCThrusterPreviewMode::Boosted: return TEXT("boosted");
    }
    return TEXT("unknown");
}
}

bool ASCVehicle::SetThrusterPreview(ESCThrusterPreviewMode Mode)
{
    if (Thrusters.IsEmpty()) return false;
    ThrusterPreview = Mode;
    bThrusterPreviewActive = true;
    const bool bOff = Mode == ESCThrusterPreviewMode::Off;
    const bool bMinimum = Mode == ESCThrusterPreviewMode::Minimum;
    float DefaultGlow = 0;
    if (!bOff)
    {
        const FSCEngineGlow& Glow = Manifest.EngineGlow;
        DefaultGlow = FSCMaterialBuilder::EngineGlowToEmissive(Glow, bMinimum ? Glow.MinStrength : Glow.DefaultStrength);
    }
    for (UMaterialInstanceDynamic* MID : EngineGlowMaterials)
        if (MID) MID->SetScalarParameterValue(TEXT("EmissiveStrength"), DefaultGlow);

    int32 Active = 0;
    int32 BoostEffects = 0;
    int32 Missing = 0;
    int32 Animations = 0;
    int32 MainEmitters = 0, ManeuverEmitters = 0;
    double MainStrength = 0, ManeuverStrength = 0;
    for (FSCThrusterBinding& Thruster : Thrusters)
    {
        // A forward-thrust preview leaves lateral, vertical and reverse thrusters at their authored idle ends.
        const bool bIdle = bMinimum || Thruster.bManeuvering;
        const bool bBoost = !bIdle && Mode == ESCThrusterPreviewMode::Boosted &&
            Thruster.Emitters.ContainsByPredicate([](const FSCThrusterEmitter& Emitter) { return Emitter.Source.bBoost; });
        for (FSCThrusterEmitter& Emitter : Thruster.Emitters)
        {
            if (bOff || Emitter.Source.bBoost != bBoost) Stop(Emitter);
            else
            {
                const float Strength = float(bIdle ? Emitter.Source.Strength.X : Emitter.Source.Strength.Y);
                if (Start(Emitter, Strength))
                {
                    ++Active;
                    BoostEffects += Emitter.Source.bBoost;
                    if (Thruster.bManeuvering)
                    {
                        ++ManeuverEmitters;
                        ManeuverStrength += Strength;
                    }
                    else
                    {
                        ++MainEmitters;
                        MainStrength += Strength;
                    }
                }
                else ++Missing;
            }
        }

        if (Thruster.bHasGlow && !bOff)
        {
            const FVector& Glow = Thruster.Glow;
            const float Driver = float(bIdle ? Glow.X : (bBoost ? Glow.Z : Glow.Y));
            for (UMaterialInstanceDynamic* MID : Thruster.Materials)
                if (MID) MID->SetScalarParameterValue(TEXT("EmissiveStrength"), Driver * FSCStarEngine::Get().SceneUnitNits);
        }
        if (ClipCatalog && Thruster.Rig && !Thruster.NozzleAnimation.IsEmpty())
        {
            const FString Animation = FPaths::GetBaseFilename(Thruster.NozzleAnimation);
            const int32 ClipIndex = ClipCatalog->Clips.IndexOfByPredicate([&](const FSCImportedClip& Clip) {
                if (!Clip.Bindings.ContainsByPredicate([&](const FSCClipBinding& Binding) {
                    return Binding.Mesh == Thruster.Rig->GetSkeletalMeshAsset();
                })) return false;
                return Clip.Name.Equals(Animation, ESearchCase::IgnoreCase) ||
                    Clip.Fragments.ContainsByPredicate([&](const FSCClipFragment& Fragment) {
                        const FName Name(*Animation);
                        return Fragment.Fragment == Name || Fragment.AnimationNames.Contains(Name);
                    });
            });
            if (ClipIndex != INDEX_NONE && StartClip(ClipIndex, 0, true, !bOff && !bIdle, {}, Thruster.Rig, -1))
                ++Animations;
        }
    }
    UE_LOG(LogTemp, Display,
        TEXT("THRUSTER_PREVIEW mode=%s bindings=%d active=%d boosted=%d strength_sum=%g missing=%d nozzle_poses=%d main=%d main_strength=%g maneuver=%d maneuver_strength=%g"),
        ModeName(Mode), Thrusters.Num(), Active, BoostEffects, MainStrength + ManeuverStrength, Missing, Animations,
        MainEmitters, MainStrength, ManeuverEmitters, ManeuverStrength);
    return Missing == 0;
}

static FAutoConsoleCommandWithWorldAndArgs GSCThrusterPreview(
    TEXT("sc.Thrusters.Preview"), TEXT("sc.Thrusters.Preview <off|minimum|full|boosted>: preview the loaded ship's exhaust effects"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(World);
        ASCVehicle* Vehicle = Viewer ? Viewer->GetVehicle() : nullptr;
        if (!Vehicle || Args.Num() != 1) return;
        for (ESCThrusterPreviewMode Mode : {ESCThrusterPreviewMode::Off, ESCThrusterPreviewMode::Minimum,
            ESCThrusterPreviewMode::Full, ESCThrusterPreviewMode::Boosted})
            if (Args[0].Equals(ModeName(Mode), ESearchCase::IgnoreCase))
            {
                Vehicle->SetThrusterPreview(Mode);
                return;
            }
    }));
