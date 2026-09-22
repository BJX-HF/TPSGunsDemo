#include "SAssetQuickOpenFavorites.h"

#include "AssetQuickOpenFavoritesSettings.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "IContentBrowserSingleton.h"
#include "IContentBrowserDataModule.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "Styling/SlateColor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "AssetQuickOpenFavorites"

namespace AssetQuickOpenFavorites
{
	static const TCHAR* ConfigSection = TEXT("AssetQuickOpen.Favorites");
	static const TCHAR* FavoriteAssetsKey = TEXT("FavoriteAssets");
	static const TCHAR* FavoriteFoldersKey = TEXT("FavoriteFolders");
}

void SAssetQuickOpenFavorites::Construct(const FArguments& InArgs)
{
	RefreshItems();

	ChildSlot
	[
		SNew(SBorder)
		.Padding(8.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SSearchBox)
					.HintText(LOCTEXT("SearchHint", "Search favorite assets and folders"))
					.OnTextChanged_Lambda([this](const FText& InText)
					{
						SearchText = InText;
						RefreshItems();
					})
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("AddSelected", "Add Selected"))
					.ToolTipText(LOCTEXT("AddSelectedToolTip", "Add the currently selected Content Browser assets and folders to this quick opener."))
					.OnClicked_Lambda([this]()
					{
						AddSelectedItems();
						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Clear", "Clear"))
					.ToolTipText(LOCTEXT("ClearToolTip", "Remove all favorite assets and folders from this panel."))
					.OnClicked_Lambda([this]()
					{
						ClearItems();
						return FReply::Handled();
					})
				]
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SAssignNew(ListView, SListView<FFavoriteItemPtr>)
				.ListItemsSource(&Items)
				.OnGenerateRow(this, &SAssetQuickOpenFavorites::MakeRow)
				.OnMouseButtonDoubleClick(this, &SAssetQuickOpenFavorites::OpenItem)
				.SelectionMode(ESelectionMode::Single)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SAssetQuickOpenFavorites::GetEmptyText)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]
	];
}

TSharedRef<ITableRow> SAssetQuickOpenFavorites::MakeRow(FFavoriteItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<FFavoriteItemPtr>, OwnerTable)
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(GetDisplayName(*Item)))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(GetDisplayPath(*Item)))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(6.0f, 2.0f)
		[
			SNew(SButton)
			.Text_Lambda([Item]()
			{
				return Item.IsValid() ? GetActionText(*Item) : LOCTEXT("Open", "Open");
			})
			.OnClicked_Lambda([this, Item]()
			{
				OpenItem(Item);
				return FReply::Handled();
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 2.0f, 6.0f, 2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Browse", "Browse"))
			.Visibility_Lambda([Item]()
			{
				return Item.IsValid() && Item->IsAsset() ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.OnClicked_Lambda([this, Item]()
			{
				SyncItemToContentBrowser(Item);
				return FReply::Handled();
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0.0f, 2.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Remove", "Remove"))
			.OnClicked_Lambda([this, Item]()
			{
				RemoveItem(Item);
				return FReply::Handled();
			})
		]
	];
}

void SAssetQuickOpenFavorites::RefreshItems()
{
	Items.Reset();

	TArray<FSoftObjectPath> FavoriteAssets;
	TArray<FString> FavoriteFolders;
	LoadFavorites(FavoriteAssets, FavoriteFolders);

	for (const FSoftObjectPath& ObjectPath : FavoriteAssets)
	{
		FAssetQuickOpenFavoriteItem Item(ObjectPath);
		if (!ObjectPath.IsNull() && MatchesSearch(Item))
		{
			Items.Add(MakeShared<FAssetQuickOpenFavoriteItem>(ObjectPath));
		}
	}

	for (const FString& FolderPath : FavoriteFolders)
	{
		const FString NormalizedFolderPath = NormalizeFolderPath(FolderPath);
		FAssetQuickOpenFavoriteItem Item(NormalizedFolderPath);
		if (!NormalizedFolderPath.IsEmpty() && MatchesSearch(Item))
		{
			Items.Add(MakeShared<FAssetQuickOpenFavoriteItem>(NormalizedFolderPath));
		}
	}

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}
}

void SAssetQuickOpenFavorites::AddSelectedItems()
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	TArray<FString> SelectedFolders;
	ContentBrowserModule.Get().GetSelectedFolders(SelectedFolders);

	TArray<FString> SelectedPathViewFolders;
	ContentBrowserModule.Get().GetSelectedPathViewFolders(SelectedPathViewFolders);
	SelectedFolders.Append(SelectedPathViewFolders);

	if (SelectedAssets.IsEmpty() && SelectedFolders.IsEmpty())
	{
		return;
	}

	TArray<FSoftObjectPath> FavoriteAssets;
	TArray<FString> FavoriteFolders;
	LoadFavorites(FavoriteAssets, FavoriteFolders);

	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (AssetData.IsValid())
		{
			FavoriteAssets.AddUnique(AssetData.GetSoftObjectPath());
		}
	}

	for (FString FolderPath : SelectedFolders)
	{
		const FString NormalizedFolderPath = NormalizeFolderPath(FolderPath);
		if (!NormalizedFolderPath.IsEmpty())
		{
			FavoriteFolders.AddUnique(NormalizedFolderPath);
		}
	}

	SaveFavorites(FavoriteAssets, FavoriteFolders);
	RefreshItems();
}

void SAssetQuickOpenFavorites::OpenItem(FFavoriteItemPtr Item) const
{
	if (!Item.IsValid())
	{
		return;
	}

	if (Item->IsFolder())
	{
		SyncItemToContentBrowser(Item);
		return;
	}

	UObject* Asset = Item->ObjectPath.TryLoad();
	if (Asset && GEditor)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
	}
}

