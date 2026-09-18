// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Camera/LyraCameraRollShake.h"
#include "Camera/LyraCameraShakeTypes.h"
#include "Curves/RichCurve.h"
#include "UObject/Package.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"

/**
 * P8 自动验证：Lyra.Recoil.RollShake.*
 *
 * 依据《FPS 相机镜头设计与实现（脱敏版）》§2.1 的算法：
 *     衰减包络 = 插值(初始振幅, 目标振幅, t / Duration)
 *     周期项   = cos(2π / Period × t + 相位扰动)
 *     Roll     = 衰减包络 × 周期项
 *
 * 本组测试覆盖四类风险：
 *   1. 数学正确性 —— 包络单调衰减、周期项在正确时刻过零、终值精确归零
 *   2. 独立性     —— Roll 的推进完全不影响 Pitch/Yaw（这是本次改动的核心设计判断）
 *   3. 可复现性   —— PhaseJitter=0 时序列逐帧一致；(Seed, ShotIndex) 决定相位
 *   4. 连射行为   —— 振幅随发序号增长并在上限封顶
 *
 * 全部走纯数值路径，不需要 UWorld —— 与既有 P2~P5 测试同一范式。
 */
namespace LyraRollShakeTest
{
	static const TCHAR* const RiflePackage = TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle");

	static ULyraRecoilProfile* LoadRifle()
	{
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), RiflePackage, *FPaths::GetBaseFilename(RiflePackage));
		return LoadObject<ULyraRecoilProfile>(nullptr, *ObjectPath);
	}

	/** 造一个最小可用的 Roll 参数：线性衰减、无相位扰动（可复现）。 */
	static FCameraRollShakeParams MakeParams(float Amplitude = 1.0f, float Duration = 0.2f, float Period = 0.05f)
	{
		FCameraRollShakeParams Params;
		Params.AmplitudeDegrees = Amplitude;
		Params.DurationSeconds = Duration;
		Params.PeriodSeconds = Period;
		Params.PhaseJitterRadians = 0.0f;
		Params.EndAmplitudeRatio = 0.0f;
		Params.DecayCurve = nullptr;   // 退化线性
		return Params;
	}

	/** 以固定步长把一次震动推进完，并把每个采样点收集起来。 */
	static TArray<float> SampleBurst(const FCameraRollShakeParams& Params, int32 Seed, int32 ShotIndex, float StepSeconds = 1.0f / 240.0f)
	{
		TArray<float> Samples;

		FCameraRollShakeState State;
		ULyraCameraRollShake::Trigger(State, Params, Seed, ShotIndex);
		Samples.Add(State.CurrentRoll);

		// 多跑一点时间，确保越过 Duration 之后还在采（用于验证归零）
		const float TotalTime = Params.DurationSeconds * 1.5f;
		const int32 NumSteps = FMath::CeilToInt(TotalTime / StepSeconds);
		for (int32 Step = 0; Step < NumSteps; ++Step)
		{
			Samples.Add(ULyraCameraRollShake::Advance(State, StepSeconds));
		}

		return Samples;
	}
}

