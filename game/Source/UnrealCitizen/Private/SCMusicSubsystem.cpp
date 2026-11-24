// WP-M3. The score stays independent of ship placement and uses the existing A1 Ogg voice.
#include "SCMusicSubsystem.h"
#include "Components/AudioComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "SCAudioSystem.h"
#include "SCViewerSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UI/Cargo/SCCargoViewSource.h"

#define LOCTEXT_NAMESPACE "SCMusic"
DEFINE_LOG_CATEGORY_STATIC(LogSCMusic, Log, All);

USCMusicSubsystem* USCMusicSubsystem::Get(const UObject* WorldContext)
{
    UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
    return World && World->IsGameWorld() ? World->GetSubsystem<USCMusicSubsystem>() : nullptr;
}

bool USCMusicSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void USCMusicSubsystem::Deinitialize()
{
    ResetArchive();
    Super::Deinitialize();
}

TStatId USCMusicSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(USCMusicSubsystem, STATGROUP_Tickables);
}

void USCMusicSubsystem::ResetArchive()
{
    Stop();
    CatalogueTask = {};
    ReadTask = {};
    ReadingId.Reset();
    Cues.Reset();
    LibraryCues.Reset();
    ++CuesRevision;
    bCatalogueLoaded = false;
    CatalogueError = FText::GetEmpty();
}

void USCMusicSubsystem::RebuildCues()
{
    // The prepared rows of the cues a wave plays (the recording that plays; the archive's row may be a newer one's).
    TArray<FSCMusicCue> PreparedRows;
    if (!PreparedWaves.IsEmpty())
    {
        TArray<TSharedPtr<FJsonValue>> Kept;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PreparedCatalogue), Kept);
        for (const TSharedPtr<FJsonValue>& Value : Kept)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            FSCMusicCue Cue;
            if (Value && Value->TryGetObject(Row) && Row && *Row && ReadCue(**Row, Cue) && PreparedWaves.Contains(Cue.Id) &&
                !PreparedRows.ContainsByPredicate([&Cue](const FSCMusicCue& Known) { return Known.Id == Cue.Id; }))
                PreparedRows.Add(MoveTemp(Cue));
        }
    }
    Cues.Reset();
    for (const FSCMusicCue& Cue : LibraryCues)
    {
        const FSCMusicCue* Prepared = PreparedRows.FindByPredicate([&Cue](const FSCMusicCue& P) { return P.Id == Cue.Id; });
        Cues.Add(Prepared ? *Prepared : Cue);
    }
    for (const FSCMusicCue& Prepared : PreparedRows)
        if (!Cues.ContainsByPredicate([&Prepared](const FSCMusicCue& Known) { return Known.Id == Prepared.Id; })) Cues.Add(Prepared);
    ++CuesRevision;
}

bool USCMusicSubsystem::ReadCue(const FJsonObject& Row, FSCMusicCue& Cue)
{
    return Row.TryGetStringField(TEXT("id"), Cue.Id) && !Cue.Id.IsEmpty() && Row.TryGetStringField(TEXT("title"), Cue.Title) &&
           !Cue.Title.IsEmpty() && Row.TryGetStringField(TEXT("authored_name"), Cue.AuthoredName) &&
           Row.TryGetNumberField(TEXT("seconds"), Cue.Seconds) && FMath::IsFinite(Cue.Seconds) && Cue.Seconds > 0;
}

void USCMusicSubsystem::RequestCatalogue()
{
    bWantCatalogue = true;
}

void USCMusicSubsystem::SetPrepared(const FString& CatalogueJson, const TMap<FString, FString>& Waves)
{
    PreparedCatalogue = CatalogueJson;
    PreparedWaves = Waves;
    if (bCatalogueLoaded) RebuildCues(); // the library already listed: its prepared cues now as they play
    // A cue being read from the archive now has a wave: the read is dropped, the wave plays (Tick).
    if (ReadTask.IsValid() && PreparedWaves.Contains(ReadingId))
    {
        ReadTask = {};
        ReadingId.Reset();
    }
}

void USCMusicSubsystem::SetPreparedCatalogue()
{
    // The prepared rows, the cues without a wave left out (nothing could play them).
    TArray<TSharedPtr<FJsonValue>> Rows, Kept;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PreparedCatalogue), Rows);
    for (const TSharedPtr<FJsonValue>& Value : Rows)
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        FString Id;
        if (Value && Value->TryGetObject(Row) && Row && *Row && (*Row)->TryGetStringField(TEXT("id"), Id) && PreparedWaves.Contains(Id)) Kept.Add(Value);
    }
    FSCTextResult Result;
    Result.bOK = true;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result.Json);
    FJsonSerializer::Serialize(Kept, Writer);
    SetCatalogue(Result);
}

