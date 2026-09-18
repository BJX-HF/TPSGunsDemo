// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Camera/LyraCameraShakeTypes.h"

#include "LyraCameraRollShake.generated.h"

#define UE_API LYRAGAME_API

class ULyraRecoilProfile;

/**
 * ULyraCameraRollShake
 *
 * Roll 阻尼震动的**纯算法层**（对应《FPS 相机镜头设计与实现》§2.1）。
 *
 * 全部函数都是静态纯函数：只依赖传入的参数与状态，不持有 UObject、
 * 不依赖 UWorld、不读 CVar。因此可以被纯数值单测完整覆盖 ——
 * 这是本项目"算法层可脱离引擎世界验证"这条既有约定的延续。
 *
 * === 算法（文档 §2.1 的直译）===
 *
 *   衰减包络 = 插值(初始振幅, 目标振幅 = 初始振幅 × EndAmplitudeRatio, t / Duration)
 *   相位扰动 = 在 ±PhaseJitter 内取一个小随机值（每发一次，震动期间不变）
 *   周期项   = cos(2π / Period × t + 相位扰动)
 *   Roll     = 衰减包络 × 周期项
 *
 * === 为什么不走"累加"===
 * 文档明确区分了两类镜头运动：
 *   - Pitch/Yaw：相机每帧应用**相对上一帧的旋转增量**（因为要累加弹道抬升）
 *   - Roll：按**当前时刻的实际震动值直接求解**（因为它是围绕零点的往复震颤）
 * 若把 Roll 也做成累加，震动会变成单向漂移，永远回不到零点，看起来像镜头歪了。
 * 所以这里按"直接求解"，每帧输出的是**当前应当施加的绝对偏移量**。
 *
 * === 确定性与随机 ===
 * 相位扰动用 (Seed, ShotIndex) 的哈希逐发新建随机流，而不是复用有状态随机流。
 * 这样第 N 发的相位只取决于 (Seed, N)，不受调用顺序影响 —— Golden 数据比对才能成立。
 * Profile 的 PhaseJitterRadians = 0 时相位恒为 0，序列完全可复现。
 */
UCLASS()
class UE_API ULyraCameraRollShake : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/**
	 * 求解某一时刻的 Roll 震动值（度）。核心算法，纯函数。
	 *
	 * @param Params              本次震动生效的参数
	 * @param Elapsed             已持续时间（秒）
	 * @param PhaseOffsetRadians  本发的相位扰动（弧度），由 ComputePhaseOffset 预先算好并缓存
	 * @return 该时刻应施加到相机的 Roll 偏移（度）。Elapsed 超出 Duration 时返回 0。
	 */
	static float EvaluateRollShake(const FCameraRollShakeParams& Params, float Elapsed, float PhaseOffsetRadians = 0.0f);

	/**
	 * 求解某一时刻的衰减包络值 [0,1]。拆出来是为了调试面板能单独显示"包络 vs 周期项"，
	 * 便于区分"震动停不下来"到底是衰减太慢还是周期太长。
	 */
	static float EvaluateEnvelope(const FCameraRollShakeParams& Params, float Elapsed);

	/**
	 * 生成第 ShotIndex 发的相位扰动（弧度），范围 ±Params.PhaseJitterRadians。
	 * 同一 (Seed, ShotIndex) 必然得到同一结果。
	 */
	static float ComputePhaseOffset(const FCameraRollShakeParams& Params, int32 Seed, int32 ShotIndex);

	/**
	 * 推进震动状态一个步长，并返回当前 Roll 值。
	 * 这是运行时唯一入口：内部会做时长裁剪、到点停用、缓存调试值。
	 *
	 * 注意：**参数取自 State.ActiveParams**（触发时缓存的快照），而不是重新传入 ——
	 * 这样同一次震动的节奏不会因为中途姿态变化而突变。
	 *
	 * @param State        读写：震动状态（内含触发时缓存的生效参数）
	 * @param DeltaSeconds 固定步长（秒）
	 * @return 当前应施加的 Roll 偏移（度）
	 */
	static float Advance(FCameraRollShakeState& State, float DeltaSeconds);

	/**
	 * 触发一次震动（每发开火调用）。
	 *
	 * @param State      读写：震动状态
	 * @param Params     本次震动生效的参数（AmplitudeDegrees 应已含连射增量与倍率）
	 * @param Seed       本轮连发的种子（与弹道链同源，保证同一轮内可复现）
	 * @param ShotIndex  本发序号（0 起），用于相位扰动
	 */
	static void Trigger(FCameraRollShakeState& State, const FCameraRollShakeParams& Params, int32 Seed, int32 ShotIndex);
};

#undef UE_API
