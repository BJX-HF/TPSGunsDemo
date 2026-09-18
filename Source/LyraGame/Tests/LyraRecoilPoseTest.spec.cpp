// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "UObject/Package.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"

/**
 * P4 自动验证：Lyra.Recoil.Pose.*
 *
 * 开发计划 §P4 要求：
 *   - 对四种姿态各模拟 10 发，输出垂直/水平累计位移
 *   - 断言：各姿态结果比值 == 配置倍率比值（1e-3 容差）
 *   - 断言：蹲姿位移 < 站姿位移（默认配置下）
 *   - 非线性恢复曲线生效（"慢—快"与"快—慢"两种，回正进程可量化区分）
 *
 * 四种"姿态"在本轮的定义（瞄准与姿态正交，作为第四种组合单独列）：
 *   Standing / Crouching / JumpingOrFalling / Standing + Aiming(alpha=1)
 *
 * 用**真实交付资产** DA_Recoil_Rifle，测出来的位移直接填进
 * Docs/Recoil/04_PoseMatrix.md。位移值同时用 AddInfo 打到日志里，便于抄录与复核。
 */
namespace LyraRecoilPoseTest
{
	static const TCHAR* const RiflePackage = TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle");

	static constexpr float RatioTolerance = 1e-3f;
	static constexpr int32 ShotsPerPose = 10;

	/** 一次姿态模拟的测量结果 */
	struct FPoseMeasurement
	{
		EPoseState PoseState = EPoseState::Standing;
		float AimingAlpha = 0.0f;
		float Multiplier = 1.0f;
		float AccumulatedPitch = 0.0f;
		float AccumulatedYaw = 0.0f;
	};

	static ULyraRecoilProfile* LoadRifle()
	{
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), RiflePackage, *FPaths::GetBaseFilename(RiflePackage));
		return LoadObject<ULyraRecoilProfile>(nullptr, *ObjectPath);
	}

	/** 在指定姿态倍率下模拟 ShotsPerPose 发，返回累计位移 */
	static FPoseMeasurement MeasurePose(ULyraRecoilProfile& Profile, EPoseState PoseState, float AimingAlpha)
	{
		FPoseMeasurement Result;
		Result.PoseState = PoseState;
		Result.AimingAlpha = AimingAlpha;
		Result.Multiplier = FRecoilRuntimeState::ComputePoseMultiplier(Profile, PoseState, AimingAlpha);

		FRecoilRuntimeState State;
		State.Reset(&Profile);

		for (int32 Shot = 0; Shot < ShotsPerPose; ++Shot)
		{
			State.ApplyShot(&Profile, Result.Multiplier, PoseState);
		}

		Result.AccumulatedPitch = State.AccumulatedPitch;
		Result.AccumulatedYaw = State.AccumulatedYaw;
		return Result;
	}

	static FString DescribePose(const FPoseMeasurement& Measurement)
	{
		const TCHAR* PoseName = TEXT("?");
		switch (Measurement.PoseState)
		{
		case EPoseState::Standing:			PoseName = TEXT("Standing"); break;
		case EPoseState::Crouching:			PoseName = TEXT("Crouching"); break;
		case EPoseState::JumpingOrFalling:	PoseName = TEXT("JumpingOrFalling"); break;
		default:							PoseName = TEXT("Unknown"); break;
		}

		return FString::Printf(TEXT("%s(aimAlpha=%.2f) mult=%.4f pitch=%.4f yaw=%.4f"),
			PoseName, Measurement.AimingAlpha, Measurement.Multiplier,
			Measurement.AccumulatedPitch, Measurement.AccumulatedYaw);
	}
}

