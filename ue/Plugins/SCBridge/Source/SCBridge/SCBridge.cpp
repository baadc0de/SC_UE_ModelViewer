#include "Modules/ModuleManager.h"
#include "SCImporter.h"
#include "Private/SCImportHarness.h"
#include "HAL/IConsoleManager.h"
#if SC_SPIKE && WITH_EDITOR
namespace SCSpike { void Start(); void Stop(); }
#endif
class FSCBridgeModule : public IModuleInterface
{
    FSCImportHarness Harness;
    TUniquePtr<FAutoConsoleCommandWithWorldAndArgs> ImportCommand;
public:
    virtual void StartupModule() override
    {
#if SC_SPIKE && WITH_EDITOR
        SCSpike::Start();
#endif
        ImportCommand = MakeUnique<FAutoConsoleCommandWithWorldAndArgs>(TEXT("sc.Import"), TEXT("sc.Import \"package folder\""),
            FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
            {
                // Console tokenization preserves quoted arguments; joining also accepts
                // unquoted folder paths with spaces. Strip outer quotes if still present.
                if (!Args.IsEmpty()) USCImportLibrary::ImportFolder(World, FString::Join(Args, TEXT(" ")).TrimQuotes());
            }));
        Harness.Start();
    }
    virtual void ShutdownModule() override
    {
        Harness.Stop(); ImportCommand.Reset();
#if SC_SPIKE && WITH_EDITOR
        SCSpike::Stop();
#endif
    }
};
IMPLEMENT_MODULE(FSCBridgeModule, SCBridge);
