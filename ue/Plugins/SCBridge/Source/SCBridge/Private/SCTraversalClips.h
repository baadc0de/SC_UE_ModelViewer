// Clip-only archive request shared by the character's authored traversal actions.
#pragma once

#include "SCBridgeClient.h"
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

private:
    static FResult Read(TArray64<uint8>&& Tar, const FString& Gender, const TArray<FString>& Names,
        const TMap<FString, int32>& Bones);
    TSharedPtr<FSCExportProgress, ESPMode::ThreadSafe> Progress;
    UE::Tasks::TTask<FResult> Parse;
};
