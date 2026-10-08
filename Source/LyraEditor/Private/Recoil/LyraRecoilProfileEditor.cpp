// Copyright Epic Games, Inc. All Rights Reserved.

#include "Recoil/LyraRecoilProfileEditor.h"

#include "Recoil/LyraRecoilEditorCommands.h"
#include "Recoil/LyraRecoilEditorSession.h"
#include "Recoil/LyraRecoilPatternAdapter.h"
#include "Recoil/LyraRecoilProfileDetails.h"
#include "Recoil/SLyraRecoilShotTable.h"
#include "Recoil/SLyraRecoilPatternGraph.h"
#include "Recoil/SLyraRecoilPreview.h"
#include "Framework/Commands/GenericCommands.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"

#include "Curves/CurveFloat.h"
#include "DetailLayoutBuilder.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IDetailsView.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/MessageDialog.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Toolkits/SimpleAssetEditor.h"

#define LOCTEXT_NAMESPACE "LyraRecoilProfileEditor"

namespace LyraRecoilProfileEditorTabs
{
	static const FName TableTabId("LyraRecoilProfileEditor_ShotTable");
	static const FName GraphTabId("LyraRecoilProfileEditor_PatternGraph");
	static const FName DetailsTabId("LyraRecoilProfileEditor_Details");
	static const FName PreviewTabId("LyraRecoilProfileEditor_Preview");
}

TMap<TWeakObjectPtr<ULyraRecoilProfile>, TWeakPtr<FLyraRecoilProfileEditor>> FLyraRecoilProfileEditor::EditorInstances;

void FLyraRecoilProfileEditor::InitEditor(const TSharedPtr<IToolkitHost>& InitToolkitHost, ULyraRecoilProfile* InProfile)
{
	Profile = InProfile;
	check(Profile);

	// 命令必须先注册，MapToolkitCommands 在 InitAssetEditor 内部被调用。
	FLyraRecoilEditorCommands::Register();

	// 会话：Transient + RF_Transactional，绝不序列化进运行时包。
	Session = NewObject<ULyraRecoilEditorSession>(GetTransientPackage(), NAME_None, RF_Transactional);
	Session->Initialize(Profile);

	Operations.Initialize(Profile, Session);

	// 局部 Details 定制：只作用于本工具包的 DetailsView，不改其他编辑器行为。
	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = true;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.bShowOptions = true;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;

	DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	DetailsView->RegisterInstancedCustomPropertyLayout(
		ULyraRecoilProfile::StaticClass(),
		FOnGetDetailCustomizationInstance::CreateSP(this, &FLyraRecoilProfileEditor::MakeDetailsCustomization));
	DetailsView->SetObject(Profile);
	DetailsView->OnFinishedChangingProperties().AddRaw(this, &FLyraRecoilProfileEditor::OnDetailsFinishedChangingProperties);

	// 上下左右布局：左逐发表格 / 中上画布 + 中下预览 / 右参数详情。
	const TSharedRef<FTabManager::FLayout> Layout=FTabManager::NewLayout("LyraRecoilProfileEditor_Layout_v2")
        ->AddArea(FTabManager::NewPrimaryArea()->SetOrientation(Orient_Vertical)
        ->Split(FTabManager::NewSplitter()->SetOrientation(Orient_Horizontal)->SetSizeCoefficient(.6f)
            ->Split(FTabManager::NewStack()->SetSizeCoefficient(.3f)->AddTab(LyraRecoilProfileEditorTabs::TableTabId,ETabState::OpenedTab))
            ->Split(FTabManager::NewStack()->SetSizeCoefficient(.4f)->AddTab(LyraRecoilProfileEditorTabs::GraphTabId,ETabState::OpenedTab))
            ->Split(FTabManager::NewStack()->SetSizeCoefficient(.3f)->AddTab(LyraRecoilProfileEditorTabs::DetailsTabId,ETabState::OpenedTab)))
        ->Split(FTabManager::NewStack()->SetSizeCoefficient(.4f)->AddTab(LyraRecoilProfileEditorTabs::PreviewTabId,ETabState::OpenedTab)));

	FAssetEditorToolkit::InitAssetEditor(
		EToolkitMode::Standalone,
		InitToolkitHost,
		TEXT("LyraRecoilProfileEditor"),
		Layout,
		/*bCreateDefaultStandaloneMenu=*/true,
		/*bCreateDefaultToolbar=*/true,
		Profile);

	RegisterEditorInstance();
	BindExternalListeners();
	if (GEditor) GEditor->RegisterForUndo(this);
	RefreshFromAsset();
}

// ---------------------------------------------------------------------------
// 标签页
// ---------------------------------------------------------------------------

void FLyraRecoilProfileEditor::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	// 本工具包自己的 Workspace 菜单分类，避免依赖基类的分类是否已创建。
	const TSharedRef<FWorkspaceItem> MenuCategory =
		InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu_LyraRecoilProfileEditor", "Lyra Recoil Profile Editor"));

	InTabManager->RegisterTabSpawner(LyraRecoilProfileEditorTabs::TableTabId,
		FOnSpawnTab::CreateSP(this, &FLyraRecoilProfileEditor::SpawnTableTab))
		.SetDisplayName(LOCTEXT("TableTabName", "逐发表格"))
		.SetGroup(MenuCategory)
		.SetIcon(FSlateIcon());

	InTabManager->RegisterTabSpawner(LyraRecoilProfileEditorTabs::GraphTabId,
		FOnSpawnTab::CreateSP(this, &FLyraRecoilProfileEditor::SpawnGraphTab))
		.SetDisplayName(LOCTEXT("GraphTabName", "配置画布"))
		.SetGroup(MenuCategory)
		.SetIcon(FSlateIcon());

	InTabManager->RegisterTabSpawner(LyraRecoilProfileEditorTabs::DetailsTabId,
		FOnSpawnTab::CreateSP(this, &FLyraRecoilProfileEditor::SpawnDetailsTab))
		.SetDisplayName(LOCTEXT("DetailsTabName", "参数详情"))
		.SetGroup(MenuCategory)
		.SetIcon(FSlateIcon());

	InTabManager->RegisterTabSpawner(LyraRecoilProfileEditorTabs::PreviewTabId,
		FOnSpawnTab::CreateSP(this, &FLyraRecoilProfileEditor::SpawnPreviewTab))
		.SetDisplayName(LOCTEXT("PreviewTabName", "时间预览与诊断"))
		.SetGroup(MenuCategory)
		.SetIcon(FSlateIcon());
}

void FLyraRecoilProfileEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(LyraRecoilProfileEditorTabs::TableTabId);
	InTabManager->UnregisterTabSpawner(LyraRecoilProfileEditorTabs::GraphTabId);
	InTabManager->UnregisterTabSpawner(LyraRecoilProfileEditorTabs::DetailsTabId);
	InTabManager->UnregisterTabSpawner(LyraRecoilProfileEditorTabs::PreviewTabId);
}

TSharedRef<SDockTab> FLyraRecoilProfileEditor::SpawnTableTab(const FSpawnTabArgs&)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.Label(LOCTEXT("TableTabLabel", "逐发表格"))
		[
			SAssignNew(ShotTable, SLyraRecoilShotTable)
			.Editor(SharedThis(this))
		];
}

TSharedRef<SDockTab> FLyraRecoilProfileEditor::SpawnGraphTab(const FSpawnTabArgs&)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.Label(LOCTEXT("GraphTabLabel", "配置画布"))
		[
			CreatePatternGraph()
		];
}

TSharedRef<SDockTab> FLyraRecoilProfileEditor::SpawnDetailsTab(const FSpawnTabArgs&)
{
	TSharedRef<SWidget> DetailsWidget = DetailsView.IsValid()
		? StaticCastSharedRef<SWidget>(DetailsView.ToSharedRef())
		: StaticCastSharedRef<SWidget>(SNew(STextBlock).Text(LOCTEXT("DetailsUnavailable", "Details 视图不可用")));

	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.Label(LOCTEXT("DetailsTabLabel", "参数详情"))
		[
			SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [SNew(SButton).Text(LOCTEXT("RawDetails","原始 Details（高级）"))
                .ToolTipText(LOCTEXT("RawDetailsTip","打开引擎通用 Details。外部数组结构修改会重建节点身份并取消草稿。"))
                .OnClicked_Lambda([this] { FSimpleAssetEditor::CreateEditor(EToolkitMode::Standalone,nullptr,Profile); return FReply::Handled(); })]
            + SVerticalBox::Slot().FillHeight(1)[DetailsWidget]
		];
}

TSharedRef<SDockTab> FLyraRecoilProfileEditor::SpawnPreviewTab(const FSpawnTabArgs&)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.Label(LOCTEXT("PreviewTabLabel", "时间预览与诊断"))
		[
			CreatePreview()
		];
}

// ---------------------------------------------------------------------------
// 命令与标识
// ---------------------------------------------------------------------------

void FLyraRecoilProfileEditor::MapToolkitCommands()
{
    GetToolkitCommands()->MapAction(FGenericCommands::Get().Undo,
        FExecuteAction::CreateLambda([] { if (GEditor) GEditor->UndoTransaction(); }));
    GetToolkitCommands()->MapAction(FGenericCommands::Get().Redo,
        FExecuteAction::CreateLambda([] { if (GEditor) GEditor->RedoTransaction(); }));

	// 先映射引擎标准命令（工具栏/菜单的保存按钮依赖它们）。
	FAssetEditorToolkit::MapToolkitCommands();

	const FLyraRecoilEditorCommands& Commands = FLyraRecoilEditorCommands::Get();
	const TSharedRef<FUICommandList> CommandList = GetToolkitCommands();

	CommandList->MapAction(Commands.SaveProfile,
		FExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::SaveAsset_Execute),
		FCanExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::CanSaveAsset));

	CommandList->MapAction(Commands.SelectAll,
		FExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::OnSelectAllCommand));

	CommandList->MapAction(Commands.CopySelection,
		FExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::OnCopyCommand),
		FCanExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::CanCopySelection));

	CommandList->MapAction(Commands.PasteNormalized,
		FExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::OnPasteCommand));

	CommandList->MapAction(Commands.PasteByAngle,
		FExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::OnPasteByAngleCommand));

	CommandList->MapAction(Commands.InsertPoint,
		FExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::OnInsertPointCommand));

	CommandList->MapAction(Commands.DeleteSelected,
		FExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::OnDeleteSelectedCommand),
		FCanExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::CanDeleteSelection));

	CommandList->MapAction(Commands.MoveRowUp,
		FExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::OnMoveRowUpCommand),
		FCanExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::CanReorderSelection));

	CommandList->MapAction(Commands.MoveRowDown,
		FExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::OnMoveRowDownCommand),
		FCanExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::CanReorderSelection));

	CommandList->MapAction(Commands.SortByCumulativePitch,
		FExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::OnSortByPitchCommand),
		FCanExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::CanReorderSelection));

	CommandList->MapAction(Commands.ValidateProfile,
		FExecuteAction::CreateSP(this, &FLyraRecoilProfileEditor::OnValidateCommand));
}

FName FLyraRecoilProfileEditor::GetToolkitFName() const
{
	return FName("LyraRecoilProfileEditor");
}

FText FLyraRecoilProfileEditor::GetBaseToolkitName() const
{
	return LOCTEXT("ToolkitName", "Lyra Recoil Profile Editor");
}

FString FLyraRecoilProfileEditor::GetWorldCentricTabPrefix() const
{
	return TEXT("LyraRecoilProfile ");
}

FLinearColor FLyraRecoilProfileEditor::GetWorldCentricTabColorScale() const
{
	return FLinearColor(0.25f, 0.35f, 0.75f, 0.5f);
}

