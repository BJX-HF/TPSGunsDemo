// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <limits>

#include "Math/RandomStream.h"
#include "Misc/Guid.h"
#include "Private/Recoil/LyraRecoilPatternAdapter.h"
#include "UObject/Package.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"

/**
 * P1 / T01–T08：LyraRecoilPatternAdapter 的无 UI 转换与结构逻辑测试。
 *
 * 命名按开发计划 §13：Lyra.Recoil.Editor.Mapping.*
 * 全部用例只构造 transient Profile（T08 额外用一个 /Temp/ 测试包验证不标脏），
 * 不读写正式资产、不触碰 Golden。
 */
namespace LyraRecoilEditorMappingTest
{
	ULyraRecoilProfile* MakeProfile(const TArray<FRecoilPatternPoint>& Points, int32 PatternLength,
		float Horizontal, float Vertical)
	{
		ULyraRecoilProfile* Profile = NewObject<ULyraRecoilProfile>(GetTransientPackage());
		Profile->PatternPoints = Points;
		Profile->PatternLength = PatternLength;
		Profile->RecoilPerShot_Horizontal = Horizontal;
		Profile->RecoilPerShot_Vertical = Vertical;
		return Profile;
	}

	void SetCurveKeys(FRuntimeFloatCurve& Curve, const TArray<FVector2D>& Keys)
	{
		Curve.EditorCurveData.Reset();
		for (const FVector2D& Key : Keys)
		{
			Curve.EditorCurveData.AddKey(static_cast<float>(Key.X), static_cast<float>(Key.Y));
		}
	}

	void SetCurveConstant(FRuntimeFloatCurve& Curve, float Value)
	{
		TArray<FVector2D> Keys;
		Keys.Add(FVector2D(0.0, static_cast<double>(Value)));
		Keys.Add(FVector2D(1000.0, static_cast<double>(Value)));
		SetCurveKeys(Curve, Keys);
	}

	FString JoinErrors(const TArray<FString>& Errors)
	{
		return FString::Join(Errors, TEXT(" | "));
	}

	bool ErrorsContain(const TArray<FString>& Errors, const FString& Needle)
	{
		for (const FString& Error : Errors)
		{
			if (Error.Contains(Needle))
			{
				return true;
			}
		}
		return false;
	}

	void CheckNear(FAutomationTestBase& Test, const FString& What, double Actual, double Expected, double Tolerance = 1.0e-6)
	{
		Test.TestTrue(FString::Printf(TEXT("%s (actual %.9f, expected %.9f)"), *What, Actual, Expected),
			FMath::IsNearlyEqual(Actual, Expected, Tolerance));
	}

	void CheckPoint(FAutomationTestBase& Test, const FString& What, const FRecoilPatternPoint& Point,
		double ExpectedX, double ExpectedY, double Tolerance = 1.0e-6)
	{
		CheckNear(Test, FString::Printf(TEXT("%s.X"), *What), static_cast<double>(Point.X), ExpectedX, Tolerance);
		CheckNear(Test, FString::Printf(TEXT("%s.Y"), *What), static_cast<double>(Point.Y), ExpectedY, Tolerance);
	}

	/** 候选数据在 Profile 曲线下正向重建，必须与目标累计点一致（容差同适配器）。 */
	void VerifyCandidateMatchesTargets(FAutomationTestBase& Test, const ULyraRecoilProfile& Profile,
		const FLyraRecoilPatternData& Candidate, const TArray<FVector2D>& Targets, const FString& Label)
	{
		TArray<FVector2D> Rebuilt;
		TArray<FString> LocalErrors;
		const bool bBuilt = FLyraRecoilPatternAdapter::BuildCumulativeFor(Profile, Candidate, Rebuilt, LocalErrors);
		Test.TestTrue(FString::Printf(TEXT("%s: candidate rebuilds"), *Label), bBuilt);
		if (!bBuilt)
		{
			Test.AddError(FString::Printf(TEXT("%s: %s"), *Label, *JoinErrors(LocalErrors)));
			return;
		}

		Test.TestEqual(FString::Printf(TEXT("%s: rebuild count"), *Label), Rebuilt.Num(), Targets.Num());
		const int32 Count = FMath::Min(Rebuilt.Num(), Targets.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double ToleranceX = 1.0e-5 + 1.0e-6 * FMath::Abs(Targets[Index].X);
			const double ToleranceY = 1.0e-5 + 1.0e-6 * FMath::Abs(Targets[Index].Y);
			Test.TestTrue(FString::Printf(TEXT("%s: rebuild[%d].Yaw (%.9f vs %.9f)"), *Label, Index, Rebuilt[Index].X, Targets[Index].X),
				FMath::IsNearlyEqual(Rebuilt[Index].X, Targets[Index].X, ToleranceX));
			Test.TestTrue(FString::Printf(TEXT("%s: rebuild[%d].Pitch (%.9f vs %.9f)"), *Label, Index, Rebuilt[Index].Y, Targets[Index].Y),
				FMath::IsNearlyEqual(Rebuilt[Index].Y, Targets[Index].Y, ToleranceY));
		}
	}
}

using namespace LyraRecoilEditorMappingTest;

// ===========================================================================
// T01 正反转换
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilEditorMappingConversionTest, "Lyra.Recoil.Editor.Mapping.Conversion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorMappingConversionTest::RunTest(const FString& Parameters)
{
	// --- 开发计划 §5.2 的往返示例：H=0.2, V=0.4, c=[1, 0.5] ---
	TArray<FRecoilPatternPoint> Points;
	Points.Add(FRecoilPatternPoint(0.5f, 0.75f));
	Points.Add(FRecoilPatternPoint(-0.5f, 1.0f));

	ULyraRecoilProfile* Example = MakeProfile(Points, 2, 0.2f, 0.4f);
	TArray<FVector2D> CurveKeys;
	CurveKeys.Add(FVector2D(0.0, 1.0));
	CurveKeys.Add(FVector2D(1.0, 0.5));
	SetCurveKeys(Example->VerticalKickCurve, CurveKeys);

	TArray<FVector2D> Cumulative;
	TArray<FString> Errors;
	TestTrue(TEXT("BuildCumulative succeeds"), FLyraRecoilPatternAdapter::BuildCumulative(*Example, Cumulative, Errors));
	TestEqual(TEXT("fixed segment point count"), Cumulative.Num(), 2);
	if (Cumulative.Num() == 2)
	{
		CheckNear(*this, TEXT("P[0].Yaw"), Cumulative[0].X, 0.1);
		CheckNear(*this, TEXT("P[0].Pitch"), Cumulative[0].Y, 0.3);
		CheckNear(*this, TEXT("P[1].Yaw"), Cumulative[1].X, 0.0);
		CheckNear(*this, TEXT("P[1].Pitch"), Cumulative[1].Y, 0.5);
	}

	// T01：固定段逐发 Kick 必须与现有运行时 ComputeShotKick（倍率 1）相符。
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const FRecoilShotKick RuntimeKick = FRecoilRuntimeState::ComputeShotKick(*Example, Index, 1.0f, 1.0f, 0);
		const double PreviousYaw = (Index > 0) ? Cumulative[Index - 1].X : 0.0;
		const double PreviousPitch = (Index > 0) ? Cumulative[Index - 1].Y : 0.0;
		CheckNear(*this, FString::Printf(TEXT("shot %d Yaw vs runtime"), Index),
			Cumulative[Index].X - PreviousYaw, static_cast<double>(RuntimeKick.Horizontal), 1.0e-5);
		CheckNear(*this, FString::Printf(TEXT("shot %d Pitch vs runtime"), Index),
			Cumulative[Index].Y - PreviousPitch, static_cast<double>(RuntimeKick.Vertical), 1.0e-5);
	}

	// T01：反算示例 —— 第一点改到 (0.15, 0.35)，第二点保持 (0.0, 0.5)。
	TArray<FVector2D> Targets;
	Targets.Add(FVector2D(0.15, 0.35));
	Targets.Add(FVector2D(0.0, 0.5));

	FLyraRecoilPatternData Candidate;
	Errors.Reset();
	TestTrue(TEXT("MoveCumulative succeeds"), FLyraRecoilPatternAdapter::MoveCumulative(*Example, Targets, Candidate, Errors));
	TestEqual(TEXT("candidate point count"), Candidate.Points.Num(), 2);
	if (Candidate.Points.Num() == 2)
	{
		CheckPoint(*this, TEXT("Pattern'[0]"), Candidate.Points[0], 0.75, 0.875);
		CheckPoint(*this, TEXT("Pattern'[1]"), Candidate.Points[1], -0.75, 0.75);
	}
	VerifyCandidateMatchesTargets(*this, *Example, Candidate, Targets, TEXT("example round trip"));

	// T01：无操作不写回原始数据（零补丁，bitwise）。
	FLyraRecoilPatternData NoOp;
	Errors.Reset();
	TestTrue(TEXT("no-op move succeeds"), FLyraRecoilPatternAdapter::MoveCumulative(*Example, Cumulative, NoOp, Errors));
	TestTrue(TEXT("no-op produces an identical patch"), FLyraRecoilPatternAdapter::Equal(NoOp, FLyraRecoilPatternAdapter::Read(*Example)));
	TestEqual(TEXT("no-op keeps PatternLength"), NoOp.PatternLength, Example->PatternLength);

	// T01：全零数组。
	TArray<FRecoilPatternPoint> ZeroPoints;
	ZeroPoints.Add(FRecoilPatternPoint(0.0f, 0.0f));
	ZeroPoints.Add(FRecoilPatternPoint(0.0f, 0.0f));
	ULyraRecoilProfile* ZeroProfile = MakeProfile(ZeroPoints, 2, 0.2f, 0.4f);
	SetCurveConstant(ZeroProfile->VerticalKickCurve, 1.0f);

	TArray<FVector2D> ZeroCumulative;
	TArray<FString> ZeroErrors;
	TestTrue(TEXT("zero pattern builds"), FLyraRecoilPatternAdapter::BuildCumulative(*ZeroProfile, ZeroCumulative, ZeroErrors));
	FLyraRecoilPatternData ZeroMove;
	ZeroErrors.Reset();
	TestTrue(TEXT("zero pattern Y-only move succeeds"),
		FLyraRecoilPatternAdapter::MoveCumulative(*ZeroProfile, ZeroCumulative, ZeroMove, ZeroErrors));
	TestTrue(TEXT("zero pattern no-op is identical"), FLyraRecoilPatternAdapter::Equal(ZeroMove, FLyraRecoilPatternAdapter::Read(*ZeroProfile)));

	// T01：负水平（X = -0.5）。
	TArray<FRecoilPatternPoint> NegativePoints;
	NegativePoints.Add(FRecoilPatternPoint(-0.5f, 0.5f));
	ULyraRecoilProfile* NegativeProfile = MakeProfile(NegativePoints, 1, 0.2f, 0.4f);
	SetCurveConstant(NegativeProfile->VerticalKickCurve, 1.0f);

	TArray<FVector2D> NegativeCumulative;
	TArray<FString> NegativeErrors;
	TestTrue(TEXT("negative X builds"), FLyraRecoilPatternAdapter::BuildCumulative(*NegativeProfile, NegativeCumulative, NegativeErrors));
	if (NegativeCumulative.Num() == 1)
	{
		CheckNear(*this, TEXT("negative X cumulative Yaw"), NegativeCumulative[0].X, -0.1);
	}

	TArray<FVector2D> NegativeTargets;
	NegativeTargets.Add(FVector2D(-0.2, 0.2));
	FLyraRecoilPatternData NegativeCandidate;
	NegativeErrors.Reset();
	TestTrue(TEXT("negative X move succeeds"),
		FLyraRecoilPatternAdapter::MoveCumulative(*NegativeProfile, NegativeTargets, NegativeCandidate, NegativeErrors));
	{
		// ΔYaw = -0.2 -> X = -1.0 边界。
		CheckPoint(*this, TEXT("negative X candidate"), NegativeCandidate.Points[0], -1.0, 0.5);
	}

	// T01：可复现随机合法数组（X ∈ [-0.5, 0.5]，Y ∈ [0.2, 0.8]，保证相邻发有余量）。
	FRandomStream Stream(20261008);
	TArray<FRecoilPatternPoint> RandomPoints;
	const int32 RandomCount = 12;
	for (int32 Index = 0; Index < RandomCount; ++Index)
	{
		RandomPoints.Add(FRecoilPatternPoint(
			Stream.FRandRange(-0.5f, 0.5f),
			Stream.FRandRange(0.2f, 0.8f)));
	}

	ULyraRecoilProfile* RandomProfile = MakeProfile(RandomPoints, RandomCount, 0.18f, 0.35f);
	SetCurveConstant(RandomProfile->VerticalKickCurve, 1.0f);

	TArray<FVector2D> RandomCumulative;
	TArray<FString> RandomErrors;
	TestTrue(TEXT("random pattern builds"), FLyraRecoilPatternAdapter::BuildCumulative(*RandomProfile, RandomCumulative, RandomErrors));

	FLyraRecoilPatternData RandomNoOp;
	RandomErrors.Reset();
	TestTrue(TEXT("random no-op move succeeds"),
		FLyraRecoilPatternAdapter::MoveCumulative(*RandomProfile, RandomCumulative, RandomNoOp, RandomErrors));
	TestTrue(TEXT("random no-op is identical"), FLyraRecoilPatternAdapter::Equal(RandomNoOp, FLyraRecoilPatternAdapter::Read(*RandomProfile)));

	TArray<FVector2D> MovedTargets = RandomCumulative;
	MovedTargets[5] += FVector2D(0.01, 0.005);
	FLyraRecoilPatternData MovedCandidate;
	RandomErrors.Reset();
	TestTrue(TEXT("random single-point move succeeds"),
		FLyraRecoilPatternAdapter::MoveCumulative(*RandomProfile, MovedTargets, MovedCandidate, RandomErrors));
	VerifyCandidateMatchesTargets(*this, *RandomProfile, MovedCandidate, MovedTargets, TEXT("random moved"));

	return true;
}

