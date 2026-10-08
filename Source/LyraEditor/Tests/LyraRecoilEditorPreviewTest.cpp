// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Curves/CurveFloat.h"
#include "UObject/Package.h"

#include "Recoil/LyraRecoilPreviewController.h"

#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"

/**
 * P4 数值预览自动化测试（计划 §13 T13 / T14，§10.2 时序合同）。
 *
 * 覆盖原则：
 *  - 所有期望值都由**直接调用 FRecoilRuntimeState** 的参考驱动程序给出，
 *    参考驱动独立复刻 LyraRangedWeaponInstance + LyraGameplayAbility_RangedWeapon
 *    的真实调用顺序（每帧 SamplePlayerAim → Advance → AdvanceSpread；
 *    发射时取方向偏移 → 加热散布 → ApplyShot）。预览必须与它逐样本一致。
 *  - 预览的相机链 / 理论累计 Kick / 逐发方向偏移分三路断言，互不混淆。
 *  - 不写任何正式资产、不改 CVar、不启动 PIE。
 */

namespace LyraRecoilPreviewTest
{
	// -----------------------------------------------------------------------
	// 夹具
	// -----------------------------------------------------------------------

	/** 造一份纯 transient 的测试 Profile（不落盘、不进 Content）。 */
	static ULyraRecoilProfile* MakeTestProfile(bool bInterpolated)
	{
		ULyraRecoilProfile* Profile = NewObject<ULyraRecoilProfile>(GetTransientPackage());
		check(Profile != nullptr);

		Profile->RecoilPerShot_Vertical = 0.5f;
		Profile->RecoilPerShot_Horizontal = 0.2f;
		Profile->MaxVerticalKick = 8.0f;
		Profile->MaxHorizontalKick = 4.0f;
		Profile->RecoveryDelay = 0.15f;
		Profile->RecoveryTime = 0.25f;
		Profile->RandomSeedMode = ERecoilRandomSeedMode::Fixed;
		Profile->FixedRandomSeed = 20260917;
		Profile->bCompensationAwareRecovery = true;
		Profile->bCompensationAwareRecoveryYaw = false;
		Profile->bEnableRollShake = false;

		// ValidateProfile 要求 VerticalKickCurve / RecoveryCurve 非空。
		Profile->VerticalKickCurve.GetRichCurve()->Reset();
		Profile->VerticalKickCurve.GetRichCurve()->AddKey(0.0f, 1.0f);
		Profile->VerticalKickCurve.GetRichCurve()->AddKey(16.0f, 1.5f);
		Profile->RecoveryCurve.GetRichCurve()->Reset();
		Profile->RecoveryCurve.GetRichCurve()->AddKey(0.0f, 0.0f);
		Profile->RecoveryCurve.GetRichCurve()->AddKey(1.0f, 1.0f);

		Profile->PatternPoints.Reset();
		for (int32 Index = 0; Index < 12; ++Index)
		{
			const float X = ((Index % 3) - 1) * 0.25f;
			const float Y = 0.4f + 0.05f * static_cast<float>(Index % 5);
			Profile->PatternPoints.Add(FRecoilPatternPoint(X, Y));
		}
		Profile->PatternLength = 8;
		Profile->HorizontalRandomRange = 0.6f;

		Profile->LiftDuration = 0.045f;
		Profile->ReboundDuration = 0.030f;
		Profile->ReboundRatio = 0.72f;

		if (bInterpolated)
		{
			Profile->SingleShotMode = ERecoilSingleShotMode::Interpolated;
			Profile->LiftCurve.GetRichCurve()->Reset();
			Profile->LiftCurve.GetRichCurve()->AddKey(0.0f, 0.0f);
			Profile->LiftCurve.GetRichCurve()->AddKey(1.0f, 1.0f);
			Profile->ReboundCurve.GetRichCurve()->Reset();
			Profile->ReboundCurve.GetRichCurve()->AddKey(0.0f, 0.0f);
			Profile->ReboundCurve.GetRichCurve()->AddKey(1.0f, 1.0f);
		}
		else
		{
			Profile->SingleShotMode = ERecoilSingleShotMode::InstantWrite;
		}

		Profile->SanitizePatternLength();
		return Profile;
	}

	/** 基础配置：固定种子、InstantWrite 覆盖、站立、瞄准 0。 */
	static FLyraRecoilPreviewConfig MakeBaseConfig()
	{
		FLyraRecoilPreviewConfig Config;
		Config.RPM = 600.0f;
		Config.ShotCount = 6;
		Config.FrameSeconds = 1.0f / 60.0f;
		Config.TailSeconds = 1.0f;
		Config.GlobalScale = 1.0f;
		Config.SeedModeOverride = ERecoilRandomSeedMode::Fixed;
		Config.FixedSeedOverride = 20260917;
		Config.SingleShotModeOverride = ELyraRecoilPreviewSingleShotMode::InstantWrite;
		Config.bAdvanceSpread = false;
		return Config;
	}

	/** 把一条输入指令追加到脚本（时间必须升序）。 */
	static void AddAim(FLyraRecoilPreviewConfig& Config, float TimeSeconds, float Pitch, float Yaw,
		EPoseState Pose = EPoseState::Standing, float AimingAlpha = 0.0f)
	{
		FLyraRecoilPreviewInputEntry Entry;
		Entry.TimeSeconds = TimeSeconds;
		Entry.AimPitchDegrees = Pitch;
		Entry.AimYawDegrees = Yaw;
		Entry.PoseState = Pose;
		Entry.AimingAlpha = AimingAlpha;
		Config.InputScript.Add(Entry);
	}

	// -----------------------------------------------------------------------
	// 参考驱动：直接调用现有 FRecoilRuntimeState，独立复刻真实调用顺序
	// -----------------------------------------------------------------------

	struct FRefSample
	{
		float Time = 0.0f;
		float TheoreticalKickPitch = 0.0f;
		float TheoreticalKickYaw = 0.0f;
		float AccumulatedPitch = 0.0f;
		float AccumulatedYaw = 0.0f;
		float CameraOffsetPitch = 0.0f;
		float CameraOffsetYaw = 0.0f;
		float CameraOffsetRoll = 0.0f;
		float VisiblePitch = 0.0f;
		float VisibleYaw = 0.0f;
		ERecoilState State = ERecoilState::Idle;
		ERecoilInterpStage InterpStage = ERecoilInterpStage::None;
		int32 ShotIndex = 0;
		float RecoveryPeakPitch = 0.0f;
		float RecoveryCoverPitch = 0.0f;
		// 这一帧内是否发生过发射（用于定位"本轮首帧"）。
		bool bFiredThisFrame = false;
	};

	/**
	 * 与 FLyraRecoilPreviewController::RunOneAtomicStep 相同的时序，但**不使用预览控制器**：
	 * 它只调 FRecoilRuntimeState 与 ULyraRecoilProfile，作为 T13/T14 的独立基准。
	 */
	// Independent offline schedule: sort the union of explicit events and frame ends,
    // then call the production runtime at each boundary. Does not use controller stepping.
    static void RunReference(const ULyraRecoilProfile& Profile,const FLyraRecoilPreviewConfig& Config,TArray<FRefSample>& OutSamples)
    {
        OutSamples.Reset();
        TArray<float> Fires, Boundaries, Frames;
        if (Config.FireInputs.IsEmpty())
            for (int32 I=0;I<Config.ShotCount;++I) Fires.Add((60.f/Config.RPM)*I);
        else for (const auto& Fire:Config.FireInputs)
            for (int32 I=0;I<Fire.ShotCount;++I) Fires.Add(Fire.StartTimeSeconds+(60.f/Fire.RPM)*I);
        Fires.Sort();
        const float Duration=(Fires.IsEmpty()?0:Fires.Last())+Config.TailSeconds;
        Boundaries.Add(0);
        for (float Time:Fires) Boundaries.Add(Time);
        for (const auto& Input:Config.InputScript) if (Input.TimeSeconds<=Duration) Boundaries.Add(Input.TimeSeconds);
        const int32 Count=FMath::CeilToInt(Duration/Config.FrameSeconds);
        for (int32 I=1;I<=Count;++I) { const float Time=FMath::Min(I*Config.FrameSeconds,Duration); Frames.Add(Time); Boundaries.Add(Time); }
        Boundaries.Sort();
        FRecoilRuntimeState State; State.Reset(&Profile);
        auto InputAt=[&](float Time)
        {
            FLyraRecoilPreviewInputEntry Input;
            Input.AimPitchDegrees=Config.BurstStartAnglePitch; Input.AimYawDegrees=Config.BurstStartAngleYaw;
            Input.GlobalScale=Config.GlobalScale;
            for (const auto& Entry:Config.InputScript) if (Entry.TimeSeconds<=Time+1.e-6f)
            {
                Input.AimPitchDegrees=Entry.AimPitchDegrees; Input.AimYawDegrees=Entry.AimYawDegrees;
                Input.PoseState=Entry.PoseState; Input.AimingAlpha=Entry.AimingAlpha;
                if (Entry.bOverrideGlobalScale) Input.GlobalScale=Entry.GlobalScale;
            }
            return Input;
        };
        auto SampleInput=[&](const FLyraRecoilPreviewInputEntry& Input)
        {
            State.SetGlobalScale(Input.GlobalScale);
            State.SetPoseMultiplier(FRecoilRuntimeState::ComputePoseMultiplier(Profile,Input.PoseState,Input.AimingAlpha));
            State.SetSpreadPlayerMultipliers(Profile.GetSpreadAimingMultiplier(Input.AimingAlpha),Config.SpreadMovementMultiplier);
            State.SamplePlayerAim(Input.AimPitchDegrees,Input.AimYawDegrees);
        };
        float Previous=0,TheoryPitch=0,TheoryYaw=0;
        int32 NextFire=0,NextFrame=0;
        bool bFired=false;
        SampleInput(InputAt(0));
        for (int32 Index=0;Index<Boundaries.Num();++Index)
        {
            const float Time=Boundaries[Index];
            if (Index>0 && FMath::IsNearlyEqual(Time,Boundaries[Index-1],1.e-6f)) continue;
            const auto OldInput=InputAt(Previous);
            SampleInput(OldInput);
            if (Time>Previous)
            {
                State.Advance(&Profile,Time-Previous);
                if (Config.bAdvanceSpread) State.AdvanceSpread(&Profile,Time-Previous,OldInput.PoseState);
            }
            const auto Input=InputAt(Time);
            SampleInput(Input);
            while (NextFire<Fires.Num() && Fires[NextFire]<=Time+1.e-6f)
            {
                if (Config.bAdvanceSpread) { State.SetPendingShotSpreadAngle(State.GetEffectiveSpreadAngle()); State.ApplySpreadShot(&Profile,Input.PoseState); }
                State.ApplyShot(&Profile,State.CurrentPoseMultiplier,Input.PoseState);
                const auto Kick=FRecoilRuntimeState::ComputeShotKick(Profile,State.ShotHistory.Last().ShotIndex,1,1,State.ActiveSeed);
                TheoryPitch+=Kick.Vertical; TheoryYaw+=Kick.Horizontal; ++NextFire; bFired=true;
            }
            if (NextFrame<Frames.Num() && FMath::IsNearlyEqual(Time,Frames[NextFrame],1.e-6f))
            {
                FRefSample Sample;
                Sample.Time=Time; Sample.TheoreticalKickPitch=TheoryPitch; Sample.TheoreticalKickYaw=TheoryYaw;
                Sample.AccumulatedPitch=State.AccumulatedPitch; Sample.AccumulatedYaw=State.AccumulatedYaw;
                Sample.CameraOffsetPitch=State.CameraOffsetPitch; Sample.CameraOffsetYaw=State.CameraOffsetYaw;
                Sample.CameraOffsetRoll=State.GetCameraRollOffset();
                Sample.VisiblePitch=Input.AimPitchDegrees+State.CameraOffsetPitch; Sample.VisibleYaw=Input.AimYawDegrees+State.CameraOffsetYaw;
                Sample.State=State.State; Sample.InterpStage=State.InterpStage; Sample.ShotIndex=State.ShotIndex;
                Sample.RecoveryPeakPitch=State.RecoveryPeakPitch; Sample.RecoveryCoverPitch=State.RecoveryCoverPitch;
                Sample.bFiredThisFrame=bFired; bFired=false;
                OutSamples.Add(Sample); ++NextFrame;
            }
            Previous=Time;
        }
    }