//////////////////////////////////////////////////////////////////////////
// 姿态判定：纯函数优先级
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilPoseResolveTest, "Lyra.Recoil.Pose.ResolvePriority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilPoseResolveTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("(crouch=false, fall=false) -> Standing"),
		FRecoilRuntimeState::ResolvePoseState(false, false) == EPoseState::Standing);

	TestTrue(TEXT("(crouch=true, fall=false) -> Crouching"),
		FRecoilRuntimeState::ResolvePoseState(true, false) == EPoseState::Crouching);

	TestTrue(TEXT("(crouch=false, fall=true) -> JumpingOrFalling"),
		FRecoilRuntimeState::ResolvePoseState(false, true) == EPoseState::JumpingOrFalling);

	// 空中优先级必须高于蹲伏：蹲着跳不该拿到蹲伏的低后坐力
	TestTrue(TEXT("(crouch=true, fall=true) -> JumpingOrFalling (air wins over crouch)"),
		FRecoilRuntimeState::ResolvePoseState(true, true) == EPoseState::JumpingOrFalling);

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 姿态倍率：位移比值 == 配置倍率比值
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilPoseRatioTest, "Lyra.Recoil.Pose.MultiplierRatios",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilPoseRatioTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilPoseTest;

	ULyraRecoilProfile* Profile = LoadRifle();
	if (!TestNotNull(TEXT("Rifle recoil profile is loadable"), Profile))
	{
		return false;
	}

	AddInfo(FString::Printf(TEXT("Pose matrix source asset: %s"), RiflePackage));
	AddInfo(FString::Printf(TEXT("Shots per pose: %d   MaxVerticalKick=%.4f"), ShotsPerPose, Profile->MaxVerticalKick));

	const FPoseMeasurement Standing = MeasurePose(*Profile, EPoseState::Standing, 0.0f);
	const FPoseMeasurement Crouching = MeasurePose(*Profile, EPoseState::Crouching, 0.0f);
	const FPoseMeasurement Jumping = MeasurePose(*Profile, EPoseState::JumpingOrFalling, 0.0f);
	const FPoseMeasurement Aiming = MeasurePose(*Profile, EPoseState::Standing, 1.0f);

	const FPoseMeasurement Measurements[] = { Standing, Crouching, Jumping, Aiming };
	for (const FPoseMeasurement& Measurement : Measurements)
	{
		AddInfo(FString::Printf(TEXT("MEASURED %s"), *DescribePose(Measurement)));
	}

	// 位移不能被 Clamp 截断，否则线性比值不成立 —— 先把这件事断言掉
	for (const FPoseMeasurement& Measurement : Measurements)
	{
		TestTrue(
			FString::Printf(TEXT("Displacement is below MaxVerticalKick so ratios stay linear: %s (%.4f < %.4f)"),
				*DescribePose(Measurement), Measurement.AccumulatedPitch, Profile->MaxVerticalKick),
			Measurement.AccumulatedPitch < Profile->MaxVerticalKick);
	}

	TestTrue(FString::Printf(TEXT("Standing displacement is non-zero (%.4f)"), Standing.AccumulatedPitch),
		Standing.AccumulatedPitch > 0.0f);

	// --- 核心断言：位移比值 == 配置倍率比值 ---
	const float StandingMultiplier = Standing.Multiplier;

	auto CheckRatio = [this, &Standing, StandingMultiplier](const FPoseMeasurement& Measurement, const TCHAR* Label)
	{
		const float ExpectedRatio = Measurement.Multiplier / StandingMultiplier;
		const float ActualRatio = (Standing.AccumulatedPitch != 0.0f)
			? (Measurement.AccumulatedPitch / Standing.AccumulatedPitch)
			: 0.0f;

		TestTrue(
			FString::Printf(TEXT("%s: displacement ratio == configured multiplier ratio (expected %.4f, actual %.4f)"),
				Label, ExpectedRatio, ActualRatio),
			FMath::IsNearlyEqual(ExpectedRatio, ActualRatio, RatioTolerance));

		// 水平方向同样是线性缩放
		const float ExpectedYawRatio = Measurement.Multiplier / StandingMultiplier;
		const float ActualYawRatio = (Standing.AccumulatedYaw != 0.0f)
			? (Measurement.AccumulatedYaw / Standing.AccumulatedYaw)
			: 0.0f;

		TestTrue(
			FString::Printf(TEXT("%s: yaw ratio == configured multiplier ratio (expected %.4f, actual %.4f)"),
				Label, ExpectedYawRatio, ActualYawRatio),
			FMath::IsNearlyEqual(ExpectedYawRatio, ActualYawRatio, RatioTolerance));
	};

	CheckRatio(Crouching, TEXT("Crouching"));
	CheckRatio(Jumping, TEXT("JumpingOrFalling"));
	CheckRatio(Aiming, TEXT("Standing+Aiming"));

	// --- 计划明确要求：蹲姿位移 < 站姿位移（默认配置下）---
	TestTrue(
		FString::Printf(TEXT("Crouching displacement < Standing displacement (%.4f < %.4f)"),
			Crouching.AccumulatedPitch, Standing.AccumulatedPitch),
		Crouching.AccumulatedPitch < Standing.AccumulatedPitch);

	// --- 空中位移 > 站姿位移（默认配置下）---
	TestTrue(
		FString::Printf(TEXT("JumpingOrFalling displacement > Standing displacement (%.4f > %.4f)"),
			Jumping.AccumulatedPitch, Standing.AccumulatedPitch),
		Jumping.AccumulatedPitch > Standing.AccumulatedPitch);

	// --- 瞄准降低后坐力 ---
	TestTrue(
		FString::Printf(TEXT("Aiming displacement < Standing displacement (%.4f < %.4f)"),
			Aiming.AccumulatedPitch, Standing.AccumulatedPitch),
		Aiming.AccumulatedPitch < Standing.AccumulatedPitch);

	// --- 倍率必须来自资产，不能在代码里兜底 ---
	TestTrue(FString::Printf(TEXT("Standing multiplier comes from the asset (%.4f == %.4f)"),
		Standing.Multiplier, Profile->PoseMultiplier_Standing),
		FMath::IsNearlyEqual(Standing.Multiplier, Profile->PoseMultiplier_Standing, RatioTolerance));
	TestTrue(FString::Printf(TEXT("Crouching multiplier comes from the asset (%.4f == %.4f)"),
		Crouching.Multiplier, Profile->PoseMultiplier_Crouching),
		FMath::IsNearlyEqual(Crouching.Multiplier, Profile->PoseMultiplier_Crouching, RatioTolerance));
	TestTrue(FString::Printf(TEXT("JumpingOrFalling multiplier comes from the asset (%.4f == %.4f)"),
		Jumping.Multiplier, Profile->PoseMultiplier_JumpingOrFalling),
		FMath::IsNearlyEqual(Jumping.Multiplier, Profile->PoseMultiplier_JumpingOrFalling, RatioTolerance));
	TestTrue(FString::Printf(TEXT("Aiming multiplier comes from the asset (%.4f == %.4f)"),
		Aiming.Multiplier, Profile->PoseMultiplier_Aiming),
		FMath::IsNearlyEqual(Aiming.Multiplier, Profile->PoseMultiplier_Aiming, RatioTolerance));

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 非线性回正曲线
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilPoseRecoveryShapeTest, "Lyra.Recoil.Pose.RecoveryCurveShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilPoseRecoveryShapeTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilPoseTest;

	// 造三份只有 RecoveryCurve 不同的 Profile，其余参数完全一致
	auto MakeCurveProfile = [](const TArray<TPair<float, float>>& Keys) -> ULyraRecoilProfile*
	{
		ULyraRecoilProfile* Profile = NewObject<ULyraRecoilProfile>(GetTransientPackage());

		Profile->RecoilPerShot_Vertical = 0.5f;
		Profile->RecoilPerShot_Horizontal = 0.25f;

		Profile->VerticalKickCurve.EditorCurveData.Reset();
		Profile->VerticalKickCurve.EditorCurveData.AddKey(0.0f, 1.0f);

		Profile->RecoveryCurve.EditorCurveData.Reset();
		for (const TPair<float, float>& Key : Keys)
		{
			Profile->RecoveryCurve.EditorCurveData.AddKey(Key.Key, Key.Value);
		}

		Profile->RecoveryDelay = 0.1f;
		Profile->RecoveryTime = 0.4f;
		Profile->RecoilReturnRatio = 0.0f;   // 完全回正，便于看回正进程本身
		Profile->MaxVerticalKick = 1000.0f;
		Profile->MaxHorizontalKick = 1000.0f;

		Profile->PatternPoints.Reset();
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Profile->PatternPoints.Emplace(0.0f, 1.0f);
		}
		Profile->PatternLength = 8;

		Profile->RandomSeedMode = ERecoilRandomSeedMode::Fixed;
		Profile->FixedRandomSeed = 12345;

		return Profile;
	};

	// "快回—慢回"：前期回正快，后期拖尾
	ULyraRecoilProfile* FastThenSlow = MakeCurveProfile({ {0.0f, 0.0f}, {0.25f, 0.65f}, {1.0f, 1.0f} });
	// "慢回—快回"：前期拖，后期收
	ULyraRecoilProfile* SlowThenFast = MakeCurveProfile({ {0.0f, 0.0f}, {0.25f, 0.05f}, {1.0f, 1.0f} });
	// 线性基准
	ULyraRecoilProfile* Linear = MakeCurveProfile({ {0.0f, 0.0f}, {1.0f, 1.0f} });

	// 曲线求值必须先符合设计意图
	const float FastAtHalf = FastThenSlow->GetRecoveryAlpha(0.5f);
	const float SlowAtHalf = SlowThenFast->GetRecoveryAlpha(0.5f);
	const float LinearAtHalf = Linear->GetRecoveryAlpha(0.5f);

	AddInfo(FString::Printf(TEXT("RecoveryProgress at normalized t=0.5: fast=%.4f linear=%.4f slow=%.4f"),
		FastAtHalf, LinearAtHalf, SlowAtHalf));

	TestTrue(FString::Printf(TEXT("Fast-then-slow is above linear at t=0.5 (%.4f > %.4f)"), FastAtHalf, LinearAtHalf),
		FastAtHalf > LinearAtHalf + 0.1f);
	TestTrue(FString::Printf(TEXT("Slow-then-fast is below linear at t=0.5 (%.4f < %.4f)"), SlowAtHalf, LinearAtHalf),
		SlowAtHalf < LinearAtHalf - 0.1f);
	TestTrue(FString::Printf(TEXT("The two shapes are clearly distinguishable (%.4f vs %.4f)"), FastAtHalf, SlowAtHalf),
		(FastAtHalf - SlowAtHalf) > 0.3f);

	// 再走一遍完整的时间轴，确认运行时真的用上了曲线（而不是只查了曲线表）
	auto TickToHalfRecovery = [this](ULyraRecoilProfile& Profile) -> float
	{
		FRecoilRuntimeState State;
		State.Reset(&Profile);

		for (int32 Shot = 0; Shot < 10; ++Shot)
		{
			State.ApplyShot(&Profile, 1.0f);
		}

		// 推进到 RecoveryDelay + RecoveryTime/2
		const float TargetSeconds = Profile.RecoveryDelay + Profile.RecoveryTime * 0.5f;
		constexpr float StepSeconds = 1.0f / 240.0f;
		const int32 NumSteps = FMath::CeilToInt(TargetSeconds / StepSeconds);

		for (int32 Step = 0; Step < NumSteps; ++Step)
		{
			State.Advance(&Profile, StepSeconds);
		}

		TestTrue(TEXT("State is Recovering at the mid-point"), State.State == ERecoilState::Recovering);
		return State.RecoveryProgress;
	};

	const float FastRuntime = TickToHalfRecovery(*FastThenSlow);
	const float SlowRuntime = TickToHalfRecovery(*SlowThenFast);
	const float LinearRuntime = TickToHalfRecovery(*Linear);

	AddInfo(FString::Printf(TEXT("Runtime RecoveryProgress at half time: fast=%.4f linear=%.4f slow=%.4f"),
		FastRuntime, LinearRuntime, SlowRuntime));

	TestTrue(FString::Printf(TEXT("Runtime fast-then-slow progress > linear (%.4f > %.4f)"), FastRuntime, LinearRuntime),
		FastRuntime > LinearRuntime);
	TestTrue(FString::Printf(TEXT("Runtime slow-then-fast progress < linear (%.4f < %.4f)"), SlowRuntime, LinearRuntime),
		SlowRuntime < LinearRuntime);
	TestTrue(FString::Printf(TEXT("Runtime shapes differ by more than 0.25 (%.4f vs %.4f)"), FastRuntime, SlowRuntime),
		(FastRuntime - SlowRuntime) > 0.25f);

	// 两条曲线最终都必须回正到 0（RecoilReturnRatio = 0）
	for (ULyraRecoilProfile* Profile : { FastThenSlow, SlowThenFast, Linear })
	{
		FRecoilRuntimeState State;
		State.Reset(Profile);
		for (int32 Shot = 0; Shot < 10; ++Shot)
		{
			State.ApplyShot(Profile, 1.0f);
		}
		for (int32 Step = 0; Step < 240; ++Step)   // 1 秒 > delay + time
		{
			State.Advance(Profile, 1.0f / 240.0f);
		}

		TestTrue(FString::Printf(TEXT("Profile %s fully returns to zero"), *Profile->GetName()),
			FMath::IsNearlyZero(State.AccumulatedPitch, 1e-4f));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
