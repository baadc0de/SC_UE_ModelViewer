// Where a game particle effect lives as a Niagara system (WP-K4c): the one naming rule shared by the builder (SCFxBuild,
// SCBridgeFx) and the runtime (cargo drones, box impacts). An effect reference `<GUID>|<library>.<effect>` (the library
// is Data/Libs/Particles/<library>.xml, split at the first '.' after the last '/', as scviewer-core's split_reference)
// becomes /Game/SC/FX/<library>/<effect>, dots of the effect name as underscores, every other character outside
// [A-Za-z0-9_] as '_'. Package paths are case-insensitive, so a library written in another case names the same asset.
#pragma once
#include "CoreMinimal.h"
#include "Misc/Paths.h"

namespace SCFxNames
{
/** The root of the built systems (gitignored, game-derived, per machine). */
inline const TCHAR* Root() { return TEXT("/Game/SC/FX"); }

inline FString SafeSegment(FString S)
{
    for (TCHAR& C : S)
        if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    return S.IsEmpty() ? FString(TEXT("_")) : S;
}

inline FString SafePath(const FString& Path)
{
    TArray<FString> Parts;
    Path.ParseIntoArray(Parts, TEXT("/"));
    for (FString& P : Parts) P = SafeSegment(P);
    return FString::Join(Parts, TEXT("/"));
}

/** The long package name of the system built for Library / Effect (as the effect JSON names them). */
inline FString PackageFor(const FString& Library, const FString& Effect)
{
    return FString(Root()) / SafePath(Library.Replace(TEXT("\\"), TEXT("/"))) / SafeSegment(Effect.Replace(TEXT("."), TEXT("_")));
}

/** Split `<GUID>|<library>.<effect>` (GUID optional) into its library and effect; false when it has no `<library>.<effect>`. */
inline bool SplitReference(const FString& Reference, FString& OutLibrary, FString& OutEffect)
{
    FString Rest = Reference.TrimStartAndEnd();
    int32 Bar = INDEX_NONE;
    if (Rest.FindChar(TEXT('|'), Bar)) Rest = Rest.Mid(Bar + 1).TrimStartAndEnd();
    Rest.ReplaceInline(TEXT("\\"), TEXT("/"));
    int32 Slash = INDEX_NONE;
    const int32 DirEnd = Rest.FindLastChar(TEXT('/'), Slash) ? Slash + 1 : 0;
    const int32 Dot = Rest.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromStart, DirEnd);
    if (Dot == INDEX_NONE) return false;
    OutLibrary = Rest.Left(Dot);
    OutEffect = Rest.Mid(Dot + 1);
    return !OutLibrary.IsEmpty() && !OutLibrary.EndsWith(TEXT("/")) && !OutEffect.IsEmpty();
}

/** The object path (`<package>.<name>`) of the system built for an effect reference; empty when it is not one. */
inline FString SystemPathForReference(const FString& Reference)
{
    FString Library, Effect;
    if (!SplitReference(Reference, Library, Effect)) return FString();
    const FString Package = PackageFor(Library, Effect);
    return Package + TEXT(".") + FPaths::GetBaseFilename(Package);
}
} // namespace SCFxNames
