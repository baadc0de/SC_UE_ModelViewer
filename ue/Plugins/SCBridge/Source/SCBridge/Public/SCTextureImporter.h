// FSCTextureImporter: exported texture -> transient UTexture2D. A package DDS (WP-T1) carries the game's own
// blocks and mip chain; they are uploaded unchanged in the matching pixel format, its encoding read from the DXGI
// format. A PNG (exporter fallback, lights) is decoded to BGRA8 with a CPU mip chain; its encoding comes from the
// PNG's sRGB / gAMA chunk, the slot role is the fallback. Decode is pure and runs on worker threads; Create runs on
// the game thread.
#pragma once
#include "CoreMinimal.h"
#include "SCPackageSource.h"

class UTexture2D;

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

/** Pure CPU result: a PNG's BGRA8 mip chain (largest first), or a package DDS's blocks as prepared for UE. */
struct SCBRIDGE_API FSCDecodedTexture
{
    FSCTextureKey Key;
    int32 Width = 0, Height = 0;
    TArray<TArray64<uint8>> Mips;
    bool bSRGB = false; // resolved encoding of the RGB channels (ResolveSRGB / the DXGI format)
    /** Package DDS file after sc_dds_prepare (SNORM blocks re-biased, normal green negated); empty for a PNG. */
    TArray<uint8> Dds;
    uint8 PixelFormat = 0;  // EPixelFormat of Dds (PF_Unknown for a PNG, which is PF_B8G8R8A8)
    int32 MipCount = 0;     // mips in Dds
    int64 DataOffset = 0;   // first mip in Dds
    FString Error;
    bool IsBlocks() const { return !Dds.IsEmpty(); }
    /** Bytes uploaded to the GPU: the DDS mip data, or the BGRA8 mips. */
    int64 UploadBytes() const;
};

struct SCBRIDGE_API FSCTextureStats
{
    int32 Requested = 0, Created = 0, Failed = 0;
    int64 Bytes = 0;
    double DecodeSeconds = 0, CreateSeconds = 0;
};

/** Validation before any decompression. Dimension limit is the RHI's; the byte limits are host budgets
 *  (fallback register, ue/verification/WP-U2.md) on the bytes uploaded: a DDS's mip data as stored, a PNG's BGRA8
 *  plus its full mip chain. */
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
    /** Decode one package texture. Normal maps convert the glTF/OpenGL (+Y) tangent convention to UE's (-Y), as
     *  Interchange's glTF translator does (a DDS normal map must be BC5: its green is negated in the blocks). */
    static bool Decode(const FSCPackageSource& Source, const FSCTextureKey& Key, FSCDecodedTexture& Out,
        const FSCTextureLimits& Limits = FSCTextureLimits::FromHost());
    /** Header pass (parallel) -> per-texture and aggregate budget in key order -> parallel decode of the
     *  accepted keys. Rejected keys carry an Error and are never decompressed. */
    static TArray<FSCDecodedTexture> DecodeAll(const FSCPackageSource& Source, const TArray<FSCTextureKey>& Keys,
        const FSCTextureLimits& Limits = FSCTextureLimits::FromHost());
    /** Encoding of a PNG's RGB channels: an `sRGB` chunk means sRGB, `gAMA` 1.0 means linear (both written by
     *  the exporter from the DDS header); a PNG without either falls back to the role. Normal is always linear. */
    static bool ResolveSRGB(TConstArrayView<uint8> Png, ESCTextureRole Role);
    /** Linear-light mean RGB of the top mip (sRGB-encoded texels are linearized first); false without texels.
     *  A DDS is decoded for it by the bridge (sc_dds_mean_rgb). */
    static bool MeanColor(const FSCDecodedTexture& Texture, FLinearColor& Out);
    /** Decoded size of a W x H BGRA8 texture with its full mip chain. */
    static int64 DecodedBytes(int64 Width, int64 Height);
    /** UE pixel format of a prepared package DDS's DXGI format (PF_Unknown: not a format the contract passes). */
    static uint8 PixelFormatOf(uint32 DxgiFormat, bool& bSRGB);
    /** Game thread: transient texture owned by Outer, never streamed: the DDS blocks in their pixel format, or
     *  uncompressed BGRA8. */
    static UTexture2D* Create(FSCDecodedTexture&& Decoded, UObject* Outer);
    /** Box-filtered mip chain down to 1x1 (exported PNGs carry a single level); sRGB-encoded RGB (bSRGB) is
     *  averaged in linear space, alpha / linear data / normals as stored. */
    static void BuildMips(FSCDecodedTexture& Texture);
};
