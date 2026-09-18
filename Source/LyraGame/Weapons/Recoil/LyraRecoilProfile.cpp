// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/Recoil/LyraRecoilProfile.h"

#include "LyraLogChannels.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LyraRecoilProfile)

#define LOCTEXT_NAMESPACE "LyraRecoilProfile"

namespace LyraRecoilProfileConstants
{
	/** 构造新资产时使用的默认 Pattern 长度（仅构造默认值，非运行时魔数）。 */
	static constexpr int32 DefaultPatternLength = 8;

	/** PatternPoints 为空时 GetPatternPoint 的退化返回值：纯垂直抬枪。 */
	static constexpr float DegeneratePatternY = 1.0f;
}

ULyraRecoilProfile::ULyraRecoilProfile()
{
	// 构造默认值 —— 只影响"新建资产时的初值"，运行时一律从资产读取。
	// 与 ULyraRangedWeaponInstance 构造函数里给 HeatToHeatPerShotCurve 加默认键
	// （LyraRangedWeaponInstance.cpp L18-L19）是同一范式。

	// 垂直 Kick 曲线：渐进增强（前几发稳、后段抬得快）
	VerticalKickCurve.EditorCurveData.AddKey(0.0f, 0.70f);
	VerticalKickCurve.EditorCurveData.AddKey(4.0f, 1.00f);
	VerticalKickCurve.EditorCurveData.AddKey(12.0f, 1.35f);

	// 回正曲线：线性 (0,0)-(1,1)，匀速回正
	RecoveryCurve.EditorCurveData.AddKey(0.0f, 0.0f);
	RecoveryCurve.EditorCurveData.AddKey(1.0f, 1.0f);

	// 上抬曲线：Ease-Out（先快后慢）。
	//
	// 为什么默认是 Ease-Out 而不是线性或 Ease-In（理由见 10_SingleShotInterpolation.md §6.2）：
	//  1) 物理直觉：枪机后座由冲量驱动，开火瞬间加速度最大，随后被复进簧与肩部吸收
	//  2) 观感：射击的「爆发感」来自起始的快速位移；线性显得拖沓，Ease-In 更糟（先没反应再窜上去）
	//  3) 抗低帧率：前 1/3 段完成约 60% 位移，即使只剩 2 个采样点，保住的也是「主要位移」而非零位移
	//  4) 与回弹段配合：窜上去 → 掉回来一点 → 稳一下 → 慢慢落回去，这才有顿挫感
	LiftCurve.EditorCurveData.AddKey(0.00f, 0.00f);
	LiftCurve.EditorCurveData.AddKey(0.15f, 0.35f);
	LiftCurve.EditorCurveData.AddKey(0.35f, 0.62f);
	LiftCurve.EditorCurveData.AddKey(0.60f, 0.83f);
	LiftCurve.EditorCurveData.AddKey(1.00f, 1.00f);

	// 回弹曲线：线性。回弹是个短促的「掉一下」，形状不敏感，线性最不容易配错。
	ReboundCurve.EditorCurveData.AddKey(0.0f, 0.0f);
	ReboundCurve.EditorCurveData.AddKey(1.0f, 1.0f);

	// 默认 Pattern：X 走"右—左—右"的摆尾，Y 从 0.55 爬到 1.00 后饱和。
	//
	// 职责划分（重要，不要再混淆）：
	//   PatternPoints 只描述"形状"，两个分量都恒定落在归一化区间内
	//     X ∈ [-1, 1] 水平（右为正），Y ∈ [0, 1] 垂直（上为正）
	//   "渐强"的整体上升趋势由 VerticalKickCurve 承担 —— 那才是它存在的理由。
	//   实际角度 = RecoilPerShot_* × PatternPoints × VerticalKickCurve。
	PatternPoints.Reserve(12);
	PatternPoints.Emplace(0.00f, 0.55f);
	PatternPoints.Emplace(0.05f, 0.62f);
	PatternPoints.Emplace(-0.08f, 0.70f);
	PatternPoints.Emplace(-0.02f, 0.78f);
	PatternPoints.Emplace(0.16f, 0.84f);
	PatternPoints.Emplace(0.34f, 0.90f);
	PatternPoints.Emplace(0.22f, 0.94f);
	PatternPoints.Emplace(-0.10f, 0.97f);
	PatternPoints.Emplace(-0.30f, 1.00f);
	PatternPoints.Emplace(-0.18f, 1.00f);
	PatternPoints.Emplace(0.12f, 1.00f);
	PatternPoints.Emplace(0.28f, 1.00f);

	PatternLength = LyraRecoilProfileConstants::DefaultPatternLength;
}

