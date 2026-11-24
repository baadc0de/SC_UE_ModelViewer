// Clip-only archive request shared by the character's authored traversal actions.
#pragma once

#include "SCArchive.h"
#include "SCPlayerAnimator.h"

class USCPlayerBodyComponent;
class USCViewerSubsystem;

class FSCTraversalClips
{
public:
    struct FResult
    {
        FSCPlayerClipMap Clips;
        FString Error;
    };
    bool Start(const USCPlayerBodyComponent& Body, USCViewerSubsystem* Viewer,
        TArray<FString> Names, const TCHAR* FixtureOption);
    TOptional<FResult> Take();
    void Cancel();
    /** WP-O1-4b-2b: whether a prepared scenario's set serves Names (Out: the clips, or why it cannot). A set that does not
     *  load (null) is a broken preparation and serves the error; a loaded set lacking a name leaves the request to the
     *  archive when it is open (bArchive: a vehicle brought in later, such as the Garage's), else serves the missing clip. */
    static bool ServeFromPrepared(const FSCPlayerClipMap* Set, const TArray<FString>& Names, bool bArchive, FResult& Out);

private:
    static FResult Read(TArray64<uint8>&& Tar, const FString& Gender, const TArray<FString>& Names,
        const TMap<FString, int32>& Bones);
    TSharedPtr<FSCExportProgress, ESPMode::ThreadSafe> Progress;
    UE::Tasks::TTask<FResult> Parse;
};