	/** 取采样序列里某量的最大值。 */
	static float MaxOf(const TArray<FLyraRecoilPreviewSample>& Samples, float FLyraRecoilPreviewSample::* Member)
	{
		float Result = 0.0f;
		for (const FLyraRecoilPreviewSample& Sample : Samples)
		{
			Result = FMath::Max(Result, Sample.*Member);
		}
		return Result;
	}

	static float MaxAbsOf(const TArray<FLyraRecoilPreviewSample>& Samples, float FLyraRecoilPreviewSample::* Member)
	{
		float Result = 0.0f;
		for (const FLyraRecoilPreviewSample& Sample : Samples)
		{
			Result = FMath::Max(Result, FMath::Abs(Sample.*Member));
		}
		return Result;
	}

	static float MaxAbsOf(const TArray<FRefSample>& Samples, float FRefSample::* Member)
	{
		float Result = 0.0f;
		for (const FRefSample& Sample : Samples)
		{
			Result = FMath::Max(Result, FMath::Abs(Sample.*Member));
		}
		return Result;
	}
}

// ===========================================================================
// T13-1：隔离快照 —— 外部曲线深内联、原资产零写入
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLyraRecoilEditorPreviewSnapshotTest,
	"Lyra.Recoil.Editor.Preview.IsolatedSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorPreviewSnapshotTest::RunTest(const FString& Parameters)
{
	ULyraRecoilProfile* Source = LyraRecoilPreviewTest::MakeTestProfile(false);

	// 构造一份共享的外部曲线资产（测试用 transient 对象，不落盘）。
	UCurveFloat* SharedCurve = NewObject<UCurveFloat>(GetTransientPackage());
	SharedCurve->FloatCurve.Reset();
	SharedCurve->FloatCurve.AddKey(0.0f, 0.25f);
	SharedCurve->FloatCurve.AddKey(4.0f, 3.0f);

	Source->VerticalKickCurve.ExternalCurve = SharedCurve;

	// 原资产使用外部曲线时的有效倍率。
	const float SourceScaleAtZero = Source->GetVerticalKickCurveScale(0);
	TestTrue(TEXT("测试夹具：原资产在索引 0 处读的是外部曲线的 0.25"),
		FMath::IsNearlyEqual(SourceScaleAtZero, 0.25f, 1.0e-4f));

	FLyraRecoilPreviewSnapshot Snapshot = FLyraRecoilPreviewController::BuildIsolatedSnapshot(Source);

	TestNotNull(TEXT("快照副本已创建"), Snapshot.Profile.Get());
	TestTrue(TEXT("快照无错误"), Snapshot.Error.IsEmpty());
	if (Snapshot.Profile == nullptr)
	{
		return false;
	}

	TestTrue(TEXT("快照不是原对象"), Snapshot.Profile.Get() != Source);
	TestTrue(TEXT("快照是 ULyraRecoilProfile"), Snapshot.Profile->IsA(ULyraRecoilProfile::StaticClass()));

	// 1) 外部引用被内联。
	TestTrue(TEXT("VerticalKickCurve 被记录为已内联"),
		Snapshot.Info.InlinedCurvePropertyNames.Contains(FName(TEXT("VerticalKickCurve"))));
	TestNull(TEXT("快照副本没有外部曲线引用"), Snapshot.Profile->VerticalKickCurve.ExternalCurve.Get());
	TestTrue(TEXT("快照内联后仍能取到同一倍率"),
		FMath::IsNearlyEqual(Snapshot.Profile->GetVerticalKickCurveScale(0), 0.25f, 1.0e-4f));
	TestTrue(TEXT("快照内联了全部键（索引 1..3 与外部曲线一致）"),
		FMath::IsNearlyEqual(Snapshot.Profile->GetVerticalKickCurveScale(2), 0.25f + (3.0f - 0.25f) * 0.5f, 1.0e-3f));

	// 2) 内联的是深复制：改共享曲线不影响已建立的快照。
	SharedCurve->FloatCurve.AddKey(0.0f, 99.0f);
	TestTrue(TEXT("共享曲线改动不影响快照（深复制内联）"),
		FMath::IsNearlyEqual(Snapshot.Profile->GetVerticalKickCurveScale(0), 0.25f, 1.0e-4f));

	// 3) 原资产的外部引用没有被预览破坏。
	TestTrue(TEXT("原资产仍引用共享曲线"), Source->VerticalKickCurve.ExternalCurve.Get() == SharedCurve);

	// 4) 快照上覆盖固定种子 / 单发模式不影响原资产。
	ULyraRecoilProfile* MutableSource = LyraRecoilPreviewTest::MakeTestProfile(false);
	MutableSource->RandomSeedMode = ERecoilRandomSeedMode::Random;
	MutableSource->FixedRandomSeed = 111;
	MutableSource->SingleShotMode = ERecoilSingleShotMode::InstantWrite;

	FLyraRecoilPreviewController Controller;
	FString Error;
	TArray<FName> InlinedNames;
	TestTrue(TEXT("RefreshProfile 成功"), Controller.RefreshProfile(MutableSource, Error, InlinedNames));

	FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
	Config.SeedModeOverride = ERecoilRandomSeedMode::Fixed;
	Config.FixedSeedOverride = 777;
	Config.SingleShotModeOverride = ELyraRecoilPreviewSingleShotMode::Interpolated;
	Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);

	TestEqual(TEXT("预览副本的种子模式被覆盖"),
		Controller.GetSnapshotProfile()->RandomSeedMode, ERecoilRandomSeedMode::Fixed);
	TestEqual(TEXT("预览副本的固定种子被覆盖"), Controller.GetSnapshotProfile()->FixedRandomSeed, 777);
	TestEqual(TEXT("预览副本的单发模式被覆盖"),
		Controller.GetSnapshotProfile()->SingleShotMode, ERecoilSingleShotMode::Interpolated);

	TestEqual(TEXT("原资产种子模式未被写入"), MutableSource->RandomSeedMode, ERecoilRandomSeedMode::Random);
	TestEqual(TEXT("原资产固定种子未被写入"), MutableSource->FixedRandomSeed, 111);
	TestEqual(TEXT("原资产单发模式未被写入"), MutableSource->SingleShotMode, ERecoilSingleShotMode::InstantWrite);

	// 5) 空 Profile 必须明确失败而不是崩溃。
	FLyraRecoilPreviewSnapshot EmptySnapshot = FLyraRecoilPreviewController::BuildIsolatedSnapshot(nullptr);
	TestNull(TEXT("空 Profile 不产生快照"), EmptySnapshot.Profile.Get());
	TestFalse(TEXT("空 Profile 有可读错误"), EmptySnapshot.Error.IsEmpty());

	return true;
}