void ULyraRecoilProfile::PostLoad()
{
	Super::PostLoad();

	// 兼容手工改动过的旧资产
	SanitizePatternLength();
}

#if WITH_EDITOR
void ULyraRecoilProfile::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	SanitizePatternLength();
}
#endif

void ULyraRecoilProfile::SanitizePatternLength()
{
	PatternLength = FMath::Clamp(PatternLength, 0, PatternPoints.Num());
}

float ULyraRecoilProfile::GetVerticalKickCurveScale(int32 ShotIndex) const
{
	const FRichCurve* Curve = VerticalKickCurve.GetRichCurveConst();
	if ((Curve == nullptr) || !Curve->HasAnyData())
	{
		return 1.0f;
	}

	return Curve->Eval(static_cast<float>(FMath::Max(0, ShotIndex)));
}

float ULyraRecoilProfile::GetRecoveryAlpha(float NormalizedTime) const
{
	const float ClampedTime = FMath::Clamp(NormalizedTime, 0.0f, 1.0f);

	const FRichCurve* Curve = RecoveryCurve.GetRichCurveConst();
	if ((Curve == nullptr) || !Curve->HasAnyData())
	{
		// 没有曲线时退化为线性回正，保证系统永远可用
		return ClampedTime;
	}

	return FMath::Clamp(Curve->Eval(ClampedTime), 0.0f, 1.0f);
}

float ULyraRecoilProfile::GetLiftAlpha(float NormalizedTime) const
{
	const float ClampedTime = FMath::Clamp(NormalizedTime, 0.0f, 1.0f);

	const FRichCurve* Curve = LiftCurve.GetRichCurveConst();
	if ((Curve == nullptr) || !Curve->HasAnyData())
	{
		// 没有曲线时退化为线性上抬。注意这**不是** Ease-Out ——
		// 资产漏配曲线时宁可给一条中性的直线上抬，也不要偷偷替策划做决定。
		return ClampedTime;
	}

	return FMath::Clamp(Curve->Eval(ClampedTime), 0.0f, 1.0f);
}

float ULyraRecoilProfile::GetReboundAlpha(float NormalizedTime) const
{
	const float ClampedTime = FMath::Clamp(NormalizedTime, 0.0f, 1.0f);

	const FRichCurve* Curve = ReboundCurve.GetRichCurveConst();
	if ((Curve == nullptr) || !Curve->HasAnyData())
	{
		return ClampedTime;
	}

	return FMath::Clamp(Curve->Eval(ClampedTime), 0.0f, 1.0f);
}

FRecoilPatternPoint ULyraRecoilProfile::GetPatternPoint(int32 ShotIndex) const
{
	if (PatternPoints.Num() == 0)
	{
		return FRecoilPatternPoint(0.0f, LyraRecoilProfileConstants::DegeneratePatternY);
	}

	// 越界时沿用最后一个点，避免尾部发散
	const int32 Index = FMath::Clamp(ShotIndex, 0, PatternPoints.Num() - 1);
	return PatternPoints[Index];
}

float ULyraRecoilProfile::GetPoseMultiplier(EPoseState PoseState) const
{
	switch (PoseState)
	{
	case EPoseState::Crouching:
		return PoseMultiplier_Crouching;

	case EPoseState::JumpingOrFalling:
		return PoseMultiplier_JumpingOrFalling;

	case EPoseState::Standing:
	default:
		return PoseMultiplier_Standing;
	}
}