// ===========================================================================
// T02 分母与异常
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilEditorMappingDenominatorsTest, "Lyra.Recoil.Editor.Mapping.Denominators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorMappingDenominatorsTest::RunTest(const FString& Parameters)
{
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	const float Infinity = std::numeric_limits<float>::infinity();
	TestTrue(TEXT("NaN helper produces NaN"), FMath::IsNaN(NaN));
	TestTrue(TEXT("Infinity helper produces Inf"), !FMath::IsFinite(Infinity));

	// --- H=0：水平轴不可逆，隐藏值必须保留 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.7f, 0.5f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 1, 0.0f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> Cumulative;
		TArray<FString> Errors;
		TestTrue(TEXT("H=0 builds"), FLyraRecoilPatternAdapter::BuildCumulative(*Profile, Cumulative, Errors));
		if (Cumulative.Num() == 1)
		{
			CheckNear(*this, TEXT("H=0 cumulative Yaw"), Cumulative[0].X, 0.0, 1.0e-12);
			CheckNear(*this, TEXT("H=0 cumulative Pitch"), Cumulative[0].Y, 0.2);
		}

		// 只改 Y：X 的隐藏值 0.7 必须原样保留。
		TArray<FVector2D> YOnlyTargets;
		YOnlyTargets.Add(FVector2D(0.0, 0.4));
		FLyraRecoilPatternData YOnlyCandidate;
		Errors.Reset();
		TestTrue(TEXT("H=0 Y-only move succeeds"),
			FLyraRecoilPatternAdapter::MoveCumulative(*Profile, YOnlyTargets, YOnlyCandidate, Errors));
		TestTrue(TEXT("H=0 preserves the hidden X bitwise"), YOnlyCandidate.Points[0].X == 0.7f);
		CheckNear(*this, TEXT("H=0 Y-only candidate Y"), YOnlyCandidate.Points[0].Y, 1.0);

		// 非零水平目标：整体拒绝，原数据不变。
		TArray<FVector2D> XTargets;
		XTargets.Add(FVector2D(0.05, 0.2));
		FLyraRecoilPatternData Rejected;
		Errors.Reset();
		TestTrue(TEXT("H=0 non-zero X target is rejected"),
			!FLyraRecoilPatternAdapter::MoveCumulative(*Profile, XTargets, Rejected, Errors));
		TestTrue(TEXT("H=0 rejection keeps the source data"),
			FLyraRecoilPatternAdapter::Equal(Rejected, FLyraRecoilPatternAdapter::Read(*Profile)));
		TestTrue(TEXT("H=0 rejection explains the non-invertible denominator"), ErrorsContain(Errors, TEXT("not invertible")));
	}

	// --- V=0：垂直轴不可逆 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.5f, 0.9f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 1, 0.2f, 0.0f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> XOnlyTargets;
		XOnlyTargets.Add(FVector2D(0.2, 0.0));
		FLyraRecoilPatternData XOnlyCandidate;
		TArray<FString> Errors;
		TestTrue(TEXT("V=0 X-only move succeeds"),
			FLyraRecoilPatternAdapter::MoveCumulative(*Profile, XOnlyTargets, XOnlyCandidate, Errors));
		TestTrue(TEXT("V=0 preserves the hidden Y bitwise"), XOnlyCandidate.Points[0].Y == 0.9f);

		TArray<FVector2D> YTargets;
		YTargets.Add(FVector2D(0.1, 0.3));
		FLyraRecoilPatternData Rejected;
		Errors.Reset();
		TestTrue(TEXT("V=0 non-zero Y target is rejected"),
			!FLyraRecoilPatternAdapter::MoveCumulative(*Profile, YTargets, Rejected, Errors));
		TestTrue(TEXT("V=0 rejection keeps the source data"),
			FLyraRecoilPatternAdapter::Equal(Rejected, FLyraRecoilPatternAdapter::Read(*Profile)));
	}

	// --- c[i] = 0（曲线在整数发序号上取零）---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.5f, 0.9f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 1, 0.2f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 0.0f);

		TArray<FVector2D> XOnlyTargets;
		XOnlyTargets.Add(FVector2D(0.2, 0.0));
		FLyraRecoilPatternData XOnlyCandidate;
		TArray<FString> Errors;
		TestTrue(TEXT("c=0 X-only move succeeds"),
			FLyraRecoilPatternAdapter::MoveCumulative(*Profile, XOnlyTargets, XOnlyCandidate, Errors));
		TestTrue(TEXT("c=0 preserves the hidden Y bitwise"), XOnlyCandidate.Points[0].Y == 0.9f);

		TArray<FVector2D> YTargets;
		YTargets.Add(FVector2D(0.1, 0.2));
		FLyraRecoilPatternData Rejected;
		Errors.Reset();
		TestTrue(TEXT("c=0 non-zero Y target is rejected"),
			!FLyraRecoilPatternAdapter::MoveCumulative(*Profile, YTargets, Rejected, Errors));
	}

	// --- 负曲线：向下 Kick 可表达，向上不可表达 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.0f, 0.5f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 1, 0.2f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, -1.0f);

		TArray<FVector2D> Cumulative;
		TArray<FString> Errors;
		TestTrue(TEXT("negative curve builds"), FLyraRecoilPatternAdapter::BuildCumulative(*Profile, Cumulative, Errors));
		if (Cumulative.Num() == 1)
		{
			CheckNear(*this, TEXT("negative curve cumulative Pitch"), Cumulative[0].Y, -0.2);
		}

		TArray<FVector2D> DownTargets;
		DownTargets.Add(FVector2D(0.0, -0.4));
		FLyraRecoilPatternData DownCandidate;
		Errors.Reset();
		TestTrue(TEXT("negative curve downward target succeeds"),
			FLyraRecoilPatternAdapter::MoveCumulative(*Profile, DownTargets, DownCandidate, Errors));
		CheckPoint(*this, TEXT("negative curve downward candidate"), DownCandidate.Points[0], 0.0, 1.0);

		TArray<FVector2D> UpTargets;
		UpTargets.Add(FVector2D(0.0, 0.2));
		FLyraRecoilPatternData Rejected;
		Errors.Reset();
		TestTrue(TEXT("negative curve upward target is rejected"),
			!FLyraRecoilPatternAdapter::MoveCumulative(*Profile, UpTargets, Rejected, Errors));
		TestTrue(TEXT("negative curve rejection keeps the source data"),
			FLyraRecoilPatternAdapter::Equal(Rejected, FLyraRecoilPatternAdapter::Read(*Profile)));

		FLyraRecoilPatternData NoOp;
		Errors.Reset();
		TestTrue(TEXT("negative curve no-op succeeds"),
			FLyraRecoilPatternAdapter::MoveCumulative(*Profile, Cumulative, NoOp, Errors));
		TestTrue(TEXT("negative curve no-op is identical"), FLyraRecoilPatternAdapter::Equal(NoOp, FLyraRecoilPatternAdapter::Read(*Profile)));
	}

	// --- 极小分母：锁定阈值内保留隐藏值，阈值内精确装载 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.6f, 0.5f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 1, 1.0e-12f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> Cumulative;
		TArray<FString> Errors;
		TestTrue(TEXT("tiny H builds"), FLyraRecoilPatternAdapter::BuildCumulative(*Profile, Cumulative, Errors));
		if (Cumulative.Num() == 1)
		{
			CheckNear(*this, TEXT("tiny H shows the exact value"), Cumulative[0].X, 6.0e-13, 1.0e-15);
		}

		TArray<FVector2D> YOnlyTargets;
		YOnlyTargets.Add(FVector2D(0.0, 0.3));
		FLyraRecoilPatternData YOnlyCandidate;
		Errors.Reset();
		TestTrue(TEXT("tiny H Y-only move succeeds"),
			FLyraRecoilPatternAdapter::MoveCumulative(*Profile, YOnlyTargets, YOnlyCandidate, Errors));
		TestTrue(TEXT("tiny H preserves the hidden X bitwise"), YOnlyCandidate.Points[0].X == 0.6f);

		TArray<FVector2D> XTargets;
		XTargets.Add(FVector2D(1.0e-6, 0.2));
		FLyraRecoilPatternData Rejected;
		Errors.Reset();
		TestTrue(TEXT("tiny H with a non-zero X target is rejected"),
			!FLyraRecoilPatternAdapter::MoveCumulative(*Profile, XTargets, Rejected, Errors));
	}

	// --- 极小但可逆的分母：不得用 epsilon 代分母，超界直接拒绝 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(1.0f, 0.5f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 1, 1.0e-7f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> Targets;
		Targets.Add(FVector2D(2.0e-7, 0.2));
		FLyraRecoilPatternData Rejected;
		TArray<FString> Errors;
		TestTrue(TEXT("out-of-range inverse is rejected, not clamped"),
			!FLyraRecoilPatternAdapter::MoveCumulative(*Profile, Targets, Rejected, Errors));
		TestTrue(TEXT("out-of-range inverse explains the allowed range"), ErrorsContain(Errors, TEXT("outside the allowed range")));
	}

	// --- NaN / Inf：构建、编辑、提交全部拒绝，且不半写入 ---
	{
		TArray<FRecoilPatternPoint> NaNSource;
		NaNSource.Add(FRecoilPatternPoint(NaN, 0.5f));
		ULyraRecoilProfile* NaNProfile = MakeProfile(NaNSource, 1, 0.2f, 0.4f);
		SetCurveConstant(NaNProfile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> Cumulative;
		TArray<FString> Errors;
		TestTrue(TEXT("NaN source fails BuildCumulative"), !FLyraRecoilPatternAdapter::BuildCumulative(*NaNProfile, Cumulative, Errors));
		TestEqual(TEXT("failed build leaves no points"), Cumulative.Num(), 0);

		TArray<FVector2D> Targets;
		Targets.Add(FVector2D(0.1, 0.2));
		FLyraRecoilPatternData Rejected;
		Errors.Reset();
		TestTrue(TEXT("NaN source fails MoveCumulative"), !FLyraRecoilPatternAdapter::MoveCumulative(*NaNProfile, Targets, Rejected, Errors));
		TestEqual(TEXT("NaN rejection keeps the point count"), Rejected.Points.Num(), 1);
		TestTrue(TEXT("NaN rejection keeps the original NaN (no silent zeroing)"), FMath::IsNaN(Rejected.Points[0].X));

		TArray<FRecoilPatternPoint> InfSource;
		InfSource.Add(FRecoilPatternPoint(0.5f, Infinity));
		ULyraRecoilProfile* InfProfile = MakeProfile(InfSource, 1, 0.2f, 0.4f);
		SetCurveConstant(InfProfile->VerticalKickCurve, 1.0f);

		Errors.Reset();
		TestTrue(TEXT("Inf source fails BuildCumulative"), !FLyraRecoilPatternAdapter::BuildCumulative(*InfProfile, Cumulative, Errors));

		ULyraRecoilProfile* InfStrengthProfile = MakeProfile(NaNSource, 1, Infinity, 0.4f);
		Errors.Reset();
		TestTrue(TEXT("non-finite strength fails BuildCumulative"),
			!FLyraRecoilPatternAdapter::BuildCumulative(*InfStrengthProfile, Cumulative, Errors));
	}

	// --- 负基础强度：既有非法数据不得反算 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 1, -0.2f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> Targets;
		Targets.Add(FVector2D(0.1, 0.2));
		FLyraRecoilPatternData Rejected;
		TArray<FString> Errors;
		TestTrue(TEXT("negative H cannot invert"),
			!FLyraRecoilPatternAdapter::MoveCumulative(*Profile, Targets, Rejected, Errors));
		TestTrue(TEXT("negative H is reported by Validate"), !FLyraRecoilPatternAdapter::Validate(*Profile, Errors));
	}

	return true;
}

