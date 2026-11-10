// FSCTextureImporter: exported PNG -> transient UTexture2D. Colour encoding comes from the PNG (the exporter
// copies the source DDS's sRGB state into an sRGB / gAMA chunk); the slot role is the fallback and sampler hint.
// Decode (and CPU mip chain) is pure and runs on worker threads; Create runs on the game thread.
#pragma once
#include "CoreMinimal.h"
#include "SCPackageSource.h"

class UTexture2D;

/** How a texture slot is sampled. Normal is a DDNA tangent-space map (always linear). For Color / Linear the
 *  role only decides the encoding of a PNG that does not state one (Color: sRGB, Linear: linear). */
enum class ESCTextureRole : uint8 { Color, Normal, Linear };

struct SCBRIDGE_API FSCTextureKey
{
    FString Path;
    ESCTextureRole Role = ESCTextureRole::Color;
    bool operator==(const FSCTextureKey& O) const { return Role == O.Role && Path == O.Path; }
    friend uint32 GetTypeHash(const FSCTextureKey& K) { return HashCombine(GetTypeHash(K.Path), uint32(K.Role)); }
};

/** Pure CPU result: BGRA8 mip chain, largest first. */
struct SCBRIDGE_API FSCDecodedTexture
{
    FSCTextureKey Key;
    int32 Width = 0, Height = 0;
    TArray<TArray64<uint8>> Mips;
    bool bSRGB = false; // resolved encoding of the RGB channels (ResolveSRGB)
    FString Error;
};

struct SCBRIDGE_API FSCTextureStats
{
    int32 Requested = 0, Created = 0, Failed = 0;
    int64 Bytes = 0;
    double DecodeSeconds = 0, CreateSeconds = 0;
};

/** Validation before any decompression. Dimension limit is the RHI's; the byte limits are host budgets
 *  (fallback register, ue/verification/WP-U2.md). Decoded bytes count BGRA8 plus the full mip chain. */
struct SCBRIDGE_API FSCTextureLimits
{
    uint32 MaxDimension = 0;          // 0: GetMax2DTextureDimension()
    int64 MaxTextureBytes = 256ll << 20;
    int64 BudgetBytes = 2048ll << 20; // all textures of one import
    static FSCTextureLimits FromHost();
};

class SCBRIDGE_API FSCTextureImporter
{
public:
    /** Decode one PNG. Normal maps convert the glTF/OpenGL (+Y) tangent convention to UE's (-Y),
     *  as Interchange's glTF translator does; the DDNA smoothness alpha is kept untouched. */
    static bool Decode(const FSCPackageSource& Source, const FSCTextureKey& Key, FSCDecodedTexture& Out,
        const FSCTextureLimits& Limits = FSCTextureLimits::FromHost());
    /** Header pass (parallel) -> per-texture and aggregate budget in key order -> parallel decode of the
     *  accepted keys. Rejected keys carry an Error and are never decompressed. */
    static TArray<FSCDecodedTexture> DecodeAll(const FSCPackageSource& Source, const TArray<FSCTextureKey>& Keys,
        const FSCTextureLimits& Limits = FSCTextureLimits::FromHost());
    /** Encoding of a PNG's RGB channels: an `sRGB` chunk means sRGB, `gAMA` 1.0 means linear (both written by
     *  the exporter from the DDS header); a PNG without either falls back to the role. Normal is always linear. */
    static bool ResolveSRGB(TConstArrayView<uint8> Png, ESCTextureRole Role);
    /** Linear-light mean RGB of the top mip (sRGB-encoded texels are linearized first); false without texels. */
    static bool MeanColor(const FSCDecodedTexture& Texture, FLinearColor& Out);
    /** Decoded size of a W x H BGRA8 texture with its full mip chain. */
    static int64 DecodedBytes(int64 Width, int64 Height);
    /** Game thread: transient texture owned by Outer; uncompressed BGRA8, never streamed. */
    static UTexture2D* Create(FSCDecodedTexture&& Decoded, UObject* Outer);
    /** Box-filtered mip chain down to 1x1 (exported PNGs carry a single level); sRGB-encoded RGB (bSRGB) is
     *  averaged in linear space, alpha / linear data / normals as stored. */
    static void BuildMips(FSCDecodedTexture& Texture);
};