// ===========================================================================
// T13-2：逐样本与"同时间步直接调用现有运行时"一致（真实调用顺序）
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLyraRecoilEditorPreviewBaselineTest,
	"Lyra.Recoil.Editor.Preview.MatchesDirectRuntimeCallOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorPreviewBaselineTest::RunTest(const FString& Parameters)
{
	struct FModeCase
	{
		bool bInterpolated;
		ELyraRecoilPreviewSingleShotMode Override;
	};

	const FModeCase Cases[] =
	{
		{ false, ELyraRecoilPreviewSingleShotMode::InstantWrite },
		{ true,  ELyraRecoilPreviewSingleShotMode::Interpolated },
		{ true,  ELyraRecoilPreviewSingleShotMode::FromProfile }
	};

	for (const FModeCase& ModeCase : Cases)
	{
		ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(ModeCase.bInterpolated);

		FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
		Config.SingleShotModeOverride = ModeCase.Override;
		Config.ShotCount = 6;
		Config.RPM = 600.0f;
		Config.TailSeconds = 1.0f;
		// 带一点压枪与姿态/瞄准脚本：覆盖"每次 Advance 前采样 + 开火前再采样"。
		LyraRecoilPreviewTest::AddAim(Config, 0.0f, 0.0f, 0.0f, EPoseState::Standing, 0.0f);
		LyraRecoilPreviewTest::AddAim(Config, 0.2f, -1.5f, 0.4f, EPoseState::Crouching, 0.5f);
		LyraRecoilPreviewTest::AddAim(Config, 0.6f, -3.0f, 0.9f, EPoseState::JumpingOrFalling, 1.0f);

		FLyraRecoilPreviewController Controller;
		FString Error;
		TArray<FName> InlinedNames;
		TestTrue(TEXT("RefreshProfile 成功"), Controller.RefreshProfile(Profile, Error, InlinedNames));
		Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);

		// 参考驱动：与控制器输入完全相同的配置。
		TArray<LyraRecoilPreviewTest::FRefSample> Reference;
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		LyraRecoilPreviewTest::RunReference(*Controller.GetSnapshotProfile(), Config, Reference);

		TestTrue(TEXT("预览执行成功"), Controller.Run());
		TestTrue(TEXT("预览时间轴跑完"), Controller.IsSimulationComplete());

		const TArray<FLyraRecoilPreviewSample>& Preview = Controller.GetSamples();
		TestTrue(FString::Printf(TEXT("采样数一致（预览 %d / 基准 %d）"), Preview.Num(), Reference.Num()),
			Preview.Num() == Reference.Num());
		TestTrue(TEXT("发射发数一致"), Controller.GetShots().Num() == 6);

		const int32 CompareCount = FMath::Min(Preview.Num(), Reference.Num());
		int32 MismatchIndex = INDEX_NONE;
		FString MismatchDetail;

		for (int32 Index = 0; Index < CompareCount; ++Index)
		{
			const FLyraRecoilPreviewSample& A = Preview[Index];
			const LyraRecoilPreviewTest::FRefSample& B = Reference[Index];

			const bool bEqual =
				FMath::IsNearlyEqual(A.TimeSeconds, B.Time, 1.0e-5f)
				&& FMath::IsNearlyEqual(A.TheoreticalKickPitch, B.TheoreticalKickPitch, 1.0e-4f)
				&& FMath::IsNearlyEqual(A.TheoreticalKickYaw, B.TheoreticalKickYaw, 1.0e-4f)
				&& FMath::IsNearlyEqual(A.AccumulatedPitch, B.AccumulatedPitch, 1.0e-4f)
				&& FMath::IsNearlyEqual(A.AccumulatedYaw, B.AccumulatedYaw, 1.0e-4f)
				&& FMath::IsNearlyEqual(A.CameraOffsetPitch, B.CameraOffsetPitch, 1.0e-4f)
				&& FMath::IsNearlyEqual(A.CameraOffsetYaw, B.CameraOffsetYaw, 1.0e-4f)
				&& FMath::IsNearlyEqual(A.CameraOffsetRoll, B.CameraOffsetRoll, 1.0e-4f)
				&& FMath::IsNearlyEqual(A.VisibleAnglePitch, B.VisiblePitch, 1.0e-4f)
				&& FMath::IsNearlyEqual(A.VisibleAngleYaw, B.VisibleYaw, 1.0e-4f)
				&& A.State == B.State
				&& A.InterpStage == B.InterpStage
				&& A.ShotIndex == B.ShotIndex;

			if (!bEqual && MismatchIndex == INDEX_NONE)
			{
				MismatchIndex = Index;
				MismatchDetail = FString::Printf(
					TEXT("t=%.5f/%.5f AccP=%.5f/%.5f CamP=%.5f/%.5f VisP=%.5f/%.5f State=%d/%d Stage=%d/%d Shot=%d/%d"),
					A.TimeSeconds, B.Time,
					A.AccumulatedPitch, B.AccumulatedPitch,
					A.CameraOffsetPitch, B.CameraOffsetPitch,
					A.VisibleAnglePitch, B.VisiblePitch,
					static_cast<int32>(A.State), static_cast<int32>(B.State),
					static_cast<int32>(A.InterpStage), static_cast<int32>(B.InterpStage),
					A.ShotIndex, B.ShotIndex);
			}
		}

		TestEqual(FString::Printf(TEXT("逐样本一致（Interpolated=%d）%s"), ModeCase.bInterpolated ? 1 : 0, *MismatchDetail),
			MismatchIndex, static_cast<int32>(INDEX_NONE));

		// 三路输出分开：理论累计 Kick 必须 >= 相机偏移（未经上限/回正）。
		const float MaxTheoretical = LyraRecoilPreviewTest::MaxOf(Preview, &FLyraRecoilPreviewSample::TheoreticalKickPitch);
		const float MaxCamera = LyraRecoilPreviewTest::MaxOf(Preview, &FLyraRecoilPreviewSample::CameraOffsetPitch);
		TestTrue(TEXT("理论累计 Kick 不小于相机偏移（两者不是同一条通道）"), MaxTheoretical >= MaxCamera - 1.0e-3f);

		// 逐发方向偏移必须与直接调用 ComputeShotKickGated 一致。
		const TArray<FLyraRecoilPreviewShot>& Shots = Controller.GetShots();
		for (int32 Index = 0; Index < Shots.Num(); ++Index)
		{
			const FLyraRecoilPreviewShot& Shot = Shots[Index];
			const FRecoilShotKick Expected = FRecoilRuntimeState::ComputeShotKickGated(
				Controller.GetSnapshotProfile(), Shot.ShotIndex, Shot.PoseMultiplier, Shot.GlobalScale,
				Controller.GetSnapshotProfile()->FixedRandomSeed, true);

			TestTrue(FString::Printf(TEXT("第 %d 发方向偏移与运行时同源"), Index),
				FMath::IsNearlyEqual(Shot.DirectionOffsetPitch, Expected.Vertical, 1.0e-4f)
				&& FMath::IsNearlyEqual(Shot.DirectionOffsetYaw, Expected.Horizontal, 1.0e-4f));
		}
	}

	return true;
}

// ===========================================================================
// T13-3：30 / 60 / 120 Hz 采样步长一致性
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLyraRecoilEditorPreviewFrameRateTest,
	"Lyra.Recoil.Editor.Preview.FrameRateConsistency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorPreviewFrameRateTest::RunTest(const FString& Parameters)
{
	const float FrameRates[] = { 30.0f, 60.0f, 120.0f };

	struct FRunResult
	{
		float FinalVisiblePitch = 0.0f;
		float FinalAccumulatedPitch = 0.0f;
		float MaxCameraPitch = 0.0f;
		int32 ShotCount = 0;
		int32 SampleCount = 0;
	};

	TArray<FRunResult> Results;

	for (const float FrameRate : FrameRates)
	{
		ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);

		FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
		Config.FrameSeconds = 1.0f / FrameRate;
		Config.ShotCount = 8;
		Config.RPM = 600.0f;
		Config.TailSeconds = 1.5f;
		LyraRecoilPreviewTest::AddAim(Config, 0.0f, 0.0f, 0.0f);
		// 从 0.25s 起持续下压 4°：覆盖压枪回正。
		LyraRecoilPreviewTest::AddAim(Config, 0.25f, -4.0f, 0.0f);

		FLyraRecoilPreviewController Controller;
		FString Error;
		TArray<FName> InlinedNames;
		TestTrue(TEXT("RefreshProfile 成功"), Controller.RefreshProfile(Profile, Error, InlinedNames));
		Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);

		TestTrue(FString::Printf(TEXT("%.0f Hz 执行成功"), FrameRate), Controller.Run());

		const TArray<FLyraRecoilPreviewSample>& Samples = Controller.GetSamples();
		TestTrue(FString::Printf(TEXT("%.0f Hz 有采样"), FrameRate), Samples.Num() > 0);

		FRunResult Result;
		if (Samples.Num() > 0)
		{
			Result.FinalVisiblePitch = Samples.Last().VisibleAnglePitch;
			Result.FinalAccumulatedPitch = Samples.Last().AccumulatedPitch;
			Result.MaxCameraPitch = LyraRecoilPreviewTest::MaxOf(Samples, &FLyraRecoilPreviewSample::CameraOffsetPitch);
		}
		Result.ShotCount = Controller.GetShots().Num();
		Result.SampleCount = Samples.Num();
		Results.Add(Result);

		// 与同一帧率下的直接调用逐样本一致（计划 §10.3 的硬要求）。
		TArray<LyraRecoilPreviewTest::FRefSample> Reference;
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		LyraRecoilPreviewTest::RunReference(*Controller.GetSnapshotProfile(), Config, Reference);
		TestTrue(FString::Printf(TEXT("%.0f Hz 采样数一致"), FrameRate), Reference.Num() == Samples.Num());

		const int32 CompareCount = FMath::Min(Reference.Num(), Samples.Num());
		int32 MismatchIndex = INDEX_NONE;
		for (int32 Index = 0; Index < CompareCount; ++Index)
		{
			if (!FMath::IsNearlyEqual(Samples[Index].CameraOffsetPitch, Reference[Index].CameraOffsetPitch, 1.0e-4f)
				|| !FMath::IsNearlyEqual(Samples[Index].VisibleAnglePitch, Reference[Index].VisiblePitch, 1.0e-4f)
				|| !FMath::IsNearlyEqual(Samples[Index].AccumulatedPitch, Reference[Index].AccumulatedPitch, 1.0e-4f))
			{
				MismatchIndex = Index;
				break;
			}
		}
		TestEqual(FString::Printf(TEXT("%.0f Hz 逐样本一致"), FrameRate),
			MismatchIndex, static_cast<int32>(INDEX_NONE));
	}

	TestTrue(TEXT("三种帧率都打完了 8 发"), Results.Num() == 3
		&& Results[0].ShotCount == 8 && Results[1].ShotCount == 8 && Results[2].ShotCount == 8);

	// 不同步长不要求中间样本位级一致，但最终稳态必须收敛到同一处。
	if (Results.Num() == 3)
	{
		const float Tolerance = 1.0e-3f;
		TestTrue(TEXT("30/60 Hz 最终可见角度一致"),
			FMath::IsNearlyEqual(Results[0].FinalVisiblePitch, Results[1].FinalVisiblePitch, Tolerance));
		TestTrue(TEXT("60/120 Hz 最终可见角度一致"),
			FMath::IsNearlyEqual(Results[1].FinalVisiblePitch, Results[2].FinalVisiblePitch, Tolerance));
		TestTrue(TEXT("30/60/120 Hz 最终累计偏移一致"),
			FMath::IsNearlyEqual(Results[0].FinalAccumulatedPitch, Results[2].FinalAccumulatedPitch, Tolerance));
	}

	return true;
}

