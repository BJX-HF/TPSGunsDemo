// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "LyraRecoilEditorSession.generated.h"

class ULyraRecoilProfile;

/**
 * P2 编辑器会话对象。
 *
 * 职责（开发计划 §09）：
 *  - 持有正在编辑的 ULyraRecoilProfile 强引用（配合工具包的 FGCObject 保护）。
 *  - 保存节点稳定身份（FGuid）与选择集，索引只表示"当前发序号"。
 *  - 记录资产修订号（Revision）：手势开始时快照，提交前比对，冲突即拒绝草稿。
 *  - 保存拖动草稿的起始修订与基准累计点（临时数据，绝不写入资产）。
 *
 * 约束：
 *  - 本对象是 Transient + RF_Transactional，绝不序列化进运行时包。
 *  - 累计坐标、校验结果、视图状态都是可重建缓存，可在刷新时整体重建。
 *  - 不提供写 Profile 的路径：所有写操作必须走 FLyraRecoilEditOperations。
 */
UCLASS(Transient)
class ULyraRecoilEditorSession : public UObject
{
	GENERATED_BODY()

public:
	ULyraRecoilEditorSession();

	/** 绑定资产并重建节点身份。 */
	void Initialize(ULyraRecoilProfile* InProfile);

	/** 切换资产（同一工具包只服务一个 Profile）。 */
	void SetProfile(ULyraRecoilProfile* InProfile);

	ULyraRecoilProfile* GetProfile() const { return Profile; }

	// ---------------------------------------------------------------------
	// 节点身份（稳定 FGuid）
	// ---------------------------------------------------------------------

	int32 GetNumNodes() const { return NodeIds.Num(); }
	const TArray<FGuid>& GetNodeIds() const { return NodeIds; }

	FGuid GetNodeId(int32 Index) const;
	bool FindNodeIndex(const FGuid& NodeId, int32& OutIndex) const;

	/** 全部重建：新点生成新 ID。外部结构无法证明等价时使用。 */
	void RebuildNodeIds();

	/**
	 * 外部刷新：可证明数组结构未变（长度一致）时保留索引映射。
	 * @return true 表示保留了映射；false 表示已重建 ID 且选择被清空。
	 */
	bool SyncNodeIdsPreservingMapping();

	/**
	 * 在 Index 处插入节点 ID。
	 *  - 新增/复制：调用方传 FGuid()，本函数生成新 GUID；
	 *  - 结构命令内部：调用方传已分配好的 GUID，保持身份可预测。
	 */
	void InsertNodeIds(int32 Index, const TArray<FGuid>& InGuids);

	/** 按升序索引删除节点 ID。 */
	void RemoveNodeIds(TArray<int32> SortedIndicesAscending);

	/** 按 Order（Order[NewIndex] = OldIndex）重排节点 ID。 */
	void ReorderNodeIds(const TArray<int32>& Order);

	/** 直接把 ID 设置为给定值（Undo/Redo 恢复映射用）。 */
	void SetNodeIds(const TArray<FGuid>& InNodeIds);

	// ---------------------------------------------------------------------
	// 选择
	// ---------------------------------------------------------------------

	const TArray<int32>& GetSelectedIndices() const { return SelectedIndices; }
	void SelectIndices(const TArray<int32>& InIndices, bool bAppend);
	void ClearSelection();

	/** 以稳定 ID 恢复选择（Undo/重排后身份不变）。 */
	void SetSelectionByNodeIds(const TArray<FGuid>& InNodeIds);
	TArray<FGuid> GetSelectedNodeIds() const;

	bool IsSelected(int32 Index) const { return SelectedIndices.Contains(Index); }

	// ---------------------------------------------------------------------
	// 修订号
	// ---------------------------------------------------------------------

	uint64 GetRevision() const { return Revision; }

	/** 任何有效属性/曲线依赖变化（含外部与自提交）都递增，使旧手势失效。 */
	void BumpRevision();

	// ---------------------------------------------------------------------
	// 拖动草稿（临时，不写资产）
	// ---------------------------------------------------------------------

	bool HasDragDraft() const { return bHasDragDraft; }
	void BeginDragDraft(const TArray<FVector2D>& InBaseCumulativePoints);
	const TArray<FVector2D>& GetDragDraftBasePoints() const { return DragDraftBasePoints; }
	uint64 GetDragStartRevision() const { return DragStartRevision; }
	void CancelDragDraft();

	// ---------------------------------------------------------------------
	// 诊断
	// ---------------------------------------------------------------------

	const TArray<FString>& GetDiagnostics() const { return Diagnostics; }
	void SetDiagnostics(const TArray<FString>& InDiagnostics);
	void ClearDiagnostics();
	void AddDiagnostic(const FString& Message);

private:
	/** 当前编辑的资产。强引用；工具包 FGCObject 同时引用本对象与 Profile。 */
	UPROPERTY(Transient)
	TObjectPtr<ULyraRecoilProfile> Profile;

	/** 与 PatternPoints 一一对应的稳定身份。 */
	UPROPERTY(Transient)
	TArray<FGuid> NodeIds;

	/** 当前选择（升序、去重）。 */
	UPROPERTY(Transient)
	TArray<int32> SelectedIndices;

	/** 资产/曲线修订号。非 UPROPERTY：不使用事务恢复，冲突检测只看单调递增。 */
	uint64 Revision = 0;

	/** 拖动草稿状态。 */
	bool bHasDragDraft = false;
	uint64 DragStartRevision = 0;
	TArray<FVector2D> DragDraftBasePoints;

	/** 最近一次校验/操作诊断（只读展示）。 */
	TArray<FString> Diagnostics;
};
