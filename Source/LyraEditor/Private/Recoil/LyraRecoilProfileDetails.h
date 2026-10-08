// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once
#include "IDetailCustomization.h"
class FLyraRecoilProfileEditor;

/** Local to the recoil toolkit; ordinary Profile editors retain their default layout. */
class FLyraRecoilProfileDetails : public IDetailCustomization
{
public:
 explicit FLyraRecoilProfileDetails(TSharedPtr<FLyraRecoilProfileEditor> InEditor) : Editor(InEditor) {}
 static TSharedRef<IDetailCustomization> MakeInstance(TSharedPtr<FLyraRecoilProfileEditor> InEditor) { return MakeShared<FLyraRecoilProfileDetails>(InEditor); }
 virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override;
private:
 TWeakPtr<FLyraRecoilProfileEditor> Editor;
};
