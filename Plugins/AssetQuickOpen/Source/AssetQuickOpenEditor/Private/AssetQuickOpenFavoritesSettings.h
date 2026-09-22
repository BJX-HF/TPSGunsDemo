#pragma once

#include "CoreMinimal.h"
#include "AssetQuickOpenFavoritesSettings.generated.h"

UCLASS(config=EditorPerProjectUserSettings)
class UAssetQuickOpenFavoritesSettings : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(config)
	TArray<FSoftObjectPath> FavoriteAssets;

	UPROPERTY(config)
	TArray<FString> FavoriteFolders;
};
