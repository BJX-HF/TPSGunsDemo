// Copyright Epic Games, Inc. All Rights Reserved.

#include "Recoil/LyraRecoilEditorCommands.h"

#include "GenericPlatform/GenericApplication.h"
#include "InputCoreTypes.h"

#define LOCTEXT_NAMESPACE "LyraRecoilEditorCommands"

FLyraRecoilEditorCommands::FLyraRecoilEditorCommands()
	: TCommands<FLyraRecoilEditorCommands>(
		TEXT("LyraRecoilProfileEditor"),
		NSLOCTEXT("Contexts", "LyraRecoilProfileEditor", "Lyra Recoil Profile Editor"),
		NAME_None,
		FAppStyle::GetAppStyleSetName())
{
}

void FLyraRecoilEditorCommands::RegisterCommands()
{
	UI_COMMAND(SaveProfile,
		"保存 Profile",
		"校验当前资产后只保存目标 Profile 包（Ctrl+S，不替代引擎的全局保存）",
		EUserInterfaceActionType::Button,
		FInputChord(EModifierKey::Control, EKeys::S));

	UI_COMMAND(SelectAll,
		"全选节点",
		"选择当前逐发表格中的全部节点",
		EUserInterfaceActionType::Button,
		FInputChord(EModifierKey::Control, EKeys::A));

	UI_COMMAND(CopySelection,
		"复制节点",
		"以 LyraRecoilClipboard/v1 结构化载荷复制原始归一化逐发参数",
		EUserInterfaceActionType::Button,
		FInputChord(EModifierKey::Control, EKeys::C));

	UI_COMMAND(PasteNormalized,
		"粘贴归一化参数",
		"按当前插入位置粘贴原始归一化逐发参数（默认语义）",
		EUserInterfaceActionType::Button,
		FInputChord(EModifierKey::Control, EKeys::V));

	UI_COMMAND(PasteByAngle,
		"按角度粘贴",
		"用目标资产强度与曲线反算载荷中的源逐发角度后粘贴",
		EUserInterfaceActionType::Button,
		FInputChord(static_cast<EModifierKey::Type>(EModifierKey::Control | EModifierKey::Shift), EKeys::V));

	UI_COMMAND(InsertPoint,
		"插入节点",
		"在选中发之前插入一个默认 (0,0) 节点（固定段边界规则见计划 §07）",
		EUserInterfaceActionType::Button,
		FInputChord(EKeys::Insert));

	UI_COMMAND(DeleteSelected,
		"删除选中节点",
		"一次性删除选中发并按操作前索引重算 L/N",
		EUserInterfaceActionType::Button,
		FInputChord(EKeys::Delete));

	UI_COMMAND(MoveRowUp,
		"上移选中发",
		"对选中发执行显式重排（与上一发交换）",
		EUserInterfaceActionType::Button,
		FInputChord(EModifierKey::Alt, EKeys::Up));

	UI_COMMAND(MoveRowDown,
		"下移选中发",
		"对选中发执行显式重排（与下一发交换）",
		EUserInterfaceActionType::Button,
		FInputChord(EModifierKey::Alt, EKeys::Down));

	UI_COMMAND(SortByCumulativePitch,
		"按累计 Pitch 排序",
		"一次性稳定排序：执行前必须先展示顺序、L/N 与曲线索引变化",
		EUserInterfaceActionType::Button,
		FInputChord());

	UI_COMMAND(ValidateProfile,
		"校验 Profile",
		"运行 ValidateProfile 与编辑器附加的有限数检查，并把结果显示在诊断面板",
		EUserInterfaceActionType::Button,
		FInputChord());
}

#undef LOCTEXT_NAMESPACE
