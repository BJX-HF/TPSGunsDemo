// Copyright Epic Games, Inc. All Rights Reserved.

#include "Private/Recoil/LyraRecoilPatternAdapter.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Math/NumericLimits.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"

namespace LyraRecoilPatternAdapterInternal
{
	/** 判定“某发增量未变化”的容差（度）。远低于任何有意义的角度，但足以吸收 double 抖动。 */
	constexpr double PointUnchangedTolerance = 1.0e-9;

	/** 判定“目标增量为零”的容差（度）。分母不可逆时只有零增量可以保留隐藏值。 */
	constexpr double ZeroAngleTolerance = 1.0e-9;

	bool IsFiniteValue(double Value)
	{
		return FMath::IsFinite(Value);
	}

	/** 形状与有限性检查：L ∈ [0, N] 且所有原始点有限。 */
	bool ValidateDataShape(const FLyraRecoilPatternData& Data, TArray<FString>& Errors)
	{
		if (Data.PatternLength < 0 || Data.PatternLength > Data.Points.Num())
		{
			Errors.Add(FString::Printf(TEXT("PatternLength = %d is out of range [0, %d]; fix the asset before editing"),
				Data.PatternLength, Data.Points.Num()));
			return false;
		}

		for (int32 Index = 0; Index < Data.Points.Num(); ++Index)
		{
			const FRecoilPatternPoint& Point = Data.Points[Index];
			if (!IsFiniteValue(static_cast<double>(Point.X)) || !IsFiniteValue(static_cast<double>(Point.Y)))
			{
				Errors.Add(FString::Printf(TEXT("PatternPoints[%d] is not finite (X=%.6f, Y=%.6f); refusing to convert"),
					Index, static_cast<double>(Point.X), static_cast<double>(Point.Y)));
				return false;
			}
		}

		return true;
	}

	/**
	 * 归一化候选的范围检查。
	 * 容差内允许归并到边界并记录误差；超出容差整体拒绝（绝不钳制后声称无损）。
	 */
	bool CheckNormalizedValue(int32 Index, const TCHAR* AxisName, double Value, double MinValue, double MaxValue,
		FLyraRecoilConversionReport& Report, TArray<FString>& Errors)
	{
		if (!IsFiniteValue(Value))
		{
			Errors.Add(FString::Printf(TEXT("PatternPoints[%d].%s candidate is not finite (NaN/Inf)"), Index, AxisName));
			return false;
		}

		if (Value < MinValue - FLyraRecoilPatternAdapter::NormalizedTolerance ||
			Value > MaxValue + FLyraRecoilPatternAdapter::NormalizedTolerance)
		{
			Errors.Add(FString::Printf(TEXT("Shot %d: PatternPoints.%s candidate %.8f is outside the allowed range [%.4f, %.4f]"),
				Index, AxisName, Value, MinValue, MaxValue));
			return false;
		}

		if (Value < MinValue || Value > MaxValue)
		{
			const double Clamped = FMath::Clamp(Value, MinValue, MaxValue);
			Report.MaxNormalizedClampError = FMath::Max(Report.MaxNormalizedClampError, FMath::Abs(Value - Clamped));
			Report.BoundaryClampedIndices.AddUnique(Index);
		}

		return true;
	}

	/**
	 * 正向重建校验：用给定强度与 Profile 曲线从候选 float 值重建累计角度，
	 * 与目标累计点逐发比较；超出容差即拒绝（“提交时转为 float 字段并再做正向重建检查”）。
	 */
	bool RebuildAndVerify(const ULyraRecoilProfile& Profile, const TArray<FRecoilPatternPoint>& Points, int32 Length,
		const TArray<FVector2D>& Targets, double Horizontal, double Vertical,
		FLyraRecoilConversionReport& Report, TArray<FString>& Errors)
	{
		double Yaw = 0.0;
		double Pitch = 0.0;

		for (int32 Index = 0; Index < Length; ++Index)
		{
			const double CurveScale = static_cast<double>(Profile.GetVerticalKickCurveScale(Index));
			if (!IsFiniteValue(CurveScale))
			{
				Errors.Add(FString::Printf(TEXT("VerticalKickCurve sample at shot %d is not finite"), Index));
				return false;
			}

			Yaw += Horizontal * static_cast<double>(Points[Index].X);
			Pitch += Vertical * static_cast<double>(Points[Index].Y) * CurveScale;

			if (!IsFiniteValue(Yaw) || !IsFiniteValue(Pitch))
			{
				Errors.Add(FString::Printf(TEXT("Shot %d: forward rebuild produced a non-finite cumulative offset"), Index));
				return false;
			}

			const double ErrorYaw = FMath::Abs(Yaw - Targets[Index].X);
			const double ErrorPitch = FMath::Abs(Pitch - Targets[Index].Y);
			Report.MaxAngleRebuildError = FMath::Max(Report.MaxAngleRebuildError, FMath::Max(ErrorYaw, ErrorPitch));

			if (ErrorYaw > FLyraRecoilPatternAdapter::GetAngleTolerance(Targets[Index].X) ||
				ErrorPitch > FLyraRecoilPatternAdapter::GetAngleTolerance(Targets[Index].Y))
			{
				Errors.Add(FString::Printf(
					TEXT("Shot %d: forward rebuild error (yaw %.8f, pitch %.8f) exceeds tolerance after float conversion; refusing the whole candidate"),
					Index, ErrorYaw, ErrorPitch));
				return false;
			}
		}

		return true;
	}

	void CheckCurveKeys(const TCHAR* CurveName, const FRuntimeFloatCurve& Curve, TArray<FString>& Errors)
	{
		const FRichCurve* RichCurve = Curve.GetRichCurveConst();
		if (RichCurve == nullptr)
		{
			return;
		}

		if (!IsFiniteValue(RichCurve->DefaultValue)) Errors.Add(FString::Printf(TEXT("%s default value is not finite"), CurveName));
		for (int32 KeyIndex = 0; KeyIndex < RichCurve->Keys.Num(); ++KeyIndex)
		{
			const FRichCurveKey& Key = RichCurve->Keys[KeyIndex];
			if (!IsFiniteValue(static_cast<double>(Key.Time)) ||
				!IsFiniteValue(static_cast<double>(Key.Value)) ||
				!IsFiniteValue(static_cast<double>(Key.ArriveTangent)) ||
				!IsFiniteValue(static_cast<double>(Key.LeaveTangent)) ||
				!IsFiniteValue(Key.ArriveTangentWeight) || !IsFiniteValue(Key.LeaveTangentWeight))
			{
				Errors.Add(FString::Printf(TEXT("%s key %d is not finite (NaN/Inf)"), CurveName, KeyIndex));
			}
		}
	}

	bool ReadJsonNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, double& OutValue)
	{
		const TSharedPtr<FJsonValue> FieldValue = Object->TryGetField(FieldName);
		if (!FieldValue.IsValid() || FieldValue->Type != EJson::Number)
		{
			return false;
		}

		OutValue = FieldValue->AsNumber();
		return true;
	}

	/** 内部 UHT 生成的 GetPathName 前缀在测试里不稳定，这里用统一格式。 */
	FString MakeMessagePrefix()
	{
		return TEXT("[RecoilPattern]");
	}
}

using namespace LyraRecoilPatternAdapterInternal;

// ---------------------------------------------------------------------------
// 读取与全 Profile 校验
// ---------------------------------------------------------------------------

FLyraRecoilPatternData FLyraRecoilPatternAdapter::Read(const ULyraRecoilProfile& Profile)
{
	FLyraRecoilPatternData Data;
	Data.Points = Profile.PatternPoints;
	Data.PatternLength = Profile.PatternLength;
	return Data;
}

