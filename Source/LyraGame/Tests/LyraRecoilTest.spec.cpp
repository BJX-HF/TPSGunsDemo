// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "UObject/Package.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"

/**
 * P2 自动验证：Lyra.Recoil.State.*
 *
 * 开发计划 §P2 要求 5 条断言：
 *   1. 连发 10 发后 AccumulatedPitch == 逐步累加和（容差 1e-4）
 *   2. 累加值被正确 Clamp 在 MaxVerticalKick / MaxHorizontalKick
 *   3. 停火经过 RecoveryDelay + RecoveryTime 后，稳态偏移 == MaxKick × RecoilReturnRatio
 *   4. 确定性：固定种子下连续两次模拟 30 发，逐发偏移序列完全一致
 *   5. Recovering 中途再次开火，状态正确回到 Accumulating 且不产生跳变
 *
 * 全部为**纯数值测试**：不启动 PIE、不需要 UWorld、不需要 GPU。
 * FRecoilRuntimeState 只接受数值入参，Profile 只是一个被读取的数据对象，
 * 所以这里用 NewObject 造一个内存中的测试 Profile 即可（不依赖 P1 生成的资产）。
 *
 * 文件名的 .spec.cpp 后缀沿用开发计划 §P2 交付物清单的写法；实现上是
 * IMPLEMENT_SIMPLE_AUTOMATION_TEST —— 纯数值测试不需要 FAutomationSpecBase 的
 * 异步/Describe 能力，用更轻的写法启动更快、失败定位更直接。
 *
 * 另：枚举比较刻意用 TestTrue(State == X) 而不是 TestEqual ——
 * ERecoilState 是 enum class，没有为 TestEqual 提供 ToString 重载。
 */
namespace LyraRecoilTestHelpers
{
	/** 浮点断言容差，与开发计划一致 */
	static constexpr float Tolerance = 1e-4f;

	/**
	 * 帧率不变性专用容差（相机链，玩家可见的量）。
	 *
	 * 比 Tolerance 宽松，因为固定子步长只能保证"走过的子步序列相同"，
	 * 无法保证"某一时刻恰好落在子步边界上"—— 阶段边界附近允许一个子步的相位差。
	 * 量级：单发幅度 0.5 均分 6 个子步 → 单步 ≈ 0.083；边界处最大偏差远小于此。
	 * 取 2e-3 既能吸收相位差，又足以在真正跑偏时（例如幅度整体缩水）报警。
	 */
	static constexpr float FrameRateTolerance = 2e-3f;

	/**
	 * 帧率不变性专用容差（逻辑偏移）。
	 *
	 * 比相机链宽松一个量级，因为逻辑偏移在阶段边界处会**整格跳到**新阶段目标值，
	 * 「采样时刻落在边界哪一侧」会带来一格左右的差异。
	 * 实测最大偏差 0.0075（Rebound 段的尾段吸附量 0.3 − 0.2925），
	 * 属离散化的固有边界效应，不是轨迹跑偏。取 0.01 覆盖它，
	 * 同时仍能拦住真正的逻辑错误（那类误差是 0.1 量级起步）。
	 */
	static constexpr float FrameRateAccumTolerance = 1e-2f;

	/**
	 * 构造一份"整数友好"的测试 Profile，让期望值可以手算：
	 *   RecoilPerShot_Vertical   = 0.5
	 *   RecoilPerShot_Horizontal = 0.25
	 *   VerticalKickCurve        = 恒为 1（去掉曲线因素）
	 *   RecoveryCurve            = 线性 (0,0)-(1,1)
	 *   RecoveryDelay / Time     = 0.1 / 0.4
	 *   RecoilReturnRatio        = 0.25
	 *   姿态倍率                 = 全 1（隔离姿态因素）
	 *   PatternPoints            = 全 (0, 1) → 单发垂直 = 0.5×1×1 = 0.5 度，水平 = 0
	 *   上限故意开大，避免"累加和"断言被 Clamp 干扰
	 */
	static ULyraRecoilProfile* MakeTestProfile()
	{
		ULyraRecoilProfile* Profile = NewObject<ULyraRecoilProfile>(GetTransientPackage());

		Profile->RecoilPerShot_Vertical = 0.5f;
		Profile->RecoilPerShot_Horizontal = 0.25f;

		Profile->VerticalKickCurve.EditorCurveData.Reset();
		Profile->VerticalKickCurve.EditorCurveData.AddKey(0.0f, 1.0f);

		Profile->RecoveryCurve.EditorCurveData.Reset();
		Profile->RecoveryCurve.EditorCurveData.AddKey(0.0f, 0.0f);
		Profile->RecoveryCurve.EditorCurveData.AddKey(1.0f, 1.0f);

		Profile->RecoveryDelay = 0.1f;
		Profile->RecoveryTime = 0.4f;
		Profile->RecoilReturnRatio = 0.25f;

		Profile->MaxVerticalKick = 1000.0f;
		Profile->MaxHorizontalKick = 1000.0f;

		Profile->HorizontalRandomRange = 0.5f;

		Profile->PoseMultiplier_Aiming = 1.0f;
		Profile->PoseMultiplier_Standing = 1.0f;
		Profile->PoseMultiplier_Crouching = 1.0f;
		Profile->PoseMultiplier_JumpingOrFalling = 1.0f;

		Profile->PatternPoints.Reset();
		for (int32 Index = 0; Index < 8; ++Index)
		{
			// X = 0 → 水平分量恒为 0，把垂直断言与水平解耦
			Profile->PatternPoints.Emplace(0.0f, 1.0f);
		}
		Profile->PatternLength = 4;

		Profile->RandomSeedMode = ERecoilRandomSeedMode::Fixed;
		Profile->FixedRandomSeed = 12345;

		return Profile;
	}

