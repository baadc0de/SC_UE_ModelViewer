// WP-M3 soundtrack catalogue and one non-positional voice for the local player.
#pragma once

#include "CoreMinimal.h"
#include "SCArchive.h"
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
    bool Play(const FString& Id);
    void TogglePause();
    void Stop();

    const TArray<FSCMusicCue>& GetCues() const { return Cues; }
    const FString& GetCurrentId() const { return RequestedId; }
    bool HasCatalogue() const { return bCatalogueLoaded; }
    bool IsLoadingCue() const { return !RequestedId.IsEmpty() && !Component; }
    bool IsPaused() const { return bPaused; }
    bool HasVoice() const { return Component != nullptr; }
    double SecondsPlayed() const;
    int64 RenderedFrames() const;
    const FText& GetError() const { return Error; }

private:
    void ResetArchive();
    void SetCatalogue(const FSCTextResult& Result);
    void StartVoice(const FSCMusicBytesResult& Result);
    void ReleaseVoice();

    TArray<FSCMusicCue> Cues;
    FText Error;
    int32 ArchiveGeneration = INDEX_NONE;
    bool bWantCatalogue = false;
    bool bCatalogueLoaded = false;
    bool bPaused = false;
    FString RequestedId, ReadingId;
    UE::Tasks::TTask<FSCTextResult> CatalogueTask;
    UE::Tasks::TTask<FSCMusicBytesResult> ReadTask;
    UPROPERTY(Transient) TObjectPtr<UAudioComponent> Component;
    UPROPERTY(Transient) TObjectPtr<USCAudioWave> Wave;
};