void SAssetQuickOpenFavorites::SyncItemToContentBrowser(FFavoriteItemPtr Item) const
{
	if (!Item.IsValid())
	{
		return;
	}

	if (Item->IsFolder())
	{
		TArray<FString> FoldersToSync;
		FoldersToSync.Add(NormalizeFolderPath(Item->FolderPath));

		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
		ContentBrowserModule.Get().SyncBrowserToFolders(FoldersToSync, false, true, FName(), true);
		return;
	}

	FAssetData AssetData = ResolveAssetData(Item->ObjectPath);
	if (!AssetData.IsValid())
	{
		if (UObject* Asset = Item->ObjectPath.ResolveObject())
		{
			AssetData = FAssetData(Asset);
		}
	}

	if (AssetData.IsValid())
	{
		TArray<FAssetData> AssetsToSync;
		AssetsToSync.Add(AssetData);

		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
		ContentBrowserModule.Get().SyncBrowserToAssets(AssetsToSync);
	}
}

void SAssetQuickOpenFavorites::RemoveItem(FFavoriteItemPtr Item)
{
	if (!Item.IsValid())
	{
		return;
	}

	TArray<FSoftObjectPath> FavoriteAssets;
	TArray<FString> FavoriteFolders;
	LoadFavorites(FavoriteAssets, FavoriteFolders);

	if (Item->IsAsset())
	{
		FavoriteAssets.Remove(Item->ObjectPath);
	}
	else
	{
		FavoriteFolders.Remove(Item->FolderPath);
	}

	SaveFavorites(FavoriteAssets, FavoriteFolders);
	RefreshItems();
}

void SAssetQuickOpenFavorites::ClearItems()
{
	SaveFavorites(TArray<FSoftObjectPath>(), TArray<FString>());
	RefreshItems();
}

void SAssetQuickOpenFavorites::SaveFavorites(const TArray<FSoftObjectPath>& FavoriteAssets, const TArray<FString>& FavoriteFolders) const
{
	TArray<FString> FavoriteAssetStrings;
	FavoriteAssetStrings.Reserve(FavoriteAssets.Num());
	for (const FSoftObjectPath& FavoriteAsset : FavoriteAssets)
	{
		if (!FavoriteAsset.IsNull())
		{
			FavoriteAssetStrings.Add(FavoriteAsset.ToString());
		}
	}

	TArray<FString> NormalizedFavoriteFolders;
	NormalizedFavoriteFolders.Reserve(FavoriteFolders.Num());
	for (const FString& FavoriteFolder : FavoriteFolders)
	{
		const FString NormalizedFolder = NormalizeFolderPath(FavoriteFolder);
		if (!NormalizedFolder.IsEmpty())
		{
			NormalizedFavoriteFolders.AddUnique(NormalizedFolder);
		}
	}

	GConfig->SetArray(
		AssetQuickOpenFavorites::ConfigSection,
		AssetQuickOpenFavorites::FavoriteAssetsKey,
		FavoriteAssetStrings,
		GEditorPerProjectIni);

	GConfig->SetArray(
		AssetQuickOpenFavorites::ConfigSection,
		AssetQuickOpenFavorites::FavoriteFoldersKey,
		NormalizedFavoriteFolders,
		GEditorPerProjectIni);

	GConfig->Flush(false, GEditorPerProjectIni);
}

