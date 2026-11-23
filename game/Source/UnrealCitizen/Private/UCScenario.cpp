// FUCScenario and FUCPrepared (UCScenario.h): reading scenario definitions and the prep step's records.
#include "UCScenario.h"
#include "Cargo/SCCargoScenario.h"
#include "SCViewerSubsystem.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
FString Str(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    FString Value;
    if (Object) Object->TryGetStringField(Field, Value);
    return Value.TrimStartAndEnd();
}
FString NormalPath(const FString& Path)
{
    return Path.Replace(TEXT("\\"), TEXT("/")).TrimStartAndEnd();
}
/** A definition's text as compared for staleness: every character, case included, but line ends as LF (a checkout can turn
 *  a file's LF into CRLF without changing it). */
FString Comparable(const FString& Text)
{
    return Text.Replace(TEXT("\r\n"), TEXT("\n"), ESearchCase::CaseSensitive);
}
} // namespace

FString FUCScenario::Directory()
{
    return FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("Scenarios"));
}

bool FUCScenario::Parse(const FString& Id, const FString& Json, const FString& CargoJson, FUCScenario& Out, FString& Error)
{
    Out = FUCScenario();
    Out.Id = Id;
    Out.Json = Json;
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root)
    {
        Error = TEXT("invalid JSON");
        return false;
    }
    Out.Title = Str(Root, TEXT("name"));
    Out.Environment = NormalPath(Str(Root, TEXT("environment")));
    Out.Vehicle = Str(Root, TEXT("vehicle"));
    Out.Skin = Str(Root, TEXT("skin"));
    Out.Cargo = Str(Root, TEXT("cargo"));
    Out.Music = Str(Root, TEXT("music"));
    // The player block typed as written: the JSON reader's conversions would take a list for no block, a number for a name or a
    // string for true, and prepare another body than the definition names.
    const TSharedPtr<FJsonValue> PlayerValue = Root->TryGetField(TEXT("player"));
    if (PlayerValue && PlayerValue->Type != EJson::Object)
    {
        Error = TEXT("\"player\" is no object");
        return false;
    }
    if (PlayerValue)
    {
        const TSharedPtr<FJsonObject> Player = PlayerValue->AsObject();
        for (const TCHAR* Field : {TEXT("camera"), TEXT("start"), TEXT("gender"), TEXT("loadout")})
            if (const TSharedPtr<FJsonValue> Value = Player->TryGetField(Field); Value && Value->Type != EJson::String)
            {
                Error = FString::Printf(TEXT("\"player.%s\" is no string"), Field);
                return false;
            }
        if (const FString Camera = Str(Player, TEXT("camera")).ToLower(); !Camera.IsEmpty()) Out.Camera = Camera;
        Out.Start = Str(Player, TEXT("start"));
        Out.PlayerGender = Str(Player, TEXT("gender"));
        Out.PlayerLoadout = NormalPath(Str(Player, TEXT("loadout")));
        const TSharedPtr<FJsonValue> WeaponsValue = Player->TryGetField(TEXT("weapons"));
        if (WeaponsValue && WeaponsValue->Type != EJson::Array)
        {
            Error = TEXT("\"player.weapons\" is no list of entity class names");
            return false;
        }
        if (WeaponsValue)
            for (const TSharedPtr<FJsonValue>& Weapon : WeaponsValue->AsArray())
            {
                const FString Name = Weapon && Weapon->Type == EJson::String ? Weapon->AsString() : FString();
                // One class name per entry: a list in one string ("a,b") names no class, and would share a body level's key
                // with the two entries it stands for (USCViewerSubsystem::PlayerLevelPath).
                if (Name.TrimStartAndEnd().IsEmpty() || Name.Contains(TEXT(",")) || Name.Contains(TEXT("|")))
                {
                    Error = TEXT("\"player.weapons\" holds something other than one entity class name per entry");
                    return false;
                }
                Out.PlayerWeapons.Add(Name.TrimStartAndEnd());
            }
        if (const TSharedPtr<FJsonValue> Spares = Player->TryGetField(TEXT("spares")))
        {
            if (Spares->Type != EJson::Boolean)
            {
                Error = TEXT("\"player.spares\" is no true or false");
                return false;
            }
            Out.bPlayerSpares = Spares->AsBool();
        }
    }
    if (Out.Title.IsEmpty()) Out.Title = Id;
    if (Out.Vehicle.IsEmpty())
    {
        Error = TEXT("no \"vehicle\" (an entity class name)");
        return false;
    }
    if (!Out.Environment.IsEmpty() && !Out.Environment.EndsWith(TEXT(".socpak"), ESearchCase::IgnoreCase))
    {
        Error = FString::Printf(TEXT("\"environment\" %s is no object container path (*.socpak)"), *Out.Environment);
        return false;
    }
    if (Out.Camera != TEXT("first") && Out.Camera != TEXT("orbit") && Out.Camera != TEXT("fly"))
    {
        Error = FString::Printf(TEXT("\"player.camera\" %s is none of first, orbit, fly"), *Out.Camera);
        return false;
    }
    if (!Out.Start.IsEmpty() && Out.Camera != TEXT("first"))
    {
        Error = TEXT("\"player.start\" needs the first-person camera");
        return false;
    }
    if (Out.Cargo.IsEmpty()) return true;
    Out.CargoJson = CargoJson;
    FSCCargoScenario Cargo;
    FString CargoError;
    if (!FSCCargoScenario::Parse(CargoJson, Cargo, CargoError))
    {
        Error = FString::Printf(TEXT("cargo setup %s: %s"), *Out.Cargo, *CargoError);
        return false;
    }
    // A cargo setup's grids and docks are chosen for its ship and environment: the scenario's must be those.
    if (!Cargo.Ship.IsEmpty() && !Cargo.Ship.Equals(Out.Vehicle, ESearchCase::IgnoreCase))
    {
        Error = FString::Printf(TEXT("cargo setup %s is for the ship %s, not %s"), *Out.Cargo, *Cargo.Ship, *Out.Vehicle);
        return false;
    }
    if (Cargo.bSpace && !Out.Environment.IsEmpty())
    {
        Error = FString::Printf(TEXT("cargo setup %s is in space: the scenario takes no environment"), *Out.Cargo);
        return false;
    }
    if (!Cargo.bSpace && !Cargo.Environment.IsEmpty() && !NormalPath(Cargo.Environment).Equals(Out.Environment, ESearchCase::IgnoreCase))
    {
        Error = FString::Printf(TEXT("cargo setup %s is for the environment %s, not %s"), *Out.Cargo, *Cargo.Environment,
                                Out.Environment.IsEmpty() ? TEXT("the map's own hangar") : *Out.Environment);
        return false;
    }
    return true;
}