void FLyraRecoilProfileEditor::OnClose()
{
	CancelActiveGesture();
	UnbindExternalListeners();
	UnregisterEditorInstance();

	ShotTable.Reset();
	PatternGraph.Reset();
	if (PreviewWidget) PreviewWidget->Cleanup();
	PreviewWidget.Reset();
	DetailsView.Reset();
	if (GEditor) GEditor->UnregisterForUndo(this);

	FAssetEditorToolkit::OnClose();
}

// ---------------------------------------------------------------------------
// GC
// ---------------------------------------------------------------------------

void FLyraRecoilProfileEditor::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Profile);
	Collector.AddReferencedObject(Session);
}

FString FLyraRecoilProfileEditor::GetReferencerName() const
{
	return TEXT("FLyraRecoilProfileEditor");
}

// ---------------------------------------------------------------------------
// 公开接口（画布 / 表格 / 预览共用）
// ---------------------------------------------------------------------------

const TArray<int32>& FLyraRecoilProfileEditor::GetSelectedIndices() const
{
	static const TArray<int32> EmptySelection;
	return Session ? Session->GetSelectedIndices() : EmptySelection;
}

void FLyraRecoilProfileEditor::SelectIndices(const TArray<int32>& Indices, bool bAppend)
{
	if (!Session)
	{
		return;
	}

	Session->SelectIndices(Indices, bAppend);
	RefreshShotTable();
}

void FLyraRecoilProfileEditor::ClearSelection()
{
	if (Session)
	{
		Session->ClearSelection();
		RefreshShotTable();
	}
}

bool FLyraRecoilProfileEditor::CommitPattern(const FLyraRecoilPatternData& Data, const FText& Description)
{
	TArray<FString> Errors;
	const bool bCommitted = Operations.CommitPattern(Data, Description, Errors);
	SetDiagnostics(Errors);

	if (bCommitted)
	{
		RefreshFromAsset();
	}
	else
	{
		ShowNotification(TEXT("提交被拒绝：候选数据不合法或资产已在其他入口更新（详见诊断面板）。"), /*bIsError=*/true);
	}

	return bCommitted;
}

bool FLyraRecoilProfileEditor::CommitCumulative(const TArray<FVector2D>& Targets)
{
	TArray<FString> Errors;
	const bool bCommitted = Operations.CommitCumulative(Targets, LOCTEXT("MoveCumulative", "移动累计点"), Errors);
	SetDiagnostics(Errors);

	if (bCommitted)
	{
		RefreshFromAsset();
	}
	else
	{
		ShowNotification(TEXT("累计点提交被拒绝（详见诊断面板）。"), /*bIsError=*/true);
	}

	return bCommitted;
}

void FLyraRecoilProfileEditor::RefreshFromAsset()
{
	if (!Profile)
	{
		return;
	}

	TArray<FString> Errors;
	CumulativePoints.Reset();
	if (!FLyraRecoilPatternAdapter::BuildCumulative(*Profile, CumulativePoints, Errors))
	{
		SetDiagnostics(Errors);
	}
	else if (Errors.Num() > 0)
	{
		SetDiagnostics(Errors);
	}

	if (Session)
	{
		if (!Session->SyncNodeIdsPreservingMapping())
		{
			Session->AddDiagnostic(TEXT("资产数组结构已在其他入口变化：节点身份已重建，选择已清空。"));
		}
	}

	const TArray<FString>& CurrentDiagnostics = Session ? Session->GetDiagnostics() : Diagnostics;
	Diagnostics = CurrentDiagnostics;

	if (DetailsView.IsValid())
	{
		DetailsView->RequestForceRefresh();
	}

	RefreshShotTable();
	if (PreviewWidget) PreviewWidget->RefreshProfile();
}

uint64 FLyraRecoilProfileEditor::GetRevision() const
{
	return Session ? Session->GetRevision() : 0;
}

void FLyraRecoilProfileEditor::SetDiagnostics(const TArray<FString>& InErrors)
{
	Diagnostics = InErrors;
	if (Session)
	{
		Session->SetDiagnostics(InErrors);
	}
}

void FLyraRecoilProfileEditor::RefreshShotTable()
{
	if (ShotTable.IsValid())
	{
		ShotTable->RefreshRows();
	}
}

// ---------------------------------------------------------------------------
// 结构与编辑命令
// ---------------------------------------------------------------------------

bool FLyraRecoilProfileEditor::ConfirmStructureChange(const FLyraRecoilPatternData& Candidate,const TArray<int32>& Order)
{
    if (!Profile) return false;
    if (FLyraRecoilPatternAdapter::Equal(Candidate,FLyraRecoilPatternAdapter::Read(*Profile))) return true;
    FString Preview=FString::Printf(TEXT("数组 N：%d → %d；固定段 L：%d → %d\n新发序号决定曲线采样；跨固定段边界的 X 会开始或停止生效。\n"),
        Profile->PatternPoints.Num(),Candidate.Points.Num(),Profile->PatternLength,Candidate.PatternLength);
    TArray<FVector2D> Cumulative;
    TArray<FString> Errors;
    if (!FLyraRecoilPatternAdapter::BuildCumulativeFor(*Profile,Candidate,Cumulative,Errors))
    { SetDiagnostics(Errors); return false; }
    for (int32 I=0; I<Candidate.Points.Num() && I<24; ++I)
    {
        const auto& P=Candidate.Points[I];
        const double Pitch=Profile->RecoilPerShot_Vertical*static_cast<double>(P.Y)*Profile->GetVerticalKickCurveScale(I);
        const FString Old=Order.IsValidIndex(I)?FString::Printf(TEXT("（原第%d发）"),Order[I]+1):FString();
        Preview+=FString::Printf(TEXT("第%d发%s %s：X=%.6g Y=%.6g ΔPitch=%.6g°"),I+1,*Old,I<Candidate.PatternLength?TEXT("固定"):TEXT("尾段"),P.X,P.Y,Pitch);
        if (Cumulative.IsValidIndex(I)) Preview+=FString::Printf(TEXT(" 累计Yaw/Pitch=%.6g/%.6g°"),Cumulative[I].X,Cumulative[I].Y);
        Preview+=TEXT("\n");
    }
    if (Candidate.Points.Num()>24) Preview+=TEXT("其余节点保留原始归一化参数并按新索引求值。\n");
    Preview+=TEXT("\n确认应用一个事务？");
    return FMessageDialog::Open(EAppMsgType::YesNo,FText::FromString(Preview))==EAppReturnType::Yes;
}

