// Copyright Epic Games, Inc. All Rights Reserved.

#include "Camera/LyraCameraRollShake.h"

#include "Math/RandomStream.h"
#include "Templates/TypeHash.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LyraCameraRollShake)

namespace LyraCameraRollShakePrivate
{
	/** 避免除零：周期/时长至少给一个极小值。 */
	constexpr float MinPeriod = 1e-3f;
	constexpr float MinDuration = 1e-3f;

	/**
	 * 与调用顺序无关的确定性随机采样，取值 [-1, 1]。
	 * 与 LyraRecoilState.cpp 的同名实现保持同一范式：逐发新建随机流。
	 */
	static float DeterministicSignedRandom(int32 Seed, int32 Index)
	{
		const uint32 Combined = HashCombine(static_cast<uint32>(Seed), static_cast<uint32>(Index));
		FRandomStream Stream(static_cast<int32>(Combined));
		return Stream.FRandRange(-1.0f, 1.0f);
	}
}

float ULyraCameraRollShake::EvaluateEnvelope(const FCameraRollShakeParams& Params, float Elapsed)
{
	using namespace LyraCameraRollShakePrivate;

	const float Duration = FMath::Max(Params.DurationSeconds, MinDuration);
	const float NormalizedTime = FMath::Clamp(Elapsed / Duration, 0.0f, 1.0f);

	const float EndAmplitude = FMath::Clamp(Params.EndAmplitudeRatio, 0.0f, 1.0f);

	// 文档 §2.1：「按衰减曲线插值(初始振幅, 目标振幅, 已持续时间 / 总持续时间)」。
	// 衰减曲线由 Profile 提供（RollShake_AmplitudeCurve），算法层不认识 Profile，
	// 只拿到一条 FRichCurve —— 曲线为空时退化为线性，保证系统永远可用。
	if (Params.DecayCurve != nullptr && Params.DecayCurve->HasAnyData())
	{
		const float CurveValue = FMath::Clamp(Params.DecayCurve->Eval(NormalizedTime), 0.0f, 1.0f);

		// 曲线给出的是"振幅保留比例"，终点再按 EndAmplitudeRatio 收一下：
		// 曲线本身负责形状（先猛后缓 / 匀速），EndAmplitudeRatio 负责终值不为零的稳态偏角。
		const float EndBlend = FMath::Lerp(CurveValue, EndAmplitude, NormalizedTime);
		return FMath::Clamp(EndBlend, 0.0f, 1.0f);
	}

	// 无曲线：线性 1 → EndAmplitudeRatio
	return FMath::Lerp(1.0f, EndAmplitude, NormalizedTime);
}

float ULyraCameraRollShake::EvaluateRollShake(const FCameraRollShakeParams& Params, float Elapsed, float PhaseOffsetRadians)
{
	using namespace LyraCameraRollShakePrivate;

	if (Elapsed < 0.0f)
	{
		return 0.0f;
	}

	const float Duration = FMath::Max(Params.DurationSeconds, MinDuration);
	if (Elapsed >= Duration)
	{
		// 震动结束：不再产生任何偏移。注意是"归零"而不是"停在末值"——
		// 末值本身由 EndAmplitudeRatio 决定，但周期项在末刻的取值是随机的，
		// 硬留一个末值会让每发结束时镜头歪一个不可预期的角度。
		return 0.0f;
	}

	const float Envelope = EvaluateEnvelope(Params, Elapsed);

	// 文档：周期项 = cos(2π / 周期 × 已持续时间 + 相位扰动)
	//
	// 相位扰动由调用方传入（每发开火时用 (Seed, ShotIndex) 算一次后缓存）。
	// 之所以不放在 Params 里：Params 是"这一发怎么震"的静态描述，
	// 而相位是"这一发的随机结果"，两者语义不同 —— 混在一起会让
	// BuildRollShakeParams 变成有状态函数，破坏它的可复现性契约。
	const float Period = FMath::Max(Params.PeriodSeconds, MinPeriod);
	const float Phase = 2.0f * PI / Period * Elapsed + PhaseOffsetRadians;
	const float PeriodicTerm = FMath::Cos(Phase);

	// 文档：返回 衰减包络 × 周期项，再乘以振幅得到实际角度
	// （文档伪码里包络本身已带振幅语义；这里把振幅单独拎出来，便于连射增量叠加。）
	return Params.AmplitudeDegrees * Envelope * PeriodicTerm;
}

float ULyraCameraRollShake::ComputePhaseOffset(const FCameraRollShakeParams& Params, int32 Seed, int32 ShotIndex)
{
	const float Jitter = FMath::Max(0.0f, Params.PhaseJitterRadians);
	if (FMath::IsNearlyZero(Jitter, KINDA_SMALL_NUMBER))
	{
		// 相位扰动为 0：完全可复现，自动化测试与 Golden 数据走这条
		return 0.0f;
	}

	return LyraCameraRollShakePrivate::DeterministicSignedRandom(Seed, ShotIndex) * Jitter;
}

void ULyraCameraRollShake::Trigger(
	FCameraRollShakeState& State,
	const FCameraRollShakeParams& Params,
	int32 Seed,
	int32 ShotIndex)
{
	State.ShotIndex = ShotIndex;
	State.StartAmplitude = Params.AmplitudeDegrees;
	State.PhaseOffset = ComputePhaseOffset(Params, Seed, ShotIndex);
	State.ElapsedTime = 0.0f;
	State.bActive = !FMath::IsNearlyZero(Params.AmplitudeDegrees, 1e-5f);
	State.CurrentEnvelope = 1.0f;

	// 缓存本发生效的参数：震动全程沿用，中途姿态变化不会改变本次震动的节奏（见头文件说明）
	State.ActiveParams = Params;

	// 触发瞬间即求解一次：第 0 帧 t=0，cos(PhaseOffset) 通常接近 1，
	// 所以玩家看到的第一个采样点是"最大振幅"附近 —— 这正是文档要的"开火瞬间爆发感"。
	State.CurrentRoll = State.bActive ? EvaluateRollShake(State.ActiveParams, 0.0f, State.PhaseOffset) : 0.0f;
}

float ULyraCameraRollShake::Advance(
	FCameraRollShakeState& State,
	float DeltaSeconds)
{
	if (!State.bActive)
	{
		State.CurrentRoll = 0.0f;
		State.CurrentEnvelope = 0.0f;
		return 0.0f;
	}

	// 一律使用触发时缓存的参数，不重新装配 —— 保证同一次震动的节奏恒定
	const FCameraRollShakeParams& Params = State.ActiveParams;

	State.ElapsedTime += FMath::Max(0.0f, DeltaSeconds);

	const float Duration = FMath::Max(Params.DurationSeconds, LyraCameraRollShakePrivate::MinDuration);
	if (State.ElapsedTime >= Duration)
	{
		// 时长到点：停用并归零
		State.bActive = false;
		State.CurrentRoll = 0.0f;
		State.CurrentEnvelope = 0.0f;
		return 0.0f;
	}

	State.CurrentEnvelope = EvaluateEnvelope(Params, State.ElapsedTime);
	State.CurrentRoll = EvaluateRollShake(Params, State.ElapsedTime, State.PhaseOffset);
	return State.CurrentRoll;
}
