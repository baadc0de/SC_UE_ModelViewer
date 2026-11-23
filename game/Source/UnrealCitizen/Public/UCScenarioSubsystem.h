// WP-O1-4: runs UnrealCitizen's scenario (UCScenario.h) from its prepared record: the prepared environment and vehicle
// levels, the camera and where the player starts, the cargo setup and the music. A scenario that is not prepared (or
// whose preparation is stale) is refused: nothing loads, and the view says which prep command to run.
#pragma once
#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UCScenario.h"
#include "UCScenarioSubsystem.generated.h"

class USCViewerSubsystem;

UCLASS()
class UNREALCITIZEN_API UUCScenarioSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    /** None: no scenario asked for (the command line and the menus decide); Refused: not run (Problem says why and what
     *  to do); Loading: its levels stream in; Starting: the camera and the player start; Running; Failed: a level did
     *  not load (Problem). */
    enum class EState : uint8 { None, Refused, Loading, Starting, Running, Failed };
    static UUCScenarioSubsystem* Get(const UObject* WorldContext);
    static const TCHAR* StateName(EState State);
    /** The switch of a command line that names another scene than a scenario's (-SCLoad=, -SCEnvironment=, -SCPackage=,
     *  -SCCargoScenario=, -SCPlayerDemo=<entity>) or turns prepared levels off (-SCNoCooked); empty when none does. */
    static FString ConflictingSwitch(const TCHAR* CommandLine);
    /** The command line's automation puts the camera (-SCCamera=, -SCScript=, -SCPlayerDemo): the scenario then sets neither
     *  its camera nor its start, so the two never switch the view under each other (a route starts once a vehicle is in). */
    static bool AutomationOwnsCamera(const TCHAR* CommandLine);

    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    EState GetState() const { return State; }
    const FString& GetId() const { return Id; }
    const FUCScenario& GetScenario() const { return Scenario; }
    const FUCPrepared& GetPrepared() const { return Prepared; }
    const FString& GetProblem() const { return Problem; }

private:
    bool Tick(float DeltaSeconds);
    /** bClearScene: nothing loads instead (the environment the viewer restored, a cargo setting's ship); false when the
     *  command line names another scene, which then loads as without a scenario. */
    void Refuse(const FString& Why, bool bClearScene = true);
    void Fail(const FString& Why);
    void StepStart(USCViewerSubsystem& Viewer);
    void StepMusic();

    FTSTicker::FDelegateHandle TickHandle;
    EState State = EState::None;
    FString Id, Problem;
    FUCScenario Scenario; // the prepared definition (FUCPrepared::Definition)
    FUCPrepared Prepared;
    bool bVehicleRequested = false;
    bool bCameraSet = false, bStartPlaced = false;
    double StartedAt = 0, LoadingSince = 0;
    bool bMusicPending = false;
    bool bCleanupPending = false; // failed: a vehicle still in the scene goes once nothing loads
};