bool FLyraRecoilProfileEditor::InsertPoints(int32 Index, const TArray<FRecoilPatternPoint>& NewPoints, bool bFixedAtBoundary)
{
	if (!Profile)
	{
		return false;
	}

	TArray<FString> Errors;
	FLyraRecoilPatternData Candidate;
	if (!FLyraRecoilPatternAdapter::Insert(FLyraRecoilPatternAdapter::Read(*Profile),Index,NewPoints,bFixedAtBoundary,Candidate,Errors))
	{ SetDiagnostics(Errors); return false; }
	if (!ConfirmStructureChange(Candidate)) return false;
	const bool bCommitted = Operations.InsertPoints(Index, NewPoints, bFixedAtBoundary, Errors);
	SetDiagnostics(Errors);

	if (!bCommitted)
	{
		ShowNotification(TEXT("插入被拒绝（详见诊断面板）。"), /*bIsError=*/true);
		return false;
	}

	RefreshFromAsset();

	if (Session && NewPoints.Num() > 0)
	{
		TArray<int32> NewSelection;
		for (int32 Offset = 0; Offset < NewPoints.Num(); ++Offset)
		{
			NewSelection.Add(Index + Offset);
		}
		Session->SelectIndices(NewSelection, /*bAppend=*/false);
		RefreshShotTable();
	}

	return true;
}

bool FLyraRecoilProfileEditor::DeleteSelectedPoints()
{
	if (!Session)
	{
		return false;
	}

	const TArray<int32> Indices = Session->GetSelectedIndices();
	if (Indices.Num() == 0)
	{
		SetDiagnostics({ TEXT("没有选中的节点可删除。") });
		return false;
	}

	TArray<FString> Errors;
	FLyraRecoilPatternData Candidate;
	if (!FLyraRecoilPatternAdapter::Delete(FLyraRecoilPatternAdapter::Read(*Profile),Indices,Candidate,Errors))
	{ SetDiagnostics(Errors); return false; }
	if (!ConfirmStructureChange(Candidate)) return false;
	const bool bCommitted = Operations.DeletePoints(Indices, Errors);
	SetDiagnostics(Errors);

	if (!bCommitted)
	{
		ShowNotification(TEXT("删除被拒绝（详见诊断面板）。"), /*bIsError=*/true);
		return false;
	}

	Session->ClearSelection();
	RefreshFromAsset();
	return true;
}

bool FLyraRecoilProfileEditor::MoveSelectedRows(int32 Delta)
{
	if (!Session || !Profile || Delta == 0)
	{
		return false;
	}

	const TArray<int32> Selected = Session->GetSelectedIndices();
	if (Selected.Num() == 0)
	{
		SetDiagnostics({ TEXT("没有选中的节点可重排。") });
		return false;
	}

	const int32 Num = Profile->PatternPoints.Num();
	TArray<int32> Order;
	Order.Reserve(Num);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		Order.Add(Index);
	}

	if (Delta < 0)
	{
		for (int32 Index : Selected)
		{
			if (Index > 0)
			{
				Swap(Order[Index], Order[Index - 1]);
			}
		}
	}
	else
	{
		for (int32 SelectionIndex = Selected.Num() - 1; SelectionIndex >= 0; --SelectionIndex)
		{
			const int32 Index = Selected[SelectionIndex];
			if (Index + 1 < Num)
			{
				Swap(Order[Index], Order[Index + 1]);
			}
		}
	}

	TArray<FString> Errors;
	FLyraRecoilPatternData Candidate;
	if (!FLyraRecoilPatternAdapter::Reorder(FLyraRecoilPatternAdapter::Read(*Profile),Order,Candidate,Errors))
	{ SetDiagnostics(Errors); return false; }
	if (!ConfirmStructureChange(Candidate,Order)) return false;
	const bool bCommitted = Operations.ReorderPoints(Order, Errors);
	SetDiagnostics(Errors);

	if (!bCommitted)
	{
		ShowNotification(TEXT("重排被拒绝（详见诊断面板）。"), /*bIsError=*/true);
		return false;
	}

	// 选择按稳定 ID 跟随节点，不需要重设索引。
	RefreshFromAsset();
	return true;
}

bool FLyraRecoilProfileEditor::SetPatternLengthValue(int32 NewLength)
{
	TArray<FString> Errors;
	FLyraRecoilPatternData Candidate;
	if (!Profile || !FLyraRecoilPatternAdapter::SetPatternLength(FLyraRecoilPatternAdapter::Read(*Profile),NewLength,Candidate,Errors))
	{ SetDiagnostics(Errors); return false; }
	if (!ConfirmStructureChange(Candidate)) return false;
	const bool bCommitted = Operations.SetPatternLength(NewLength, Errors);
	SetDiagnostics(Errors);

	if (!bCommitted)
	{
		ShowNotification(TEXT("固定段长度修改被拒绝（详见诊断面板）。"), /*bIsError=*/true);
		return false;
	}

	RefreshFromAsset();
	return true;
}

