// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"

/**
 * P10 自动验证：Lyra.Recoil.Spread.*
 *
 * 覆盖「散布并入后坐力资产」这条改造（姿态-角度直接模型，见 Docs/Recoil/12_SpreadInProfile.md）：
 *
 *   1. 默认关闭 —— 不启用时逐位不动任何状态（零回归的保证点）
 *   2. 连射累加 + 上限封顶 —— 数值可手算复核
 *   3. 停火回落 + 延迟 + 基础角地板 —— 数值可手算复核
 *   4. 姿态切换 —— 蹲下立刻收紧、起跳立刻放开（含"新基础角高于当前锥角"的抬升路径）
 *   5. 玩家侧倍率（瞄准 / 移动）—— 纯函数的边界与退化区间
 *   6. 资产参数解析与校验 —— Max < Base 必须被报错，且运行时出口会兜底
 *
 * 为什么全部是纯数值单测：散布的累加/回落与后坐力一样不依赖 UWorld，
 * 所以可以脱离引擎世界逐发断言 —— 这是"每阶段可验证"这条开发约束的直接兑现。
 */

namespace LyraRecoilSpreadTest
{
	static constexpr float Tolerance = 1e-4f;

	/**
	 * 造一份"整数友好"的散布配置，便于手算复核。
	 *
	 * 站定：base 1.0 / max 3.0 / 每发 +0.5 / 回落 2.0 度每秒 / 停火延迟 0.1s
	 * 蹲伏：base 0.5 / max 1.0 / 每发 +0.1 / 回落 1.0
	 * 空中：base 2.5 / max 4.0 / 每发 +0.2 / 回落 0（空中不回落）
	 */
	static ULyraRecoilProfile* MakeSpreadProfile()
	{
		ULyraRecoilProfile* Profile = NewObject<ULyraRecoilProfile>(GetTransientPackage());

		Profile->bEnableProfileSpread = true;

		Profile->SpreadAngle_Standing = 1.0f;
		Profile->MaxSpreadAngle_Standing = 3.0f;
		Profile->SpreadAddPerShot_Standing = 0.5f;
		Profile->SpreadRecoverRate_Standing = 2.0f;

		Profile->SpreadAngle_Crouching = 0.5f;
		Profile->MaxSpreadAngle_Crouching = 1.0f;
		Profile->SpreadAddPerShot_Crouching = 0.1f;
		Profile->SpreadRecoverRate_Crouching = 1.0f;

		Profile->SpreadAngle_JumpingOrFalling = 2.5f;
		Profile->MaxSpreadAngle_JumpingOrFalling = 4.0f;
		Profile->SpreadAddPerShot_JumpingOrFalling = 0.2f;
		Profile->SpreadRecoverRate_JumpingOrFalling = 0.0f;

		Profile->SpreadMultiplier_Aiming = 0.5f;
		Profile->SpreadMultiplier_StandingStill = 0.4f;
		Profile->SpreadStandingStillSpeedThreshold = 100.0f;
		Profile->SpreadStandingStillToMovingRange = 50.0f;
		Profile->SpreadTransitionRate_StandingStill = 5.0f;
		Profile->SpreadRecoveryDelay = 0.1f;
		Profile->SpreadExponent = 1.0f;

		return Profile;
	}

	/** 走一个"推进一帧"：先 Advance（刷新停火时长），再 AdvanceSpread（吃那一帧）。 */
	static void StepFrame(FRecoilRuntimeState& State, const ULyraRecoilProfile& Profile, float DeltaSeconds, EPoseState PoseState)
	{
		State.Advance(&Profile, DeltaSeconds);
		State.AdvanceSpread(&Profile, DeltaSeconds, PoseState);
	}
}

