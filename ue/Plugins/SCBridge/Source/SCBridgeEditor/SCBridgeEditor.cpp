#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Editor.h"
#include "SCImporter.h"
#include "SCViewerSubsystem.h"
#include "Private/SCKeepAssets.h"
#include "Framework/Application/SlateApplication.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Cargo/SCCargoDroneArt.h"
#include "GameDelegates.h"
#include "Misc/PackageName.h"
#include "SCFxNames.h"
namespace
{
// Assets the runtime loads by a path from data (no cooked map references them): the drone art Config/Cargo/drone_art.json
// names (licensed, installed per machine) and the game's effects SCFxBuild built (/Game/SC/FX, game-derived). A
// machine without them cooks without them (the runtime then uses the stand-in drone and the beam mesh).
void AddDataLoadedAssetsToCook(TConstArrayView<const ITargetPlatform*>, TArray<FName>& ToCook, TArray<FName>&)
{
    int32 Art = 0, Fx = 0;
    for (const FString& Path : FSCCargoDroneArt::Get().AssetPaths())
    {
        const FString Package = FPackageName::ObjectPathToPackageName(Path);
        if (!FPackageName::DoesPackageExist(Package)) continue;
        ToCook.AddUnique(FName(*Package));
        ++Art;
    }
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    TArray<FAssetData> Assets;
    Registry.GetAssetsByPath(FName(SCFxNames::Root()), Assets, /*bRecursive=*/true);
    for (const FAssetData& A : Assets)
    {
        ToCook.AddUnique(A.PackageName);
        ++Fx;
    }
    UE_LOG(LogTemp, Display, TEXT("SC_COOK drone art packages=%d (%s), effect packages=%d (%s)"), Art, *FSCCargoDroneArt::Get().Source, Fx, SCFxNames::Root());
}
} // namespace
class FSCBridgeEditorModule : public IModuleInterface
{
public:
    void StartupModule() override
    {
        // Editor host extra: the Runtime UI shows "Keep as assets" while this handler is bound.
        USCViewerSubsystem::KeepHandler().BindStatic(&SCKeepAssets::Keep);
        CookHandle = FGameDelegates::Get().GetModifyCookDelegate().AddStatic(&AddDataLoadedAssetsToCook);
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FSCBridgeEditorModule::RegisterMenus));
    }
    void ShutdownModule() override
    {
        USCViewerSubsystem::KeepHandler().Unbind();
        FGameDelegates::Get().GetModifyCookDelegate().Remove(CookHandle);
        UToolMenus::UnRegisterStartupCallback(this); UToolMenus::UnregisterOwner(this);
    }
    FDelegateHandle CookHandle;
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
