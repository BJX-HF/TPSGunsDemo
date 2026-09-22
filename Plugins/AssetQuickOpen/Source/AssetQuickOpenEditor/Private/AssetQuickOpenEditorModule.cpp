#include "Modules/ModuleManager.h"

#include "Framework/Commands/UIAction.h"
#include "Framework/Docking/TabManager.h"
#include "SAssetQuickOpenFavorites.h"
#include "Textures/SlateIcon.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "FAssetQuickOpenEditorModule"

namespace AssetQuickOpenEditor
{
	static const FName AssetQuickOpenTabName(TEXT("AssetQuickOpen.Favorites"));
}

class FAssetQuickOpenEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			AssetQuickOpenEditor::AssetQuickOpenTabName,
			FOnSpawnTab::CreateRaw(this, &FAssetQuickOpenEditorModule::SpawnAssetQuickOpenTab))
			.SetDisplayName(LOCTEXT("AssetQuickOpenTabTitle", "Asset Quick Open"))
			.SetTooltipText(LOCTEXT("AssetQuickOpenTabToolTip", "Open favorite assets and folders quickly."))
			.SetMenuType(ETabSpawnerMenuType::Hidden);

		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAssetQuickOpenEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);

		if (UToolMenus::IsToolMenuUIEnabled())
		{
			UToolMenus::UnregisterOwner(this);
		}

		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(AssetQuickOpenEditor::AssetQuickOpenTabName);
	}

private:
	TSharedRef<SDockTab> SpawnAssetQuickOpenTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SAssetQuickOpenFavorites)
			];
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
		Section.AddMenuEntry(
			"OpenAssetQuickOpenFavorites",
			LOCTEXT("OpenAssetQuickOpenFavorites", "Asset Quick Open"),
			LOCTEXT("OpenAssetQuickOpenFavoritesToolTip", "Open the favorite asset quick opener."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FAssetQuickOpenEditorModule::OpenAssetQuickOpenTab)));
	}

	void OpenAssetQuickOpenTab()
	{
		FGlobalTabmanager::Get()->TryInvokeTab(AssetQuickOpenEditor::AssetQuickOpenTabName);
	}
};

IMPLEMENT_MODULE(FAssetQuickOpenEditorModule, AssetQuickOpenEditor)

#undef LOCTEXT_NAMESPACE
