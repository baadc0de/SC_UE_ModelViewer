// WP-O1-4c-3a: the light records' JSON accessors and the MegaLights gate, shared by FSCLights and FSCLightImporter.
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "RHI.h"
#include "RenderUtils.h"
#include "SceneManagement.h"
namespace SCLightJson
{
using FLightJson = TSharedPtr<FJsonObject>;
inline FString LightString(const FLightJson& J, const TCHAR* K)
{
    FString S;
    J->TryGetStringField(K, S);
    return S;
}
inline double LightNumber(const FLightJson& J, const TCHAR* K, double Default = 0)
{
    double V;
    return J->TryGetNumberField(K, V) && FMath::IsFinite(V) ? V : Default;
}
inline FVector LightVector(const FLightJson& J, const TCHAR* K, FVector Default)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!J->TryGetArrayField(K, A) || A->Num() != 3) return Default;
    return FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
}
inline FQuat LightRotation(const FLightJson& J, const TCHAR* K)
{
    const TArray<TSharedPtr<FJsonValue>>* A;
    if (!J->TryGetArrayField(K, A) || A->Num() != 4) return FQuat::Identity;
    return FQuat((*A)[1]->AsNumber(), (*A)[2]->AsNumber(), (*A)[3]->AsNumber(), (*A)[0]->AsNumber()).GetNormalized();
}
inline FVector LightBasis(FVector V)
{
    return FVector(V.Y, V.X, V.Z);
}
// Shadows follow the game: only lights authored `shadowParams.shadowCasting` cast them (2-4 per ship), with or without
// MegaLights. WP-U3b shadowed every light under MegaLights as a stand-in for the game's per-room light confinement;
// WP-L1 confines lights to their rooms with lighting channels instead (SCLightAreas), so the stand-in is gone.
// Packages without the authored field keep the radius-quartile rule.
inline bool ConsoleEnabled(const TCHAR* Name)
{
    const auto* CVar = IConsoleManager::Get().FindConsoleVariable(Name);
    return CVar && CVar->GetInt() != 0;
}
inline bool MegaLightsActive()
{
    // Project-level Viewer policy; the Viewer PPV does not override MegaLights.
    // Match scalability, shader-platform and tracing gates, not just the project preference.
    if (GUsingNullRHI || !ConsoleEnabled(TEXT("r.MegaLights.EnableForProject")) || !ConsoleEnabled(TEXT("r.MegaLights.Allowed")) ||
        !ShouldCompileMegaLightsShaders(GMaxRHIShaderPlatform))
        return false;
    const bool Hardware =
        IsRayTracingEnabled() && ConsoleEnabled(TEXT("r.MegaLights.HardwareRayTracing")) &&
        (GRHISupportsRayTracingShaders || (GRHISupportsInlineRayTracing && ConsoleEnabled(TEXT("r.MegaLights.HardwareRayTracing.Inline"))));
    const bool Software = ConsoleEnabled(TEXT("r.MegaLights.SoftwareRayTracing.Allow")) && DoesProjectSupportDistanceFields();
    return Hardware || Software;
}
// The authored light model is present when the exporter wrote the WP-R5b fields.
inline bool HasAuthoredModel(const FLightJson& J)
{
    return J->HasField(TEXT("bulb_radius_m"));
}
} // namespace SCLightJson
