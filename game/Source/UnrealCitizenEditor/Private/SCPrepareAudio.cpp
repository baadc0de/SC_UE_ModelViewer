// SCPrepareAudio (SCPrepareAudio.h): a package's audio staged beside its prepared level.
#include "SCPrepareAudio.h"
#include "SCPackageSource.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
    TSet<FString> Files;
    const TSharedPtr<FJsonObject>* Media = nullptr;
    if (Manifest->TryGetObjectField(TEXT("media"), Media))
        for (const auto& [Id, Value] : (*Media)->Values)
        {
            FString File;
            if (Value->Type == EJson::Object && Value->AsObject()->TryGetStringField(TEXT("file"), File) && !File.IsEmpty()) Files.Add(File);
        }
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