bool FLyraRecoilProfileEditor::SortByCumulativePitch()
{
	TArray<int32> Order;
	TArray<FString> Errors;
	if (!Operations.BuildCumulativePitchSortOrder(Order, Errors))
	{
		SetDiagnostics(Errors);
		return false;
	}

	FLyraRecoilPatternData Candidate;
	if (!FLyraRecoilPatternAdapter::Reorder(FLyraRecoilPatternAdapter::Read(*Profile),Order,Candidate,Errors)) return false;
	if (!ConfirmStructureChange(Candidate,Order)) return false;
	const bool bCommitted = Operations.ReorderPoints(Order, Errors);
	SetDiagnostics(Errors);

	if (!bCommitted)
	{
		ShowNotification(TEXT("排序被拒绝（详见诊断面板）。"), /*bIsError=*/true);
		return false;
	}

	RefreshFromAsset();
	return true;
}

bool FLyraRecoilProfileEditor::MoveSelectedPoints(const TArray<FVector2D>& Targets)
{
	return CommitCumulative(Targets);
}

bool FLyraRecoilProfileEditor::SetPointNormalized(int32 Index, float X, float Y)
{
	TArray<FString> Errors;
	const bool bCommitted = Operations.CommitPointNormalized(Index, X, Y, Errors);
	SetDiagnostics(Errors);

	if (!bCommitted)
	{
		ShowNotification(TEXT("该发的候选值被拒绝（详见诊断面板）。"), /*bIsError=*/true);
		return false;
	}

	RefreshFromAsset();
	return true;
}

bool FLyraRecoilProfileEditor::SetCumulativeValue(int32 Index, bool bPitchAxis, double Value)
{
	if (!CumulativePoints.IsValidIndex(Index))
	{
		SetDiagnostics({ FString::Printf(
			TEXT("第 %d 发不在固定段内（当前固定段长度 %d），累计角度不可编辑。"), Index + 1, CumulativePoints.Num()) });
		return false;
	}

	TArray<FVector2D> Targets = CumulativePoints;
	if (bPitchAxis)
	{
		Targets[Index].Y = Value;
	}
	else
	{
		Targets[Index].X = Value;
	}

	return CommitCumulative(Targets);
}

void FLyraRecoilProfileEditor::BeginGesture()
{
	GestureStartRevision = Operations.BeginGesture();

	if (Session)
	{
		Session->BeginDragDraft(CumulativePoints);
	}
}

bool FLyraRecoilProfileEditor::EndGesture(const TArray<FVector2D>& Targets)
{
	TArray<FString> Errors;
	if (!Operations.ValidateGestureRevision(GestureStartRevision, Errors))
	{
		// 修订冲突：直接丢弃草稿，不需要反向写回。
		CancelActiveGesture();
		SetDiagnostics(Errors);
		ShowNotification(TEXT("资产已在其他入口更新，本次拖动草稿已丢弃。"), /*bIsError=*/true);
		return false;
	}

	const bool bCommitted = CommitCumulative(Targets);

	if (Session)
	{
		Session->CancelDragDraft();
	}
	GestureStartRevision = 0;

	return bCommitted;
}

void FLyraRecoilProfileEditor::CancelActiveGesture()
{
	if (PatternGraph) PatternGraph->CancelDrag();
	if (Session)
	{
		Session->CancelDragDraft();
	}
	GestureStartRevision = 0;
}

// ---------------------------------------------------------------------------
// 曲线所有权
// ---------------------------------------------------------------------------

bool FLyraRecoilProfileEditor::IsCurveShared(FName PropertyName) const
{
	return Profile && FLyraRecoilEditOperations::IsCurveExternallyShared(Profile, PropertyName);
}

FString FLyraRecoilProfileEditor::GetCurveOwnershipText(FName PropertyName) const
{
	if (!Profile)
	{
		return FString();
	}

	if (const UCurveFloat* External = FLyraRecoilEditOperations::GetExternalCurve(Profile, PropertyName))
	{
		return FString::Printf(TEXT("共享引用（只读）：%s"), *External->GetPathName());
	}

	return TEXT("内联曲线（可在 Profile 事务中编辑）");
}

bool FLyraRecoilProfileEditor::CopyCurveToInline(FName PropertyName)
{
	TArray<FString> Errors;
	const bool bDone = Operations.CopyCurveToInline(PropertyName, Errors);
	SetDiagnostics(Errors);

	if (bDone)
	{
		RefreshFromAsset();
	}
	else
	{
		ShowNotification(TEXT("复制为内联曲线失败（详见诊断面板）。"), /*bIsError=*/true);
	}

	return bDone;
}

bool FLyraRecoilProfileEditor::DuplicateCurveAsset(FName PropertyName)
{
	TArray<FString> Errors;
	const bool bDone = Operations.DuplicateCurveAsset(PropertyName, Errors);
	SetDiagnostics(Errors);

	if (bDone)
	{
		RefreshFromAsset();
	}
	else
	{
		ShowNotification(TEXT("复制为新曲线资产失败（详见诊断面板）。"), /*bIsError=*/true);
	}

	return bDone;
}

bool FLyraRecoilProfileEditor::OpenSharedCurveAsset(FName PropertyName)
{
	TArray<FString> Errors;
	const bool bDone = Operations.OpenSharedCurveAsset(PropertyName, Errors);
	SetDiagnostics(Errors);

	if (!bDone)
	{
		ShowNotification(TEXT("打开共享曲线资产失败（详见诊断面板）。"), /*bIsError=*/true);
	}

	return bDone;
}

void FLyraRecoilProfileEditor::RefreshDetails()
{
	if (DetailsView.IsValid())
	{
		DetailsView->RequestForceRefresh();
	}
}

// ---------------------------------------------------------------------------
// 保存 / 另存
// ---------------------------------------------------------------------------

bool FLyraRecoilProfileEditor::CanSaveAsset() const
{
	// 保持可见可点：保存失败时给出明确诊断，而不是静默禁用。
	return true;
}

