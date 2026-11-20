// Audio-thread one-pole filters for Wwise's authored 0–100 LPF/HPF levels.
#pragma once

#include "CoreMinimal.h"

struct SCBRIDGE_API FSCAudioFilterLevels
{
    float Lpf = 0.f;
    float Hpf = 0.f;

    void Add(const FString& Property, float Value);
    static bool IsVoiceFilter(const FString& Property);
    static bool IsOutputFilter(const FString& Property);
    static float LowPassHz(float Value);
    static float HighPassHz(float Value) { return LowPassHz(100.f - Value); }
};

class SCBRIDGE_API FSCAudioFilter
{
public:
    void Init(int32 Channels);
    void Process(float* Data, int32 Frames, float SampleRate, FSCAudioFilterLevels Levels);

private:
    TArray<FVector2f> History;
    FVector2f LastPole = FVector2f::ZeroVector;
    FVector2f LastWet = FVector2f::ZeroVector;
    bool bStarted = false;
};
