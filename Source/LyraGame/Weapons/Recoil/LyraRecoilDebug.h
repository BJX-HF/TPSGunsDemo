// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"

#include "LyraRecoilDebug.generated.h"

#define UE_API LYRAGAME_API

class ULyraRangedWeaponInstance;
class ULyraRecoilProfile;
class UWorld;
struct FRecoilRuntimeState;

/**
 * ULyraRecoilDebug
 *
 * 后坐力调试工具层（开发计划 §3.2 / §P5）。三类职责：
 *   1. CVar 注册与取值    —— Lyra.Recoil.Enable / Scale / Debug / DebugDraw
 *   2. 可视化            —— 屏幕数值面板（P2）+ 世界内 DebugDraw（P5）
 *   3. 数据导出与热重载  —— CSV Dump / ReloadProfile（P5）
 *
 * 所有 CVar 都用 FAutoConsoleVariableRef 注册，而不是 ULyraCheatManager 的 exec 函数 ——
 * 前者不依赖 PlayerController 上下文，可以在 UnrealEditor-Cmd 下靠 -ExecCmds 驱动，
 * 这样自动化测试能直接调 CVar（范式参考 LyraGameplayAbility_RangedWeapon.cpp L16-L38）。
 *
 * 设计约束：**本类不进 FRecoilRuntimeState**。算法层必须保持无 UWorld 依赖，
 * 所以"找当前玩家武器""画线""写文件"这些需要世界/IO 的事全部落在这一层。
 *
 * CVar 清单：
 *   Lyra.Recoil.Enable        0/1   总开关（关闭时弹道偏移恒为 0、相机不动）
 *   Lyra.Recoil.Scale         <f>   全局调试倍率（不改资产）
 *   Lyra.Recoil.Debug         0/1   屏幕数值面板
 *   Lyra.Recoil.DebugDraw     0/1   世界内可视化
 *   Lyra.Recoil.RollShake     <f>   Roll 震动实时振幅倍率（0 = 关掉 Roll）
 *   Lyra.Recoil.RollDebug     0/1   Roll 震动的实时曲线/参数面板（独立于 Debug）
 *   Lyra.Recoil.Dump                导出本轮连发到 Saved/RecoilDump_<timestamp>.csv
 *   Lyra.Recoil.ReloadProfile       从磁盘强制重载当前武器的资产
 */
