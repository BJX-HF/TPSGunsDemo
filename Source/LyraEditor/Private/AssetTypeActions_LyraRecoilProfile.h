// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once
#include "AssetTypeActions_Base.h"

class FAssetTypeActions_LyraRecoilProfile : public FAssetTypeActions_Base
{
public:
 virtual FText GetName() const override { return NSLOCTEXT("LyraRecoilEditor","AssetTypeName","Lyra Recoil Profile"); }
 virtual FColor GetTypeColor() const override { return FColor(45,175,153); }
 virtual UClass* GetSupportedClass() const override;
 virtual uint32 GetCategories() override { return EAssetTypeCategories::Misc; }
 virtual void OpenAssetEditor(const TArray<UObject*>& InObjects,TSharedPtr<IToolkitHost> Host) override;
};
