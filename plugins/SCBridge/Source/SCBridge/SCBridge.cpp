#include "Modules/ModuleManager.h"
#include "SCImporter.h"
#include "SCBridgeClient.h"
#include "SCImport.h"
#include "SCAudioSource.h"
ISCImporting& SCImportingInstance();
FSCAudioSourcePtr MakeImporterOggSource(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Ogg, FString& Error);
#include "Private/SCImportHarness.h"
#include "HAL/IConsoleManager.h"
#include "EngineUtils.h"
#include "SCVehicle.h"
#if SC_SPIKE && WITH_EDITOR
namespace SCSpike { void Start(); void Stop(); }
#endif
class FSCBridgeModule : public IModuleInterface
{
    FSCImportHarness Harness;
    TUniquePtr<FAutoConsoleCommandWithWorldAndArgs> ImportCommand, GlowCommand;
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
        GlowCommand = MakeUnique<FAutoConsoleCommandWithWorldAndArgs>(TEXT("sc.EngineGlow"), TEXT("sc.EngineGlow <strength>: main-thruster emission (scene controls.engine_glow range)"),
            FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
            {
                if (!Args.IsEmpty()) for (TActorIterator<ASCVehicle> It(World); It; ++It) It->SetEngineGlow(FCString::Atof(*Args[0]));
            }));
        Harness.Start();
        // The archive the runtime asks for (ISCArchive::Create): the library's client.
        ISCArchive::SetFactory([] { return TSharedPtr<ISCArchive, ESPMode::ThreadSafe>(MakeShared<FSCBridgeClient, ESPMode::ThreadSafe>()); });
        ISCImporting::Set(&SCImportingInstance());
        SetOggSourceFactory(&MakeImporterOggSource);
    }
    virtual void ShutdownModule() override
    {
        SetOggSourceFactory(nullptr);
        ISCImporting::Set(nullptr);
        ISCArchive::SetFactory(nullptr);
        Harness.Stop(); ImportCommand.Reset(); GlowCommand.Reset();
#if SC_SPIKE && WITH_EDITOR
        SCSpike::Stop();
#endif
    }
};
IMPLEMENT_MODULE(FSCBridgeModule, SCBridge);