FString FUCScenario::PlayerLevel() const
{
    return USCViewerSubsystem::PlayerLevelPath(PlayerGender, PlayerLoadout, PlayerWeapons, bPlayerSpares);
}

bool FUCScenario::Exists(const FString& Id)
{
    return !Id.IsEmpty() && FPaths::FileExists(FPaths::Combine(Directory(), Id + TEXT(".json")));
}

bool FUCScenario::Load(const FString& Id, FUCScenario& Out, FString& Error)
{
    if (Id.IsEmpty() || Id.Contains(TEXT("/")) || Id.Contains(TEXT("\\")) || Id.Contains(TEXT("..")))
    {
        Error = FString::Printf(TEXT("\"%s\" is no scenario name (a file name under Config/Scenarios, without .json)"), *Id);
        return false;
    }
    const FString File = FPaths::Combine(Directory(), Id + TEXT(".json"));
    FString Json;
    if (!FFileHelper::LoadFileToString(Json, *File))
    {
        Error = TEXT("no scenario ") + File;
        return false;
    }
    // The cargo setup it names, read the way UUCSettings resolves a bare name (Config/Cargo/<name>.json).
    FString CargoJson;
    TSharedPtr<FJsonObject> Root;
    if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) && Root)
        if (const FString Cargo = Str(Root, TEXT("cargo")); !Cargo.IsEmpty())
        {
            const FString CargoFile = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("Cargo"), Cargo + TEXT(".json"));
            if (Cargo.Contains(TEXT("/")) || Cargo.Contains(TEXT("\\")) || !FFileHelper::LoadFileToString(CargoJson, *CargoFile))
            {
                Error = FString::Printf(TEXT("%s: no cargo setup %s"), *File, *CargoFile);
                return false;
            }
        }
    if (!Parse(Id, Json, CargoJson, Out, Error))
    {
        Error = File + TEXT(": ") + Error;
        return false;
    }
    return true;
}