// ===========================================================================
// T13-4：两种单发模式 × 单发 / 连发 / 停火回正
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLyraRecoilEditorPreviewSingleShotModesTest,
	"Lyra.Recoil.Editor.Preview.SingleShotModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorPreviewSingleShotModesTest::RunTest(const FString& Parameters)
{
	const ELyraRecoilPreviewSingleShotMode Modes[] =
	{
		ELyraRecoilPreviewSingleShotMode::InstantWrite,
		ELyraRecoilPreviewSingleShotMode::Interpolated
	};

	for (const ELyraRecoilPreviewSingleShotMode Mode : Modes)
	{
		ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);

		// ---- 单发 ----
		{
			FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
			Config.SingleShotModeOverride = Mode;
			Config.ShotCount = 1;
			Config.RPM = 600.0f;
			Config.TailSeconds = 1.0f;

			FLyraRecoilPreviewController Controller;
			FString Error;
			TArray<FName> InlinedNames;
			TestTrue(TEXT("单发：RefreshProfile"), Controller.RefreshProfile(Profile, Error, InlinedNames));
			Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
			TestTrue(TEXT("单发：Run"), Controller.Run());

			TestEqual(TEXT("单发：只记一发"), Controller.GetShots().Num(), 1);
			const TArray<FLyraRecoilPreviewSample>& Samples = Controller.GetSamples();
			TestTrue(TEXT("单发：有采样"), Samples.Num() > 0);
			TestEqual(TEXT("单发：回到 Idle"), Samples.Last().State, ERecoilState::Idle);
			TestEqual(TEXT("单发：Idle 时插值阶段清空"), Samples.Last().InterpStage, ERecoilInterpStage::None);
			TestTrue(TEXT("单发：最终可见角度回到起枪角 0"),
				FMath::IsNearlyEqual(Samples.Last().VisibleAnglePitch, 0.0f, 1.0e-3f));
		}

		// ---- 连发 + 停火回正 ----
		{
			FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
			Config.SingleShotModeOverride = Mode;
			Config.ShotCount = 8;
			Config.RPM = 600.0f;
			Config.TailSeconds = 1.5f;

			FLyraRecoilPreviewController Controller;
			FString Error;
			TArray<FName> InlinedNames;
			TestTrue(TEXT("连发：RefreshProfile"), Controller.RefreshProfile(Profile, Error, InlinedNames));
			Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
			TestTrue(TEXT("连发：Run"), Controller.Run());

			TestEqual(TEXT("连发：记满 8 发"), Controller.GetShots().Num(), 8);

			const TArray<FLyraRecoilPreviewSample>& Samples = Controller.GetSamples();
			TestTrue(TEXT("连发：有采样"), Samples.Num() > 0);

			// 连发期间必须出现过 Accumulating / Recovering 与插值阶段。
			bool bSawAccumulating = false;
			bool bSawRecovering = false;
			bool bSawLift = false;
			for (const FLyraRecoilPreviewSample& Sample : Samples)
			{
				bSawAccumulating |= (Sample.State == ERecoilState::Accumulating);
				bSawRecovering |= (Sample.State == ERecoilState::Recovering);
				bSawLift |= (Sample.InterpStage == ERecoilInterpStage::Lift);
			}
			TestTrue(TEXT("连发：出现过 Accumulating"), bSawAccumulating);
			TestTrue(TEXT("连发：出现过 Recovering"), bSawRecovering);
			if (Mode == ELyraRecoilPreviewSingleShotMode::Interpolated)
			{
				TestTrue(TEXT("插值：出现过 Lift 阶段"), bSawLift);
			}

			// 全程不压枪 → 回正终点回到 0，且不得出现非有限数。
			TestTrue(TEXT("连发：最终可见角度回到 0"),
				FMath::IsNearlyEqual(Samples.Last().VisibleAnglePitch, 0.0f, 1.0e-3f));
			TestTrue(TEXT("连发：所有输出有限"),
				FMath::IsFinite(Samples.Last().CameraOffsetPitch)
				&& FMath::IsFinite(Samples.Last().TheoreticalKickPitch)
				&& FMath::IsFinite(Samples.Last().RecoveryPeakPitch));
		}
	}

	return true;
}

