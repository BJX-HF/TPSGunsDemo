// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Styling/AppStyle.h"
#include "Templates/SharedPointer.h"

/**
 * P2 工具包命令集。
 *
 * 作用域约束（计划 §04）：
 *  - 命令只映射到本工具包自己的 FUICommandList（FLyraRecoilProfileEditor::MapToolkitCommands），
 *    不注册到全局菜单，不覆盖引擎全局快捷键；
 *  - 文本输入框获得焦点时 Slate 会优先处理文本输入，命令不会抢键；
 *  - 画布（P3）与预览（P4）后续复用同一命令集，避免重复注册。
 */
class FLyraRecoilEditorCommands : public TCommands<FLyraRecoilEditorCommands>
{
public:
	FLyraRecoilEditorCommands();

	//~TCommands interface
	virtual void RegisterCommands() override;

	/** Ctrl+S：校验后只保存目标资产包。 */
	TSharedPtr<FUICommandInfo> SaveProfile;

	TSharedPtr<FUICommandInfo> SelectAll;
	TSharedPtr<FUICommandInfo> CopySelection;
	TSharedPtr<FUICommandInfo> PasteNormalized;
	TSharedPtr<FUICommandInfo> PasteByAngle;

	TSharedPtr<FUICommandInfo> InsertPoint;
	TSharedPtr<FUICommandInfo> DeleteSelected;
	TSharedPtr<FUICommandInfo> MoveRowUp;
	TSharedPtr<FUICommandInfo> MoveRowDown;
	TSharedPtr<FUICommandInfo> SortByCumulativePitch;

	TSharedPtr<FUICommandInfo> ValidateProfile;
};