FString FUCPrepared::StampFile(const FString& Level)
{
    return FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / Level.RightChop(FString(TEXT("/Game/")).Len()) + TEXT(".cookformat"));
}
bool FUCPrepared::IsLevelCurrent(const FString& Level)
{
    if (Level.IsEmpty() || !FPackageName::DoesPackageExist(Level)) return false;
    if (FPlatformProperties::RequiresCookedData()) return true;
    // A complete stamp: the format on its first line, the archive build on its second and the save's id on its third.
    TArray<FString> Lines;
    return FFileHelper::LoadFileToStringArray(Lines, *StampFile(Level)) && Lines.Num() >= 3 && Lines[0].TrimStartAndEnd().IsNumeric() &&
           FCString::Atoi(*Lines[0]) == FormatVersion && !Lines[1].TrimStartAndEnd().IsEmpty() && !Lines[2].TrimStartAndEnd().IsEmpty();
}
FString FUCPrepared::LevelBuild(const FString& Level)
{
    TArray<FString> Lines;
    if (Level.IsEmpty() || !FFileHelper::LoadFileToStringArray(Lines, *StampFile(Level)) || Lines.Num() < 2) return FString();
    return Lines[1].TrimStartAndEnd();
}
FString FUCPrepared::LevelId(const FString& Level)
{
    TArray<FString> Lines;
    if (Level.IsEmpty() || !FFileHelper::LoadFileToStringArray(Lines, *StampFile(Level)) || Lines.Num() < 3) return FString();
    return Lines[2].TrimStartAndEnd();
}
FString FUCPrepared::TraversalSources() const
{
    // Every level the record names has an id (only an absent environment leaves its field empty); else there are none.
    const FString Body = LevelId(PlayerLevel), Vehicle = LevelId(VehicleLevel), Environment = LevelId(EnvironmentLevel);
    if (Body.IsEmpty() || Vehicle.IsEmpty() || (!EnvironmentLevel.IsEmpty() && Environment.IsEmpty())) return FString();
    return FString::Printf(TEXT("%s|%s|%s"), *Body, *Vehicle, *Environment);
}
FString FUCPrepared::CargoLevelPath(const FString& Entity)
{
    // Readable from the entity, told apart by a hash of the name exactly as the setup writes it and the export is asked for
    // (entity classes ignore case): names that differ only in characters a level name drops, spaces included, are other
    // levels.
    FString Name = Entity.TrimStartAndEnd();
    for (TCHAR& C : Name)
        if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
    return Name.IsEmpty() ? FString() : FString::Printf(TEXT("/Game/SC/Cargo/Maps/%s_%08x"), *Name, FCrc::StrCrc32(*Entity.ToLower()));
}
FString FUCPrepared::RecordedSources(const FString& TraversalClips)
{
    TArray<FString> Lines;
    if (TraversalClips.IsEmpty() || !FFileHelper::LoadFileToStringArray(Lines, *StampFile(TraversalClips)) || Lines.Num() < 4) return FString();
    return Lines[3].TrimStartAndEnd();
}
FString FUCPrepared::Directory()
{
    return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("SCPrepared")));
}
FString FUCPrepared::File(const FString& Id)
{
    return FPaths::Combine(Directory(), Id + TEXT(".json"));
}
FString FUCPrepared::PrepareCommand(const FString& Id)
{
    return FString::Printf(TEXT("UnrealEditor-Cmd \"%s\" -run=SCPrepare -Scenario=%s -AllowCommandletRendering -unattended"),
                           *FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()), *Id);
}