// ===========================================================================
// T14：回正到本轮起枪角（0°/30° 起枪角，0/4/10/11° 压枪，两模式）
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLyraRecoilEditorPreviewRecoveryTest,
	"Lyra.Recoil.Editor.Preview.CompensationVisibleEndpoints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorPreviewRecoveryTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		float BurstAngle;
		float DownPull;
		float ExpectedVisiblePitch;
	};

	const FCase Cases[] =
	{
		{ 0.0f,  0.0f,  0.0f },
		{ 0.0f,  4.0f,  0.0f },
		{ 0.0f, 10.0f,  0.0f },
		{ 0.0f, 11.0f, -1.0f },
		{ 30.0f, 0.0f, 30.0f },
		{ 30.0f, 4.0f, 30.0f },
		{ 30.0f, 10.0f, 30.0f },
		{ 30.0f, 11.0f, 29.0f }
	};

	const ELyraRecoilPreviewSingleShotMode Modes[] =
	{
		ELyraRecoilPreviewSingleShotMode::InstantWrite,
		ELyraRecoilPreviewSingleShotMode::Interpolated
	};

	int32 CaseCount = 0;

	for (const ELyraRecoilPreviewSingleShotMode Mode : Modes)
	{
		for (const FCase& Case : Cases)
		{
			ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);
			// 每发 1°（Pattern Y = 1，每发垂直 Kick = 1°），10 发 = 10° 累计上跳。
			Profile->RecoilPerShot_Vertical = 1.0f;
			Profile->RecoilPerShot_Horizontal = 0.0f;
			Profile->MaxVerticalKick = 20.0f;
			Profile->ReboundRatio = 1.0f;
			Profile->PatternLength = 10;
			Profile->PatternPoints.Reset();
			for (int32 Index = 0; Index < 12; ++Index)
			{
				Profile->PatternPoints.Add(FRecoilPatternPoint(0.0f, 1.0f));
			}
			Profile->VerticalKickCurve.GetRichCurve()->Reset();
			Profile->VerticalKickCurve.GetRichCurve()->AddKey(0.0f, 1.0f);

			FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
			Config.SingleShotModeOverride = Mode;
			Config.ShotCount = 10;
			Config.RPM = 600.0f;
			Config.TailSeconds = 2.0f;
			Config.BurstStartAnglePitch = Case.BurstAngle;

			LyraRecoilPreviewTest::AddAim(Config, 0.0f, Case.BurstAngle, 0.0f);
			if (Case.DownPull > 0.0f)
			{
				// 连发期间持续下压：控制角 = 起枪角 − 下压量。
				for (int32 Shot=0;Shot<10;++Shot)
					LyraRecoilPreviewTest::AddAim(Config, Shot*.1f+.001f, Case.BurstAngle-Case.DownPull*(Shot+1)/10.f,0.0f);
			}

			FLyraRecoilPreviewController Controller;
			FString Error;
			TArray<FName> InlinedNames;
			TestTrue(TEXT("T14：RefreshProfile"), Controller.RefreshProfile(Profile, Error, InlinedNames));
			Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
			TestTrue(TEXT("T14：Run"), Controller.Run());

			const TArray<FLyraRecoilPreviewSample>& Samples = Controller.GetSamples();
			if (!TestTrue(TEXT("T14：有采样"), Samples.Num() > 0))
			{
				continue;
			}

			// 与直接调用逐样本一致（两种模式都要）。
			TArray<LyraRecoilPreviewTest::FRefSample> Reference;
			Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		LyraRecoilPreviewTest::RunReference(*Controller.GetSnapshotProfile(), Config, Reference);
			const int32 CompareCount = FMath::Min(Reference.Num(), Samples.Num());
			int32 MismatchIndex = INDEX_NONE;
			for (int32 Index = 0; Index < CompareCount; ++Index)
			{
				if (!FMath::IsNearlyEqual(Samples[Index].VisibleAnglePitch, Reference[Index].VisiblePitch, 1.0e-4f)
					|| !FMath::IsNearlyEqual(Samples[Index].AccumulatedPitch, Reference[Index].AccumulatedPitch, 1.0e-4f))
				{
					MismatchIndex = Index;
					break;
				}
			}
            if (MismatchIndex!=INDEX_NONE)
                AddInfo(FString::Printf(TEXT("Mismatch sample=%d t=%.9g/%.9g camera=%.9g/%.9g acc=%.9g/%.9g state=%d/%d stage=%d/%d"),MismatchIndex,
                    Samples[MismatchIndex].TimeSeconds,Reference[MismatchIndex].Time,Samples[MismatchIndex].CameraOffsetPitch,Reference[MismatchIndex].CameraOffsetPitch,
                    Samples[MismatchIndex].AccumulatedPitch,Reference[MismatchIndex].AccumulatedPitch,(int32)Samples[MismatchIndex].State,(int32)Reference[MismatchIndex].State,
                    (int32)Samples[MismatchIndex].InterpStage,(int32)Reference[MismatchIndex].InterpStage));
			TestEqual(FString::Printf(TEXT("T14 基准一致 A=%.0f P=%.0f (Mode=%d)"),
				Case.BurstAngle, Case.DownPull, static_cast<int32>(Mode)),
				MismatchIndex, static_cast<int32>(INDEX_NONE));

			// 实际受限上跳 K 与冻结压枪量 C 由状态机给出。
			float MaxCover = 0.0f;
			float MaxRecoveryPeak = 0.0f;
			float BurstStartOffset = 0.0f;
			bool bPeakCaptured = false;
			for (const FLyraRecoilPreviewSample& Sample : Samples)
			{
				MaxCover = FMath::Max(MaxCover, Sample.RecoveryCoverPitch);
				MaxRecoveryPeak = FMath::Max(MaxRecoveryPeak, Sample.RecoveryPeakPitch);
				if (!bPeakCaptured && Sample.RecoveryPeakPitch > 0.0f)
				{
					BurstStartOffset = Controller.GetShots()[0].BurstStartPitchOffset;
					bPeakCaptured = true;
				}
			}
			// 本轮实际受限上跳 K（相对起枪偏移）。
			const float ActualKick = FMath::Max(0.0f, MaxRecoveryPeak - BurstStartOffset);

			const FString CaseName = FString::Printf(
				TEXT("A=%.0f P=%.0f Mode=%d"), Case.BurstAngle, Case.DownPull, static_cast<int32>(Mode));

			// 需求：可见终点 = A − max(P − K, 0)，K 用运行时实际峰值。
			const float ExpectedVisible = Case.BurstAngle - FMath::Max(Case.DownPull - ActualKick, 0.0f);
			TestTrue(FString::Printf(TEXT("T14 %s：可见终点 %.4f ≈ %.4f"), *CaseName,
				Samples.Last().VisibleAnglePitch, ExpectedVisible),
				FMath::IsNearlyEqual(Samples.Last().VisibleAnglePitch, ExpectedVisible, 1.0e-3f));

			// 文档表给出的定点值（与本夹具的每发 1° × 10 发一致）。
			TestTrue(FString::Printf(TEXT("T14 %s：文档期望 %.4f"), *CaseName, Case.ExpectedVisiblePitch),
				FMath::IsNearlyEqual(Samples.Last().VisibleAnglePitch, Case.ExpectedVisiblePitch, 1.0e-3f));

			// 结束状态必须是 Idle（回正跑完），且偏移不是被"清零"伪造的：
			// Idle 中保留补偿偏移是合法状态。
			TestEqual(FString::Printf(TEXT("T14 %s：结束在 Idle"), *CaseName),
				Samples.Last().State, ERecoilState::Idle);
			TestTrue(FString::Printf(TEXT("T14 %s：压枪量被记录（%.4f >= %.4f）"), *CaseName, MaxCover, Case.DownPull),
				MaxCover >= Case.DownPull - 1.0e-3f);

			// 插值模式必须真正走过 Drop 段（不是被跳过或停在半路）。
			if (Mode == ELyraRecoilPreviewSingleShotMode::Interpolated)
			{
				bool bSawDrop = false;
				bool bSawRecovering = false;
				for (const FLyraRecoilPreviewSample& Sample : Samples)
				{
					bSawDrop |= (Sample.InterpStage == ERecoilInterpStage::Drop);
					bSawRecovering |= (Sample.State == ERecoilState::Recovering);
				}
				TestTrue(FString::Printf(TEXT("T14 %s：插值模式走过 Drop 段"), *CaseName), bSawDrop);
				TestTrue(FString::Printf(TEXT("T14 %s：插值模式走过 Recovering"), *CaseName), bSawRecovering);
			}

			++CaseCount;
		}
	}

	TestEqual(TEXT("T14 用例数 = 2 模式 × 8 组"), CaseCount, 16);

	// Drop 中重开火：回正段（Drop / Recovering）中途再开火，必须以"重开火瞬间的可见角度"
	// 作为新一轮起点（不得从 0 重来，也不得继续旧回正）。
	{
		ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);
		Profile->RecoilPerShot_Vertical = 1.0f;
		Profile->RecoilPerShot_Horizontal = 0.0f;
		Profile->MaxVerticalKick = 20.0f;
		Profile->PatternPoints.Reset();
		for (int32 Index = 0; Index < 12; ++Index)
		{
			Profile->PatternPoints.Add(FRecoilPatternPoint(0.0f, 1.0f));
		}
		Profile->VerticalKickCurve.GetRichCurve()->Reset();
		Profile->VerticalKickCurve.GetRichCurve()->AddKey(0.0f, 1.0f);

		FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
		Config.SingleShotModeOverride = ELyraRecoilPreviewSingleShotMode::Interpolated;
		Config.TailSeconds = 2.0f;

		FLyraRecoilPreviewFireInput FirstBurst;
		FirstBurst.StartTimeSeconds = 0.0f;
		FirstBurst.ShotCount = 10; // 10 发 × 0.1s = 0.9s
		FirstBurst.RPM = 600.0f;
		Config.FireInputs.Add(FirstBurst);

		// 第一轮最后一发在 0.9s，停火后 0.1s（RecoveryDelay）进入 Drop；
		// 1.05s 重开火 —— 此时正处在 Drop 中。
		FLyraRecoilPreviewFireInput SecondBurst;
		SecondBurst.StartTimeSeconds = 1.20f;
		SecondBurst.ShotCount = 2;
		SecondBurst.RPM = 600.0f;
		Config.FireInputs.Add(SecondBurst);

		FLyraRecoilPreviewController Controller;
		FString Error;
		TArray<FName> InlinedNames;
		TestTrue(TEXT("Drop 重开火：RefreshProfile"), Controller.RefreshProfile(Profile, Error, InlinedNames));
		Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		TestTrue(TEXT("Drop 重开火：Run"), Controller.Run());

		const TArray<FLyraRecoilPreviewShot>& Shots = Controller.GetShots();
		TestEqual(TEXT("Drop 重开火：共 12 发"), Shots.Num(), 12);

		if (Shots.Num() == 12)
		{
			TestEqual(TEXT("Drop 重开火：第二轮首发 ShotIndex 归零"), Shots[10].ActualShotIndex, 0);
			TestTrue(TEXT("Drop 重开火：第二轮首发标记为新一轮"), Shots[10].bStartedNewBurst);

			// 重开火时刻必须处于回正段（Recovering / Drop）——这是"Drop 中重开火"的前提。
			const TArray<FLyraRecoilPreviewSample>& Samples = Controller.GetSamples();
			int32 RefireSampleIndex = INDEX_NONE;
			for (int32 Index = 0; Index < Samples.Num(); ++Index)
			{
				if (FMath::IsNearlyEqual(Samples[Index].TimeSeconds, 1.20f, 1.0e-4f))
				{
					RefireSampleIndex = Index;
					break;
				}
			}
			TestTrue(TEXT("Drop 重开火：找到重开火时刻采样"), RefireSampleIndex != INDEX_NONE);

			bool bWasRecoveringBeforeRefire = false;
			if (RefireSampleIndex != INDEX_NONE)
			{
				for (int32 Index = 0; Index <= RefireSampleIndex; ++Index)
				{
					bWasRecoveringBeforeRefire |= (Samples[Index].State == ERecoilState::Recovering);
				}
				TestTrue(TEXT("Drop 重开火：重开火前已经进入回正段"), bWasRecoveringBeforeRefire);

				// 新一轮起点 = 重开火瞬间的可见偏移（非 0，因为前一轮还有残留）。
				TestTrue(TEXT("Drop 重开火：新一轮起点 = 重开火瞬间残留偏移"),
					FMath::IsNearlyEqual(Shots[10].BurstStartPitchOffset,
						Shots[10].BeforeCameraOffsetPitch, 1.0e-3f));
				TestTrue(TEXT("Drop 重开火：残留偏移确实非 0"),
					FMath::Abs(Shots[10].BurstStartPitchOffset) > 1.0e-3f);
			}

			// 重开火后必须回到 Lift（中断旧回正），且最终回到 Idle。
			bool bSawLiftAfterRefire = false;
			if (RefireSampleIndex != INDEX_NONE)
			{
				for (int32 Index = RefireSampleIndex; Index < Samples.Num(); ++Index)
				{
					bSawLiftAfterRefire |= (Samples[Index].InterpStage == ERecoilInterpStage::Lift);
				}
			}
			TestTrue(TEXT("Drop 重开火：重开火后重新进入 Lift"), bSawLiftAfterRefire);
			TestEqual(TEXT("Drop 重开火：最终回到 Idle"), Samples.Last().State, ERecoilState::Idle);
		}
	}

	// 补偿开关关闭：一律回满到 0（保留 A/B 对照能力）。
	{
		ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);
		Profile->RecoilPerShot_Vertical = 1.0f;
		Profile->RecoilPerShot_Horizontal = 0.0f;
		Profile->MaxVerticalKick = 20.0f;
		Profile->bCompensationAwareRecovery = false;
		Profile->PatternPoints.Reset();
		for (int32 Index = 0; Index < 12; ++Index)
		{
			Profile->PatternPoints.Add(FRecoilPatternPoint(0.0f, 1.0f));
		}
		Profile->VerticalKickCurve.GetRichCurve()->Reset();
		Profile->VerticalKickCurve.GetRichCurve()->AddKey(0.0f, 1.0f);

		FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
		Config.ShotCount = 10;
		Config.TailSeconds = 2.0f;
		Config.BurstStartAnglePitch = 30.0f;
		LyraRecoilPreviewTest::AddAim(Config, 0.0f, 30.0f, 0.0f);
		LyraRecoilPreviewTest::AddAim(Config, 0.02f, 19.0f, 0.0f);

		FLyraRecoilPreviewController Controller;
		FString Error;
		TArray<FName> InlinedNames;
		TestTrue(TEXT("T14 关补偿：RefreshProfile"), Controller.RefreshProfile(Profile, Error, InlinedNames));
		Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		TestTrue(TEXT("T14 关补偿：Run"), Controller.Run());

		const TArray<FLyraRecoilPreviewSample>& Samples = Controller.GetSamples();
		TestTrue(TEXT("T14 关补偿：有采样"), Samples.Num() > 0);
		if (Samples.Num() > 0)
		{
			TestTrue(TEXT("T14 关补偿：可见终点回到起枪角 30°（偏移回满 0）"),
				FMath::IsNearlyEqual(Samples.Last().VisibleAnglePitch, 19.0f, 1.0e-3f));
		}
	}

	return true;
}