float ULyraRecoilProfile::GetAimingBlendedMultiplier(float AimingAlpha) const
{
	const float Alpha = FMath::Clamp(AimingAlpha, 0.0f, 1.0f);

	// AimingAlpha 为 0 时完全不瞄准（倍率 1），为 1 时完全生效（倍率 PoseMultiplier_Aiming）
	return FMath::Lerp(1.0f, PoseMultiplier_Aiming, Alpha);
}

// ---------------------------------------------------------------------------
// Roll 震屏（独立通道，见 LyraCameraShakeTypes.h / LyraCameraRollShake.h）
//
// 注意：这里**没有** GetRollShakeDecayAlpha。
// 衰减曲线由 BuildRollShakeParams 直接把 FRichCurve 指针交给算法层
// （FCameraRollShakeParams::DecayCurve），算法层自己 Eval。
// 曾经有过一个 GetRollShakeDecayAlpha 包一层，但它制造了两套衰减逻辑并存的隐患
// （Profile 一套、算法层一套），已删除 —— 衰减只有一个实现点，在 ULyraCameraRollShake。
// ---------------------------------------------------------------------------

float ULyraRecoilProfile::GetRollShakeSegmentScale(int32 ShotIndex) const
{
	const FRichCurve* Curve = RollShake_SegmentScaleCurve.GetRichCurveConst();
	if ((Curve == nullptr) || !Curve->HasAnyData())
	{
		return 1.0f;
	}

	return FMath::Max(0.0f, Curve->Eval(static_cast<float>(FMath::Max(0, ShotIndex))));
}

float ULyraRecoilProfile::GetRollShakePeriodScale(int32 ShotIndex) const
{
	const FRichCurve* Curve = RollShake_PeriodScaleCurve.GetRichCurveConst();
	if ((Curve == nullptr) || !Curve->HasAnyData())
	{
		return 1.0f;
	}

	return FMath::Max(KINDA_SMALL_NUMBER, Curve->Eval(static_cast<float>(FMath::Max(0, ShotIndex))));
}

bool ULyraRecoilProfile::BuildRollShakeParams(
	int32 ShotIndex,
	float PoseMultiplier,
	float GlobalScale,
	int32 Seed,
	FCameraRollShakeParams& OutParams) const
{
	OutParams = FCameraRollShakeParams();

	if (!bEnableRollShake)
	{
		return false;
	}

	const int32 ClampedIndex = FMath::Max(0, ShotIndex);

	// 连射增量：从 RampStartShot 起线性叠加，封顶。
	// 例：Amplitude=0.6, PerShot=0.05, Start=4, MaxBonus=0.9
	//     第 0~3 发 → 0.6；第 4 发 → 0.65；第 22 发起 → 1.5（0.6+0.9）
	float AmplitudeBonus = 0.0f;
	if (ClampedIndex >= RollShake_RampStartShot)
	{
		const int32 RampShotCount = ClampedIndex - RollShake_RampStartShot + 1;
		AmplitudeBonus = FMath::Min(RampShotCount * RollShake_AmplitudePerShot, RollShake_MaxAmplitudeBonus);
	}

	// 振幅 = (基础 + 连射增量) × 分段系数 × 姿态倍率 × 全局倍率
	const float BaseAmplitude = FMath::Max(0.0f, RollShake_Amplitude + AmplitudeBonus);
	const float SegmentScale = GetRollShakeSegmentScale(ClampedIndex);
	const float Magnitude = FMath::Max(0.0f, PoseMultiplier) * FMath::Max(0.0f, GlobalScale);

	OutParams.AmplitudeDegrees = BaseAmplitude * SegmentScale * Magnitude;
	OutParams.DurationSeconds = RollShake_Duration;
	OutParams.PeriodSeconds = RollShake_Period * GetRollShakePeriodScale(ClampedIndex);
	OutParams.PhaseJitterRadians = RollShake_PhaseJitter;
	OutParams.EndAmplitudeRatio = RollShake_EndAmplitudeRatio;

	// 把衰减曲线本身交给算法层（算法层只认 FRichCurve，不认识 Profile）。
	// GetRichCurveConst 返回的曲线生命周期跟随资产，调用方立刻用掉，安全。
	OutParams.DecayCurve = RollShake_AmplitudeCurve.GetRichCurveConst();

	// 振幅为 0 时不产生震动（例如全局倍率被调成 0 做 A/B 对比）
	return !FMath::IsNearlyZero(OutParams.AmplitudeDegrees, 1e-5f);
}

