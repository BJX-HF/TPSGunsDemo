// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Weapons/Recoil/LyraRecoilDebug.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"

/**
 * P5 自动验证：Lyra.Recoil.Console.*
 *
 * 开发计划 §P5 的交付物是两个"动作型"命令（Dump / ReloadProfile）加上四个"变量型" CVar。
 * 这些名字是**对外契约**（大祥老师会照着文档在控制台里敲），一旦拼错或没注册，
 * 手动验收直接卡住。所以这里把"名字 + 类型"钉成断言。
 *
 * 为什么不用 -ExecCmds 验证：
 *   本机实测 `-ExecCmds="A; B; Quit"` 的多命令形式不会被执行（`Quit` 也不生效，
 *   编辑器挂住直到超时被杀）—— 一行里塞多个分号命令在这台机器上不可靠。
 *   直接断言注册状态既确定又没有这个坑。
 *
 * 类型断言很重要：CVar 和命令都注册在同一个 IConsoleManager 里，
 * 只断言"名字存在"的话，把 Dump 误注册成变量也照样通过。
 */

namespace LyraRecoilConsoleTest
{
	static const TCHAR* const RecoilCvarNames[] =
	{
		TEXT("Lyra.Recoil.Enable"),
		TEXT("Lyra.Recoil.Scale"),
		TEXT("Lyra.Recoil.Debug"),
		TEXT("Lyra.Recoil.DebugDraw")
	};

	static const TCHAR* const RecoilCommandNames[] =
	{
		TEXT("Lyra.Recoil.Dump"),
		TEXT("Lyra.Recoil.ReloadProfile")
	};
}

//////////////////////////////////////////////////////////////////////////
// 注册契约：4 个变量 + 2 个命令
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilConsoleRegistrationTest, "Lyra.Recoil.Console.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilConsoleRegistrationTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilConsoleTest;

	IConsoleManager& ConsoleManager = IConsoleManager::Get();

	// --- 变量型 ---
	for (const TCHAR* Name : RecoilCvarNames)
	{
		IConsoleObject* Object = ConsoleManager.FindConsoleObject(Name, /*bTrackFrequentCalls=*/ false);

		if (!TestNotNull(FString::Printf(TEXT("console variable is registered: %s"), Name), Object))
		{
			continue;
		}

		TestNotNull(FString::Printf(TEXT("%s is a variable (not a command)"), Name), Object->AsVariable());
		TestTrue(FString::Printf(TEXT("%s is NOT a command"), Name), Object->AsCommand() == nullptr);
	}

	// --- 动作型 ---
	for (const TCHAR* Name : RecoilCommandNames)
	{
		IConsoleObject* Object = ConsoleManager.FindConsoleObject(Name, /*bTrackFrequentCalls=*/ false);

		if (!TestNotNull(FString::Printf(TEXT("console command is registered: %s"), Name), Object))
		{
			continue;
		}

		TestNotNull(FString::Printf(TEXT("%s is a command (not a variable)"), Name), Object->AsCommand());
		TestTrue(FString::Printf(TEXT("%s is NOT a variable"), Name), Object->AsVariable() == nullptr);
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 空世界安全性：命令在没有世界/没持枪时必须安静失败，不能崩、不能写文件
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilConsoleNullWorldTest, "Lyra.Recoil.Console.NullWorldSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilConsoleNullWorldTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilConsoleTest;

	IConsoleManager& ConsoleManager = IConsoleManager::Get();

	// 这是"自动跑一遍 Dump 的失败分支"：DedicatedServer / 菜单 / 手无寸铁都会走到这里。
	const FString DumpPathBefore = ULyraRecoilDebug::GetLastDumpPath();

	for (const TCHAR* Name : RecoilCommandNames)
	{
		IConsoleObject* Object = ConsoleManager.FindConsoleObject(Name, /*bTrackFrequentCalls=*/ false);
		IConsoleCommand* Command = (Object != nullptr) ? Object->AsCommand() : nullptr;

		if (!TestNotNull(FString::Printf(TEXT("%s is executable"), Name), Command))
		{
			continue;
		}

		const TArray<FString> NoArgs;
		// InWorld = nullptr：模拟没有本地玩家武器的场景
		Command->Execute(NoArgs, /*InWorld=*/ nullptr, *GLog);
	}

	TestEqual(TEXT("Executing Dump without a world must not write a file"),
		ULyraRecoilDebug::GetLastDumpPath(), DumpPathBefore);

	// 顺便把"取当前武器"在无世界时的行为也钉住
	TestTrue(TEXT("FindLocalPlayerRangedWeapon(nullptr) returns nullptr"),
		ULyraRecoilDebug::FindLocalPlayerRangedWeapon(nullptr) == nullptr);

	return true;
}