// ===========================================================================
// T03 相邻影响
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilEditorMappingAdjacentTest, "Lyra.Recoil.Editor.Mapping.Adjacent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorMappingAdjacentTest::RunTest(const FString& Parameters)
{
	// 固定段 4 发（Δ 全部相同）+ 2 个尾段点。
	TArray<FRecoilPatternPoint> Points;
	Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
	Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
	Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
	Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
	Points.Add(FRecoilPatternPoint(0.7f, 0.6f));
	Points.Add(FRecoilPatternPoint(0.7f, 0.6f));

	ULyraRecoilProfile* Profile = MakeProfile(Points, 4, 0.2f, 0.4f);
	SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

	TArray<FVector2D> Cumulative;
	TArray<FString> Errors;
	TestTrue(TEXT("adjacent source builds"), FLyraRecoilPatternAdapter::BuildCumulative(*Profile, Cumulative, Errors));
	TestEqual(TEXT("only the fixed segment is built"), Cumulative.Num(), 4);

	// --- 移动中间点 P[1]：Δ[1] += d，Δ[2] -= d，Δ[0]/Δ[3] 不变 ---
	if (Cumulative.Num() == 4)
	{
		TArray<FVector2D> Targets = Cumulative;
		Targets[1] = FVector2D(0.25, 0.5);

		FLyraRecoilPatternData Candidate;
		Errors.Reset();
		TestTrue(TEXT("middle move succeeds"), FLyraRecoilPatternAdapter::MoveCumulative(*Profile, Targets, Candidate, Errors));
		TestEqual(TEXT("tail is preserved on a fixed-segment move"), Candidate.Points.Num(), 6);
		if (Candidate.Points.Num() == 6)
		{
			CheckPoint(*this, TEXT("P[0] unchanged"), Candidate.Points[0], 0.5f, 0.5f, 0.0);
			CheckPoint(*this, TEXT("P[1] recomputed"), Candidate.Points[1], 0.75, 0.75);
			CheckPoint(*this, TEXT("P[2] recomputed by the adjacent rule"), Candidate.Points[2], 0.25, 0.25);
			CheckPoint(*this, TEXT("P[3] unchanged"), Candidate.Points[3], 0.5f, 0.5f, 0.0);
			CheckPoint(*this, TEXT("tail[4] untouched"), Candidate.Points[4], 0.7f, 0.6f, 0.0);
			CheckPoint(*this, TEXT("tail[5] untouched"), Candidate.Points[5], 0.7f, 0.6f, 0.0);
		}
		VerifyCandidateMatchesTargets(*this, *Profile, Candidate, Targets, TEXT("middle move"));

		// --- 非连续多选：P[0] 与 P[2] 同时平移 ---
		TArray<FVector2D> MultiTargets = Cumulative;
		MultiTargets[0] = FVector2D(0.15, 0.3);
		MultiTargets[2] = FVector2D(0.35, 0.7);

		FLyraRecoilPatternData MultiCandidate;
		Errors.Reset();
		TestTrue(TEXT("non-contiguous multi move succeeds"),
			FLyraRecoilPatternAdapter::MoveCumulative(*Profile, MultiTargets, MultiCandidate, Errors));
		if (MultiCandidate.Points.Num() == 6)
		{
			CheckPoint(*this, TEXT("multi P[0]"), MultiCandidate.Points[0], 0.75, 0.75);
			CheckPoint(*this, TEXT("multi P[1] boundary"), MultiCandidate.Points[1], 0.25, 0.25);
			CheckPoint(*this, TEXT("multi P[2]"), MultiCandidate.Points[2], 0.75, 0.75);
			CheckPoint(*this, TEXT("multi P[3] boundary"), MultiCandidate.Points[3], 0.25, 0.25);
		}
		VerifyCandidateMatchesTargets(*this, *Profile, MultiCandidate, MultiTargets, TEXT("multi move"));

		// --- 末固定点：只改 Δ[L-1]，尾段不受影响 ---
		TArray<FVector2D> LastTargets = Cumulative;
		LastTargets[3] = FVector2D(0.5, 1.0);

		FLyraRecoilPatternData LastCandidate;
		Errors.Reset();
		TestTrue(TEXT("last fixed point move succeeds"),
			FLyraRecoilPatternAdapter::MoveCumulative(*Profile, LastTargets, LastCandidate, Errors));
		if (LastCandidate.Points.Num() == 6)
		{
			CheckPoint(*this, TEXT("last fixed P[3]"), LastCandidate.Points[3], 1.0, 1.0);
			CheckPoint(*this, TEXT("last fixed keeps tail[4]"), LastCandidate.Points[4], 0.7f, 0.6f, 0.0);
			CheckPoint(*this, TEXT("last fixed keeps tail[5]"), LastCandidate.Points[5], 0.7f, 0.6f, 0.0);
		}
	}

	// --- 受影响的下一发超限：整组回退，不出现半写入 ---
	{
		TArray<FRecoilPatternPoint> EdgePoints;
		EdgePoints.Add(FRecoilPatternPoint(0.5f, 0.0f));
		EdgePoints.Add(FRecoilPatternPoint(0.5f, 0.5f));
		ULyraRecoilProfile* EdgeProfile = MakeProfile(EdgePoints, 2, 0.2f, 0.4f);
		SetCurveConstant(EdgeProfile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> EdgeCumulative;
		TArray<FString> EdgeErrors;
		TestTrue(TEXT("edge source builds"), FLyraRecoilPatternAdapter::BuildCumulative(*EdgeProfile, EdgeCumulative, EdgeErrors));

		TArray<FVector2D> EdgeTargets = EdgeCumulative;
		EdgeTargets[0] = FVector2D(0.1, 0.3); // Δ[1].Pitch = 0.2 - 0.3 = -0.1 -> Y 超下界

		FLyraRecoilPatternData Rejected;
		EdgeErrors.Reset();
		TestTrue(TEXT("adjacent overflow is rejected"),
			!FLyraRecoilPatternAdapter::MoveCumulative(*EdgeProfile, EdgeTargets, Rejected, EdgeErrors));
		TestTrue(TEXT("adjacent overflow reports the affected shot"), ErrorsContain(EdgeErrors, TEXT("Shot 1")));
		TestTrue(TEXT("adjacent overflow keeps the source data"),
			FLyraRecoilPatternAdapter::Equal(Rejected, FLyraRecoilPatternAdapter::Read(*EdgeProfile)));
	}

	// --- 边界舍入：容差内归并到 [-1,1] / [0,1]，并记录误差；超容差拒绝 ---
	{
		TArray<FRecoilPatternPoint> RoundPoints;
		RoundPoints.Add(FRecoilPatternPoint(0.5f, 1.0f));
		ULyraRecoilProfile* RoundProfile = MakeProfile(RoundPoints, 1, 0.2f, 1.0f);
		SetCurveConstant(RoundProfile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> RoundTargets;
		RoundTargets.Add(FVector2D(0.1, 1.0 + 5.0e-7));

		FLyraRecoilPatternData Rounded;
		FLyraRecoilConversionReport Report;
		TArray<FString> RoundErrors;
		TestTrue(TEXT("in-tolerance boundary rounding succeeds"),
			FLyraRecoilPatternAdapter::MoveCumulative(*RoundProfile, RoundTargets, Rounded, Report, RoundErrors));
		TestTrue(TEXT("boundary rounding reports the clamped index"), Report.BoundaryClampedIndices.Contains(0));
		TestTrue(TEXT("boundary rounding reports the clamp error"), Report.MaxNormalizedClampError > 0.0);
		if (Rounded.Points.Num() == 1)
		{
			TestTrue(TEXT("boundary rounding clamps to exactly 1.0"), Rounded.Points[0].Y == 1.0f);
		}

		TArray<FVector2D> BeyondTargets;
		BeyondTargets.Add(FVector2D(0.1, 1.0 + 5.0e-6));
		FLyraRecoilPatternData Rejected;
		RoundErrors.Reset();
		TestTrue(TEXT("beyond-tolerance boundary is rejected"),
			!FLyraRecoilPatternAdapter::MoveCumulative(*RoundProfile, BeyondTargets, Rejected, RoundErrors));
	}

	return true;
}

// ===========================================================================
// T04 固定 / 随机尾段与空数组
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilEditorMappingTailTest, "Lyra.Recoil.Editor.Mapping.Tail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorMappingTailTest::RunTest(const FString& Parameters)
{
	// --- N=0：空数组不自动补点 ---
	{
		ULyraRecoilProfile* Empty = MakeProfile(TArray<FRecoilPatternPoint>(), 0, 0.2f, 0.4f);
		SetCurveConstant(Empty->VerticalKickCurve, 1.0f);

		const FLyraRecoilPatternData Read = FLyraRecoilPatternAdapter::Read(*Empty);
		TestEqual(TEXT("empty array stays empty (no auto insert)"), Read.Points.Num(), 0);
		TestEqual(TEXT("empty array keeps PatternLength 0"), Read.PatternLength, 0);

		TArray<FVector2D> Cumulative;
		TArray<FString> Errors;
		TestTrue(TEXT("empty array builds an empty fixed segment"), FLyraRecoilPatternAdapter::BuildCumulative(*Empty, Cumulative, Errors));
		TestEqual(TEXT("empty fixed segment has no points"), Cumulative.Num(), 0);

		FLyraRecoilPatternData NoOp;
		Errors.Reset();
		TestTrue(TEXT("empty array accepts an empty move"),
			FLyraRecoilPatternAdapter::MoveCumulative(*Empty, TArray<FVector2D>(), NoOp, Errors));
		TestTrue(TEXT("empty array move is a zero patch"), FLyraRecoilPatternAdapter::Equal(NoOp, Read));

		// 运行时退化点：GetPatternPoint 对空数组返回纯垂直 (0,1)，适配器不得改写。
		const FRecoilPatternPoint Degenerate = Empty->GetPatternPoint(3);
		CheckPoint(*this, TEXT("degenerate GetPatternPoint"), Degenerate, 0.0, 1.0, 0.0);

		// 显式插入才是新增路径：Index == L == 0 默认归入尾段。
		TArray<FRecoilPatternPoint> DefaultPoint;
		DefaultPoint.Add(FRecoilPatternPoint(0.0f, 0.0f));

		FLyraRecoilPatternData TailInsert;
		Errors.Reset();
		TestTrue(TEXT("insert at the boundary defaults to tail"),
			FLyraRecoilPatternAdapter::Insert(Read, 0, DefaultPoint, false, TailInsert, Errors));
		TestEqual(TEXT("boundary insert into the tail keeps L"), TailInsert.PatternLength, 0);
		TestEqual(TEXT("boundary insert grows N"), TailInsert.Points.Num(), 1);

		FLyraRecoilPatternData FixedInsert;
		Errors.Reset();
		TestTrue(TEXT("explicit fixed insert at the boundary"),
			FLyraRecoilPatternAdapter::Insert(Read, 0, DefaultPoint, true, FixedInsert, Errors));
		TestEqual(TEXT("explicit fixed insert grows L"), FixedInsert.PatternLength, 1);
	}

	// --- L=0 且 N>0：无固定点，尾段保留 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		Points.Add(FRecoilPatternPoint(0.9f, 0.7f));
		Points.Add(FRecoilPatternPoint(-0.9f, 0.2f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 0, 0.2f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> Cumulative;
		TArray<FString> Errors;
		TestTrue(TEXT("L=0 builds an empty fixed segment"), FLyraRecoilPatternAdapter::BuildCumulative(*Profile, Cumulative, Errors));
		TestEqual(TEXT("L=0 has no fixed points"), Cumulative.Num(), 0);

		FLyraRecoilPatternData NoOp;
		Errors.Reset();
		TestTrue(TEXT("L=0 accepts an empty move"),
			FLyraRecoilPatternAdapter::MoveCumulative(*Profile, TArray<FVector2D>(), NoOp, Errors));
		TestTrue(TEXT("L=0 move is a zero patch"), FLyraRecoilPatternAdapter::Equal(NoOp, FLyraRecoilPatternAdapter::Read(*Profile)));
	}

	// --- L<N：尾段 X/Y 原样保留 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		Points.Add(FRecoilPatternPoint(0.9f, 0.7f));
		Points.Add(FRecoilPatternPoint(-0.9f, 0.2f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 2, 0.2f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> Cumulative;
		TArray<FString> Errors;
		TestTrue(TEXT("L<N builds only the fixed segment"), FLyraRecoilPatternAdapter::BuildCumulative(*Profile, Cumulative, Errors));
		TestEqual(TEXT("L<N fixed point count"), Cumulative.Num(), 2);

		TArray<FVector2D> Targets;
		Targets.Add(FVector2D(0.1, 0.2));
		Targets.Add(FVector2D(0.3, 0.5));

		FLyraRecoilPatternData Candidate;
		Errors.Reset();
		TestTrue(TEXT("L<N move succeeds"), FLyraRecoilPatternAdapter::MoveCumulative(*Profile, Targets, Candidate, Errors));
		TestEqual(TEXT("L<N keeps N"), Candidate.Points.Num(), 4);
		if (Candidate.Points.Num() == 4)
		{
			CheckPoint(*this, TEXT("L<N tail[2] preserve"), Candidate.Points[2], 0.9f, 0.7f, 0.0);
			CheckPoint(*this, TEXT("L<N tail[3] preserve"), Candidate.Points[3], -0.9f, 0.2f, 0.0);
			CheckPoint(*this, TEXT("L<N fixed[1]"), Candidate.Points[1], 1.0, 0.75);
		}
	}

	// --- L=N：全部点都在固定段 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 2, 0.2f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> Cumulative;
		TArray<FString> Errors;
		TestTrue(TEXT("L=N builds all points"), FLyraRecoilPatternAdapter::BuildCumulative(*Profile, Cumulative, Errors));
		TestEqual(TEXT("L=N point count"), Cumulative.Num(), 2);

		const FLyraRecoilPatternData Data = FLyraRecoilPatternAdapter::Read(*Profile);
		TestEqual(TEXT("L=N read point count"), Data.Points.Num(), 2);
		TestEqual(TEXT("L=N read keeps PatternLength"), Data.PatternLength, 2);
		CheckPoint(*this, TEXT("L=N read[0]"), Data.Points[0], 0.5f, 0.5f, 0.0);
	}

	// --- i = N-1 / N / N+1：GetPatternPoint 沿用最后一点，适配器不做额外假设 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.1f, 0.2f));
		Points.Add(FRecoilPatternPoint(0.3f, 0.4f));
		Points.Add(FRecoilPatternPoint(0.5f, 0.6f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 3, 0.2f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		CheckPoint(*this, TEXT("GetPatternPoint(N-1)"), Profile->GetPatternPoint(2), 0.5f, 0.6f, 0.0);
		CheckPoint(*this, TEXT("GetPatternPoint(N)"), Profile->GetPatternPoint(3), 0.5f, 0.6f, 0.0);
		CheckPoint(*this, TEXT("GetPatternPoint(N+1)"), Profile->GetPatternPoint(4), 0.5f, 0.6f, 0.0);

		// N 之后曲线仍按发序号求值（不假定最终 Kick 恒定）。
		TArray<FVector2D> CurveKeys;
		CurveKeys.Add(FVector2D(0.0, 1.0));
		CurveKeys.Add(FVector2D(5.0, 2.0));
		SetCurveKeys(Profile->VerticalKickCurve, CurveKeys);
		CheckNear(*this, TEXT("curve keeps evaluating beyond N"), Profile->GetVerticalKickCurveScale(5), 2.0, 1.0e-5);

		// 适配器只构建前 L 个固定点，不使用尾段曲线索引。
		TArray<FVector2D> Cumulative;
		TArray<FString> Errors;
		TestTrue(TEXT("curve beyond N does not change the fixed segment"), FLyraRecoilPatternAdapter::BuildCumulative(*Profile, Cumulative, Errors));
		TestEqual(TEXT("fixed segment count with tail curve data"), Cumulative.Num(), 3);
	}

	return true;
}

// ===========================================================================
// T05 结构命令
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilEditorMappingStructuralTest, "Lyra.Recoil.Editor.Mapping.Structural",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorMappingStructuralTest::RunTest(const FString& Parameters)
{
	// --- 插入边界规则 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.1f, 0.1f));
		Points.Add(FRecoilPatternPoint(0.2f, 0.2f));
		Points.Add(FRecoilPatternPoint(0.3f, 0.3f));
		Points.Add(FRecoilPatternPoint(0.4f, 0.4f));

		const FLyraRecoilPatternData Source = FLyraRecoilPatternAdapter::Read(*MakeProfile(Points, 2, 0.2f, 0.4f));

		TArray<FRecoilPatternPoint> Inserted;
		Inserted.Add(FRecoilPatternPoint(0.5f, 0.5f));

		TArray<FString> Errors;
		FLyraRecoilPatternData Out;

		TestTrue(TEXT("insert before the boundary"), FLyraRecoilPatternAdapter::Insert(Source, 1, Inserted, false, Out, Errors));
		TestEqual(TEXT("insert before L grows N"), Out.Points.Num(), 5);
		TestEqual(TEXT("insert before L grows L"), Out.PatternLength, 3);
		if (Out.Points.Num() == 5)
		{
			CheckPoint(*this, TEXT("insert before L keeps order"), Out.Points[1], 0.5f, 0.5f, 0.0);
			CheckPoint(*this, TEXT("insert before L shifts the rest"), Out.Points[2], 0.2f, 0.2f, 0.0);
		}

		Errors.Reset();
		TestTrue(TEXT("insert at the boundary defaults to tail"), FLyraRecoilPatternAdapter::Insert(Source, 2, Inserted, false, Out, Errors));
		TestEqual(TEXT("boundary tail insert keeps L"), Out.PatternLength, 2);
		TestEqual(TEXT("boundary tail insert grows N"), Out.Points.Num(), 5);

		Errors.Reset();
		TestTrue(TEXT("insert at the boundary with the fixed option"), FLyraRecoilPatternAdapter::Insert(Source, 2, Inserted, true, Out, Errors));
		TestEqual(TEXT("boundary fixed insert grows L"), Out.PatternLength, 3);

		Errors.Reset();
		TestTrue(TEXT("insert after the boundary"), FLyraRecoilPatternAdapter::Insert(Source, 3, Inserted, true, Out, Errors));
		TestEqual(TEXT("insert after the boundary keeps L"), Out.PatternLength, 2);
		TestEqual(TEXT("insert after the boundary grows N"), Out.Points.Num(), 5);

		Errors.Reset();
		TestTrue(TEXT("append at the end"), FLyraRecoilPatternAdapter::Insert(Source, 4, Inserted, true, Out, Errors));
		TestEqual(TEXT("append at the end keeps L"), Out.PatternLength, 2);

		Errors.Reset();
		TestTrue(TEXT("insert out of range is rejected"), !FLyraRecoilPatternAdapter::Insert(Source, 5, Inserted, false, Out, Errors));
		Errors.Reset();
		TestTrue(TEXT("insert with a negative index is rejected"), !FLyraRecoilPatternAdapter::Insert(Source, -1, Inserted, false, Out, Errors));

		TArray<FRecoilPatternPoint> BadPoint;
		BadPoint.Add(FRecoilPatternPoint(1.5f, 0.5f));
		Errors.Reset();
		TestTrue(TEXT("insert with an out-of-range value is rejected"), !FLyraRecoilPatternAdapter::Insert(Source, 0, BadPoint, false, Out, Errors));

		Errors.Reset();
		TestTrue(TEXT("empty insert is a no-op"), FLyraRecoilPatternAdapter::Insert(Source, 1, TArray<FRecoilPatternPoint>(), false, Out, Errors));
		TestTrue(TEXT("empty insert is a zero patch"), FLyraRecoilPatternAdapter::Equal(Out, Source));
	}

	// --- 删除：跨边界、删光 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.1f, 0.1f));
		Points.Add(FRecoilPatternPoint(0.2f, 0.2f));
		Points.Add(FRecoilPatternPoint(0.3f, 0.3f));
		Points.Add(FRecoilPatternPoint(0.4f, 0.4f));
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));

		const FLyraRecoilPatternData Source = FLyraRecoilPatternAdapter::Read(*MakeProfile(Points, 3, 0.2f, 0.4f));

		TArray<int32> CrossBoundary;
		CrossBoundary.Add(1);
		CrossBoundary.Add(4);

		TArray<FString> Errors;
		FLyraRecoilPatternData Out;
		TestTrue(TEXT("cross-boundary delete succeeds"), FLyraRecoilPatternAdapter::Delete(Source, CrossBoundary, Out, Errors));
		TestEqual(TEXT("cross-boundary delete shrinks N"), Out.Points.Num(), 3);
		TestEqual(TEXT("cross-boundary delete shrinks L"), Out.PatternLength, 2);
		if (Out.Points.Num() == 3)
		{
			CheckPoint(*this, TEXT("delete keeps original points"), Out.Points[0], 0.1f, 0.1f, 0.0);
			CheckPoint(*this, TEXT("delete keeps original points 2"), Out.Points[1], 0.3f, 0.3f, 0.0);
			CheckPoint(*this, TEXT("delete keeps original points 3"), Out.Points[2], 0.4f, 0.4f, 0.0);
		}

		TArray<int32> FixedOnly;
		FixedOnly.Add(0);
		FixedOnly.Add(1);
		FixedOnly.Add(2);
		Errors.Reset();
		TestTrue(TEXT("delete the whole fixed segment"), FLyraRecoilPatternAdapter::Delete(Source, FixedOnly, Out, Errors));
		TestEqual(TEXT("deleting the fixed segment leaves L=0"), Out.PatternLength, 0);
		TestEqual(TEXT("deleting the fixed segment leaves N=2"), Out.Points.Num(), 2);

		TArray<int32> Everything;
		for (int32 Index = 0; Index < 5; ++Index)
		{
			Everything.Add(Index);
		}
		Errors.Reset();
		TestTrue(TEXT("delete everything"), FLyraRecoilPatternAdapter::Delete(Source, Everything, Out, Errors));
		TestEqual(TEXT("delete everything leaves N=0"), Out.Points.Num(), 0);
		TestEqual(TEXT("delete everything leaves L=0"), Out.PatternLength, 0);

		TArray<int32> OutOfRange;
		OutOfRange.Add(5);
		Errors.Reset();
		TestTrue(TEXT("delete out of range is rejected"), !FLyraRecoilPatternAdapter::Delete(Source, OutOfRange, Out, Errors));

		Errors.Reset();
		TestTrue(TEXT("empty delete is a no-op"), FLyraRecoilPatternAdapter::Delete(Source, TArray<int32>(), Out, Errors));
		TestTrue(TEXT("empty delete is a zero patch"), FLyraRecoilPatternAdapter::Equal(Out, Source));
	}

	// --- 重排：N/L 不变，显式置换 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.1f, 0.1f));
		Points.Add(FRecoilPatternPoint(0.2f, 0.2f));
		Points.Add(FRecoilPatternPoint(0.3f, 0.3f));
		Points.Add(FRecoilPatternPoint(0.4f, 0.4f));

		const FLyraRecoilPatternData Source = FLyraRecoilPatternAdapter::Read(*MakeProfile(Points, 2, 0.2f, 0.4f));

		TArray<int32> Order;
		Order.Add(3);
		Order.Add(1);
		Order.Add(0);
		Order.Add(2);

		TArray<FString> Errors;
		FLyraRecoilPatternData Out;
		TestTrue(TEXT("reorder succeeds"), FLyraRecoilPatternAdapter::Reorder(Source, Order, Out, Errors));
		TestEqual(TEXT("reorder keeps N"), Out.Points.Num(), 4);
		TestEqual(TEXT("reorder keeps L"), Out.PatternLength, 2);
		if (Out.Points.Num() == 4)
		{
			CheckPoint(*this, TEXT("reorder[0]"), Out.Points[0], 0.4f, 0.4f, 0.0);
			CheckPoint(*this, TEXT("reorder[1]"), Out.Points[1], 0.2f, 0.2f, 0.0);
			CheckPoint(*this, TEXT("reorder[2]"), Out.Points[2], 0.1f, 0.1f, 0.0);
			CheckPoint(*this, TEXT("reorder[3]"), Out.Points[3], 0.3f, 0.3f, 0.0);
		}

		TArray<int32> WrongLength;
		WrongLength.Add(0);
		Errors.Reset();
		TestTrue(TEXT("reorder with the wrong length is rejected"), !FLyraRecoilPatternAdapter::Reorder(Source, WrongLength, Out, Errors));

		TArray<int32> Duplicate;
		Duplicate.Add(0);
		Duplicate.Add(0);
		Duplicate.Add(1);
		Duplicate.Add(2);
		Errors.Reset();
		TestTrue(TEXT("reorder with duplicates is rejected"), !FLyraRecoilPatternAdapter::Reorder(Source, Duplicate, Out, Errors));

		TArray<int32> OutOfRange;
		OutOfRange.Add(0);
		OutOfRange.Add(1);
		OutOfRange.Add(2);
		OutOfRange.Add(4);
		Errors.Reset();
		TestTrue(TEXT("reorder out of range is rejected"), !FLyraRecoilPatternAdapter::Reorder(Source, OutOfRange, Out, Errors));

		TArray<int32> Identity;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Identity.Add(Index);
		}
		Errors.Reset();
		TestTrue(TEXT("identity reorder succeeds"), FLyraRecoilPatternAdapter::Reorder(Source, Identity, Out, Errors));
		TestTrue(TEXT("identity reorder is a zero patch"), FLyraRecoilPatternAdapter::Equal(Out, Source));
	}

	// --- 显式改变 PatternLength：激活尾段 X，且曲线按新索引求值 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		Points.Add(FRecoilPatternPoint(0.9f, 0.5f));
		Points.Add(FRecoilPatternPoint(-0.9f, 0.5f));

		ULyraRecoilProfile* Profile = MakeProfile(Points, 2, 0.2f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		const FLyraRecoilPatternData Source = FLyraRecoilPatternAdapter::Read(*Profile);

		TArray<FString> Errors;
		FLyraRecoilPatternData Out;
		TestTrue(TEXT("SetPatternLength grows L"), FLyraRecoilPatternAdapter::SetPatternLength(Source, 4, Out, Errors));
		TestEqual(TEXT("growing L keeps N"), Out.Points.Num(), 4);
		TestEqual(TEXT("growing L sets the new L"), Out.PatternLength, 4);

		TArray<FVector2D> FixedBefore;
		TArray<FVector2D> FixedAfter;
		TestTrue(TEXT("build L=2"), FLyraRecoilPatternAdapter::BuildCumulativeFor(*Profile, Source, FixedBefore, Errors));
		Errors.Reset();
		TestTrue(TEXT("build L=4"), FLyraRecoilPatternAdapter::BuildCumulativeFor(*Profile, Out, FixedAfter, Errors));
		TestEqual(TEXT("L=2 has 2 fixed points"), FixedBefore.Num(), 2);
		TestEqual(TEXT("L=4 has 4 fixed points"), FixedAfter.Num(), 4);
		if (FixedAfter.Num() == 4)
		{
			// 尾部 X=0.9 激活后，第 3 发 ΔYaw = 0.18：累计 P[2].Yaw = P[1].Yaw(0.2) + 0.18 = 0.38。
			CheckNear(*this, TEXT("activated tail Yaw"), FixedAfter[2].X, 0.38, 1.0e-5);
		}

		Errors.Reset();
		TestTrue(TEXT("SetPatternLength out of range is rejected"), !FLyraRecoilPatternAdapter::SetPatternLength(Source, 5, Out, Errors));
		Errors.Reset();
		TestTrue(TEXT("SetPatternLength negative is rejected"), !FLyraRecoilPatternAdapter::SetPatternLength(Source, -1, Out, Errors));
	}

	// --- 一次性稳定排序（按累计 Pitch；同值保持原顺序）---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.0f, 0.5f));
		Points.Add(FRecoilPatternPoint(0.0f, 0.25f));
		Points.Add(FRecoilPatternPoint(0.0f, 0.75f));
		Points.Add(FRecoilPatternPoint(0.0f, 0.5f));

		ULyraRecoilProfile* Profile = MakeProfile(Points, 4, 1.0f, 1.0f);
		TArray<FVector2D> CurveKeys;
		CurveKeys.Add(FVector2D(0.0, 1.0));
		CurveKeys.Add(FVector2D(1.0, -1.0));
		CurveKeys.Add(FVector2D(2.0, 1.0));
		CurveKeys.Add(FVector2D(3.0, 1.0));
		SetCurveKeys(Profile->VerticalKickCurve, CurveKeys);

		TArray<int32> Order;
		TArray<FString> Errors;
		TestTrue(TEXT("stable pitch order succeeds"), FLyraRecoilPatternAdapter::MakeStablePitchOrder(*Profile, Order, Errors));
		TestEqual(TEXT("order length"), Order.Num(), 4);
		if (Order.Num() == 4)
		{
			TestEqual(TEXT("order[0]"), Order[0], 1);
			TestEqual(TEXT("order[1]"), Order[1], 0);
			TestEqual(TEXT("order[2]"), Order[2], 2);
			TestEqual(TEXT("order[3]"), Order[3], 3);
		}

		FLyraRecoilPatternData Reordered;
		Errors.Reset();
		TestTrue(TEXT("apply the pitch order"), FLyraRecoilPatternAdapter::Reorder(FLyraRecoilPatternAdapter::Read(*Profile), Order, Reordered, Errors));
		TestEqual(TEXT("sorted reorder keeps N"), Reordered.Points.Num(), 4);
		TestEqual(TEXT("sorted reorder keeps L"), Reordered.PatternLength, 4);

		// 归一化值保留（同一多重集合）；曲线按新索引求值 → 累计点与排序前不同。
		TArray<FVector2D> OriginalCumulative;
		TArray<FVector2D> ReorderedCumulative;
		Errors.Reset();
		TestTrue(TEXT("original builds"), FLyraRecoilPatternAdapter::BuildCumulativeFor(*Profile, FLyraRecoilPatternAdapter::Read(*Profile), OriginalCumulative, Errors));
		Errors.Reset();
		TestTrue(TEXT("reordered builds"), FLyraRecoilPatternAdapter::BuildCumulativeFor(*Profile, Reordered, ReorderedCumulative, Errors));
		if (OriginalCumulative.Num() == 4 && ReorderedCumulative.Num() == 4)
		{
			// 排序后归一化值保留：[P1(0.25), P0(0.5), P2(0.75), P3(0.5)]，但曲线按新索引 0..3 重新求值。
			CheckNear(*this, TEXT("reorder resamples the curve at the new index 0"), ReorderedCumulative[0].Y, 0.25, 1.0e-6);
			CheckNear(*this, TEXT("reorder resamples the curve at the new index 1"), ReorderedCumulative[1].Y, -0.25, 1.0e-6);
			TestTrue(TEXT("reorder changes the resampled cumulative path"),
				!FMath::IsNearlyEqual(ReorderedCumulative[1].Y, OriginalCumulative[1].Y, 1.0e-6));
		}
	}

	return true;
}

