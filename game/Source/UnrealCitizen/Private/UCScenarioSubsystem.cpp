// UUCScenarioSubsystem (UCScenarioSubsystem.h): a scenario run from its prepared record.
#include "UCScenarioSubsystem.h"
#include "SCFirstPersonCharacter.h"
#include "SCMusicSubsystem.h"
#include "SCVehicle.h"
#include "SCViewerSubsystem.h"
#include "UCSettings.h"
#include "Cargo/SCCargoSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogUCScenario, Log, All);

namespace
{
// How long the run tries to put the player at the scenario's start (first person and the teleport destinations appear
// once the levels' collision and the hangar's standing places are ready).
constexpr double StartPlacementSeconds = 20.0;
// How long the prepared levels may take to stream in before the run fails (an extra-large hangar streams in well under a
// minute from a local disk).
constexpr double LoadingSeconds = 600.0;
} // namespace

UUCScenarioSubsystem* UUCScenarioSubsystem::Get(const UObject* WorldContext)
{
    const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
    const UGameInstance* Instance = World ? World->GetGameInstance() : nullptr;
    return Instance ? Instance->GetSubsystem<UUCScenarioSubsystem>() : nullptr;
}

const TCHAR* UUCScenarioSubsystem::StateName(EState InState)
{
    switch (InState)
    {
    case EState::None: return TEXT("none");
    case EState::Refused: return TEXT("refused");
    case EState::Loading: return TEXT("loading");
    case EState::Starting: return TEXT("starting");
    case EState::Running: return TEXT("running");
    case EState::Failed: return TEXT("failed");
    }
    return TEXT("none");
}

bool UUCScenarioSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    return !IsRunningCommandlet(); // the prep step's headless viewer runs no scenario
}

void UUCScenarioSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    USCViewerSubsystem* Viewer = Collection.InitializeDependency<USCViewerSubsystem>();
    Id = UUCSettings::Get()->ResolveScenario();
    if (Id.IsEmpty() || !Viewer) return;
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UUCScenarioSubsystem::Tick));
    // A scenario puts its own scene in the world: with a switch that names another one it does not run (not mixed in), and
    // the switches act as they do without a scenario.
    const TCHAR* Cmd = FCommandLine::Get();
    FString Ignored;
    for (const TCHAR* Switch : {TEXT("SCLoad="), TEXT("SCEnvironment="), TEXT("SCPackage="), TEXT("SCCargoScenario=")})
        if (FParse::Value(Cmd, Switch, Ignored))
            return Refuse(FString::Printf(TEXT("-Scenario=%s runs alone: drop -%s"), *Id, *FString(Switch).LeftChop(1)), false);
    if (FParse::Param(Cmd, TEXT("SCNoCooked"))) // a scenario runs from its prepared (cooked) levels only
        return Refuse(FString::Printf(TEXT("-Scenario=%s runs from prepared levels: drop -SCNoCooked"), *Id), false);
    // The definition on disk, when there is one (not in a packaged game): its title names a refused run, and a change since
    // prep makes the record stale.
    FUCScenario Source;
    FString Error, SourceError;
    const bool bSource = FUCScenario::Load(Id, Source, SourceError);
    if (bSource) Scenario.Title = Source.Title;
    // Only a packaged game runs without its definition (Config/Scenarios is not staged); anywhere else a definition that
    // is missing or does not read is refused, as packaging refuses it.
    else if (!FPlatformProperties::RequiresCookedData() || FUCScenario::Exists(Id))
        return Refuse(SourceError + TEXT(": fix the definition, then run ") + FUCPrepared::PrepareCommand(Id));
    if (!FUCPrepared::Load(Id, Prepared, Error)) return Refuse(Error);
    if (const FString Why = Prepared.Problem(bSource ? &Source : nullptr); !Why.IsEmpty()) return Refuse(Why);
    if (!FUCScenario::Parse(Id, Prepared.Definition, Prepared.CargoDefinition, Scenario, Error))
        return Refuse(FString::Printf(TEXT("scenario %s: the prepared definition does not read (%s): run %s"), *Id, *Error, *FUCPrepared::PrepareCommand(Id)));
    // The cargo setup comes from the record, and the scenario owns the scene (the cargo subsystem loads nothing itself).
    UUCSettings* Settings = UUCSettings::Get();
    Settings->bScenarioOwnsScene = true;
    Settings->ScenarioCargoName = Scenario.Cargo;
    Settings->ScenarioCargoJson = Scenario.CargoJson;
    Settings->ScenarioVehicle = Scenario.Vehicle;
    // The environment before anything else is requested (it replaces a saved choice the viewer restored at start).
    Viewer->SetEnvironment(Scenario.Environment.IsEmpty() ? FString(TEXT("none")) : Scenario.Environment);
    State = EState::Loading;
    LoadingSince = FPlatformTime::Seconds();
    UE_LOG(LogUCScenario, Display, TEXT("SCENARIO start id=%s title=\"%s\" vehicle=%s level=%s environment=%s cargo=%s music=%s camera=%s start=\"%s\" prepared=%s archive=%s"),
           *Id, *Scenario.Title, *Scenario.Vehicle, *Prepared.VehicleLevel, Prepared.EnvironmentLevel.IsEmpty() ? TEXT("none") : *Prepared.EnvironmentLevel,
           Scenario.Cargo.IsEmpty() ? TEXT("none") : *Scenario.Cargo, Scenario.Music.IsEmpty() ? TEXT("none") : *Scenario.Music, *Scenario.Camera,
           *Scenario.Start, *Prepared.PreparedAt, *Prepared.Archive);
}

