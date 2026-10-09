// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

class ULyraRecoilProfile;
class FLyraRecoilProfileEditor;
class FSimpleAssetEditor;
class IToolkitHost;

namespace LyraRecoilEditorRouting
{
    TSharedPtr<FLyraRecoilProfileEditor> OpenGUI(ULyraRecoilProfile* Profile, TSharedPtr<IToolkitHost> Host = nullptr);
    TSharedPtr<FSimpleAssetEditor> OpenRawDetails(ULyraRecoilProfile* Profile);
    void RegisterMenus();
}