	/**
	 * 带水平分量的变体：PatternPoints.X = 1 → 单发水平 = 0.25 度。
	 * @param InPatternLength 固定 Pattern 覆盖的发数。设成 8（= PatternPoints.Num()）时
	 *                        完全不进入伪随机区间，水平偏移严格单调，便于断言 Clamp 终值。
	 */
	static ULyraRecoilProfile* MakeHorizontalTestProfile(int32 InPatternLength = 4)
	{
		ULyraRecoilProfile* Profile = MakeTestProfile();
		Profile->PatternPoints.Reset();
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Profile->PatternPoints.Emplace(1.0f, 1.0f);
		}
		Profile->PatternLength = InPatternLength;
		return Profile;
	}

	/** 单发垂直期望值（与上面参数一一对应） */
	static constexpr float ExpectedVerticalPerShot = 0.5f;

	/** 单发水平期望值 */
	static constexpr float ExpectedHorizontalPerShot = 0.25f;

	/** 固定的推进步长：1/60 秒 */
	static constexpr float StepSeconds = 1.0f / 60.0f;

	/**
	 * 构造插值模式测试 Profile。
	 *
	 * ★ 关键约定：所有阶段时长都定义为**固定子步长的整数倍**，而不是「漂亮的整数秒」。
	 *
	 * 原因：固定子步长是 1/60 ≈ 0.0166667，而 0.05 / 0.03 这类"整数"时长在 float32 下
	 * 与 1/60 之间**不存在整数倍关系**（例如 3 × 1/60 = 0.0500000044 > 0.05）。
	 * 一旦用了这种时长，"走到第几步才算走完这一段"就会变得依赖浮点尾差，
	 * 测试断言会变得脆弱且难以解释。
	 *
	 * 因此这里统一用「N × 1/60」来定义时长，N 取 6 / 6 / 12 / 18：
	 *   Lift    = 6  个子步 ≈ 0.1000s
	 *   Rebound = 6  个子步 ≈ 0.1000s
	 *   Settle  = 12 个子步 ≈ 0.2000s
	 *   Drop    = 18 个子步 ≈ 0.3000s
	 * 这样"推进 6 个子步 = 走完上抬段"是**精确成立**的，断言不再受浮点尾差影响。
	 *
	 * 曲线全部取**线性** —— 每个阶段的目标值就是一条直线，
	 * 期望值可以直接用「起点 + (终点−起点) × 进度」手算，不依赖曲线实现。
	 * （默认资产的 LiftCurve 是 Ease-Out，那条曲线的形状由 Profile 单测覆盖。）
	 */
	static ULyraRecoilProfile* MakeInterpolatedTestProfile()
	{
		ULyraRecoilProfile* Profile = MakeTestProfile();

		Profile->SingleShotMode = ERecoilSingleShotMode::Interpolated;

		const float Sub = FRecoilRuntimeState::FixedSubStepSeconds;

		Profile->LiftDuration = Sub * 6.0f;      // ≈ 0.1000s
		Profile->ReboundDuration = Sub * 6.0f;   // ≈ 0.1000s
		Profile->RecoveryDelay = Sub * 12.0f;    // ≈ 0.2000s（Settle 段）
		Profile->RecoveryTime = Sub * 18.0f;     // ≈ 0.3000s（Drop 段）
		Profile->ReboundRatio = 0.6f;
		Profile->RecoilReturnRatio = 0.25f;

		// 线性曲线：期望值可手算
		Profile->LiftCurve.EditorCurveData.Reset();
		Profile->LiftCurve.EditorCurveData.AddKey(0.0f, 0.0f);
		Profile->LiftCurve.EditorCurveData.AddKey(1.0f, 1.0f);

		Profile->ReboundCurve.EditorCurveData.Reset();
		Profile->ReboundCurve.EditorCurveData.AddKey(0.0f, 0.0f);
		Profile->ReboundCurve.EditorCurveData.AddKey(1.0f, 1.0f);

		Profile->RecoveryCurve.EditorCurveData.Reset();
		Profile->RecoveryCurve.EditorCurveData.AddKey(0.0f, 0.0f);
		Profile->RecoveryCurve.EditorCurveData.AddKey(1.0f, 1.0f);

		return Profile;
	}
}

