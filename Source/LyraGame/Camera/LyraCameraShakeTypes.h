// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Curves/RichCurve.h"

#include "LyraCameraShakeTypes.generated.h"

/**
 * Roll 震动的一组可调参数快照。
 *
 * 存在的意义：把"参数怎么算"与"怎么取"解耦。
 * Profile 资产里存的是资产的原始值；本结构存的是**本次震动实际生效的值**
 * （已把连射增量、分段系数、姿态倍率折算进去），因此算法层不依赖 Profile 类型，
 * 可以直接被纯数值单测构造。
 *
 * 单位：AmplitudeDegrees 为度；Period / Duration 为秒；PhaseJitterRadians 为弧度。
 */
USTRUCT(BlueprintType)
struct FCameraRollShakeParams
{
	GENERATED_BODY()

	/** 本次震动的起始振幅（度，已含所有倍率与增量）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera|RollShake")
	float AmplitudeDegrees = 0.0f;

	/** 震动总时长（秒）。超过此值后不再产生输出。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera|RollShake")
	float DurationSeconds = 0.25f;

	/** 震动周期（秒）。越小抖得越快。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera|RollShake")
	float PeriodSeconds = 0.06f;

	/** 相位扰动范围（弧度）。0 = 每次震动相位完全一致（可复现，自动化测试用）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera|RollShake")
	float PhaseJitterRadians = 0.0f;

	/** 衰减终值比例 [0,1]。0 = 震到零，1 = 完全不衰减。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera|RollShake")
	float EndAmplitudeRatio = 0.0f;

	/**
	 * 可选的衰减曲线（X = 归一化时间 [0,1]，Y = 振幅保留比例）。
	 *
	 * 用裸指针指向 FRichCurve 而非 UObject：FRichCurve 是纯数学类型，
	 * 这样算法层既不认识 Profile，也不需要 UObject，单测可直接构造一条曲线。
	 * 为空时退化为线性衰减。
	 *
	 * 注意：**不是 UPROPERTY**（USTRUCT 不允许裸指针反射属性），
	 * 因此本字段不参与序列化，只作运行时参数传递。
	 * 生命周期：调用方应在同一次调用内装配并使用；Profile 资产的曲线生命周期足够长。
	 */
	const FRichCurve* DecayCurve = nullptr;
};

/**
 * FCameraRollShakeState
 *
 * Roll 方向阻尼震动的运行时状态。
 *
 * === 与 Pitch/Yaw 的根本区别（重要）===
 * Pitch/Yaw 走"每发累加 → 停火后回正"的**积分模型**，表现弹道持续上抬。
 * Roll 走"每发重置震动时钟 → 按衰减包络 × 周期项直接求解"的**解析模型**，
 * 表现开火瞬间的爆发震颤与释放。两者唯一共同点是都作用在相机 POV 上；
 * 除此之外没有共享逻辑，因此刻意不放进 FRecoilRuntimeState。
 *
 * 本结构只管状态，不做任何插值计算 —— 求解在 ULyraCameraRollShake 的静态函数里，
 * 这样"状态"和"算法"都能脱离 UWorld 单独测试。
 */
USTRUCT(BlueprintType)
struct FCameraRollShakeState
{
	GENERATED_BODY()

public:

	/** 本次震动的内部时钟（秒）。每发开火归零。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera|RollShake")
	float ElapsedTime = 0.0f;

	/** 本次震动是否仍在进行。false 时输出恒为 0。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera|RollShake")
	bool bActive = false;

	/** 本次震动的相位扰动（弧度）。每发开火时生成，震动期间保持不变。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera|RollShake")
	float PhaseOffset = 0.0f;

	/** 本次震动的起始振幅（度，已含连射增量与分段系数）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera|RollShake")
	float StartAmplitude = 0.0f;

	/** 最近一次求解出的 Roll 值（度），供调试面板显示。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera|RollShake")
	float CurrentRoll = 0.0f;

	/** 最近一次求解出的衰减包络值，供调试面板显示。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera|RollShake")
	float CurrentEnvelope = 0.0f;

	/** 触发本次震动的连射序号（0 起），用于连射增量与分段系数查询。 */
	UPROPERTY(BlueprintReadOnly, Category = "Camera|RollShake")
	int32 ShotIndex = 0;

	/**
	 * 本次震动生效的参数快照（触发时装配，震动期间不变）。
	 *
	 * 为什么要缓存：参数里含连射增量、分段系数、姿态倍率等"随发变化"的量。
	 * 若在每帧 Advance 时重新装配，一旦姿态中途改变（例如边打边跳），
	 * 同一次震动的振幅/周期会突变 —— 看起来就是"抖到一半突然换了个节奏"。
	 * 因此口径是：**参数的取样点在开火瞬间，震动全程沿用**，与手感预期一致。
	 *
	 * 非 UPROPERTY：内含裸指针（DecayCurve），不参与序列化。
	 */
	FCameraRollShakeParams ActiveParams;

public:

	/**
	 * 中止本次震动并归零输出，但**保留参数快照与发序号**。
	 *
	 * 与 Reset 的区别：Reset 是全量清空（含 ShotIndex 和 ActiveParams），
	 * 用在卸下武器 / 总开关关闭等"整条链断掉"的场合；
	 * Stop 只停这一次震动，用在开火时参数装配失败（例如本次振幅为 0 或不启用 Roll）
	 * 的场合 —— 此时不该把连射计数也一起清掉。
	 */
	void Stop()
	{
		ElapsedTime = 0.0f;
		bActive = false;
		CurrentRoll = 0.0f;
		CurrentEnvelope = 0.0f;
	}

	/** 全量重置。卸下武器 / 总开关关闭时调用。 */
	void Reset()
	{
		ElapsedTime = 0.0f;
		bActive = false;
		PhaseOffset = 0.0f;
		StartAmplitude = 0.0f;
		CurrentRoll = 0.0f;
		CurrentEnvelope = 0.0f;
		ShotIndex = 0;
		ActiveParams = FCameraRollShakeParams();
	}
};
