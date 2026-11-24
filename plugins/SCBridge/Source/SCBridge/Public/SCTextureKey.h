// WP-O1-4c-3a: a material's texture slot as the materials name it (path and sampling role), split from
// SCTextureImporter.h so the material builder needs no texture decoder.
#pragma once
#include "CoreMinimal.h"

/** How a texture slot is sampled. Normal is a DDNA tangent-space map (always linear). For Color / Linear the
 *  role only decides the encoding of a PNG that does not state one (Color: sRGB, Linear: linear).
 *  DerivedRoughness is the exporter's PNG fallback for a DDNA smoothness map, r = sqrt(1 - smoothness): it is
 *  decoded back to the game's smoothness (1 - r^2, linear) before its mips are built, so the masters read the
 *  same quantity from it as from the DDS smoothness map (WP-T1). */
enum class ESCTextureRole : uint8 { Color, Normal, Linear, DerivedRoughness };

struct SCBRIDGE_API FSCTextureKey
{
    FString Path;
    ESCTextureRole Role = ESCTextureRole::Color;
    bool operator==(const FSCTextureKey& O) const { return Role == O.Role && Path == O.Path; }
    friend uint32 GetTypeHash(const FSCTextureKey& K) { return HashCombine(GetTypeHash(K.Path), uint32(K.Role)); }
};
