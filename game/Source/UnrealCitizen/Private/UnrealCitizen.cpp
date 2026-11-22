// UnrealCitizen's game module (WP-O1-2): registers what the game adds to the shared viewer shell (SCViewerShell.h): the
// mobiGlas as the F1 menu, the mobiGlas and the world callouts as view overlays, the garage kiosk as a first-person
// interaction, and the cargo, Ship Link and Garage routes of the RPC harness.
#include "CoreGlobals.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "SCFirstPersonCharacter.h"
#include "SCInteractionPrompt.h"
#include "SCVehicle.h"
#include "SCViewerShell.h"
#include "SCViewerSubsystem.h"
#include "UCRpcRoutes.h"
#include "Cargo/SCCargoSubsystem.h"
#include "UI/Cargo/SCCargoViewSource.h"
#include "UI/Cargo/SCMobiGlasModel.h"
#include "UI/Cargo/SSCCalloutLayer.h"
#include "UI/Cargo/SSCMobiGlas.h"
#include "Widgets/SOverlay.h"

#define LOCTEXT_NAMESPACE "UnrealCitizen"

namespace
{
/** WP-M2: the hangar's vehicle kiosk (ASCVehicle::GarageKiosks): "Open Garage" opens the mobiGlas on its Garage app. */
class FGarageKioskInteraction final : public ISCInteractionProvider
{
public:
    bool BuildPrompt(const ASCFirstPersonCharacter& Character, const FVector& Eye, const FVector& View, float Reach,
                     FSCInteractionPrompt& Out) const override
    {
        if (!Aimed(Character, Eye, View, Reach)) return false;
        Out.Key = TEXT("garage");
        Out.Options.Add({LOCTEXT("OpenGarage", "Open Garage").ToString(), true, FString()});
        Out.Anchor = Character.AimPoint(Reach);
        Out.bAnchored = true;
        return true;
    }
    bool Interact(ASCFirstPersonCharacter& Character, const FVector& Eye, const FVector& View, float Reach, int32) override
    {
        if (!Aimed(Character, Eye, View, Reach)) return false;
        FSCMobiGlasRequests::Get().App = TEXT("Garage");
        SCViewerShell::SetMenuOpen(USCViewerSubsystem::Get(&Character), true);
        return true;
    }

private:
    static bool Aimed(const ASCFirstPersonCharacter& Character, const FVector& Eye, const FVector& View, float Reach)
    {
        const USCViewerSubsystem* Viewer = USCViewerSubsystem::Get(&Character);
        const ASCVehicle* Environment = Viewer ? Viewer->GetEnvironment() : nullptr;
        return Environment && Environment->AimGarageKiosk(Eye, View, Reach, &Character);
    }
};
} // namespace

class FUnrealCitizenModule final : public FDefaultGameModuleImpl
{
    void StartupModule() override
    {
        FDefaultGameModuleImpl::StartupModule();
#if WITH_EDITOR
        // The uncooked game uses the runtime importer. Skip editor Python initialization and its collection callback in
        // this host. Explicit Python launch options stay available.
        const TCHAR* CommandLine = FCommandLine::Get();
        if (!GIsEditor && !IsRunningCommandlet() && FParse::Param(CommandLine, TEXT("game")) && !FParse::Param(CommandLine, TEXT("EnablePython")) &&
            !FParse::Param(CommandLine, TEXT("ForceEnablePython")) && !FParse::Param(CommandLine, TEXT("DisablePython")))
        {
            FCommandLine::Append(TEXT(" -DisablePython"));
        }
#endif
        // WP-K1c: the mobiGlas (F1) and the world-space callouts above it share one model; both show in the showcase too.
        SCViewerShell::SetMenu(LOCTEXT("MobiGlas", "mobiGlas"));
        SCViewerShell::AddOverlay(TEXT("MobiGlas"), [](const FSCViewOverlayArgs& Args) -> TSharedRef<SWidget> {
            const TSharedRef<FSCMobiGlasModel> Model = MakeShared<FSCMobiGlasModel>(Args.Subsystem);
            const TFunction<void()> FocusView = Args.FocusView;
            return SNew(SOverlay)
                + SOverlay::Slot()[SNew(SSCMobiGlas).Model(Model).OnClosed_Lambda([FocusView] { if (FocusView) FocusView(); })]
                + SOverlay::Slot()[SNew(SSCCalloutLayer).Model(Model)];
        });
        SCViewerShell::AddInteraction(TEXT("GarageKiosk"), MakeShared<FGarageKioskInteraction>());
        // WP-K1: a cargo job keeps the access doors it uses open.
        ASCVehicle::AddDoorUseQuery(TEXT("Cargo"), [](const ASCVehicle& Ship, int32 Door) -> FString {
            const USCCargoSubsystem* Cargo = USCCargoSubsystem::Get(&Ship);
            return Cargo && Cargo->IsAccessDoorBusy(&Ship, Door) ? FString(TEXT("Door in use by cargo")) : FString();
        });
        UCRpcRoutes::Register();
    }
    void ShutdownModule() override
    {
        UCRpcRoutes::Unregister();
        ASCVehicle::RemoveDoorUseQuery(TEXT("Cargo"));
        SCViewerShell::RemoveInteraction(TEXT("GarageKiosk"));
        SCViewerShell::RemoveOverlay(TEXT("MobiGlas"));
        FDefaultGameModuleImpl::ShutdownModule();
    }
};

IMPLEMENT_PRIMARY_GAME_MODULE(FUnrealCitizenModule, UnrealCitizen, "UnrealCitizen");

#undef LOCTEXT_NAMESPACE