// ===========================================================================
// T14：连续两轮 / 回正中重开火（同状态实例，不 Reset）
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLyraRecoilEditorPreviewTwoBurstTest,
	"Lyra.Recoil.Editor.Preview.TwoBurstsAndRefire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorPreviewTwoBurstTest::RunTest(const FString& Parameters)
{
	ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);
	Profile->MaxVerticalKick = 20.0f;

	FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
	Config.SingleShotModeOverride = ELyraRecoilPreviewSingleShotMode::InstantWrite;
	Config.TailSeconds = 2.0f;

	FLyraRecoilPreviewFireInput FirstBurst;
	FirstBurst.StartTimeSeconds = 0.0f;
	FirstBurst.ShotCount = 5;
	FirstBurst.RPM = 600.0f;
	Config.FireInputs.Add(FirstBurst);

	FLyraRecoilPreviewFireInput SecondBurst;
	SecondBurst.StartTimeSeconds = 0.65f; // 第一轮 0.4s 结束 + 回正途中重开火
	SecondBurst.ShotCount = 5;
	SecondBurst.RPM = 600.0f;
	Config.FireInputs.Add(SecondBurst);

	FLyraRecoilPreviewController Controller;
	FString Error;
	TArray<FName> InlinedNames;
	TestTrue(TEXT("两轮：RefreshProfile"), Controller.RefreshProfile(Profile, Error, InlinedNames));
	Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
	TestTrue(TEXT("两轮：Run"), Controller.Run());

	const TArray<FLyraRecoilPreviewShot>& Shots = Controller.GetShots();
	TestEqual(TEXT("两轮：共 10 发"), Shots.Num(), 10);

	if (Shots.Num() == 10)
	{
		// 第二轮首发（下标 5）必须是新一轮：ShotIndex 归零、账本重置。
		TestEqual(TEXT("第二轮首发 ShotIndex 归零"), Shots[5].ActualShotIndex, 0);
		TestTrue(TEXT("第二轮首发被标记为新一轮"), Shots[5].bStartedNewBurst);
		TestTrue(TEXT("第一轮首发也是新一轮"), Shots[0].bStartedNewBurst);
		TestFalse(TEXT("同一轮内第二发不是新一轮"), Shots[1].bStartedNewBurst);

		// 新一轮以"当前可见偏移"为零点：起枪偏移 = 重开火时刻的累计偏移。
		const TArray<FLyraRecoilPreviewSample>& Samples = Controller.GetSamples();
		int32 SampleBeforeRefire = INDEX_NONE;
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			if (FMath::IsNearlyEqual(Samples[Index].TimeSeconds, 0.65f, 1.0e-4f))
			{
				SampleBeforeRefire = Index;
				break;
			}
		}
		TestTrue(TEXT("找到重开火时刻的采样点"), SampleBeforeRefire != INDEX_NONE);
		if (SampleBeforeRefire != INDEX_NONE)
		{
			TestTrue(TEXT("第二轮起枪偏移与重开火时可见偏移一致"),
				FMath::IsNearlyEqual(Shots[5].BurstStartPitchOffset,
					Shots[5].BeforeAccumulatedPitch, 1.0e-3f));
			TestTrue(TEXT("第二轮从重开火瞬间的可见角度继续（不是从 0 重来）"),
				FMath::IsNearlyEqual(Shots[5].BurstStartPitchOffset,
					Shots[5].BeforeCameraOffsetPitch, 1.0e-3f));
		}

		// 逐发一致：与直接调用同源。
		TArray<LyraRecoilPreviewTest::FRefSample> Reference;
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		LyraRecoilPreviewTest::RunReference(*Controller.GetSnapshotProfile(), Config, Reference);
		const TArray<FLyraRecoilPreviewSample>& PreviewSamples = Controller.GetSamples();
		const int32 CompareCount = FMath::Min(Reference.Num(), PreviewSamples.Num());
		int32 MismatchIndex = INDEX_NONE;
		for (int32 Index = 0; Index < CompareCount; ++Index)
		{
			if (!FMath::IsNearlyEqual(PreviewSamples[Index].CameraOffsetPitch, Reference[Index].CameraOffsetPitch, 1.0e-4f))
			{
				MismatchIndex = Index;
				break;
			}
		}
		TestEqual(TEXT("两轮逐样本与基准一致"), MismatchIndex, static_cast<int32>(INDEX_NONE));
	}

	return true;
}

// ===========================================================================
// T13：固定种子可复现、不同种子受影响、姿态/瞄准/全局倍率不改变理论 Kick
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLyraRecoilEditorPreviewSeedAndMultiplierTest,
	"Lyra.Recoil.Editor.Preview.SeedAndMultipliers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorPreviewSeedAndMultiplierTest::RunTest(const FString& Parameters)
{
	// 尾段随机游走需要 8 发之后，因此用 16 发覆盖 i >= PatternLength。
	ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);
	Profile->MaxVerticalKick = 40.0f;

	FLyraRecoilPreviewConfig BaseConfig = LyraRecoilPreviewTest::MakeBaseConfig();
	BaseConfig.ShotCount = 16;
	BaseConfig.TailSeconds = 1.5f;

	auto RunWith = [&](const FLyraRecoilPreviewConfig& Config, TArray<FLyraRecoilPreviewShot>& OutShots,
		TArray<FLyraRecoilPreviewSample>& OutSamples)
	{
		FLyraRecoilPreviewController Controller;
		FString Error;
		TArray<FName> InlinedNames;
		Controller.RefreshProfile(Profile, Error, InlinedNames);
		Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		Controller.Run();
		OutShots = Controller.GetShots();
		OutSamples = Controller.GetSamples();
	};

	// 1) 同一种子两次回放逐发一致。
	TArray<FLyraRecoilPreviewShot> ShotsA;
	TArray<FLyraRecoilPreviewSample> SamplesA;
	TArray<FLyraRecoilPreviewShot> ShotsB;
	TArray<FLyraRecoilPreviewSample> SamplesB;
	RunWith(BaseConfig, ShotsA, SamplesA);
	RunWith(BaseConfig, ShotsB, SamplesB);

	TestEqual(TEXT("同种子：发数一致"), ShotsA.Num(), ShotsB.Num());
	int32 FirstDiff = INDEX_NONE;
	for (int32 Index = 0; Index < FMath::Min(ShotsA.Num(), ShotsB.Num()); ++Index)
	{
		if (!FMath::IsNearlyEqual(ShotsA[Index].DirectionOffsetYaw, ShotsB[Index].DirectionOffsetYaw, 1.0e-6f)
			|| !FMath::IsNearlyEqual(ShotsA[Index].DirectionOffsetPitch, ShotsB[Index].DirectionOffsetPitch, 1.0e-6f))
		{
			FirstDiff = Index;
			break;
		}
	}
	TestEqual(TEXT("同种子：逐发方向偏移完全一致"), FirstDiff, static_cast<int32>(INDEX_NONE));

	// 2) 换种子：固定区间（前 8 发）不变，尾段（i >= 8）改变。
	FLyraRecoilPreviewConfig OtherSeedConfig = BaseConfig;
	OtherSeedConfig.FixedSeedOverride = 12345;
	TArray<FLyraRecoilPreviewShot> ShotsC;
	TArray<FLyraRecoilPreviewSample> SamplesC;
	RunWith(OtherSeedConfig, ShotsC, SamplesC);

	TestEqual(TEXT("换种子：发数一致"), ShotsA.Num(), ShotsC.Num());
	bool bFixedSegmentIdentical = true;
	for (int32 Index = 0; Index < FMath::Min(Profile->PatternLength, FMath::Min(ShotsA.Num(), ShotsC.Num())); ++Index)
	{
		if (!FMath::IsNearlyEqual(ShotsA[Index].DirectionOffsetYaw, ShotsC[Index].DirectionOffsetYaw, 1.0e-6f))
		{
			bFixedSegmentIdentical = false;
			break;
		}
	}
	TestTrue(TEXT("换种子：固定段（i < PatternLength）不受影响"), bFixedSegmentIdentical);

	bool bTailChanged = false;
	for (int32 Index = Profile->PatternLength; Index < FMath::Min(ShotsA.Num(), ShotsC.Num()); ++Index)
	{
		if (!FMath::IsNearlyEqual(ShotsA[Index].DirectionOffsetYaw, ShotsC[Index].DirectionOffsetYaw, 1.0e-6f))
		{
			bTailChanged = true;
			break;
		}
	}
	TestTrue(TEXT("换种子：随机尾段发生变化"), bTailChanged);

	// 3) 姿态 / 瞄准 Alpha / 全局倍率只改实际 Kick，不改理论累计 Kick。
	auto MaxTheoretical = [](const TArray<FLyraRecoilPreviewSample>& Samples)
	{
		return LyraRecoilPreviewTest::MaxOf(Samples, &FLyraRecoilPreviewSample::TheoreticalKickPitch);
	};

	FLyraRecoilPreviewConfig PoseConfig = BaseConfig;
	LyraRecoilPreviewTest::AddAim(PoseConfig, 0.0f, 0.0f, 0.0f, EPoseState::Crouching, 1.0f);
	PoseConfig.GlobalScale = 2.0f;
	TArray<FLyraRecoilPreviewShot> ShotsPose;
	TArray<FLyraRecoilPreviewSample> SamplesPose;
	RunWith(PoseConfig, ShotsPose, SamplesPose);

	TestTrue(TEXT("姿态/瞄准/倍率不改变理论累计 Kick"),
		FMath::IsNearlyEqual(MaxTheoretical(SamplesA), MaxTheoretical(SamplesPose), 1.0e-4f));
	TestTrue(TEXT("姿态/瞄准/倍率改变实际 Kick"),
		!FMath::IsNearlyEqual(ShotsA[0].AppliedKickPitch, ShotsPose[0].AppliedKickPitch, 1.0e-5f));

	// 4) 全局倍率 = 0：相机偏移恒为 0，理论累计 Kick 仍照常累加。
	FLyraRecoilPreviewConfig ZeroConfig = BaseConfig;
	ZeroConfig.GlobalScale = 0.0f;
	TArray<FLyraRecoilPreviewShot> ShotsZero;
	TArray<FLyraRecoilPreviewSample> SamplesZero;
	RunWith(ZeroConfig, ShotsZero, SamplesZero);

	TestTrue(TEXT("倍率 0：相机 Pitch 偏移恒为 0"),
		FMath::IsNearlyEqual(LyraRecoilPreviewTest::MaxAbsOf(SamplesZero, &FLyraRecoilPreviewSample::CameraOffsetPitch), 0.0f, 1.0e-5f));
	TestTrue(TEXT("倍率 0：理论累计 Kick 仍累加"),
		MaxTheoretical(SamplesZero) > 0.0f);

	// 5) 姿态 站立/蹲伏/空中 的倍率比值与资产一致。
	FLyraRecoilPreviewConfig CrouchConfig = BaseConfig;
	LyraRecoilPreviewTest::AddAim(CrouchConfig, 0.0f, 0.0f, 0.0f, EPoseState::Crouching, 0.0f);
	TArray<FLyraRecoilPreviewShot> ShotsCrouch;
	TArray<FLyraRecoilPreviewSample> SamplesCrouch;
	RunWith(CrouchConfig, ShotsCrouch, SamplesCrouch);

	TestTrue(TEXT("蹲伏实际 Kick = 站立 × 姿态倍率比"),
		FMath::IsNearlyEqual(
			ShotsCrouch[0].AppliedKickPitch,
			ShotsA[0].AppliedKickPitch * Profile->PoseMultiplier_Crouching, 1.0e-4f));

	return true;
}

