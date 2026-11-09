// USCViewerSettings: launch-dir relative path resolution and persistence.
#include "SCViewerSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"

USCViewerSettings* USCViewerSettings::Get()
{
    return GetMutableDefault<USCViewerSettings>();
}
void USCViewerSettings::AddRecent(const FString& Entity)
{
    RecentEntities.RemoveAll([&](const FString& E) { return E.Equals(Entity, ESearchCase::IgnoreCase); });
    RecentEntities.Insert(Entity, 0);
    if (RecentEntities.Num() > FMath::Max(1, MaxRecents)) RecentEntities.SetNum(FMath::Max(1, MaxRecents));
}
FString USCViewerSettings::ResolveCacheDir() const
{
    FString Dir = CacheDir;
    FParse::Value(FCommandLine::Get(), TEXT("SCCacheDir="), Dir);
    if (Dir.Equals(TEXT("none"), ESearchCase::IgnoreCase)) return FString();
    if (Dir.IsEmpty()) Dir = TEXT("cache");
    // LaunchDir is the shell's working directory before UE switched to the binary directory.
    Dir = FPaths::ConvertRelativePathToFull(FPlatformMisc::LaunchDir(), Dir);
    FPaths::NormalizeDirectoryName(Dir);
    return Dir;
}
FString USCViewerSettings::ResolveArchivePath() const
{
    FString Path = ArchivePath;
    FParse::Value(FCommandLine::Get(), TEXT("SCArchive="), Path);
    return Path.IsEmpty() ? Path : FPaths::ConvertRelativePathToFull(FPlatformMisc::LaunchDir(), Path);
}
void USCViewerSettings::Save()
{
    SaveConfig();
}
