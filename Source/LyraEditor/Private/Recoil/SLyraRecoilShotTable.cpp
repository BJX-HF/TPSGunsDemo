// Copyright Epic Games, Inc. All Rights Reserved.

#include "Recoil/SLyraRecoilShotTable.h"

#include "Recoil/LyraRecoilEditorSession.h"
#include "Recoil/LyraRecoilProfileEditor.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"

#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "SLyraRecoilShotTable"

namespace LyraRecoilShotTableColumns
{
	static const FName Index("Index");
	static const FName Segment("Segment");
	static const FName RawX("RawX");
	static const FName RawY("RawY");
	static const FName DeltaYaw("DeltaYaw");
	static const FName DeltaPitch("DeltaPitch");
	static const FName CumulativeYaw("CumulativeYaw");
	static const FName CumulativePitch("CumulativePitch");
	static const FName TailXState("TailXState");
	static const FName Validation("Validation");
}

// ---------------------------------------------------------------------------
// SLyraRecoilShotTable
// ---------------------------------------------------------------------------

void SLyraRecoilShotTable::Construct(const FArguments& InArgs)
{
	EditorWeak = InArgs._Editor;

	TSharedRef<SHeaderRow> HeaderRow = SNew(SHeaderRow);
	HeaderRow->AddColumn(SHeaderRow::Column(LyraRecoilShotTableColumns::Index)
		.DefaultLabel(LOCTEXT("ColIndex", "序号"))
		.FixedWidth(46.0f));
	HeaderRow->AddColumn(SHeaderRow::Column(LyraRecoilShotTableColumns::Segment)
		.DefaultLabel(LOCTEXT("ColSegment", "段"))
		.FixedWidth(52.0f));
	HeaderRow->AddColumn(SHeaderRow::Column(LyraRecoilShotTableColumns::RawX)
		.DefaultLabel(LOCTEXT("ColRawX", "原值 X"))
		.FixedWidth(84.0f));
	HeaderRow->AddColumn(SHeaderRow::Column(LyraRecoilShotTableColumns::RawY)
		.DefaultLabel(LOCTEXT("ColRawY", "原值 Y"))
		.FixedWidth(84.0f));
	HeaderRow->AddColumn(SHeaderRow::Column(LyraRecoilShotTableColumns::DeltaYaw)
		.DefaultLabel(LOCTEXT("ColDeltaYaw", "逐发 ΔYaw°"))
		.FixedWidth(92.0f));
	HeaderRow->AddColumn(SHeaderRow::Column(LyraRecoilShotTableColumns::DeltaPitch)
		.DefaultLabel(LOCTEXT("ColDeltaPitch", "逐发 ΔPitch°"))
		.FixedWidth(100.0f));
	HeaderRow->AddColumn(SHeaderRow::Column(LyraRecoilShotTableColumns::CumulativeYaw)
		.DefaultLabel(LOCTEXT("ColCumulativeYaw", "累计 Yaw°"))
		.FixedWidth(96.0f));
	HeaderRow->AddColumn(SHeaderRow::Column(LyraRecoilShotTableColumns::CumulativePitch)
		.DefaultLabel(LOCTEXT("ColCumulativePitch", "累计 Pitch°"))
		.FixedWidth(104.0f));
	HeaderRow->AddColumn(SHeaderRow::Column(LyraRecoilShotTableColumns::TailXState)
		.DefaultLabel(LOCTEXT("ColTailX", "尾X状态"))
		.FillWidth(1.0f));
	HeaderRow->AddColumn(SHeaderRow::Column(LyraRecoilShotTableColumns::Validation)
		.DefaultLabel(LOCTEXT("ColValidation", "校验"))
		.FillWidth(1.4f));

	ChildSlot
	[
		SNew(SVerticalBox)

		// 结构命令工具栏：所有结构操作都走同一命令服务，规则见计划 §07。
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(3.0f)
		[
			SNew(SWrapBox).UseAllottedSize(true)
			+ SWrapBox::Slot().Padding(2.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("InsertButton", "插入节点"))
				.ToolTipText(LOCTEXT("InsertTip", "在选中发之前插入默认 (0,0) 节点；k==L 时默认归入尾段"))
				.OnClicked(this, &SLyraRecoilShotTable::OnInsertClicked)
			]
			+ SWrapBox::Slot().Padding(2.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("DeleteButton", "删除选中"))
				.OnClicked(this, &SLyraRecoilShotTable::OnDeleteClicked)
			]
			+ SWrapBox::Slot().Padding(2.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("MoveUpButton", "上移"))
				.ToolTipText(LOCTEXT("MoveUpTip", "显式重排：与上一发交换（不按高度自动排序）"))
				.OnClicked(this, &SLyraRecoilShotTable::OnMoveUpClicked)
			]
			+ SWrapBox::Slot().Padding(2.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("MoveDownButton", "下移"))
				.OnClicked(this, &SLyraRecoilShotTable::OnMoveDownClicked)
			]
			+ SWrapBox::Slot().Padding(2.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("SortButton", "按累计 Pitch 排序"))
				.ToolTipText(LOCTEXT("SortTip", "一次性稳定排序；执行前展示重排与曲线索引变化"))
				.OnClicked(this, &SLyraRecoilShotTable::OnSortByPitchClicked)
			]
			+ SWrapBox::Slot().Padding(6.0f, 2.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("PatternLengthLabel", "固定段长度 L"))
			]
			+ SWrapBox::Slot().Padding(2.0f)
			[
				SNew(SNumericEntryBox<int32>)
				.Value(this, &SLyraRecoilShotTable::GetPatternLengthValue)
				.MinValue(TOptional<int32>(0))
				.AllowSpin(true)
				.OnValueCommitted(this, &SLyraRecoilShotTable::OnPatternLengthCommitted)
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(2.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			[
				SNew(SScrollBox).Orientation(Orient_Horizontal)
                +SScrollBox::Slot()[SNew(SBox).WidthOverride(1140)
                [SAssignNew(ListView, SListView<TSharedPtr<FLyraRecoilShotRow>>)
				.ListItemsSource(&Rows)
				.HeaderRow(HeaderRow)
				.SelectionMode(ESelectionMode::Multi)
				.OnGenerateRow(this, &SLyraRecoilShotTable::OnGenerateRow)
				.OnSelectionChanged(this, &SLyraRecoilShotTable::OnSelectionChanged)]]
			]
		]

		// 诊断：错误不能只依赖颜色，这里给出文字原因与解决方向。
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(this, &SLyraRecoilShotTable::GetStatusText)
		]
	];

	RefreshRows();
}