void UUCScenarioSubsystem::Deinitialize()
{
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    UUCSettings* Settings = UUCSettings::Get();
    Settings->bScenarioOwnsScene = false;
    Settings->ScenarioCargoName.Reset();
    Settings->ScenarioCargoJson.Reset();
    Settings->ScenarioVehicle.Reset();
    Super::Deinitialize();
}

void UUCScenarioSubsystem::Refuse(const FString& Why, bool bClearScene)
{
    State = EState::Refused;
    Problem = Why;
    UE_LOG(LogUCScenario, Error, TEXT("SCENARIO_REFUSED id=%s %s"), *Id, *Why);
    if (!bClearScene) return;
    // Nothing loads instead: not the environment the viewer restored at start, nor a ship a cargo setting names.
    UUCSettings::Get()->bScenarioOwnsScene = true;
    if (USCViewerSubsystem* Viewer = GetGameInstance() ? GetGameInstance()->GetSubsystem<USCViewerSubsystem>() : nullptr)
        Viewer->SetEnvironment(TEXT("none"));
}

void UUCScenarioSubsystem::Fail(const FString& Why)
{
    State = EState::Failed;
    Problem = Why;
    // What the run started stops and goes: a level still streaming, the environment, and the cargo setup not applied yet.
    UUCSettings::Get()->ScenarioCargoJson.Reset();
    if (USCCargoSubsystem* Cargo = USCCargoSubsystem::Get(GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr)) Cargo->DropScenario();
    if (USCViewerSubsystem* Viewer = GetGameInstance() ? GetGameInstance()->GetSubsystem<USCViewerSubsystem>() : nullptr)
    {
        Viewer->CancelLoad();
        if (Viewer->IsSpace()) Viewer->SetSpace(false); // a cargo setup in space took the world there: the map's hangar and gravity return
        Viewer->SetEnvironment(TEXT("none"));
    }
    UE_LOG(LogUCScenario, Error, TEXT("SCENARIO_FAILED id=%s %s"), *Id, *Why);
}

bool UUCScenarioSubsystem::Tick(float)
{
    USCViewerSubsystem* Viewer = GetGameInstance() ? GetGameInstance()->GetSubsystem<USCViewerSubsystem>() : nullptr;
    if (!Viewer || !GetGameInstance()->GetWorld()) return true;
    switch (State)
    {
    case EState::Loading:
        if (FPlatformTime::Seconds() - LoadingSince > LoadingSeconds)
        {
            Fail(FString::Printf(TEXT("the prepared levels did not load within %.0f s"), LoadingSeconds));
            return true;
        }
        // The environment streams first (a load waits for it: IsBusy), then the vehicle's prepared level.
        if (!bVehicleRequested)
        {
            if (Viewer->IsBusy()) return true;
            if (!Prepared.EnvironmentLevel.IsEmpty() && !Viewer->GetEnvironment())
            {
                Fail(FString::Printf(TEXT("the environment level %s did not load"), *Prepared.EnvironmentLevel));
                return true;
            }
            bVehicleRequested = Viewer->LoadPreparedEntity(Scenario.Vehicle, Prepared.VehicleLevel);
            if (!bVehicleRequested) Fail(FString::Printf(TEXT("the vehicle level %s did not start loading"), *Prepared.VehicleLevel));
            return true;
        }
        if (Viewer->GetLoadPhase() == ESCLoadPhase::Failed || Viewer->GetLoadPhase() == ESCLoadPhase::Cancelled)
        {
            Fail(FString::Printf(TEXT("the vehicle level %s did not load: %s"), *Prepared.VehicleLevel, *Viewer->GetLastReport().Error));
            return true;
        }
        if (Viewer->GetLoadPhase() != ESCLoadPhase::Succeeded || Viewer->IsBusy()) return true;
        State = EState::Starting;
        StartedAt = FPlatformTime::Seconds();
        UE_LOG(LogUCScenario, Display, TEXT("SCENARIO loaded id=%s vehicle=%s environment=%s"), *Id, *Viewer->GetLoadedEntity(),
               Viewer->GetEnvironment() ? *Viewer->GetEnvironment()->GetName() : TEXT("none"));
        return true;
    case EState::Starting:
        StepStart(*Viewer);
        return true;
    case EState::Running:
        StepMusic();
        return true;
    default:
        return true;
    }
}