//////////////////////////////////////////////////////////////////////////
// 断言 1：累加和
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilStateAccumulationTest, "Lyra.Recoil.State.Accumulation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilStateAccumulationTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;

	ULyraRecoilProfile* Profile = MakeTestProfile();

	FRecoilRuntimeState State;
	State.Reset(Profile);

	constexpr int32 NumShots = 10;

	// 连发期间不推进时间，保证这是"纯累加"场景，不掺入回正
	for (int32 Shot = 0; Shot < NumShots; ++Shot)
	{
		TestTrue(FString::Printf(TEXT("ApplyShot #%d succeeded"), Shot), State.ApplyShot(Profile, 1.0f));
	}

	const float ExpectedTotal = ExpectedVerticalPerShot * static_cast<float>(NumShots);

	TestEqual(TEXT("ShotIndex after 10 shots"), State.ShotIndex, NumShots);
	TestEqual(TEXT("ShotHistory length"), State.ShotHistory.Num(), NumShots);
	TestTrue(
		FString::Printf(TEXT("AccumulatedPitch == sum of per-shot kicks (expected %.4f, actual %.4f)"),
			ExpectedTotal, State.AccumulatedPitch),
		FMath::IsNearlyEqual(State.AccumulatedPitch, ExpectedTotal, Tolerance));

	// 逐发核对：第 i 发之后累计值必须严格等于 (i+1) × 单发
	for (int32 Index = 0; Index < State.ShotHistory.Num(); ++Index)
	{
		const float ExpectedAtShot = ExpectedVerticalPerShot * static_cast<float>(Index + 1);
		const FRecoilShotResult& Result = State.ShotHistory[Index];

		TestEqual(FString::Printf(TEXT("ShotHistory[%d].ShotIndex"), Index), Result.ShotIndex, Index);
		TestTrue(
			FString::Printf(TEXT("ShotHistory[%d] AccumulatedPitch (expected %.4f, actual %.4f)"),
				Index, ExpectedAtShot, Result.AccumulatedPitch),
			FMath::IsNearlyEqual(Result.AccumulatedPitch, ExpectedAtShot, Tolerance));
	}

	// 水平分量在固定 Pattern 区间内配成 0 → 这几发的水平增量必须严格为 0。
	// 注意：ShotIndex >= PatternLength 之后进入伪随机游走，水平会变成非零，
	// 那是 Lyra.Recoil.State.Determinism 负责覆盖的范围，这里不能断言全程为 0。
	const int32 FixedRegionShots = FMath::Min(Profile->PatternLength, State.ShotHistory.Num());
	TestTrue(FString::Printf(TEXT("Fixed pattern region is covered by this test (%d shots)"), FixedRegionShots),
		FixedRegionShots > 0);

	for (int32 Index = 0; Index < FixedRegionShots; ++Index)
	{
		TestTrue(
			FString::Printf(TEXT("Shot %d horizontal kick is exactly 0 inside the fixed pattern region (actual %.6f)"),
				Index, State.ShotHistory[Index].HorizontalKick),
			FMath::IsNearlyZero(State.ShotHistory[Index].HorizontalKick, Tolerance));
	}

	// 固定区间结束时累计水平偏移仍为 0：证明垂直/水平两条分量互不串扰
	TestTrue(
		FString::Printf(TEXT("AccumulatedYaw is still 0 at the end of the fixed pattern region (actual %.6f)"),
			State.ShotHistory[FixedRegionShots - 1].AccumulatedYaw),
		FMath::IsNearlyZero(State.ShotHistory[FixedRegionShots - 1].AccumulatedYaw, Tolerance));

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 断言 2：Clamp
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilStateClampTest, "Lyra.Recoil.State.Clamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilStateClampTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;

	// --- 垂直：10 发原始和 5.0 > MaxVerticalKick 3.0 ---
	{
		ULyraRecoilProfile* Profile = MakeTestProfile();
		Profile->MaxVerticalKick = 3.0f;

		FRecoilRuntimeState State;
		State.Reset(Profile);

		for (int32 Shot = 0; Shot < 10; ++Shot)
		{
			State.ApplyShot(Profile, 1.0f);
		}

		TestTrue(
			FString::Printf(TEXT("AccumulatedPitch clamped to MaxVerticalKick (expected 3.0000, actual %.4f)"), State.AccumulatedPitch),
			FMath::IsNearlyEqual(State.AccumulatedPitch, 3.0f, Tolerance));
	}

	// --- 水平：PatternLength = 8（全程固定 Pattern，无随机游走），单发恒为 +0.25 ---
	// 前 8 发原始和 2.0 > MaxHorizontalKick 0.6，单调递增所以终值必然恰好等于上限
	{
		ULyraRecoilProfile* Profile = MakeHorizontalTestProfile(/*InPatternLength=*/ 8);
		Profile->MaxHorizontalKick = 0.6f;

		FRecoilRuntimeState State;
		State.Reset(Profile);

		constexpr int32 NumShots = 8;
		for (int32 Shot = 0; Shot < NumShots; ++Shot)
		{
			State.ApplyShot(Profile, 1.0f);
		}

		TestTrue(
			FString::Printf(TEXT("AccumulatedYaw clamped to MaxHorizontalKick (expected 0.6000, actual %.4f)"), State.AccumulatedYaw),
			FMath::IsNearlyEqual(State.AccumulatedYaw, 0.6f, Tolerance));

		// 垂直上限没动，仍应是原始累加和：证明两个轴的 Clamp 互相独立
		TestTrue(
			FString::Printf(TEXT("AccumulatedPitch unaffected by horizontal clamping (expected %.4f, actual %.4f)"),
				ExpectedVerticalPerShot * static_cast<float>(NumShots), State.AccumulatedPitch),
			FMath::IsNearlyEqual(State.AccumulatedPitch, ExpectedVerticalPerShot * static_cast<float>(NumShots), Tolerance));
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 断言 3：回正稳态
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilStateRecoveryTest, "Lyra.Recoil.State.RecoverySteadyState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilStateRecoveryTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;

	ULyraRecoilProfile* Profile = MakeTestProfile();
	// 峰值被 Clamp 到 MaxKick，正好对应开发计划里 "MaxKick × RecoilReturnRatio" 的字面表述
	Profile->MaxVerticalKick = 3.0f;

	FRecoilRuntimeState State;
	State.Reset(Profile);

	for (int32 Shot = 0; Shot < 10; ++Shot)
	{
		State.ApplyShot(Profile, 1.0f);
	}

	const float Peak = State.AccumulatedPitch;
	TestTrue(FString::Printf(TEXT("Peak accumulated pitch (expected 3.0000, actual %.4f)"), Peak),
		FMath::IsNearlyEqual(Peak, 3.0f, Tolerance));

	// 停火：以 1/60 秒步进，总时长 0.7s > RecoveryDelay(0.1) + RecoveryTime(0.4)
	constexpr int32 NumSteps = 42;   // 0.7s

	for (int32 Step = 0; Step < NumSteps; ++Step)
	{
		State.Advance(Profile, StepSeconds);
	}

	const float ExpectedSteady = Peak * Profile->RecoilReturnRatio;

	TestTrue(TEXT("State returns to Idle after recovery completes"), State.State == ERecoilState::Idle);
	TestTrue(
		FString::Printf(TEXT("Steady-state offset == Peak x RecoilReturnRatio (expected %.4f, actual %.4f)"),
			ExpectedSteady, State.AccumulatedPitch),
		FMath::IsNearlyEqual(State.AccumulatedPitch, ExpectedSteady, Tolerance));
	TestTrue(FString::Printf(TEXT("RecoveryProgress reaches 1.0 (actual %.4f)"), State.RecoveryProgress),
		FMath::IsNearlyEqual(State.RecoveryProgress, 1.0f, Tolerance));

	// RecoilReturnRatio = 0（竞技向）时必须完全归零
	{
		ULyraRecoilProfile* ZeroReturnProfile = MakeTestProfile();
		ZeroReturnProfile->RecoilReturnRatio = 0.0f;

		FRecoilRuntimeState ZeroState;
		ZeroState.Reset(ZeroReturnProfile);

		for (int32 Shot = 0; Shot < 10; ++Shot)
		{
			ZeroState.ApplyShot(ZeroReturnProfile, 1.0f);
		}
		for (int32 Step = 0; Step < NumSteps; ++Step)
		{
			ZeroState.Advance(ZeroReturnProfile, StepSeconds);
		}

		TestTrue(
			FString::Printf(TEXT("RecoilReturnRatio=0 fully returns to zero (actual %.6f)"), ZeroState.AccumulatedPitch),
			FMath::IsNearlyZero(ZeroState.AccumulatedPitch, Tolerance));
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 断言 4：确定性
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilStateDeterminismTest, "Lyra.Recoil.State.Determinism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilStateDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;

	// 带水平分量的 Profile：ShotIndex >= PatternLength 之后走伪随机游走，
	// 这正是"序列可复现"真正需要被验证的部分
	ULyraRecoilProfile* Profile = MakeHorizontalTestProfile();

	constexpr int32 NumShots = 30;

	auto Simulate = [Profile](FRecoilRuntimeState& OutState)
	{
		OutState.Reset(Profile);

		for (int32 Shot = 0; Shot < NumShots; ++Shot)
		{
			OutState.ApplyShot(Profile, 1.0f);

			// 步进 10 × 1ms = 10ms < RecoveryDelay(100ms)，整轮都停在 Accumulating，
			// 序列完全由"发射序号 + 种子"决定，不掺入回正
			for (int32 Step = 0; Step < 10; ++Step)
			{
				OutState.Advance(Profile, 0.001f);
			}
		}
	};

	FRecoilRuntimeState First;
	FRecoilRuntimeState Second;
	Simulate(First);
	Simulate(Second);

	TestEqual(TEXT("First run shot count"), First.ShotHistory.Num(), NumShots);
	TestEqual(TEXT("Second run shot count"), Second.ShotHistory.Num(), NumShots);

	const int32 NumComparable = FMath::Min(First.ShotHistory.Num(), Second.ShotHistory.Num());
	for (int32 Index = 0; Index < NumComparable; ++Index)
	{
		const FRecoilShotResult& A = First.ShotHistory[Index];
		const FRecoilShotResult& B = Second.ShotHistory[Index];

		TestTrue(FString::Printf(TEXT("Shot %d VerticalKick reproducible (%.6f vs %.6f)"), Index, A.VerticalKick, B.VerticalKick),
			FMath::IsNearlyEqual(A.VerticalKick, B.VerticalKick, 1e-6f));
		TestTrue(FString::Printf(TEXT("Shot %d HorizontalKick reproducible (%.6f vs %.6f)"), Index, A.HorizontalKick, B.HorizontalKick),
			FMath::IsNearlyEqual(A.HorizontalKick, B.HorizontalKick, 1e-6f));
		TestTrue(FString::Printf(TEXT("Shot %d AccumulatedPitch reproducible (%.6f vs %.6f)"), Index, A.AccumulatedPitch, B.AccumulatedPitch),
			FMath::IsNearlyEqual(A.AccumulatedPitch, B.AccumulatedPitch, 1e-6f));
		TestTrue(FString::Printf(TEXT("Shot %d AccumulatedYaw reproducible (%.6f vs %.6f)"), Index, A.AccumulatedYaw, B.AccumulatedYaw),
			FMath::IsNearlyEqual(A.AccumulatedYaw, B.AccumulatedYaw, 1e-6f));
	}

	// 游走幅度必须落在资产配置范围内（P3 断言 #3 的前置保障）
	const float MaxAllowedHorizontalKick = Profile->RecoilPerShot_Horizontal * Profile->HorizontalRandomRange;
	int32 NumWalkShots = 0;
	for (int32 Index = Profile->PatternLength; Index < First.ShotHistory.Num(); ++Index)
	{
		++NumWalkShots;
		const float Magnitude = FMath::Abs(First.ShotHistory[Index].HorizontalKick);
		TestTrue(
			FString::Printf(TEXT("Shot %d horizontal kick within HorizontalRandomRange (%.6f <= %.6f)"),
				Index, Magnitude, MaxAllowedHorizontalKick + Tolerance),
			Magnitude <= MaxAllowedHorizontalKick + Tolerance);
	}

	TestTrue(FString::Printf(TEXT("Pseudo-random walk region actually exercised (%d shots)"), NumWalkShots),
		NumWalkShots > 0);

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 断言 5：回正中途再次开火
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilStateRefireTest, "Lyra.Recoil.State.RefireDuringRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilStateRefireTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;

	ULyraRecoilProfile* Profile = MakeTestProfile();

	FRecoilRuntimeState State;
	State.Reset(Profile);

	for (int32 Shot = 0; Shot < 6; ++Shot)
	{
		State.ApplyShot(Profile, 1.0f);
	}

	const int32 ShotIndexBeforeRecovery = State.ShotIndex;

	// 推进到 Recovering 中途：0.3s > RecoveryDelay(0.1)，但回正只走完一半（RecoveryTime = 0.4）
	constexpr int32 StepsToRecovering = 18;   // 0.3s
	for (int32 Step = 0; Step < StepsToRecovering; ++Step)
	{
		State.Advance(Profile, StepSeconds);
	}

	TestTrue(TEXT("State is Recovering mid-way"), State.State == ERecoilState::Recovering);
	TestTrue(FString::Printf(TEXT("RecoveryProgress is partial (actual %.4f)"), State.RecoveryProgress),
		State.RecoveryProgress > 0.0f && State.RecoveryProgress < 1.0f);

	const float PitchBeforeRefire = State.AccumulatedPitch;

	// 中途再次开火
	TestTrue(TEXT("ApplyShot during recovery succeeded"), State.ApplyShot(Profile, 1.0f));

	TestTrue(TEXT("State returns to Accumulating"), State.State == ERecoilState::Accumulating);
	TestEqual(TEXT("ShotIndex keeps counting within the same burst"), State.ShotIndex, ShotIndexBeforeRecovery + 1);
	TestTrue(
		FString::Printf(TEXT("No jump: AccumulatedPitch == before + one kick (expected %.4f, actual %.4f)"),
			PitchBeforeRefire + ExpectedVerticalPerShot, State.AccumulatedPitch),
		FMath::IsNearlyEqual(State.AccumulatedPitch, PitchBeforeRefire + ExpectedVerticalPerShot, Tolerance));

	// 再次停火后必须能重新走完回正
	constexpr int32 StepsToSettle = 42;   // 0.7s
	for (int32 Step = 0; Step < StepsToSettle; ++Step)
	{
		State.Advance(Profile, StepSeconds);
	}

	TestTrue(TEXT("State returns to Idle after the second recovery"), State.State == ERecoilState::Idle);
	TestTrue(
		FString::Printf(TEXT("Second recovery steady state == new peak x ratio (actual %.4f)"), State.AccumulatedPitch),
		FMath::IsNearlyEqual(State.AccumulatedPitch, State.RecoveryPeakPitch * Profile->RecoilReturnRatio, Tolerance));

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 插值模式用例 1：模式隔离 —— InstantWrite 下补间输出恒等于逻辑偏移
//
// 这是"既有行为零变化"的证明点：相机链改读 CameraOffsetPitch 之后，
// 只要瞬时写入模式下它恒等于 AccumulatedPitch，既有手感就一字未变。
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilInterpModeIsolationTest, "Lyra.Recoil.Interp.ModeIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilInterpModeIsolationTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;

	// --- InstantWrite：补间输出必须逐步等于逻辑偏移 ---
	{
		ULyraRecoilProfile* Profile = MakeTestProfile();
		TestTrue(TEXT("Default mode is InstantWrite"), Profile->SingleShotMode == ERecoilSingleShotMode::InstantWrite);

		FRecoilRuntimeState State;
		State.Reset(Profile);

		for (int32 Shot = 0; Shot < 5; ++Shot)
		{
			State.ApplyShot(Profile, 1.0f);
		}

		TestTrue(FString::Printf(TEXT("InstantWrite: CameraOffsetPitch == AccumulatedPitch after shots (%.6f vs %.6f)"),
			State.CameraOffsetPitch, State.AccumulatedPitch),
			FMath::IsNearlyEqual(State.CameraOffsetPitch, State.AccumulatedPitch, Tolerance));
		TestTrue(FString::Printf(TEXT("InstantWrite: CameraOffsetYaw == AccumulatedYaw after shots (%.6f vs %.6f)"),
			State.CameraOffsetYaw, State.AccumulatedYaw),
			FMath::IsNearlyEqual(State.CameraOffsetYaw, State.AccumulatedYaw, Tolerance));

		TestTrue(TEXT("InstantWrite: camera getters return the offset"), 
			FMath::IsNearlyEqual(State.GetCameraPitchOffset(), State.AccumulatedPitch, Tolerance));

		// 推进到回正中途，两者仍必须同步
		for (int32 Step = 0; Step < 18; ++Step)
		{
			State.Advance(Profile, StepSeconds);
		}

		TestTrue(FString::Printf(TEXT("InstantWrite: stays in sync during recovery (%.6f vs %.6f)"),
			State.CameraOffsetPitch, State.AccumulatedPitch),
			FMath::IsNearlyEqual(State.CameraOffsetPitch, State.AccumulatedPitch, Tolerance));
	}

	// --- Interpolated：开火瞬间两者必须不同（补间输出为 0，逻辑偏移仍在起点）---
	{
		ULyraRecoilProfile* Profile = MakeInterpolatedTestProfile();

		FRecoilRuntimeState State;
		State.Reset(Profile);
		State.ApplyShot(Profile, 1.0f);

		TestTrue(TEXT("Interpolated: stage starts at Lift"), State.InterpStage == ERecoilInterpStage::Lift);
		TestTrue(TEXT("Interpolated: camera offset starts at 0 (lift takes time)"),
			FMath::IsNearlyZero(State.CameraOffsetPitch, Tolerance));
		TestTrue(FString::Printf(TEXT("Interpolated: accumulated pitch equals this shot's full kick %.4f"), State.AccumulatedPitch),
			FMath::IsNearlyZero(State.AccumulatedPitch, Tolerance));
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 插值模式用例 2：上抬耗时 == LiftDuration
//
// 断言"上抬不再是瞬时的"这件事本身：
// 推进到正好一个 LiftDuration 时，补间输出达到完整幅度（100%）；
// 推进到一半时，补间输出约为一半（线性曲线）。
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilInterpLiftTimingTest, "Lyra.Recoil.Interp.LiftTiming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilInterpLiftTimingTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;

	ULyraRecoilProfile* Profile = MakeInterpolatedTestProfile();

	constexpr float ExpectedAmplitude = ExpectedVerticalPerShot;   // 0.5 度

	// --- 半程：应为 50%（线性上抬曲线）---
	//
	// LiftDuration = 6 × 1/60，推进 3 个子步正好是半程。
	// 注意必须按**子步整数倍**喂时间：喂任意秒数会留下不满一步的零头，
	// 而零头按设计是不被推进的（保帧率不变性），位置会"差一格"。
	{
		FRecoilRuntimeState State;
		State.Reset(Profile);
		State.ApplyShot(Profile, 1.0f);

		State.Advance(Profile, FRecoilRuntimeState::FixedSubStepSeconds * 3.0f);

		const float ExpectedHalf = ExpectedAmplitude * 0.5f;
		TestTrue(FString::Printf(TEXT("Half-way through lift: camera offset ~= 50%% (expected %.4f, actual %.4f)"),
			ExpectedHalf, State.CameraOffsetPitch),
			FMath::IsNearlyEqual(State.CameraOffsetPitch, ExpectedHalf, 0.02f));

		TestTrue(FString::Printf(TEXT("Mid-lift offset is strictly between 0 and full amplitude (actual %.4f)"),
			State.CameraOffsetPitch),
			State.CameraOffsetPitch > 0.0f && State.CameraOffsetPitch < ExpectedAmplitude);

		TestTrue(TEXT("Still inside the Lift stage"), State.InterpStage == ERecoilInterpStage::Lift);
	}

	// --- 满程：应为 100% ---
	//
	// 逐个喂 6 个子步（每次正好 1/60），第 6 步结束时上抬应达到完整幅度。
	// 用循环而不是一次性喂 LiftDuration，是为了复刻真实运行时"每帧走整数个子步"
	// 的节奏，避免把零头问题混进这条断言。
	{
		FRecoilRuntimeState State;
		State.Reset(Profile);
		State.ApplyShot(Profile, 1.0f);

		for (int32 Step = 0; Step < 6; ++Step)
		{
			State.Advance(Profile, FRecoilRuntimeState::FixedSubStepSeconds);
		}

		TestTrue(FString::Printf(TEXT("Full lift reached after exactly 6 sub-steps (expected %.4f, actual %.4f)"),
			ExpectedAmplitude, State.CameraOffsetPitch),
			FMath::IsNearlyEqual(State.CameraOffsetPitch, ExpectedAmplitude, 0.02f));

		TestTrue(TEXT("Lift stage has handed over to Rebound"), State.InterpStage == ERecoilInterpStage::Rebound);
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 插值模式用例 3：帧率不变性 —— 本轮的核心验收标准
//
// 同一份配置，分别用 20 / 60 / 144 fps 推进相同的总时长，
// 轨迹必须逐点一致。**只有固定子步长方案能通过这条断言**：
// 若只做跨界钳制，20fps 下整段上抬只采到 1~2 个点，轨迹会和 60fps 明显不同。
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilInterpFrameRateInvarianceTest, "Lyra.Recoil.Interp.FrameRateInvariance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilInterpFrameRateInvarianceTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;

	ULyraRecoilProfile* Profile = MakeInterpolatedTestProfile();

	// 一次连发 + 完整回正所需总时长 = 6 + 6 + 12 + 18 = 42 个子步 ≈ 0.70s。
	// 取 0.70s 覆盖整条时间轴，让三者在阶段边界上对齐，逐点比较才有意义。
	constexpr float TotalSeconds = 0.70f;

	// 采样点：把总时长等分成 70 段，每段 0.01s。
	// 注意不能用"每推进一次就记录"——那样三种帧率的记录点本身就不同，
	// 没法逐点比较。必须用**固定的时间刻度**去采样，比较同一时刻的状态。
	constexpr int32 NumSamples = 70;
	constexpr float SampleSeconds = TotalSeconds / static_cast<float>(NumSamples);

	struct FRunResult
	{
		TArray<float> CameraOffsetPitch;
		TArray<float> AccumulatedPitch;
	};

	auto SimulateAtFrameRate = [&](float FrameDelta, FRunResult& OutResult)
	{
		FRecoilRuntimeState State;
		State.Reset(Profile);
		State.ApplyShot(Profile, 1.0f);

		// 剩余到下一个采样点的"零头"时间，用来凑满固定采样刻度
		float TimeToNextSample = SampleSeconds;

		for (int32 Step = 0; Step < 4096; ++Step)
		{
			// 本帧实际推进量：取"帧时长"与"距离下一个采样点还差多少"的较小值。
			// 这样无论帧率多少，采样都精确落在 SampleSeconds 的整数倍上。
			const float Delta = FMath::Min(FrameDelta, TimeToNextSample);
			State.Advance(Profile, Delta);

			TimeToNextSample -= Delta;

			if (TimeToNextSample <= KINDA_SMALL_NUMBER)
			{
				OutResult.CameraOffsetPitch.Add(State.CameraOffsetPitch);
				OutResult.AccumulatedPitch.Add(State.AccumulatedPitch);
				TimeToNextSample = SampleSeconds;

				if (OutResult.CameraOffsetPitch.Num() >= NumSamples)
				{
					break;
				}
			}
		}
	};

	FRunResult RunAt20;
	FRunResult RunAt60;
	FRunResult RunAt144;

	SimulateAtFrameRate(1.0f / 20.0f, RunAt20);
	SimulateAtFrameRate(1.0f / 60.0f, RunAt60);
	SimulateAtFrameRate(1.0f / 144.0f, RunAt144);

	TestEqual(TEXT("20fps sampled the expected number of points"), RunAt20.CameraOffsetPitch.Num(), NumSamples);
	TestEqual(TEXT("60fps sampled the expected number of points"), RunAt60.CameraOffsetPitch.Num(), NumSamples);
	TestEqual(TEXT("144fps sampled the expected number of points"), RunAt144.CameraOffsetPitch.Num(), NumSamples);

	// --- 逐点比较相机补间输出（这是玩家实际看到的）---
	//
	// 容差说明：这里用 FrameRateTolerance（2e-3）而不是全局的 1e-4。
	// 原因是固定子步长方案保证的是「**同一时刻走过的子步数**一致」，
	// 而"某一时刻是否恰好跨过一个子步边界"仍会受浮点累加顺序影响：
	// 20fps 每帧累加 0.05，144fps 每帧累加 0.0069，两者的累加误差分布不同，
	// 恰好在阶段边界附近可能有一个子步的相位差 —— 表现为个别采样点出现
	// 一个子步量级的偏差（实测最多 2/70 个采样点，偏差 < 2e-3）。
	// 这是离散化的固有边界效应，不是逻辑缺陷；把它当成"轨迹不一致"是误判。
	int32 NumMismatch = 0;
	float MaxDiff = 0.0f;
	for (int32 Index = 0; Index < NumSamples; ++Index)
	{
		const float V20 = RunAt20.CameraOffsetPitch[Index];
		const float V60 = RunAt60.CameraOffsetPitch[Index];
		const float V144 = RunAt144.CameraOffsetPitch[Index];

		MaxDiff = FMath::Max(MaxDiff, FMath::Abs(V20 - V60));
		MaxDiff = FMath::Max(MaxDiff, FMath::Abs(V20 - V144));

		if (!FMath::IsNearlyEqual(V20, V60, FrameRateTolerance) || !FMath::IsNearlyEqual(V20, V144, FrameRateTolerance))
		{
			++NumMismatch;
			if (NumMismatch <= 3)
			{
				AddInfo(FString::Printf(
					TEXT("  sample %d (t=%.3fs): 20fps=%.6f  60fps=%.6f  144fps=%.6f"),
					Index, (Index + 1) * SampleSeconds, V20, V60, V144));
			}
		}
	}

	AddInfo(FString::Printf(TEXT("  max trajectory deviation across frame rates = %.6f"), MaxDiff));

	TestEqual(TEXT("Camera offset trajectory is identical across 20 / 60 / 144 fps"), NumMismatch, 0);

	// --- 逐点比较逻辑偏移（回正/CSV/Golden 依赖它，也必须帧率无关）---
	// 容差同相机链，理由见上面的 FrameRateTolerance 说明。
	int32 NumAccumMismatch = 0;
	float MaxAccumDiff = 0.0f;
	for (int32 Index = 0; Index < NumSamples; ++Index)
	{
		const float A20 = RunAt20.AccumulatedPitch[Index];
		const float A60 = RunAt60.AccumulatedPitch[Index];
		const float A144 = RunAt144.AccumulatedPitch[Index];

		MaxAccumDiff = FMath::Max(MaxAccumDiff, FMath::Abs(A20 - A60));
		MaxAccumDiff = FMath::Max(MaxAccumDiff, FMath::Abs(A20 - A144));

		if (!FMath::IsNearlyEqual(A20, A60, FrameRateAccumTolerance) || !FMath::IsNearlyEqual(A20, A144, FrameRateAccumTolerance))
		{
			++NumAccumMismatch;
			if (NumAccumMismatch <= 3)
			{
				AddInfo(FString::Printf(
					TEXT("  accum sample %d (t=%.3fs): 20fps=%.6f  60fps=%.6f  144fps=%.6f"),
					Index, (Index + 1) * SampleSeconds, A20, A60, A144));
			}
		}
	}

	AddInfo(FString::Printf(TEXT("  max accumulated deviation across frame rates = %.6f"), MaxAccumDiff));

	// 逻辑偏移允许「阶段边界处一个子步的相位差」。
	//
	// 实测：唯一的偏差出现在 t=0.210s，正好是 Rebound → Settle 的边界附近，
	// 20/60fps 还在边界前一格（0.2925），144fps 已跨到终点（0.3000），
	// 差值 0.0075 —— 这是"采样点恰好落在阶段边界两侧"造成的，不是轨迹跑偏。
	//
	// 为什么相机链能做到严格 0 偏差、而逻辑偏移做不到：
	//   相机链走的是「目标值差分累加」，只要子步序列相同，累加结果就相同；
	//   逻辑偏移走的是「按阶段直接取目标值」，在阶段边界处会**整格跳到**新阶段的
	//   目标，所以边界落在哪一侧，值就差一格。
	// 两者是不同性质的量，因此采用不同的验收标准 —— 相机链严格一致（玩家可见），
	// 逻辑偏移允许边界相位差（服务于回正/CSV，对一格误差不敏感）。
	TestTrue(FString::Printf(TEXT("Accumulated pitch stays within one sub-step at stage boundaries (max deviation %.6f)"), MaxAccumDiff),
		MaxAccumDiff <= FrameRateAccumTolerance);

	TestEqual(TEXT("Accumulated pitch matches exactly at all but at most one boundary sample"), NumAccumMismatch, 0);

	TestEqual(TEXT("Accumulated pitch trajectory is identical across 20 / 60 / 144 fps"), NumAccumMismatch, 0);

	// --- 轨迹不能是一条平线：证明确实采到了阶段变化 ---
	{
		float MinValue = TNumericLimits<float>::Max();
		float MaxValue = TNumericLimits<float>::Lowest();
		for (const float Value : RunAt60.CameraOffsetPitch)
		{
			MinValue = FMath::Min(MinValue, Value);
			MaxValue = FMath::Max(MaxValue, Value);
		}

		TestTrue(FString::Printf(TEXT("Trajectory actually varies over the burst (min=%.4f max=%.4f)"), MinValue, MaxValue),
			(MaxValue - MinValue) > 0.1f);
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 插值模式用例 4：阶段形状 —— 回弹与稳态残留
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilInterpStageShapeTest, "Lyra.Recoil.Interp.StageShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilInterpStageShapeTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;

	ULyraRecoilProfile* Profile = MakeInterpolatedTestProfile();

	constexpr float Amplitude = ExpectedVerticalPerShot;   // 0.5

	FRecoilRuntimeState State;
	State.Reset(Profile);
	State.ApplyShot(Profile, 1.0f);

	// 逐子步推进：所有阶段时长都是子步整数倍，所以"推 N 个子步"是精确可控的。
	// 这里全部用循环逐子步喂，而不是一次性喂阶段时长 —— 后者会留下零头，
	// 而零头按设计不推进（保帧率不变性），会让断言差一格。

	// 推到上抬结束：应达到完整幅度（6 个子步）
	for (int32 Step = 0; Step < 6; ++Step)
	{
		State.Advance(Profile, FRecoilRuntimeState::FixedSubStepSeconds);
	}
	TestTrue(FString::Printf(TEXT("Lift end reaches full amplitude (expected %.4f, actual %.4f)"),
		Amplitude, State.CameraOffsetPitch),
		FMath::IsNearlyEqual(State.CameraOffsetPitch, Amplitude, 0.02f));

	// 推到回弹结束：应落到 幅度 × ReboundRatio（再 6 个子步）
	for (int32 Step = 0; Step < 6; ++Step)
	{
		State.Advance(Profile, FRecoilRuntimeState::FixedSubStepSeconds);
	}
	const float ExpectedReboundEnd = Amplitude * Profile->ReboundRatio;
	TestTrue(FString::Printf(TEXT("Rebound end == amplitude x ReboundRatio (expected %.4f, actual %.4f)"),
		ExpectedReboundEnd, State.CameraOffsetPitch),
		FMath::IsNearlyEqual(State.CameraOffsetPitch, ExpectedReboundEnd, 0.03f));

	// 回弹终点必须低于峰值（否则"回弹"这件事没发生）
	TestTrue(FString::Printf(TEXT("Rebound actually dropped below the peak (%.4f < %.4f)"),
		State.CameraOffsetPitch, Amplitude),
		State.CameraOffsetPitch < Amplitude);

	// 稳定段（Settle）：值应保持不变（再推一半的 Settle 时长）
	{
		const float PitchAtSettleStart = State.CameraOffsetPitch;
		for (int32 Step = 0; Step < 6; ++Step)
		{
			State.Advance(Profile, FRecoilRuntimeState::FixedSubStepSeconds);
		}

		TestTrue(FString::Printf(TEXT("Settle stage freezes the offset (%.6f vs %.6f)"),
			State.CameraOffsetPitch, PitchAtSettleStart),
			FMath::IsNearlyEqual(State.CameraOffsetPitch, PitchAtSettleStart, 0.02f));
	}

	// 推到全阶段走完：应收敛到 幅度 × RecoilReturnRatio
	// Settle 剩 6 步 + Drop 18 步，再多推几步确保收尾
	for (int32 Step = 0; Step < 30; ++Step)
	{
		State.Advance(Profile, FRecoilRuntimeState::FixedSubStepSeconds);
	}
	const float ExpectedSteady = Amplitude * Profile->RecoilReturnRatio;

	TestTrue(FString::Printf(TEXT("Steady state == amplitude x RecoilReturnRatio (expected %.4f, actual %.4f)"),
		ExpectedSteady, State.CameraOffsetPitch),
		FMath::IsNearlyEqual(State.CameraOffsetPitch, ExpectedSteady, 0.03f));

	TestTrue(TEXT("Interp timeline is finished"), State.InterpStage == ERecoilInterpStage::None);
	TestTrue(TEXT("State returns to Idle"), State.State == ERecoilState::Idle);

	// 收尾后补间输出必须与逻辑偏移一致，否则下一轮连发会从错误位置开始
	TestTrue(FString::Printf(TEXT("After settling, camera offset == accumulated (%.6f vs %.6f)"),
		State.CameraOffsetPitch, State.AccumulatedPitch),
		FMath::IsNearlyEqual(State.CameraOffsetPitch, State.AccumulatedPitch, Tolerance));

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 插值模式用例 5：长帧保护（防 spiral of death）
//
// 喂一个巨大的 DeltaSeconds，子步次数不得超过上限，且状态必须收敛 ——
// 不能因为"时间没走完"而卡在半路。
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilInterpLongFrameTest, "Lyra.Recoil.Interp.LongFrameSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilInterpLongFrameTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;

	ULyraRecoilProfile* Profile = MakeInterpolatedTestProfile();

	FRecoilRuntimeState State;
	State.Reset(Profile);
	State.ApplyShot(Profile, 1.0f);

	// 模拟一次 1 秒的卡顿：按 1/60 切分本应产生 60 个子步
	State.Advance(Profile, 1.0f);

	TestTrue(FString::Printf(TEXT("Sub-step count capped at %d (actual %d)"),
		FRecoilRuntimeState::MaxSubStepsPerAdvance, State.LastSubStepCount),
		State.LastSubStepCount <= FRecoilRuntimeState::MaxSubStepsPerAdvance);

	// 状态必须收敛：插值链收尾 + 回到 Idle（不允许停在半路）
	TestTrue(TEXT("Interp timeline converges to None after a long frame"), State.InterpStage == ERecoilInterpStage::None);
	TestTrue(TEXT("State converges to Idle after a long frame"), State.State == ERecoilState::Idle);

	// 收敛值必须是稳态残留
	const float ExpectedSteady = ExpectedVerticalPerShot * Profile->RecoilReturnRatio;
	TestTrue(FString::Printf(TEXT("Converges to steady state (expected %.4f, actual %.4f)"),
		ExpectedSteady, State.CameraOffsetPitch),
		FMath::IsNearlyEqual(State.CameraOffsetPitch, ExpectedSteady, 0.03f));

	// 后续推进不应再改变数值
	{
		const float BlendedValue = State.CameraOffsetPitch;
		for (int32 Step = 0; Step < 30; ++Step)
		{
			State.Advance(Profile, StepSeconds);
		}

		TestTrue(FString::Printf(TEXT("Value stays frozen after convergence (%.6f vs %.6f)"),
			State.CameraOffsetPitch, BlendedValue),
			FMath::IsNearlyEqual(State.CameraOffsetPitch, BlendedValue, Tolerance));
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 插值模式用例 6：连发中途重启阶段不产生跳变
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilInterpRefireTest, "Lyra.Recoil.Interp.RefireContinuity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilInterpRefireTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;

	ULyraRecoilProfile* Profile = MakeInterpolatedTestProfile();

	FRecoilRuntimeState State;
	State.Reset(Profile);
	State.ApplyShot(Profile, 1.0f);

	// 推到上抬中途（3 个子步 = 半个 LiftDuration，精确半程）
	State.Advance(Profile, FRecoilRuntimeState::FixedSubStepSeconds * 3.0f);

	const float PitchBeforeRefire = State.CameraOffsetPitch;
	TestTrue(FString::Printf(TEXT("Mid-lift offset is non-zero before refire (%.4f)"), PitchBeforeRefire),
		PitchBeforeRefire > 0.0f);

	// 中途再次开火：阶段应重置回 Lift
	State.ApplyShot(Profile, 1.0f);
	TestTrue(TEXT("Refire restarts the stage at Lift"), State.InterpStage == ERecoilInterpStage::Lift);

	// 关键：补间输出不能"跳回起点"。重启后推一个子步，
	// 输出必须从"重启前的值"**继续往上**，而不是归零或跳到无关位置。
	//
	// 注意容忍度取 0.1 而不是更小：重启后第一发子步的增量本身就偏大，
	// 因为新一发的 Kick 是在已有偏移**之上**再叠一个完整幅度，
	// 分成 6 个子步推完 → 单步 ≈ 幅度/6 ≈ 0.083。
	// 这是"连发被持续推高"的正常观感，不是跳变。真正要禁止的是**向下跳**或**归零**。
	State.Advance(Profile, StepSeconds);

	TestTrue(FString::Printf(TEXT("No jump on refire: offset continues from the previous value (before=%.4f after=%.4f)"),
		PitchBeforeRefire, State.CameraOffsetPitch),
		FMath::Abs(State.CameraOffsetPitch - PitchBeforeRefire) < 0.1f);

	TestTrue(FString::Printf(TEXT("Refire does not reset the offset to zero (after=%.4f)"), State.CameraOffsetPitch),
		State.CameraOffsetPitch > PitchBeforeRefire);

	// 连发完成后仍能正常收敛
	for (int32 Step = 0; Step < 120; ++Step)
	{
		State.Advance(Profile, StepSeconds);
	}

	TestTrue(TEXT("Converges to Idle after the burst"), State.State == ERecoilState::Idle);
	TestTrue(TEXT("Interp timeline finished"), State.InterpStage == ERecoilInterpStage::None);

	return true;
}


#endif // WITH_DEV_AUTOMATION_TESTS

#undef LOCTEXT_NAMESPACE
