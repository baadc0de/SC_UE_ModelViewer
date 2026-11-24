// WP-O1-4c-3b: the importer's module: it registers the archive (ISCArchive), the importer (ISCImporting) and the Ogg
// decoder (MakeOggSource) the runtime asks for, runs the import harness (-SCImport...) and the sc.Import command.
#include "Modules/ModuleManager.h"
#include "SCImporter.h"
#include "SCBridgeClient.h"
#include "SCImport.h"
#include "SCAudioSource.h"
#include "Private/SCImportHarness.h"
#include "HAL/IConsoleManager.h"
ISCImporting& SCImportingInstance();
FSCAudioSourcePtr MakeImporterOggSource(const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Ogg, FString& Error);
#if SC_SPIKE && WITH_EDITOR
namespace SCSpike { void Start(); void Stop(); }
#endif
class FSCBridgeImportModule : public IModuleInterface
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
        Harness.Stop();
        ImportCommand.Reset();
#if SC_SPIKE && WITH_EDITOR
        SCSpike::Stop();
#endif
    }
    // Its objects (archive clients, import jobs, caches, Ogg decoders) live in runtime objects until the world goes; the
    // module stays loaded for the process.
    virtual bool SupportsDynamicReloading() override { return false; }
};
IMPLEMENT_MODULE(FSCBridgeImportModule, SCBridgeImport);
