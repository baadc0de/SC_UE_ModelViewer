// SCTextureSource (SCTextureSource.h): a runtime texture's editable BGRA8 source from its mip 0.
#include "SCTextureSource.h"
#include "SCBridgeClient.h" // scbridge.h: sc_texels_decode_bgra8
#include "Engine/Texture2D.h"
#include "TextureResource.h"

bool SCTextureSource::Add(UTexture2D* Texture, FString& Error)
{
    // A texture with a source already (a prepared one, its source made at prep from the uploaded pixels) keeps it: a
    // decode of its platform mips would replace it with the block-compressed approximation.
    if (Texture && Texture->Source.IsValid()) return true;
    FTexturePlatformData* Platform = Texture ? Texture->GetPlatformData() : nullptr;
    if (!Platform || Platform->Mips.IsEmpty())
    {
        Error = TEXT("no platform mips");
        return false;
    }
    // Float (HDR) pixels, e.g. an EXR projector gobo (four channels, or one): copied as they are, linear, into a float
    // source of the same layout, kept float when the editor builds it.
    struct FFloatLayout { EPixelFormat Pixel; ETextureSourceFormat Source; int32 Bytes; TextureCompressionSettings Compression; };
    static const FFloatLayout Floats[] = {
        {PF_FloatRGBA, TSF_RGBA16F, 8, TC_HDR},
        {PF_A32B32G32R32F, TSF_RGBA32F, 16, TC_HDR},
        {PF_R16F, TSF_R16F, 2, TC_HalfFloat},
        {PF_R32_FLOAT, TSF_R32F, 4, TC_SingleFloat},
    };
    for (const FFloatLayout& Float : Floats)
    {
        if (Platform->PixelFormat != Float.Pixel) continue;
        FTexture2DMipMap& Mip = Platform->Mips[0];
        const int64 Expected = int64(Mip.SizeX) * Mip.SizeY * Float.Bytes;
        const uint8* Data = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
        const bool bOK = Data && Mip.BulkData.GetBulkDataSize() == Expected;
        if (bOK) Texture->Source.Init(Mip.SizeX, Mip.SizeY, 1, 1, Float.Source, Data);
        Mip.BulkData.Unlock();
        if (!bOK)
        {
            Error = Data ? TEXT("float mip 0 has an unexpected size") : TEXT("mip 0 not CPU-resident");
            return false;
        }
        Texture->SRGB = false;
        if (Texture->CompressionSettings == TC_Default) Texture->CompressionSettings = Float.Compression;
        return true;
    }
    uint32 Dxgi = 0;
    switch (Platform->PixelFormat)
    {
    case PF_B8G8R8A8: Dxgi = 87; break;
    case PF_R8G8B8A8: Dxgi = 28; break;
    case PF_G8: Dxgi = 61; break;
    case PF_DXT1: Dxgi = 71; break;
    case PF_DXT3: Dxgi = 74; break;
    case PF_DXT5: Dxgi = 77; break;
    case PF_BC4: Dxgi = 80; break;
    case PF_BC5: Dxgi = 83; break;
    case PF_BC7: Dxgi = 98; break;
    default:
        Error = FString::Printf(TEXT("pixel format %s has no source decode"), GPixelFormats[Platform->PixelFormat].Name);
        return false;
    }
    FTexture2DMipMap& Mip = Platform->Mips[0];
    const uint8* Data = static_cast<const uint8*>(Mip.BulkData.LockReadOnly());
    TArray64<uint8> Bgra;
    Bgra.SetNumUninitialized(int64(Mip.SizeX) * Mip.SizeY * 4);
    const bool bOK = Data && sc_texels_decode_bgra8(Data, size_t(Mip.BulkData.GetBulkDataSize()), Dxgi, uint32(Mip.SizeX), uint32(Mip.SizeY),
                                                    Bgra.GetData(), size_t(Bgra.Num())) == SC_STATUS_OK;
    Mip.BulkData.Unlock();
    if (!bOK)
    {
        Error = FString::Printf(TEXT("decode failed (%s)"), Data ? UTF8_TO_TCHAR(sc_last_error(nullptr)) : TEXT("mip 0 not CPU-resident"));
        return false;
    }
    Texture->Source.Init(Mip.SizeX, Mip.SizeY, 1, 1, TSF_BGRA8, Bgra.GetData());
    return true;
}