bool USCMusicSubsystem::Play(const FString& Id)
{
    // A prepared cue plays from its wave whatever catalogue is loaded (WP-O1-4b-3c); one the loaded catalogue lacks joins it
    // from the prepared one (the app lists what plays).
    if (!Cues.ContainsByPredicate([&Id](const FSCMusicCue& Cue) { return Cue.Id == Id; }))
    {
        if (!PreparedWaves.Contains(Id)) return false;
        RebuildCues(); // the prepared cues join the list (before or without a library)
    }
    // The same cue playing from the archive while it has a prepared wave: replaced by the wave (Tick plays it).
    if (Id == RequestedId && Component && !bPreparedVoice && PreparedWaves.Contains(Id))
    {
        ReleaseVoice();
        return true;
    }
    if (Id == RequestedId)
    {
        if (bPaused) TogglePause();
        return true;
    }
    ReleaseVoice();
    Error = FText::GetEmpty();
    RequestedId = Id;
    return true;
}

void USCMusicSubsystem::TogglePause()
{
    if (!Component) return;
    bPaused = !bPaused;
    Component->SetPaused(bPaused);
}

int64 USCMusicSubsystem::RenderedFrames() const
{
    return Wave ? Wave->Health.Frames.load(std::memory_order_relaxed) : 0;
}

double USCMusicSubsystem::SecondsPlayed() const
{
    return Wave && Wave->Rate > 0 ? double(RenderedFrames()) / Wave->Rate : 0.0;
}

void USCMusicSubsystem::ReleaseVoice()
{
    if (Component)
    {
        Component->Stop();
        Component->DestroyComponent();
        Component = nullptr;
    }
    Wave = nullptr;
    bPaused = false;
}

void USCMusicSubsystem::Stop()
{
    ReleaseVoice();
    RequestedId.Reset();
    Error = FText::GetEmpty();
}

void USCMusicSubsystem::SetCatalogue(const FSCTextResult& Result)
{
    if (!Result.bOK)
    {
        CatalogueError = Result.Error;
        return;
    }
    TArray<TSharedPtr<FJsonValue>> Rows;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result.Json), Rows))
    {
        CatalogueError = LOCTEXT("InvalidCatalogue", "Could not read the music library.");
        return;
    }
    // The library's rows in its order, one per cue (RebuildCues lists them with the prepared ones).
    LibraryCues.Reset();
    for (const auto& Value : Rows)
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if (!Value || !Value->TryGetObject(Row) || !Row || !*Row)
            continue;
        if (FSCMusicCue Cue; ReadCue(**Row, Cue) && !LibraryCues.ContainsByPredicate([&Cue](const FSCMusicCue& Known) { return Known.Id == Cue.Id; }))
            LibraryCues.Add(MoveTemp(Cue));
    }
    bCatalogueLoaded = true;
    CatalogueError = FText::GetEmpty();
    RebuildCues();
    UE_LOG(LogSCMusic, Display, TEXT("MUSIC_CATALOGUE cues=%d"), Cues.Num());
}

void USCMusicSubsystem::StartVoice(const FSCMusicBytesResult& Result)
{
    if (!Result.Ogg)
    {
        RequestedId.Reset();
        Error = Result.Error;
        return;
    }
    FString DecodeError;
    StartVoiceFrom(MakeOggSource(Result.Ogg, DecodeError), Result.Ogg->Num());
}

void USCMusicSubsystem::StartVoiceFrom(const FSCAudioSourcePtr& Source, int64 Bytes)
{
    USCAudioWave* Sound = NewObject<USCAudioWave>(this);
    if (!Sound->Init(Source, 1))
    {
        RequestedId.Reset();
        Error = LOCTEXT("InvalidAudio", "Could not decode this cue.");
        return;
    }

    // World-managed, non-spatial playback. Closing the app or unpairing the ship leaves the player running.
    Component = UGameplayStatics::CreateSound2D(this, Sound, 1.f, 1.f, 0.f, nullptr, false, false);
    if (!Component)
    {
        RequestedId.Reset();
        Error = LOCTEXT("NoAudioDevice", "Music playback needs an active audio device.");
        return;
    }
    Wave = Sound;
    bPreparedVoice = Bytes == 0;
    Component->Play();
    UE_LOG(LogSCMusic, Display, TEXT("MUSIC_PLAY id=%s bytes=%lld prepared=%d channels=%d rate=%d duration=%.3f"),
        *RequestedId, Bytes, Bytes == 0, Wave->Channels, Wave->Rate, Wave->PlaySeconds);
}

