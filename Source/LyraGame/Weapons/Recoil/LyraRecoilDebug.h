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
 *   Lyra.Recoil.SpreadDebug   0/1   散布（锥角）实时面板（独立于 Debug / RollDebug）
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
	// CVar 取值（散布专用通道）
	//
	// 散布与后坐力是两件事（Lyra.Recoil.Enable 关掉后坐力时散布照常工作），
	// 所以调试入口也独立：想把「散布有问题」和「后坐力有问题」分开排查，
	// 就必须能只开其中一个面板。
	// ---------------------------------------------------------------------

	/** Lyra.Recoil.SpreadDebug：是否显示散布（锥角）实时面板。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Debug")
	static bool IsSpreadDebugPanelEnabled();

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
	 * 散布（锥角）实时调试面板（独立于 DrawDebugPanel / DrawRollShakeDebugPanel，
	 * 由 Lyra.Recoil.SpreadDebug 控制）。
	 *
	 * 显示内容分四段：
	 *   1) 总开关：资产散布是否启用 / 曲线来源（资产 or Lyra 原生 heat）
	 *   2) 实时锥角：基础角 → 当前角 → 上限角，以及两条玩家倍率（瞄准 / 移动）
	 *   3) 姿态参数快照：本姿态的 Base / Max / AddPerShot / RecoverRate
	 *   4) 数值链：最终锥角（喂给变体锥的那个值）与它相对基础角的放大倍数
	 *
	 * 为什么单开一个面板：散布的问题是"准星为什么这么大 / 为什么连发到头了还不变大"，
	 * 与后坐力的"镜头为什么飞了"是两套排查路径，混在一屏反而看不清。
	 * 尤其是「面板显示的最终锥角」与「玩家看到的准星半径」必须能对上 —— 这一条
	 * 是散布调试的第一判据（见 Docs/Recoil/后坐力系统调试.html §11）。
	 *
	 * @param NativeHeat                  Lyra 原生 heat 链路：当前 heat（资产模型下被忽略）
	 * @param NativeSpreadAngle           Lyra 原生 heat 链路：当前基础锥角（度，全锥角）
	 * @param NativeSpreadAngleMultiplier Lyra 原生 heat 链路：玩家侧合并倍率
	 *
	 * 三个 Native* 参数带默认值，是为了让"只想看资产散布"的调用方少传三个 0；
	 * 武器实例会把 heat 链路的实时值传进来，于是同一个面板可以服务两条链路。
	 */
	static void DrawSpreadDebugPanel(
		const UWorld* World,
		const ULyraRecoilProfile* Profile,
		const FRecoilRuntimeState& State,
		float NativeHeat = 0.0f,
		float NativeSpreadAngle = 0.0f,
		float NativeSpreadAngleMultiplier = 1.0f);

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
	 *   ShotIndex,VerticalKick,HorizontalKick,AccumulatedPitch,AccumulatedYaw,TimeSinceFire,RollShake,SpreadAngle
	 *
	 * 前 6 列来自 FRecoilShotResult（零映射代码），第 7 列 RollShake 为**开火瞬间**的
	 * Roll 震动值（度）—— 注意它是解析解在 t=0 的采样，不是"本发累计"，语义上与 AccumulatedPitch 不同。
	 *
	 * 第 8 列 SpreadAngle（P10 追加）为**本发弹道实际使用的散布锥角**（度，全锥角，含姿态/瞄准/移动倍率）。
	 * 它同样不是"累计量"，并且是**真实存储**的（FRecoilShotResult::SpreadAngle）而不是回放的 ——
	 * 因为散布角依赖姿态/移动/瞄准三条链路，事后无法用 ShotIndex 还原。
	 * 未启用资产散布（ULyraRecoilProfile::bEnableProfileSpread == false）时该列恒为 0。
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