bool FLyraRecoilPatternAdapter::Validate(const ULyraRecoilProfile& Profile, TArray<FString>& Errors)
{
	const int32 StartErrorCount = Errors.Num();

	// 1) 复用资产级校验（const 只读，不触发 PostEditChangeProperty / 标脏）。
	Profile.ValidateProfile(Errors);

	// 2) 编辑器附加：有限数检查。ValidateProfile 的范围比较对 NaN 恒为 false，
	//    因此这里必须独立补一遍，避免 NaN 进入 Slate 绘制与资产提交。
	const FString Prefix = MakeMessagePrefix();

	auto CheckFloat = [&Errors, &Prefix](const TCHAR* Name, double Value)
	{
		if (!IsFiniteValue(Value))
		{
			Errors.Add(FString::Printf(TEXT("%s %s is not finite (NaN/Inf)"), *Prefix, Name));
		}
	};

	auto CheckVector = [&Errors, &Prefix](const TCHAR* Name, const FVector& Value)
	{
		if (!IsFiniteValue(Value.X) || !IsFiniteValue(Value.Y) || !IsFiniteValue(Value.Z))
		{
			Errors.Add(FString::Printf(TEXT("%s %s is not finite (NaN/Inf)"), *Prefix, Name));
		}
	};

	auto CheckRotator = [&Errors, &Prefix](const TCHAR* Name, const FRotator& Value)
	{
		if (!IsFiniteValue(Value.Pitch) || !IsFiniteValue(Value.Yaw) || !IsFiniteValue(Value.Roll))
		{
			Errors.Add(FString::Printf(TEXT("%s %s is not finite (NaN/Inf)"), *Prefix, Name));
		}
	};

	// --- Base / SingleShot / Recovery / Clamp ---
	CheckFloat(TEXT("RecoilPerShot_Vertical"), Profile.RecoilPerShot_Vertical);
	CheckFloat(TEXT("RecoilPerShot_Horizontal"), Profile.RecoilPerShot_Horizontal);
	CheckFloat(TEXT("LiftDuration"), Profile.LiftDuration);
	CheckFloat(TEXT("ReboundDuration"), Profile.ReboundDuration);
	CheckFloat(TEXT("ReboundRatio"), Profile.ReboundRatio);
	CheckFloat(TEXT("RecoveryDelay"), Profile.RecoveryDelay);
	CheckFloat(TEXT("RecoveryTime"), Profile.RecoveryTime);
	CheckFloat(TEXT("MaxVerticalKick"), Profile.MaxVerticalKick);
	CheckFloat(TEXT("MaxHorizontalKick"), Profile.MaxHorizontalKick);

	// --- Pattern ---
	CheckFloat(TEXT("HorizontalRandomRange"), Profile.HorizontalRandomRange);
	for (int32 Index = 0; Index < Profile.PatternPoints.Num(); ++Index)
	{
		CheckFloat(*FString::Printf(TEXT("PatternPoints[%d].X"), Index), Profile.PatternPoints[Index].X);
		CheckFloat(*FString::Printf(TEXT("PatternPoints[%d].Y"), Index), Profile.PatternPoints[Index].Y);
	}

	// --- Multipliers ---
	CheckFloat(TEXT("PoseMultiplier_Aiming"), Profile.PoseMultiplier_Aiming);
	CheckFloat(TEXT("PoseMultiplier_Standing"), Profile.PoseMultiplier_Standing);
	CheckFloat(TEXT("PoseMultiplier_Crouching"), Profile.PoseMultiplier_Crouching);
	CheckFloat(TEXT("PoseMultiplier_JumpingOrFalling"), Profile.PoseMultiplier_JumpingOrFalling);

	// --- RollShake ---
	CheckFloat(TEXT("RollShake_Amplitude"), Profile.RollShake_Amplitude);
	CheckFloat(TEXT("RollShake_Duration"), Profile.RollShake_Duration);
	CheckFloat(TEXT("RollShake_Period"), Profile.RollShake_Period);
	CheckFloat(TEXT("RollShake_PhaseJitter"), Profile.RollShake_PhaseJitter);
	CheckFloat(TEXT("RollShake_EndAmplitudeRatio"), Profile.RollShake_EndAmplitudeRatio);
	CheckFloat(TEXT("RollShake_AmplitudePerShot"), Profile.RollShake_AmplitudePerShot);
	CheckFloat(TEXT("RollShake_MaxAmplitudeBonus"), Profile.RollShake_MaxAmplitudeBonus);

	// --- Spread ---
	CheckFloat(TEXT("SpreadAngle_Standing"), Profile.SpreadAngle_Standing);
	CheckFloat(TEXT("MaxSpreadAngle_Standing"), Profile.MaxSpreadAngle_Standing);
	CheckFloat(TEXT("SpreadAddPerShot_Standing"), Profile.SpreadAddPerShot_Standing);
	CheckFloat(TEXT("SpreadRecoverRate_Standing"), Profile.SpreadRecoverRate_Standing);
	CheckFloat(TEXT("SpreadAngle_Crouching"), Profile.SpreadAngle_Crouching);
	CheckFloat(TEXT("MaxSpreadAngle_Crouching"), Profile.MaxSpreadAngle_Crouching);
	CheckFloat(TEXT("SpreadAddPerShot_Crouching"), Profile.SpreadAddPerShot_Crouching);
	CheckFloat(TEXT("SpreadRecoverRate_Crouching"), Profile.SpreadRecoverRate_Crouching);
	CheckFloat(TEXT("SpreadAngle_JumpingOrFalling"), Profile.SpreadAngle_JumpingOrFalling);
	CheckFloat(TEXT("MaxSpreadAngle_JumpingOrFalling"), Profile.MaxSpreadAngle_JumpingOrFalling);
	CheckFloat(TEXT("SpreadAddPerShot_JumpingOrFalling"), Profile.SpreadAddPerShot_JumpingOrFalling);
	CheckFloat(TEXT("SpreadRecoverRate_JumpingOrFalling"), Profile.SpreadRecoverRate_JumpingOrFalling);
	CheckFloat(TEXT("SpreadMultiplier_Aiming"), Profile.SpreadMultiplier_Aiming);
	CheckFloat(TEXT("SpreadMultiplier_StandingStill"), Profile.SpreadMultiplier_StandingStill);
	CheckFloat(TEXT("SpreadStandingStillSpeedThreshold"), Profile.SpreadStandingStillSpeedThreshold);
	CheckFloat(TEXT("SpreadStandingStillToMovingRange"), Profile.SpreadStandingStillToMovingRange);
	CheckFloat(TEXT("SpreadTransitionRate_StandingStill"), Profile.SpreadTransitionRate_StandingStill);
	CheckFloat(TEXT("SpreadRecoveryDelay"), Profile.SpreadRecoveryDelay);
	CheckFloat(TEXT("SpreadExponent"), Profile.SpreadExponent);

	// --- WeaponVisual（开关关闭时 ValidateProfile 会跳过，这里仍做有限数检查）---
	const FWeaponVisualRecoilSettings& Visual = Profile.WeaponVisual;
	CheckFloat(TEXT("WeaponVisual.VisualScale"), Visual.VisualScale);
	CheckFloat(TEXT("WeaponVisual.PitchFromVerticalKick"), Visual.PitchFromVerticalKick);
	CheckFloat(TEXT("WeaponVisual.YawFromHorizontalKick"), Visual.YawFromHorizontalKick);
	CheckFloat(TEXT("WeaponVisual.RollFromYaw"), Visual.RollFromYaw);
	CheckFloat(TEXT("WeaponVisual.UpCmPerShot"), Visual.UpCmPerShot);
	CheckFloat(TEXT("WeaponVisual.SideCmPerShot"), Visual.SideCmPerShot);
	CheckFloat(TEXT("WeaponVisual.ADSVisualScale"), Visual.ADSVisualScale);
	CheckFloat(TEXT("WeaponVisual.BackCmPerShot"), Visual.BackCmPerShot);
	CheckFloat(TEXT("WeaponVisual.AttackDuration"), Visual.AttackDuration);
	CheckFloat(TEXT("WeaponVisual.PeakHoldDuration"), Visual.PeakHoldDuration);
	CheckFloat(TEXT("WeaponVisual.ReturnTime"), Visual.ReturnTime);
	CheckFloat(TEXT("WeaponVisual.MaxPitchDegrees"), Visual.MaxPitchDegrees);
	CheckFloat(TEXT("WeaponVisual.MaxYawDegrees"), Visual.MaxYawDegrees);
	CheckFloat(TEXT("WeaponVisual.MaxRollDegrees"), Visual.MaxRollDegrees);
	CheckFloat(TEXT("WeaponVisual.MaxBackCm"), Visual.MaxBackCm);
	CheckFloat(TEXT("WeaponVisual.MaxUpCm"), Visual.MaxUpCm);
	CheckFloat(TEXT("WeaponVisual.MaxSideCm"), Visual.MaxSideCm);
	CheckFloat(TEXT("WeaponVisual.CameraPitchAlignment"), Visual.CameraPitchAlignment);
	CheckFloat(TEXT("WeaponVisual.CameraYawAlignment"), Visual.CameraYawAlignment);
	CheckFloat(TEXT("WeaponVisual.MaxAlignmentDegrees"), Visual.MaxAlignmentDegrees);
	CheckFloat(TEXT("WeaponVisual.BlendInTime"), Visual.BlendInTime);
	CheckFloat(TEXT("WeaponVisual.BlendOutTime"), Visual.BlendOutTime);
	CheckVector(TEXT("WeaponVisual.BackAxisBoneSpace"), Visual.BackAxisBoneSpace);
	CheckVector(TEXT("WeaponVisual.UpAxisBoneSpace"), Visual.UpAxisBoneSpace);
	CheckVector(TEXT("WeaponVisual.SideAxisBoneSpace"), Visual.SideAxisBoneSpace);
	CheckVector(TEXT("WeaponVisual.TargetBackAxisBoneSpace"), Visual.TargetBackAxisBoneSpace);
	CheckVector(TEXT("WeaponVisual.TargetUpAxisBoneSpace"), Visual.TargetUpAxisBoneSpace);
	CheckVector(TEXT("WeaponVisual.TargetSideAxisBoneSpace"), Visual.TargetSideAxisBoneSpace);
	CheckRotator(TEXT("WeaponVisual.PitchAxisBoneSpace"), Visual.PitchAxisBoneSpace);
	CheckRotator(TEXT("WeaponVisual.YawAxisBoneSpace"), Visual.YawAxisBoneSpace);
	CheckRotator(TEXT("WeaponVisual.RollAxisBoneSpace"), Visual.RollAxisBoneSpace);
	CheckRotator(TEXT("WeaponVisual.TargetPitchAxisBoneSpace"), Visual.TargetPitchAxisBoneSpace);
	CheckRotator(TEXT("WeaponVisual.TargetYawAxisBoneSpace"), Visual.TargetYawAxisBoneSpace);
	CheckRotator(TEXT("WeaponVisual.TargetRollAxisBoneSpace"), Visual.TargetRollAxisBoneSpace);

	// --- 曲线键与采样 ---
	CheckCurveKeys(TEXT("VerticalKickCurve"), Profile.VerticalKickCurve, Errors);
	CheckCurveKeys(TEXT("RecoveryCurve"), Profile.RecoveryCurve, Errors);
	CheckCurveKeys(TEXT("LiftCurve"), Profile.LiftCurve, Errors);
	CheckCurveKeys(TEXT("ReboundCurve"), Profile.ReboundCurve, Errors);
	CheckCurveKeys(TEXT("RollShake_AmplitudeCurve"), Profile.RollShake_AmplitudeCurve, Errors);
	CheckCurveKeys(TEXT("RollShake_SegmentScaleCurve"), Profile.RollShake_SegmentScaleCurve, Errors);
	CheckCurveKeys(TEXT("RollShake_PeriodScaleCurve"), Profile.RollShake_PeriodScaleCurve, Errors);

	const int32 MaxShotIndex = FMath::Max(Profile.PatternPoints.Num(), FMath::Max(0, Profile.PatternLength));
	for (int32 ShotIndex = 0; ShotIndex <= MaxShotIndex; ++ShotIndex)
	{
		const double Sample = static_cast<double>(Profile.GetVerticalKickCurveScale(ShotIndex));
		if (!IsFiniteValue(Sample))
		{
			Errors.Add(FString::Printf(TEXT("%s VerticalKickCurve sample at shot %d is not finite (NaN/Inf)"), *Prefix, ShotIndex));
		}
	}

	return Errors.Num() == StartErrorCount;
}