void FLyraRecoilProfileEditor::SaveAsset_Execute()
{
	// 先结束当前有效输入或取消拖动。
	CancelActiveGesture();
	RefreshFromAsset();

	TArray<FString> Errors;
	if (!ValidateBeforeSave(Errors))
	{
		SetDiagnostics(Errors);
		ShowNotification(TEXT("保存已取消：资产未通过校验，内存修改与 Dirty 标记保留（详见诊断面板）。"), /*bIsError=*/true);
		return;
	}

	// 只保存 GetSaveableObjects() 返回的目标 Profile 包；引擎负责源控/只读/失败处理。
	FAssetEditorToolkit::SaveAsset_Execute();
}

void FLyraRecoilProfileEditor::SaveAssetAs_Execute()
{
	CancelActiveGesture();
	RefreshFromAsset();

	TArray<FString> Errors;
	if (!ValidateBeforeSave(Errors))
	{
		SetDiagnostics(Errors);
		ShowNotification(TEXT("另存已取消：资产未通过校验（详见诊断面板）。"), /*bIsError=*/true);
		return;
	}

	// 引擎标准另存：以当前已提交内存值复制为新资产，成功保存后打开新编辑器；
	// 原资产 Dirty 状态与武器引用保持不变。
	FAssetEditorToolkit::SaveAssetAs_Execute();
}

bool FLyraRecoilProfileEditor::ValidateBeforeSave(TArray<FString>& OutErrors) const
{
	if (!Profile)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile。"));
		return false;
	}

	return Operations.ValidateProfileWithAdapter(OutErrors);
}

void FLyraRecoilProfileEditor::ShowNotification(const FString& Message, bool bIsError) const
{
	FNotificationInfo Info(FText::FromString(Message));
	Info.ExpireDuration = bIsError ? 8.0f : 3.0f;
	Info.bUseSuccessFailIcons = true;

	const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (Item.IsValid())
	{
		Item->SetCompletionState(bIsError ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
	}
}

// ---------------------------------------------------------------------------
// 外部监听
// ---------------------------------------------------------------------------

void FLyraRecoilProfileEditor::BindExternalListeners()
{
	ProfilePropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
		this, &FLyraRecoilProfileEditor::HandleProfilePropertyChanged);
	ObjectsReplacedHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddRaw(
		this, &FLyraRecoilProfileEditor::HandleObjectsReplaced);
	ObjectTransactedHandle = FCoreUObjectDelegates::OnObjectTransacted.AddRaw(
		this, &FLyraRecoilProfileEditor::HandleObjectTransacted);

	RebindCurveDependencies();
}

void FLyraRecoilProfileEditor::UnbindExternalListeners()
{
	if (ProfilePropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(ProfilePropertyChangedHandle);
		ProfilePropertyChangedHandle.Reset();
	}
	if (ObjectsReplacedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectsReplaced.Remove(ObjectsReplacedHandle);
		ObjectsReplacedHandle.Reset();
	}
	if (ObjectTransactedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectTransacted.Remove(ObjectTransactedHandle);
		ObjectTransactedHandle.Reset();
	}

	UnbindCurveDependencies();
}

void FLyraRecoilProfileEditor::UnbindCurveDependencies()
{
	for (FDelegateHandle& Handle : CurveDependencyHandles)
	{
		if (Handle.IsValid())
		{
			FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle);
		}
	}
	CurveDependencyHandles.Reset();
}

void FLyraRecoilProfileEditor::RebindCurveDependencies()
{
    UnbindCurveDependencies();
    if (!Profile) return;
    for (FName Name : FLyraRecoilEditOperations::GetCurvePropertyNames())
    {
        if (FLyraRecoilEditOperations::GetExternalCurve(Profile, Name))
        {
            CurveDependencyHandles.Add(FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
                this, &FLyraRecoilProfileEditor::HandleCurvePropertyChanged));
            break; // One global delegate, filtered against all current references.
        }
    }
}

FLyraRecoilProfileEditor::~FLyraRecoilProfileEditor()
{
    UnbindExternalListeners();
    UnregisterEditorInstance();
    if (PreviewWidget) PreviewWidget->Cleanup();
    if (GEditor) GEditor->UnregisterForUndo(this);
}

void FLyraRecoilProfileEditor::PostUndo(bool bSuccess)
{
    if (!bSuccess || !Profile) return;
    CancelActiveGesture();
    if (PatternGraph) PatternGraph->CancelDrag();
    if (Session) Session->BumpRevision();
    RebindCurveDependencies();
    RefreshFromAsset();
}

bool FLyraRecoilProfileEditor::AdjustStrengthForRejectedTargets()
{
    if (!Profile || !PatternGraph) return false;
    FLyraRecoilStrengthAdjustment Adjustment;
    FLyraRecoilPatternData Candidate;
    TArray<FString> Errors;
    const auto Targets = PatternGraph->GetRejectedTargets();
    if (!FLyraRecoilPatternAdapter::BuildStrengthAdjustmentCandidate(*Profile,Targets,Adjustment,Candidate,Errors))
    { SetDiagnostics(Errors); return false; }
    const FText Message = FText::FromString(FString::Join(Adjustment.Differences,TEXT("\n"))
        + TEXT("\n\n随机尾段采样仅供比较；上限、姿态、曲线、随机区间保持当前值。确认提交一个事务？"));
    if (FMessageDialog::Open(EAppMsgType::YesNo,Message)!=EAppReturnType::Yes) return false;
    const bool bCommitted=Operations.CommitStrengthAdjustment(Targets,Errors);
    SetDiagnostics(Errors);
    if (bCommitted) RefreshFromAsset();
    return bCommitted;
}

