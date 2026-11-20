// Per-emitter ray reduction. Distances are supplied by the selected ray profile.
#pragma once

#include "CoreMinimal.h"

struct FSCAudioRayProfile
{
    float MinimumM = 0.f, MaximumM = 0.f, FullObstructionM = 0.f;

    bool IsConfigured() const
    {
        return FMath::IsFinite(MinimumM) && FMath::IsFinite(MaximumM) && FMath::IsFinite(FullObstructionM) &&
            MinimumM >= 0.f && MaximumM > MinimumM && FullObstructionM > 0.f;
    }

    bool Includes(double Distance) const
    {
        return IsConfigured() && Distance >= MinimumM && Distance <= MaximumM && Distance > 0.;
    }
};

struct FSCAudioRayLevels
{
    float Occlusion = 0.f, Obstruction = 0.f;

    static FSCAudioRayLevels Reduce(const FSCAudioRayProfile& Profile, double DistanceM, TConstArrayView<float> Rays)
    {
        if (!Profile.Includes(DistanceM) || Rays.Num() != 5) return {};
        float Outside = 0.f;
        for (int32 Index = 1; Index < Rays.Num(); ++Index) Outside += FMath::Clamp(Rays[Index], 0.f, 1.f);
        const float Occlusion = Outside / 4.f;
        const float Scale = float(FMath::Min(double(Profile.FullObstructionM) / DistanceM, 1.));
        const float Centre = FMath::Clamp(Rays[0], 0.f, 1.f) * Scale;
        return {Occlusion, FMath::Max(0.f, Centre - Occlusion)};
    }
};
