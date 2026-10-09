// Copyright Epic Games, Inc. All Rights Reserved.
#include "AssetTypeActions_LyraRecoilProfile.h"
#include "Recoil/LyraRecoilEditorRouting.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"

UClass* FAssetTypeActions_LyraRecoilProfile::GetSupportedClass() const { return ULyraRecoilProfile::StaticClass(); }
void FAssetTypeActions_LyraRecoilProfile::OpenAssetEditor(const TArray<UObject*>& InObjects,TSharedPtr<IToolkitHost> Host)
{
 for(UObject* Object:InObjects)
  if(ULyraRecoilProfile* Profile=Cast<ULyraRecoilProfile>(Object))
  {
   LyraRecoilEditorRouting::OpenGUI(Profile,Host);
  }
}
