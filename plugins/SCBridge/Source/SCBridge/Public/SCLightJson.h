// WP-O1-4c-3a: the light records' JSON accessors and the MegaLights gate, shared by FSCLights and FSCLightImporter
// (defined in SCLights.cpp).
#pragma once
#include "CoreMinimal.h"
class FJsonObject;
namespace SCLightJson
{
using FLightJson = TSharedPtr<FJsonObject>;
SCBRIDGE_API FString LightString(const FLightJson& J, const TCHAR* K);
SCBRIDGE_API double LightNumber(const FLightJson& J, const TCHAR* K, double Default = 0);
SCBRIDGE_API FVector LightVector(const FLightJson& J, const TCHAR* K, FVector Default);
SCBRIDGE_API FQuat LightRotation(const FLightJson& J, const TCHAR* K);
SCBRIDGE_API FVector LightBasis(FVector V);
// Shadows follow the game: only lights authored `shadowParams.shadowCasting` cast them (2-4 per ship), with or without
// MegaLights. WP-U3b shadowed every light under MegaLights as a stand-in for the game's per-room light confinement;
// WP-L1 confines lights to their rooms with lighting channels instead (SCLightAreas), so the stand-in is gone.
// Packages without the authored field keep the radius-quartile rule.
SCBRIDGE_API bool ConsoleEnabled(const TCHAR* Name);
SCBRIDGE_API bool MegaLightsActive();
// The authored light model is present when the exporter wrote the WP-R5b fields.
SCBRIDGE_API bool HasAuthoredModel(const FLightJson& J);
} // namespace SCLightJson
