// Copyright Epic Games, Inc. All Rights Reserved.
#include "Recoil/LyraRecoilEditorRouting.h"
#include "Recoil/LyraRecoilProfileEditor.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "ContentBrowserMenuContexts.h"
#include "ToolMenus.h"
#include "Toolkits/SimpleAssetEditor.h"
#include "Widgets/Input/SButton.h"

#define LOCTEXT_NAMESPACE "LyraRecoilEditorRouting"
namespace LyraRecoilEditorRouting
{
class FRecoilRawDetailsEditor : public FSimpleAssetEditor
{
public:
    TWeakObjectPtr<ULyraRecoilProfile> Profile;

    virtual void PostRegenerateMenusAndToolbars() override
    {
        FSimpleAssetEditor::PostRegenerateMenusAndToolbars();
        const auto WeakProfile = Profile;
        SetMenuOverlay(SNew(SButton)
            .Text(LOCTEXT("BackToGUI", "后坐力 GUI"))
            .ToolTipText(LOCTEXT("BackToGUITip", "打开同一资产的逐发表格、配置画布与时间预览。"))
            .OnClicked_Lambda([WeakProfile] { OpenGUI(WeakProfile.Get()); return FReply::Handled(); }));
    }
};

static TMap<TWeakObjectPtr<ULyraRecoilProfile>, TWeakPtr<FSimpleAssetEditor>> RawEditors;

TSharedPtr<FLyraRecoilProfileEditor> OpenGUI(ULyraRecoilProfile* Profile, TSharedPtr<IToolkitHost> Host)
{
    if (!Profile) return nullptr;
    if (auto Existing = FLyraRecoilProfileEditor::FindEditorForProfile(Profile))
    {
        Existing->FocusWindow(Profile);
        return Existing;
    }
    auto Editor = MakeShared<FLyraRecoilProfileEditor>();
    Editor->InitEditor(Host, Profile);
    return Editor;
}

TSharedPtr<FSimpleAssetEditor> OpenRawDetails(ULyraRecoilProfile* Profile)
{
    if (!Profile) return nullptr;
    for (auto It = RawEditors.CreateIterator(); It; ++It)
        if (!It.Key().IsValid() || !It.Value().IsValid()) It.RemoveCurrent();
    if (auto Existing = RawEditors.FindRef(Profile).Pin())
    {
        Existing->FocusWindow(Profile);
        return Existing;
    }
    auto Editor = MakeShared<FRecoilRawDetailsEditor>();
    Editor->Profile = Profile;
    Editor->InitEditor(EToolkitMode::Standalone, nullptr, { Profile }, FSimpleAssetEditor::FGetDetailsViewObjects());
    RawEditors.Add(Profile, Editor);
    Editor->RegenerateMenusAndToolbars();
    return Editor;
}

void RegisterMenus()
{
    FToolMenuOwnerScoped Owner(TEXT("LyraRecoilEditor"));
    UToolMenu* Menu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(ULyraRecoilProfile::StaticClass());
    auto& Section = Menu->FindOrAddSection(TEXT("GetAssetActions"));
    FToolUIAction OpenGUIAction;
    OpenGUIAction.CanExecuteAction = FToolMenuCanExecuteAction::CreateLambda([](const FToolMenuContext& Context)
    {
        const auto* Assets = Context.FindContext<UContentBrowserAssetContextMenuContext>();
        return Assets && Assets->bCanBeModified;
    });
    OpenGUIAction.ExecuteAction = FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext& Context)
    {
        if (const auto* Assets = Context.FindContext<UContentBrowserAssetContextMenuContext>())
            for (auto* Profile : Assets->LoadSelectedObjects<ULyraRecoilProfile>()) OpenGUI(Profile);
    });
    Section.AddMenuEntry(TEXT("LyraRecoilOpenGUI"), LOCTEXT("OpenGUI", "后坐力 GUI 编辑器"),
        LOCTEXT("OpenGUITip", "打开逐发表格、配置画布、参数详情与时间预览。"), FSlateIcon(), OpenGUIAction);
    FToolUIAction OpenRawAction;
    OpenRawAction.CanExecuteAction = OpenGUIAction.CanExecuteAction;
    OpenRawAction.ExecuteAction = FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext& Context)
    {
        if (const auto* Assets = Context.FindContext<UContentBrowserAssetContextMenuContext>())
            for (auto* Profile : Assets->LoadSelectedObjects<ULyraRecoilProfile>()) OpenRawDetails(Profile);
    });
    Section.AddMenuEntry(TEXT("LyraRecoilOpenRaw"), LOCTEXT("OpenRaw", "原始参数（Details）"),
        LOCTEXT("OpenRawTip", "打开引擎通用 Details；顶部按钮可返回后坐力 GUI。"), FSlateIcon(), OpenRawAction);
}
}
#undef LOCTEXT_NAMESPACE