//////////////////////////////////////////////////////////////////////////
// Lyra.Recoil.Scale 必须真的流进算法
//
// 这条除了断言功能本身，还顺带产出**两份只有 Scale 不同的 CSV** ——
// 它们正好是 P5 手动验收里"两条曲线叠加可见差异"那一步的可复现素材。
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilScaleAffectsKickTest, "Lyra.Recoil.Scale.AffectsDump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilScaleAffectsKickTest::RunTest(const FString& Parameters)
{
	// 用真实交付资产，避免和 Dump 测试里的内存 Profile 重复造一套参数
	ULyraRecoilProfile* Profile = LoadObject<ULyraRecoilProfile>(
		nullptr, TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle.DA_Recoil_Rifle"));

	if (!TestNotNull(TEXT("Rifle recoil profile is loadable"), Profile))
	{
		return false;
	}

	IConsoleVariable* ScaleCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Lyra.Recoil.Scale"));
	if (!TestNotNull(TEXT("Lyra.Recoil.Scale is registered"), ScaleCVar))
	{
		return false;
	}

	const float OriginalScale = ScaleCVar->GetFloat();

	constexpr int32 NumShots = 17;

	auto FireAndDump = [ScaleCVar, Profile, NumShots](float InScale, FString& OutPath) -> bool
	{
		ScaleCVar->Set(InScale);

		FRecoilRuntimeState State;
		State.Reset(Profile);
		State.SetGlobalScale(ULyraRecoilDebug::GetGlobalScale());

		for (int32 Shot = 0; Shot < NumShots; ++Shot)
		{
			State.ApplyShot(Profile, 1.0f);
			State.SetGlobalScale(ULyraRecoilDebug::GetGlobalScale());
		}

		return ULyraRecoilDebug::DumpShotHistoryToCsv(Profile, State, OutPath);
	};

	FString FullScalePath;
	FString HalfScalePath;

	const bool bDumpedFull = FireAndDump(1.0f, FullScalePath);
	const bool bDumpedHalf = FireAndDump(0.5f, HalfScalePath);

	// 恢复现场，避免污染后续测试
	ScaleCVar->Set(OriginalScale);
	TestTrue(TEXT("Scale CVar restored"), FMath::IsNearlyEqual(ScaleCVar->GetFloat(), OriginalScale, 1e-6f));

	if (!TestTrue(TEXT("Dumped at Scale=1.0"), bDumpedFull) || !TestTrue(TEXT("Dumped at Scale=0.5"), bDumpedHalf))
	{
		return false;
	}

	TestTrue(FString::Printf(TEXT("The two dumps are different files (%s / %s)"), *FullScalePath, *HalfScalePath),
		FullScalePath != HalfScalePath);

	AddInfo(FString::Printf(TEXT("Scale=1.0 dump: %s"), *FullScalePath));
	AddInfo(FString::Printf(TEXT("Scale=0.5 dump: %s"), *HalfScalePath));

	// --- 逐发比对：Scale=0.5 的每一发都必须正好是一半 ---
	auto ReadColumn = [](const FString& FilePath, int32 ColumnIndex, TArray<float>& OutValues) -> bool
	{
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *FilePath))
		{
			return false;
		}

		TArray<FString> Lines;
		Raw.ParseIntoArrayLines(Lines, /*bCullEmpty=*/ true);

		for (int32 Index = 1; Index < Lines.Num(); ++Index)   // 跳过表头
		{
			TArray<FString> Columns;
			Lines[Index].ParseIntoArray(Columns, TEXT(","), /*InCullEmpty=*/ false);
			if (Columns.IsValidIndex(ColumnIndex))
			{
				OutValues.Add(FCString::Atof(*Columns[ColumnIndex]));
			}
		}

		return OutValues.Num() > 0;
	};

	TArray<float> FullKicks;
	TArray<float> HalfKicks;

	// 第 1 列 = VerticalKick
	if (!TestTrue(TEXT("Read VerticalKick from the Scale=1.0 dump"), ReadColumn(FullScalePath, 1, FullKicks))
		|| !TestTrue(TEXT("Read VerticalKick from the Scale=0.5 dump"), ReadColumn(HalfScalePath, 1, HalfKicks)))
	{
		return false;
	}

	if (!TestEqual(TEXT("Both dumps have the same shot count"), FullKicks.Num(), HalfKicks.Num()))
	{
		return false;
	}

	for (int32 Index = 0; Index < FullKicks.Num(); ++Index)
	{
		const float ExpectedHalved = FullKicks[Index] * 0.5f;
		TestTrue(FString::Printf(TEXT("Shot %d: Scale=0.5 kick is exactly half of Scale=1.0 (%.6f vs %.6f)"),
			Index, HalfKicks[Index], ExpectedHalved),
			FMath::IsNearlyEqual(HalfKicks[Index], ExpectedHalved, 1e-4f));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
