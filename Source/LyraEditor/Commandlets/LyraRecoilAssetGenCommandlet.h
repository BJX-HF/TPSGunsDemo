// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commandlets/Commandlet.h"

#include "LyraRecoilAssetGenCommandlet.generated.h"

/**
 * ULyraRecoilAssetGenCommandlet
 *
 * 生成 P1 交付物中的三份后坐力配置资产：
 *   /Game/Weapons/Recoil/DA_Recoil_Rifle
 *   /Game/Weapons/Recoil/DA_Recoil_Pistol
 *   /Game/Weapons/Recoil/DA_Recoil_Shotgun
 *
 * 用法（单行）：
 *   "D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject" -run=LyraRecoilAssetGen -unattended -nopause -nullrhi -nosplash -log
 *
 * 开关：
 *   -force     已存在的资产也重新生成（覆盖本地未提交的改动，慎用）
 *
 * 幂等：不加 -force 时，已存在的资产直接跳过，不会触碰。
 */
UCLASS()
class ULyraRecoilAssetGenCommandlet : public UCommandlet
{
	GENERATED_UCLASS_BODY()

public:
	// Begin UCommandlet Interface
	virtual int32 Main(const FString& Params) override;
	// End UCommandlet Interface
};