void FLyraRecoilProfileEditor::HandleProfilePropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	if (Object != Profile)
	{
		return;
	}

	// 自提交作用域内不重复重建；调用方在提交后统一刷新一次。
	if (Operations.IsCommitting() || GIsTransacting)
	{
		// Undo restores Profile and Session in engine-defined order. Wait for PostUndo
		// rather than treating an intermediate PostEditUndo notification as an external edit.
		return;
	}

	// 外部修改到来时先终止本地拖动，再接收最终资产值，不覆盖外部新值。
	CancelActiveGesture();
    if (PatternGraph) PatternGraph->CancelDrag();
    if (Session && (!Event.MemberProperty || (Event.MemberProperty->GetFName()==GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile,PatternPoints)
        && (Event.ChangeType!=EPropertyChangeType::ValueSet || Event.GetArrayIndex(TEXT("PatternPoints"))==INDEX_NONE))))
    {
        Session->RebuildNodeIds();
        Session->AddDiagnostic(TEXT("外部数组结构无法证明未变：节点身份已重建，选择已清空。"));
    }

	if (Session)
	{
		Session->BumpRevision();
	}

	RebindCurveDependencies();
	RefreshFromAsset();
}

void FLyraRecoilProfileEditor::HandleCurvePropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	if (!Object || !Profile || GIsTransacting)
	{
		return;
	}

	bool bIsBoundDependency = false;
	for (const FName& CurveName : FLyraRecoilEditOperations::GetCurvePropertyNames())
	{
		if (FLyraRecoilEditOperations::GetExternalCurve(Profile, CurveName) == Object)
		{
			bIsBoundDependency = true;
			break;
		}
	}

	if (!bIsBoundDependency)
	{
		return;
	}
	CancelActiveGesture();
	if (PatternGraph) PatternGraph->CancelDrag();

	if (Session)
	{
		Session->BumpRevision();
	}

	RefreshFromAsset();
}

void FLyraRecoilProfileEditor::HandleObjectsReplaced(const FCoreUObjectDelegates::FReplacementObjectMap& ReplacementMap)
{
	bool bRelevant = false;

	if (Profile)
	{
		if (UObject* const* Replacement = ReplacementMap.Find(Profile.Get()))
		{
			ULyraRecoilProfile* NewProfile = Cast<ULyraRecoilProfile>(*Replacement);
			if (NewProfile)
			{
				CancelActiveGesture();
				if (PatternGraph) PatternGraph->CancelDrag();
				UnregisterEditorInstance();
				RemoveEditingObject(Profile);
				Profile = NewProfile;
				AddEditingObject(Profile);
				RegisterEditorInstance();
				if (PreviewWidget) PreviewWidget->SetProfile(Profile);
				Operations.Initialize(Profile, Session);
				if (Session)
				{
					Session->SetProfile(Profile);
				}
				if (DetailsView.IsValid())
				{
					DetailsView->SetObject(Profile, /*bForceRefresh=*/true);
				}
				bRelevant = true;
			}
		}
	}

	// 被引用的共享曲线可能被替换：重新解析依赖。
	RebindCurveDependencies();

	if (bRelevant)
	{
		RefreshFromAsset();
	}
}

void FLyraRecoilProfileEditor::HandleObjectTransacted(UObject* Object, const FTransactionObjectEvent& Event)
{
    // Undo/redo restoration is refreshed once, after all transaction objects restore, in PostUndo.
}

// ---------------------------------------------------------------------------
// 命令处理
// ---------------------------------------------------------------------------

void FLyraRecoilProfileEditor::OnSelectAllCommand()
{
	if (!Session || !Profile)
	{
		return;
	}

	TArray<int32> AllIndices;
	AllIndices.Reserve(Profile->PatternPoints.Num());
	for (int32 Index = 0; Index < Profile->PatternPoints.Num(); ++Index)
	{
		AllIndices.Add(Index);
	}

	Session->SelectIndices(AllIndices, /*bAppend=*/false);
	RefreshShotTable();
}

void FLyraRecoilProfileEditor::OnCopyCommand()
{
	TArray<FString> Errors;
	if (!Operations.CopySelectionToClipboard(Errors))
	{
		SetDiagnostics(Errors);
		return;
	}

	SetDiagnostics(TArray<FString>());
}

void FLyraRecoilProfileEditor::OnPasteCommand()
{
    if (!Profile) return;
    FString Payload; FPlatformApplicationMisc::ClipboardPaste(Payload);
    TArray<FString> Errors;
    TArray<FRecoilPatternPoint> Points;
    const auto& Selected=GetSelectedIndices();
    const int32 Index=Selected.IsEmpty()?Profile->PatternPoints.Num():Selected[0];
    if (!FLyraRecoilPatternAdapter::ParseClipboard(Payload,Points,Errors)) { SetDiagnostics(Errors); return; }
    InsertPoints(Index,Points,false);
}

void FLyraRecoilProfileEditor::OnPasteByAngleCommand()
{
    if (!Profile) return;
    FString Payload; FPlatformApplicationMisc::ClipboardPaste(Payload);
    TArray<FString> Errors;
    TArray<FRecoilPatternPoint> Points;
    const auto& Selected=GetSelectedIndices();
    const int32 Index=Selected.IsEmpty()?Profile->PatternPoints.Num():Selected[0];
    FLyraRecoilClipboardData Clipboard;
    if (!FLyraRecoilPatternAdapter::ParseClipboardEx(Payload,Clipboard,Errors)
        || !FLyraRecoilPatternAdapter::ConvertClipboardAngles(*Profile,Clipboard,Index,Points,Errors))
    { SetDiagnostics(Errors); return; }
    InsertPoints(Index,Points,false);
}

void FLyraRecoilProfileEditor::OnInsertPointCommand()
{
	if (!Profile)
	{
		return;
	}

	const TArray<int32>& Selected = GetSelectedIndices();
	const int32 InsertIndex = Selected.Num() > 0 ? Selected[0] : Profile->PatternPoints.Num();

	TArray<FRecoilPatternPoint> NewPoints;
	NewPoints.Add(FRecoilPatternPoint(0.0f, 0.0f));

	// 插入点默认不改变固定段长度（k == N 时归入尾段）；需要改 L 时走显式命令。
	InsertPoints(InsertIndex, NewPoints, /*bFixedAtBoundary=*/false);
}

void FLyraRecoilProfileEditor::OnDeleteSelectedCommand()
{
	DeleteSelectedPoints();
}

