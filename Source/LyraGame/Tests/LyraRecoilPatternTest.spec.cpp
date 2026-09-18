// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Weapons/Recoil/LyraRecoilDebug.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"

/**
 * P3 自动验证：Lyra.Recoil.Pattern.*
 *
 * 开发计划 §P3 要求 4 条断言：
 *   1. 固定种子连发 20 发，方向偏移序列与 Golden 数据逐发比对（1e-4 容差）
 *   2. 前 PatternLength 发的水平偏移严格等于 PatternPoints 中的配置值
 *   3. PatternLength 之后进入伪随机区间：仍落在 HorizontalRandomRange 内且同种子可复现
 *   4. 开关关闭时弹道偏移恒为 0（不污染原有扩散逻辑）
 *
 * 全部基于**真实交付资产** /Game/Weapons/Recoil/DA_Recoil_Rifle —— P3 的依赖是 P1，
 * 所以这里不再造内存 Profile，直接用资产，顺便验证"参数真的来自资产"。
 *
 * 关于 Golden 数据：它是**回归锁**，由 LyraRecoilGoldenDump 从实现导出。
 * 因此除逐发比对外，还额外断言 golden 里的关键字段与资产当前值一致 ——
 * 一旦资产被改动而没重新导出，测试会明确报 "golden is stale" 而不是含糊地失败。
 */
namespace LyraRecoilPatternTest
{
	static const TCHAR* const RiflePackage = TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle");

	static constexpr float Tolerance = 1e-4f;

	/** Golden 覆盖的发数，与导出 Commandlet 保持一致 */
	static constexpr int32 GoldenShotCount = 20;

	static ULyraRecoilProfile* LoadRifle()
	{
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), RiflePackage, *FPaths::GetBaseFilename(RiflePackage));
		return LoadObject<ULyraRecoilProfile>(nullptr, *ObjectPath);
	}

	static FString GetGoldenFilePath()
	{
		return FPaths::ProjectDir() / TEXT("Source/LyraGame/Tests/Data/RecoilGolden_DA_Recoil_Rifle.json");
	}

	static bool LoadGolden(TSharedPtr<FJsonObject>& OutRoot)
	{
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *GetGoldenFilePath()))
		{
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		return FJsonSerializer::Deserialize(Reader, OutRoot) && OutRoot.IsValid();
	}

	/** Golden 过期提示，统一文案便于搜索 */
	static const TCHAR* StaleGoldenHint = TEXT("golden data is stale - re-run: UnrealEditor-Cmd <uproject> -run=LyraRecoilGoldenDump");
}