TSharedRef<ITableRow> SLyraRecoilShotTable::OnGenerateRow(TSharedPtr<FLyraRecoilShotRow> Row, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SLyraRecoilShotTableRow, OwnerTable)
		.RowData(Row)
		.Editor(EditorWeak);
}

void SLyraRecoilShotTable::OnSelectionChanged(TSharedPtr<FLyraRecoilShotRow> Row, ESelectInfo::Type SelectInfo)
{
	if (bIsSyncingSelection || !ListView.IsValid())
	{
		return;
	}

	TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin();
	if (!Editor)
	{
		return;
	}

	TArray<int32> SelectedIndices;
	for (const TSharedPtr<FLyraRecoilShotRow>& Item : Rows)
	{
		if (Item.IsValid() && ListView->IsItemSelected(Item))
		{
			SelectedIndices.Add(Item->Index);
		}
	}

	// 守卫：SelectIndices 会触发 RefreshShotTable，避免在同步期间重建行。
	TGuardValue<bool> Guard(bIsSyncingSelection, true);
	Editor->SelectIndices(SelectedIndices, /*bAppend=*/false);
}

void SLyraRecoilShotTable::SyncListSelection()
{
	if (!ListView.IsValid())
	{
		return;
	}

	TGuardValue<bool> Guard(bIsSyncingSelection, true);
	ListView->ClearSelection();

	TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin();
	if (!Editor)
	{
		return;
	}

	for (int32 Index : Editor->GetSelectedIndices())
	{
		if (Rows.IsValidIndex(Index))
		{
			ListView->SetItemSelection(Rows[Index], true, ESelectInfo::Direct);
		}
	}
}