void FLyraRecoilProfileEditor::OnMoveRowUpCommand()
{
	MoveSelectedRows(-1);
}

void FLyraRecoilProfileEditor::OnMoveRowDownCommand()
{
	MoveSelectedRows(+1);
}

void FLyraRecoilProfileEditor::OnSortByPitchCommand()
{
	SortByCumulativePitch();
}

void FLyraRecoilProfileEditor::OnValidateCommand()
{
	TArray<FString> Errors;
	const bool bValid = Operations.ValidateProfileWithAdapter(Errors);
	SetDiagnostics(Errors);

	ShowNotification(bValid ? TEXT("校验通过：Profile 与编辑器附加检查全部合格。")
		: TEXT("校验发现问题（详见诊断面板）。"), /*bIsError=*/!bValid);
}

bool FLyraRecoilProfileEditor::CanDeleteSelection() const
{
	return Session != nullptr && Session->GetSelectedIndices().Num() > 0;
}

bool FLyraRecoilProfileEditor::CanReorderSelection() const
{
	return Profile != nullptr && Profile->PatternPoints.Num() > 1;
}

bool FLyraRecoilProfileEditor::CanCopySelection() const
{
	return Session != nullptr && Session->GetSelectedIndices().Num() > 0;
}

// ---------------------------------------------------------------------------
// Details 定制工厂
// ---------------------------------------------------------------------------

TSharedRef<IDetailCustomization> FLyraRecoilProfileEditor::MakeDetailsCustomization()
{
	return MakeShared<FLyraRecoilProfileDetails>(SharedThis(this));
}

void FLyraRecoilProfileEditor::OnDetailsFinishedChangingProperties(const FPropertyChangedEvent& Event)
{
    // OnObjectPropertyChanged is the single refresh route for ordinary Details edits.
}

// ---------------------------------------------------------------------------
// P3 / P4 占位
// ---------------------------------------------------------------------------

TSharedRef<SWidget> FLyraRecoilProfileEditor::CreatePatternGraph()
{
    return SNew(SVerticalBox)
    + SVerticalBox::Slot().AutoHeight()
    [ SNew(SHorizontalBox)
      + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("适配全部 Home")))
        .OnClicked_Lambda([this] { if (PatternGraph) PatternGraph->FitAll(); return FReply::Handled(); })]
      + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("适配选择 F")))
        .OnClicked_Lambda([this] { if (PatternGraph) PatternGraph->FitSelection(); return FReply::Handled(); })]
      + SHorizontalBox::Slot().AutoWidth()[SNew(SCheckBox)
        .IsChecked_Lambda([this] { return PatternGraph && PatternGraph->IsSnapEnabled() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { if (PatternGraph) PatternGraph->SetSnapEnabled(State==ECheckBoxState::Checked); })
        [SNew(STextBlock).Text(FText::FromString(TEXT("吸附 G")))]]
      + SHorizontalBox::Slot().AutoWidth()[SNew(SNumericEntryBox<double>).MinDesiredValueWidth(60).MinValue(.000001).MaxValue(10000)
        .Value_Lambda([this]() -> TOptional<double> { return PatternGraph ? PatternGraph->GetSnapStep() : .1; })
        .OnValueCommitted_Lambda([this](double Value,ETextCommit::Type) { if (PatternGraph) PatternGraph->SetSnapStep(Value); })]
    ]
    + SVerticalBox::Slot().FillHeight(1)[SAssignNew(PatternGraph, SLyraRecoilPatternGraph).Editor(SharedThis(this))]
    + SVerticalBox::Slot().AutoHeight()[SNew(SButton).Text(FText::FromString(TEXT("高级：调整基础强度以容纳被拒绝的目标")))
        .IsEnabled_Lambda([this] { return PatternGraph && !PatternGraph->GetRejectedTargets().IsEmpty(); })
        .OnClicked_Lambda([this] { AdjustStrengthForRejectedTargets(); return FReply::Handled(); })]
    + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true)
        .Text_Lambda([this] { return FText::FromString(FString::Join(Diagnostics,TEXT("\n"))); })];
}

#if WITH_DEV_AUTOMATION_TESTS
void FLyraRecoilProfileEditor::StartGraphPerformanceCapture()
{
    if (PatternGraph) PatternGraph->StartPerformanceCapture();
}
FString FLyraRecoilProfileEditor::GetGraphPerformanceSummary() const
{
    return PatternGraph ? PatternGraph->GetPerformanceSummary() : TEXT("Graph closed\n");
}
#endif

TSharedRef<SWidget> FLyraRecoilProfileEditor::CreatePreview()
{
    return SAssignNew(PreviewWidget, SLyraRecoilPreview).Profile(Profile);
}

// ---------------------------------------------------------------------------
// 工具包注册表
// ---------------------------------------------------------------------------

void FLyraRecoilProfileEditor::RegisterEditorInstance()
{
	if (Profile)
	{
		EditorInstances.Add(TWeakObjectPtr<ULyraRecoilProfile>(Profile.Get()), SharedThis(this));
	}
}

void FLyraRecoilProfileEditor::UnregisterEditorInstance()
{
	if (Profile)
	{
		EditorInstances.Remove(TWeakObjectPtr<ULyraRecoilProfile>(Profile.Get()));
	}
}

TSharedPtr<FLyraRecoilProfileEditor> FLyraRecoilProfileEditor::FindEditorForProfile(const ULyraRecoilProfile* InProfile)
{
	if (!InProfile)
	{
		return nullptr;
	}

	// 顺带清理失效条目（资产被 GC 或工具包已关闭）。
	for (auto It = EditorInstances.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid() || !It.Value().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	for (const TPair<TWeakObjectPtr<ULyraRecoilProfile>, TWeakPtr<FLyraRecoilProfileEditor>>& Pair : EditorInstances)
	{
		if (Pair.Key.Get() == InProfile)
		{
			return Pair.Value.Pin();
		}
	}

	return nullptr;
}

#undef LOCTEXT_NAMESPACE
