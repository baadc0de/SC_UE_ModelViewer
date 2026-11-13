// FSCTextureImporter: package DDS blocks (prepared by the bridge) or PNG decode + CPU mips on workers, transient
// UTexture2D creation on the game thread.
#include "SCTextureImporter.h"
#include "RHI.h"
#include "SCBridgeClient.h" // scbridge.h: sc_dds_prepare / sc_dds_mean_rgb
#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "TextureResource.h"
#include "Misc/Paths.h"
#include "RHIGlobals.h"

FSCTextureLimits FSCTextureLimits::FromHost()
{
    FSCTextureLimits L; L.MaxDimension = GetMax2DTextureDimension();
    // One import may hold a third of the GPU's dedicated memory (game environments carry ~1000 textures), never less
    // than the default budget.
    FTextureMemoryStats Stats;
    RHIGetTextureMemoryStats(Stats);
    if (Stats.DedicatedVideoMemory > 0) L.BudgetBytes = FMath::Max(L.BudgetBytes, Stats.DedicatedVideoMemory / 3);
    return L;
}

int64 FSCDecodedTexture::UploadBytes() const
{
    if (IsBlocks()) return Dds.Num() - DataOffset;
    int64 Bytes = 0; for (const auto& Mip : Mips) Bytes += Mip.Num();
    return Bytes;
}

uint8 FSCTextureImporter::PixelFormatOf(uint32 Dxgi, bool& bSRGB)
{
    // The DXGI formats docs/work-packages/WP-T1-compressed-textures.md passes, after sc_dds_prepare (no SNORM).
    bSRGB = Dxgi == 72 || Dxgi == 75 || Dxgi == 78 || Dxgi == 99 || Dxgi == 91 || Dxgi == 29;
    switch (Dxgi)
    {
    case 61: return PF_G8;
    case 71: case 72: return PF_DXT1;
    case 74: case 75: return PF_DXT3;
    case 77: case 78: return PF_DXT5;
    case 80: return PF_BC4;
    case 83: return PF_BC5;
    case 98: case 99: return PF_BC7;
    case 87: case 91: return PF_B8G8R8A8;
    case 28: case 29: return PF_R8G8B8A8;
    default: bSRGB = false; return PF_Unknown;
    }
}

int64 FSCTextureImporter::DecodedBytes(int64 W, int64 H)
{
    int64 Bytes = 0;
    for (W = FMath::Max<int64>(W, 1), H = FMath::Max<int64>(H, 1);; W = FMath::Max<int64>(1, W / 2), H = FMath::Max<int64>(1, H / 2))
    { Bytes += W * H * 4; if (W == 1 && H == 1) break; }
    return Bytes;
}

