// FUCScenario and FUCPrepared (UCScenario.h): reading scenario definitions and the prep step's records.
#include "UCScenario.h"
#include "Cargo/SCCargoScenario.h"
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
    const TSharedPtr<FJsonObject>* Player = nullptr;
    if (Root->TryGetObjectField(TEXT("player"), Player))
    {
        if (const FString Camera = Str(*Player, TEXT("camera")).ToLower(); !Camera.IsEmpty()) Out.Camera = Camera;
        Out.Start = Str(*Player, TEXT("start"));
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
    // A complete stamp: the format on its first line and the archive build on its second.
    TArray<FString> Lines;
    return FFileHelper::LoadFileToStringArray(Lines, *StampFile(Level)) && Lines.Num() >= 2 && Lines[0].TrimStartAndEnd().IsNumeric() &&
           FCString::Atoi(*Lines[0]) == FormatVersion && !Lines[1].TrimStartAndEnd().IsEmpty();
}
FString FUCPrepared::LevelBuild(const FString& Level)
{
    TArray<FString> Lines;
    if (Level.IsEmpty() || !FFileHelper::LoadFileToStringArray(Lines, *StampFile(Level)) || Lines.Num() < 2) return FString();
    return Lines[1].TrimStartAndEnd();
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
    // Byte for byte (FString's == ignores case): any edit since prep makes the record stale.
    if (Source && !Source->Json.Equals(Definition, ESearchCase::CaseSensitive))
        return FString::Printf(TEXT("scenario %s changed since it was prepared%s"), *Scenario, *Fix);
    if (Source && !Source->CargoJson.Equals(CargoDefinition, ESearchCase::CaseSensitive))
        return FString::Printf(TEXT("the cargo setup of scenario %s changed since it was prepared%s"), *Scenario, *Fix);
    return FString();
}
FString FUCPrepared::Problem(const FUCScenario* Source) const
{
    return Problem(Source, [](const FString& Level) { return IsLevelCurrent(Level); });
}