//////////////////////////////////////////////////////////////////////////
// 1) 数学正确性：包络单调衰减 + 终值归零
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRollShakeEnvelopeTest, "Lyra.Recoil.RollShake.EnvelopeDecayAndZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRollShakeEnvelopeTest::RunTest(const FString& Parameters)
{
	using namespace LyraRollShakeTest;

	const FCameraRollShakeParams Params = MakeParams(/*Amp=*/1.0f, /*Duration=*/0.2f, /*Period=*/0.05f);

	// --- 包络：t=0 为满，t=Duration 为 0，全程单调不增 ---
	TestTrue(TEXT("Envelope at t=0 is 1.0"),
		FMath::IsNearlyEqual(ULyraCameraRollShake::EvaluateEnvelope(Params, 0.0f), 1.0f, 1e-4f));

	TestTrue(TEXT("Envelope at t=Duration is 0.0 (EndAmplitudeRatio=0)"),
		FMath::IsNearlyZero(ULyraCameraRollShake::EvaluateEnvelope(Params, Params.DurationSeconds), 1e-4f));

	{
		float Previous = ULyraCameraRollShake::EvaluateEnvelope(Params, 0.0f);
		bool bMonotonic = true;
		constexpr int32 Samples = 40;
		for (int32 Index = 1; Index <= Samples; ++Index)
		{
			const float Time = Params.DurationSeconds * (static_cast<float>(Index) / Samples);
			const float Current = ULyraCameraRollShake::EvaluateEnvelope(Params, Time);
			if (Current > Previous + 1e-5f)
			{
				bMonotonic = false;
				break;
			}
			Previous = Current;
		}
		TestTrue(TEXT("Envelope decays monotonically across the whole duration"), bMonotonic);
	}

	// --- 终值归零：超过 Duration 之后必须精确为 0（而不是停在末值）---
	TestTrue(TEXT("Roll at t=Duration is exactly 0"),
		FMath::IsNearlyZero(ULyraCameraRollShake::EvaluateRollShake(Params, Params.DurationSeconds), 1e-6f));

	TestTrue(TEXT("Roll past Duration stays 0 (t = 3x duration)"),
		FMath::IsNearlyZero(ULyraCameraRollShake::EvaluateRollShake(Params, Params.DurationSeconds * 3.0f), 1e-6f));

	// --- 振幅上限：任何时刻的绝对值都不该超过起始振幅 ---
	{
		const TArray<float> Samples = SampleBurst(Params, /*Seed=*/1234, /*ShotIndex=*/0);
		float MaxAbs = 0.0f;
		for (const float Value : Samples)
		{
			MaxAbs = FMath::Max(MaxAbs, FMath::Abs(Value));
		}

		AddInfo(FString::Printf(TEXT("Roll shake peak over the burst: %.6f (declared amplitude %.6f)"), MaxAbs, Params.AmplitudeDegrees));

		TestTrue(FString::Printf(TEXT("Peak never exceeds the declared amplitude (%.6f <= %.6f)"), MaxAbs, Params.AmplitudeDegrees),
			MaxAbs <= Params.AmplitudeDegrees + 1e-4f);

		TestTrue(FString::Printf(TEXT("Peak reaches a meaningful fraction of the amplitude (%.6f)"), MaxAbs),
			MaxAbs > Params.AmplitudeDegrees * 0.5f);

		// 尾部必须已经归零（最后一次采样在 Duration*1.5 之后）
		TestTrue(FString::Printf(TEXT("The burst ends at zero (last sample %.6f)"), Samples.Last()),
			FMath::IsNearlyZero(Samples.Last(), 1e-6f));
	}

	// --- 周期项：在 t = Period/2 处应该过零（cos(π) 的零点是 π/2）---
	// 注意线性衰减下振幅非零，所以这里验证的是"符号翻转"这个特征
	{
		const float QuarterPeriod = Params.PeriodSeconds * 0.25f;
		const float ThreeQuarterPeriod = Params.PeriodSeconds * 0.75f;

		const float Early = ULyraCameraRollShake::EvaluateRollShake(Params, QuarterPeriod);
		const float Late = ULyraCameraRollShake::EvaluateRollShake(Params, ThreeQuarterPeriod);

		AddInfo(FString::Printf(TEXT("Roll at T/4 = %.6f, at 3T/4 = %.6f (expect opposite signs)"), Early, Late));

		TestTrue(FString::Printf(TEXT("Sign flips within the first period (%.6f vs %.6f)"), Early, Late),
			(Early > 0.0f) != (Late > 0.0f));
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 2) 独立性：Roll 不影响 Pitch/Yaw
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRollShakeIndependenceTest, "Lyra.Recoil.RollShake.IndependentFromPitchYaw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRollShakeIndependenceTest::RunTest(const FString& Parameters)
{
	using namespace LyraRollShakeTest;

	ULyraRecoilProfile* Profile = LoadRifle();
	if (!TestNotNull(TEXT("Rifle recoil profile is loadable"), Profile))
	{
		return false;
	}

	// 同一份资产跑两轮：一轮启用 Roll、一轮禁用。Pitch/Yaw 必须逐帧一致。
	auto RunBurst = [](ULyraRecoilProfile& InProfile, bool bEnableRoll)
	{
		const bool bOriginal = InProfile.bEnableRollShake;
		InProfile.bEnableRollShake = bEnableRoll;

		FRecoilRuntimeState State;
		State.Reset(&InProfile);

		TArray<float> PitchTrace;
		TArray<float> YawTrace;
		TArray<float> RollTrace;

		constexpr float StepSeconds = 1.0f / 120.0f;
		for (int32 Shot = 0; Shot < 8; ++Shot)
		{
			State.ApplyShot(&InProfile, 1.0f, EPoseState::Standing);
			for (int32 Step = 0; Step < 12; ++Step)   // 每发之间推进 0.1 秒
			{
				State.Advance(&InProfile, StepSeconds);
				PitchTrace.Add(State.AccumulatedPitch);
				YawTrace.Add(State.AccumulatedYaw);
				RollTrace.Add(State.GetCameraRollOffset());
			}
		}

		InProfile.bEnableRollShake = bOriginal;
		return MakeTuple(MoveTemp(PitchTrace), MoveTemp(YawTrace), MoveTemp(RollTrace));
	};

	auto [PitchWithRoll, YawWithRoll, RollWithRoll] = RunBurst(*Profile, true);
	auto [PitchNoRoll, YawNoRoll, RollNoRoll] = RunBurst(*Profile, false);

	AddInfo(FString::Printf(TEXT("Samples collected: %d"), PitchWithRoll.Num()));

	// --- Pitch/Yaw 必须逐帧完全相同 ---
	TestEqual(TEXT("Pitch/Yaw/Roll trace lengths match"), PitchWithRoll.Num(), PitchNoRoll.Num());

	if (PitchWithRoll.Num() == PitchNoRoll.Num())
	{
		bool bPitchIdentical = true;
		bool bYawIdentical = true;
		for (int32 Index = 0; Index < PitchWithRoll.Num(); ++Index)
		{
			if (!FMath::IsNearlyEqual(PitchWithRoll[Index], PitchNoRoll[Index], 1e-6f))
			{
				bPitchIdentical = false;
			}
			if (!FMath::IsNearlyEqual(YawWithRoll[Index], YawNoRoll[Index], 1e-6f))
			{
				bYawIdentical = false;
			}
		}

		TestTrue(TEXT("AccumulatedPitch is bit-identical regardless of whether Roll is enabled"), bPitchIdentical);
		TestTrue(TEXT("AccumulatedYaw is bit-identical regardless of whether Roll is enabled"), bYawIdentical);
	}

	// --- Roll 关闭时必须恒为 0 ---
	{
		float MaxAbsRoll = 0.0f;
		for (const float Value : RollNoRoll)
		{
			MaxAbsRoll = FMath::Max(MaxAbsRoll, FMath::Abs(Value));
		}
		TestTrue(FString::Printf(TEXT("Roll is exactly 0 for the whole burst when disabled (max %.8f)"), MaxAbsRoll),
			FMath::IsNearlyZero(MaxAbsRoll, 1e-6f));
	}

	// --- Roll 开启时必须真的动起来（否则上面那条 0 断言可能只是"没实现"的假阳性）---
	{
		float MaxAbsRoll = 0.0f;
		for (const float Value : RollWithRoll)
		{
			MaxAbsRoll = FMath::Max(MaxAbsRoll, FMath::Abs(Value));
		}
		AddInfo(FString::Printf(TEXT("Roll peak with roll enabled: %.6f"), MaxAbsRoll));
		TestTrue(FString::Printf(TEXT("Roll actually moves when enabled (max %.6f > 0)"), MaxAbsRoll),
			MaxAbsRoll > 1e-4f);
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 3) 可复现性：同种子同发序号 → 完全一致的序列
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRollShakeDeterminismTest, "Lyra.Recoil.RollShake.Determinism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRollShakeDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace LyraRollShakeTest;

	// PhaseJitter = 0：序列必须逐帧一致
	{
		const FCameraRollShakeParams Params = MakeParams(/*Amp=*/0.6f, /*Duration=*/0.22f, /*Period=*/0.055f);

		const TArray<float> First = SampleBurst(Params, /*Seed=*/20260917, /*ShotIndex=*/3);
		const TArray<float> Second = SampleBurst(Params, /*Seed=*/20260917, /*ShotIndex=*/3);

		TestEqual(TEXT("Two bursts have the same sample count"), First.Num(), Second.Num());

		bool bIdentical = (First.Num() == Second.Num());
		if (bIdentical)
		{
			for (int32 Index = 0; Index < First.Num(); ++Index)
			{
				if (!FMath::IsNearlyEqual(First[Index], Second[Index], 1e-7f))
				{
					bIdentical = false;
					break;
				}
			}
		}

		TestTrue(TEXT("PhaseJitter=0 gives a bit-identical replay (Seed and ShotIndex are the only inputs)"), bIdentical);
	}

	// PhaseJitter > 0：相位必须由 (Seed, ShotIndex) 决定，且不同发序号要拿到不同相位
	{
		FCameraRollShakeParams Params = MakeParams(/*Amp=*/0.6f, /*Duration=*/0.22f, /*Period=*/0.055f);
		Params.PhaseJitterRadians = 0.35f;

		const float PhaseRepeat = ULyraCameraRollShake::ComputePhaseOffset(Params, /*Seed=*/777, /*ShotIndex=*/5);
		const float PhaseSameAgain = ULyraCameraRollShake::ComputePhaseOffset(Params, /*Seed=*/777, /*ShotIndex=*/5);
		const float PhaseDifferentShot = ULyraCameraRollShake::ComputePhaseOffset(Params, /*Seed=*/777, /*ShotIndex=*/6);
		const float PhaseDifferentSeed = ULyraCameraRollShake::ComputePhaseOffset(Params, /*Seed=*/778, /*ShotIndex=*/5);

		AddInfo(FString::Printf(TEXT("Phase (seed=777, shot=5) = %.6f"), PhaseRepeat));
		AddInfo(FString::Printf(TEXT("Phase (seed=777, shot=6) = %.6f"), PhaseDifferentShot));
		AddInfo(FString::Printf(TEXT("Phase (seed=778, shot=5) = %.6f"), PhaseDifferentSeed));

		TestTrue(TEXT("The same (Seed, ShotIndex) yields the same phase"),
			FMath::IsNearlyEqual(PhaseRepeat, PhaseSameAgain, 1e-7f));

		TestTrue(TEXT("A different ShotIndex yields a different phase"),
			!FMath::IsNearlyEqual(PhaseRepeat, PhaseDifferentShot, 1e-6f));

		TestTrue(TEXT("A different Seed yields a different phase"),
			!FMath::IsNearlyEqual(PhaseRepeat, PhaseDifferentSeed, 1e-6f));

		// 相位必须落在 ±PhaseJitter 内
		const float MaxAbsPhase = FMath::Max3(FMath::Abs(PhaseRepeat), FMath::Abs(PhaseDifferentShot), FMath::Abs(PhaseDifferentSeed));
		TestTrue(FString::Printf(TEXT("Phase stays within +-PhaseJitter (max %.6f <= %.6f)"), MaxAbsPhase, Params.PhaseJitterRadians),
			MaxAbsPhase <= Params.PhaseJitterRadians + 1e-5f);
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 4) 连射行为：振幅随发序号增长并封顶
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRollShakeRampTest, "Lyra.Recoil.RollShake.AmplitudeRampAndCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRollShakeRampTest::RunTest(const FString& Parameters)
{
	using namespace LyraRollShakeTest;

	ULyraRecoilProfile* Profile = LoadRifle();
	if (!TestNotNull(TEXT("Rifle recoil profile is loadable"), Profile))
	{
		return false;
	}

	// 先确认资产上真的配了连射增量，否则下面的断言会失去意义
	TestTrue(FString::Printf(TEXT("Roll shake is enabled on the asset (asset flag=%d)"), Profile->bEnableRollShake ? 1 : 0),
		Profile->bEnableRollShake);

	TestTrue(FString::Printf(TEXT("RollShake_RampStartShot is within the pattern window (start=%d, patternLength=%d)"),
		Profile->RollShake_RampStartShot, Profile->PatternLength),
		Profile->RollShake_RampStartShot < Profile->PatternLength);

	TestTrue(FString::Printf(TEXT("RollShake_AmplitudePerShot is positive (%.4f)"), Profile->RollShake_AmplitudePerShot),
		Profile->RollShake_AmplitudePerShot > 0.0f);

	TestTrue(FString::Printf(TEXT("RollShake_MaxAmplitudeBonus is positive (%.4f)"), Profile->RollShake_MaxAmplitudeBonus),
		Profile->RollShake_MaxAmplitudeBonus > 0.0f);

	// --- 逐发装配参数，检查振幅走势 ---
	auto BuildAmplitude = [Profile](int32 ShotIndex) -> float
	{
		FCameraRollShakeParams Built;
		if (!Profile->BuildRollShakeParams(ShotIndex, 1.0f, 1.0f, /*Seed=*/20260917, Built))
		{
			return 0.0f;
		}
		return Built.AmplitudeDegrees;
	};

	const float AmpShot0 = BuildAmplitude(0);
	const float AmpBeforeRamp = BuildAmplitude(FMath::Max(0, Profile->RollShake_RampStartShot - 1));
	const float AmpAtRampStart = BuildAmplitude(Profile->RollShake_RampStartShot);
	const float AmpLate = BuildAmplitude(Profile->PatternLength - 1);
	const float AmpFarBeyond = BuildAmplitude(Profile->PatternLength + 50);

	AddInfo(FString::Printf(TEXT("Amplitude by shot: shot0=%.4f beforeRamp=%.4f atRampStart=%.4f late=%.4f farBeyond=%.4f"),
		AmpShot0, AmpBeforeRamp, AmpAtRampStart, AmpLate, AmpFarBeyond));

	TestTrue(FString::Printf(TEXT("First shot uses the base amplitude (%.4f == %.4f)"), AmpShot0, Profile->RollShake_Amplitude),
		FMath::IsNearlyEqual(AmpShot0, Profile->RollShake_Amplitude, 1e-5f));

	TestTrue(FString::Printf(TEXT("Amplitude is flat before the ramp start (%.4f == %.4f)"), AmpBeforeRamp, Profile->RollShake_Amplitude),
		FMath::IsNearlyEqual(AmpBeforeRamp, Profile->RollShake_Amplitude, 1e-5f));

	TestTrue(FString::Printf(TEXT("Amplitude grows at the ramp start (%.4f > %.4f)"), AmpAtRampStart, AmpShot0),
		AmpAtRampStart > AmpShot0);

	TestTrue(FString::Printf(TEXT("Amplitude keeps growing later in the burst (%.4f > %.4f)"), AmpLate, AmpAtRampStart),
		AmpLate > AmpAtRampStart);

	// --- 封顶：给一个远超 ramp 长度的发序号，增长必须停住 ---
	const float Ceiling = Profile->RollShake_Amplitude + Profile->RollShake_MaxAmplitudeBonus;
	TestTrue(FString::Printf(TEXT("Amplitude is capped at base+bonus (%.4f <= %.4f + %.4f)"),
		AmpFarBeyond, Profile->RollShake_Amplitude, Profile->RollShake_MaxAmplitudeBonus),
		AmpFarBeyond <= Ceiling + 1e-4f);

	TestTrue(FString::Printf(TEXT("The cap is actually reached (%.4f >= %.4f)"), AmpFarBeyond, Ceiling - 1e-4f),
		AmpFarBeyond >= Ceiling - 1e-4f);

	// --- 同一发序号重复装配必须得到同一结果（参数装配也是确定性的）---
	TestTrue(TEXT("BuildRollShakeParams is deterministic for a given shot index"),
		FMath::IsNearlyEqual(BuildAmplitude(7), BuildAmplitude(7), 1e-7f));

	// --- 姿态倍率与全局倍率必须线性缩放振幅 ---
	{
		FCameraRollShakeParams Full;
		FCameraRollShakeParams Half;
		Profile->BuildRollShakeParams(0, 1.0f, 1.0f, 20260917, Full);
		Profile->BuildRollShakeParams(0, 0.5f, 1.0f, 20260917, Half);

		TestTrue(FString::Printf(TEXT("Amplitude scales linearly with the pose multiplier (%.4f == 0.5 x %.4f)"),
			Half.AmplitudeDegrees, Full.AmplitudeDegrees),
			FMath::IsNearlyEqual(Half.AmplitudeDegrees, Full.AmplitudeDegrees * 0.5f, 1e-5f));
	}

	// --- 关掉资产开关时装配必须失败（调用方据此走 Stop 分支）---
	{
		const bool bOriginal = Profile->bEnableRollShake;
		Profile->bEnableRollShake = false;

		FCameraRollShakeParams Disabled;
		const bool bBuilt = Profile->BuildRollShakeParams(0, 1.0f, 1.0f, 20260917, Disabled);

		Profile->bEnableRollShake = bOriginal;

		TestFalse(TEXT("BuildRollShakeParams returns false when the asset disables roll shake"), bBuilt);
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 5) 衰减曲线：配了曲线就按曲线走，不配则退化线性
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRollShakeCurveTest, "Lyra.Recoil.RollShake.DecayCurveOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRollShakeCurveTest::RunTest(const FString& Parameters)
{
	using namespace LyraRollShakeTest;

	// --- 快衰减曲线：半程时包络应明显低于线性 ---
	FRichCurve FastCurve;
	FastCurve.AddKey(0.0f, 1.0f);
	FastCurve.AddKey(0.5f, 0.1f);
	FastCurve.AddKey(1.0f, 0.0f);

	FCameraRollShakeParams CurveParams = MakeParams(/*Amp=*/1.0f, /*Duration=*/0.2f, /*Period=*/0.05f);
	CurveParams.DecayCurve = &FastCurve;

	FCameraRollShakeParams LinearParams = MakeParams(/*Amp=*/1.0f, /*Duration=*/0.2f, /*Period=*/0.05f);
	LinearParams.DecayCurve = nullptr;

	const float CurveAtHalf = ULyraCameraRollShake::EvaluateEnvelope(CurveParams, CurveParams.DurationSeconds * 0.5f);
	const float LinearAtHalf = ULyraCameraRollShake::EvaluateEnvelope(LinearParams, LinearParams.DurationSeconds * 0.5f);

	AddInfo(FString::Printf(TEXT("Envelope at half duration: curve=%.4f linear=%.4f"), CurveAtHalf, LinearAtHalf));

	TestTrue(FString::Printf(TEXT("A steep decay curve is below the linear fallback at t=0.5 (%.4f < %.4f)"), CurveAtHalf, LinearAtHalf),
		CurveAtHalf < LinearAtHalf - 0.1f);

	TestTrue(TEXT("Curve-based envelope still starts at 1.0"),
		FMath::IsNearlyEqual(ULyraCameraRollShake::EvaluateEnvelope(CurveParams, 0.0f), 1.0f, 1e-4f));

	// --- EndAmplitudeRatio：配成 1 则完全不衰减 ---
	{
		FCameraRollShakeParams Flat = MakeParams(/*Amp=*/1.0f, /*Duration=*/0.2f, /*Period=*/0.05f);
		Flat.EndAmplitudeRatio = 1.0f;

		const float AtStart = ULyraCameraRollShake::EvaluateEnvelope(Flat, 0.0f);
		const float AtEnd = ULyraCameraRollShake::EvaluateEnvelope(Flat, Flat.DurationSeconds);

		AddInfo(FString::Printf(TEXT("EndAmplitudeRatio=1 envelope: start=%.4f end=%.4f"), AtStart, AtEnd));

		TestTrue(FString::Printf(TEXT("EndAmplitudeRatio=1 keeps the envelope flat (%.4f ≈ %.4f)"), AtStart, AtEnd),
			FMath::IsNearlyEqual(AtStart, AtEnd, 1e-3f));
	}

	// --- 极端参数不该产生 NaN / Inf ---
	{
		FCameraRollShakeParams Degenerate;
		Degenerate.AmplitudeDegrees = 1.0f;
		Degenerate.DurationSeconds = 0.0f;   // 时长为 0
		Degenerate.PeriodSeconds = 0.0f;     // 周期为 0（除零风险）

		const float Value = ULyraCameraRollShake::EvaluateRollShake(Degenerate, 0.01f);
		TestTrue(FString::Printf(TEXT("Zero duration/period does not produce NaN or Inf (%.6f)"), Value),
			FMath::IsFinite(Value));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
