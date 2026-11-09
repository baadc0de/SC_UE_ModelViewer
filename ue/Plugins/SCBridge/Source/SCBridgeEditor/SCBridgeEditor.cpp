#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Editor.h"
#include "SCImporter.h"
#include "SCViewerSubsystem.h"
#include "Private/SCKeepAssets.h"
#include "Framework/Application/SlateApplication.h"
class FSCBridgeEditorModule : public IModuleInterface
{
public:
    void StartupModule() override
    {
        // Editor host extra: the Runtime UI shows "Keep as assets" while this handler is bound.
        USCViewerSubsystem::KeepHandler().BindStatic(&SCKeepAssets::Keep);
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FSCBridgeEditorModule::RegisterMenus));
    }
    void ShutdownModule() override
    {
        USCViewerSubsystem::KeepHandler().Unbind();
        UToolMenus::UnRegisterStartupCallback(this); UToolMenus::UnregisterOwner(this);
    }
    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        auto* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.File");
        Menu->AddSection("SCBridge", FText::FromString(TEXT("Star Citizen"))).AddMenuEntry("SCImportFolder",
            FText::FromString(TEXT("Import SC package folder…")), FText::FromString(TEXT("Import a decomposed export root into the current world")), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([]
            {
                FString Folder;
                if (auto* Desktop = FDesktopPlatformModule::Get())
                    if (Desktop->OpenDirectoryDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr), TEXT("Import SC package folder"), TEXT(""), Folder))
                        USCImportLibrary::ImportFolder(GEditor->GetEditorWorldContext().World(), Folder);
            })));
    }
};
IMPLEMENT_MODULE(FSCBridgeEditorModule, SCBridgeEditor);