// ---------------------------------------------------------------------------
// 正向构建
// ---------------------------------------------------------------------------

bool FLyraRecoilPatternAdapter::BuildCumulativeFor(const ULyraRecoilProfile& Profile, const FLyraRecoilPatternData& Data,
	TArray<FVector2D>& OutPoints, TArray<FString>& Errors)
{
	const int32 StartErrorCount = Errors.Num();
	OutPoints.Reset();

	if (!ValidateDataShape(Data, Errors))
	{
		return false;
	}

	const double Horizontal = static_cast<double>(Profile.RecoilPerShot_Horizontal);
	const double Vertical = static_cast<double>(Profile.RecoilPerShot_Vertical);
	if (!IsFiniteValue(Horizontal) || !IsFiniteValue(Vertical))
	{
		Errors.Add(TEXT("RecoilPerShot_Vertical/Horizontal is not finite (NaN/Inf)"));
		return false;
	}

	TArray<FVector2D> Built;
	Built.Reserve(Data.PatternLength);

	double Yaw = 0.0;
	double Pitch = 0.0;
	for (int32 Index = 0; Index < Data.PatternLength; ++Index)
	{
		const double CurveScale = static_cast<double>(Profile.GetVerticalKickCurveScale(Index));
		if (!IsFiniteValue(CurveScale))
		{
			Errors.Add(FString::Printf(TEXT("VerticalKickCurve sample at shot %d is not finite (NaN/Inf)"), Index));
			return false;
		}

		const double DeltaYaw = Horizontal * static_cast<double>(Data.Points[Index].X);
		const double DeltaPitch = Vertical * static_cast<double>(Data.Points[Index].Y) * CurveScale;
		if (!IsFiniteValue(DeltaYaw) || !IsFiniteValue(DeltaPitch))
		{
			Errors.Add(FString::Printf(TEXT("Shot %d: per-shot kick is not finite (overflow or NaN)"), Index));
			return false;
		}

		Yaw += DeltaYaw;
		Pitch += DeltaPitch;
		if (!IsFiniteValue(Yaw) || !IsFiniteValue(Pitch))
		{
			Errors.Add(FString::Printf(TEXT("Shot %d: cumulative offset is not finite (overflow)"), Index));
			return false;
		}

		Built.Add(FVector2D(Yaw, Pitch));
	}

	OutPoints = MoveTemp(Built);
	return Errors.Num() == StartErrorCount;
}

bool FLyraRecoilPatternAdapter::BuildCumulative(const ULyraRecoilProfile& Profile, TArray<FVector2D>& OutPoints,
	TArray<FString>& Errors)
{
	return BuildCumulativeFor(Profile, Read(Profile), OutPoints, Errors);
}

// ---------------------------------------------------------------------------
// 累计点 → 归一化字段
// ---------------------------------------------------------------------------

bool FLyraRecoilPatternAdapter::MoveCumulative(const ULyraRecoilProfile& Profile, const TArray<FVector2D>& Targets,
	FLyraRecoilPatternData& OutData, TArray<FString>& Errors)
{
	FLyraRecoilConversionReport Report;
	return MoveCumulative(Profile, Targets, OutData, Report, Errors);
}

bool FLyraRecoilPatternAdapter::MoveCumulative(const ULyraRecoilProfile& Profile, const TArray<FVector2D>& Targets,
	FLyraRecoilPatternData& OutData, FLyraRecoilConversionReport& OutReport, TArray<FString>& Errors)
{
	const int32 StartErrorCount = Errors.Num();
	OutReport = FLyraRecoilConversionReport();

	const FLyraRecoilPatternData Source = Read(Profile);

	// 失败时保持源数据，绝不半写入。
	OutData = Source;

	if (!ValidateDataShape(Source, Errors))
	{
		return false;
	}

	TArray<FVector2D> Original;
	if (!BuildCumulative(Profile, Original, Errors))
	{
		return false;
	}

	const int32 FixedLength = Source.PatternLength;
	if (Targets.Num() != FixedLength)
	{
		Errors.Add(FString::Printf(TEXT("MoveCumulative expects %d target points (PatternLength), got %d"),
			FixedLength, Targets.Num()));
		return false;
	}

	const double Horizontal = static_cast<double>(Profile.RecoilPerShot_Horizontal);
	const double Vertical = static_cast<double>(Profile.RecoilPerShot_Vertical);
	if (!IsFiniteValue(Horizontal) || !IsFiniteValue(Vertical))
	{
		Errors.Add(TEXT("RecoilPerShot_Vertical/Horizontal is not finite (NaN/Inf)"));
		return false;
	}
	if (Horizontal < 0.0 || Vertical < 0.0)
	{
		Errors.Add(TEXT("Base strengths must be non-negative before the pattern can be inverted; fix the asset first"));
		return false;
	}

	for (int32 Index = 0; Index < FixedLength; ++Index)
	{
		if (!IsFiniteValue(Targets[Index].X) || !IsFiniteValue(Targets[Index].Y))
		{
			Errors.Add(FString::Printf(TEXT("Targets[%d] is not finite (NaN/Inf); refusing the whole candidate"), Index));
			return false;
		}
	}

	TArray<FRecoilPatternPoint> Candidate = Source.Points;

	for (int32 Index = 0; Index < FixedLength; ++Index)
	{
		const double OriginalPrevYaw = (Index > 0) ? Original[Index - 1].X : 0.0;
		const double OriginalPrevPitch = (Index > 0) ? Original[Index - 1].Y : 0.0;
		const double TargetPrevYaw = (Index > 0) ? Targets[Index - 1].X : 0.0;
		const double TargetPrevPitch = (Index > 0) ? Targets[Index - 1].Y : 0.0;

		const double OriginalDeltaYaw = Original[Index].X - OriginalPrevYaw;
		const double OriginalDeltaPitch = Original[Index].Y - OriginalPrevPitch;
		const double TargetDeltaYaw = Targets[Index].X - TargetPrevYaw;
		const double TargetDeltaPitch = Targets[Index].Y - TargetPrevPitch;

		// ---- 水平轴 ----
		if (!FMath::IsNearlyEqual(TargetDeltaYaw, OriginalDeltaYaw, PointUnchangedTolerance))
		{
			if (Horizontal <= DenominatorEpsilon)
			{
				if (!FMath::IsNearlyZero(TargetDeltaYaw, ZeroAngleTolerance))
				{
					Errors.Add(FString::Printf(
						TEXT("Shot %d: horizontal base strength H=%.8f is not invertible; non-zero horizontal target %.6f deg cannot be expressed"),
						Index, Horizontal, TargetDeltaYaw));
					return false;
				}
				// 目标水平增量为零且分母不可逆：保留原始隐藏值（不写成 0）。
			}
			else
			{
				const double RawX = TargetDeltaYaw / Horizontal;
				if (!CheckNormalizedValue(Index, TEXT("X"), RawX, -1.0, 1.0, OutReport, Errors))
				{
					return false;
				}
				Candidate[Index].X = static_cast<float>(FMath::Clamp(RawX, -1.0, 1.0));
			}
		}

		// ---- 垂直轴 ----
		const double CurveScale = static_cast<double>(Profile.GetVerticalKickCurveScale(Index));
		if (!IsFiniteValue(CurveScale))
		{
			Errors.Add(FString::Printf(TEXT("VerticalKickCurve sample at shot %d is not finite (NaN/Inf)"), Index));
			return false;
		}

		const double VerticalDenominator = Vertical * CurveScale;
		if (!IsFiniteValue(VerticalDenominator))
		{
			Errors.Add(FString::Printf(TEXT("Shot %d: vertical denominator V*curve is not finite (overflow)"), Index));
			return false;
		}

		if (!FMath::IsNearlyEqual(TargetDeltaPitch, OriginalDeltaPitch, PointUnchangedTolerance))
		{
			if (FMath::Abs(VerticalDenominator) <= DenominatorEpsilon)
			{
				if (!FMath::IsNearlyZero(TargetDeltaPitch, ZeroAngleTolerance))
				{
					Errors.Add(FString::Printf(
						TEXT("Shot %d: vertical denominator V*curve=%.8f is not invertible; non-zero vertical target %.6f deg cannot be expressed"),
						Index, VerticalDenominator, TargetDeltaPitch));
					return false;
				}
				// 目标垂直增量为零且分母不可逆：保留原始隐藏值。
			}
			else
			{
				if (VerticalDenominator < 0.0 && TargetDeltaPitch > ZeroAngleTolerance)
				{
					Errors.Add(FString::Printf(
						TEXT("Shot %d: negative curve scale %.6f makes this a downward kick; upward target %.6f deg is not supported (the curve must not be forced to 1)"),
						Index, CurveScale, TargetDeltaPitch));
					return false;
				}

				const double RawY = TargetDeltaPitch / VerticalDenominator;
				if (!CheckNormalizedValue(Index, TEXT("Y"), RawY, 0.0, 1.0, OutReport, Errors))
				{
					return false;
				}
				Candidate[Index].Y = static_cast<float>(FMath::Clamp(RawY, 0.0, 1.0));
			}
		}
	}

	if (!RebuildAndVerify(Profile, Candidate, FixedLength, Targets, Horizontal, Vertical, OutReport, Errors))
	{
		return false;
	}

	OutData.Points = MoveTemp(Candidate);
	OutData.PatternLength = FixedLength;
	return Errors.Num() == StartErrorCount;
}

