// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "Weapons/Recoil/LyraRecoilDebug.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"

/**
 * P5 自动验证：Lyra.Recoil.Dump.*
 *
 * 开发计划 §P5 要求：
 *   - 程序化发射 N 发（N = 17，非整数倍用于覆盖边界）后触发 Dump
 *   - 断言 CSV 行数 == N
 *   - 断言 CSV 数值与 P2 自动化测试中同参数下的计算值一致
 *
 * 这里刻意复用 P2（LyraRecoilTest.spec.cpp）那套"整数友好"的测试参数，
 * 这样"CSV 里的数值 == P2 算出来的数值"是可人工手算复核的。
 *
 * 本文件测的是**导出链路本身**（列顺序、行数、数值精度、空历史的行为）。
 * 导出到磁盘是纯 IO，不需要 UWorld，所以可以完整自动化。
 */

namespace LyraRecoilDumpTest
{
	static constexpr float Tolerance = 1e-4f;

	/**
	 * 冻结的列顺序契约。
	 *
	 * P5 起初是 6 列（对应 FRecoilShotResult 的全部字段）；P8 追加了第 7 列 RollShake；
	 * P10（散布并入后坐力资产）追加了第 8 列 SpreadAngle。
	 *
	 * 注意这两列与前 6 列的语义都**不同**：
	 *   RollShake   —— Roll 解析解在开火瞬间（t=0）的采样，不是"本发累计"
	 *   SpreadAngle —— 本发弹道实际使用的散布锥角（全锥角，度），同样不是累计量
	 * 之所以仍放进同一行，是为了"一行内看到三轴的起点"。
	 * 本常量是唯一契约点：改 CSV 就必须改这里，测试会立刻拦住。
	 */
	static const TCHAR* const ExpectedHeader = TEXT("ShotIndex,VerticalKick,HorizontalKick,AccumulatedPitch,AccumulatedYaw,TimeSinceFire,RollShake,SpreadAngle");

	/** 列数（供断言使用，避免各处硬编码 6/7/8） */
	static constexpr int32 ExpectedColumnCount = 8;

	/** 每发之间推进的时间（秒）：5 × 1ms = 5ms，远小于 RecoveryDelay，保证整轮停在 Accumulating */
	static constexpr int32 StepsBetweenShots = 5;
	static constexpr float StepSeconds = 0.001f;

	/** 与 LyraRecoilTest.spec.cpp 完全一致的参数，便于交叉核对 */
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

		// X 非 0，让水平列也有非平凡数值（覆盖列映射错误）
		Profile->PatternPoints.Reset();
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Profile->PatternPoints.Emplace(0.4f, 1.0f);
		}
		Profile->PatternLength = 8;

		Profile->RandomSeedMode = ERecoilRandomSeedMode::Fixed;
		Profile->FixedRandomSeed = 12345;

		return Profile;
	}

	/** 把 CSV 读回来切成行（去掉空行与行尾 \r） */
	static TArray<FString> ReadCsvLines(const FString& FilePath)
	{
		TArray<FString> Lines;

		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *FilePath))
		{
			return Lines;
		}

		TArray<FString> SplitLines;
		Raw.ParseIntoArrayLines(SplitLines, /*bCullEmpty=*/ true);

		for (FString& Line : SplitLines)
		{
			Line.TrimStartAndEndInline();
			if (!Line.IsEmpty())
			{
				Lines.Add(Line);
			}
		}

		return Lines;
	}

	static TArray<FString> SplitColumns(const FString& Line)
	{
		TArray<FString> Columns;
		Line.ParseIntoArray(Columns, TEXT(","), /*InCullEmpty=*/ false);
		return Columns;
	}
}

