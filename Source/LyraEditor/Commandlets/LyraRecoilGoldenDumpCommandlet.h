// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commandlets/Commandlet.h"

#include "LyraRecoilGoldenDumpCommandlet.generated.h"

/**
 * ULyraRecoilGoldenDumpCommandlet
 *
 * P3 交付物：导出后坐力弹道 Golden 基准数据。
 *
 * 对 /Game/Weapons/Recoil/ 下的三份 ULyraRecoilProfile，按固定条件
 * （PoseMultiplier = 1、GlobalScale = 1、Seed 取自资产）连算 20 发，
 * 把每发的弹道方向偏移写进：
 *   Source/LyraGame/Tests/Data/RecoilGolden_<资产名>.json
 *
 * 用途：作为回归锁 —— Lyra.Recoil.Pattern.Golden 测试逐发比对运行时的
 * ComputeShotKick 输出与这份文件。**改了资产参数就必须重新导出**，
 * 否则该测试会明确报出 "golden is stale"。
 *
 * 用法（单行）：
 *   "D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject" -run=LyraRecoilGoldenDump -unattended -nopause -nullrhi -nosplash -log
 */
UCLASS()
class ULyraRecoilGoldenDumpCommandlet : public UCommandlet
{
	GENERATED_UCLASS_BODY()

public:
	// Begin UCommandlet Interface
	virtual int32 Main(const FString& Params) override;
	// End UCommandlet Interface
};