void UUCScenarioSubsystem::StepStart(USCViewerSubsystem& Viewer)
{
    const bool bFirst = Scenario.Camera == TEXT("first");
    const bool bTimedOut = FPlatformTime::Seconds() - StartedAt > StartPlacementSeconds;
    if (!bCameraSet)
    {
        const ESCCameraMode Mode = bFirst ? ESCCameraMode::FirstPerson : Scenario.Camera == TEXT("fly") ? ESCCameraMode::Fly : ESCCameraMode::Orbit;
        bCameraSet = Viewer.SetCameraMode(Mode) && (!bFirst || Viewer.GetFirstPerson());
        if (!bCameraSet && !bTimedOut) return; // first person waits for the vehicle's collision
        if (!bCameraSet) UE_LOG(LogUCScenario, Warning, TEXT("SCENARIO camera %s did not take"), *Scenario.Camera);
    }
    ASCFirstPersonCharacter* Character = bFirst ? Viewer.GetFirstPerson() : nullptr;
    if (!bStartPlaced && Character && !Scenario.Start.IsEmpty())
    {
        const TArray<FSCTeleportRow> Rows = Character->TeleportRows();
        for (int32 Row = 0; Row < Rows.Num(); ++Row)
            if (Character->DestinationLabel(Rows[Row]).Contains(Scenario.Start) && Character->TeleportToRow(Row))
            {
                bStartPlaced = true;
                UE_LOG(LogUCScenario, Display, TEXT("SCENARIO start_at=\"%s\" (\"%s\")"), *Character->DestinationLabel(Rows[Row]), *Scenario.Start);
                break;
            }
        if (!bStartPlaced && !bTimedOut) return; // the destinations list once the hangar's standing places are checked
        if (!bStartPlaced)
        {
            TArray<FString> Labels;
            for (const FSCTeleportRow& Row : Rows) Labels.Add(Character->DestinationLabel(Row));
            UE_LOG(LogUCScenario, Warning, TEXT("SCENARIO start \"%s\" names no teleport destination (%s): the default start"), *Scenario.Start,
                   *FString::Join(Labels, TEXT(", ")));
        }
    }
    bMusicPending = !Scenario.Music.IsEmpty();
    if (USCMusicSubsystem* Music = bMusicPending ? USCMusicSubsystem::Get(GetGameInstance()->GetWorld()) : nullptr) Music->RequestCatalogue();
    State = EState::Running;
    UE_LOG(LogUCScenario, Display, TEXT("SCENARIO running id=%s camera=%d seconds=%.1f"), *Id, int32(Viewer.GetCameraMode()), FPlatformTime::Seconds() - StartedAt);
}

void UUCScenarioSubsystem::StepMusic()
{
    if (!bMusicPending) return;
    USCMusicSubsystem* Music = USCMusicSubsystem::Get(GetGameInstance()->GetWorld());
    if (!Music) return;
    if (!Music->GetError().IsEmpty() && !Music->HasCatalogue())
    {
        bMusicPending = false;
        UE_LOG(LogUCScenario, Warning, TEXT("SCENARIO music %s: %s"), *Scenario.Music, *Music->GetError().ToString());
        return;
    }
    if (!Music->HasCatalogue()) return;
    bMusicPending = false;
    const FSCMusicCue* Cue = Music->GetCues().FindByPredicate([this](const FSCMusicCue& C) {
        return C.Id == Scenario.Music || C.Title.Equals(Scenario.Music, ESearchCase::IgnoreCase);
    });
    if (Cue && Music->Play(Cue->Id))
    {
        UE_LOG(LogUCScenario, Display, TEXT("SCENARIO music id=%s title=\"%s\""), *Cue->Id, *Cue->Title);
    }
    else
    {
        UE_LOG(LogUCScenario, Warning, TEXT("SCENARIO music %s names no soundtrack cue (%d in the catalogue)"), *Scenario.Music, Music->GetCues().Num());
    }
}
