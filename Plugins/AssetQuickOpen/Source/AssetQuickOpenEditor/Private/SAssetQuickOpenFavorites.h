#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

struct FAssetData;

enum class EAssetQuickOpenFavoriteType : uint8
{
	Asset,
	Folder
};

struct FAssetQuickOpenFavoriteItem
{
	explicit FAssetQuickOpenFavoriteItem(const FSoftObjectPath& InObjectPath)
		: Type(EAssetQuickOpenFavoriteType::Asset)
		, ObjectPath(InObjectPath)
	{
	}

	explicit FAssetQuickOpenFavoriteItem(const FString& InFolderPath)
		: Type(EAssetQuickOpenFavoriteType::Folder)
		, FolderPath(InFolderPath)
	{
	}

	bool IsAsset() const
	{
		return Type == EAssetQuickOpenFavoriteType::Asset;
	}

	bool IsFolder() const
	{
		return Type == EAssetQuickOpenFavoriteType::Folder;
	}

	EAssetQuickOpenFavoriteType Type;
	FSoftObjectPath ObjectPath;
	FString FolderPath;
};

class SAssetQuickOpenFavorites : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SAssetQuickOpenFavorites) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	using FFavoriteItemPtr = TSharedPtr<FAssetQuickOpenFavoriteItem>;

	TSharedRef<ITableRow> MakeRow(FFavoriteItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable);
	void RefreshItems();
	void AddSelectedItems();
	void OpenItem(FFavoriteItemPtr Item) const;
	void SyncItemToContentBrowser(FFavoriteItemPtr Item) const;
	void RemoveItem(FFavoriteItemPtr Item);
	void ClearItems();
	void SaveFavorites(const TArray<FSoftObjectPath>& FavoriteAssets, const TArray<FString>& FavoriteFolders) const;
	bool MatchesSearch(const FAssetQuickOpenFavoriteItem& Item) const;
	FText GetEmptyText() const;
	static void LoadFavorites(TArray<FSoftObjectPath>& OutFavoriteAssets, TArray<FString>& OutFavoriteFolders);
	static FText GetActionText(const FAssetQuickOpenFavoriteItem& Item);
	static FString GetDisplayName(const FAssetQuickOpenFavoriteItem& Item);
	static FString GetDisplayPath(const FAssetQuickOpenFavoriteItem& Item);
	static FAssetData ResolveAssetData(const FSoftObjectPath& ObjectPath);
	static FString NormalizeFolderPath(const FString& FolderPath);

	TArray<FFavoriteItemPtr> Items;
	TSharedPtr<SListView<FFavoriteItemPtr>> ListView;
	FText SearchText;
};
