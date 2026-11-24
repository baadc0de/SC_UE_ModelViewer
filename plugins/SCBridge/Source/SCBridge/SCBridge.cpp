#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"
#include "EngineUtils.h"
#include "SCVehicle.h"
class FSCBridgeModule : public IModuleInterface
{
    TUniquePtr<FAutoConsoleCommandWithWorldAndArgs> GlowCommand;
public:
    virtual void StartupModule() override
    {
        GlowCommand = MakeUnique<FAutoConsoleCommandWithWorldAndArgs>(TEXT("sc.EngineGlow"), TEXT("sc.EngineGlow <strength>: main-thruster emission (scene controls.engine_glow range)"),
            FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
            {
                if (!Args.IsEmpty()) for (TActorIterator<ASCVehicle> It(World); It; ++It) It->SetEngineGlow(FCString::Atof(*Args[0]));
            }));
    }
    virtual void ShutdownModule() override { GlowCommand.Reset(); }
};
IMPLEMENT_MODULE(FSCBridgeModule, SCBridge);
