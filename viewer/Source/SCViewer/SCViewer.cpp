#include "CoreGlobals.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"

class FSCViewerModule final : public FDefaultGameModuleImpl
{
    void StartupModule() override
    {
        FDefaultGameModuleImpl::StartupModule();
#if WITH_EDITOR
        // The uncooked game uses the runtime importer. Skip editor Python initialization and its
        // collection callback in this host. Explicit Python launch options stay available.
        const TCHAR* CommandLine = FCommandLine::Get();
        if (!GIsEditor && !IsRunningCommandlet() && FParse::Param(CommandLine, TEXT("game")) &&
            !FParse::Param(CommandLine, TEXT("EnablePython")) &&
            !FParse::Param(CommandLine, TEXT("ForceEnablePython")) &&
            !FParse::Param(CommandLine, TEXT("DisablePython")))
        {
            FCommandLine::Append(TEXT(" -DisablePython"));
        }
#endif
    }
};

IMPLEMENT_PRIMARY_GAME_MODULE(FSCViewerModule, SCViewer, "SCViewer");
