// FSCTextureImporter: exported PNG -> transient UTexture2D with per-role colour space and sampler flags.
// Decode (and CPU mip chain) is pure and runs on worker threads; Create runs on the game thread.
#pragma once
#include "CoreMinimal.h"
#include "SCPackageSource.h"

class UTexture2D;

/** How a texture slot is sampled. Only Color is sRGB; Normal is a DDNA tangent-space map. */
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
    FString Error;
};

struct SCBRIDGE_API FSCTextureStats
{
    int32 Requested = 0, Created = 0, Failed = 0;
    int64 Bytes = 0;
    double DecodeSeconds = 0, CreateSeconds = 0;
};

class SCBRIDGE_API FSCTextureImporter
{
public:
    /** Decode one PNG. Normal maps convert the glTF/OpenGL (+Y) tangent convention to UE's (-Y),
     *  as Interchange's glTF translator does; the DDNA smoothness alpha is kept untouched. */
    static bool Decode(const FSCPackageSource& Source, const FSCTextureKey& Key, FSCDecodedTexture& Out);
    /** Decode every key in parallel on the task graph (call from a worker or the game thread). */
    static TArray<FSCDecodedTexture> DecodeAll(const FSCPackageSource& Source, const TArray<FSCTextureKey>& Keys);
    /** Game thread: transient texture owned by Outer; uncompressed BGRA8, never streamed. */
    static UTexture2D* Create(FSCDecodedTexture&& Decoded, UObject* Outer);
    /** Box-filtered mip chain down to 1x1 (exported PNGs carry a single level). */
    static void BuildMips(FSCDecodedTexture& Texture);
};