// ---------------------------------------------------------------------------
// 结构命令
// ---------------------------------------------------------------------------

bool FLyraRecoilPatternAdapter::Insert(const FLyraRecoilPatternData& Source, int32 Index,
	const TArray<FRecoilPatternPoint>& NewPoints, bool bFixedAtBoundary,
	FLyraRecoilPatternData& OutData, TArray<FString>& Errors)
{
	const int32 StartErrorCount = Errors.Num();
	OutData = Source;

	if (!ValidateDataShape(Source, Errors))
	{
		return false;
	}

	const int32 Count = Source.Points.Num();
	const int32 FixedLength = Source.PatternLength;

	if (Index < 0 || Index > Count)
	{
		Errors.Add(FString::Printf(TEXT("Insert index %d is out of range [0, %d]"), Index, Count));
		return false;
	}

	for (int32 NewIndex = 0; NewIndex < NewPoints.Num(); ++NewIndex)
	{
		const FRecoilPatternPoint& Point = NewPoints[NewIndex];
		if (!IsFiniteValue(static_cast<double>(Point.X)) || !IsFiniteValue(static_cast<double>(Point.Y)))
		{
			Errors.Add(FString::Printf(TEXT("InsertedPoints[%d] is not finite (NaN/Inf)"), NewIndex));
			return false;
		}
		if (Point.X < -1.0f || Point.X > 1.0f)
		{
			Errors.Add(FString::Printf(TEXT("InsertedPoints[%d].X = %.6f is outside [-1, 1]"), NewIndex, static_cast<double>(Point.X)));
			return false;
		}
		if (Point.Y < 0.0f || Point.Y > 1.0f)
		{
			Errors.Add(FString::Printf(TEXT("InsertedPoints[%d].Y = %.6f is outside [0, 1]"), NewIndex, static_cast<double>(Point.Y)));
			return false;
		}
	}

	const int32 InsertCount = NewPoints.Num();

	TArray<FRecoilPatternPoint> Points;
	Points.Reserve(Count + InsertCount);
	for (int32 SourceIndex = 0; SourceIndex < Index; ++SourceIndex)
	{
		Points.Add(Source.Points[SourceIndex]);
	}
	for (const FRecoilPatternPoint& Point : NewPoints)
	{
		Points.Add(Point);
	}
	for (int32 SourceIndex = Index; SourceIndex < Count; ++SourceIndex)
	{
		Points.Add(Source.Points[SourceIndex]);
	}

	// §07：Index < L → 固定段增长；Index == L → 默认尾段（显式选项才归入固定段）；Index > L → 不变。
	int32 NewFixedLength = FixedLength;
	if (InsertCount > 0)
	{
		if (Index < FixedLength)
		{
			NewFixedLength = FixedLength + InsertCount;
		}
		else if (Index == FixedLength && bFixedAtBoundary)
		{
			NewFixedLength = FixedLength + InsertCount;
		}
	}

	OutData.Points = MoveTemp(Points);
	OutData.PatternLength = NewFixedLength;
	return Errors.Num() == StartErrorCount;
}

bool FLyraRecoilPatternAdapter::Delete(const FLyraRecoilPatternData& Source, const TArray<int32>& Indices,
	FLyraRecoilPatternData& OutData, TArray<FString>& Errors)
{
	const int32 StartErrorCount = Errors.Num();
	OutData = Source;

	if (!ValidateDataShape(Source, Errors))
	{
		return false;
	}

	const int32 Count = Source.Points.Num();
	const int32 FixedLength = Source.PatternLength;

	TSet<int32> UniqueIndices;
	for (int32 Index : Indices)
	{
		if (Index < 0 || Index >= Count)
		{
			Errors.Add(FString::Printf(TEXT("Delete index %d is out of range [0, %d]"), Index, Count - 1));
			return false;
		}
		UniqueIndices.Add(Index);
	}

	TArray<FRecoilPatternPoint> Points;
	Points.Reserve(Count - UniqueIndices.Num());

	int32 RemovedFixedCount = 0;
	for (int32 SourceIndex = 0; SourceIndex < Count; ++SourceIndex)
	{
		if (UniqueIndices.Contains(SourceIndex))
		{
			if (SourceIndex < FixedLength)
			{
				++RemovedFixedCount;
			}
			continue;
		}
		Points.Add(Source.Points[SourceIndex]);
	}

	OutData.Points = MoveTemp(Points);
	OutData.PatternLength = FixedLength - RemovedFixedCount;
	return Errors.Num() == StartErrorCount;
}

bool FLyraRecoilPatternAdapter::Reorder(const FLyraRecoilPatternData& Source, const TArray<int32>& Order,
	FLyraRecoilPatternData& OutData, TArray<FString>& Errors)
{
	const int32 StartErrorCount = Errors.Num();
	OutData = Source;

	if (!ValidateDataShape(Source, Errors))
	{
		return false;
	}

	const int32 Count = Source.Points.Num();
	if (Order.Num() != Count)
	{
		Errors.Add(FString::Printf(TEXT("Reorder order has %d entries but the pattern has %d points"), Order.Num(), Count));
		return false;
	}

	TSet<int32> Seen;
	Seen.Reserve(Count);
	for (int32 OrderIndex = 0; OrderIndex < Order.Num(); ++OrderIndex)
	{
		const int32 SourceIndex = Order[OrderIndex];
		if (SourceIndex < 0 || SourceIndex >= Count)
		{
			Errors.Add(FString::Printf(TEXT("Reorder entry %d = %d is out of range [0, %d]"), OrderIndex, SourceIndex, Count - 1));
			return false;
		}
		if (Seen.Contains(SourceIndex))
		{
			Errors.Add(FString::Printf(TEXT("Reorder entry %d = %d is duplicated; the order must be a permutation"), OrderIndex, SourceIndex));
			return false;
		}
		Seen.Add(SourceIndex);
	}

	TArray<FRecoilPatternPoint> Points;
	Points.Reserve(Count);
	for (int32 SourceIndex : Order)
	{
		Points.Add(Source.Points[SourceIndex]);
	}

	OutData.Points = MoveTemp(Points);
	// N 与 L 的数值不变，固定段仍是新顺序的前 L 个节点。
	OutData.PatternLength = Source.PatternLength;
	return Errors.Num() == StartErrorCount;
}

