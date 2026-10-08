// Copyright Epic Games, Inc. All Rights Reserved.

#include "Recoil/LyraRecoilEditorSession.h"

#include "Weapons/Recoil/LyraRecoilProfile.h"

ULyraRecoilEditorSession::ULyraRecoilEditorSession()
{
	// 会话参与事务（节点身份/选择随 Undo/Redo 还原），但绝不序列化进资产包。
	SetFlags(RF_Transient | RF_Transactional);
}

void ULyraRecoilEditorSession::Initialize(ULyraRecoilProfile* InProfile)
{
	SetProfile(InProfile);
}

void ULyraRecoilEditorSession::SetProfile(ULyraRecoilProfile* InProfile)
{
	Profile = InProfile;
	CancelDragDraft();
	ClearDiagnostics();
	SelectedIndices.Reset();
	RebuildNodeIds();
	BumpRevision();
}

FGuid ULyraRecoilEditorSession::GetNodeId(int32 Index) const
{
	return NodeIds.IsValidIndex(Index) ? NodeIds[Index] : FGuid();
}

bool ULyraRecoilEditorSession::FindNodeIndex(const FGuid& NodeId, int32& OutIndex) const
{
	if (!NodeId.IsValid())
	{
		return false;
	}

	for (int32 Index = 0; Index < NodeIds.Num(); ++Index)
	{
		if (NodeIds[Index] == NodeId)
		{
			OutIndex = Index;
			return true;
		}
	}

	return false;
}

void ULyraRecoilEditorSession::RebuildNodeIds()
{
	const int32 Num = Profile ? Profile->PatternPoints.Num() : 0;

	NodeIds.Reset(Num);
	for (int32 Index = 0; Index < Num; ++Index)
	{
		NodeIds.Add(FGuid::NewGuid());
	}

	SelectedIndices.Reset();
}

bool ULyraRecoilEditorSession::SyncNodeIdsPreservingMapping()
{
	const int32 Num = Profile ? Profile->PatternPoints.Num() : 0;

	if (NodeIds.Num() == Num)
	{
		// Caller already routed known commands / undo or invalidated unknown external
		// structure changes. This is only a length sanity check, not proof of identity.
		for (int32 Index = 0; Index < SelectedIndices.Num(); ++Index)
		{
			if (!NodeIds.IsValidIndex(SelectedIndices[Index]))
			{
				SelectedIndices.RemoveAt(Index);
				--Index;
			}
		}
		return true;
	}

	// 结构无法证明等价：重建 ID、清空选择，不凭相同坐标猜身份。
	RebuildNodeIds();
	return false;
}

void ULyraRecoilEditorSession::InsertNodeIds(int32 Index, const TArray<FGuid>& InGuids)
{
	const int32 Num = Profile ? Profile->PatternPoints.Num() : 0;
	Index = FMath::Clamp(Index, 0, NodeIds.Num());

	// 以资产数组长度为准补齐（正常情况下一致）。
	while (NodeIds.Num() < Num && NodeIds.Num() <= Index)
	{
		NodeIds.Add(FGuid::NewGuid());
	}

	TArray<FGuid> NewGuids = InGuids;
	for (FGuid& Guid : NewGuids)
	{
		if (!Guid.IsValid())
		{
			Guid = FGuid::NewGuid();
		}
	}

	NodeIds.Insert(NewGuids, Index);

	// 长度兜底：把数组收拢到资产实际长度。
	while (NodeIds.Num() > Num)
	{
		NodeIds.RemoveAt(NodeIds.Num() - 1);
	}
	while (NodeIds.Num() < Num)
	{
		NodeIds.Add(FGuid::NewGuid());
	}

	// 索引移动后同步选择集。
	TArray<int32> NewSelection;
	for (int32 Selected : SelectedIndices)
	{
		NewSelection.Add(Selected >= Index ? Selected + NewGuids.Num() : Selected);
	}
	SelectedIndices = MoveTemp(NewSelection);
}

