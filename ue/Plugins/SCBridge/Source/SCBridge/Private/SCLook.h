// Look switches shared by the console commands (SCLook.cpp) and the viewer's startup defaults.
#pragma once
#include "CoreMinimal.h"

class UWorld;

namespace SCLook
{
// FFT (convolution) bloom on every unbound post-process volume with a generated kernel: a sharp peak plus <Points>
// diffraction streaks of <Length> (fraction of the kernel radius); Points 0 restores the standard bloom.
void SetBloomStar(UWorld* World, int32 Points, float Length, float Intensity);
}