bool FLyraRecoilPatternAdapter::SetPatternLength(const FLyraRecoilPatternData& Source, int32 NewLength,
	FLyraRecoilPatternData& OutData, TArray<FString>& Errors)
{
	const int32 StartErrorCount = Errors.Num();
	OutData = Source;

	if (!ValidateDataShape(Source, Errors))
	{
		return false;
	}

	if (NewLength < 0 || NewLength > Source.Points.Num())
	{
		Errors.Add(FString::Printf(TEXT("PatternLength %d is out of range [0, %d]"), NewLength, Source.Points.Num()));
		return false;
	}

	OutData.PatternLength = NewLength;
	return Errors.Num() == StartErrorCount;
}

bool FLyraRecoilPatternAdapter::Equal(const FLyraRecoilPatternData& A, const FLyraRecoilPatternData& B)
{
	if (A.PatternLength != B.PatternLength || A.Points.Num() != B.Points.Num())
	{
		return false;
	}

	for (int32 Index = 0; Index < A.Points.Num(); ++Index)
	{
		if (A.Points[Index].X != B.Points[Index].X || A.Points[Index].Y != B.Points[Index].Y)
		{
			return false;
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// 剪贴板
// ---------------------------------------------------------------------------

const TCHAR* FLyraRecoilPatternAdapter::GetClipboardFormatName()
{
	return TEXT("LyraRecoilClipboard");
}

int32 FLyraRecoilPatternAdapter::GetClipboardFormatVersion()
{
	return 1;
}

FString FLyraRecoilPatternAdapter::Copy(const ULyraRecoilProfile& Profile, const TArray<int32>& Indices)
{
	const int32 Count = Profile.PatternPoints.Num();

	TSet<int32> UniqueIndices;
	for (int32 Index : Indices)
	{
		if (Index < 0 || Index >= Count)
		{
			return FString();
		}
		UniqueIndices.Add(Index);
	}

	TArray<int32> SortedIndices = UniqueIndices.Array();
	SortedIndices.Sort();

	if (SortedIndices.Num() == 0 || SortedIndices.Num() > MaxClipboardPoints)
	{
		return FString();
	}

	const double Horizontal = static_cast<double>(Profile.RecoilPerShot_Horizontal);
	const double Vertical = static_cast<double>(Profile.RecoilPerShot_Vertical);
	if (!IsFiniteValue(Horizontal) || !IsFiniteValue(Vertical))
	{
		return FString();
	}

	TArray<TSharedPtr<FJsonValue>> IndexArray;
	TArray<TSharedPtr<FJsonValue>> PointArray;
	IndexArray.Reserve(SortedIndices.Num());
	PointArray.Reserve(SortedIndices.Num());

	for (int32 SourceIndex : SortedIndices)
	{
		const FRecoilPatternPoint& Point = Profile.PatternPoints[SourceIndex];
		if (!IsFiniteValue(static_cast<double>(Point.X)) || !IsFiniteValue(static_cast<double>(Point.Y)))
		{
			return FString();
		}

		const double CurveScale = static_cast<double>(Profile.GetVerticalKickCurveScale(SourceIndex));
		const double YawDegrees = Horizontal * static_cast<double>(Point.X);
		const double PitchDegrees = Vertical * static_cast<double>(Point.Y) * CurveScale;
		if (!IsFiniteValue(CurveScale) || !IsFiniteValue(YawDegrees) || !IsFiniteValue(PitchDegrees))
		{
			return FString();
		}

		IndexArray.Add(MakeShared<FJsonValueNumber>(static_cast<double>(SourceIndex)));

		const TSharedRef<FJsonObject> PointObject = MakeShared<FJsonObject>();
		PointObject->SetNumberField(TEXT("x"), static_cast<double>(Point.X));
		PointObject->SetNumberField(TEXT("y"), static_cast<double>(Point.Y));
		PointObject->SetNumberField(TEXT("yawDegrees"), YawDegrees);
		PointObject->SetNumberField(TEXT("pitchDegrees"), PitchDegrees);
		PointObject->SetNumberField(TEXT("verticalScale"), CurveScale);
		PointArray.Add(MakeShared<FJsonValueObject>(PointObject));
	}

	const TSharedRef<FJsonObject> SourceObject = MakeShared<FJsonObject>();
	SourceObject->SetNumberField(TEXT("horizontal"), Horizontal);
	SourceObject->SetNumberField(TEXT("vertical"), Vertical);
	SourceObject->SetStringField(TEXT("assetPath"), Profile.GetPathName());

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("format"), GetClipboardFormatName());
	Root->SetNumberField(TEXT("version"), static_cast<double>(GetClipboardFormatVersion()));
	Root->SetField(TEXT("source"), MakeShared<FJsonValueObject>(SourceObject));
	Root->SetArrayField(TEXT("indices"), IndexArray);
	Root->SetArrayField(TEXT("points"), PointArray);

	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Root, Writer, /*bCloseWriter=*/ true))
	{
		return FString();
	}

	if (FTCHARToUTF8(*Serialized).Length() > MaxClipboardBytes)
	{
		return FString();
	}

	return Serialized;
}

bool FLyraRecoilPatternAdapter::ParseClipboardEx(const FString& Text, FLyraRecoilClipboardData& OutData,
	TArray<FString>& Errors)
{
	const int32 StartErrorCount = Errors.Num();
	OutData = FLyraRecoilClipboardData();

	if (Text.IsEmpty())
	{
		Errors.Add(TEXT("Clipboard payload is empty"));
		return false;
	}

	if (FTCHARToUTF8(*Text).Length() > MaxClipboardBytes)
	{
		Errors.Add(FString::Printf(TEXT("Clipboard payload exceeds the %d byte editor limit"), MaxClipboardBytes));
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		Errors.Add(TEXT("Clipboard payload is not a valid JSON object"));
		return false;
	}

	// --- 严格标识与版本 ---
	const TSharedPtr<FJsonValue> FormatValue = Root->TryGetField(TEXT("format"));
	if (!FormatValue.IsValid() || FormatValue->Type != EJson::String ||
		FormatValue->AsString() != GetClipboardFormatName())
	{
		Errors.Add(FString::Printf(TEXT("Clipboard format marker is missing or unknown (expected '%s'); refusing the whole payload"),
			GetClipboardFormatName()));
		return false;
	}

	const TSharedPtr<FJsonValue> VersionValue = Root->TryGetField(TEXT("version"));
	if (!VersionValue.IsValid() || VersionValue->Type != EJson::Number)
	{
		Errors.Add(TEXT("Clipboard version field is missing or not a JSON number; refusing the whole payload"));
		return false;
	}

	const double VersionNumber = VersionValue->AsNumber();
	if (VersionNumber != static_cast<double>(GetClipboardFormatVersion()))
	{
		Errors.Add(FString::Printf(TEXT("Clipboard version %.0f is unsupported (expected %d); refusing the whole payload"),
			VersionNumber, GetClipboardFormatVersion()));
		return false;
	}

	// --- 点数组 ---
	const TArray<TSharedPtr<FJsonValue>>* PointArray = nullptr;
	if (!Root->TryGetArrayField(TEXT("points"), PointArray) || PointArray == nullptr)
	{
		Errors.Add(TEXT("Clipboard points array is missing"));
		return false;
	}

	if (PointArray->Num() == 0)
	{
		Errors.Add(TEXT("Clipboard points array is empty"));
		return false;
	}

	if (PointArray->Num() > MaxClipboardPoints)
	{
		Errors.Add(FString::Printf(TEXT("Clipboard carries %d points which exceeds the %d point editor limit"),
			PointArray->Num(), MaxClipboardPoints));
		return false;
	}

	bool bAnySourceAngles = false;
	bool bAllSourceAngles = true;

	TArray<FRecoilPatternPoint> Points;
	Points.Reserve(PointArray->Num());

	for (int32 PointIndex = 0; PointIndex < PointArray->Num(); ++PointIndex)
	{
		const TSharedPtr<FJsonValue>& PointValue = (*PointArray)[PointIndex];
		if (!PointValue.IsValid() || PointValue->Type != EJson::Object)
		{
			Errors.Add(FString::Printf(TEXT("Clipboard points[%d] is not a JSON object"), PointIndex));
			return false;
		}

		const TSharedPtr<FJsonObject>& PointObject = PointValue->AsObject();
		if (!PointObject.IsValid())
		{
			Errors.Add(FString::Printf(TEXT("Clipboard points[%d] is not a JSON object"), PointIndex));
			return false;
		}

		double X = 0.0;
		double Y = 0.0;
		if (!ReadJsonNumber(PointObject, TEXT("x"), X))
		{
			Errors.Add(FString::Printf(TEXT("Clipboard points[%d].x is missing or not a JSON number"), PointIndex));
			return false;
		}
		if (!ReadJsonNumber(PointObject, TEXT("y"), Y))
		{
			Errors.Add(FString::Printf(TEXT("Clipboard points[%d].y is missing or not a JSON number"), PointIndex));
			return false;
		}
		if (!IsFiniteValue(X) || !IsFiniteValue(Y))
		{
			Errors.Add(FString::Printf(TEXT("Clipboard points[%d] is not finite (NaN/Inf)"), PointIndex));
			return false;
		}

		FRecoilPatternPoint Point;
		Point.X = static_cast<float>(X);
		Point.Y = static_cast<float>(Y);
		Points.Add(Point);

		double YawDegrees = 0.0;
		double PitchDegrees = 0.0;
		double VerticalScale = 0.0;
		const bool bHasYaw = ReadJsonNumber(PointObject, TEXT("yawDegrees"), YawDegrees);
		const bool bHasPitch = ReadJsonNumber(PointObject, TEXT("pitchDegrees"), PitchDegrees);
		const bool bHasScale = ReadJsonNumber(PointObject, TEXT("verticalScale"), VerticalScale);

		if (bHasYaw != bHasPitch)
		{
			Errors.Add(FString::Printf(TEXT("Clipboard points[%d] carries only one of yawDegrees/pitchDegrees"), PointIndex));
			return false;
		}

		if (bHasYaw)
		{
			if (!IsFiniteValue(YawDegrees) || !IsFiniteValue(PitchDegrees) ||
				(bHasScale && !IsFiniteValue(VerticalScale)))
			{
				Errors.Add(FString::Printf(TEXT("Clipboard points[%d] source angles are not finite (NaN/Inf)"), PointIndex));
				return false;
			}
			bAnySourceAngles = true;
		}
		else
		{
			bAllSourceAngles = false;
		}

		OutData.SourceYawDegrees.Add(YawDegrees);
		OutData.SourcePitchDegrees.Add(PitchDegrees);
		OutData.SourceVerticalScales.Add(VerticalScale);
	}

	if (bAnySourceAngles && !bAllSourceAngles)
	{
		Errors.Add(TEXT("Clipboard source angles are present on only some points; refusing the whole payload"));
		return false;
	}

	OutData.Points = MoveTemp(Points);
	OutData.bHasSourceAngles = bAnySourceAngles && bAllSourceAngles;

	// --- 可选的源序号 ---
	const TArray<TSharedPtr<FJsonValue>>* IndexArray = nullptr;
	if (Root->TryGetArrayField(TEXT("indices"), IndexArray) && IndexArray != nullptr)
	{
		if (IndexArray->Num() != PointArray->Num())
		{
			Errors.Add(FString::Printf(TEXT("Clipboard indices count %d does not match points count %d"),
				IndexArray->Num(), PointArray->Num()));
			return false;
		}

		for (int32 IndexValue = 0; IndexValue < IndexArray->Num(); ++IndexValue)
		{
			const TSharedPtr<FJsonValue>& Value = (*IndexArray)[IndexValue];
			if (!Value.IsValid() || Value->Type != EJson::Number)
			{
				Errors.Add(FString::Printf(TEXT("Clipboard indices[%d] is not a JSON number"), IndexValue));
				return false;
			}

			const double Number = Value->AsNumber();
			if (!IsFiniteValue(Number) || Number < 0.0 || FMath::FloorToDouble(Number) != Number)
			{
				Errors.Add(FString::Printf(TEXT("Clipboard indices[%d] = %.6f is not a non-negative integer"), IndexValue, Number));
				return false;
			}

			OutData.SourceIndices.Add(static_cast<int32>(Number));
		}
	}

	// --- 可选的源强度 ---
	const TSharedPtr<FJsonObject>* SourceObject = nullptr;
	if (Root->TryGetObjectField(TEXT("source"), SourceObject) && SourceObject != nullptr && SourceObject->IsValid())
	{
		double SourceHorizontal = 0.0;
		double SourceVertical = 0.0;
		if (ReadJsonNumber(*SourceObject, TEXT("horizontal"), SourceHorizontal) && IsFiniteValue(SourceHorizontal))
		{
			OutData.SourceHorizontal = SourceHorizontal;
		}
		if (ReadJsonNumber(*SourceObject, TEXT("vertical"), SourceVertical) && IsFiniteValue(SourceVertical))
		{
			OutData.SourceVertical = SourceVertical;
		}
	}

	return Errors.Num() == StartErrorCount;
}