//////////////////////////////////////////////////////////////////////////
// 断言 1：Golden 逐发比对
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilPatternGoldenTest, "Lyra.Recoil.Pattern.Golden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilPatternGoldenTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilPatternTest;

	ULyraRecoilProfile* Profile = LoadRifle();
	if (!TestNotNull(TEXT("Rifle recoil profile is loadable (run -run=LyraRecoilAssetGen first)"), Profile))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	if (!TestTrue(FString::Printf(TEXT("Golden file exists and parses: %s"), *GetGoldenFilePath()), LoadGolden(Root)))
	{
		return false;
	}

	const int32 Seed = FRecoilRuntimeState::ResolveSeed(Profile);

	// --- golden 过期检测：关键输入必须与资产当前值一致 ---
	TestTrue(FString::Printf(TEXT("Golden seed matches asset (asset=%d). %s"), Seed, StaleGoldenHint),
		Root->GetIntegerField(TEXT("seed")) == Seed);

	TestTrue(FString::Printf(TEXT("Golden patternLength matches asset (asset=%d). %s"), Profile->PatternLength, StaleGoldenHint),
		Root->GetIntegerField(TEXT("patternLength")) == Profile->PatternLength);

	TestTrue(FString::Printf(TEXT("Golden recoilPerShotVertical matches asset (asset=%.6f). %s"), Profile->RecoilPerShot_Vertical, StaleGoldenHint),
		FMath::IsNearlyEqual(static_cast<float>(Root->GetNumberField(TEXT("recoilPerShotVertical"))), Profile->RecoilPerShot_Vertical, Tolerance));

	TestTrue(FString::Printf(TEXT("Golden recoilPerShotHorizontal matches asset (asset=%.6f). %s"), Profile->RecoilPerShot_Horizontal, StaleGoldenHint),
		FMath::IsNearlyEqual(static_cast<float>(Root->GetNumberField(TEXT("recoilPerShotHorizontal"))), Profile->RecoilPerShot_Horizontal, Tolerance));

	TestTrue(FString::Printf(TEXT("Golden horizontalRandomRange matches asset (asset=%.6f). %s"), Profile->HorizontalRandomRange, StaleGoldenHint),
		FMath::IsNearlyEqual(static_cast<float>(Root->GetNumberField(TEXT("horizontalRandomRange"))), Profile->HorizontalRandomRange, Tolerance));

	// --- 逐发比对 ---
	const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
	if (!Root->TryGetArrayField(TEXT("shots"), Shots) || (Shots == nullptr))
	{
		AddError(TEXT("Golden file has no 'shots' array"));
		return false;
	}

	TestEqual(TEXT("Golden shot count"), Shots->Num(), GoldenShotCount);

	for (int32 ShotIndex = 0; ShotIndex < Shots->Num(); ++ShotIndex)
	{
		const TSharedPtr<FJsonObject> ShotObject = (*Shots)[ShotIndex]->AsObject();
		if (!ShotObject.IsValid())
		{
			AddError(FString::Printf(TEXT("Golden shots[%d] is not an object"), ShotIndex));
			continue;
		}

		const float GoldenVertical = static_cast<float>(ShotObject->GetNumberField(TEXT("vertical")));
		const float GoldenHorizontal = static_cast<float>(ShotObject->GetNumberField(TEXT("horizontal")));

		const FRecoilShotKick Kick = FRecoilRuntimeState::ComputeShotKick(*Profile, ShotIndex, 1.0f, 1.0f, Seed);

		TestTrue(
			FString::Printf(TEXT("Shot %d vertical matches golden (golden=%.6f actual=%.6f)"), ShotIndex, GoldenVertical, Kick.Vertical),
			FMath::IsNearlyEqual(Kick.Vertical, GoldenVertical, Tolerance));

		TestTrue(
			FString::Printf(TEXT("Shot %d horizontal matches golden (golden=%.6f actual=%.6f)"), ShotIndex, GoldenHorizontal, Kick.Horizontal),
			FMath::IsNearlyEqual(Kick.Horizontal, GoldenHorizontal, Tolerance));
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 断言 2：固定 Pattern 区间严格等于配置值
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilPatternFixedRegionTest, "Lyra.Recoil.Pattern.FixedRegion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilPatternFixedRegionTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilPatternTest;

	ULyraRecoilProfile* Profile = LoadRifle();
	if (!TestNotNull(TEXT("Rifle recoil profile is loadable"), Profile))
	{
		return false;
	}

	const int32 Seed = FRecoilRuntimeState::ResolveSeed(Profile);

	TestTrue(FString::Printf(TEXT("Asset has a non-empty fixed pattern region (PatternLength=%d, PatternPoints=%d)"),
		Profile->PatternLength, Profile->PatternPoints.Num()),
		Profile->PatternLength > 0 && Profile->PatternLength <= Profile->PatternPoints.Num());

	for (int32 ShotIndex = 0; ShotIndex < Profile->PatternLength; ++ShotIndex)
	{
		const FRecoilPatternPoint Point = Profile->GetPatternPoint(ShotIndex);
		const FRecoilShotKick Kick = FRecoilRuntimeState::ComputeShotKick(*Profile, ShotIndex, 1.0f, 1.0f, Seed);

		// 水平分量不经过 VerticalKickCurve，必须严格等于 配置值 × 基准水平 Kick
		const float ExpectedHorizontal = Profile->RecoilPerShot_Horizontal * Point.X;
		TestTrue(
			FString::Printf(TEXT("Shot %d horizontal == PatternPoints[%d].X x RecoilPerShot_Horizontal (expected %.6f actual %.6f)"),
				ShotIndex, ShotIndex, ExpectedHorizontal, Kick.Horizontal),
			FMath::IsNearlyEqual(Kick.Horizontal, ExpectedHorizontal, Tolerance));

		// 垂直分量 = 配置值 × 基准垂直 Kick × VerticalKickCurve
		const float ExpectedVertical = Profile->RecoilPerShot_Vertical * Point.Y * Profile->GetVerticalKickCurveScale(ShotIndex);
		TestTrue(
			FString::Printf(TEXT("Shot %d vertical == PatternPoints[%d].Y x RecoilPerShot_Vertical x VerticalKickCurve (expected %.6f actual %.6f)"),
				ShotIndex, ShotIndex, ExpectedVertical, Kick.Vertical),
			FMath::IsNearlyEqual(Kick.Vertical, ExpectedVertical, Tolerance));
	}

	// 固定区间必须与种子无关（这是"可复现弹道"的基础）
	for (int32 ShotIndex = 0; ShotIndex < Profile->PatternLength; ++ShotIndex)
	{
		const FRecoilShotKick KickA = FRecoilRuntimeState::ComputeShotKick(*Profile, ShotIndex, 1.0f, 1.0f, 111);
		const FRecoilShotKick KickB = FRecoilRuntimeState::ComputeShotKick(*Profile, ShotIndex, 1.0f, 1.0f, 222);

		TestTrue(FString::Printf(TEXT("Shot %d inside the fixed region is seed independent (%.6f vs %.6f)"),
			ShotIndex, KickA.Vertical, KickB.Vertical),
			FMath::IsNearlyEqual(KickA.Vertical, KickB.Vertical, Tolerance)
			&& FMath::IsNearlyEqual(KickA.Horizontal, KickB.Horizontal, Tolerance));
	}

	// 成员接口（GetShotDirectionOffset，走状态里的倍率）与静态计算必须一致
	{
		FRecoilRuntimeState State;
		State.Reset(Profile);
		for (int32 ShotIndex = 0; ShotIndex < Profile->PatternLength; ++ShotIndex)
		{
			const FRecoilShotKick ViaState = State.GetShotDirectionOffset(*Profile, ShotIndex);
			const FRecoilShotKick ViaStatic = FRecoilRuntimeState::ComputeShotKick(
				*Profile, ShotIndex, State.CurrentPoseMultiplier, State.GlobalScale, State.ActiveSeed);

			TestTrue(FString::Printf(TEXT("Shot %d: GetShotDirectionOffset matches ComputeShotKick (%.6f vs %.6f)"),
				ShotIndex, ViaState.Vertical, ViaStatic.Vertical),
				FMath::IsNearlyEqual(ViaState.Vertical, ViaStatic.Vertical, Tolerance)
				&& FMath::IsNearlyEqual(ViaState.Horizontal, ViaStatic.Horizontal, Tolerance));
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 断言 3：伪随机区间
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilPatternRandomWalkTest, "Lyra.Recoil.Pattern.RandomWalk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilPatternRandomWalkTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilPatternTest;

	ULyraRecoilProfile* Profile = LoadRifle();
	if (!TestNotNull(TEXT("Rifle recoil profile is loadable"), Profile))
	{
		return false;
	}

	const int32 Seed = FRecoilRuntimeState::ResolveSeed(Profile);

	const float MaxHorizontalShape = Profile->HorizontalRandomRange;
	const float MaxHorizontalKick = Profile->RecoilPerShot_Horizontal * Profile->HorizontalRandomRange;

	constexpr int32 NumWalkShots = 12;

	int32 NumChecked = 0;
	for (int32 ShotIndex = Profile->PatternLength; ShotIndex < Profile->PatternLength + NumWalkShots; ++ShotIndex)
	{
		++NumChecked;

		const float WalkShape = FRecoilRuntimeState::ComputePatternHorizontal(*Profile, ShotIndex, Seed);
		TestTrue(
			FString::Printf(TEXT("Shot %d random walk shape within HorizontalRandomRange (%.6f <= %.6f)"), ShotIndex, FMath::Abs(WalkShape), MaxHorizontalShape + Tolerance),
			FMath::Abs(WalkShape) <= MaxHorizontalShape + Tolerance);

		const FRecoilShotKick Kick = FRecoilRuntimeState::ComputeShotKick(*Profile, ShotIndex, 1.0f, 1.0f, Seed);
		TestTrue(
			FString::Printf(TEXT("Shot %d horizontal kick within range (%.6f <= %.6f)"), ShotIndex, FMath::Abs(Kick.Horizontal), MaxHorizontalKick + Tolerance),
			FMath::Abs(Kick.Horizontal) <= MaxHorizontalKick + Tolerance);

		// 同种子可复现
		const FRecoilShotKick Repeat = FRecoilRuntimeState::ComputeShotKick(*Profile, ShotIndex, 1.0f, 1.0f, Seed);
		TestTrue(
			FString::Printf(TEXT("Shot %d is reproducible with the same seed (%.6f vs %.6f)"), ShotIndex, Kick.Horizontal, Repeat.Horizontal),
			FMath::IsNearlyEqual(Kick.Horizontal, Repeat.Horizontal, 1e-6f));
	}

	TestTrue(FString::Printf(TEXT("Pseudo-random region actually exercised (%d shots)"), NumChecked), NumChecked > 0);

	// 不同种子必须产生不同序列，否则"种子可控"就是假的
	bool bSeedHasEffect = false;
	for (int32 ShotIndex = Profile->PatternLength; ShotIndex < Profile->PatternLength + NumWalkShots; ++ShotIndex)
	{
		const FRecoilShotKick KickA = FRecoilRuntimeState::ComputeShotKick(*Profile, ShotIndex, 1.0f, 1.0f, 111);
		const FRecoilShotKick KickB = FRecoilRuntimeState::ComputeShotKick(*Profile, ShotIndex, 1.0f, 1.0f, 222);

		if (!FMath::IsNearlyEqual(KickA.Horizontal, KickB.Horizontal, 1e-6f))
		{
			bSeedHasEffect = true;
			break;
		}
	}

	TestTrue(TEXT("Different seeds produce different walk sequences"), bSeedHasEffect);

	// 顺序无关：先算第 30 发再算第 10 发，结果必须一致（哈希式采样，不是有状态随机流）
	{
		const FRecoilShotKick ForwardFirst = FRecoilRuntimeState::ComputeShotKick(*Profile, 10, 1.0f, 1.0f, Seed);
		const FRecoilShotKick BackwardFirst = FRecoilRuntimeState::ComputeShotKick(*Profile, 30, 1.0f, 1.0f, Seed);
		const FRecoilShotKick ForwardAgain = FRecoilRuntimeState::ComputeShotKick(*Profile, 10, 1.0f, 1.0f, Seed);

		TestTrue(FString::Printf(TEXT("Shot 10 is order independent (%.6f vs %.6f)"), ForwardFirst.Horizontal, ForwardAgain.Horizontal),
			FMath::IsNearlyEqual(ForwardFirst.Horizontal, ForwardAgain.Horizontal, 1e-6f));
		TestTrue(TEXT("Shot 30 was actually evaluated (guards against dead code elimination)"),
			!FMath::IsNaN(BackwardFirst.Horizontal));
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 断言 4：总开关关闭时偏移恒为 0
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilPatternEnableGateTest, "Lyra.Recoil.Pattern.EnableGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilPatternEnableGateTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilPatternTest;

	ULyraRecoilProfile* Profile = LoadRifle();
	if (!TestNotNull(TEXT("Rifle recoil profile is loadable"), Profile))
	{
		return false;
	}

	IConsoleVariable* EnableCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Lyra.Recoil.Enable"));
	if (!TestNotNull(TEXT("Lyra.Recoil.Enable console variable is registered"), EnableCVar))
	{
		return false;
	}

	const int32 OriginalValue = EnableCVar->GetInt();
	const int32 Seed = FRecoilRuntimeState::ResolveSeed(Profile);

	// --- 开启：偏移必须非零 ---
	EnableCVar->Set(1);
	TestTrue(TEXT("IsRecoilEnabled() reflects CVar=1"), ULyraRecoilDebug::IsRecoilEnabled());

	const FRecoilShotKick EnabledKick = FRecoilRuntimeState::ComputeShotKickGated(
		Profile, 0, 1.0f, 1.0f, Seed, ULyraRecoilDebug::IsRecoilEnabled());

	TestTrue(FString::Printf(TEXT("Enabled: vertical offset is non-zero (%.6f)"), EnabledKick.Vertical),
		!FMath::IsNearlyZero(EnabledKick.Vertical));

	// --- 关闭：偏移必须严格为 0，不污染 Lyra 原有扩散 ---
	EnableCVar->Set(0);
	TestTrue(TEXT("IsRecoilEnabled() reflects CVar=0"), !ULyraRecoilDebug::IsRecoilEnabled());

	const FRecoilShotKick DisabledKick = FRecoilRuntimeState::ComputeShotKickGated(
		Profile, 0, 1.0f, 1.0f, Seed, ULyraRecoilDebug::IsRecoilEnabled());

	TestTrue(FString::Printf(TEXT("Disabled: vertical offset is exactly 0 (%.6f)"), DisabledKick.Vertical),
		DisabledKick.Vertical == 0.0f);
	TestTrue(FString::Printf(TEXT("Disabled: horizontal offset is exactly 0 (%.6f)"), DisabledKick.Horizontal),
		DisabledKick.Horizontal == 0.0f);

	// --- 无资产：即使开关开着也必须是 0 ---
	EnableCVar->Set(1);
	const FRecoilShotKick NoProfileKick = FRecoilRuntimeState::ComputeShotKickGated(
		nullptr, 0, 1.0f, 1.0f, Seed, /*bRecoilEnabled=*/ true);

	TestTrue(FString::Printf(TEXT("No profile: vertical offset is exactly 0 (%.6f)"), NoProfileKick.Vertical),
		NoProfileKick.Vertical == 0.0f);
	TestTrue(FString::Printf(TEXT("No profile: horizontal offset is exactly 0 (%.6f)"), NoProfileKick.Horizontal),
		NoProfileKick.Horizontal == 0.0f);

	// --- 恢复现场 ---
	EnableCVar->Set(OriginalValue);
	TestTrue(FString::Printf(TEXT("CVar restored to %d"), OriginalValue), EnableCVar->GetInt() == OriginalValue);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