namespace
{
/** Header-only probe: the PNG wrapper parses IHDR in SetCompressed; GetRaw decompresses. A package DDS is
 *  validated and prepared in place (cheap: only BC4 / BC5 endpoints and indices are rewritten). */
struct FProbe
{
    TSharedPtr<IImageWrapper> Png;
    TArray<uint8> Bytes;
    int64 Width = 0, Height = 0, Upload = 0;
    bool bSRGB = false;
    bool bDds = false;
    uint8 Format = PF_Unknown;
    int32 MipCount = 0;
    int64 DataOffset = 0;
};
bool ProbeDds(const FSCTextureKey& Key, const FSCTextureLimits& Limits, FProbe& P, FString& Error)
{
    P.bDds = true;
    sc_dds_info Info;
    if (Key.Role == ESCTextureRole::DerivedRoughness)
    { Error = TEXT("A package DDS is never a derived roughness map (the DDNA smoothness is stored as is): ") + Key.Path; return false; }
    if (sc_dds_prepare(P.Bytes.GetData(), size_t(P.Bytes.Num()), Key.Role == ESCTextureRole::Normal, &Info) != SC_STATUS_OK)
    { Error = FString::Printf(TEXT("Invalid package DDS (%s): %s"), UTF8_TO_TCHAR(sc_last_error(nullptr)), *Key.Path); return false; }
    const EPixelFormat Format = EPixelFormat(FSCTextureImporter::PixelFormatOf(Info.dxgi_format, P.bSRGB));
    if (Format == PF_Unknown || !GPixelFormats[Format].Supported)
    { Error = FString::Printf(TEXT("DXGI format %u has no supported pixel format: %s"), Info.dxgi_format, *Key.Path); return false; }
    P.Width = Info.width; P.Height = Info.height; P.Format = uint8(Format); P.MipCount = int32(Info.mip_count);
    P.DataOffset = int64(Info.data_offset); P.Upload = int64(Info.data_size);
    const uint32 MaxDim = Limits.MaxDimension ? Limits.MaxDimension : GetMax2DTextureDimension();
    if (P.Width <= 0 || P.Height <= 0 || P.Width > MaxDim || P.Height > MaxDim || P.MipCount <= 0)
    { Error = FString::Printf(TEXT("Texture %lldx%lld outside the RHI limit %u: %s"), P.Width, P.Height, MaxDim, *Key.Path); return false; }
    if (RequiresBlock4Alignment(Format) && (P.Width % 4 || P.Height % 4))
    { Error = FString::Printf(TEXT("Block-compressed texture %lldx%lld is not 4-aligned: %s"), P.Width, P.Height, *Key.Path); return false; }
    uint64 Expected = 0; // the bridge validated the DXGI layout; check it against UE's own block sizes
    for (int32 Mip = 0; Mip < P.MipCount; ++Mip) Expected += GPixelFormats[Format].Get2DTextureMipSizeInBytes(uint32(P.Width), uint32(P.Height), uint32(Mip));
    if (Expected != Info.data_size || P.DataOffset + P.Upload != P.Bytes.Num())
    { Error = FString::Printf(TEXT("DDS mip data %llu bytes, %s needs %llu: %s"), Info.data_size, GetPixelFormatString(Format), Expected, *Key.Path); return false; }
    if (P.Upload > Limits.MaxTextureBytes)
    { Error = FString::Printf(TEXT("Texture %lldx%lld exceeds the per-texture budget: %s"), P.Width, P.Height, *Key.Path); return false; }
    return true;
}
bool Probe(const FSCPackageSource& Source, const FSCTextureKey& Key, const FSCTextureLimits& Limits, FProbe& P, FString& Error)
{
    if (!Source.ReadBytes(Key.Path, P.Bytes, Error)) return false;
    if (P.Bytes.Num() >= 4 && !FMemory::Memcmp(P.Bytes.GetData(), "DDS ", 4)) return ProbeDds(Key, Limits, P, Error);
    IImageWrapperModule* Module = FModuleManager::GetModulePtr<IImageWrapperModule>(TEXT("ImageWrapper"));
    if (!Module) { Error = TEXT("ImageWrapper module not loaded"); return false; }
    P.Png = Module->CreateImageWrapper(EImageFormat::PNG);
    if (!P.Png || !P.Png->SetCompressed(P.Bytes.GetData(), P.Bytes.Num())) { Error = TEXT("Invalid PNG: ") + Key.Path; return false; }
    P.Width = P.Png->GetWidth(); P.Height = P.Png->GetHeight();
    P.bSRGB = FSCTextureImporter::ResolveSRGB(P.Bytes, Key.Role);
    const uint32 MaxDim = Limits.MaxDimension ? Limits.MaxDimension : GetMax2DTextureDimension();
    if (P.Width <= 0 || P.Height <= 0 || P.Width > MaxDim || P.Height > MaxDim)
    { Error = FString::Printf(TEXT("Texture %lldx%lld outside the RHI limit %u: %s"), P.Width, P.Height, MaxDim, *Key.Path); return false; }
    P.Upload = FSCTextureImporter::DecodedBytes(P.Width, P.Height);
    if (P.Upload > Limits.MaxTextureBytes)
    { Error = FString::Printf(TEXT("Texture %lldx%lld exceeds the per-texture budget: %s"), P.Width, P.Height, *Key.Path); return false; }
    return true;
}
bool Finish(FProbe& P, const FSCTextureKey& Key, FSCDecodedTexture& Out)
{
    if (P.bDds)
    {
        Out.Width = int32(P.Width); Out.Height = int32(P.Height); Out.bSRGB = P.bSRGB; Out.PixelFormat = P.Format;
        Out.MipCount = P.MipCount; Out.DataOffset = P.DataOffset; Out.Dds = MoveTemp(P.Bytes);
        return true;
    }
    TArray64<uint8> Raw;
    if (!P.Png->GetRaw(ERGBFormat::BGRA, 8, Raw) || Raw.Num() != P.Width * P.Height * 4)
    { Out.Error = TEXT("Unexpected PNG layout: ") + Key.Path; return false; }
    P.Png.Reset(); P.Bytes.Empty();
    Out.Width = int32(P.Width); Out.Height = int32(P.Height); Out.bSRGB = P.bSRGB;
    if (Key.Role == ESCTextureRole::Normal)
        for (int64 I = 1; I < Raw.Num(); I += 4) Raw[I] = 255 - Raw[I]; // glTF +Y -> UE -Y; BGRA index 1 is green
    if (Key.Role == ESCTextureRole::DerivedRoughness)
        for (int64 I = 0; I < Raw.Num(); I += 4) // r = sqrt(1 - s) in red (BGRA index 2) -> s = 1 - r^2 in B, G, R
        {
            const float R = Raw[I + 2] / 255.f;
            Raw[I] = Raw[I + 1] = Raw[I + 2] = uint8(FMath::Clamp(FMath::RoundToInt((1.f - R * R) * 255.f), 0, 255));
        }
    Out.Mips.Add(MoveTemp(Raw));
    FSCTextureImporter::BuildMips(Out);
    return true;
}
float SrgbToLinear(uint8 V) { return FLinearColor::sRGBToLinearTable[V]; }
uint8 LinearToSrgb(float L)
{
    L = FMath::Clamp(L, 0.f, 1.f);
    const float S = L <= 0.0031308f ? L * 12.92f : 1.055f * FMath::Pow(L, 1.f / 2.4f) - 0.055f;
    return uint8(FMath::Clamp(FMath::RoundToInt(S * 255.f), 0, 255));
}
}

