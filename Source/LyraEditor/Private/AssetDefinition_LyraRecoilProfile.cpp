// Copyright Epic Games, Inc. All Rights Reserved.
#include "AssetDefinition_LyraRecoilProfile.h"
#include "Recoil/LyraRecoilEditorRouting.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"

FText UAssetDefinition_LyraRecoilProfile::GetAssetDisplayName() const
{
    return NSLOCTEXT("LyraRecoilEditor", "AssetTypeName", "Lyra Recoil Profile");
}
TSoftClassPtr<UObject> UAssetDefinition_LyraRecoilProfile::GetAssetClass() const
{
    return ULyraRecoilProfile::StaticClass();
}
FLinearColor UAssetDefinition_LyraRecoilProfile::GetAssetColor() const
{
    return FLinearColor(FColor(45, 175, 153));
}
TConstArrayView<FAssetCategoryPath> UAssetDefinition_LyraRecoilProfile::GetAssetCategories() const
{
    static const FAssetCategoryPath Categories[] = { EAssetCategoryPaths::Misc };
    return Categories;
}
EAssetCommandResult UAssetDefinition_LyraRecoilProfile::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
    // Preserve the engine's read-only viewer path when editing is not requested.
    if (OpenArgs.OpenMethod != EAssetOpenMethod::Edit) return Super::OpenAssets(OpenArgs);
    for (ULyraRecoilProfile* Profile : OpenArgs.LoadObjects<ULyraRecoilProfile>())
        LyraRecoilEditorRouting::OpenGUI(Profile, OpenArgs.ToolkitHost);
    return EAssetCommandResult::Handled;
}
