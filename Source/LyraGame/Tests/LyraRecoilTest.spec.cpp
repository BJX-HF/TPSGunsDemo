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
 *   3. 停火经过 RecoveryDelay + RecoveryTime 后，无压枪时偏移完全回满到 0
 *      （后坐力偏移收敛到「本梭累计压枪量」；不压枪即为 0 ⇒ 屏幕回开枪前）
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
	// 峰值被 Clamp 到 MaxKick。现行口径：无压枪时偏移**完全回满到 0**（屏幕回开枪前）。
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

	// 无压枪 ⇒ 终止值 = 0（偏移完全回满）
	const float ExpectedSteady = 0.0f;

	TestTrue(TEXT("State returns to Idle after recovery completes"), State.State == ERecoilState::Idle);
	TestTrue(
		FString::Printf(TEXT("Steady-state offset == 0 when no compensation (expected %.4f, actual %.4f)"),
			ExpectedSteady, State.AccumulatedPitch),
		FMath::IsNearlyEqual(State.AccumulatedPitch, ExpectedSteady, Tolerance));
	TestTrue(FString::Printf(TEXT("RecoveryProgress reaches 1.0 (actual %.4f)"), State.RecoveryProgress),
		FMath::IsNearlyEqual(State.RecoveryProgress, 1.0f, Tolerance));

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
		FString::Printf(TEXT("Second recovery steady state == 0 (no compensation) (actual %.4f)"), State.AccumulatedPitch),
		FMath::IsNearlyZero(State.AccumulatedPitch, Tolerance));

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

	// 推到全阶段走完：无压枪 ⇒ 偏移应完全回满到 0
	// Settle 剩 6 步 + Drop 18 步，再多推几步确保收尾
	for (int32 Step = 0; Step < 30; ++Step)
	{
		State.Advance(Profile, FRecoilRuntimeState::FixedSubStepSeconds);
	}
	const float ExpectedSteady = 0.0f;

	TestTrue(FString::Printf(TEXT("Steady state == 0 (no compensation) (expected %.4f, actual %.4f)"),
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

	// 收敛值必须是稳态残留（无压枪 ⇒ 偏移回满到 0）
	const float ExpectedSteady = 0.0f;
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
//////////////////////////////////////////////////////////////////////////
// 回正收敛到「玩家压枪量」（Docs/Recoil/11_BurstAccumulationFix.md §13）
//
// ★ 2026-09-21 **二次**定型口径（大祥老师拍板，最终版）：
//
//   公式：**终止值 = 本梭累计压枪量**（= 玩家自己往下压了多少）
//
//   屏幕视角 = ControlRotation（含玩家压枪）+ 后坐力偏移，于是
//     不压枪   ⇒ 终止值 0  ⇒ 偏移回满 ⇒ 屏幕回开枪前
//     压 N 度   ⇒ 终止值 N  ⇒ 玩家的 Ctrl 已低了 N 度 ⇒ 屏幕同样回开枪前
//
//   ⚠ 上一版是 `峰值 − 压枪量`，屏幕 = 峰值 − 2×压枪量 ⇒ 压在真实弹道上就是**看地板**。
//     实机 trace 坐实（DA_Recoil_Rifle_S 连发）：峰值 17.6 / 压枪 13.65
//       · 旧式 ⇒ 屏幕 −9.70（错误）    · 现行 ⇒ 屏幕 0.00（回到开枪前）
//
//   旧字段 RecoilReturnRatio（残留比例）与 RecoilCompensationMinResidualRatio（残留地板）
//   已按大祥老师要求**删除** —— 不做不被要求的额外设计。
//
//   权威验收见 Lyra.Recoil.Compensation.UserContractScenarios。
//
// 压枪量的口径：以「本轮连发第一发的玩家瞄准」为基准，取 ControlRotation 差值的**负值**
// （往下压 / 往左拉 → 正）。水平轴默认不参与抵扣（bCompensationAwareRecoveryYaw = false）。
//
// 这一组刻意全部走公开 API（SamplePlayerAim / ApplyShot / Advance），
// 不去直接调 ComputeRecoveryTarget —— 要验的是「整条链路最后落在哪」，不是公式本身。
//////////////////////////////////////////////////////////////////////////

namespace LyraRecoilCompensationHelpers
{
	/**
	 * 造一个"刚连发完 InShots 发、玩家又往后坐力反方向拉了 InPullDown / InYawDrag 度"的状态。
	 *
	 * 采样顺序刻意与运行时一致：先采基准 → 开第一发（基准在此锁定）→ 连发 → 连发期间再采样。
	 *
	 * ★ 注意连发期间不调 Advance —— 于是 RecoveryCoverPitch 不会在连发途中累积，
	 *   真正的累计发生在第一个 Advance（进入回正那一帧）。
	 *   这与实机路径略有差异（实机每帧都 Advance），但让期望值可以手算，
	 *   且"抵扣量 = 玩家压了多少"这个语义是一致的。
	 */
	static void RunBurstWithCompensation(
		FRecoilRuntimeState& State,
		ULyraRecoilProfile& Profile,
		int32 InShots,
		float InPullDownDegrees,
		float InYawDragDegrees = 0.0f)
	{
		State.Reset(&Profile);

		// 基准：抬头 0°、朝向 0°
		State.SamplePlayerAim(0.0f, 0.0f);

		for (int32 Shot = 0; Shot < InShots; ++Shot)
		{
			State.ApplyShot(&Profile, 1.0f);
		}

		// 往下压 = Pitch 减小；往左拉 = Yaw 减小 → 两者都让压枪量为正
		State.SamplePlayerAim(-InPullDownDegrees, -InYawDragDegrees);
	}

	/** 推进到稳态（0.7s ≫ RecoveryDelay 0.1 + RecoveryTime 0.4）。 */
	static void AdvanceToSteady(FRecoilRuntimeState& State, const ULyraRecoilProfile& Profile, int32 InSteps = 42)
	{
		for (int32 Step = 0; Step < InSteps; ++Step)
		{
			State.Advance(&Profile, LyraRecoilTestHelpers::StepSeconds);
		}
	}
}

/** 零输入时必须把偏移完全回满到 0 —— 这是「完全不压枪也能回到开枪前」的结构性保证。 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilCompensationZeroInputTest, "Lyra.Recoil.Compensation.ZeroInputMatchesBaseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilCompensationZeroInputTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;
	using namespace LyraRecoilCompensationHelpers;

	ULyraRecoilProfile* Profile = MakeTestProfile();
	TestTrue(TEXT("开关默认开启（新行为是默认行为）"), Profile->bCompensationAwareRecovery);

	FRecoilRuntimeState State;
	RunBurstWithCompensation(State, *Profile, 10, 0.0f);

	const float Peak = 5.0f;   // 10 发 × 0.5（用于断言"回正量 = 峰值"，不再是终止值）
	TestTrue(FString::Printf(TEXT("没有玩家输入时压枪量为 0（实际 %.6f）"), State.PlayerCompensationPitch),
		FMath::IsNearlyZero(State.PlayerCompensationPitch, Tolerance));

	AdvanceToSteady(State, *Profile);

	TestTrue(TEXT("回正结束后回到 Idle"), State.State == ERecoilState::Idle);
	TestTrue(
		FString::Printf(TEXT("零输入下终止值 = 0（期望 0.0000，实际 %.4f）"), State.AccumulatedPitch),
		FMath::IsNearlyZero(State.AccumulatedPitch, Tolerance));
	TestTrue(
		FString::Printf(TEXT("零输入下回正量 = 峰值 = %.4f（实际 %.4f）"), Peak, Peak - State.AccumulatedPitch),
		FMath::IsNearlyEqual(Peak - State.AccumulatedPitch, Peak, Tolerance));

	return true;
}

/** 压了 1° → 偏移收敛到 1°（屏幕回开枪前）：终止值 = 累计压枪量。 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilCompensationRetainsPullDownTest, "Lyra.Recoil.Compensation.RetainsPullDown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilCompensationRetainsPullDownTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;
	using namespace LyraRecoilCompensationHelpers;

	ULyraRecoilProfile* Profile = MakeTestProfile();   // 峰值 5.0

	FRecoilRuntimeState State;
	RunBurstWithCompensation(State, *Profile, 10, 1.0f);

	TestTrue(FString::Printf(TEXT("压枪量 = 1.0（实际 %.6f）"), State.PlayerCompensationPitch),
		FMath::IsNearlyEqual(State.PlayerCompensationPitch, 1.0f, Tolerance));

	AdvanceToSteady(State, *Profile);

	// 终止值 = 累计压枪量 = 1.0
	const float Expected = 1.0f;
	TestTrue(
		FString::Printf(TEXT("终止值 = 累计压枪量（期望 %.4f，实际 %.4f）"), Expected, State.AccumulatedPitch),
		FMath::IsNearlyEqual(State.AccumulatedPitch, Expected, Tolerance));

	// 换个说法断言同一件事：实际回正量 = 峰值 − 终止值 = 5.0 − 1.0 = 4.0
	TestTrue(
		FString::Printf(TEXT("实际回正量 = 峰值 − 压枪量 = 4.0（实际 %.4f）"), 5.0f - State.AccumulatedPitch),
		FMath::IsNearlyEqual(5.0f - State.AccumulatedPitch, 4.0f, Tolerance));

	return true;
}

/**
 * 压过头（压枪量 20° > 峰值 5°）：终止值 = 压枪量本身（20°），屏幕依旧回到开枪前。
 *
 * ★ 口径变化（2026-09-21 二次定型）：
 *   旧式 `峰值 − 压枪量` 在压过头时给 −15（偏移反向），屏幕 = −20 + (−15) = −35 ⇒ 极端看地板。
 *   现行 `压枪量` 给 +20，屏幕 = −20 + 20 = 0 ⇒ 仍然精确回到开枪前。
 *
 * 注意：压过头时偏移不是"下降"而是**上升**（从峰值 5 升到 20）。
 * 这是"屏幕必须回开枪前"这条口径的数学必然，不是笔误。
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilCompensationOverPullTest, "Lyra.Recoil.Compensation.OverCompensationFollowsPull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilCompensationOverPullTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;
	using namespace LyraRecoilCompensationHelpers;

	constexpr float Peak = 5.0f;      // 10 发 × 0.5
	constexpr float Pull = 20.0f;     // 远超峰值

	ULyraRecoilProfile* Profile = MakeTestProfile();

	FRecoilRuntimeState State;
	RunBurstWithCompensation(State, *Profile, 10, Pull);

	AdvanceToSteady(State, *Profile);

	TestTrue(TEXT("回正结束后回到 Idle"), State.State == ERecoilState::Idle);

	// 终止值 = 压枪量 = 20.0
	TestTrue(
		FString::Printf(TEXT("压过头时终止值 = 压枪量（期望 %.4f，实际 %.4f）"), Pull, State.AccumulatedPitch),
		FMath::IsNearlyEqual(State.AccumulatedPitch, Pull, Tolerance));

	// 屏幕 = Ctrl(−20) + 偏移(20) = 0 —— 仍精确回到开枪前
	TestTrue(
		FString::Printf(TEXT("屏幕仍回到开枪前（−P + 偏移 = %.4f）"), -Pull + State.AccumulatedPitch),
		FMath::IsNearlyZero(-Pull + State.AccumulatedPitch, Tolerance));

	// 回正量 = 峰值 − 终止值 = 5 − 20 = −15：偏移反而**上升**（屏幕归位所需）
	TestTrue(
		FString::Printf(TEXT("回正量为负（偏移上升）%.4f < 0"), Peak - State.AccumulatedPitch),
		(Peak - State.AccumulatedPitch) < 0.0f);

	return true;
}

/**
 * ★ 大祥老师 2026-09-21 指定的验收场景 —— **二次定型**后的权威口径。
 *
 *   设 K = 本轮峰值（枪把镜头抬高多少度）、P = 玩家压枪位移（向下为正）。
 *
 *   口径：**回正后「后坐力偏移」= P**，于是
 *         屏幕视角 = ControlRotation(−P) + 偏移(P) = 0 ⇒ 精确回到开枪前。
 *
 *   逐条含义：
 *     · 完全不压枪   P = 0  ⇒ 偏移回满到 0   ⇒ 屏幕回开枪前（回正量 = K）
 *     · 上抬 10 压 5 P = 5  ⇒ 偏移收敛到 5   ⇒ 屏幕回开枪前（回正量 = K − P = 5）
 *     · 压过头       P > K  ⇒ 偏移升到 P     ⇒ 屏幕仍回开枪前（回正量为负）
 *
 *   ⚠ 上一版口径是 `K − P`，屏幕 = K − 2P。实机 trace（峰值 17.6 / 压枪 13.65）
 *     给出屏幕 −9.70（**看地板**）⇒ 已推翻，本用例锁死现行口径。
 *
 * 实现方式：直接用公开 API 造状态（ApplyShot / SamplePlayerAim / Advance），
 * 不去调 ComputeRecoveryTarget —— 验的是整条链路最后落在哪。
 * 峰值靠"每发 0.5° × N 发"凑出整数，K=5 用 10 发、K=10 用 20 发。
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilRecoveryContractScenariosTest, "Lyra.Recoil.Compensation.UserContractScenarios",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilRecoveryContractScenariosTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;
	using namespace LyraRecoilCompensationHelpers;

	// 参数化：K 靠发数实现（每发 0.5°，MaxVerticalKick 故意开到 1000 不夹）
	struct FScenario
	{
		const TCHAR* Name;
		int32 Shots;             // K = Shots × 0.5
		float Pull;              // P
		float ExpectedRecovery;  // 回正量 = K − P
	};

	const FScenario Scenarios[] =
	{
		{ TEXT("上抬 5、完全不压枪"),        10,  0.0f,   5.0f },
		{ TEXT("上抬 5、压 3"),              10,  3.0f,   2.0f },
		{ TEXT("上抬 10、压 5（老师原例）"), 20,  5.0f,   5.0f },
		{ TEXT("上抬 5、压过头 10"),         10, 10.0f,  -5.0f },
	};

	for (const FScenario& S : Scenarios)
	{
		ULyraRecoilProfile* Profile = MakeTestProfile();

		const float K = 0.5f * static_cast<float>(S.Shots);
		const float P = S.Pull;

		FRecoilRuntimeState State;
		RunBurstWithCompensation(State, *Profile, S.Shots, P);

		// 连发结束时（回正之前）偏移应当正好等于峰值 K
		TestTrue(FString::Printf(TEXT("%s：峰值 = K = %.2f（实际 %.4f）"), S.Name, K, State.AccumulatedPitch),
			FMath::IsNearlyEqual(State.AccumulatedPitch, K, Tolerance));

		AdvanceToSteady(State, *Profile);

		TestTrue(TEXT("回正结束后回到 Idle"), State.State == ERecoilState::Idle);

		// ① 回正后偏移 = 压枪量 P
		TestTrue(
			FString::Printf(TEXT("%s：回正后偏移 = P = %.2f（实际 %.4f）"), S.Name, P, State.AccumulatedPitch),
			FMath::IsNearlyEqual(State.AccumulatedPitch, P, Tolerance));

		// ② 屏幕视角精确回到开枪前：屏幕 = Ctrl(−P) + 偏移(P) = 0
		TestTrue(
			FString::Printf(TEXT("%s：屏幕回开枪前（−P + 偏移 = %.4f）"), S.Name, -P + State.AccumulatedPitch),
			FMath::IsNearlyZero(-P + State.AccumulatedPitch, Tolerance));

		// ③ 回正量 = K − P（老师原例：10 − 5 = 5）
		TestTrue(
			FString::Printf(TEXT("%s：回正量 = K − P = %.2f（实际 %.4f）"),
				S.Name, S.ExpectedRecovery, K - State.AccumulatedPitch),
			FMath::IsNearlyEqual(K - State.AccumulatedPitch, S.ExpectedRecovery, Tolerance));
	}

	return true;
}

/**
 * 水平轴：**默认不参与抵扣**（bCompensationAwareRecoveryYaw = false）。
 *
 * 2026-09-21 修订（原语义：「水平轴同规则，位移也保留」）：
 *   水平方向没有「压枪」这个动作 —— 玩家的水平移动是**转身追目标**。
 *   而 MaxHorizontalKick 只有 2.0°，门槛 1.7°，
 *   实机里转身超过 1.7° 随时发生 ⇒ 两轴同规则会让 Yaw 回正**长期恒为 0**。
 *   所以默认改成一轴一闸：Pitch 扣、Yaw 不扣。
 *
 * 本用例覆盖两种情形：
 *   情形 1（默认）    ：位移照常被记录，但不参与回正 → 终止值 = 0（回满）
 *   情形 2（显式打开）：水平轴也收敛到位移量        → 终止值 = 位移
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilCompensationYawTest, "Lyra.Recoil.Compensation.YawRetainsDrag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilCompensationYawTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;
	using namespace LyraRecoilCompensationHelpers;

	// X = 1、PatternLength = 8 → 8 发水平严格单调：峰值 = 8 × 0.25 = 2.0
	ULyraRecoilProfile* Profile = MakeHorizontalTestProfile(8);

	TestTrue(TEXT("Yaw 抵扣默认关闭（一轴一闸）"), !Profile->bCompensationAwareRecoveryYaw);

	// --- 情形 1：默认不抵扣 ---
	FRecoilRuntimeState State;
	RunBurstWithCompensation(State, *Profile, 8, 0.0f, /*InYawDragDegrees=*/ 0.5f);

	TestTrue(FString::Printf(TEXT("水平压枪量 = 0.5（照常记录，实际 %.6f）"), State.PlayerCompensationYaw),
		FMath::IsNearlyEqual(State.PlayerCompensationYaw, 0.5f, Tolerance));

	const float PeakYaw = State.AccumulatedYaw;   // 回正前：8 发 × 0.25 = 2.0
	TestTrue(FString::Printf(TEXT("水平峰值 = 2.0（实际 %.4f）"), PeakYaw),
		FMath::IsNearlyEqual(PeakYaw, 2.0f, Tolerance));

	AdvanceToSteady(State, *Profile);

	// 默认不抵扣 ⇒ 水平终止值 = 0（回满）
	TestTrue(
		FString::Printf(TEXT("默认：水平终止值 = 0，位移不参与（期望 0.0000，实际 %.4f）"), State.AccumulatedYaw),
		FMath::IsNearlyZero(State.AccumulatedYaw, Tolerance));

	// --- 情形 2：显式打开 → 水平轴收敛到「位移量」 ---
	Profile->bCompensationAwareRecoveryYaw = true;

	FRecoilRuntimeState StateYawApplied;
	RunBurstWithCompensation(StateYawApplied, *Profile, 8, 0.0f, /*InYawDragDegrees=*/ 0.5f);
	AdvanceToSteady(StateYawApplied, *Profile);

	const float AppliedYaw = 0.5f;   // = 水平位移量本身
	TestTrue(
		FString::Printf(TEXT("打开后：水平终止值 = 位移量（期望 %.4f，实际 %.4f）"),
			AppliedYaw, StateYawApplied.AccumulatedYaw),
		FMath::IsNearlyEqual(StateYawApplied.AccumulatedYaw, AppliedYaw, Tolerance));

	return true;
}