UCLASS()
class UE_API ULyraRecoilDebug : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	// ---------------------------------------------------------------------
	// CVar 取值
	// ---------------------------------------------------------------------

	/** Lyra.Recoil.Enable：总开关。关闭时弹道偏移恒为 0、相机偏移不施加。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Debug")
	static bool IsRecoilEnabled();

	/** Lyra.Recoil.Scale：全局调试倍率，叠加在资产数值之上，不修改资产。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Debug")
	static float GetGlobalScale();

	/** Lyra.Recoil.Debug：是否显示屏幕数值面板。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Debug")
	static bool IsDebugPanelEnabled();

	/** Lyra.Recoil.DebugDraw：是否开启世界内可视化。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Debug")
	static bool IsDebugDrawEnabled();

	// ---------------------------------------------------------------------
	// CVar 取值（Roll 震动专用通道）
	//
	// Roll 与 Pitch/Yaw 是两套独立机制，因此调试入口也独立：
	// 关掉 Roll 不影响后坐力，反之亦然。这样能把"手感发飘"归因到具体是哪一路。
	// ---------------------------------------------------------------------

	/**
	 * Lyra.Recoil.RollShake：Roll 震动的实时振幅倍率。
	 *
	 * 这是**调参期最常用的旋钮** —— 0 直接关掉 Roll（用于 A/B 对比"有没有 Roll 的差别"），
	 * 大于 1 放大以便在远处/低分辨率下也看得清抖动。
	 * 它只影响相机显示层，不修改资产、不影响弹道。
	 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Debug")
	static float GetRollShakeScale();

	/** Lyra.Recoil.RollDebug：是否显示 Roll 震动的实时曲线与参数面板。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Debug")
	static bool IsRollDebugPanelEnabled();

	// ---------------------------------------------------------------------
	// 可视化
	// ---------------------------------------------------------------------

	/**
	 * P2：屏幕左上角数值面板。
	 * 显示 ShotIndex / 本发 Kick / 状态机状态 / 回正计时 / 姿态与全局倍率。
	 * 用固定 key 覆盖显示，不刷屏。
	 */
	static void DrawDebugPanel(const UWorld* World, const ULyraRecoilProfile* Profile, const FRecoilRuntimeState& State);

	/**
	 * Roll 震动实时调试面板（独立于 DrawDebugPanel，由 Lyra.Recoil.RollDebug 控制）。
	 *
	 * 显示内容分三段：
	 *   1) 实时值：当前 Roll 角度 / 衰减包络 / 剩余时间占比 / 相位
	 *   2) 本次生效参数：振幅、周期、时长、终值比例、发序号
	 *   3) 时间轴曲线：把整段震动从 t=0 到 t=Duration 采样成 ASCII 折线，
	 *      并把当前时刻用 '|' 标出来 —— 这是"不用画世界线也能看出波形对不对"的手段。
	 */
	static void DrawRollShakeDebugPanel(const UWorld* World, const ULyraRecoilProfile* Profile, const FRecoilRuntimeState& State);

	/**
	 * P5：世界内可视化。
	 *   绿线 = 真实瞄准轴（ControlRotation，永远不受后坐力影响）
	 *   红线 = 相机链当前偏移后的方向，也就是玩家画面上看到的那条
	 *   黄点 = Pattern 点阵（前 PatternLength 发的弹道方向）
	 *   青条 = 回正进度
	 *   品红线 = Roll 震动当前的横滚角示意（以相机所在位置为圆心画弧）
	 * @param Origin      画线起点（建议用眼睛高度附近，与 GA 的发射源同一量级）
	 * @param AimRotation 真实瞄准朝向
	 */
	static void DrawWorldDebug(const UWorld* World, const FVector& Origin, const FRotator& AimRotation, const ULyraRecoilProfile* Profile, const FRecoilRuntimeState& State);

	// ---------------------------------------------------------------------
	// 数据导出
	// ---------------------------------------------------------------------

	/**
	 * P5：把 ShotHistory 导出成 CSV。
	 *
	 * 列顺序（**契约，改动需同步 Golden 数据与所有解析脚本**）：
	 *   ShotIndex,VerticalKick,HorizontalKick,AccumulatedPitch,AccumulatedYaw,TimeSinceFire,RollShake
	 * 前 6 列来自 FRecoilShotResult（零映射代码），末尾的 RollShake 为**开火瞬间**的
	 * Roll 震动值（度）—— 注意它是解析解在 t=0 的采样，不是"本发累计"，语义上与 AccumulatedPitch 不同。
	 *
	 * 落盘路径：Saved/RecoilDump_<YYYYMMDD_HHMMSS_fff>.csv
	 *
	 * 纯 IO，不需要 UWorld —— 因此可以被自动化测试直接调用并回读校验。
	 *
	 * @param Profile     仅用于日志（可为 nullptr）
	 * @param State       数据源
	 * @param OutFilePath 成功时写出实际落盘路径
	 * @return 成功写出返回 true；ShotHistory 为空或写文件失败返回 false
	 */
	static bool DumpShotHistoryToCsv(const ULyraRecoilProfile* Profile, const FRecoilRuntimeState& State, FString& OutFilePath);

	/** 取最近一次成功导出的路径（空字符串表示本次会话还没导出过）。 */
	static FString GetLastDumpPath();

	// ---------------------------------------------------------------------
	// 世界查询
	// ---------------------------------------------------------------------

	/**
	 * P5：定位当前本地玩家的武器实例。
	 * Dump / ReloadProfile / DebugDraw 共用的**唯一查找点**：
	 *   World -> FirstPlayerController -> Pawn -> ULyraEquipmentManagerComponent
	 *         -> 第一个 ULyraRangedWeaponInstance
	 * 找不到返回 nullptr（DedicatedServer、菜单场景、没持枪都会走到这里）。
	 */
	static ULyraRangedWeaponInstance* FindLocalPlayerRangedWeapon(UWorld* World);
};

#undef UE_API