bool SAssetQuickOpenFavorites::MatchesSearch(const FAssetQuickOpenFavoriteItem& Item) const
{
	if (SearchText.IsEmpty())
	{
		return true;
	}

	const FString Needle = SearchText.ToString();
	return GetDisplayName(Item).Contains(Needle, ESearchCase::IgnoreCase)
		|| GetDisplayPath(Item).Contains(Needle, ESearchCase::IgnoreCase);
}

FText SAssetQuickOpenFavorites::GetEmptyText() const
{
	TArray<FSoftObjectPath> FavoriteAssets;
	TArray<FString> FavoriteFolders;
	LoadFavorites(FavoriteAssets, FavoriteFolders);

	if (FavoriteAssets.IsEmpty() && FavoriteFolders.IsEmpty())
	{
		return LOCTEXT("EmptyFavorites", "Select assets or folders in the Content Browser, then click Add Selected.");
	}

	if (Items.IsEmpty())
	{
		return LOCTEXT("NoSearchResults", "No favorites match the current search.");
	}

	return FText::GetEmpty();
}

void SAssetQuickOpenFavorites::LoadFavorites(TArray<FSoftObjectPath>& OutFavoriteAssets, TArray<FString>& OutFavoriteFolders)
{
	OutFavoriteAssets.Reset();
	OutFavoriteFolders.Reset();

	TArray<FString> FavoriteAssetStrings;
	GConfig->GetArray(
		AssetQuickOpenFavorites::ConfigSection,
		AssetQuickOpenFavorites::FavoriteAssetsKey,
		FavoriteAssetStrings,
		GEditorPerProjectIni);

	for (const FString& FavoriteAssetString : FavoriteAssetStrings)
	{
		FSoftObjectPath FavoriteAsset(FavoriteAssetString);
		if (!FavoriteAsset.IsNull())
		{
			OutFavoriteAssets.AddUnique(FavoriteAsset);
		}
	}

	GConfig->GetArray(
		AssetQuickOpenFavorites::ConfigSection,
		AssetQuickOpenFavorites::FavoriteFoldersKey,
		OutFavoriteFolders,
		GEditorPerProjectIni);

	for (FString& FavoriteFolder : OutFavoriteFolders)
	{
		FavoriteFolder = NormalizeFolderPath(FavoriteFolder);
	}

	OutFavoriteFolders.RemoveAll([](const FString& FavoriteFolder)
	{
		return FavoriteFolder.IsEmpty();
	});

	if (OutFavoriteAssets.IsEmpty() && OutFavoriteFolders.IsEmpty())
	{
		const UAssetQuickOpenFavoritesSettings* LegacySettings = GetDefault<UAssetQuickOpenFavoritesSettings>();
		OutFavoriteAssets = LegacySettings->FavoriteAssets;
		OutFavoriteFolders = LegacySettings->FavoriteFolders;
	}
}

FText SAssetQuickOpenFavorites::GetActionText(const FAssetQuickOpenFavoriteItem& Item)
{
	return Item.IsFolder() ? LOCTEXT("OpenFolder", "Browse") : LOCTEXT("OpenAsset", "Open");
}

FString SAssetQuickOpenFavorites::GetDisplayName(const FAssetQuickOpenFavoriteItem& Item)
{
	if (Item.IsAsset())
	{
		return Item.ObjectPath.GetAssetName();
	}

	FString FolderName = FPaths::GetCleanFilename(Item.FolderPath);
	return FolderName.IsEmpty() ? Item.FolderPath : FolderName;
}

FString SAssetQuickOpenFavorites::GetDisplayPath(const FAssetQuickOpenFavoriteItem& Item)
{
	return Item.IsAsset() ? Item.ObjectPath.GetAssetPathString() : Item.FolderPath;
}

FAssetData SAssetQuickOpenFavorites::ResolveAssetData(const FSoftObjectPath& ObjectPath)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	return AssetRegistryModule.Get().GetAssetByObjectPath(ObjectPath);
}

FString SAssetQuickOpenFavorites::NormalizeFolderPath(const FString& FolderPath)
{
	FString NormalizedPath = FolderPath;
	NormalizedPath.TrimStartAndEndInline();

	if (NormalizedPath.IsEmpty())
	{
		return NormalizedPath;
	}

	FPaths::NormalizeDirectoryName(NormalizedPath);

	if (UContentBrowserDataSubsystem* ContentBrowserData = IContentBrowserDataModule::Get().GetSubsystem())
	{
		FString InternalPath;
		if (ContentBrowserData->TryConvertVirtualPath(NormalizedPath, InternalPath) == EContentBrowserPathType::Internal)
		{
			return InternalPath;
		}
	}

	if (NormalizedPath.StartsWith(TEXT("/All/")))
	{
		NormalizedPath.RightChopInline(4);
	}

	return NormalizedPath;
}

#undef LOCTEXT_NAMESPACE