void SLyraRecoilShotTable::RefreshRows()
{
	// 选择同步期间不重建，避免 SListView 在回调中重建列表。
	if (bIsSyncingSelection)
	{
		return;
	}

	Rows.Reset();

	TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin();
	ULyraRecoilProfile* Profile = Editor.IsValid() ? Editor->GetProfile() : nullptr;

	if (Editor.IsValid() && Profile)
	{
		const TArray<FVector2D>& CumulativePoints = Editor->GetCumulativePoints();
		const int32 Num = Profile->PatternPoints.Num();
		const int32 PatternLength = FMath::Clamp(Profile->PatternLength, 0, Num);

		const double Horizontal = static_cast<double>(Profile->RecoilPerShot_Horizontal);
		const double Vertical = static_cast<double>(Profile->RecoilPerShot_Vertical);

		Rows.Reserve(Num);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			TSharedPtr<FLyraRecoilShotRow> Row = MakeShared<FLyraRecoilShotRow>();
			Row->Index = Index;
			Row->bFixed = Index < PatternLength;
			Row->bTailHorizontalInactive = Index >= PatternLength;
			Row->RawValue = Profile->PatternPoints[Index];

			const double VerticalScale = static_cast<double>(Profile->GetVerticalKickCurveScale(Index));

			if (Row->bFixed)
			{
				Row->DeltaYawDegrees = Horizontal * static_cast<double>(Row->RawValue.X);
				Row->DeltaPitchDegrees = Vertical * static_cast<double>(Row->RawValue.Y) * VerticalScale;

				// 累计列以 Adapter 的固定段累计点为准（与画布/预览完全同源）。
				if (CumulativePoints.IsValidIndex(Index))
				{
					Row->CumulativeYawDegrees = static_cast<double>(CumulativePoints[Index].X);
					Row->CumulativePitchDegrees = static_cast<double>(CumulativePoints[Index].Y);
					Row->bCumulativeEditable = true;
				}
				else
				{
					Row->CumulativeYawDegrees = Row->DeltaYawDegrees;
					Row->CumulativePitchDegrees = Row->DeltaPitchDegrees;
					Row->bCumulativeEditable = false;
				}
			}
			else
			{
				// 尾段：X 当前不参与水平计算（ΔYaw 记 0），Y 仍按曲线参与垂直计算。
				Row->DeltaYawDegrees = 0.0;
				Row->DeltaPitchDegrees = Vertical * static_cast<double>(Row->RawValue.Y) * VerticalScale;
				Row->bCumulativeEditable = false;
			}

			Row->Validation = GetRowValidationText(Row).ToString();
			Rows.Add(Row);
		}
	}

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}

	SyncListSelection();
}

FText SLyraRecoilShotTable::GetRowValidationText(TSharedPtr<FLyraRecoilShotRow> Row) const
{
	if (!Row.IsValid())
	{
		return FText::GetEmpty();
	}

	const FRecoilPatternPoint& Point = Row->RawValue;
	TArray<FString> Issues;

	if (!FMath::IsFinite(Point.X))
	{
		Issues.Add(TEXT("X 非有限数（NaN/Inf）"));
	}
	else if (Point.X < -1.0f || Point.X > 1.0f)
	{
		Issues.Add(FString::Printf(TEXT("X=%.6f 超出允许范围 [-1, 1]"), Point.X));
	}

	if (!FMath::IsFinite(Point.Y))
	{
		Issues.Add(TEXT("Y 非有限数（NaN/Inf）"));
	}
	else if (Point.Y < 0.0f || Point.Y > 1.0f)
	{
		Issues.Add(FString::Printf(TEXT("Y=%.6f 超出允许范围 [0, 1]"), Point.Y));
	}

	if (Row->bTailHorizontalInactive)
	{
		Issues.Add(TEXT("尾段：X 当前不参与水平计算（Y 仍生效）"));
	}

	if (Issues.Num() == 0)
	{
		return LOCTEXT("RowValid", "OK");
	}

	return FText::FromString(FString::Join(Issues, TEXT("；")));
}

FText SLyraRecoilShotTable::GetStatusText() const
{
	TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin();
	if (!Editor)
	{
		return LOCTEXT("StatusNoEditor", "（未绑定工具包）");
	}

	const TArray<FString>& Diagnostics = Editor->GetDiagnostics();
	if (Diagnostics.Num() == 0)
	{
		return LOCTEXT("StatusClean", "无诊断信息。编辑失败时会在这里给出 第几发 / 字段 / 候选值 / 允许范围 / 解决方式。");
	}

	return FText::FromString(FString::Join(Diagnostics, TEXT("\n")));
}

