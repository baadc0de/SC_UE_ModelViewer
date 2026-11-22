// The viewer's game module (WP-O1-3): names the app, makes the picker (SSCViewerPicker) its F1 menu and view overlay,
// and has ships and environments import from the archive through the local cache (the viewer does not cook).
#include "CoreGlobals.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "SCViewerShell.h"
#include "SSCViewerPicker.h"

#define LOCTEXT_NAMESPACE "SCViewer"

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
        SCViewerShell::SetAppName(LOCTEXT("AppName", "SC Viewer"));
        SCViewerShell::SetCookedLevels(false);
        SCViewerShell::SetMenu(LOCTEXT("Menu", "Menu"));
        SCViewerShell::AddOverlay(TEXT("Picker"), [](const FSCViewOverlayArgs& Args) -> TSharedRef<SWidget> {
            return SNew(SSCViewerPicker).Subsystem(Args.Subsystem).FocusView(Args.FocusView);
        });
    }
    void ShutdownModule() override
    {
        SCViewerShell::RemoveOverlay(TEXT("Picker"));
        SCViewerShell::ClearMenu();
        FDefaultGameModuleImpl::ShutdownModule();
    }
    // The picker built into the viewer root lives as long as the session.
    bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_PRIMARY_GAME_MODULE(FSCViewerModule, SCViewer, "SCViewer");

#undef LOCTEXT_NAMESPACE
