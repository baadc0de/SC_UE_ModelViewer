// WP-H2 camera sampling at the output frame time, independent of render latency.
#pragma once

#include "CoreMinimal.h"

namespace SCRenderPath
{
struct FPose
{
    FVector Position;
    FRotator Rotation;
};

inline double Progress(double Elapsed, double Duration, bool bEaseInOut)
{
    const double U = FMath::Clamp(Elapsed / Duration, 0.0, 1.0);
    return bEaseInOut ? U * U * (3.0 - 2.0 * U) : U;
}

inline FPose Orbit(const FVector& Center, double Radius, double Height, double StartDegrees,
                   double EndDegrees, double Progress)
{
    const double Angle = FMath::DegreesToRadians(FMath::Lerp(StartDegrees, EndDegrees, Progress));
    const FVector Position = Center + FVector(Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle), Height);
    return { Position, (Center - Position).Rotation() };
}

inline FPose Move(const FPose& From, const FPose& To, double Progress)
{
    return { FMath::Lerp(From.Position, To.Position, Progress),
             FRotator(FMath::Lerp(From.Rotation.Pitch, To.Rotation.Pitch, Progress),
                      FMath::Lerp(From.Rotation.Yaw, To.Rotation.Yaw, Progress),
                      FMath::Lerp(From.Rotation.Roll, To.Rotation.Roll, Progress)) };
}

inline FPose MoveLookingAt(const FVector& From, const FVector& To, const FVector& FromTarget,
                           const FVector& ToTarget, double Progress)
{
    const FVector Position = FMath::Lerp(From, To, Progress);
    const FVector Target = FMath::Lerp(FromTarget, ToTarget, Progress);
    return { Position, (Target - Position).Rotation() };
}
}
