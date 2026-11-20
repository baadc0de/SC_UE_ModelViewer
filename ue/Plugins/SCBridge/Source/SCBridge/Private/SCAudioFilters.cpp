#include "SCAudioFilters.h"

#include "HAL/IConsoleManager.h"

namespace
{
TAutoConsoleVariable<int32> GFilters(TEXT("sc.Audio.Filters"), 1,
    TEXT("Apply the authored Wwise voice and bus low- and high-pass filters."));

float Pole(float Hz, float SampleRate)
{
    return FMath::Exp(-2.f * UE_PI * FMath::Min(Hz, SampleRate * 0.49f) / SampleRate);
}
}

bool FSCAudioFilterLevels::IsVoiceFilter(const FString& Property)
{
    return Property == TEXT("lpf") || Property == TEXT("hpf");
}

bool FSCAudioFilterLevels::IsOutputFilter(const FString& Property)
{
    return Property == TEXT("output_bus_lpf") || Property == TEXT("output_bus_hpf");
}

void FSCAudioFilterLevels::Add(const FString& Property, float Value)
{
    if (Property == TEXT("lpf") || Property == TEXT("output_bus_lpf"))
        Lpf += Value;
    else if (Property == TEXT("hpf") || Property == TEXT("output_bus_hpf"))
        Hpf += Value;
}

float FSCAudioFilterLevels::LowPassHz(float Value)
{
    // Wwise 2024.1's published filter scale (WP-A2, section 2.1). Fit the two ranges independently.
    const float X = FMath::Clamp(Value, 0.f, 100.f);
    if (X <= 30.f)
        return FMath::Lerp(20000.f, 7000.f, X / 30.f);
    return 7000.f * FMath::Pow(17.f / 7000.f, (X - 30.f) / 70.f);
}

void FSCAudioFilter::Init(int32 Channels)
{
    History.Init(FVector2f::ZeroVector, Channels);
    bStarted = false;
}

void FSCAudioFilter::Process(float* Data, int32 Frames, float SampleRate, FSCAudioFilterLevels Levels)
{
    if (Frames <= 0 || SampleRate <= 0.f || History.IsEmpty())
        return;
    const bool bEnabled = GFilters.GetValueOnAnyThread() != 0;
    const FVector2f Wet(bEnabled && Levels.Lpf > 0.f ? 1.f : 0.f,
                        bEnabled && Levels.Hpf > 0.f ? 1.f : 0.f);
    if (Wet.IsZero() && LastWet.IsZero())
        return;
    const FVector2f TargetPole(Pole(FSCAudioFilterLevels::LowPassHz(Levels.Lpf), SampleRate),
                              Pole(FSCAudioFilterLevels::HighPassHz(Levels.Hpf), SampleRate));
    if (!bStarted)
    {
        LastPole = TargetPole;
        LastWet = Wet;
        bStarted = true;
    }

    const FVector2f PoleStep = (TargetPole - LastPole) / float(Frames);
    const FVector2f WetStep = (Wet - LastWet) / float(Frames);
    for (int32 Frame = 0; Frame < Frames; ++Frame)
    {
        LastPole += PoleStep;
        LastWet += WetStep;
        for (FVector2f& State : History)
        {
            const float Input = *Data;
            State.X = FMath::Lerp(Input, State.X, LastPole.X);
            const float Low = FMath::Lerp(Input, State.X, LastWet.X);
            State.Y = FMath::Lerp(Low, State.Y, LastPole.Y);
            *Data++ = Low - LastWet.Y * State.Y;
        }
    }
    LastPole = TargetPole;
    LastWet = Wet;
}