bool FLyraRecoilPatternAdapter::ParseClipboard(const FString& Text, TArray<FRecoilPatternPoint>& OutPoints,
	TArray<FString>& Errors)
{
	OutPoints.Reset();

	FLyraRecoilClipboardData Data;
	if (!ParseClipboardEx(Text, Data, Errors))
	{
		return false;
	}

	OutPoints = MoveTemp(Data.Points);
	return true;
}

bool FLyraRecoilPatternAdapter::ConvertClipboardAngles(const ULyraRecoilProfile& TargetProfile,
	const FLyraRecoilClipboardData& Clipboard, int32 FirstTargetIndex,
	TArray<FRecoilPatternPoint>& OutPoints, TArray<FString>& Errors)
{
	const int32 StartErrorCount = Errors.Num();
	OutPoints.Reset();

	const int32 Count = Clipboard.Points.Num();
	if (!Clipboard.bHasSourceAngles ||
		Clipboard.SourceYawDegrees.Num() != Count ||
		Clipboard.SourcePitchDegrees.Num() != Count ||
		Clipboard.SourceVerticalScales.Num() != Count)
	{
		Errors.Add(TEXT("Clipboard does not carry complete source per-shot angles; use the normalized paste mode instead"));
		return false;
	}

	if (Count == 0)
	{
		Errors.Add(TEXT("Clipboard carries no points"));
		return false;
	}

	if (FirstTargetIndex < 0)
	{
		Errors.Add(FString::Printf(TEXT("FirstTargetIndex %d must be non-negative"), FirstTargetIndex));
		return false;
	}

	const double Horizontal = static_cast<double>(TargetProfile.RecoilPerShot_Horizontal);
	const double Vertical = static_cast<double>(TargetProfile.RecoilPerShot_Vertical);
	if (!IsFiniteValue(Horizontal) || !IsFiniteValue(Vertical))
	{
		Errors.Add(TEXT("Target RecoilPerShot_Vertical/Horizontal is not finite (NaN/Inf)"));
		return false;
	}
	if (Horizontal < 0.0 || Vertical < 0.0)
	{
		Errors.Add(TEXT("Target base strengths must be non-negative before angle paste can invert them"));
		return false;
	}

	FLyraRecoilConversionReport Report;
	TArray<FRecoilPatternPoint> Built;
	Built.Reserve(Count);

	for (int32 ClipIndex = 0; ClipIndex < Count; ++ClipIndex)
	{
		const int32 TargetIndex = FirstTargetIndex + ClipIndex;
		const double CurveScale = static_cast<double>(TargetProfile.GetVerticalKickCurveScale(TargetIndex));
		if (!IsFiniteValue(CurveScale))
		{
			Errors.Add(FString::Printf(TEXT("Target VerticalKickCurve sample at shot %d is not finite (NaN/Inf)"), TargetIndex));
			return false;
		}

		const double YawDegrees = Clipboard.SourceYawDegrees[ClipIndex];
		const double PitchDegrees = Clipboard.SourcePitchDegrees[ClipIndex];
		if (!IsFiniteValue(YawDegrees) || !IsFiniteValue(PitchDegrees))
		{
			Errors.Add(FString::Printf(TEXT("Clipboard points[%d] source angles are not finite (NaN/Inf)"), ClipIndex));
			return false;
		}

		FRecoilPatternPoint Point;
		Point.X = Clipboard.Points[ClipIndex].X;
		Point.Y = Clipboard.Points[ClipIndex].Y;

		if (Horizontal <= DenominatorEpsilon)
		{
			if (!FMath::IsNearlyZero(YawDegrees, ZeroAngleTolerance))
			{
				Errors.Add(FString::Printf(
					TEXT("Shot %d: target horizontal base strength H=%.8f is not invertible; source angle %.6f deg cannot be expressed"),
					TargetIndex, Horizontal, YawDegrees));
				return false;
			}
			// 目标 H 不可逆且源角度为零：保留剪贴板的原始隐藏值。
		}
		else
		{
			const double RawX = YawDegrees / Horizontal;
			if (!CheckNormalizedValue(TargetIndex, TEXT("X"), RawX, -1.0, 1.0, Report, Errors))
			{
				return false;
			}
			Point.X = static_cast<float>(FMath::Clamp(RawX, -1.0, 1.0));
		}

		const double VerticalDenominator = Vertical * CurveScale;
		if (!IsFiniteValue(VerticalDenominator))
		{
			Errors.Add(FString::Printf(TEXT("Shot %d: target vertical denominator V*curve is not finite (overflow)"), TargetIndex));
			return false;
		}

		if (FMath::Abs(VerticalDenominator) <= DenominatorEpsilon)
		{
			if (!FMath::IsNearlyZero(PitchDegrees, ZeroAngleTolerance))
			{
				Errors.Add(FString::Printf(
					TEXT("Shot %d: target vertical denominator V*curve=%.8f is not invertible; source angle %.6f deg cannot be expressed"),
					TargetIndex, VerticalDenominator, PitchDegrees));
				return false;
			}
			// 目标曲线为零且源角度为零：保留剪贴板的原始隐藏值。
		}
		else
		{
			if (VerticalDenominator < 0.0 && PitchDegrees > ZeroAngleTolerance)
			{
				Errors.Add(FString::Printf(
					TEXT("Shot %d: target curve scale %.6f is negative; upward source angle %.6f deg is not supported"),
					TargetIndex, CurveScale, PitchDegrees));
				return false;
			}

			const double RawY = PitchDegrees / VerticalDenominator;
			if (!CheckNormalizedValue(TargetIndex, TEXT("Y"), RawY, 0.0, 1.0, Report, Errors))
			{
				return false;
			}
			Point.Y = static_cast<float>(FMath::Clamp(RawY, 0.0, 1.0));
		}

		Built.Add(Point);
	}

	// 正向重建校验：目标强度下重建的逐发角度必须与源角度一致。
	for (int32 ClipIndex = 0; ClipIndex < Count; ++ClipIndex)
	{
		const int32 TargetIndex = FirstTargetIndex + ClipIndex;
		const double CurveScale = static_cast<double>(TargetProfile.GetVerticalKickCurveScale(TargetIndex));
		const double RebuiltYaw = Horizontal * static_cast<double>(Built[ClipIndex].X);
		const double RebuiltPitch = Vertical * static_cast<double>(Built[ClipIndex].Y) * CurveScale;
		const double TargetYaw = Clipboard.SourceYawDegrees[ClipIndex];
		const double TargetPitch = Clipboard.SourcePitchDegrees[ClipIndex];

		if (!IsFiniteValue(RebuiltYaw) || !IsFiniteValue(RebuiltPitch))
		{
			Errors.Add(FString::Printf(TEXT("Shot %d: angle paste rebuild is not finite"), TargetIndex));
			return false;
		}

		const double ErrorYaw = FMath::Abs(RebuiltYaw - TargetYaw);
		const double ErrorPitch = FMath::Abs(RebuiltPitch - TargetPitch);
		Report.MaxAngleRebuildError = FMath::Max(Report.MaxAngleRebuildError, FMath::Max(ErrorYaw, ErrorPitch));

		if (ErrorYaw > GetAngleTolerance(TargetYaw) || ErrorPitch > GetAngleTolerance(TargetPitch))
		{
			Errors.Add(FString::Printf(
				TEXT("Shot %d: angle paste rebuild error (yaw %.8f, pitch %.8f) exceeds tolerance after float conversion"),
				TargetIndex, ErrorYaw, ErrorPitch));
			return false;
		}
	}

	OutPoints = MoveTemp(Built);
	return Errors.Num() == StartErrorCount;
}