void USCMusicSubsystem::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(this);
    if (!Viewer) return;
    if (ArchiveGeneration != Viewer->GetArchiveGeneration())
    {
        ArchiveGeneration = Viewer->GetArchiveGeneration();
        ResetArchive();
    }
    const auto Client = Viewer->GetClient();
    if (Wave && Wave->Health.bFinished.load(std::memory_order_relaxed))
        Stop();
    // No archive reader in this build (WP-O1-4c): the prepared catalogue (WP-O1-4b-3c), else the library never comes and a
    // request (the app, a scenario's music) ends with that reason instead of waiting.
    // With a reader whose archive is closed or failed, the prepared catalogue too (an archive opened later is a new
    // generation: its full catalogue replaces it when asked for again).
    const ESCArchiveState Archive = Viewer->GetArchiveState();
    const bool bArchiveUnavailable = !Viewer->CanReadArchive() || Archive == ESCArchiveState::Closed || Archive == ESCArchiveState::Failed;
    if (bArchiveUnavailable && bWantCatalogue && !bCatalogueLoaded)
    {
        if (!PreparedCatalogue.IsEmpty())
        {
            bWantCatalogue = false;
            SetPreparedCatalogue();
        }
        else if (!Viewer->CanReadArchive() && CatalogueError.IsEmpty())
            CatalogueError = LOCTEXT("NoArchiveReader", "The soundtrack library is unavailable in this build.");
    }
    // A prepared cue plays from its wave, archive or not.
    // (An archive read of another cue still in flight is not waited for: its result is dropped when it completes.)
    if (!RequestedId.IsEmpty() && !Component)
        if (const FString* Path = PreparedWaves.Find(RequestedId))
        {
            const FString Asset = *Path + TEXT(".") + FPackageName::GetShortName(*Path);
            USoundWave* Prepared = LoadObject<USoundWave>(nullptr, *Asset);
            FString SourceError;
            const FSCAudioSourcePtr Source = Prepared ? MakeSoundWaveSource(Prepared, SourceError) : nullptr;
            if (Source)
                StartVoiceFrom(Source, 0);
            else
            {
                Error = LOCTEXT("InvalidPrepared", "Could not play this prepared cue.");
                UE_LOG(LogSCMusic, Warning, TEXT("MUSIC_PREPARED id=%s wave=%s unavailable: %s"), *RequestedId, *Asset,
                       Prepared ? *SourceError : TEXT("not found"));
                RequestedId.Reset();
            }
        }
    if (!Client || Viewer->GetArchiveState() != ESCArchiveState::Ready) return;

    if (CatalogueTask.IsValid() && CatalogueTask.IsCompleted())
    {
        SetCatalogue(CatalogueTask.GetResult());
        CatalogueTask = {};
    }
    if (bWantCatalogue)
    {
        bWantCatalogue = false;
        if (!bCatalogueLoaded && !CatalogueTask.IsValid())
        {
            CatalogueError = FText::GetEmpty();
            CatalogueTask = Client->MusicCatalogue();
        }
    }
    if (ReadTask.IsValid() && ReadTask.IsCompleted())
    {
        if (ReadingId == RequestedId && !PreparedWaves.Contains(ReadingId)) // a prepared wave serves it (Tick, above)
            StartVoice(ReadTask.GetResult());
        ReadTask = {};
        ReadingId.Reset();
    }
    // While a read is in flight, rapid clicks coalesce to the latest cue. A prepared cue plays from its wave (above, next
    // tick), never from the archive.
    if (!RequestedId.IsEmpty() && !Component && !ReadTask.IsValid() && !PreparedWaves.Contains(RequestedId))
    {
        ReadingId = RequestedId;
        ReadTask = Client->MusicRead(ReadingId);
    }
}

#if !UE_BUILD_SHIPPING
namespace
{
USCMusicSubsystem* MusicIn(UWorld* World)
{
    return USCMusicSubsystem::Get(World ? World : SCViewerShell::ViewerWorld());
}
FAutoConsoleCommandWithWorldAndArgs GMusicList(TEXT("sc.Music.List"), TEXT("List the music cue names and playback IDs."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World) {
        if (auto* Music = MusicIn(World))
        {
            Music->RequestCatalogue();
            for (const FSCMusicCue& Cue : Music->GetCues())
                UE_LOG(LogSCMusic, Display, TEXT("MUSIC_CUE id=%s name=%s seconds=%.3f"), *Cue.Id, *Cue.AuthoredName, Cue.Seconds);
        }
    }));
FAutoConsoleCommandWithWorldAndArgs GMusicControl(TEXT("sc.Music.Control"), TEXT("sc.Music.Control <cue-id>|pause|stop"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World) {
        auto* Music = MusicIn(World);
        if (!Music || Args.Num() != 1) return;
        if (Args[0] == TEXT("pause")) Music->TogglePause();
        else if (Args[0] == TEXT("stop")) Music->Stop();
        else Music->Play(Args[0]);
    }));
FAutoConsoleCommandWithWorldAndArgs GMusicState(TEXT("sc.Music.State"), TEXT("Show catalogue, cue and audio progress."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World) {
        if (auto* Music = MusicIn(World))
            UE_LOG(LogSCMusic, Display, TEXT("MUSIC_STATE cues=%d ready=%d id=%s voice=%d paused=%d frames=%lld error=\"%s\""),
                Music->GetCues().Num(), Music->HasCatalogue(), *Music->GetCurrentId(), Music->HasVoice(),
                Music->IsPaused(), Music->RenderedFrames(), *Music->GetError().ToString());
    }));
}
#endif

#undef LOCTEXT_NAMESPACE