// ===========================================================================
// T06 越界解决方案（有损投影 / 扩大强度）
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilEditorMappingOutOfRangeTest, "Lyra.Recoil.Editor.Mapping.OutOfRangeSolutions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorMappingOutOfRangeTest::RunTest(const FString& Parameters)
{
	// --- 默认拒绝、不钳制 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 1, 0.2f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> Targets;
		Targets.Add(FVector2D(0.5, 0.5));

		FLyraRecoilPatternData Rejected;
		TArray<FString> Errors;
		TestTrue(TEXT("out-of-range cumulative target is rejected"),
			!FLyraRecoilPatternAdapter::MoveCumulative(*Profile, Targets, Rejected, Errors));
		TestTrue(TEXT("rejection keeps the source data"),
			FLyraRecoilPatternAdapter::Equal(Rejected, FLyraRecoilPatternAdapter::Read(*Profile)));
	}

	// --- 扩大强度：H' / V' 候选 + 重新归一化 + 差异 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 2, 0.2f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> Targets;
		Targets.Add(FVector2D(0.1, 0.2));
		Targets.Add(FVector2D(0.2, 0.8));

		FLyraRecoilStrengthAdjustment Adjustment;
		FLyraRecoilPatternData Candidate;
		TArray<FString> Errors;
		TestTrue(TEXT("strength candidate is generated"),
			FLyraRecoilPatternAdapter::BuildStrengthAdjustmentCandidate(*Profile, Targets, Adjustment, Candidate, Errors));
		TestTrue(TEXT("candidate is applicable"), Adjustment.bApplicable);
		CheckNear(*this, TEXT("candidate H"), Adjustment.NewHorizontal, 0.2);
		CheckNear(*this, TEXT("candidate V"), Adjustment.NewVertical, 0.6);
		TestEqual(TEXT("candidate keeps N"), Candidate.Points.Num(), 2);
		TestEqual(TEXT("candidate keeps L"), Candidate.PatternLength, 2);
		if (Candidate.Points.Num() == 2)
		{
			CheckPoint(*this, TEXT("renormalized[0]"), Candidate.Points[0], 0.5, 1.0 / 3.0);
			CheckPoint(*this, TEXT("renormalized[1]"), Candidate.Points[1], 0.5, 1.0);
		}
		TestTrue(TEXT("differences list the vertical strength change"),
			ErrorsContain(Adjustment.Differences, TEXT("RecoilPerShot_Vertical")));
		TestTrue(TEXT("differences mention the affected random tail"),
			ErrorsContain(Adjustment.Differences, TEXT("tail")));

		// 源资产绝不被自动修改。
		TestTrue(TEXT("source H is untouched"), Profile->RecoilPerShot_Horizontal == 0.2f);
		TestTrue(TEXT("source V is untouched"), Profile->RecoilPerShot_Vertical == 0.4f);
		TestTrue(TEXT("source points are untouched"),
			Profile->PatternPoints[0].X == 0.5f && Profile->PatternPoints[0].Y == 0.5f);

		// 用候选强度正向重建必须命中目标。
		TArray<FVector2D> Rebuilt;
		TArray<FString> RebuildErrors;
		ULyraRecoilProfile* ProbeProfile = MakeProfile(Points, 2, static_cast<float>(Adjustment.NewHorizontal), static_cast<float>(Adjustment.NewVertical));
		SetCurveConstant(ProbeProfile->VerticalKickCurve, 1.0f);
		TestTrue(TEXT("candidate rebuilds with the candidate strengths"),
			FLyraRecoilPatternAdapter::BuildCumulativeFor(*ProbeProfile, Candidate, Rebuilt, RebuildErrors));
		if (Rebuilt.Num() == 2)
		{
			CheckNear(*this, TEXT("candidate rebuild[0].Pitch"), Rebuilt[0].Y, Targets[0].Y, 1.0e-5);
			CheckNear(*this, TEXT("candidate rebuild[1].Pitch"), Rebuilt[1].Y, Targets[1].Y, 1.0e-5);
		}
	}

	// --- H 变化会波及随机尾段：差异必须明确标注，且不改 HorizontalRandomRange ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 1, 0.1f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);
		Profile->HorizontalRandomRange = 0.42f;

		TArray<FVector2D> Targets;
		Targets.Add(FVector2D(0.15, 0.5));

		FLyraRecoilStrengthAdjustment Adjustment;
		FLyraRecoilPatternData Candidate;
		TArray<FString> Errors;
		TestTrue(TEXT("H-raising candidate is generated"),
			FLyraRecoilPatternAdapter::BuildStrengthAdjustmentCandidate(*Profile, Targets, Adjustment, Candidate, Errors));
		CheckNear(*this, TEXT("H candidate"), Adjustment.NewHorizontal, 0.15);
		CheckNear(*this, TEXT("V candidate"), Adjustment.NewVertical, 0.5);
		TestTrue(TEXT("differences list the horizontal strength change"),
			ErrorsContain(Adjustment.Differences, TEXT("RecoilPerShot_Horizontal")));
		TestTrue(TEXT("differences warn about HorizontalRandomRange"),
			ErrorsContain(Adjustment.Differences, TEXT("HorizontalRandomRange")));
		TestTrue(TEXT("HorizontalRandomRange is not modified"), Profile->HorizontalRandomRange == 0.42f);
	}

	// --- 零曲线：无法用强度解决，命令必须禁用 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 1, 0.2f, 0.4f);
		SetCurveConstant(Profile->VerticalKickCurve, 0.0f);

		TArray<FVector2D> Targets;
		Targets.Add(FVector2D(0.1, 0.5));

		FLyraRecoilStrengthAdjustment Adjustment;
		FLyraRecoilPatternData Candidate;
		TArray<FString> Errors;
		TestTrue(TEXT("zero-curve candidate is rejected"),
			!FLyraRecoilPatternAdapter::BuildStrengthAdjustmentCandidate(*Profile, Targets, Adjustment, Candidate, Errors));
		TestTrue(TEXT("zero-curve candidate is not applicable"), !Adjustment.bApplicable);
		TestTrue(TEXT("zero-curve blocking reasons are reported"), Adjustment.BlockingReasons.Num() > 0);
		TestTrue(TEXT("zero-curve rejection keeps the source data"),
			FLyraRecoilPatternAdapter::Equal(Candidate, FLyraRecoilPatternAdapter::Read(*Profile)));
	}

	// --- 隐藏值被迫改为 0：必须显式列出，不能声称原值保留 ---
	{
		TArray<FRecoilPatternPoint> Points;
		Points.Add(FRecoilPatternPoint(0.5f, 0.9f));
		Points.Add(FRecoilPatternPoint(0.5f, 0.7f));
		ULyraRecoilProfile* Profile = MakeProfile(Points, 2, 0.2f, 0.0f);
		SetCurveConstant(Profile->VerticalKickCurve, 1.0f);

		TArray<FVector2D> Targets;
		Targets.Add(FVector2D(0.1, 0.2));
		Targets.Add(FVector2D(0.2, 0.2));

		FLyraRecoilStrengthAdjustment Adjustment;
		FLyraRecoilPatternData Candidate;
		TArray<FString> Errors;
		TestTrue(TEXT("hidden-value candidate is generated"),
			FLyraRecoilPatternAdapter::BuildStrengthAdjustmentCandidate(*Profile, Targets, Adjustment, Candidate, Errors));
		CheckNear(*this, TEXT("hidden-value candidate V"), Adjustment.NewVertical, 0.2);
		TestTrue(TEXT("hidden value loss is listed"), Adjustment.HiddenValuesForcedToZero.Contains(1));
		TestTrue(TEXT("hidden value loss is explained"),
			ErrorsContain(Adjustment.Differences, TEXT("zero denominator")));
	}

	return true;
}