bool FSCTextureImporter::ResolveSRGB(TConstArrayView<uint8> Png, ESCTextureRole Role)
{
    if (Role == ESCTextureRole::Normal || Role == ESCTextureRole::DerivedRoughness) return false;
    // PNG chunks after the 8-byte signature: u32 length, 4-byte type, data, u32 CRC (big-endian). The colour
    // chunks precede IDAT. UE's PNG wrapper applies neither chunk to the pixels, so they are pure metadata here.
    auto U32 = [&Png](int64 At) { return (uint32(Png[At]) << 24) | (uint32(Png[At + 1]) << 16) | (uint32(Png[At + 2]) << 8) | uint32(Png[At + 3]); };
    for (int64 At = 8; At + 12 <= Png.Num();)
    {
        const int64 Length = U32(At);
        const uint8* Type = &Png[At + 4];
        if (At + 12 + Length > Png.Num() || !FMemory::Memcmp(Type, "IDAT", 4)) break;
        if (!FMemory::Memcmp(Type, "sRGB", 4)) return true;
        if (!FMemory::Memcmp(Type, "gAMA", 4) && Length == 4 && U32(At + 8) == 100000) return false;
        At += 12 + Length;
    }
    return Role == ESCTextureRole::Color;
}

bool FSCTextureImporter::Decode(const FSCPackageSource& Source, const FSCTextureKey& Key, FSCDecodedTexture& Out, const FSCTextureLimits& Limits)
{
    Out = FSCDecodedTexture(); Out.Key = Key;
    FProbe P;
    return Probe(Source, Key, Limits, P, Out.Error) && Finish(P, Key, Out);
}

bool FSCTextureImporter::MeanColor(const FSCDecodedTexture& T, FLinearColor& Out)
{
    if (T.IsBlocks())
    {
        float Rgb[3] = {0, 0, 0};
        if (sc_dds_mean_rgb(T.Dds.GetData(), size_t(T.Dds.Num()), Rgb) != SC_STATUS_OK) return false;
        Out = FLinearColor(Rgb[0], Rgb[1], Rgb[2]);
        return true;
    }
    if (T.Mips.IsEmpty() || T.Mips[0].Num() < 4) return false;
    const TArray64<uint8>& Top = T.Mips[0];
    const int64 Count = Top.Num() / 4;
    double Sum[3] = {0, 0, 0}; // BGRA
    for (int64 I = 0; I < Count; ++I)
        for (int32 C = 0; C < 3; ++C) Sum[C] += T.bSRGB ? double(SrgbToLinear(Top[I * 4 + C])) : Top[I * 4 + C] / 255.0;
    Out = FLinearColor(float(Sum[2] / Count), float(Sum[1] / Count), float(Sum[0] / Count));
    return true;
}

void FSCTextureImporter::BuildMips(FSCDecodedTexture& T)
{
    const bool bSrgb = T.bSRGB;
    int32 W = T.Width, H = T.Height;
    while (T.Mips.Num() && (W > 1 || H > 1))
    {
        const int32 NW = FMath::Max(1, W / 2), NH = FMath::Max(1, H / 2);
        const TArray64<uint8>& Src = T.Mips.Last();
        TArray64<uint8> Dst; Dst.SetNumUninitialized(int64(NW) * NH * 4);
        for (int32 Y = 0; Y < NH; ++Y)
            for (int32 X = 0; X < NW; ++X)
                for (int32 C = 0; C < 4; ++C)
                {
                    // Average the 2x2 (or 2x1 / 1x2 at odd edges) parent footprint; sRGB colour in linear light.
                    const bool bLinearize = bSrgb && C < 3;
                    float Sum = 0; int32 Count = 0;
                    for (int32 DY = 0; DY < (H > 1 ? 2 : 1); ++DY)
                        for (int32 DX = 0; DX < (W > 1 ? 2 : 1); ++DX)
                        {
                            const int32 SX = FMath::Min(X * 2 + DX, W - 1), SY = FMath::Min(Y * 2 + DY, H - 1);
                            const uint8 V = Src[(int64(SY) * W + SX) * 4 + C];
                            Sum += bLinearize ? SrgbToLinear(V) : float(V); ++Count;
                        }
                    Dst[(int64(Y) * NW + X) * 4 + C] = bLinearize ? LinearToSrgb(Sum / Count)
                        : uint8(FMath::Clamp(FMath::RoundToInt(Sum / Count), 0, 255));
                }
        T.Mips.Add(MoveTemp(Dst)); W = NW; H = NH;
    }
}