// ---------------------------------------------------------------------------
// 稳定排序
// ---------------------------------------------------------------------------

bool FLyraRecoilPatternAdapter::MakeStablePitchOrder(const ULyraRecoilProfile& Profile, TArray<int32>& OutOrder,
	TArray<FString>& Errors)
{
	const int32 StartErrorCount = Errors.Num();
	OutOrder.Reset();

	const FLyraRecoilPatternData Source = Read(Profile);
	if (!ValidateDataShape(Source, Errors))
	{
		return false;
	}

	const int32 Count = Source.Points.Num();
	const double Vertical = static_cast<double>(Profile.RecoilPerShot_Vertical);
	if (!IsFiniteValue(Vertical))
	{
		Errors.Add(TEXT("RecoilPerShot_Vertical is not finite (NaN/Inf)"));
		return false;
	}

	TArray<double> CumulativePitch;
	CumulativePitch.SetNum(Count);

	double Pitch = 0.0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const double CurveScale = static_cast<double>(Profile.GetVerticalKickCurveScale(Index));
		if (!IsFiniteValue(CurveScale))
		{
			Errors.Add(FString::Printf(TEXT("VerticalKickCurve sample at shot %d is not finite (NaN/Inf)"), Index));
			return false;
		}

		Pitch += Vertical * static_cast<double>(Source.Points[Index].Y) * CurveScale;
		if (!IsFiniteValue(Pitch))
		{
			Errors.Add(FString::Printf(TEXT("Shot %d: cumulative pitch is not finite (overflow)"), Index));
			return false;
		}

		CumulativePitch[Index] = Pitch;
	}

	OutOrder.SetNum(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		OutOrder[Index] = Index;
	}

	// 稳定排序：累计 Pitch 升序，同值保持原来的发序号顺序。
	OutOrder.StableSort([&CumulativePitch](int32 Left, int32 Right)
	{
		return CumulativePitch[Left] < CumulativePitch[Right];
	});

	return Errors.Num() == StartErrorCount;
}

// ---------------------------------------------------------------------------
// 高级：调整基础强度以容纳候选
// ---------------------------------------------------------------------------