FReply SLyraRecoilShotTable::OnInsertClicked()
{
	if (TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin())
	{
		const TArray<int32>& Selected = Editor->GetSelectedIndices();
		const int32 Num = Editor->GetProfile() ? Editor->GetProfile()->PatternPoints.Num() : 0;
		const int32 InsertIndex = Selected.Num() > 0 ? Selected[0] : Num;

		TArray<FRecoilPatternPoint> NewPoints;
		NewPoints.Add(FRecoilPatternPoint(0.0f, 0.0f));
		Editor->InsertPoints(InsertIndex, NewPoints, /*bFixedAtBoundary=*/false);
	}

	return FReply::Handled();
}

FReply SLyraRecoilShotTable::OnDeleteClicked()
{
	if (TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin())
	{
		Editor->DeleteSelectedPoints();
	}

	return FReply::Handled();
}

FReply SLyraRecoilShotTable::OnMoveUpClicked()
{
	if (TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin())
	{
		Editor->MoveSelectedRows(-1);
	}

	return FReply::Handled();
}

FReply SLyraRecoilShotTable::OnMoveDownClicked()
{
	if (TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin())
	{
		Editor->MoveSelectedRows(+1);
	}

	return FReply::Handled();
}

FReply SLyraRecoilShotTable::OnSortByPitchClicked()
{
	if (TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin())
	{
		Editor->SortByCumulativePitch();
	}

	return FReply::Handled();
}

void SLyraRecoilShotTable::OnPatternLengthCommitted(int32 NewValue, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnCleared)
	{
		return;
	}

	if (TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin())
	{
		Editor->SetPatternLengthValue(NewValue);
	}
}

TOptional<int32> SLyraRecoilShotTable::GetPatternLengthValue() const
{
	if (TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin())
	{
		if (const ULyraRecoilProfile* Profile = Editor->GetProfile())
		{
			return TOptional<int32>(Profile->PatternLength);
		}
	}

	return TOptional<int32>();
}

// ---------------------------------------------------------------------------
// SLyraRecoilShotTableRow
// ---------------------------------------------------------------------------

void SLyraRecoilShotTableRow::Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTableView)
{
	RowData = InArgs._RowData;
	EditorWeak = InArgs._Editor;

	SMultiColumnTableRow<TSharedPtr<FLyraRecoilShotRow>>::Construct(FSuperRowType::FArguments(), OwnerTableView);
}

