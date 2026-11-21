// Asynchronous request and sidecar validation for an authored body action set.
#include "SCTraversalClips.h"
#include "SCPackageSource.h"
#include "SCPlayerBody.h"
#include "SCRigComponent.h"
#include "SCViewerSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Engine/SkeletalMesh.h"

bool FSCTraversalClips::Start(const USCPlayerBodyComponent& Body, USCViewerSubsystem* Viewer,
    TArray<FString> Names, const TCHAR* FixtureOption)
{
    if (Names.IsEmpty() || !Body.GetLeader() || !Body.GetLeader()->GetSkeletalMeshAsset()) return false;
    const FString Gender = Body.Gender();
    Names.Sort();
    TMap<FString, int32> Bones;
    const auto& Ref = Body.GetLeader()->GetSkeletalMeshAsset()->GetRefSkeleton();
    for (int32 Index = 0; Index < Ref.GetNum(); ++Index) Bones.Add(Ref.GetBoneName(Index).ToString(), Index);

    FString File;
    if (FParse::Value(FCommandLine::Get(), FixtureOption, File))
    {
        Parse = UE::Tasks::Launch(UE_SOURCE_LOCATION, [File, Gender, Names = MoveTemp(Names), Bones = MoveTemp(Bones)] {
            TArray64<uint8> Tar;
            if (!FFileHelper::LoadFileToArray(Tar, *File)) return FResult{{}, TEXT("Cannot read traversal clips: ") + File};
            return Read(MoveTemp(Tar), Gender, Names, Bones);
        }, UE::Tasks::ETaskPriority::BackgroundNormal);
    }
    else
    {
        const auto Client = Viewer ? Viewer->GetClient() : nullptr;
        if (!Client) return false;
        Progress = MakeShared<FSCExportProgress, ESPMode::ThreadSafe>();
        auto Export = Client->ExportPlayerClips(Gender, Names, Progress.ToSharedRef());
        Parse = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Export, Gender, Names = MoveTemp(Names), Bones = MoveTemp(Bones)]() mutable {
            FSCExportResult& Result = Export.GetResult();
            if (!Result.bOK) return FResult{{}, Result.Error.ToString()};
            return Read(MoveTemp(Result.Tar), Gender, Names, Bones);
        }, UE::Tasks::Prerequisites(Export), UE::Tasks::ETaskPriority::BackgroundNormal);
    }
    return true;
}

FSCTraversalClips::FResult FSCTraversalClips::Read(TArray64<uint8>&& Tar, const FString& Gender, const TArray<FString>& Names,
    const TMap<FString, int32>& Bones)
{
    FSCTarPackageSource Source(MoveTemp(Tar));
    FResult Result;
    if (!Source.IsValid())
    {
        Result.Error = Source.GetError();
        return Result;
    }
    auto ReadText = [&](const FString& Path, FString& Json) {
        TArray<uint8> Bytes;
        if (!Source.ReadBytes(Path, Bytes, Result.Error)) return false;
        Json = FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()));
        return true;
    };
    FString Json;
    TSharedPtr<FJsonObject> Scene;
    const TSharedPtr<FJsonObject>* Root = nullptr;
    const TSharedPtr<FJsonObject>* ClipSet = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
    FString PackageGender;
    if (!ReadText(TEXT("Packages/player_clips/scene.json"), Json) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Scene) || !Scene ||
        !Scene->TryGetObjectField(TEXT("root_entity"), Root) || !(*Root)->TryGetArrayField(TEXT("animations"), Records) ||
        !Scene->TryGetObjectField(TEXT("player_clips"), ClipSet) || !(*ClipSet)->TryGetStringField(TEXT("gender"), PackageGender) ||
        !PackageGender.Equals(Gender, ESearchCase::IgnoreCase))
    {
        if (Result.Error.IsEmpty()) Result.Error = TEXT("Invalid traversal clip manifest");
        return Result;
    }
    TMap<FName, FString> Paths;
    for (const auto& Value : *Records)
        if (Value && Value->Type == EJson::Object)
        {
            FString Name, Sidecar;
            const auto Object = Value->AsObject();
            if (Object && Object->TryGetStringField(TEXT("name"), Name) && Object->TryGetStringField(TEXT("sidecar"), Sidecar))
                Paths.Add(FName(*Name), Sidecar);
        }
    for (const FString& Name : Names)
    {
        const FString* Path = Paths.Find(FName(*Name));
        const auto Clip = MakeShared<FSCPlayerClip>();
        if (!Path)
        {
            Result.Error = TEXT("Missing traversal animation ") + Name;
            return Result;
        }
        if (!ReadText(FString(TEXT("Packages/player_clips/")) + *Path, Json) ||
            !FSCPlayerClip::Parse(Json,
                [&](const FString& Bone) { const int32* Index = Bones.Find(Bone); return Index ? *Index : INDEX_NONE; },
                *Clip, Result.Error) || Clip->bAdditive ||
            Clip->Tracks.IsEmpty() || !FMath::IsFinite(Clip->Duration) || Clip->Duration <= 0.f)
        {
            if (Result.Error.IsEmpty()) Result.Error = TEXT("Unreadable traversal animation ") + Name;
            return Result;
        }
        Result.Clips.Add(FName(*Name), Clip);
    }
    return Result;
}

TOptional<FSCTraversalClips::FResult> FSCTraversalClips::Take()
{
    if (!Parse.IsValid() || !Parse.IsCompleted()) return {};
    FResult Result = MoveTemp(Parse.GetResult());
    Parse = {};
    Progress.Reset();
    return Result;
}

void FSCTraversalClips::Cancel()
{
    if (Progress) Progress->RequestCancel();
    Progress.Reset();
    Parse = {};
}