bool FUCPrepared::Parse(const FString& Json, FUCPrepared& Out, FString& Error)
{
    Out = FUCPrepared();
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root)
    {
        Error = TEXT("invalid JSON");
        return false;
    }
    double Format = 0;
    Root->TryGetNumberField(TEXT("format"), Format);
    Out.Format = int32(Format);
    Out.Scenario = Str(Root, TEXT("scenario"));
    Root->TryGetStringField(TEXT("definition"), Out.Definition); // as written: compared with the file byte for byte
    Root->TryGetStringField(TEXT("cargo_definition"), Out.CargoDefinition);
    Out.EnvironmentLevel = Str(Root, TEXT("environment_level"));
    Out.VehicleLevel = Str(Root, TEXT("vehicle_level"));
    Out.PlayerLevel = Str(Root, TEXT("player_level"));
    Out.TraversalClips = Str(Root, TEXT("traversal_clips"));
    const TSharedPtr<FJsonObject>* Cargo = nullptr;
    if (Root->TryGetObjectField(TEXT("cargo_levels"), Cargo))
        for (const auto& [Entity, Value] : (*Cargo)->Values)
            if (FString Level; Value && Value->TryGetString(Level)) Out.CargoLevels.Add(FString(Entity), Level);
    Root->TryGetStringField(TEXT("tractor_record"), Out.TractorRecordJson);
    Root->TryGetStringField(TEXT("cargo_fx"), Out.CargoFxJson);
    Out.Archive = Str(Root, TEXT("archive"));
    Out.PreparedAt = Str(Root, TEXT("prepared_at"));
    if (Out.Scenario.IsEmpty() || Out.Definition.IsEmpty() || Out.VehicleLevel.IsEmpty())
    {
        Error = TEXT("not a prepared-scenario record (no scenario, definition or vehicle level)");
        return false;
    }
    return true;
}

FString FUCPrepared::ToJson() const
{
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("format"), Format);
    Root->SetStringField(TEXT("scenario"), Scenario);
    Root->SetStringField(TEXT("environment_level"), EnvironmentLevel);
    Root->SetStringField(TEXT("vehicle_level"), VehicleLevel);
    Root->SetStringField(TEXT("player_level"), PlayerLevel);
    Root->SetStringField(TEXT("traversal_clips"), TraversalClips);
    const TSharedRef<FJsonObject> Cargo = MakeShared<FJsonObject>();
    for (const auto& [Entity, Level] : CargoLevels) Cargo->SetStringField(Entity, Level);
    Root->SetObjectField(TEXT("cargo_levels"), Cargo);
    Root->SetStringField(TEXT("tractor_record"), TractorRecordJson);
    Root->SetStringField(TEXT("cargo_fx"), CargoFxJson);
    Root->SetStringField(TEXT("archive"), Archive);
    Root->SetStringField(TEXT("prepared_at"), PreparedAt);
    Root->SetStringField(TEXT("definition"), Definition);
    Root->SetStringField(TEXT("cargo_definition"), CargoDefinition);
    FString Out;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Root, Writer);
    return Out;
}

bool FUCPrepared::Load(const FString& Id, FUCPrepared& Out, FString& Error)
{
    FString Json;
    if (!FFileHelper::LoadFileToString(Json, *File(Id)))
    {
        Error = FString::Printf(TEXT("scenario %s is not prepared (no %s): run %s"), *Id, *File(Id), *PrepareCommand(Id));
        return false;
    }
    if (!Parse(Json, Out, Error))
    {
        Error = FString::Printf(TEXT("%s: %s: run %s"), *File(Id), *Error, *PrepareCommand(Id));
        return false;
    }
    return true;
}