TSharedRef<SWidget> SLyraRecoilShotTableRow::GenerateWidgetForColumn(const FName& ColumnName)
{
	if (!RowData.IsValid())
	{
		return SNew(STextBlock).Text(FText::GetEmpty());
	}

	const TSharedPtr<FLyraRecoilShotRow> Row = RowData;

	if (ColumnName == LyraRecoilShotTableColumns::Index)
	{
		// UI 序号从 1 开始；曲线采样与代码 ShotIndex 从 0 开始（见工具提示与图例）。
		return SNew(STextBlock)
			.Text(FText::AsNumber(Row->Index + 1))
			.ToolTipText(LOCTEXT("IndexTooltip", "UI 序号从 1 开始；代码/曲线 ShotIndex 从 0 开始"));
	}

	if (ColumnName == LyraRecoilShotTableColumns::Segment)
	{
		return SNew(STextBlock)
			.Text(Row->bFixed ? LOCTEXT("SegmentFixed", "固定") : LOCTEXT("SegmentTail", "尾段"));
	}

	if (ColumnName == LyraRecoilShotTableColumns::RawX)
	{
		return SNew(SNumericEntryBox<float>)
			.Value(TAttribute<TOptional<float>>::CreateLambda([Row]()
			{
				return TOptional<float>(Row->RawValue.X);
			}))
			.MinValue(TOptional<float>(-1.0f))
			.MaxValue(TOptional<float>(1.0f))
			.AllowSpin(false)
			.OnValueCommitted(SNumericEntryBox<float>::FOnValueCommitted::CreateLambda(
				[this](float NewValue, ETextCommit::Type CommitType) { OnRawXCommitted(NewValue, CommitType); }));
	}

	if (ColumnName == LyraRecoilShotTableColumns::RawY)
	{
		return SNew(SNumericEntryBox<float>)
			.Value(TAttribute<TOptional<float>>::CreateLambda([Row]()
			{
				return TOptional<float>(Row->RawValue.Y);
			}))
			.MinValue(TOptional<float>(0.0f))
			.MaxValue(TOptional<float>(1.0f))
			.AllowSpin(false)
			.OnValueCommitted(SNumericEntryBox<float>::FOnValueCommitted::CreateLambda(
				[this](float NewValue, ETextCommit::Type CommitType) { OnRawYCommitted(NewValue, CommitType); }));
	}

	if (ColumnName == LyraRecoilShotTableColumns::DeltaYaw)
	{
		return SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%.4f"), Row->DeltaYawDegrees)));
	}

	if (ColumnName == LyraRecoilShotTableColumns::DeltaPitch)
	{
		return SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%.4f"), Row->DeltaPitchDegrees)));
	}

	if (ColumnName == LyraRecoilShotTableColumns::CumulativeYaw)
	{
		if (!Row->bCumulativeEditable)
		{
			return SNew(STextBlock).Text(LOCTEXT("NotEditableDash", "—"));
		}

		return SNew(SNumericEntryBox<double>)
			.Value(TAttribute<TOptional<double>>::CreateLambda([Row]()
			{
				return TOptional<double>(Row->CumulativeYawDegrees);
			}))
			.AllowSpin(false)
			.OnValueCommitted(SNumericEntryBox<double>::FOnValueCommitted::CreateLambda(
				[this](double NewValue, ETextCommit::Type CommitType) { OnCumulativeYawCommitted(NewValue, CommitType); }));
	}

	if (ColumnName == LyraRecoilShotTableColumns::CumulativePitch)
	{
		if (!Row->bCumulativeEditable)
		{
			return SNew(STextBlock).Text(LOCTEXT("NotEditableDash2", "—"));
		}

		return SNew(SNumericEntryBox<double>)
			.Value(TAttribute<TOptional<double>>::CreateLambda([Row]()
			{
				return TOptional<double>(Row->CumulativePitchDegrees);
			}))
			.AllowSpin(false)
			.OnValueCommitted(SNumericEntryBox<double>::FOnValueCommitted::CreateLambda(
				[this](double NewValue, ETextCommit::Type CommitType) { OnCumulativePitchCommitted(NewValue, CommitType); }));
	}

	if (ColumnName == LyraRecoilShotTableColumns::TailXState)
	{
		return SNew(STextBlock)
			.Text(Row->bTailHorizontalInactive
				? LOCTEXT("TailXInactive", "未生效（可显式修改原值 X）")
				: LOCTEXT("TailXActive", "生效"))
			.ColorAndOpacity(Row->bTailHorizontalInactive
				? FSlateColor(FLinearColor(0.70f, 0.70f, 0.70f))
				: FSlateColor(FLinearColor(0.55f, 0.85f, 0.55f)));
	}

	if (ColumnName == LyraRecoilShotTableColumns::Validation)
	{
		return SNew(STextBlock)
			.AutoWrapText(true)
			.Text(FText::FromString(Row->Validation));
	}

	return SNew(STextBlock).Text(FText::GetEmpty());
}

void SLyraRecoilShotTableRow::OnRawXCommitted(float NewValue, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnCleared || !RowData.IsValid())
	{
		return;
	}

	if (TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin())
	{
		Editor->SetPointNormalized(RowData->Index, NewValue, RowData->RawValue.Y);
	}
}

void SLyraRecoilShotTableRow::OnRawYCommitted(float NewValue, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnCleared || !RowData.IsValid())
	{
		return;
	}

	if (TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin())
	{
		Editor->SetPointNormalized(RowData->Index, RowData->RawValue.X, NewValue);
	}
}

void SLyraRecoilShotTableRow::OnCumulativeYawCommitted(double NewValue, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnCleared || !RowData.IsValid())
	{
		return;
	}

	if (TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin())
	{
		Editor->SetCumulativeValue(RowData->Index, /*bPitchAxis=*/false, NewValue);
	}
}

void SLyraRecoilShotTableRow::OnCumulativePitchCommitted(double NewValue, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnCleared || !RowData.IsValid())
	{
		return;
	}

	if (TSharedPtr<FLyraRecoilProfileEditor> Editor = EditorWeak.Pin())
	{
		Editor->SetCumulativeValue(RowData->Index, /*bPitchAxis=*/true, NewValue);
	}
}

#undef LOCTEXT_NAMESPACE
