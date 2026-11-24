// SCPrepareAudio (SCPrepareAudio.h): a package's audio staged beside its prepared level.
#include "SCPrepareAudio.h"
#include "SCPackageSource.h"
#include "SCAudioSource.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
bool ReadJson(const FSCPackageSource& Source, const FString& Path, TSharedPtr<FJsonObject>& Out, FString& Error)
{
    TArray<uint8> Bytes;
    if (!Source.ReadBytes(Path, Bytes, Error)) return false;
    FString Text;
    FFileHelper::BufferToString(Text, Bytes.GetData(), Bytes.Num());
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) || !Out)
    {
        Error = Path + TEXT(": invalid JSON");
        return false;
    }
    return true;
}
} // namespace

bool SCPrepareAudio::Stage(const FSCPackageSource& Source, const FString& Scene, const FString& Target, int32& OutFiles, FString& Error)
{
    OutFiles = 0;
    const FString PackageDir = FPaths::GetPath(Scene);
    const FString AudioJson = PackageDir / TEXT("audio.json");
    if (!Source.Exists(AudioJson))
    {
        Error = Scene + TEXT(": no audio.json (an export before cache format 36)");
        return false;
    }
    TSharedPtr<FJsonObject> Manifest;
    if (!ReadJson(Source, AudioJson, Manifest, Error)) return false;
    TSet<FString> Files; // the media are SoundWave assets (MakeWaves)
    const TSharedPtr<FJsonObject>* Environments = nullptr;
    if (Manifest->TryGetObjectField(TEXT("environments"), Environments))
        for (const auto& [Id, Value] : (*Environments)->Values)
        {
            const TSharedPtr<FJsonObject>* Ir = nullptr;
            FString File;
            if (Value->Type == EJson::Object && Value->AsObject()->TryGetObjectField(TEXT("impulse_response"), Ir) &&
                (*Ir)->TryGetStringField(TEXT("file"), File) && !File.IsEmpty())
                Files.Add(File);
        }
    TArray<FString> Sorted = Files.Array();
    Sorted.Sort();
    TArray<FString> Missing;
    for (const FString& File : Sorted)
        if (!Source.Exists(File)) Missing.Add(File);
    if (!Missing.IsEmpty())
    {
        Error = FString::Printf(TEXT("%s: %d audio payloads missing (first: %s)"), *Scene, Missing.Num(), *Missing[0]);
        return false;
    }
    // Written beside the target first: a failed staging leaves the previous one in place.
    IFileManager& FileManager = IFileManager::Get();
    const FString Staging = Target + TEXT(".staging");
    FileManager.DeleteDirectory(*Staging, false, true);
    auto Write = [&](const FString& Rel, const TArray<uint8>& Bytes) {
        const FString Path = Staging / Rel;
        FileManager.MakeDirectory(*FPaths::GetPath(Path), true);
        if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
        {
            Error = TEXT("cannot write ") + Path;
            return false;
        }
        return true;
    };
    TArray<uint8> Bytes;
    if (!Source.ReadBytes(AudioJson, Bytes, Error) || !Write(AudioJson, Bytes)) return false;
    for (const FString& File : Sorted)
    {
        if (!Source.ReadBytes(File, Bytes, Error) || !Write(File, Bytes)) return false;
        ++OutFiles;
    }
    // The interiors' audio records: the prepared actor keeps their anchors, not the scene.
    TSharedPtr<FJsonObject> SceneJson;
    if (!ReadJson(Source, Scene, SceneJson, Error)) return false;
    TArray<TSharedPtr<FJsonValue>> Interiors;
    const TArray<TSharedPtr<FJsonValue>>* SceneInteriors = nullptr;
    if (SceneJson->TryGetArrayField(TEXT("interiors"), SceneInteriors))
        for (const TSharedPtr<FJsonValue>& Value : *SceneInteriors)
        {
            const TSharedPtr<FJsonObject> In = Value->Type == EJson::Object ? Value->AsObject() : nullptr;
            const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
            FString Name;
            if (In) In->TryGetStringField(TEXT("name"), Name);
            Out->SetStringField(TEXT("name"), Name);
            for (const TCHAR* Field : {TEXT("audio_spots"), TEXT("audio_areas"), TEXT("room_volumes")})
            {
                const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
                Out->SetArrayField(Field, In && In->TryGetArrayField(Field, List) ? *List : TArray<TSharedPtr<FJsonValue>>());
            }
            Interiors.Add(MakeShared<FJsonValueObject>(Out));
        }
    const TSharedRef<FJsonObject> Sidecar = MakeShared<FJsonObject>();
    Sidecar->SetNumberField(TEXT("version"), 1);
    Sidecar->SetArrayField(TEXT("interiors"), Interiors);
    FString SidecarText;
    FJsonSerializer::Serialize(Sidecar, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&SidecarText));
    if (!FFileHelper::SaveStringToFile(SidecarText, *(Staging / PackageDir / TEXT("audio_interiors.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        Error = TEXT("cannot write the interiors' audio records under ") + Staging;
        return false;
    }
    FileManager.DeleteDirectory(*Target, false, true);
    if (!FPlatformFileManager::Get().GetPlatformFile().MoveFile(*Target, *Staging))
    {
        Error = FString::Printf(TEXT("cannot move %s to %s"), *Staging, *Target);
        return false;
    }
    return true;
}

namespace
{
/** The audio decoders' modules, as FAudioDeviceManager::RegisterAudioInfoFactories loads them (a commandlet has no audio
 *  device, so nothing registered the codecs a saved wave is checked with): Engine.ini's Audio:AudioInfoModules, or
 *  UE's built-in list. */
void LoadAudioDecoders()
{
    TArray<FString> Modules;
    if (!GConfig->GetArray(TEXT("Audio"), TEXT("AudioInfoModules"), Modules, GEngineIni))
        Modules = {TEXT("OpusAudioDecoder"), TEXT("VorbisAudioDecoder"), TEXT("AdpcmAudioDecoder"), TEXT("BinkAudioDecoder"), TEXT("RadAudioDecoder")};
    for (const FString& Module : Modules) FModuleManager::Get().LoadModule(*Module);
}
} // namespace

USoundWave* SCPrepareAudio::SaveWave(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Ogg, const FString& Path, const FString& What, FString& Error)
{
    LoadAudioDecoders();
    TArray<float> Decoded;
    FString E;
    const FSCAudioSourcePtr Source = MakeOggSource(Ogg, E);
    FSCAudioStream Stream;
    if (!Source || !Stream.Open(Source, 1) || Stream.Channels <= 0 || Stream.Rate <= 0)
    {
        Error = FString::Printf(TEXT("%s: %s"), *What, E.IsEmpty() ? TEXT("does not decode") : *E);
        return nullptr;
    }
    // A surround wave needs UE's per-channel source layout (USoundFactory's); no prepared media has more than two.
    if (Stream.Channels > 2)
    {
        Error = FString::Printf(TEXT("%s: %d channels (a prepared media is mono or stereo)"), *What, Stream.Channels);
        return nullptr;
    }
    // One pass: the whole media, in blocks (the length the media states is not trusted for the allocation). The
    // samples, padded, and their 16-bit bytes must fit UE's 32-bit array counts.
    const int32 Ch = Stream.Channels, Block = 4096;
    constexpr int64 MaxSamples = (int64(MAX_int32) - 1024) / 2;
    Decoded.Reset();
    int64 Frames = 0;
    for (;;)
    {
        if ((Frames + Block) * Ch > MaxSamples)
        {
            Error = What + TEXT(": too long");
            return nullptr;
        }
        if (int64(Decoded.Num()) < (Frames + Block) * Ch) Decoded.SetNumUninitialized(FMath::Min((Frames + Block) * Ch * 2, MaxSamples));
        const int64 Got = Stream.Read(Decoded.GetData() + Frames * Ch, Block);
        if (Got < 0)
        {
            Error = What + TEXT(": a decoding error");
            return nullptr;
        }
        if (Got == 0) break;
        Frames += Got;
    }
    if (Frames <= 0)
    {
        Error = What + TEXT(": no audio");
        return nullptr;
    }
    UPackage* Package = CreatePackage(*Path);
    USoundWave* Wave = NewObject<USoundWave>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone);
    FillSoundWave(Wave, Decoded.GetData(), Frames, Ch, Stream.Rate, Stream.Frames, Stream.LoopStart, Stream.LoopEnd);
    // The running platform's compressed audio now (the derived-data cache), not when a prepared run first loads it,
    // and it must play: the source a run makes of it opens a decoder and decodes its first frames.
    Wave->CachePlatformData(false);
    const FSCAudioSourcePtr Played = MakeSoundWaveSource(Wave, E);
    FSCAudioStream Check;
    TArray<float> First;
    First.SetNumZeroed(256 * Ch);
    if (!Played || !Check.Open(Played, 1) || Check.Read(First.GetData(), 256) <= 0)
    {
        Error = FString::Printf(TEXT("%s: the saved wave does not play (%s)"), *What, E.IsEmpty() ? TEXT("no decoder or no frames") : *E);
        return nullptr;
    }
    FAssetRegistryModule::AssetCreated(Wave);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    const FString PackageFile = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
    if (UPackage::Save(Package, Wave, *PackageFile, Args).Result != ESavePackageResult::Success)
    {
        Error = TEXT("cannot save ") + Path;
        return nullptr;
    }
    return Wave;
}

bool SCPrepareAudio::MakeWaves(const FSCPackageSource& Source, const FString& Scene, const FString& Folder, TMap<uint32, TObjectPtr<USoundWave>>& Out,
                               FString& Error)
{
    Out.Reset();
    LoadAudioDecoders();
    TSharedPtr<FJsonObject> Manifest;
    if (!ReadJson(Source, FPaths::GetPath(Scene) / TEXT("audio.json"), Manifest, Error)) return false;
    const TSharedPtr<FJsonObject>* Media = nullptr;
    if (!Manifest->TryGetObjectField(TEXT("media"), Media)) return true;
    TArray<TPair<FString, TSharedPtr<FJsonValue>>> Entries;
    for (const auto& [IdText, Value] : (*Media)->Values) Entries.Emplace(IdText, Value);
    Entries.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });
    for (const auto& [IdText, Value] : Entries)
    {
        FString File;
        if (Value->Type != EJson::Object || !Value->AsObject()->TryGetStringField(TEXT("file"), File) || File.IsEmpty()) continue;
        const uint32 Id = uint32(FCString::Strtoui64(*IdText, nullptr, 10));
        TArray<uint8> Bytes;
        if (!Source.ReadBytes(File, Bytes, Error)) return false;
        USoundWave* Wave = SaveWave(MakeShared<const TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(Bytes)), Folder / (TEXT("M_") + IdText), File, Error);
        if (!Wave) return false;
        Out.Add(Id, Wave);
    }
    return true;
}