// ===========================================================================
// T07 剪贴板
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilEditorMappingClipboardTest, "Lyra.Recoil.Editor.Mapping.Clipboard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorMappingClipboardTest::RunTest(const FString& Parameters)
{
	TArray<FRecoilPatternPoint> Points;
	Points.Add(FRecoilPatternPoint(0.5f, 0.75f));
	Points.Add(FRecoilPatternPoint(-0.25f, 0.5f));
	Points.Add(FRecoilPatternPoint(0.6f, 0.8f));
	Points.Add(FRecoilPatternPoint(-0.1f, 0.2f));

	ULyraRecoilProfile* Profile = MakeProfile(Points, 4, 0.2f, 0.4f);
	TArray<FVector2D> CurveKeys;
	CurveKeys.Add(FVector2D(0.0, 1.0));
	CurveKeys.Add(FVector2D(1.0, 0.5));
	CurveKeys.Add(FVector2D(2.0, 1.0));
	SetCurveKeys(Profile->VerticalKickCurve, CurveKeys);

	// --- 非连续选择：按原发序号升序复制原始归一化参数 ---
	TArray<int32> Selection;
	Selection.Add(3);
	Selection.Add(0);
	Selection.Add(2);

	const FString ClipboardText = FLyraRecoilPatternAdapter::Copy(*Profile, Selection);
	TestTrue(TEXT("copy produces a payload"), !ClipboardText.IsEmpty());

	TArray<FRecoilPatternPoint> Parsed;
	TArray<FString> Errors;
	TestTrue(TEXT("payload parses"), FLyraRecoilPatternAdapter::ParseClipboard(ClipboardText, Parsed, Errors));
	TestEqual(TEXT("parsed point count"), Parsed.Num(), 3);
	if (Parsed.Num() == 3)
	{
		CheckPoint(*this, TEXT("parsed[0] (source index 0)"), Parsed[0], 0.5, 0.75, 0.0);
		CheckPoint(*this, TEXT("parsed[1] (source index 2)"), Parsed[1], 0.6f, 0.8f, 0.0);
		CheckPoint(*this, TEXT("parsed[2] (source index 3)"), Parsed[2], -0.1f, 0.2f, 0.0);
	}

	FLyraRecoilClipboardData ClipboardData;
	Errors.Reset();
	TestTrue(TEXT("full payload parses"), FLyraRecoilPatternAdapter::ParseClipboardEx(ClipboardText, ClipboardData, Errors));
	TestTrue(TEXT("payload carries source angles"), ClipboardData.bHasSourceAngles);
	TestEqual(TEXT("payload carries source indices"), ClipboardData.SourceIndices.Num(), 3);
	if (ClipboardData.SourceIndices.Num() == 3)
	{
		TestEqual(TEXT("source index 0"), ClipboardData.SourceIndices[0], 0);
		TestEqual(TEXT("source index 1"), ClipboardData.SourceIndices[1], 2);
		TestEqual(TEXT("source index 2"), ClipboardData.SourceIndices[2], 3);
	}
	CheckNear(*this, TEXT("payload source H"), ClipboardData.SourceHorizontal, 0.2);
	CheckNear(*this, TEXT("payload source V"), ClipboardData.SourceVertical, 0.4);

	// --- 角度粘贴：目标强度/曲线下重新反算 ---
	{
		TArray<FRecoilPatternPoint> EmptyPoints;
		ULyraRecoilProfile* Target = MakeProfile(EmptyPoints, 0, 0.5f, 0.6f);
		SetCurveConstant(Target->VerticalKickCurve, 1.0f);

		TArray<FRecoilPatternPoint> Converted;
		Errors.Reset();
		TestTrue(TEXT("angle paste succeeds"),
			FLyraRecoilPatternAdapter::ConvertClipboardAngles(*Target, ClipboardData, 0, Converted, Errors));
		TestEqual(TEXT("angle paste point count"), Converted.Num(), 3);
		if (Converted.Num() == 3)
		{
			// 源 (0.5, 0.75) @ H=0.2,V=0.4,c=1 -> yaw 0.1, pitch 0.3 -> 目标 H=0.5,V=0.6 -> (0.2, 0.5)
			CheckPoint(*this, TEXT("angle paste[0]"), Converted[0], 0.2, 0.5);
			// 源 (0.6, 0.8) @ c=1 -> yaw 0.12, pitch 0.32 -> (0.24, 0.5333333)
			CheckPoint(*this, TEXT("angle paste[1]"), Converted[1], 0.24, 0.32 / 0.6);
		}

		TArray<FRecoilPatternPoint> Rejected;
		Errors.Reset();
		TestTrue(TEXT("angle paste onto a non-invertible target is rejected"),
			!FLyraRecoilPatternAdapter::ConvertClipboardAngles(*MakeProfile(EmptyPoints, 0, 0.0f, 0.6f), ClipboardData, 0, Rejected, Errors));
	}

	// --- 未知版本 / 未知格式 / 畸形 JSON / 非数字 / 空数组：整体拒绝且无副作用 ---
	{
		TArray<FRecoilPatternPoint> OutPoints;
		OutPoints.Add(FRecoilPatternPoint(0.9f, 0.9f)); // 哨兵：拒绝后必须被清空
		TArray<FString> BadErrors;

		TestTrue(TEXT("unknown version is rejected"),
			!FLyraRecoilPatternAdapter::ParseClipboard(
				TEXT("{\"format\":\"LyraRecoilClipboard\",\"version\":99,\"points\":[{\"x\":0.1,\"y\":0.2}]}"), OutPoints, BadErrors));
		TestEqual(TEXT("unknown version leaves no points"), OutPoints.Num(), 0);
		BadErrors.Reset();

		TestTrue(TEXT("unknown format is rejected"),
			!FLyraRecoilPatternAdapter::ParseClipboard(
				TEXT("{\"format\":\"OtherTool/v1\",\"version\":1,\"points\":[{\"x\":0.1,\"y\":0.2}]}"), OutPoints, BadErrors));
		BadErrors.Reset();

		TestTrue(TEXT("malformed JSON is rejected"),
			!FLyraRecoilPatternAdapter::ParseClipboard(TEXT("{ not json"), OutPoints, BadErrors));
		BadErrors.Reset();

		TestTrue(TEXT("string coordinates are rejected"),
			!FLyraRecoilPatternAdapter::ParseClipboard(
				TEXT("{\"format\":\"LyraRecoilClipboard\",\"version\":1,\"points\":[{\"x\":\"0.1\",\"y\":0.2}]}"), OutPoints, BadErrors));
		BadErrors.Reset();

		TestTrue(TEXT("empty point array is rejected"),
			!FLyraRecoilPatternAdapter::ParseClipboard(
				TEXT("{\"format\":\"LyraRecoilClipboard\",\"version\":1,\"points\":[]}"), OutPoints, BadErrors));
		TestEqual(TEXT("rejected payload leaves no points"), OutPoints.Num(), 0);
	}

	// --- 输入大小保护：1 MiB ---
	{
		const FString Oversized = FString::ChrN(1024 * 1024 + 16, TEXT('a'));
		TArray<FRecoilPatternPoint> OutPoints;
		TArray<FString> SizeErrors;
		TestTrue(TEXT("oversized payload is rejected"),
			!FLyraRecoilPatternAdapter::ParseClipboard(Oversized, OutPoints, SizeErrors));
		TestTrue(TEXT("oversized rejection is explained"), ErrorsContain(SizeErrors, TEXT("byte editor limit")));
	}

	// --- 节点数量保护：4096 ---
	{
		FString TooMany = TEXT("{\"format\":\"LyraRecoilClipboard\",\"version\":1,\"points\":[");
		for (int32 Index = 0; Index < FLyraRecoilPatternAdapter::MaxClipboardPoints + 1; ++Index)
		{
			if (Index > 0)
			{
				TooMany += TEXT(",");
			}
			TooMany += TEXT("{\"x\":0.1,\"y\":0.2}");
		}
		TooMany += TEXT("]}");

		TArray<FRecoilPatternPoint> OutPoints;
		TArray<FString> CountErrors;
		TestTrue(TEXT("too many points is rejected"),
			!FLyraRecoilPatternAdapter::ParseClipboard(TooMany, OutPoints, CountErrors));
		TestTrue(TEXT("too many points rejection is explained"), ErrorsContain(CountErrors, TEXT("editor limit")));
	}

	// --- Copy 的输入保护 ---
	{
		TArray<int32> OutOfRangeIndices;
		OutOfRangeIndices.Add(4);
		TestTrue(TEXT("copy rejects out-of-range indices"),
			FLyraRecoilPatternAdapter::Copy(*Profile, OutOfRangeIndices).IsEmpty());
		TestTrue(TEXT("copy rejects an empty selection"),
			FLyraRecoilPatternAdapter::Copy(*Profile, TArray<int32>()).IsEmpty());
	}

	return true;
}

