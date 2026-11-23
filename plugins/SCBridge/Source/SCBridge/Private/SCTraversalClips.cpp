// Asynchronous request and sidecar validation for an authored body action set.
#include "SCTraversalClips.h"
#include "SCPackageReader.h"
#include "SCPackageSource.h"
#include "SCLadderRecords.h"
#include "SCPlayerKitData.h"
#include "SCSeatRecords.h"
#include "SCVehicle.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogSCTraversal, Log, All);

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
    // A prepared scenario (WP-O1-4b-2b): its clips were read for its body at prep and serve every seat and ladder of its
    // vehicle and environment. A request they do not cover (a vehicle brought in after the start, the Garage's) asks the
    // archive when it is open, as without a scenario; else it fails as a missing clip.
    FResult Prepared;
    bool bPrepared = false;
    if (Viewer && Viewer->UsesPreparedTraversalClips())
    {
        const TSharedPtr<const FSCPlayerClipMap> Set = Viewer->GetPreparedTraversalClips();
        if (!Set) Prepared.Error = TEXT("the prepared traversal clips do not load for this body (prepare the scenario again)");
        for (const FString& Name : Names)
        {
            const TSharedPtr<const FSCPlayerClip>* Clip = Set ? Set->Find(FName(*Name)) : nullptr;
            if (Clip && *Clip)
            {
                Prepared.Clips.Add(FName(*Name), *Clip);
                continue;
            }
            if (Prepared.Error.IsEmpty()) Prepared.Error = TEXT("Traversal animation ") + Name + TEXT(" was not prepared");
            Prepared.Clips.Reset();
            break;
        }
        bPrepared = Prepared.Error.IsEmpty() || Viewer->GetArchiveState() != ESCArchiveState::Ready;
    }
    const bool bFixture = !bPrepared && FParse::Value(FCommandLine::Get(), FixtureOption, File);
    UE_LOG(LogSCTraversal, Display, TEXT("TRAVERSAL_CLIPS_SOURCE source=%s names=%d%s"), bPrepared ? TEXT("prepared") : bFixture ? TEXT("fixture") : TEXT("archive"),
           Names.Num(), Viewer && Viewer->UsesPreparedTraversalClips() && !bPrepared ? TEXT(" (not in the prepared set)") : TEXT(""));
    if (bPrepared)
    {
        Parse = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Result = MoveTemp(Prepared)]() mutable { return MoveTemp(Result); });
    }
    else if (bFixture)
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
    TArray<FString> Skipped;
    if (!Source.IsValid()) Result.Error = Source.GetError();
    else if (!SCTraversal::ReadClips(Source, Gender, Names, Bones, false, Result.Clips, Skipped, Result.Error)) Result.Clips.Reset();
    return Result;
}

bool SCTraversal::ReadClips(const FSCPackageSource& Source, const FString& Gender, const TArray<FString>& Names, const TMap<FString, int32>& Bones,
                            bool bSkipMissing, FSCPlayerClipMap& Out, TArray<FString>& Skipped, FString& Error)
{
    auto ReadText = [&](const FString& Path, FString& Json, FString& ReadError) {
        TArray<uint8> Bytes;
        if (!Source.ReadBytes(Path, Bytes, ReadError)) return false;
        Json = FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()));
        return true;
    };
    FString Json;
    TSharedPtr<FJsonObject> Scene;
    const TSharedPtr<FJsonObject>* Root = nullptr;
    const TSharedPtr<FJsonObject>* ClipSet = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
    FString PackageGender;
    if (!ReadText(TEXT("Packages/player_clips/scene.json"), Json, Error) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Scene) || !Scene ||
        !Scene->TryGetObjectField(TEXT("root_entity"), Root) || !(*Root)->TryGetArrayField(TEXT("animations"), Records) ||
        !Scene->TryGetObjectField(TEXT("player_clips"), ClipSet) || !(*ClipSet)->TryGetStringField(TEXT("gender"), PackageGender) ||
        !PackageGender.Equals(Gender, ESearchCase::IgnoreCase))
    {
        if (Error.IsEmpty()) Error = TEXT("Invalid traversal clip manifest");
        return false;
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
        FString ClipError;
        if (!Path) ClipError = TEXT("Missing traversal animation ") + Name;
        else if (!ReadText(FString(TEXT("Packages/player_clips/")) + *Path, Json, ClipError) ||
                 !FSCPlayerClip::Parse(Json,
                     [&](const FString& Bone) { const int32* Index = Bones.Find(Bone); return Index ? *Index : INDEX_NONE; },
                     *Clip, ClipError) || Clip->bAdditive ||
                 Clip->Tracks.IsEmpty() || !FMath::IsFinite(Clip->Duration) || Clip->Duration <= 0.f)
        {
            if (ClipError.IsEmpty()) ClipError = TEXT("Unreadable traversal animation ") + Name;
        }
        if (ClipError.IsEmpty())
        {
            Out.Add(FName(*Name), Clip);
            continue;
        }
        if (!bSkipMissing)
        {
            Error = ClipError;
            return false;
        }
        Skipped.Add(Name + TEXT(": ") + ClipError);
    }
    return true;
}

TArray<FString> SCTraversal::ClipNames(const ASCVehicle& Vehicle)
{
    TArray<FString> Names;
    for (const FSCSceneInstance& Child : Vehicle.Manifest.Children)
        if (Child.Seat.IsSet())
            for (const FSCSeatAction& Action : Child.Seat->Actions)
                if (!Action.Animation.IsNone()) Names.AddUnique(Action.Animation.ToString());
    for (const FSCLadderAnchor& Ladder : Vehicle.LadderAnchors)
        for (const auto& [State, Action] : Ladder.Data.Actions)
            if (!Action.Clip.IsNone()) Names.AddUnique(Action.Clip.ToString());
    Names.Sort();
    return Names;
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