bool ULyraRecoilProfile::ValidateProfile(TArray<FString>& OutErrors) const
{
	const int32 StartNum = OutErrors.Num();

	// 注意：校验消息一律使用英文 —— 引擎控制台/日志按 ANSI 输出中文会乱码，
	// 这些消息要作为验收证据被复制粘贴，可读性优先。
	const FString Prefix = FString::Printf(TEXT("[%s]"), *GetPathName());

	auto CheckMultiplier = [&](const TCHAR* Name, float Value)
	{
		if (!(Value > 0.0f && Value <= 5.0f))
		{
			OutErrors.Add(FString::Printf(TEXT("%s %s = %.4f out of valid range (0, 5]"), *Prefix, Name, Value));
		}
	};

	// --- Base ---
	if (RecoilPerShot_Vertical < 0.0f)
	{
		OutErrors.Add(FString::Printf(TEXT("%s RecoilPerShot_Vertical = %.4f must not be negative"), *Prefix, RecoilPerShot_Vertical));
	}
	if (RecoilPerShot_Horizontal < 0.0f)
	{
		OutErrors.Add(FString::Printf(TEXT("%s RecoilPerShot_Horizontal = %.4f must not be negative"), *Prefix, RecoilPerShot_Horizontal));
	}

	// --- Curves must not be empty ---
	const FRichCurve* VerticalCurve = VerticalKickCurve.GetRichCurveConst();
	if ((VerticalCurve == nullptr) || !VerticalCurve->HasAnyData())
	{
		OutErrors.Add(FString::Printf(TEXT("%s VerticalKickCurve is empty: at least one key is required"), *Prefix));
	}

	const FRichCurve* RecoveryCurvePtr = RecoveryCurve.GetRichCurveConst();
	if ((RecoveryCurvePtr == nullptr) || !RecoveryCurvePtr->HasAnyData())
	{
		OutErrors.Add(FString::Printf(TEXT("%s RecoveryCurve is empty: at least one key is required"), *Prefix));
	}

	// --- Recovery ---
	if (!(RecoveryTime > 0.0f))
	{
		OutErrors.Add(FString::Printf(TEXT("%s RecoveryTime = %.4f must be > 0"), *Prefix, RecoveryTime));
	}
	if (RecoveryDelay < 0.0f)
	{
		OutErrors.Add(FString::Printf(TEXT("%s RecoveryDelay = %.4f must not be negative"), *Prefix, RecoveryDelay));
	}
	if (RecoilReturnRatio < 0.0f || RecoilReturnRatio > 1.0f)
	{
		OutErrors.Add(FString::Printf(TEXT("%s RecoilReturnRatio = %.4f must be within [0, 1]"), *Prefix, RecoilReturnRatio));
	}

	// --- Clamp ---
	if (!(MaxVerticalKick > 0.0f))
	{
		OutErrors.Add(FString::Printf(TEXT("%s MaxVerticalKick = %.4f must be > 0"), *Prefix, MaxVerticalKick));
	}
	if (!(MaxHorizontalKick > 0.0f))
	{
		OutErrors.Add(FString::Printf(TEXT("%s MaxHorizontalKick = %.4f must be > 0"), *Prefix, MaxHorizontalKick));
	}

	// --- SingleShot ---
	//
	// 只在插值模式下校验阶段参数：InstantWrite 模式下这组参数不参与计算，
	// 报了错反而会误导策划去改一个不影响手感的旋钮。
	if (SingleShotMode == ERecoilSingleShotMode::Interpolated)
	{
		if (LiftDuration <= 0.0f)
		{
			OutErrors.Add(FString::Printf(TEXT("%s LiftDuration = %.4f must be > 0 in Interpolated mode "
				"(a zero-length lift stage makes the shot instantaneous again)"), *Prefix, LiftDuration));
		}
		if (ReboundDuration < 0.0f)
		{
			OutErrors.Add(FString::Printf(TEXT("%s ReboundDuration = %.4f must not be negative"), *Prefix, ReboundDuration));
		}
		if (ReboundRatio < 0.0f || ReboundRatio > 1.0f)
		{
			OutErrors.Add(FString::Printf(TEXT("%s ReboundRatio = %.4f must be within [0, 1]"), *Prefix, ReboundRatio));
		}
		if (ReboundRatio < RecoilReturnRatio)
		{
			// 回弹点低于最终稳态点的话，下降段会变成「往上走」，观感是一个诡异的回升
			OutErrors.Add(FString::Printf(TEXT("%s ReboundRatio = %.4f must be >= RecoilReturnRatio = %.4f, "
				"otherwise the drop stage would rise instead of settling down"), *Prefix, ReboundRatio, RecoilReturnRatio));
		}

		const FRichCurve* LiftCurvePtr = LiftCurve.GetRichCurveConst();
		if ((LiftCurvePtr == nullptr) || !LiftCurvePtr->HasAnyData())
		{
			OutErrors.Add(FString::Printf(TEXT("%s LiftCurve is empty: Interpolated mode uses it to shape the lift stage "
				"(it silently falls back to a straight line)"), *Prefix));
		}
	}

	// --- Pattern ---
	if (PatternLength < 0 || PatternLength > PatternPoints.Num())
	{
		OutErrors.Add(FString::Printf(TEXT("%s PatternLength = %d out of range [0, PatternPoints.Num()=%d]"),
			*Prefix, PatternLength, PatternPoints.Num()));
	}
	if (HorizontalRandomRange < 0.0f || HorizontalRandomRange > 1.0f)
	{
		OutErrors.Add(FString::Printf(TEXT("%s HorizontalRandomRange = %.4f must be within [0, 1]"), *Prefix, HorizontalRandomRange));
	}

	for (int32 Index = 0; Index < PatternPoints.Num(); ++Index)
	{
		const FRecoilPatternPoint& Point = PatternPoints[Index];
		if (Point.X < -1.0f || Point.X > 1.0f)
		{
			OutErrors.Add(FString::Printf(TEXT("%s PatternPoints[%d].X = %.4f must be within [-1, 1]"), *Prefix, Index, Point.X));
		}
		if (Point.Y < 0.0f || Point.Y > 1.0f)
		{
			OutErrors.Add(FString::Printf(TEXT("%s PatternPoints[%d].Y = %.4f must be within [0, 1]. "
				"Pattern only describes shape; put the rising ramp into VerticalKickCurve instead."), *Prefix, Index, Point.Y));
		}
	}

	// --- 倍率 (0, 5] ---
	CheckMultiplier(TEXT("PoseMultiplier_Aiming"), PoseMultiplier_Aiming);
	CheckMultiplier(TEXT("PoseMultiplier_Standing"), PoseMultiplier_Standing);
	CheckMultiplier(TEXT("PoseMultiplier_Crouching"), PoseMultiplier_Crouching);
	CheckMultiplier(TEXT("PoseMultiplier_JumpingOrFalling"), PoseMultiplier_JumpingOrFalling);

	return OutErrors.Num() == StartNum;
}

#undef LOCTEXT_NAMESPACE
