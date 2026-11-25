// USCViewerSettings: launch-dir relative path resolution and persistence.
#include "SCViewerSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
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
    // A relative directory is under the editor host's launch directory (the shell's working directory before UE switched
    // to the binary directory: run from the repository root), and under a packaged build's own folder (the one holding
    // SCViewer.exe), whatever a shortcut's start directory is.
#if WITH_EDITOR
    Dir = FPaths::ConvertRelativePathToFull(FPlatformMisc::LaunchDir(), Dir);
#else
    Dir = FPaths::ConvertRelativePathToFull(FPaths::RootDir(), Dir);
#endif
    FPaths::NormalizeDirectoryName(Dir);
    return Dir;
}
FString USCViewerSettings::ResolveArchivePath() const
{
    // The saved choice only while its file is there (a moved or reinstalled game: discovery instead); the command line
    // is taken as given (its error is shown).
    FString Path = !ArchivePath.IsEmpty() && FPaths::FileExists(ArchivePath) ? ArchivePath : FString();
    FParse::Value(FCommandLine::Get(), TEXT("SCArchive="), Path);
    return Path.IsEmpty() ? Path : FPaths::ConvertRelativePathToFull(FPlatformMisc::LaunchDir(), Path);
}
FString USCViewerSettings::ResolvePlayerGender() const
{
    FString Value = PlayerGender;
    FParse::Value(FCommandLine::Get(), TEXT("SCPlayerGender="), Value);
    return Value.TrimStartAndEnd();
}
FString USCViewerSettings::ResolvePlayerLoadout() const
{
    FString Value = PlayerLoadout;
    FParse::Value(FCommandLine::Get(), TEXT("SCPlayerLoadout="), Value);
    return Value.TrimStartAndEnd();
}
TArray<FString> USCViewerSettings::ResolvePlayerWeapons() const
{
    TArray<FString> Out = PlayerWeapons;
    FString List;
    if (FParse::Value(FCommandLine::Get(), TEXT("SCPlayerWeapons="), List)) List.ParseIntoArray(Out, TEXT(","), true);
    for (FString& W : Out) W.TrimStartAndEndInline();
    Out.RemoveAll([](const FString& W) { return W.IsEmpty(); });
    return Out;
}
bool USCViewerSettings::ResolvePlayerSpareAttachments() const
{
    bool bValue = bPlayerSpareAttachments;
    FParse::Bool(FCommandLine::Get(), TEXT("SCPlayerSpares="), bValue);
    return bValue;
}
bool USCViewerSettings::MigrateLook()
{
    if (LookRevision >= CurrentLookRevision) return false;
    // A value saved under an earlier look revision is that revision's default, not a user choice (no UI sets these), so it moves to
    // the new one; later user changes are kept.
    if (LookRevision < 9) StarBloom = LookStarBloom; // LOOK-8: the standard bloom replaced the star bloom
    LookRevision = CurrentLookRevision;
    return true;
}
void USCViewerSettings::Save()
{
    SaveConfig();
}
