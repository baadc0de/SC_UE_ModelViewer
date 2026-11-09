// FSCTextureImporter: PNG decode + CPU mips on workers, transient UTexture2D creation on the game thread.
#include "SCTextureImporter.h"
#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "TextureResource.h"
#include "Misc/Paths.h"

bool FSCTextureImporter::Decode(const FSCPackageSource& Source, const FSCTextureKey& Key, FSCDecodedTexture& Out)
{
    Out = FSCDecodedTexture(); Out.Key = Key;
    // Loaded by the importer on the game thread; module lookup is then thread-safe.
    IImageWrapperModule* Module = FModuleManager::GetModulePtr<IImageWrapperModule>(TEXT("ImageWrapper"));
    if (!Module) { Out.Error = TEXT("ImageWrapper module not loaded"); return false; }
    TArray<uint8> Bytes;
    if (!Source.ReadBytes(Key.Path, Bytes, Out.Error)) return false;
    TSharedPtr<IImageWrapper> Png = Module->CreateImageWrapper(EImageFormat::PNG);
    TArray64<uint8> Raw;
    if (!Png || !Png->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Png->GetRaw(ERGBFormat::BGRA, 8, Raw))
    { Out.Error = TEXT("Invalid PNG: ") + Key.Path; return false; }
    Out.Width = int32(Png->GetWidth()); Out.Height = int32(Png->GetHeight());
    if (Out.Width <= 0 || Out.Height <= 0 || Raw.Num() != int64(Out.Width) * Out.Height * 4)
    { Out.Error = TEXT("Unexpected PNG layout: ") + Key.Path; return false; }
    if (Key.Role == ESCTextureRole::Normal)
        for (int64 I = 1; I < Raw.Num(); I += 4) Raw[I] = 255 - Raw[I]; // BGRA: index 1 is green.
    Out.Mips.Add(MoveTemp(Raw));
    BuildMips(Out);
    return true;
}

void FSCTextureImporter::BuildMips(FSCDecodedTexture& T)
{
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
                    // Average the 2x2 (or 2x1 / 1x2 at odd edges) footprint of the parent level.
                    uint32 Sum = 0, Count = 0;
                    for (int32 DY = 0; DY < (H > 1 ? 2 : 1); ++DY)
                        for (int32 DX = 0; DX < (W > 1 ? 2 : 1); ++DX)
                        {
                            const int32 SX = FMath::Min(X * 2 + DX, W - 1), SY = FMath::Min(Y * 2 + DY, H - 1);
                            Sum += Src[(int64(SY) * W + SX) * 4 + C]; ++Count;
                        }
                    Dst[(int64(Y) * NW + X) * 4 + C] = uint8((Sum + Count / 2) / Count);
                }
        T.Mips.Add(MoveTemp(Dst)); W = NW; H = NH;
    }
}

TArray<FSCDecodedTexture> FSCTextureImporter::DecodeAll(const FSCPackageSource& Source, const TArray<FSCTextureKey>& Keys)
{
    TArray<FSCDecodedTexture> Out; Out.SetNum(Keys.Num());
    ParallelFor(Keys.Num(), [&](int32 I) { Decode(Source, Keys[I], Out[I]); });
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
