// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STableRow.h"

class FLyraRecoilProfileEditor;
template <typename ItemType>
class SListView;

/**
 * 逐发表格的一行数据（可重建缓存，不是持久数据）。
 *
 * 列语义（计划 §04 左栏）：
 *  - 原值：PatternPoints[i].X / .Y（归一化，直接写入）；
 *  - 逐发：ΔYaw[i] = H × X，ΔPitch[i] = V × Y × c[i]（只读展示）；
 *  - 累计：P[i] = P[i-1] + Δ[i]（固定段可精确编辑，走 MoveCumulative）；
 *  - 尾段：i ≥ PatternLength 的 Y 仍参与垂直计算（可编辑），X 当前不参与水平计算
 *    （"未生效"高级列，可显式修改但不会影响当前水平结果）。
 */
struct FLyraRecoilShotRow
{
	/** 当前发序号（0 起，数组索引）。 */
	int32 Index = 0;

	/** 是否落在固定段（i < PatternLength）。 */
	bool bFixed = false;

	/** 原始归一化参数（原值列）。 */
	FRecoilPatternPoint RawValue;

	/** 逐发角度（度）。尾段的 ΔYaw 当前不参与水平计算，故为 0 并标记无效。 */
	double DeltaYawDegrees = 0.0;
	double DeltaPitchDegrees = 0.0;

	/** 累计角度（度），只对固定段有意义。 */
	double CumulativeYawDegrees = 0.0;
	double CumulativePitchDegrees = 0.0;

	/** 该发是否有可编辑的累计点（i < PatternLength 且累计点可用）。 */
	bool bCumulativeEditable = false;

	/** 尾段 X 未生效标记（i ≥ PatternLength）。 */
	bool bTailHorizontalInactive = false;

	/** 逐行校验状态（超界/非有限等）。 */
	FString Validation;
};

/**
 * 逐发表格：序号 / 段 / 归一化原值 / 逐发角度 / 累计角度 / 尾X状态 / 校验。
 *
 * 编辑路径统一走 FLyraRecoilProfileEditor（内部再走 FLyraRecoilEditOperations），
 * 因此不会绕过结构规则，也不会出现"普通 Details 数组按钮绕过校验"的情况。
 */
class SLyraRecoilShotTable : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SLyraRecoilShotTable) {}
		/** 所属工具包（弱引用：Slate 回调不得延长工具包生命周期）。 */
		SLATE_ARGUMENT(TWeakPtr<FLyraRecoilProfileEditor>, Editor)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** 资产/选择/诊断变化后重建行缓存。 */
	void RefreshRows();

private:
	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FLyraRecoilShotRow> Row, const TSharedRef<STableViewBase>& OwnerTable);
	void OnSelectionChanged(TSharedPtr<FLyraRecoilShotRow> Row, ESelectInfo::Type SelectInfo);

	FReply OnInsertClicked();
	FReply OnDeleteClicked();
	FReply OnMoveUpClicked();
	FReply OnMoveDownClicked();
	FReply OnSortByPitchClicked();

	void OnPatternLengthCommitted(int32 NewValue, ETextCommit::Type CommitType);
	TOptional<int32> GetPatternLengthValue() const;

	FText GetStatusText() const;
	FText GetRowValidationText(TSharedPtr<FLyraRecoilShotRow> Row) const;

	/** 把会话选择同步到 SListView（带防重入守卫）。 */
	void SyncListSelection();

	TWeakPtr<FLyraRecoilProfileEditor> EditorWeak;

	TArray<TSharedPtr<FLyraRecoilShotRow>> Rows;

	TSharedPtr<SListView<TSharedPtr<FLyraRecoilShotRow>>> ListView;

	/** 选择同步防重入：同步期间不重建行、不回调会话。 */
	bool bIsSyncingSelection = false;
};

/**
 * 行控件：按列生成单元格。编辑单元格直接调用工具包公开接口。
 */
class SLyraRecoilShotTableRow : public SMultiColumnTableRow<TSharedPtr<FLyraRecoilShotRow>>
{
public:
	SLATE_BEGIN_ARGS(SLyraRecoilShotTableRow) {}
		SLATE_ARGUMENT(TSharedPtr<FLyraRecoilShotRow>, RowData)
		SLATE_ARGUMENT(TWeakPtr<FLyraRecoilProfileEditor>, Editor)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTableView);

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override;

private:
	void OnRawXCommitted(float NewValue, ETextCommit::Type CommitType);
	void OnRawYCommitted(float NewValue, ETextCommit::Type CommitType);
	void OnCumulativeYawCommitted(double NewValue, ETextCommit::Type CommitType);
	void OnCumulativePitchCommitted(double NewValue, ETextCommit::Type CommitType);

	TSharedPtr<FLyraRecoilShotRow> RowData;
	TWeakPtr<FLyraRecoilProfileEditor> EditorWeak;
};
