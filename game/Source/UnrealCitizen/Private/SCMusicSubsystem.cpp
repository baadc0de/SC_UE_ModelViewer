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
    bCatalogueLoaded = false;
}

void USCMusicSubsystem::RequestCatalogue()
{
    bWantCatalogue = true;
}

bool USCMusicSubsystem::Play(const FString& Id)
{
    if (!Cues.ContainsByPredicate([&Id](const FSCMusicCue& Cue) { return Cue.Id == Id; }))
        return false;
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
        Error = Result.Error;
        return;
    }
    TArray<TSharedPtr<FJsonValue>> Rows;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result.Json), Rows))
    {
        Error = LOCTEXT("InvalidCatalogue", "Could not read the music library.");
        return;
    }
    for (const auto& Value : Rows)
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if (!Value || !Value->TryGetObject(Row) || !Row || !*Row)
            continue;
        FSCMusicCue Cue;
        if ((*Row)->TryGetStringField(TEXT("id"), Cue.Id) && !Cue.Id.IsEmpty() &&
            (*Row)->TryGetStringField(TEXT("title"), Cue.Title) && !Cue.Title.IsEmpty() &&
            (*Row)->TryGetStringField(TEXT("authored_name"), Cue.AuthoredName) &&
            (*Row)->TryGetNumberField(TEXT("seconds"), Cue.Seconds) &&
            FMath::IsFinite(Cue.Seconds) && Cue.Seconds > 0)
        {
            Cues.Add(MoveTemp(Cue));
        }
    }
    bCatalogueLoaded = true;
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
    USCAudioWave* Sound = NewObject<USCAudioWave>(this);
    if (!Sound->Init(Result.Ogg, 1))
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
    Component->Play();
    UE_LOG(LogSCMusic, Display, TEXT("MUSIC_PLAY id=%s bytes=%d channels=%d rate=%d duration=%.3f"),
        *RequestedId, Result.Ogg->Num(), Wave->Channels, Wave->Rate, Wave->PlaySeconds);
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
            Error = FText::GetEmpty();
            CatalogueTask = Client->MusicCatalogue();
        }
    }
    if (Wave && Wave->Health.bFinished.load(std::memory_order_relaxed))
        Stop();

    if (ReadTask.IsValid() && ReadTask.IsCompleted())
    {
        if (ReadingId == RequestedId)
            StartVoice(ReadTask.GetResult());
        ReadTask = {};
        ReadingId.Reset();
    }
    // While a read is in flight, rapid clicks coalesce to the latest cue.
    if (!RequestedId.IsEmpty() && !Component && !ReadTask.IsValid())
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
