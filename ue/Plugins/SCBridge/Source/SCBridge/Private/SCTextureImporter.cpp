// FSCTextureImporter: PNG decode + CPU mips on workers, transient UTexture2D creation on the game thread.
#include "SCTextureImporter.h"
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
    FSCTextureLimits L; L.MaxDimension = GetMax2DTextureDimension(); return L;
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
/** Header-only probe: the PNG wrapper parses IHDR in SetCompressed; GetRaw decompresses. */
struct FProbe
{
    TSharedPtr<IImageWrapper> Png;
    TArray<uint8> Bytes;
    int64 Width = 0, Height = 0;
};
bool Probe(const FSCPackageSource& Source, const FSCTextureKey& Key, const FSCTextureLimits& Limits, FProbe& P, FString& Error)
{
    IImageWrapperModule* Module = FModuleManager::GetModulePtr<IImageWrapperModule>(TEXT("ImageWrapper"));
    if (!Module) { Error = TEXT("ImageWrapper module not loaded"); return false; }
    if (!Source.ReadBytes(Key.Path, P.Bytes, Error)) return false;
    P.Png = Module->CreateImageWrapper(EImageFormat::PNG);
    if (!P.Png || !P.Png->SetCompressed(P.Bytes.GetData(), P.Bytes.Num())) { Error = TEXT("Invalid PNG: ") + Key.Path; return false; }
    P.Width = P.Png->GetWidth(); P.Height = P.Png->GetHeight();
    const uint32 MaxDim = Limits.MaxDimension ? Limits.MaxDimension : GetMax2DTextureDimension();
    if (P.Width <= 0 || P.Height <= 0 || P.Width > MaxDim || P.Height > MaxDim)
    { Error = FString::Printf(TEXT("Texture %lldx%lld outside the RHI limit %u: %s"), P.Width, P.Height, MaxDim, *Key.Path); return false; }
    if (FSCTextureImporter::DecodedBytes(P.Width, P.Height) > Limits.MaxTextureBytes)
    { Error = FString::Printf(TEXT("Texture %lldx%lld exceeds the per-texture budget: %s"), P.Width, P.Height, *Key.Path); return false; }
    return true;
}
bool Finish(FProbe& P, const FSCTextureKey& Key, FSCDecodedTexture& Out)
{
    TArray64<uint8> Raw;
    if (!P.Png->GetRaw(ERGBFormat::BGRA, 8, Raw) || Raw.Num() != P.Width * P.Height * 4)
    { Out.Error = TEXT("Unexpected PNG layout: ") + Key.Path; return false; }
    P.Png.Reset(); P.Bytes.Empty();
    Out.Width = int32(P.Width); Out.Height = int32(P.Height);
    if (Key.Role == ESCTextureRole::Normal)
        for (int64 I = 1; I < Raw.Num(); I += 4) Raw[I] = 255 - Raw[I]; // glTF +Y -> UE -Y; BGRA index 1 is green
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

bool FSCTextureImporter::Decode(const FSCPackageSource& Source, const FSCTextureKey& Key, FSCDecodedTexture& Out, const FSCTextureLimits& Limits)
{
    Out = FSCDecodedTexture(); Out.Key = Key;
    FProbe P;
    return Probe(Source, Key, Limits, P, Out.Error) && Finish(P, Key, Out);
}

void FSCTextureImporter::BuildMips(FSCDecodedTexture& T)
{
    const bool bSrgb = T.Key.Role == ESCTextureRole::Color;
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
        const int64 Bytes = DecodedBytes(Probes[I].Width, Probes[I].Height);
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
    if (D.Mips.IsEmpty()) return nullptr;
    FString Name = FPaths::GetBaseFilename(D.Key.Path);
    for (TCHAR& C : Name) if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    UTexture2D* T = NewObject<UTexture2D>(Outer, MakeUniqueObjectName(Outer, UTexture2D::StaticClass(), FName(TEXT("T_") + Name)), RF_Transient);
    auto* Platform = new FTexturePlatformData();
    Platform->SizeX = D.Width; Platform->SizeY = D.Height; Platform->SetNumSlices(1); Platform->PixelFormat = PF_B8G8R8A8;
    int32 W = D.Width, H = D.Height;
    for (TArray64<uint8>& Bytes : D.Mips)
    {
        auto* Mip = new FTexture2DMipMap(W, H, 1);
        Mip->BulkData.Lock(LOCK_READ_WRITE);
        FMemory::Memcpy(Mip->BulkData.Realloc(Bytes.Num()), Bytes.GetData(), Bytes.Num());
        Mip->BulkData.Unlock();
        Platform->Mips.Add(Mip);
        W = FMath::Max(1, W / 2); H = FMath::Max(1, H / 2);
    }
    T->SetPlatformData(Platform);
    T->bNotOfflineProcessed = true; // Mips are uploaded as-is; never platform-processed.
    T->NeverStream = true;
    T->SRGB = D.Key.Role == ESCTextureRole::Color;
    T->CompressionSettings = D.Key.Role == ESCTextureRole::Normal ? TC_Normalmap : D.Key.Role == ESCTextureRole::Linear ? TC_Masks : TC_Default;
    T->LODGroup = D.Key.Role == ESCTextureRole::Normal ? TEXTUREGROUP_WorldNormalMap : D.Key.Role == ESCTextureRole::Linear ? TEXTUREGROUP_WorldSpecular : TEXTUREGROUP_World;
    T->Filter = TF_Default;
    T->AddressX = T->AddressY = TA_Wrap;
    T->UpdateResource();
    D.Mips.Empty();
    return T;
}
