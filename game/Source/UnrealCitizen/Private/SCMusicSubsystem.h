// WP-M3 soundtrack catalogue and one non-positional voice for the local player.
#pragma once

#include "CoreMinimal.h"
#include "SCArchive.h"
#include "SCAudioSource.h"
#include "Subsystems/WorldSubsystem.h"
#include "SCMusicSubsystem.generated.h"

class UAudioComponent;
class USCAudioWave;

struct FSCMusicCue
{
    FString Id, Title, AuthoredName;
    double Seconds = 0;
};

UCLASS()
class USCMusicSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    static USCMusicSubsystem* Get(const UObject* WorldContext);
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    void RequestCatalogue();
    /** A catalogue row as the player takes it (id, title, authored name and a finite positive length); false: it is left
     *  out. The prepared record's cue must be such a row (FUCPrepared::ResolveMusicCue). */
    static bool ReadCue(const FJsonObject& Row, FSCMusicCue& Out);
    /** A prepared scenario's music (WP-O1-4b-3c): the catalogue as read at prep and the cues saved as waves (id -> asset).
     *  A prepared cue plays from its wave; without an archive reader the catalogue is the prepared one, listing the
     *  prepared cues only (the others cannot play). */
    void SetPrepared(const FString& CatalogueJson, const TMap<FString, FString>& Waves);
    bool Play(const FString& Id);
    void TogglePause();
    void Stop();

    const TArray<FSCMusicCue>& GetCues() const { return Cues; }
    /** Bumped whenever the listed cues change (their rows too, not only their number). */
    int32 GetCuesRevision() const { return CuesRevision; }
    const FString& GetCurrentId() const { return RequestedId; }
    bool HasCatalogue() const { return bCatalogueLoaded; }
    bool IsLoadingCue() const { return !RequestedId.IsEmpty() && !Component; }
    bool IsPaused() const { return bPaused; }
    bool HasVoice() const { return Component != nullptr; }
    double SecondsPlayed() const;
    int64 RenderedFrames() const;
    /** Playback's error, else the library's (a library that loads clears only its own). */
    const FText& GetError() const { return Error.IsEmpty() ? CatalogueError : Error; }

private:
    void ResetArchive();
    void SetCatalogue(const FSCTextResult& Result);
    void StartVoice(const FSCMusicBytesResult& Result);
    /** Play a decoded source (Bytes: its size, for the log; 0 for a prepared wave). */
    void StartVoiceFrom(const FSCAudioSourcePtr& Source, int64 Bytes);
    /** The prepared catalogue, as SetCatalogue reads one (only the cues with a wave). */
    void SetPreparedCatalogue();
    void ReleaseVoice();

    TArray<FSCMusicCue> Cues;
    FText Error;          // playback
    FText CatalogueError; // the library
    int32 ArchiveGeneration = INDEX_NONE;
    bool bWantCatalogue = false;
    bool bCatalogueLoaded = false;
    bool bPaused = false;
    FString RequestedId, ReadingId;
    bool bPreparedVoice = false; // the voice plays a prepared wave (not an archive read)
    TArray<FSCMusicCue> LibraryCues; // the library as it came (the archive's, or the prepared one without a reader)
    int32 CuesRevision = 0;
    /** Cues from LibraryCues (a cue a prepared wave plays: the prepared row) and the prepared cues it lacks, after it. */
    void RebuildCues();
    FString PreparedCatalogue;
    TMap<FString, FString> PreparedWaves;
    UE::Tasks::TTask<FSCTextResult> CatalogueTask;
    UE::Tasks::TTask<FSCMusicBytesResult> ReadTask;
    UPROPERTY(Transient) TObjectPtr<UAudioComponent> Component;
    UPROPERTY(Transient) TObjectPtr<USCAudioWave> Wave;
};