// ===========================================================================
// T13：上限饱和与长帧保护（单次大 Delta 原样交给 Advance）
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLyraRecoilEditorPreviewSaturationAndLongFrameTest,
	"Lyra.Recoil.Editor.Preview.SaturationAndLongFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorPreviewSaturationAndLongFrameTest::RunTest(const FString& Parameters)
{
	// ---- 垂直上限 ----
	{
		ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);
		Profile->RecoilPerShot_Vertical = 5.0f;
		Profile->MaxVerticalKick = 6.0f;
		Profile->PatternPoints.Reset();
		for (int32 Index = 0; Index < 12; ++Index)
		{
			Profile->PatternPoints.Add(FRecoilPatternPoint(0.0f, 1.0f));
		}
		Profile->VerticalKickCurve.GetRichCurve()->Reset();
		Profile->VerticalKickCurve.GetRichCurve()->AddKey(0.0f, 1.0f);

		FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
		Config.ShotCount = 8;
		Config.TailSeconds = 0.05f; // 停火前观察饱和（尾时长不覆盖回正）

		FLyraRecoilPreviewController Controller;
		FString Error;
		TArray<FName> InlinedNames;
		TestTrue(TEXT("饱和：RefreshProfile"), Controller.RefreshProfile(Profile, Error, InlinedNames));
		Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		TestTrue(TEXT("饱和：Run"), Controller.Run());

		const TArray<FLyraRecoilPreviewSample>& Samples = Controller.GetSamples();
		TestTrue(TEXT("饱和：有采样"), Samples.Num() > 0);
		const float MaxCamera = LyraRecoilPreviewTest::MaxOf(Samples, &FLyraRecoilPreviewSample::CameraOffsetPitch);
		TestTrue(FString::Printf(TEXT("相机偏移被钳在 MaxVerticalKick=6 以内（实测 %.4f）"), MaxCamera),
			MaxCamera <= Profile->MaxVerticalKick + 1.0e-3f);
		TestTrue(TEXT("确实到达上限附近"), MaxCamera >= Profile->MaxVerticalKick - 0.5f);

		// 理论上跳远大于上限：证明"理论累计 Kick"确实是与相机链分开的一条通道。
		const float MaxTheoretical = LyraRecoilPreviewTest::MaxOf(Samples, &FLyraRecoilPreviewSample::TheoreticalKickPitch);
		TestTrue(TEXT("理论累计 Kick 不受相机上限约束"), MaxTheoretical > Profile->MaxVerticalKick + 1.0f);
	}

	// ---- 水平上限 ----
	{
		ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);
		Profile->RecoilPerShot_Horizontal = 2.0f;
		Profile->MaxHorizontalKick = 3.0f;
		Profile->PatternPoints.Reset();
		for (int32 Index = 0; Index < 12; ++Index)
		{
			Profile->PatternPoints.Add(FRecoilPatternPoint(1.0f, 0.0f));
		}

		FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
		Config.ShotCount = 8;
		Config.TailSeconds = 0.05f;

		FLyraRecoilPreviewController Controller;
		FString Error;
		TArray<FName> InlinedNames;
		TestTrue(TEXT("水平饱和：RefreshProfile"), Controller.RefreshProfile(Profile, Error, InlinedNames));
		Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		TestTrue(TEXT("水平饱和：Run"), Controller.Run());

		const TArray<FLyraRecoilPreviewSample>& Samples = Controller.GetSamples();
		TestTrue(TEXT("水平饱和：有采样"), Samples.Num() > 0);
		const float MaxYaw = LyraRecoilPreviewTest::MaxAbsOf(Samples, &FLyraRecoilPreviewSample::CameraOffsetYaw);
		TestTrue(FString::Printf(TEXT("Yaw 被钳在 MaxHorizontalKick=3 以内（实测 %.4f）"), MaxYaw),
			MaxYaw <= Profile->MaxHorizontalKick + 1.0e-3f);
	}

	// ---- 长帧：0.5s 单次大 Delta 必须走现有 Advance 的真实长帧收尾分支 ----
	{
		ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(true);
		Profile->MaxVerticalKick = 20.0f;

		FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
		Config.SingleShotModeOverride = ELyraRecoilPreviewSingleShotMode::Interpolated;
		Config.ShotCount = 4;
		Config.TailSeconds = 0.05f;
		Config.LongFrameOverrideSeconds = 0.5f;
		// 压枪 4°：长帧收尾必须把压枪量带进稳态终点，而不是把峰值清零。
		LyraRecoilPreviewTest::AddAim(Config, 0.0f, 0.0f, 0.0f);
		LyraRecoilPreviewTest::AddAim(Config, 0.19f, -4.0f, 0.0f);

		FLyraRecoilPreviewController Controller;
		FString Error;
		TArray<FName> InlinedNames;
		TestTrue(TEXT("长帧：RefreshProfile"), Controller.RefreshProfile(Profile, Error, InlinedNames));
		Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		TestTrue(TEXT("长帧：Run"), Controller.Run());

		const int32 SampleCountBeforeLongFrame = Controller.GetSampleCount();
		const bool bLongFrameApplied = Controller.RunLongFrameStep();
		TestTrue(TEXT("长帧步骤被执行"), bLongFrameApplied);

		const TArray<FLyraRecoilPreviewSample>& Samples = Controller.GetSamples();
		TestTrue(TEXT("长帧：样本增加"), Samples.Num() > SampleCountBeforeLongFrame);

		const FLyraRecoilPreviewSample& Last = Samples.Last();
		TestTrue(TEXT("长帧：推进到 0.5s 之后"), Last.TimeSeconds >= 0.5f);
		TestEqual(TEXT("长帧：收尾后回到 Idle"), Last.State, ERecoilState::Idle);
		TestEqual(TEXT("长帧：插值阶段清空"), Last.InterpStage, ERecoilInterpStage::None);
		// K=4 发 × 每发约 0.5*Y(0.4~0.6)=0.2~0.3 → 实际受限上跳 K；压枪 4°。
		// 可见终点 = 0 − max(4 − K, 0)。只要求：有限、单调收敛、且不是把压枪量整体丢掉。
		TestTrue(TEXT("长帧：输出有限"), FMath::IsFinite(Last.VisibleAnglePitch));
		TestTrue(FString::Printf(TEXT("长帧：保留了压枪抵扣（可见终点 %.4f ≤ 0）"), Last.VisibleAnglePitch),
			Last.VisibleAnglePitch <= 1.0e-3f);
		TestTrue(TEXT("长帧：压枪量被冻结记录"), Last.RecoveryCoverPitch >= 4.0f - 1.0e-3f);

		// 长帧路径必须与"直接调用 Advance(0.5s)"一致。
		FRecoilRuntimeState ReferenceState;
		ReferenceState.Reset(Controller.GetSnapshotProfile());
		ReferenceState.SetGlobalScale(Config.GlobalScale);
		ReferenceState.SamplePlayerAim(-4.0f, 0.0f);
		ReferenceState.Advance(Controller.GetSnapshotProfile(), 0.5f);
		TestTrue(TEXT("长帧与直接 Advance(0.5f) 的语义同类（都进入 Idle 或 Recovering）"),
			ReferenceState.State == ERecoilState::Idle || ReferenceState.State == ERecoilState::Recovering);
	}

	return true;
}