//////////////////////////////////////////////////////////////////////////
// 1) 默认关闭：不启用资产散布时，任何调用都不得改动状态
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilSpreadDisabledIsNoOpTest, "Lyra.Recoil.Spread.DisabledIsNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilSpreadDisabledIsNoOpTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilSpreadTest;

	// 新建资产默认必须**关闭** —— 这是"既有资产/既有手感零回归"的第一道保证：
	// 只要没人在资产上勾这个开关，运行时就一定走 Lyra 原生 heat 链路。
	ULyraRecoilProfile* Profile = NewObject<ULyraRecoilProfile>(GetTransientPackage());
	TestTrue(TEXT("A freshly created ULyraRecoilProfile has profile spread disabled by default"),
		!Profile->bEnableProfileSpread);

	// 故意把数值配成"非 0"，用来证明"关闭 = 完全不动"而不是"数值恰好是 0"
	Profile->SpreadAngle_Standing = 5.0f;
	Profile->MaxSpreadAngle_Standing = 9.0f;
	Profile->SpreadAddPerShot_Standing = 1.0f;
	Profile->SpreadRecoverRate_Standing = 1.0f;

	FRecoilRuntimeState State;
	State.Reset(Profile);

	TestTrue(TEXT("Reset leaves the spread angle at 0 while disabled"),
		FMath::IsNearlyZero(State.CurrentSpreadAngle, Tolerance));

	State.ApplySpreadShot(Profile, EPoseState::Standing);
	TestTrue(TEXT("ApplySpreadShot is a no-op while disabled"),
		FMath::IsNearlyZero(State.CurrentSpreadAngle, Tolerance));

	State.Advance(Profile, 1.0f);
	State.AdvanceSpread(Profile, 1.0f, EPoseState::Standing);
	TestTrue(TEXT("AdvanceSpread is a no-op while disabled"),
		FMath::IsNearlyZero(State.CurrentSpreadAngle, Tolerance));

	TestTrue(TEXT("Effective spread stays 0 while disabled"),
		FMath::IsNearlyZero(State.GetEffectiveSpreadAngle(), Tolerance));

	// 也确认 ApplyShot 不会因为散布字段的存在而改变行为（ShotHistory 里的 SpreadAngle 恒为 0）
	State.ApplyShot(Profile, 1.0f);
	if (TestEqual(TEXT("One shot recorded"), State.ShotHistory.Num(), 1))
	{
		TestTrue(TEXT("ShotHistory SpreadAngle is 0 while disabled"),
			FMath::IsNearlyZero(State.ShotHistory[0].SpreadAngle, Tolerance));
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 2) 连射累加 + 上限封顶（手算：1.0 → 1.5 → 2.0 → 2.5 → 3.0 → 3.0）
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilSpreadAccumulateTest, "Lyra.Recoil.Spread.AccumulateAndClamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilSpreadAccumulateTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilSpreadTest;

	ULyraRecoilProfile* Profile = MakeSpreadProfile();

	FRecoilRuntimeState State;
	State.Reset(Profile);

	// Reset 后应从"站定基础角"起步，而不是 0 ——
	// 0 会让换枪那一帧的准星瞬间缩到满精度（肉眼可见的跳变）。
	TestTrue(FString::Printf(TEXT("Reset seeds the standing base angle (%.4f)"), State.CurrentSpreadAngle),
		FMath::IsNearlyEqual(State.CurrentSpreadAngle, 1.0f, Tolerance));

	const float ExpectedAfterEachShot[] = { 1.5f, 2.0f, 2.5f, 3.0f, 3.0f, 3.0f };
	constexpr int32 NumShots = UE_ARRAY_COUNT(ExpectedAfterEachShot);

	for (int32 ShotIndex = 0; ShotIndex < NumShots; ++ShotIndex)
	{
		State.ApplySpreadShot(Profile, EPoseState::Standing);

		TestTrue(FString::Printf(TEXT("Shot %d: spread = %.4f (expected %.4f)"),
			ShotIndex, State.CurrentSpreadAngle, ExpectedAfterEachShot[ShotIndex]),
			FMath::IsNearlyEqual(State.CurrentSpreadAngle, ExpectedAfterEachShot[ShotIndex], Tolerance));

		TestTrue(FString::Printf(TEXT("Shot %d: spread never exceeds the pose max (%.4f <= 3.0)"),
			ShotIndex, State.CurrentSpreadAngle),
			State.CurrentSpreadAngle <= 3.0f + Tolerance);
	}

	// LastSpreadAngle 是"修完玩家倍率之后"的值：本用例瞄准倍率 0.5、移动倍率 0.4（Reset 默认 1.0）
	// 未设置玩家倍率时（Reset 后默认 1.0/1.0）它应该等于裸锥角
	TestTrue(FString::Printf(TEXT("LastSpreadAngle mirrors the raw angle while player multipliers are 1 (%.4f)"), State.LastSpreadAngle),
		FMath::IsNearlyEqual(State.LastSpreadAngle, 3.0f, Tolerance));

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 3) 停火回落：延迟内不动 → 按速率回落 → 停在基础角
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilSpreadRecoverTest, "Lyra.Recoil.Spread.Recover",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilSpreadRecoverTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilSpreadTest;

	ULyraRecoilProfile* Profile = MakeSpreadProfile();

	FRecoilRuntimeState State;
	State.Reset(Profile);

	// 打到上限
	for (int32 Shot = 0; Shot < 5; ++Shot)
	{
		State.ApplySpreadShot(Profile, EPoseState::Standing);
	}
	TestTrue(TEXT("Spread reached the max before recovery"),
		FMath::IsNearlyEqual(State.CurrentSpreadAngle, 3.0f, Tolerance));

	// --- 延迟内：不动 ---
	// 停火延迟 0.1s，这里正好停在边界上（判定是 <=，所以仍然算"延迟内"）
	StepFrame(State, *Profile, 0.1f, EPoseState::Standing);
	TestTrue(FString::Printf(TEXT("No recovery inside the delay window (angle still %.4f)"), State.CurrentSpreadAngle),
		FMath::IsNearlyEqual(State.CurrentSpreadAngle, 3.0f, Tolerance));

	// --- 越过延迟：按 2.0 度/秒 回落 ---
	// 累计停火 0.35s → 越过延迟的时长就是本帧的 0.25s → 3.0 - 2.0 × 0.25 = 2.5
	StepFrame(State, *Profile, 0.25f, EPoseState::Standing);
	TestTrue(FString::Printf(TEXT("Recovery runs at the configured rate (angle %.4f, expected 2.5000)"), State.CurrentSpreadAngle),
		FMath::IsNearlyEqual(State.CurrentSpreadAngle, 2.5f, Tolerance));

	// --- 回到底：钳在基础角，不会掉到基础角以下 ---
	StepFrame(State, *Profile, 2.0f, EPoseState::Standing);
	TestTrue(FString::Printf(TEXT("Recovery floors at the pose base angle (angle %.4f, expected 1.0000)"), State.CurrentSpreadAngle),
		FMath::IsNearlyEqual(State.CurrentSpreadAngle, 1.0f, Tolerance));

	// 再推很久也必须停在基础角
	StepFrame(State, *Profile, 5.0f, EPoseState::Standing);
	TestTrue(TEXT("Spread stays at the base angle no matter how long we wait"),
		FMath::IsNearlyEqual(State.CurrentSpreadAngle, 1.0f, Tolerance));

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 4) 姿态切换：蹲下立刻收紧到蹲伏上限、起跳立刻抬到空中基础角
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilSpreadPoseSwitchTest, "Lyra.Recoil.Spread.PoseSwitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilSpreadPoseSwitchTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilSpreadTest;

	ULyraRecoilProfile* Profile = MakeSpreadProfile();

	FRecoilRuntimeState State;
	State.Reset(Profile);

	// 站定打到上限 3.0
	for (int32 Shot = 0; Shot < 5; ++Shot)
	{
		State.ApplySpreadShot(Profile, EPoseState::Standing);
	}
	TestTrue(TEXT("Start at the standing max"),
		FMath::IsNearlyEqual(State.CurrentSpreadAngle, 3.0f, Tolerance));

	// --- 蹲下：上限降到 1.0，锥角被立刻钳下来 ---
	State.AdvanceSpread(Profile, 0.001f, EPoseState::Crouching);
	TestTrue(FString::Printf(TEXT("Crouching clamps the angle down to the crouching max (%.4f, expected 1.0000)"), State.CurrentSpreadAngle),
		FMath::IsNearlyEqual(State.CurrentSpreadAngle, 1.0f, Tolerance));
	TestTrue(TEXT("Crouching pose base is reported as 0.5"),
		FMath::IsNearlyEqual(State.CurrentSpreadBaseAngle, 0.5f, Tolerance));

	// 蹲着再打两发：1.0 已在蹲伏上限，增量应该顶不动它
	State.ApplySpreadShot(Profile, EPoseState::Crouching);
	State.ApplySpreadShot(Profile, EPoseState::Crouching);
	TestTrue(TEXT("Crouching stays pinned at its own max"),
		FMath::IsNearlyEqual(State.CurrentSpreadAngle, 1.0f, Tolerance));

	// --- 起跳：基础角 2.5 高于当前 1.0，锥角被**抬上来**（不是回落到 1.0）---
	// 这条路径是"慢一步换姿态就会看到准星先缩再放"的根源，必须钉住。
	State.AdvanceSpread(Profile, 0.001f, EPoseState::JumpingOrFalling);
	TestTrue(FString::Printf(TEXT("Jumping lifts the angle up to the new pose base (%.4f, expected 2.5000)"), State.CurrentSpreadAngle),
		FMath::IsNearlyEqual(State.CurrentSpreadAngle, 2.5f, Tolerance));

	// 空中回落速率为 0：停火再久也不回落（对应"跳在空中不打枪也一直散着"）
	StepFrame(State, *Profile, 3.0f, EPoseState::JumpingOrFalling);
	TestTrue(FString::Printf(TEXT("Airborne pose does not recover with rate 0 (%.4f)"), State.CurrentSpreadAngle),
		FMath::IsNearlyEqual(State.CurrentSpreadAngle, 2.5f, Tolerance));

	// --- 落地回到站定：落到站定基础角 1.0 ---
	StepFrame(State, *Profile, 2.0f, EPoseState::Standing);
	TestTrue(FString::Printf(TEXT("Landing recovers back to the standing base (%.4f, expected 1.0000)"), State.CurrentSpreadAngle),
		FMath::IsNearlyEqual(State.CurrentSpreadAngle, 1.0f, Tolerance));

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 5) 玩家侧倍率：瞄准混合 + 移动 ramp（含带宽为 0 的退化区间）
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilSpreadPlayerMultiplierTest, "Lyra.Recoil.Spread.PlayerMultipliers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilSpreadPlayerMultiplierTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilSpreadTest;

	ULyraRecoilProfile* Profile = MakeSpreadProfile();

	// --- 瞄准：Lerp(1, 0.5, Alpha) ---
	TestTrue(TEXT("Aiming alpha 0 -> multiplier 1.0"),
		FMath::IsNearlyEqual(Profile->GetSpreadAimingMultiplier(0.0f), 1.0f, Tolerance));
	TestTrue(TEXT("Aiming alpha 1 -> multiplier == SpreadMultiplier_Aiming (0.5)"),
		FMath::IsNearlyEqual(Profile->GetSpreadAimingMultiplier(1.0f), 0.5f, Tolerance));
	TestTrue(TEXT("Aiming alpha 0.5 -> multiplier 0.75"),
		FMath::IsNearlyEqual(Profile->GetSpreadAimingMultiplier(0.5f), 0.75f, Tolerance));
	TestTrue(TEXT("Aiming alpha out of range is clamped (alpha 2 -> same as 1)"),
		FMath::IsNearlyEqual(Profile->GetSpreadAimingMultiplier(2.0f), 0.5f, Tolerance));

	// --- 移动：阈值 100，带宽 50，站定倍率 0.4 ---
	TestTrue(TEXT("Speed 0 -> standing-still multiplier 0.4"),
		FMath::IsNearlyEqual(Profile->GetSpreadMovementMultiplierTarget(0.0f), 0.4f, Tolerance));
	TestTrue(TEXT("Speed == threshold -> still 0.4"),
		FMath::IsNearlyEqual(Profile->GetSpreadMovementMultiplierTarget(100.0f), 0.4f, Tolerance));
	TestTrue(TEXT("Speed == threshold + range -> 1.0"),
		FMath::IsNearlyEqual(Profile->GetSpreadMovementMultiplierTarget(150.0f), 1.0f, Tolerance));
	TestTrue(TEXT("Speed halfway through the ramp -> 0.7"),
		FMath::IsNearlyEqual(Profile->GetSpreadMovementMultiplierTarget(125.0f), 0.7f, Tolerance));
	TestTrue(TEXT("Speed above the ramp -> clamped at 1.0"),
		FMath::IsNearlyEqual(Profile->GetSpreadMovementMultiplierTarget(9999.0f), 1.0f, Tolerance));

	// --- 带宽为 0 的退化区间：必须是一个有明确语义的阶跃，而不是 NaN ---
	Profile->SpreadStandingStillToMovingRange = 0.0f;
	TestTrue(TEXT("Zero range: speed at threshold -> 0.4"),
		FMath::IsNearlyEqual(Profile->GetSpreadMovementMultiplierTarget(100.0f), 0.4f, Tolerance));
	TestTrue(TEXT("Zero range: speed above threshold -> 1.0"),
		FMath::IsNearlyEqual(Profile->GetSpreadMovementMultiplierTarget(100.5f), 1.0f, Tolerance));
	TestTrue(TEXT("Zero range: the result is always finite"),
		FMath::IsFinite(Profile->GetSpreadMovementMultiplierTarget(1.0e9f)));

	// --- 合成：最终锥角 = 裸锥角 × 瞄准 × 移动 ---
	FRecoilRuntimeState State;
	State.Reset(Profile);
	State.ApplySpreadShot(Profile, EPoseState::Standing);	// 裸锥角 1.5

	State.SetSpreadPlayerMultipliers(0.5f, 0.4f);
	TestTrue(FString::Printf(TEXT("Effective spread = raw x aim x move (%.4f, expected 0.3000)"), State.GetEffectiveSpreadAngle()),
		FMath::IsNearlyEqual(State.GetEffectiveSpreadAngle(), 1.5f * 0.5f * 0.4f, Tolerance));

	// 负倍率必须被夹成 0，而不是产生一个负锥角（负锥角会让 VRandCone 拿到负半角）
	State.SetSpreadPlayerMultipliers(-1.0f, -1.0f);
	TestTrue(TEXT("Negative player multipliers are clamped to 0"),
		FMath::IsNearlyZero(State.GetEffectiveSpreadAngle(), Tolerance));

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 6) 资产参数解析 + 校验（Max < Base 必须报错，运行时出口兜底）
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilSpreadProfileParamsTest, "Lyra.Recoil.Spread.ProfileParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilSpreadProfileParamsTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilSpreadTest;

	ULyraRecoilProfile* Profile = MakeSpreadProfile();

	const FRecoilSpreadParams Standing = Profile->GetSpreadParams(EPoseState::Standing);
	TestTrue(TEXT("Standing base = 1.0"), FMath::IsNearlyEqual(Standing.BaseAngleDegrees, 1.0f, Tolerance));
	TestTrue(TEXT("Standing max = 3.0"), FMath::IsNearlyEqual(Standing.MaxAngleDegrees, 3.0f, Tolerance));
	TestTrue(TEXT("Standing add per shot = 0.5"), FMath::IsNearlyEqual(Standing.AddPerShotDegrees, 0.5f, Tolerance));
	TestTrue(TEXT("Standing recover rate = 2.0"), FMath::IsNearlyEqual(Standing.RecoverRateDegreesPerSecond, 2.0f, Tolerance));

	const FRecoilSpreadParams Crouching = Profile->GetSpreadParams(EPoseState::Crouching);
	TestTrue(TEXT("Crouching base = 0.5"), FMath::IsNearlyEqual(Crouching.BaseAngleDegrees, 0.5f, Tolerance));
	TestTrue(TEXT("Crouching max = 1.0"), FMath::IsNearlyEqual(Crouching.MaxAngleDegrees, 1.0f, Tolerance));

	const FRecoilSpreadParams Airborne = Profile->GetSpreadParams(EPoseState::JumpingOrFalling);
	TestTrue(TEXT("Airborne base = 2.5"), FMath::IsNearlyEqual(Airborne.BaseAngleDegrees, 2.5f, Tolerance));
	TestTrue(TEXT("Airborne max = 4.0"), FMath::IsNearlyEqual(Airborne.MaxAngleDegrees, 4.0f, Tolerance));

	// --- 正常资产不应报错 ---
	{
		TArray<FString> Errors;
		TestTrue(TEXT("A well-formed profile passes ValidateProfile"), Profile->ValidateProfile(Errors));
		for (const FString& Error : Errors)
		{
			AddInfo(FString::Printf(TEXT("unexpected validation error: %s"), *Error));
		}
	}

	// --- Max < Base：必须被校验报出来 ---
	Profile->MaxSpreadAngle_Standing = 0.2f;	// 低于 base 1.0
	{
		TArray<FString> Errors;
		const bool bValid = Profile->ValidateProfile(Errors);
		TestTrue(TEXT("MaxSpreadAngle_Standing < SpreadAngle_Standing fails validation"), !bValid);

		bool bMentionsTheField = false;
		for (const FString& Error : Errors)
		{
			// 校验消息是英文（控制台按 ANSI 输出中文会乱码），断言只认字段名
			bMentionsTheField |= Error.Contains(TEXT("MaxSpreadAngle_Standing"));
		}
		TestTrue(TEXT("The validation error names the offending field"), bMentionsTheField);
	}

	// --- 运行时出口兜底：GetSpreadParams 把 Max 抬到 >= Base，
	//     这样即使资产配错，也不会出现"第一发就被钳到比基础角还小"的怪现象 ---
	const FRecoilSpreadParams Clamped = Profile->GetSpreadParams(EPoseState::Standing);
	TestTrue(FString::Printf(TEXT("GetSpreadParams raises max to the base when misconfigured (base=%.4f max=%.4f)"),
		Clamped.BaseAngleDegrees, Clamped.MaxAngleDegrees),
		Clamped.MaxAngleDegrees >= Clamped.BaseAngleDegrees - Tolerance);

	// --- 负值也要在出口被夹成 0（资产校验负责报错，出口负责不让负数流进采样）---
	Profile->SpreadAddPerShot_Standing = -5.0f;
	Profile->SpreadRecoverRate_Standing = -5.0f;
	const FRecoilSpreadParams Sanitized = Profile->GetSpreadParams(EPoseState::Standing);
	TestTrue(TEXT("Negative add-per-shot is clamped to 0 at the query exit"),
		FMath::IsNearlyEqual(Sanitized.AddPerShotDegrees, 0.0f, Tolerance));
	TestTrue(TEXT("Negative recover rate is clamped to 0 at the query exit"),
		FMath::IsNearlyEqual(Sanitized.RecoverRateDegreesPerSecond, 0.0f, Tolerance));

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 7) 关闭开关后必须回到 Lyra 原生 heat 语义：散布字段一个都不该被读
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilSpreadToggleBackTest, "Lyra.Recoil.Spread.ToggleBackIsClean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilSpreadToggleBackTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilSpreadTest;

	ULyraRecoilProfile* Profile = MakeSpreadProfile();

	FRecoilRuntimeState State;
	State.Reset(Profile);

	// 先累起来
	for (int32 Shot = 0; Shot < 3; ++Shot)
	{
		State.ApplySpreadShot(Profile, EPoseState::Standing);
	}
	TestTrue(TEXT("Spread accumulated while enabled"), State.CurrentSpreadAngle > 1.0f + Tolerance);

	// 关掉开关：Reset 之后必须完全回到 0，而不是带着上一次的锥角。
	// （这一步模拟"策划把开关勾掉"，此时不允许有任何残留值继续影响弹道。）
	Profile->bEnableProfileSpread = false;
	State.Reset(Profile);

	TestTrue(TEXT("After disabling, Reset zeroes the spread angle"),
		FMath::IsNearlyZero(State.CurrentSpreadAngle, Tolerance));
	TestTrue(TEXT("After disabling, the base angle is zeroed too"),
		FMath::IsNearlyZero(State.CurrentSpreadBaseAngle, Tolerance));
	TestTrue(TEXT("After disabling, the max angle is zeroed too"),
		FMath::IsNearlyZero(State.CurrentSpreadMaxAngle, Tolerance));

	// 再发一发：仍然必须是 0（说明 ApplySpreadShot 真的被开关挡住了）
	State.ApplySpreadShot(Profile, EPoseState::Standing);
	TestTrue(TEXT("After disabling, shots no longer accumulate spread"),
		FMath::IsNearlyZero(State.CurrentSpreadAngle, Tolerance));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
