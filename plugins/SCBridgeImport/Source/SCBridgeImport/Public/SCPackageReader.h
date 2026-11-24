// FSCPackageReader: a decomposed package's scene.json -> FSCSceneManifest (SCSceneManifest.h).
#pragma once
#include "CoreMinimal.h"
#include "SCPackageSource.h"
#include "SCSceneManifest.h"

class SCBRIDGEIMPORT_API FSCPackageReader
{
public:
    // Empty ScenePath selects the sole manifest; shared roots with multiple scenes require an explicit path.
    static bool Read(const FSCPackageSource& Source, FSCSceneManifest& Out, FString& Error, const FString& ScenePath = TEXT(""), TArray<FString>* Warnings = nullptr);
    static FString Hash(const TArray<uint8>& Bytes);
};
