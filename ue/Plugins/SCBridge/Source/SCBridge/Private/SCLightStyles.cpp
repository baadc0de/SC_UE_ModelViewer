// Playback of the renderer's periodic, sampled light style presets.
#include "SCLightImporter.h"
#include "SCVehicle.h"
#include "Components/LightComponent.h"
#include "Engine/World.h"

FVector3f FSCLightStyleWaveform::Sample(double TimeSeconds, double Speed, double Phase) const
{
    if (Samples.IsEmpty()) return FVector3f(1);
    if (Samples.Num() == 1) return Samples[0];

    // LIVE adds (phase byte / 255) * TimeIncr to speed-scaled time.
    // It then scales the result by TimeIncr and interpolates across the loop boundary.
    const double PhaseFraction = Phase / 255.;
    const double Position = (TimeSeconds * Speed + PhaseFraction * SamplesPerSecond) * SamplesPerSecond;
    if (!FMath::IsFinite(Position)) return FVector3f(1);
    const double Wrapped = FMath::Fmod(Position, double(Samples.Num()));
    const double Frame = Wrapped < 0 ? Wrapped + Samples.Num() : Wrapped;
    const int32 First = FMath::FloorToInt(Frame);
    return FMath::Lerp(Samples[First % Samples.Num()], Samples[(First + 1) % Samples.Num()], float(Frame - First));
}

namespace
{
void ApplyPhotometry(const FSCImportedLightState& State, const FVector3f& Multiplier)
{
    ULightComponent* Light = State.Component;
    if (!Light) return;
    const float Peak = Multiplier.GetMax();
    // Keep a fully dark sample resident in the light scene. A zero candela setter
    // would unregister and re-register the light on every on/off edge.
    const float Scale = Peak > 0 ? Peak : 1;
    const FVector3f Tint = Multiplier / Scale;
    Light->SetLightColor(State.BaseColor * FLinearColor(Tint.X, Tint.Y, Tint.Z));
    Light->SetIntensity(State.bEnabled ? float(State.BaseCandela * State.WallLightScale * State.FloorWashScale * Scale) : 0.f);
}

void SampleAndApply(FSCImportedLightState& State, const FSCLightStyleWaveform* Style, double Now)
{
    State.StyleSampleSeconds = Now;
    ApplyPhotometry(State, Style ? Style->Sample(Now, State.AnimationSpeed, State.AnimationPhase) : FVector3f(1));
}
}
void FSCLightImporter::ApplyStyle(ASCVehicle* Vehicle, int32 Index)
{
    FSCImportedLightState& State = Vehicle->LightStates[Index];
    const FSCLightStyleWaveform* Style = State.bEnabled ? Vehicle->Manifest.LightStyles.Find(State.LightStyleId) : nullptr;
    const bool bHadLoops = !Vehicle->LoopingLights.IsEmpty();
    if (Style && Style->Samples.Num() > 1 && State.AnimationSpeed != 0)
        Vehicle->LoopingLights.Add(Index);
    else
        Vehicle->LoopingLights.Remove(Index);
    if (bHadLoops != !Vehicle->LoopingLights.IsEmpty()) Vehicle->UpdateTickEnabled();

    const double Now = Vehicle->GetWorld() ? Vehicle->GetWorld()->GetTimeSeconds() : 0;
    SampleAndApply(State, Style, Now);
    State.Component->SetVisibility(State.bEnabled);
}

void FSCLightImporter::TickStyles(ASCVehicle* Vehicle)
{
    if (Vehicle->LoopingLights.IsEmpty()) return;
    const double Now = Vehicle->GetWorld()->GetTimeSeconds();
    for (int32 Index : Vehicle->LoopingLights)
    {
        FSCImportedLightState& State = Vehicle->LightStates[Index];
        const auto& Style = Vehicle->Manifest.LightStyles.FindChecked(State.LightStyleId);
        SampleAndApply(State, &Style, Now);
    }
}

void FSCLightImporter::ResumeStyles(ASCVehicle* Vehicle)
{
    if (Vehicle->Manifest.LightStyles.IsEmpty()) return;
    Vehicle->LoopingLights.Reset();
    for (int32 Index = 0; Index < Vehicle->LightStates.Num(); ++Index)
    {
        const FSCImportedLightState& State = Vehicle->LightStates[Index];
        if (IsValid(State.Component) && Vehicle->Manifest.LightStyles.Contains(State.LightStyleId))
            ApplyStyle(Vehicle, Index);
    }
    Vehicle->UpdateTickEnabled();
}

bool FSCLightImporter::SetIntensityScale(ASCVehicle* Vehicle, ULightComponent* Light, ESCLightIntensityScale Layer, float Scale)
{
    if (!Vehicle || !Light || Vehicle->Manifest.LightStyles.IsEmpty()) return false;
    const int32 Index = Vehicle->LightStates.IndexOfByPredicate([Light](const FSCImportedLightState& State) {
        return State.Component == Light;
    });
    if (Index == INDEX_NONE) return false;

    FSCImportedLightState& State = Vehicle->LightStates[Index];
    float& LayerScale = Layer == ESCLightIntensityScale::Wall ? State.WallLightScale : State.FloorWashScale;
    LayerScale = FMath::IsFinite(Scale) ? FMath::Max(0.f, Scale) : 1.f;
    ApplyStyle(Vehicle, Index);
    return true;
}
