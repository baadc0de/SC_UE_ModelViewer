// Stereo output for one decoded source shared by multiple authored directions.
#pragma once

#include "CoreMinimal.h"

struct FSCAudioStereoDirections
{
    FVector2f PerSpeaker = FVector2f::ZeroVector;
    float Reference = 0.f;

    void Add(float Side, float LinearDistanceGain)
    {
        const float Angle = (FMath::Clamp(Side, -1.f, 1.f) + 1.f) * (UE_PI / 4.f);
        const FVector2f Direction(FMath::Max(0.f, FMath::Cos(Angle)), FMath::Max(0.f, FMath::Sin(Angle)));
        PerSpeaker.X = FMath::Max(PerSpeaker.X, Direction.X * LinearDistanceGain);
        PerSpeaker.Y = FMath::Max(PerSpeaker.Y, Direction.Y * LinearDistanceGain);
        Reference = FMath::Max(Reference, LinearDistanceGain);
    }

    FVector2f RelativeGains() const
    {
        return Reference > 0.f ? PerSpeaker / Reference : FVector2f::ZeroVector;
    }

    /** Apply a gain ramp and expand mono in place. The allocation already holds Frames * 2 floats. */
    static void Render(float* Data, int32 Frames, int32 SourceChannels, FVector2f From, FVector2f To)
    {
        for (int32 Frame = Frames - 1; Frame >= 0; --Frame)
        {
            const FVector2f Gain = FMath::Lerp(From, To, float(Frame + 1) / Frames);
            const float Left = Data[Frame * SourceChannels];
            const float Right = Data[Frame * SourceChannels + SourceChannels - 1];
            // Keep the stereo image where both channels share gain, and carry the remaining energy toward the
            // louder speaker using a normalized fold-down. At a hard pan either input remains audible.
            const float Shared = FMath::Min(Gain.X, Gain.Y);
            const float Combined = (Left + Right) * 0.5f;
            Data[Frame * 2] = Left * Shared + Combined * (Gain.X - Shared);
            Data[Frame * 2 + 1] = Right * Shared + Combined * (Gain.Y - Shared);
        }
    }
};