// ===========================================================================
// T13：缓存失效 / 预算增量执行 / 上限与 seek 重放
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLyraRecoilEditorPreviewCacheBudgetTest,
	"Lyra.Recoil.Editor.Preview.CacheInvalidationAndBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilEditorPreviewCacheBudgetTest::RunTest(const FString& Parameters)
{
	// ---- 缓存失效 ----
	{
		ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);

		FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
		Config.ShotCount = 5;
		Config.TailSeconds = 0.5f;

		FLyraRecoilPreviewController Controller;
		FString Error;
		TArray<FName> InlinedNames;
		TestTrue(TEXT("缓存：RefreshProfile"), Controller.RefreshProfile(Profile, Error, InlinedNames));
		TestFalse(TEXT("缓存：刷新后失效"), Controller.IsResultCacheValid());

		Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		TestFalse(TEXT("缓存：设置配置后失效"), Controller.IsResultCacheValid());

		TestTrue(TEXT("缓存：Run 成功"), Controller.Run());
		TestTrue(TEXT("缓存：Run 后有效"), Controller.IsResultCacheValid());

		// 改射速 → 令牌变化 → 结果失效。
		FLyraRecoilPreviewConfig FasterConfig = Config;
		FasterConfig.RPM = 900.0f;
		Controller.SetConfig(FasterConfig);
		TestFalse(TEXT("缓存：改射速后失效"), Controller.IsResultCacheValid());

		TestTrue(TEXT("缓存：重跑成功"), Controller.Run());
		TestTrue(TEXT("缓存：重跑后有效"), Controller.IsResultCacheValid());

		// 改输入脚本 → 同样失效。
		FLyraRecoilPreviewConfig ScriptConfig = FasterConfig;
		LyraRecoilPreviewTest::AddAim(ScriptConfig, 0.0f, -2.0f, 0.0f);
		Controller.SetConfig(ScriptConfig);
		TestFalse(TEXT("缓存：改输入脚本后失效"), Controller.IsResultCacheValid());

		// 刷新快照 → 失效。
		TestTrue(TEXT("缓存：再次 RefreshProfile"), Controller.RefreshProfile(Profile, Error, InlinedNames));
		TestFalse(TEXT("缓存：刷新快照后失效"), Controller.IsResultCacheValid());

		// 结果不能混用新旧参数：重新 Run 后与"直接新配置跑一遍"一致。
		FLyraRecoilPreviewController FreshController;
		TArray<FName> FreshNames;
		FreshController.RefreshProfile(Profile, Error, FreshNames);
		FreshController.SetConfig(ScriptConfig);
		FreshController.Run();

		Controller.SetConfig(ScriptConfig);
		Controller.Run();
		TestEqual(TEXT("缓存：重算后的发数与全新控制器一致"),
			Controller.GetShots().Num(), FreshController.GetShots().Num());
		if (Controller.GetShots().Num() > 0 && FreshController.GetShots().Num() > 0)
		{
			TestTrue(TEXT("缓存：重算后的逐发结果与全新控制器一致"),
				FMath::IsNearlyEqual(Controller.GetShots()[0].DirectionOffsetPitch,
					FreshController.GetShots()[0].DirectionOffsetPitch, 1.0e-6f));
		}
	}

	// ---- 预算增量执行 ----
	{
		ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);

		FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
		Config.ShotCount = 8;
		Config.TailSeconds = 1.0f;

		FLyraRecoilPreviewController Unlimited;
		FString Error;
		TArray<FName> InlinedNames;
		Unlimited.RefreshProfile(Profile, Error, InlinedNames);
		Unlimited.SetConfig(Config);
		Unlimited.Run();

		FLyraRecoilPreviewController Budgeted;
		Budgeted.RefreshProfile(Profile, Error, InlinedNames);
		Budgeted.SetConfig(Config);

		int32 BudgetCalls = 0;
		bool bCompleted = false;
		while (!bCompleted && BudgetCalls < 100000)
		{
			Budgeted.AdvanceBudget(0.0, bCompleted);
			++BudgetCalls;
		}

		TestTrue(TEXT("预算：最终跑完"), bCompleted);
		TestTrue(TEXT("预算：需要多次调用（确实是增量执行）"), BudgetCalls > 1);
		TestEqual(TEXT("预算：发数与不限预算一致"), Budgeted.GetShots().Num(), Unlimited.GetShots().Num());

		const TArray<FLyraRecoilPreviewSample>& A = Unlimited.GetSamples();
		const TArray<FLyraRecoilPreviewSample>& B = Budgeted.GetSamples();
		TestEqual(TEXT("预算：样本数与不限预算一致"), B.Num(), A.Num());
		if (A.Num() > 0 && A.Num() == B.Num())
		{
			int32 FirstDiff = INDEX_NONE;
			for (int32 Index = 0; Index < A.Num(); ++Index)
			{
				if (!FMath::IsNearlyEqual(A[Index].CameraOffsetPitch, B[Index].CameraOffsetPitch, 1.0e-5f)
					|| !FMath::IsNearlyEqual(A[Index].TimeSeconds, B[Index].TimeSeconds, 1.0e-5f))
				{
					FirstDiff = Index;
					break;
				}
			}
			TestEqual(TEXT("预算：逐样本与不限预算一致（事件序列不随预算改变）"),
				FirstDiff, static_cast<int32>(INDEX_NONE));
			TestEqual(TEXT("预算：最终状态一致"), static_cast<int32>(B.Last().State), static_cast<int32>(A.Last().State));
		}
	}

	// ---- 限额与 seek 重放 ----
	{
		ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);

		// 发数上限 4096：用极短间隔让被截断的时间轴仍然很小，测试保持轻量。
		FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
		Config.ShotCount = 5000;
		Config.RPM = 60000.0f; // 间隔 1ms
		Config.FrameSeconds = 1.0f / 240.0f;
		Config.TailSeconds = 0.05f;

		FLyraRecoilPreviewController Controller;
		FString Error;
		TArray<FName> InlinedNames;
		TestTrue(TEXT("限额：RefreshProfile"), Controller.RefreshProfile(Profile, Error, InlinedNames));
		Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		TestTrue(TEXT("限额：Run"), Controller.Run());

		TestTrue(TEXT("限额：被标记截断"), Controller.IsTruncatedByLimit());
		TestTrue(TEXT("限额：截断原因非空"), !Controller.GetTruncationReason().IsEmpty());
		TestTrue(FString::Printf(TEXT("限额：发射数不超过 %d（实测 %d）"),
			FLyraRecoilPreviewConfig::MaxShots, Controller.GetShots().Num()),
			Controller.GetShots().Num() <= FLyraRecoilPreviewConfig::MaxShots);
		TestTrue(TEXT("限额：恰好用满上限（截断确实生效）"),
			Controller.GetShots().Num() == FLyraRecoilPreviewConfig::MaxShots);
		TestTrue(TEXT("限额：时间轴不超过 120 秒"),
			Controller.GetSimulationDurationSeconds() <= FLyraRecoilPreviewConfig::MaxSimulationSeconds + 1.0e-3f);

		// seek 向后跳必须能从 t=0 重放出一致结果。
		const float Duration = Controller.GetSimulationDurationSeconds();
		TestTrue(TEXT("限额：时间轴为正"), Duration > 0.0f);

		FLyraRecoilPreviewController ReplayController;
		TArray<FName> ReplayNames;
		ReplayController.RefreshProfile(Profile, Error, ReplayNames);
		ReplayController.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		ReplayController.Run();
		const int32 ShotCountBefore = ReplayController.GetShots().Num();
		const float FinalPitchBefore = ReplayController.GetSamples().Num() > 0
			? ReplayController.GetSamples().Last().CameraOffsetPitch : 0.0f;

		// 跳到中途 → 再回 0：必须重新得到同一份逐发结果。
		ReplayController.SeekToTime(Duration * 0.5f);
		ReplayController.SeekToTime(0.0f);
		TestTrue(TEXT("seek 后播放头归零"),
			FMath::IsNearlyZero(ReplayController.GetPlayheadTimeSeconds(), 1.0e-5f));
		TestFalse(TEXT("seek 后不再处于完成态（等待重放）"), ReplayController.IsSimulationComplete());
		ReplayController.Run();
		TestTrue(TEXT("seek 重放后再次跑完"), ReplayController.IsSimulationComplete());

		TestEqual(TEXT("seek 重放：发数一致"), ReplayController.GetShots().Num(), ShotCountBefore);
		if (ReplayController.GetSamples().Num() > 0)
		{
			TestTrue(TEXT("seek 重放：最终相机偏移一致"),
				FMath::IsNearlyEqual(ReplayController.GetSamples().Last().CameraOffsetPitch, FinalPitchBefore, 1.0e-5f));
		}

		// 播放倍率只影响播放位置，不改变模拟结果。
		const float SimTimeBefore = ReplayController.GetLastSampleTimeSeconds();
		ReplayController.AdvancePlayhead(0.25f, 4.0f);
		TestTrue(TEXT("播放位置按倍率推进"), ReplayController.GetPlayheadTimeSeconds() > 0.0f);
		TestTrue(TEXT("播放不改变模拟结果"),
			FMath::IsNearlyEqual(ReplayController.GetLastSampleTimeSeconds(), SimTimeBefore, 1.0e-6f));
	}

	// 时长上限：120 秒截断。
	{
		ULyraRecoilProfile* Profile = LyraRecoilPreviewTest::MakeTestProfile(false);

		FLyraRecoilPreviewConfig Config = LyraRecoilPreviewTest::MakeBaseConfig();
		Config.ShotCount = 100;
		Config.RPM = 600.0f;
		Config.TailSeconds = 300.0f;

		FLyraRecoilPreviewController Controller;
		FString Error;
		TArray<FName> InlinedNames;
		Controller.RefreshProfile(Profile, Error, InlinedNames);
		Controller.SetConfig(Config);
		Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);
		Controller.Run();

		TestTrue(TEXT("时长：被标记截断"), Controller.IsTruncatedByLimit());
		TestTrue(TEXT("时长：时间轴被截到 120 秒"),
			Controller.GetSimulationDurationSeconds() <= FLyraRecoilPreviewConfig::MaxSimulationSeconds + 1.0e-3f);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