// ===========================================================================
// T08 原样保留与不标脏
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilEditorMappingPreservationTest, "Lyra.Recoil.Editor.Mapping.Preservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorMappingPreservationTest::RunTest(const FString& Parameters)
{
	// T08：含未生效尾部 X、零分母下非零原值、复杂曲线与全部附属参数的资产。
	const FString PackageName = FString::Printf(TEXT("/Temp/LyraRecoilEditorMapping_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*PackageName);
	ULyraRecoilProfile* Profile = NewObject<ULyraRecoilProfile>(Package, TEXT("MappingPreservationProfile"));

	Profile->PatternPoints.Empty();
	Profile->PatternPoints.Add(FRecoilPatternPoint(0.5f, 0.5f));
	Profile->PatternPoints.Add(FRecoilPatternPoint(0.5f, 0.7f)); // c[1] = 0 -> Y 是隐藏值
	Profile->PatternPoints.Add(FRecoilPatternPoint(0.9f, 0.7f)); // 未生效尾部 X
	Profile->PatternPoints.Add(FRecoilPatternPoint(-0.8f, 0.6f));
	Profile->PatternLength = 2;
	Profile->RecoilPerShot_Horizontal = 0.2f;
	Profile->RecoilPerShot_Vertical = 0.4f;
	Profile->HorizontalRandomRange = 0.42f;
	Profile->FixedRandomSeed = 12345;

	Profile->bEnableRollShake = true;
	Profile->RollShake_Amplitude = 1.25f;
	Profile->RollShake_Duration = 0.31f;
	Profile->bEnableProfileSpread = true;
	Profile->SpreadAngle_Standing = 0.42f;
	Profile->MaxSpreadAngle_Standing = 2.5f;
	Profile->SpreadExponent = 1.7f;
	Profile->WeaponVisual.bEnabled = true;
	Profile->WeaponVisual.VisualScale = 1.5f;
	Profile->WeaponVisual.MaxPitchDegrees = 9.5f;

	TArray<FVector2D> CurveKeys;
	CurveKeys.Add(FVector2D(0.0, 1.0));
	CurveKeys.Add(FVector2D(1.0, 0.0));
	CurveKeys.Add(FVector2D(2.0, 1.0));
	CurveKeys.Add(FVector2D(3.0, 1.0));
	SetCurveKeys(Profile->VerticalKickCurve, CurveKeys);

	Package->SetDirtyFlag(false);
	TestTrue(TEXT("test package starts clean"), !Package->IsDirty());

	const FLyraRecoilPatternData Source = FLyraRecoilPatternAdapter::Read(*Profile);
	const float RollAmplitudeBefore = Profile->RollShake_Amplitude;
	const float SpreadExponentBefore = Profile->SpreadExponent;
	const float VisualScaleBefore = Profile->WeaponVisual.VisualScale;
	const float RandomRangeBefore = Profile->HorizontalRandomRange;
	const int32 SeedBefore = Profile->FixedRandomSeed;
	const int32 CurveKeyCountBefore = Profile->VerticalKickCurve.EditorCurveData.Keys.Num();

	// 打开 / 选点 / 预览：只读操作不得标脏。
	TArray<FVector2D> Cumulative;
	TArray<FString> Errors;
	TestTrue(TEXT("preservation source builds"), FLyraRecoilPatternAdapter::BuildCumulative(*Profile, Cumulative, Errors));
	TestEqual(TEXT("fixed segment excludes the tail"), Cumulative.Num(), 2);
	TestTrue(TEXT("valid complex profile passes Validate"), FLyraRecoilPatternAdapter::Validate(*Profile, Errors));

	// 无操作移动：零补丁。
	FLyraRecoilPatternData NoOp;
	Errors.Reset();
	TestTrue(TEXT("preservation no-op move succeeds"),
		FLyraRecoilPatternAdapter::MoveCumulative(*Profile, Cumulative, NoOp, Errors));
	TestTrue(TEXT("preservation no-op is a zero patch"), FLyraRecoilPatternAdapter::Equal(NoOp, Source));

	// 局部编辑：只动第 2 发的水平增量；隐藏 Y 与尾部 X/Y 必须原样保留。
	TArray<FVector2D> EditTargets = Cumulative;
	EditTargets[1] = FVector2D(0.3, Cumulative[1].Y);

	FLyraRecoilPatternData Edited;
	Errors.Reset();
	TestTrue(TEXT("preservation local edit succeeds"),
		FLyraRecoilPatternAdapter::MoveCumulative(*Profile, EditTargets, Edited, Errors));
	TestEqual(TEXT("preservation keeps N"), Edited.Points.Num(), 4);
	if (Edited.Points.Num() == 4)
	{
		TestTrue(TEXT("preservation keeps the hidden Y bitwise"), Edited.Points[1].Y == 0.7f);
		CheckPoint(*this, TEXT("preservation edits fixed X"), Edited.Points[1], 1.0, 0.7f, 0.0);
		CheckPoint(*this, TEXT("preservation keeps tail X/Y (2)"), Edited.Points[2], 0.9f, 0.7f, 0.0);
		CheckPoint(*this, TEXT("preservation keeps tail X/Y (3)"), Edited.Points[3], -0.8f, 0.6f, 0.0);
	}

	// 结构命令与高级命令都只生成候选。
	TArray<FRecoilPatternPoint> Inserted;
	Inserted.Add(FRecoilPatternPoint(0.0f, 0.0f));
	FLyraRecoilPatternData InsertedData;
	Errors.Reset();
	TestTrue(TEXT("preservation insert succeeds"), FLyraRecoilPatternAdapter::Insert(Source, 0, Inserted, true, InsertedData, Errors));

	FLyraRecoilPatternData DeletedData;
	TArray<int32> DeleteIndices;
	DeleteIndices.Add(0);
	Errors.Reset();
	TestTrue(TEXT("preservation delete succeeds"), FLyraRecoilPatternAdapter::Delete(Source, DeleteIndices, DeletedData, Errors));

	TArray<int32> Order;
	Order.Add(1);
	Order.Add(0);
	Order.Add(2);
	Order.Add(3);
	FLyraRecoilPatternData ReorderedData;
	Errors.Reset();
	TestTrue(TEXT("preservation reorder succeeds"), FLyraRecoilPatternAdapter::Reorder(Source, Order, ReorderedData, Errors));

	TArray<int32> SortOrder;
	Errors.Reset();
	TestTrue(TEXT("preservation sort order succeeds"), FLyraRecoilPatternAdapter::MakeStablePitchOrder(*Profile, SortOrder, Errors));

	FLyraRecoilStrengthAdjustment Adjustment;
	FLyraRecoilPatternData AdjustedData;
	TArray<FVector2D> RaiseTargets = Cumulative;
	RaiseTargets[0] = FVector2D(Cumulative[0].X, Cumulative[0].Y + 0.1);
	Errors.Reset();
	FLyraRecoilPatternAdapter::BuildStrengthAdjustmentCandidate(*Profile, RaiseTargets, Adjustment, AdjustedData, Errors);

	const FString ClipboardText = FLyraRecoilPatternAdapter::Copy(*Profile, Order);
	TArray<FRecoilPatternPoint> ParsedClipboard;
	Errors.Reset();
	FLyraRecoilPatternAdapter::ParseClipboard(ClipboardText, ParsedClipboard, Errors);

	// 所有候选/只读调用之后：源资产逐字段不变，包不脏。
	TestTrue(TEXT("preservation keeps Roll amplitude"), Profile->RollShake_Amplitude == RollAmplitudeBefore);
	TestTrue(TEXT("preservation keeps Spread exponent"), Profile->SpreadExponent == SpreadExponentBefore);
	TestTrue(TEXT("preservation keeps WeaponVisual scale"), Profile->WeaponVisual.VisualScale == VisualScaleBefore);
	TestTrue(TEXT("preservation keeps HorizontalRandomRange"), Profile->HorizontalRandomRange == RandomRangeBefore);
	TestTrue(TEXT("preservation keeps FixedRandomSeed"), Profile->FixedRandomSeed == SeedBefore);
	TestEqual(TEXT("preservation keeps the curve key count"), Profile->VerticalKickCurve.EditorCurveData.Keys.Num(), CurveKeyCountBefore);
	TestTrue(TEXT("preservation keeps the full point array"),
		FLyraRecoilPatternAdapter::Equal(FLyraRecoilPatternAdapter::Read(*Profile), Source));
	TestTrue(TEXT("adapter calls do not dirty the package"), !Package->IsDirty());

	Package->SetDirtyFlag(false);
	return true;
}

// ===========================================================================
// 附加：全 Profile 校验的有限数补充
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilEditorMappingProfileValidationTest, "Lyra.Recoil.Editor.Mapping.ProfileValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorMappingProfileValidationTest::RunTest(const FString& Parameters)
{
	// Validate 必须覆盖“现有 ValidateProfile + 编辑器附加有限数检查”。
	TArray<FRecoilPatternPoint> Points;
	Points.Add(FRecoilPatternPoint(0.5f, 0.5f));
	ULyraRecoilProfile* Valid = MakeProfile(Points, 1, 0.2f, 0.4f);
	SetCurveConstant(Valid->VerticalKickCurve, 1.0f);

	TArray<FString> Errors;
	TestTrue(TEXT("valid profile passes Validate"), FLyraRecoilPatternAdapter::Validate(*Valid, Errors));
	TestEqual(TEXT("valid profile produces no errors"), Errors.Num(), 0);

	// NaN 会绕过 ValidateProfile 的范围比较，但必须被 Validate 捕获。
	TArray<FRecoilPatternPoint> NaNSource;
	NaNSource.Add(FRecoilPatternPoint(std::numeric_limits<float>::quiet_NaN(), 0.5f));
	ULyraRecoilProfile* NaNProfile = MakeProfile(NaNSource, 1, 0.2f, 0.4f);
	SetCurveConstant(NaNProfile->VerticalKickCurve, 1.0f);

	TArray<FString> ProfileErrors;
	const bool bProfileValid = NaNProfile->ValidateProfile(ProfileErrors);
	TestTrue(TEXT("ValidateProfile alone misses NaN (documented gap)"), bProfileValid);

	TArray<FString> EditorErrors;
	TestTrue(TEXT("Validate rejects a NaN pattern point"), !FLyraRecoilPatternAdapter::Validate(*NaNProfile, EditorErrors));
	TestTrue(TEXT("Validate explains the non-finite point"), ErrorsContain(EditorErrors, TEXT("not finite")));

	// 非法曲线键也必须被捕获。
	TArray<FRecoilPatternPoint> GoodPoints;
	GoodPoints.Add(FRecoilPatternPoint(0.5f, 0.5f));
	ULyraRecoilProfile* BadCurve = MakeProfile(GoodPoints, 1, 0.2f, 0.4f);
	BadCurve->VerticalKickCurve.EditorCurveData.Reset();
	BadCurve->VerticalKickCurve.EditorCurveData.AddKey(0.0f, 1.0f);
	BadCurve->VerticalKickCurve.EditorCurveData.AddKey(1.0f, 2.0f);
	BadCurve->VerticalKickCurve.EditorCurveData.Keys[1].Value = std::numeric_limits<float>::quiet_NaN();

	TArray<FString> BadCurveErrors;
	TestTrue(TEXT("Validate rejects a non-finite curve key"), !FLyraRecoilPatternAdapter::Validate(*BadCurve, BadCurveErrors));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