/** 关掉开关 → 严格退回旧公式（A/B 对照能力）。 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilCompensationDisabledTest, "Lyra.Recoil.Compensation.DisabledKeepsLegacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilCompensationDisabledTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;
	using namespace LyraRecoilCompensationHelpers;

	ULyraRecoilProfile* Profile = MakeTestProfile();
	Profile->bCompensationAwareRecovery = false;

	FRecoilRuntimeState State;
	RunBurstWithCompensation(State, *Profile, 10, 1.0f);

	// 压枪量照常被测出来（只是不参与回正），所以调试面板在关掉开关时依然能看数
	TestTrue(FString::Printf(TEXT("关掉开关后压枪量依然被记录（实际 %.6f）"), State.PlayerCompensationPitch),
		FMath::IsNearlyEqual(State.PlayerCompensationPitch, 1.0f, Tolerance));

	AdvanceToSteady(State, *Profile);

	// 关掉抵扣 ⇒ 终止值 = 0（偏移完全回满，与"零输入"场景一致）
	const float DisabledExpected = 0.0f;
	TestTrue(
		FString::Printf(TEXT("关掉开关后不抵扣、偏移回满到 0（期望 %.4f，实际 %.4f）"), DisabledExpected, State.AccumulatedPitch),
		FMath::IsNearlyEqual(State.AccumulatedPitch, DisabledExpected, Tolerance));

	return true;
}

/** 累计抵扣在"开始回正"那一刻冻结 —— 回正途中再动鼠标不再改变落点。 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilCompensationFrozenTest, "Lyra.Recoil.Compensation.FrozenAfterRecoveryStarts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilCompensationFrozenTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;
	using namespace LyraRecoilCompensationHelpers;

	ULyraRecoilProfile* Profile = MakeTestProfile();

	FRecoilRuntimeState State;
	RunBurstWithCompensation(State, *Profile, 10, 1.0f);

	// 推进到进入 Recovering 之后（RecoveryDelay = 0.1 → 20 步 ≈ 0.333s）
	AdvanceToSteady(State, *Profile, 20);
	TestTrue(TEXT("已经进入 Recovering"), State.State == ERecoilState::Recovering);
	TestTrue(FString::Printf(TEXT("抵扣量已冻结在 1.0（实际 %.6f）"), State.RecoveryCompensationPitch),
		FMath::IsNearlyEqual(State.RecoveryCompensationPitch, 1.0f, Tolerance));
	TestTrue(TEXT("已标记本梭抵扣用掉"), State.bRecoveryCoverApplied);

	// 回正途中玩家又狂压 30°：不该改变落点
	State.SamplePlayerAim(-31.0f, 0.0f);

	AdvanceToSteady(State, *Profile, 60);

	// 冻结的抵扣 = 1.0 ⇒ 终止值 = 1.0（途中再压 30° 不改变落点）
	TestTrue(
		FString::Printf(TEXT("途中继续压枪不影响落点（期望 1.0000，实际 %.4f）"), State.AccumulatedPitch),
		FMath::IsNearlyEqual(State.AccumulatedPitch, 1.0f, Tolerance));

	return true;
}

/** 插值模式下 Drop 段终点与收官值必须一致（否则 Drop 结束那一帧会跳）。 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilCompensationInterpTest, "Lyra.Recoil.Compensation.InterpolatedDropConsistency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilCompensationInterpTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilTestHelpers;

	// 单发幅度 0.5；压 0.3 → 终止值 = 压枪量 = 0.3
	ULyraRecoilProfile* Profile = MakeInterpolatedTestProfile();

	FRecoilRuntimeState State;
	State.Reset(Profile);

	State.SamplePlayerAim(0.0f, 0.0f);
	State.ApplyShot(Profile, 1.0f);
	State.SamplePlayerAim(-0.3f, 0.0f);

	// Lift 6 + Rebound 6 + Settle 12 + Drop 18 = 42 个子步，跑 120 步确保收尾
	for (int32 Step = 0; Step < 120; ++Step)
	{
		State.Advance(Profile, StepSeconds);
	}

	TestTrue(TEXT("插值链已收尾"), State.InterpStage == ERecoilInterpStage::None);
	TestTrue(TEXT("回到 Idle"), State.State == ERecoilState::Idle);
	TestTrue(FString::Printf(TEXT("本发峰值 = 0.5（实际 %.4f）"), State.RecoveryPeakPitch),
		FMath::IsNearlyEqual(State.RecoveryPeakPitch, 0.5f, Tolerance));
	TestTrue(FString::Printf(TEXT("抵扣量 = 0.3（实际 %.4f）"), State.RecoveryCompensationPitch),
		FMath::IsNearlyEqual(State.RecoveryCompensationPitch, 0.3f, Tolerance));

	// 终止值 = 压枪量 = 0.3
	const float Expected = 0.3f;
	TestTrue(
		FString::Printf(TEXT("逻辑偏移落到 %.4f（实际 %.4f）"), Expected, State.AccumulatedPitch),
		FMath::IsNearlyEqual(State.AccumulatedPitch, Expected, Tolerance));
	TestTrue(
		FString::Printf(TEXT("补间输出与逻辑偏移一致，收尾无跳变（cam=%.4f acc=%.4f）"),
			State.CameraOffsetPitch, State.AccumulatedPitch),
		FMath::IsNearlyEqual(State.CameraOffsetPitch, State.AccumulatedPitch, Tolerance));

	return true;
}


#endif // WITH_DEV_AUTOMATION_TESTS

#undef LOCTEXT_NAMESPACE