bool FLyraRecoilPatternAdapter::BuildStrengthAdjustmentCandidate(const ULyraRecoilProfile& Profile,
	const TArray<FVector2D>& Targets, FLyraRecoilStrengthAdjustment& OutAdjustment,
	FLyraRecoilPatternData& OutData, TArray<FString>& Errors)
{
	const int32 StartErrorCount = Errors.Num();
	OutAdjustment = FLyraRecoilStrengthAdjustment();

	const FLyraRecoilPatternData Source = Read(Profile);
	OutData = Source;

	if (!ValidateDataShape(Source, Errors))
	{
		return false;
	}

	TArray<FVector2D> Original;
	if (!BuildCumulative(Profile, Original, Errors))
	{
		return false;
	}

	const int32 FixedLength = Source.PatternLength;
	OutAdjustment.TargetCumulativePoints = Targets;
	OutAdjustment.OldHorizontal = static_cast<double>(Profile.RecoilPerShot_Horizontal);
	OutAdjustment.OldVertical = static_cast<double>(Profile.RecoilPerShot_Vertical);
	OutAdjustment.NewHorizontal = OutAdjustment.OldHorizontal;
	OutAdjustment.NewVertical = OutAdjustment.OldVertical;

	if (Targets.Num() != FixedLength)
	{
		Errors.Add(FString::Printf(TEXT("BuildStrengthAdjustmentCandidate expects %d target points (PatternLength), got %d"),
			FixedLength, Targets.Num()));
		return false;
	}

	const double OldHorizontal = OutAdjustment.OldHorizontal;
	const double OldVertical = OutAdjustment.OldVertical;
	if (!IsFiniteValue(OldHorizontal) || !IsFiniteValue(OldVertical) || OldHorizontal < 0.0 || OldVertical < 0.0)
	{
		OutAdjustment.BlockingReasons.Add(TEXT("Base strengths must be finite and non-negative before the strength accommodation command can run"));
		Errors.Append(OutAdjustment.BlockingReasons);
		return false;
	}

	TArray<FVector2D> Deltas;
	Deltas.SetNum(FixedLength);
	for (int32 Index = 0; Index < FixedLength; ++Index)
	{
		if (!IsFiniteValue(Targets[Index].X) || !IsFiniteValue(Targets[Index].Y))
		{
			Errors.Add(FString::Printf(TEXT("Targets[%d] is not finite (NaN/Inf)"), Index));
			return false;
		}

		Deltas[Index].X = Targets[Index].X - ((Index > 0) ? Targets[Index - 1].X : 0.0);
		Deltas[Index].Y = Targets[Index].Y - ((Index > 0) ? Targets[Index - 1].Y : 0.0);
	}

	// --- 候选强度：H' = max(H, max|ΔYaw|)，V' = max(V, max(ΔPitch / c))，只纳入符号相容的目标发 ---
	double NewHorizontal = OldHorizontal;
	double NewVertical = OldVertical;

	for (int32 Index = 0; Index < FixedLength; ++Index)
	{
		const double CurveScale = static_cast<double>(Profile.GetVerticalKickCurveScale(Index));
		if (!IsFiniteValue(CurveScale))
		{
			Errors.Add(FString::Printf(TEXT("VerticalKickCurve sample at shot %d is not finite (NaN/Inf)"), Index));
			return false;
		}

		NewHorizontal = FMath::Max(NewHorizontal, FMath::Abs(Deltas[Index].X));

		if (FMath::Abs(CurveScale) <= DenominatorEpsilon)
		{
			if (!FMath::IsNearlyZero(Deltas[Index].Y, ZeroAngleTolerance))
			{
				OutAdjustment.BlockingReasons.Add(FString::Printf(
					TEXT("Shot %d: VerticalKickCurve scale is zero; vertical target %.6f deg cannot be expressed by raising V (the curve itself must be edited)"),
					Index, Deltas[Index].Y));
			}
			continue;
		}

		const double RequiredVertical = Deltas[Index].Y / CurveScale;
		if (RequiredVertical < -ZeroAngleTolerance)
		{
			OutAdjustment.BlockingReasons.Add(FString::Printf(
				TEXT("Shot %d: vertical target %.6f deg is opposite to curve scale %.6f; raising V cannot express it and the curve must not be forced to 1"),
				Index, Deltas[Index].Y, CurveScale));
			continue;
		}

		NewVertical = FMath::Max(NewVertical, RequiredVertical);
	}

	const double FloatMax = static_cast<double>(TNumericLimits<float>::Max());
	if (!IsFiniteValue(NewHorizontal) || !IsFiniteValue(NewVertical) ||
		NewHorizontal > FloatMax || NewVertical > FloatMax)
	{
		OutAdjustment.BlockingReasons.Add(TEXT("Candidate base strengths overflow float storage"));
	}

	if (OutAdjustment.BlockingReasons.Num() > 0)
	{
		OutAdjustment.bApplicable = false;
		OutAdjustment.NewHorizontal = OldHorizontal;
		OutAdjustment.NewVertical = OldVertical;
		Errors.Append(OutAdjustment.BlockingReasons);
		return false;
	}

	OutAdjustment.NewHorizontal = NewHorizontal;
	OutAdjustment.NewVertical = NewVertical;

	// --- 用候选强度重新归一化（保持目标逐发角度，而不是保留原始归一化值）---
	FLyraRecoilConversionReport Report;
	TArray<FRecoilPatternPoint> Candidate = Source.Points;

	for (int32 Index = 0; Index < FixedLength; ++Index)
	{
		const FRecoilPatternPoint& OldPoint = Source.Points[Index];
		const double CurveScale = static_cast<double>(Profile.GetVerticalKickCurveScale(Index));

		if (NewHorizontal > DenominatorEpsilon)
		{
			const double RawX = Deltas[Index].X / NewHorizontal;
			if (!CheckNormalizedValue(Index, TEXT("X"), RawX, -1.0, 1.0, Report, Errors))
			{
				OutAdjustment.bApplicable = false;
				OutAdjustment.BlockingReasons.Add(TEXT("Renormalized horizontal candidate is out of range"));
				Errors.Append(OutAdjustment.BlockingReasons);
				return false;
			}

			const float NewRawX = static_cast<float>(FMath::Clamp(RawX, -1.0, 1.0));
			if (NewRawX != OldPoint.X)
			{
				OutAdjustment.Differences.Add(FString::Printf(TEXT("PatternPoints[%d].X: %.8f -> %.8f"),
					Index, static_cast<double>(OldPoint.X), static_cast<double>(NewRawX)));

				if (OldHorizontal <= DenominatorEpsilon && FMath::Abs(static_cast<double>(OldPoint.X)) > 0.0 && NewRawX == 0.0f)
				{
					OutAdjustment.HiddenValuesForcedToZero.AddUnique(Index);
				}

				Candidate[Index].X = NewRawX;
			}
		}

		const double NewVerticalDenominator = NewVertical * CurveScale;
		if (FMath::Abs(NewVerticalDenominator) <= DenominatorEpsilon)
		{
			// 仍不可逆：目标垂直增量必然为零（否则上面已 blocked）。保留原始隐藏值。
			continue;
		}

		const double RawY = Deltas[Index].Y / NewVerticalDenominator;
		if (!CheckNormalizedValue(Index, TEXT("Y"), RawY, 0.0, 1.0, Report, Errors))
		{
			OutAdjustment.bApplicable = false;
			OutAdjustment.BlockingReasons.Add(TEXT("Renormalized vertical candidate is out of range"));
			Errors.Append(OutAdjustment.BlockingReasons);
			return false;
		}

		const float NewRawY = static_cast<float>(FMath::Clamp(RawY, 0.0, 1.0));
		if (NewRawY != OldPoint.Y)
		{
			OutAdjustment.Differences.Add(FString::Printf(TEXT("PatternPoints[%d].Y: %.8f -> %.8f"),
				Index, static_cast<double>(OldPoint.Y), static_cast<double>(NewRawY)));

			const double OldVerticalDenominator = OldVertical * CurveScale;
			if (FMath::Abs(OldVerticalDenominator) <= DenominatorEpsilon &&
				FMath::Abs(static_cast<double>(OldPoint.Y)) > 0.0 && NewRawY == 0.0f)
			{
				OutAdjustment.HiddenValuesForcedToZero.AddUnique(Index);
			}

			Candidate[Index].Y = NewRawY;
		}
	}


    // Preserve the effective vertical angle of every array tail shot, even when L is zero.
    for (int32 Index = FixedLength; Index < Candidate.Num(); ++Index)
    {
        const double C = Profile.GetVerticalKickCurveScale(Index);
        const double Target = OldVertical * Source.Points[Index].Y * C;
        const double Denominator = NewVertical * C;
        if (!IsFiniteValue(C) || !IsFiniteValue(Target) || !IsFiniteValue(Denominator))
        { Errors.Add(TEXT("数组尾段曲线或角度非有限。")); return false; }
        if (FMath::Abs(Denominator) > DenominatorEpsilon)
        {
            const double Y = Target / Denominator;
            if (!CheckNormalizedValue(Index, TEXT("Y"), Y, 0, 1, Report, Errors)) return false;
            const float Stored = static_cast<float>(FMath::Clamp(Y, 0.0, 1.0));
            if (Stored != Candidate[Index].Y)
            {
                OutAdjustment.Differences.Add(FString::Printf(TEXT("尾段第%d发Y: %.8g -> %.8g，垂直角度保持%.8g°"), Index+1, Candidate[Index].Y, Stored, Target));
                if (FMath::Abs(OldVertical*C) <= DenominatorEpsilon && Stored == 0 && Candidate[Index].Y != 0)
                    OutAdjustment.HiddenValuesForcedToZero.AddUnique(Index);
                Candidate[Index].Y = Stored;
            }
            if (FMath::Abs(NewVertical*Candidate[Index].Y*C-Target) > GetAngleTolerance(Target))
            { Errors.Add(TEXT("数组尾段角度重建误差超限。")); return false; }
        }
        // Tail X remains byte-for-byte unchanged.
    }
    for (int32 Seed : { 17, 42, 20260917 })
        for (int32 Index = FixedLength; Index < Source.Points.Num()+8; ++Index)
        {
            const double Walk = FRecoilRuntimeState::ComputePatternHorizontal(Profile, Index, Seed);
            const double C = Profile.GetVerticalKickCurveScale(Index);
            const double BeforeY = Profile.GetPatternPoint(Index).Y;
            const double AfterY = Candidate.IsEmpty() ? 1.0 : Candidate[FMath::Min(Index, Candidate.Num()-1)].Y;
            const double BeforeV=OldVertical*BeforeY*C, AfterV=NewVertical*AfterY*C;
            if (!IsFiniteValue(Walk) || !IsFiniteValue(BeforeV) || !IsFiniteValue(AfterV))
            { Errors.Add(TEXT("尾段前后比较出现非有限值。")); return false; }
            OutAdjustment.Differences.Add(FString::Printf(TEXT("种子%d，第%d发%s：ΔYaw %.6g -> %.6g°；ΔPitch %.6g -> %.6g°"),
                Seed, Index+1, Index >= Source.Points.Num() ? TEXT("（数组外）") : TEXT(""),
                OldHorizontal*Walk, NewHorizontal*Walk, BeforeV, AfterV));
        }

	if (NewHorizontal != OldHorizontal)
	{
		OutAdjustment.Differences.Add(FString::Printf(TEXT("RecoilPerShot_Horizontal: %.8f -> %.8f"), OldHorizontal, NewHorizontal));
		OutAdjustment.Differences.Add(TEXT("Note: raising H rescales the random horizontal tail; HorizontalRandomRange is NOT modified."));
	}
	if (NewVertical != OldVertical)
	{
		OutAdjustment.Differences.Add(FString::Printf(TEXT("RecoilPerShot_Vertical: %.8f -> %.8f"), OldVertical, NewVertical));
		OutAdjustment.Differences.Add(TEXT("Note: raising V changes tail shots beyond the array and curve extrapolation; infinite tails are NOT guaranteed lossless."));
	}
	for (int32 HiddenIndex : OutAdjustment.HiddenValuesForcedToZero)
	{
		OutAdjustment.Differences.Add(FString::Printf(
			TEXT("PatternPoints[%d]: the original value was hidden behind a zero denominator; keeping the same zero angle forces the stored value to 0."),
			HiddenIndex));
	}
	if (OutAdjustment.Differences.Num() == 0)
	{
		OutAdjustment.Differences.Add(TEXT("No field changes required: the candidate is already expressible with the current strengths."));
	}

	if (!RebuildAndVerify(Profile, Candidate, FixedLength, Targets, NewHorizontal, NewVertical, Report, Errors))
	{
		OutAdjustment.bApplicable = false;
		OutAdjustment.BlockingReasons.Add(TEXT("Renormalized candidate failed the forward rebuild check"));
		Errors.Append(OutAdjustment.BlockingReasons);
		return false;
	}

	OutAdjustment.bApplicable = true;
	OutData.Points = MoveTemp(Candidate);
	OutData.PatternLength = FixedLength;
	return Errors.Num() == StartErrorCount;
}

// ---------------------------------------------------------------------------
// 容差
// ---------------------------------------------------------------------------

double FLyraRecoilPatternAdapter::GetAngleTolerance(double TargetAngle)
{
	return AngleToleranceAbsolute + AngleToleranceRelative * FMath::Abs(TargetAngle);
}