//////////////////////////////////////////////////////////////////////////
// 行数 + 数值一致性
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilDumpRowCountAndValuesTest, "Lyra.Recoil.Dump.RowCountAndValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilDumpRowCountAndValuesTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilDumpTest;

	ULyraRecoilProfile* Profile = MakeTestProfile();

	// N = 17：非整数倍、非 2 的幂，专门用来卡"少写一行/多写一行/表头算进数据行"这类边界
	constexpr int32 NumShots = 17;

	FRecoilRuntimeState State;
	State.Reset(Profile);

	for (int32 Shot = 0; Shot < NumShots; ++Shot)
	{
		State.ApplyShot(Profile, 1.0f);

		for (int32 Step = 0; Step < StepsBetweenShots; ++Step)
		{
			State.Advance(Profile, StepSeconds);
		}
	}

	TestEqual(TEXT("ShotHistory holds exactly N shots"), State.ShotHistory.Num(), NumShots);

	FString DumpPath;
	if (!TestTrue(TEXT("DumpShotHistoryToCsv succeeded"), ULyraRecoilDebug::DumpShotHistoryToCsv(Profile, State, DumpPath)))
	{
		return false;
	}

	AddInfo(FString::Printf(TEXT("CSV written to: %s"), *DumpPath));

	TestTrue(FString::Printf(TEXT("Dump path is under Saved/ (%s)"), *DumpPath),
		DumpPath.StartsWith(FPaths::ProjectSavedDir()));

	const TArray<FString> Lines = ReadCsvLines(DumpPath);

	// --- 行数断言：1 行表头 + N 行数据 ---
	TestEqual(FString::Printf(TEXT("CSV line count == 1 header + %d data rows"), NumShots), Lines.Num(), NumShots + 1);

	if (Lines.Num() < 1)
	{
		return false;
	}

	// --- 表头 ---
	TestTrue(FString::Printf(TEXT("CSV header matches the plan column spec (actual: %s)"), *Lines[0]),
		Lines[0] == FString(ExpectedHeader));

	// --- 逐行数值：与 P2 同参数下的计算值比对 ---
	float ExpectedAccumulatedPitch = 0.0f;
	float ExpectedAccumulatedYaw = 0.0f;

	const int32 Seed = FRecoilRuntimeState::ResolveSeed(Profile);

	for (int32 ShotIndex = 0; ShotIndex < NumShots; ++ShotIndex)
	{
		const FRecoilShotKick Kick = FRecoilRuntimeState::ComputeShotKick(*Profile, ShotIndex, 1.0f, 1.0f, Seed);
		ExpectedAccumulatedPitch += Kick.Vertical;
		ExpectedAccumulatedYaw += Kick.Horizontal;

		const FString& Line = Lines[ShotIndex + 1];
		const TArray<FString> Columns = SplitColumns(Line);

		if (!TestEqual(FString::Printf(TEXT("Row %d column count"), ShotIndex), Columns.Num(), ExpectedColumnCount))
		{
			continue;
		}

		const int32 CsvShotIndex = FCString::Atoi(*Columns[0]);
		const float CsvVertical = FCString::Atof(*Columns[1]);
		const float CsvHorizontal = FCString::Atof(*Columns[2]);
		const float CsvAccumulatedPitch = FCString::Atof(*Columns[3]);
		const float CsvAccumulatedYaw = FCString::Atof(*Columns[4]);
		const float CsvTimeSinceFire = FCString::Atof(*Columns[5]);
		// Columns[6] = RollShake：语义与上面六列不同（解析解在 t=0 的采样，非累加量），
		// 因此这一列不做"逐发累加"比对，只断言它是一个有限的、非超调的数值。
		const float CsvRollShake = FCString::Atof(*Columns[6]);

		// Columns[7] = SpreadAngle：本发实际使用的散布锥角。
		// 本测试用的 Profile 没有打开 bEnableProfileSpread，所以必然是 0 ——
		// 这正好把"未启用资产散布时这一列恒为 0"这条契约也钉住了
		// （启用后的逐发数值由 Lyra.Recoil.Spread.* 覆盖）。
		const float CsvSpreadAngle = FCString::Atof(*Columns[7]);

		TestTrue(FString::Printf(TEXT("Row %d RollShake is finite (%.6f)"), ShotIndex, CsvRollShake),
			FMath::IsFinite(CsvRollShake));

		TestTrue(FString::Printf(TEXT("Row %d SpreadAngle is 0 when profile spread is disabled (csv=%.6f)"),
			ShotIndex, CsvSpreadAngle),
			FMath::IsNearlyZero(CsvSpreadAngle, Tolerance));

		TestEqual(FString::Printf(TEXT("Row %d ShotIndex"), ShotIndex), CsvShotIndex, ShotIndex);

		TestTrue(FString::Printf(TEXT("Row %d VerticalKick (csv=%.6f expected=%.6f)"), ShotIndex, CsvVertical, Kick.Vertical),
			FMath::IsNearlyEqual(CsvVertical, Kick.Vertical, Tolerance));

		TestTrue(FString::Printf(TEXT("Row %d HorizontalKick (csv=%.6f expected=%.6f)"), ShotIndex, CsvHorizontal, Kick.Horizontal),
			FMath::IsNearlyEqual(CsvHorizontal, Kick.Horizontal, Tolerance));

		TestTrue(FString::Printf(TEXT("Row %d AccumulatedPitch (csv=%.6f expected=%.6f)"), ShotIndex, CsvAccumulatedPitch, ExpectedAccumulatedPitch),
			FMath::IsNearlyEqual(CsvAccumulatedPitch, ExpectedAccumulatedPitch, Tolerance));

		TestTrue(FString::Printf(TEXT("Row %d AccumulatedYaw (csv=%.6f expected=%.6f)"), ShotIndex, CsvAccumulatedYaw, ExpectedAccumulatedYaw),
			FMath::IsNearlyEqual(CsvAccumulatedYaw, ExpectedAccumulatedYaw, Tolerance));

		// 首发 TimeSinceFire 必须是 0，之后每发都是 StepsBetweenShots × StepSeconds
		const float ExpectedTimeSinceFire = (ShotIndex == 0) ? 0.0f : (StepsBetweenShots * StepSeconds);
		TestTrue(FString::Printf(TEXT("Row %d TimeSinceFire (csv=%.6f expected=%.6f)"), ShotIndex, CsvTimeSinceFire, ExpectedTimeSinceFire),
			FMath::IsNearlyEqual(CsvTimeSinceFire, ExpectedTimeSinceFire, Tolerance));
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 空历史：必须明确失败，而不是写出一个只有表头的文件
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilDumpEmptyHistoryTest, "Lyra.Recoil.Dump.EmptyHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilDumpEmptyHistoryTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilDumpTest;

	ULyraRecoilProfile* Profile = MakeTestProfile();

	FRecoilRuntimeState State;
	State.Reset(Profile);

	TestEqual(TEXT("Fresh state has no shot history"), State.ShotHistory.Num(), 0);

	FString DumpPath;
	const bool bDumped = ULyraRecoilDebug::DumpShotHistoryToCsv(Profile, State, DumpPath);

	TestTrue(TEXT("Dumping an empty history returns false"), !bDumped);
	TestTrue(FString::Printf(TEXT("No path is reported on failure (got '%s')"), *DumpPath), DumpPath.IsEmpty());

	// 打一发之后必须立刻变得可导出（证明上面的失败只是"没数据"，不是链路坏了）
	State.ApplyShot(Profile, 1.0f);

	TestTrue(TEXT("Dumping after one shot succeeds"), ULyraRecoilDebug::DumpShotHistoryToCsv(Profile, State, DumpPath));
	TestTrue(FString::Printf(TEXT("Path is reported on success (%s)"), *DumpPath), !DumpPath.IsEmpty());

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 列清单是冻结契约：任何增删列都必须改这条断言（并同步计划文档）
//////////////////////////////////////////////////////////////////////////

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilDumpHeaderSchemaTest, "Lyra.Recoil.Dump.HeaderSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilDumpHeaderSchemaTest::RunTest(const FString& Parameters)
{
	using namespace LyraRecoilDumpTest;

	ULyraRecoilProfile* Profile = MakeTestProfile();

	FRecoilRuntimeState State;
	State.Reset(Profile);
	State.ApplyShot(Profile, 1.0f);

	FString DumpPath;
	if (!TestTrue(TEXT("Dump succeeded"), ULyraRecoilDebug::DumpShotHistoryToCsv(Profile, State, DumpPath)))
	{
		return false;
	}

	TestEqual(TEXT("GetLastDumpPath reflects the last export"), ULyraRecoilDebug::GetLastDumpPath(), DumpPath);

	const TArray<FString> Lines = ReadCsvLines(DumpPath);
	if (!TestTrue(TEXT("CSV is not empty"), Lines.Num() > 0))
	{
		return false;
	}

	const TArray<FString> HeaderColumns = SplitColumns(Lines[0]);

	if (!TestEqual(TEXT("Header column count"), HeaderColumns.Num(), ExpectedColumnCount))
	{
		return false;
	}

	// 逐列比对，报出具体是第几列不符，而不是笼统地失败
	const TArray<FString> ExpectedColumns = SplitColumns(FString(ExpectedHeader));
	for (int32 ColumnIndex = 0; ColumnIndex < ExpectedColumns.Num(); ++ColumnIndex)
	{
		TestTrue(FString::Printf(TEXT("Header column %d is '%s' (actual '%s')"),
			ColumnIndex, *ExpectedColumns[ColumnIndex], *HeaderColumns[ColumnIndex]),
			HeaderColumns[ColumnIndex] == ExpectedColumns[ColumnIndex]);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
