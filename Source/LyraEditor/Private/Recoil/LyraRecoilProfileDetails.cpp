// Copyright Epic Games, Inc. All Rights Reserved.
#include "Recoil/LyraRecoilProfileDetails.h"
#include "Recoil/LyraRecoilProfileEditor.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyHandle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void FLyraRecoilProfileDetails::CustomizeDetails(IDetailLayoutBuilder& Builder)
{
 const TWeakPtr<FLyraRecoilProfileEditor> Weak = Editor;
 Builder.HideProperty(Builder.GetProperty(GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile, PatternPoints)));
 Builder.HideProperty(Builder.GetProperty(GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile, PatternLength)));
 IDetailCategoryBuilder& Pattern = Builder.EditCategory("Recoil|Pattern");
 Pattern.AddCustomRow(FText::FromString(TEXT("固定区间 PatternLength")))
 .NameContent()[SNew(STextBlock).Text(FText::FromString(TEXT("固定区间长度")))]
 .ValueContent().MinDesiredWidth(160)
 [SNew(SNumericEntryBox<int32>).MinValue(0).AllowSpin(false)
  .Value_Lambda([Weak]() -> TOptional<int32> { auto E=Weak.Pin(); return E && E->GetProfile() ? TOptional<int32>(E->GetProfile()->PatternLength) : TOptional<int32>(); })
  .OnValueCommitted_Lambda([Weak](int32 Value,ETextCommit::Type) { if(auto E=Weak.Pin()) E->SetPatternLengthValue(Value); })];
 Pattern.AddCustomRow(FText::FromString(TEXT("逐发编辑"))).WholeRowContent()
 [SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("逐发原值、增删与重排请使用左侧表格。序号从1开始；曲线采样从0开始。随机尾段的X当前不生效，原值仍保留。")))];

 IDetailCategoryBuilder& Curves=Builder.EditCategory("CurveOwnership",FText::FromString(TEXT("曲线所有权")),ECategoryPriority::Important);
 for(FName Name: FLyraRecoilEditOperations::GetCurvePropertyNames())
 {
  auto E=Weak.Pin();
  const bool bShared=E && E->IsCurveShared(Name);
  const TSharedRef<IPropertyHandle> Property=Builder.GetProperty(Name);
  // Shared curve content is not embedded in the editable rich-curve control.
  if(bShared)
  {
   Builder.HideProperty(Property);
   TSharedPtr<IPropertyHandle> Reference=Property->GetChildHandle(TEXT("ExternalCurve"));
   if(Reference.IsValid()) Curves.AddProperty(Reference.ToSharedRef());
  }
  Curves.AddCustomRow(FText::FromName(Name)).WholeRowContent()
  [SNew(SVerticalBox)
   +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true)
    .Text_Lambda([Weak,Name] { auto Toolkit=Weak.Pin(); return FText::FromString(Name.ToString()+TEXT("：")+(Toolkit?Toolkit->GetCurveOwnershipText(Name):TEXT("已关闭"))); })]
   +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
    +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("复制为内联"))).IsEnabled(bShared)
     .OnClicked_Lambda([Weak,Name] { if(auto Toolkit=Weak.Pin()) Toolkit->CopyCurveToInline(Name); return FReply::Handled(); })]
    +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("复制新曲线资产")))
     .OnClicked_Lambda([Weak,Name] { if(auto Toolkit=Weak.Pin()) Toolkit->DuplicateCurveAsset(Name); return FReply::Handled(); })]
    +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("打开共享曲线"))).IsEnabled(bShared)
     .ToolTipText(FText::FromString(TEXT("修改原共享曲线会影响所有引用者；它有独立的保存流程。")))
     .OnClicked_Lambda([Weak,Name] { if(auto Toolkit=Weak.Pin()) Toolkit->OpenSharedCurveAsset(Name); return FReply::Handled(); })]]];
 }
}
