// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "AssetDefinitionDefault.h"
#include "AssetDefinition_LyraRecoilProfile.generated.h"

/** Bind the recoil GUI to the asset class through the native editor asset registry. */
UCLASS()
class UAssetDefinition_LyraRecoilProfile : public UAssetDefinitionDefault
{
    GENERATED_BODY()
public:
    virtual FText GetAssetDisplayName() const override;
    virtual TSoftClassPtr<UObject> GetAssetClass() const override;
    virtual FLinearColor GetAssetColor() const override;
    virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override;
    virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
};