bool FUCPrepared::Save(FString& Error) const
{
    if (!FFileHelper::SaveStringToFile(ToJson(), *File(Scenario), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        Error = TEXT("cannot write ") + File(Scenario);
        return false;
    }
    return true;
}

FString FUCPrepared::Problem(const FUCScenario* Source, TFunctionRef<bool(const FString& Level)> LevelCurrent) const
{
    const FString Fix = TEXT(": run ") + PrepareCommand(Scenario);
    if (Format != FormatVersion)
        return FString::Printf(TEXT("scenario %s was prepared with format %d, the game reads %d%s"), *Scenario, Format, FormatVersion, *Fix);
    if (!LevelCurrent(VehicleLevel))
        return FString::Printf(TEXT("scenario %s: the prepared vehicle level %s is missing or not finished%s"), *Scenario, *VehicleLevel, *Fix);
    if (!EnvironmentLevel.IsEmpty() && !LevelCurrent(EnvironmentLevel))
        return FString::Printf(TEXT("scenario %s: the prepared environment level %s is missing or not finished%s"), *Scenario, *EnvironmentLevel, *Fix);
    if (PlayerLevel.IsEmpty() || !LevelCurrent(PlayerLevel))
        return FString::Printf(TEXT("scenario %s: the prepared player body level %s is missing or not finished%s"), *Scenario,
                               PlayerLevel.IsEmpty() ? TEXT("(none named)") : *PlayerLevel, *Fix);
    // Every box entity of the prepared cargo setup has its level (a record missing one would send those boxes to the archive).
    if (!CargoDefinition.IsEmpty())
    {
        FSCCargoScenario Cargo;
        FString CargoError;
        if (!FSCCargoScenario::Parse(CargoDefinition, Cargo, CargoError))
            return FString::Printf(TEXT("scenario %s: the prepared cargo setup does not read (%s)%s"), *Scenario, *CargoError, *Fix);
        const TArray<FString> Entities = Cargo.Entities();
        for (const FString& Entity : Entities)
            if (CargoLevels.FindRef(Entity) != CargoLevelPath(Entity))
                return FString::Printf(TEXT("scenario %s: the cargo box %s has no prepared level in the record%s"), *Scenario, *Entity, *Fix);
        // And nothing else: a mapping the setup does not name would serve a live spawn of that entity another box.
        for (const auto& [Entity, Level] : CargoLevels)
            if (!Entities.Contains(Entity))
                return FString::Printf(TEXT("scenario %s: the record maps a cargo box %s its setup does not name%s"), *Scenario, *Entity, *Fix);
    }
    else if (!CargoLevels.IsEmpty())
    {
        return FString::Printf(TEXT("scenario %s: the record maps cargo boxes without a cargo setup%s"), *Scenario, *Fix);
    }
    for (const auto& [Entity, Level] : CargoLevels)
        if (!LevelCurrent(Level))
            return FString::Printf(TEXT("scenario %s: the prepared cargo box level %s (%s) is missing or not finished%s"), *Scenario, *Level, *Entity, *Fix);
    if (TraversalClips.IsEmpty() || !LevelCurrent(TraversalClips))
        return FString::Printf(TEXT("scenario %s: the prepared traversal clips %s are missing or not finished%s"), *Scenario,
                               TraversalClips.IsEmpty() ? TEXT("(none named)") : *TraversalClips, *Fix);
    // Character for character (FString's == ignores case), line ends aside: any edit since prep makes the record stale.
    if (Source && !Comparable(Source->Json).Equals(Comparable(Definition), ESearchCase::CaseSensitive))
        return FString::Printf(TEXT("scenario %s changed since it was prepared%s"), *Scenario, *Fix);
    if (Source && !Comparable(Source->CargoJson).Equals(Comparable(CargoDefinition), ESearchCase::CaseSensitive))
        return FString::Printf(TEXT("the cargo setup of scenario %s changed since it was prepared%s"), *Scenario, *Fix);
    return FString();
}
FString FUCPrepared::Problem(const FUCScenario* Source) const
{
    // The traversal clips also follow the levels they were read from (a packaged game has no stamps: packaging checks it).
    return Problem(Source, [this](const FString& Level) {
        if (!IsLevelCurrent(Level)) return false;
        if (Level != TraversalClips || FPlatformProperties::RequiresCookedData()) return true;
        const FString Sources = TraversalSources();
        return !Sources.IsEmpty() && RecordedSources(Level) == Sources;
    });
}