void ULyraRecoilEditorSession::RemoveNodeIds(TArray<int32> SortedIndicesAscending)
{
	SortedIndicesAscending.Sort();

	for (int32 Index = SortedIndicesAscending.Num() - 1; Index >= 0; --Index)
	{
		const int32 RemoveIndex = SortedIndicesAscending[Index];
		if (NodeIds.IsValidIndex(RemoveIndex))
		{
			NodeIds.RemoveAt(RemoveIndex);
		}
	}

	// 重建选择：被删掉的发移除，其余索引按删除数量前移。
	TArray<int32> NewSelection;
	for (int32 Selected : SelectedIndices)
	{
		if (SortedIndicesAscending.Contains(Selected))
		{
			continue;
		}

		int32 Shift = 0;
		for (int32 Removed : SortedIndicesAscending)
		{
			if (Removed < Selected)
			{
				++Shift;
			}
		}
		NewSelection.Add(Selected - Shift);
	}
	NewSelection.Sort();
	SelectedIndices = MoveTemp(NewSelection);
}

void ULyraRecoilEditorSession::ReorderNodeIds(const TArray<int32>& Order)
{
	if (Order.Num() != NodeIds.Num())
	{
		return;
	}

	TArray<FGuid> Reordered;
	Reordered.Reserve(Order.Num());
	for (int32 OldIndex : Order)
	{
		if (!NodeIds.IsValidIndex(OldIndex))
		{
			return;
		}
		Reordered.Add(NodeIds[OldIndex]);
	}

	TArray<FGuid> SelectedIds = GetSelectedNodeIds();
	NodeIds = MoveTemp(Reordered);

	// Capture the old identities before replacing the index mapping.
	SelectedIndices.Reset();
	for (const FGuid& SelectedId : SelectedIds)
	{
		int32 NewIndex = INDEX_NONE;
		if (FindNodeIndex(SelectedId, NewIndex))
		{
			SelectedIndices.Add(NewIndex);
		}
	}
	SelectedIndices.Sort();
}

void ULyraRecoilEditorSession::SetNodeIds(const TArray<FGuid>& InNodeIds)
{
	NodeIds = InNodeIds;
}

void ULyraRecoilEditorSession::SelectIndices(const TArray<int32>& InIndices, bool bAppend)
{
	if (!bAppend)
	{
		SelectedIndices.Reset();
	}

	const int32 NumNodes = NodeIds.Num();
	for (int32 Index : InIndices)
	{
		if (Index >= 0 && Index < NumNodes && !SelectedIndices.Contains(Index))
		{
			SelectedIndices.Add(Index);
		}
	}

	SelectedIndices.Sort();
}

void ULyraRecoilEditorSession::ClearSelection()
{
	SelectedIndices.Reset();
}

void ULyraRecoilEditorSession::SetSelectionByNodeIds(const TArray<FGuid>& InNodeIds)
{
	SelectedIndices.Reset();
	for (const FGuid& NodeId : InNodeIds)
	{
		int32 Index = INDEX_NONE;
		if (FindNodeIndex(NodeId, Index))
		{
			SelectedIndices.Add(Index);
		}
	}
	SelectedIndices.Sort();
}

TArray<FGuid> ULyraRecoilEditorSession::GetSelectedNodeIds() const
{
	TArray<FGuid> Result;
	Result.Reserve(SelectedIndices.Num());
	for (int32 Index : SelectedIndices)
	{
		if (NodeIds.IsValidIndex(Index))
		{
			Result.Add(NodeIds[Index]);
		}
	}
	return Result;
}

void ULyraRecoilEditorSession::BumpRevision()
{
	++Revision;
}

void ULyraRecoilEditorSession::BeginDragDraft(const TArray<FVector2D>& InBaseCumulativePoints)
{
	bHasDragDraft = true;
	DragStartRevision = Revision;
	DragDraftBasePoints = InBaseCumulativePoints;
}

void ULyraRecoilEditorSession::CancelDragDraft()
{
	bHasDragDraft = false;
	DragStartRevision = 0;
	DragDraftBasePoints.Reset();
}

void ULyraRecoilEditorSession::SetDiagnostics(const TArray<FString>& InDiagnostics)
{
	Diagnostics = InDiagnostics;
}

void ULyraRecoilEditorSession::ClearDiagnostics()
{
	Diagnostics.Reset();
}

void ULyraRecoilEditorSession::AddDiagnostic(const FString& Message)
{
	Diagnostics.Add(Message);
}