TArray<FSCDecodedTexture> FSCTextureImporter::DecodeAll(const FSCPackageSource& Source, const TArray<FSCTextureKey>& Keys, const FSCTextureLimits& Limits)
{
    TArray<FSCDecodedTexture> Out; Out.SetNum(Keys.Num());
    TArray<FProbe> Probes; Probes.SetNum(Keys.Num());
    TArray<bool> Ok; Ok.Init(false, Keys.Num());
    ParallelFor(Keys.Num(), [&](int32 I) { Out[I].Key = Keys[I]; Ok[I] = Probe(Source, Keys[I], Limits, Probes[I], Out[I].Error); });
    int64 Total = 0; // aggregate budget in deterministic key order
    for (int32 I = 0; I < Keys.Num(); ++I)
    {
        if (!Ok[I]) continue;
        const int64 Bytes = Probes[I].Upload;
        if (Total + Bytes > Limits.BudgetBytes)
        { Ok[I] = false; Probes[I] = FProbe(); Out[I].Error = TEXT("Import texture budget exhausted: ") + Keys[I].Path; continue; }
        Total += Bytes;
    }
    ParallelFor(Keys.Num(), [&](int32 I) { if (Ok[I]) Finish(Probes[I], Keys[I], Out[I]); });
    return Out;
}

UTexture2D* FSCTextureImporter::Create(FSCDecodedTexture&& D, UObject* Outer)
{
    check(IsInGameThread());
    if (D.Mips.IsEmpty() && !D.IsBlocks()) return nullptr;
    FString Name = FPaths::GetBaseFilename(D.Key.Path);
    for (TCHAR& C : Name) if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    UTexture2D* T = NewObject<UTexture2D>(Outer, MakeUniqueObjectName(Outer, UTexture2D::StaticClass(), FName(TEXT("T_") + Name)), RF_Transient);
    auto* Platform = new FTexturePlatformData();
    const EPixelFormat Format = D.IsBlocks() ? EPixelFormat(D.PixelFormat) : PF_B8G8R8A8;
    Platform->SizeX = D.Width; Platform->SizeY = D.Height; Platform->SetNumSlices(1); Platform->PixelFormat = Format;
    auto AddMip = [Platform](int32 W, int32 H, const uint8* Bytes, int64 Num)
    {
        auto* Mip = new FTexture2DMipMap(W, H, 1);
        Mip->BulkData.Lock(LOCK_READ_WRITE);
        FMemory::Memcpy(Mip->BulkData.Realloc(Num), Bytes, Num);
        Mip->BulkData.Unlock();
        Platform->Mips.Add(Mip);
    };
    if (D.IsBlocks()) // the game's mip chain as stored; sizes validated against GPixelFormats in ProbeDds
    {
        int64 Offset = D.DataOffset;
        for (int32 I = 0; I < D.MipCount; ++I)
        {
            const int64 Num = int64(GPixelFormats[Format].Get2DTextureMipSizeInBytes(uint32(D.Width), uint32(D.Height), uint32(I)));
            AddMip(FMath::Max(1, D.Width >> I), FMath::Max(1, D.Height >> I), D.Dds.GetData() + Offset, Num);
            Offset += Num;
        }
    }
    else
    {
        int32 W = D.Width, H = D.Height;
        for (const TArray64<uint8>& Bytes : D.Mips)
        {
            AddMip(W, H, Bytes.GetData(), Bytes.Num());
            W = FMath::Max(1, W / 2); H = FMath::Max(1, H / 2);
        }
    }
    T->SetPlatformData(Platform);
    T->bNotOfflineProcessed = true; // Mips are uploaded as-is; never platform-processed.
    T->NeverStream = true;
    T->SRGB = D.bSRGB;
    const bool bData = D.Key.Role == ESCTextureRole::Linear || D.Key.Role == ESCTextureRole::DerivedRoughness;
    T->CompressionSettings = D.Key.Role == ESCTextureRole::Normal ? TC_Normalmap : bData && !D.bSRGB ? TC_Masks : TC_Default;
    T->LODGroup = D.Key.Role == ESCTextureRole::Normal ? TEXTUREGROUP_WorldNormalMap : bData ? TEXTUREGROUP_WorldSpecular : TEXTUREGROUP_World;
    T->Filter = TF_Default;
    T->AddressX = T->AddressY = TA_Wrap;
    T->UpdateResource();
    D.Mips.Empty(); D.Dds.Empty();
    return T;
}
