// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/Recoil/LyraRecoilDebug.h"

#include "DrawDebugHelpers.h"
#include "Camera/LyraCameraRollShake.h"
#include "Engine/Engine.h"
#include "Equipment/LyraEquipmentManagerComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Weapons/LyraRangedWeaponInstance.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LyraRecoilDebug)

DEFINE_LOG_CATEGORY_STATIC(LogLyraRecoilDebug, Log, All);

namespace LyraRecoilDebugPrivate
{
	/** 面板固定 key，保证每帧覆盖同一条消息而不是刷屏 */
	static constexpr uint64 PanelMessageKey = 0x525452434F494C31ull;

	/** Roll 面板用另一个 key，这样它与主面板可以同时显示、互不覆盖 */
	static constexpr uint64 RollPanelMessageKey = 0x525452434F494C32ull;

	/** Roll 曲线采样的段数（时间轴 ASCII 折线用） */
	static constexpr int32 RollCurveSamples = 48;

	/** 世界内可视化的画线长度（cm） */
	static constexpr float DebugDrawTraceLength = 1500.0f;

	/** 最近一次成功导出的 CSV 路径 */
	static FString LastDumpPath;

	static const TCHAR* GetStateName(ERecoilState State)
	{
		switch (State)
		{
		case ERecoilState::Idle:			return TEXT("Idle");
		case ERecoilState::Accumulating:	return TEXT("Accumulating");
		case ERecoilState::Recovering:		return TEXT("Recovering");
		default:							return TEXT("Unknown");
		}
	}

	/** 插值阶段名：只在单发模型选了 Interpolated 时才有意义。 */
	static const TCHAR* GetInterpStageName(ERecoilInterpStage Stage)
	{
		switch (Stage)
		{
		case ERecoilInterpStage::None:		return TEXT("-");
		case ERecoilInterpStage::Lift:		return TEXT("Lift");
		case ERecoilInterpStage::Rebound:	return TEXT("Rebound");
		case ERecoilInterpStage::Settle:	return TEXT("Settle");
		case ERecoilInterpStage::Drop:		return TEXT("Drop");
		default:							return TEXT("Unknown");
		}
	}

	static const TCHAR* GetPoseName(EPoseState Pose)
	{
		switch (Pose)
		{		case EPoseState::Standing:			return TEXT("Standing");
		case EPoseState::Crouching:			return TEXT("Crouching");
		case EPoseState::JumpingOrFalling:	return TEXT("JumpingOrFalling");
		default:							return TEXT("Unknown");
		}
	}
}

namespace LyraRecoilCVars
{
	/** 总开关 */
	static bool bRecoilEnabled = true;
	static FAutoConsoleVariableRef CVarRecoilEnabled(
		TEXT("Lyra.Recoil.Enable"),
		bRecoilEnabled,
		TEXT("Enable the weapon recoil system. 0 = disabled: bullet deviation is zero and no camera offset is applied.\n")
		TEXT("Default: 1"),
		ECVF_Default);

	/** 全局调试倍率 */
	static float GlobalScale = 1.0f;
	static FAutoConsoleVariableRef CVarRecoilScale(
		TEXT("Lyra.Recoil.Scale"),
		GlobalScale,
		TEXT("Global debug multiplier applied on top of the recoil profile values. Does NOT modify the data asset.\n")
		TEXT("Default: 1.0"),
		ECVF_Default);

	/** 屏幕数值面板 */
	static bool bDebugPanel = false;
	static FAutoConsoleVariableRef CVarRecoilDebug(
		TEXT("Lyra.Recoil.Debug"),
		bDebugPanel,
		TEXT("Show the on-screen recoil state panel (ShotIndex / Kick / State / RecoveryTimer).\n")
		TEXT("Default: 0"),
		ECVF_Default);

	/** 世界内可视化 */
	static bool bDebugDraw = false;
	static FAutoConsoleVariableRef CVarRecoilDebugDraw(
		TEXT("Lyra.Recoil.DebugDraw"),
		bDebugDraw,
		TEXT("Draw in-world recoil visualization: green = true aim axis, red = current camera offset direction,\n")
		TEXT("yellow points = pattern points, cyan bar = recovery progress, magenta arc = current roll shake angle.\n")
		TEXT("Default: 0"),
		ECVF_Default);

	/**
	 * Roll 震动实时振幅倍率。
	 *
	 * 调参期的主旋钮：0 = 关掉 Roll（用于 A/B 对比），2 = 放大两倍便于观察。
	 * 之所以做成倍率而不是"直接改振幅"：资产数值是基准，倍率是临时叠加，
	 * 退出 PIE 即失效，不会把调试值写进资产。
	 */
	static float RollShakeScale = 1.0f;
	static FAutoConsoleVariableRef CVarRecoilRollShakeScale(
		TEXT("Lyra.Recoil.RollShake"),
		RollShakeScale,
		TEXT("Runtime amplitude multiplier for the roll shake channel (0 = disable roll shake, 2 = double it).\n")
		TEXT("Affects the camera display layer only; never modifies the data asset or the bullet path.\n")
		TEXT("Default: 1.0"),
		ECVF_Default);

	/**
	 * Roll 震动实时调试面板。
	 *
	 * 与 Lyra.Recoil.Debug 分开：主面板关心"后坐力状态机"，本面板关心"这一发 Roll 的波形"，
	 * 两者排查的是不同问题，合在一起会出现一屏 10 行数字反而看不清。
	 */
	static bool bRollDebugPanel = false;
	static FAutoConsoleVariableRef CVarRecoilRollDebug(
		TEXT("Lyra.Recoil.RollDebug"),
		bRollDebugPanel,
		TEXT("Show the live roll-shake panel: current angle / decay envelope / phase / active params / waveform.\n")
		TEXT("Independent from Lyra.Recoil.Debug so both panels can be shown side by side.\n")
		TEXT("Default: 0"),
		ECVF_Default);
}

bool ULyraRecoilDebug::IsRecoilEnabled()
{
	return LyraRecoilCVars::bRecoilEnabled;
}

float ULyraRecoilDebug::GetGlobalScale()
{
	return FMath::Max(0.0f, LyraRecoilCVars::GlobalScale);
}

bool ULyraRecoilDebug::IsDebugPanelEnabled()
{
	return LyraRecoilCVars::bDebugPanel;
}

bool ULyraRecoilDebug::IsDebugDrawEnabled()
{
	return LyraRecoilCVars::bDebugDraw;
}

float ULyraRecoilDebug::GetRollShakeScale()
{
	return FMath::Max(0.0f, LyraRecoilCVars::RollShakeScale);
}

bool ULyraRecoilDebug::IsRollDebugPanelEnabled()
{
	return LyraRecoilCVars::bRollDebugPanel;
}

FString ULyraRecoilDebug::GetLastDumpPath()
{
	return LyraRecoilDebugPrivate::LastDumpPath;
}

//////////////////////////////////////////////////////////////////////////
// 可视化：屏幕数值面板（P2）
//////////////////////////////////////////////////////////////////////////

void ULyraRecoilDebug::DrawDebugPanel(const UWorld* World, const ULyraRecoilProfile* Profile, const FRecoilRuntimeState& State)
{
#if !UE_BUILD_SHIPPING
	if (!LyraRecoilCVars::bDebugPanel || (World == nullptr) || (GEngine == nullptr))
	{
		return;
	}

	const float Scale = LyraRecoilCVars::GlobalScale;
	const bool bEnabled = LyraRecoilCVars::bRecoilEnabled;

	const FString Header = FString::Printf(
		TEXT("[Recoil] %s   Scale=x%.2f   Profile=%s"),
		bEnabled ? TEXT("ON") : TEXT("OFF"),
		Scale,
		(Profile != nullptr) ? *Profile->GetName() : TEXT("<none>"));

	const FString ShotLine = FString::Printf(
		TEXT("  ShotIndex=%d   Kick V=%.3f H=%.3f"),
		State.ShotIndex,
		State.LastVerticalKick,
		State.LastHorizontalKick);

	const FString AccumLine = FString::Printf(
		TEXT("  Accum Pitch=%.3f Yaw=%.3f   Cam Pitch=%.3f Yaw=%.3f"),
		State.AccumulatedPitch,
		State.AccumulatedYaw,
		State.GetCameraPitchOffset(),
		State.GetCameraYawOffset());

	// 单发模型一行：模式 / 阶段 / 本帧子步数 / 阶段进度。
	// 插值模式下这几个量是排查"轨迹为什么和预期不一样"的第一现场：
	//   - Mode    ：确认这把枪配的是哪套模型（同一个 HUD 就能对比两把枪）
	//   - Stage   ：当前处于 Lift / Rebound / Settle / Drop 哪一段
	//   - SubSteps：本帧实际推进了几个固定子步（>1 说明帧率偏低；
	//               触到上限 max 说明本帧发生了卡顿，时间被丢弃）
	const FString InterpLine = FString::Printf(
		TEXT("  Mode=%s   Stage=%s   SubSteps=%d   StageT=%.4f"),
		(Profile != nullptr && Profile->IsInterpolatedSingleShot()) ? TEXT("Interpolated") : TEXT("InstantWrite"),
		LyraRecoilDebugPrivate::GetInterpStageName(State.InterpStage),
		State.LastSubStepCount,
		State.StageElapsed);

	const FString StateLine = FString::Printf(
		TEXT("  State=%s   RecoveryTimer t=%.3f / delay=%.3f / prog=%.3f"),
		LyraRecoilDebugPrivate::GetStateName(State.State),
		State.TimeSinceLastFire,
		(Profile != nullptr) ? Profile->RecoveryDelay : 0.0f,
		State.RecoveryProgress);

	const FString PoseLine = FString::Printf(
		TEXT("  Pose=%s x%.3f   Seed=%d   History=%d"),
		LyraRecoilDebugPrivate::GetPoseName(State.LastPoseState),
		State.CurrentPoseMultiplier,
		State.ActiveSeed,
		State.ShotHistory.Num());

	const FString PanelText = FString::Printf(
		TEXT("%s\n%s\n%s\n%s\n%s\n%s"),
		*Header, *ShotLine, *AccumLine, *InterpLine, *StateLine, *PoseLine);

	const FColor PanelColor = bEnabled ? FColor::Yellow : FColor::Silver;

	GEngine->AddOnScreenDebugMessage(
		static_cast<int32>(LyraRecoilDebugPrivate::PanelMessageKey),
		0.0f,
		PanelColor,
		PanelText);
#endif // !UE_BUILD_SHIPPING
}

//////////////////////////////////////////////////////////////////////////
// 可视化：Roll 震动实时面板
//////////////////////////////////////////////////////////////////////////

void ULyraRecoilDebug::DrawRollShakeDebugPanel(const UWorld* World, const ULyraRecoilProfile* Profile, const FRecoilRuntimeState& State)
{
#if !UE_BUILD_SHIPPING
	if (!LyraRecoilCVars::bRollDebugPanel || (World == nullptr) || (GEngine == nullptr))
	{
		return;
	}

	const FCameraRollShakeState& Roll = State.RollShake;
	const FCameraRollShakeParams& Params = Roll.ActiveParams;

	const bool bEnabled = LyraRecoilCVars::bRecoilEnabled;
	const float Scale = LyraRecoilCVars::RollShakeScale;
	const bool bProfileAllows = (Profile != nullptr) && Profile->bEnableRollShake;

	// --- 1) 头部：总开关 / 倍率 / 资产是否启用 ---
	const FString Header = FString::Printf(
		TEXT("[RollShake] %s  Scale=x%.2f  Profile=%s  AssetRoll=%s"),
		bEnabled ? TEXT("ON") : TEXT("OFF"),
		Scale,
		(Profile != nullptr) ? *Profile->GetName() : TEXT("<none>"),
		bProfileAllows ? TEXT("on") : TEXT("off"));

	// --- 2) 实时值 ---
	const float RemainingRatio = (Params.DurationSeconds > KINDA_SMALL_NUMBER)
		? FMath::Clamp(1.0f - Roll.ElapsedTime / Params.DurationSeconds, 0.0f, 1.0f)
		: 0.0f;

	const FString LiveLine = FString::Printf(
		TEXT("  Active=%s  Roll=%+.4f deg  Envelope=%.4f  t=%.4f/%.4f (left %.0f%%)"),
		Roll.bActive ? TEXT("yes") : TEXT("no"),
		Roll.CurrentRoll,
		Roll.CurrentEnvelope,
		Roll.ElapsedTime,
		Params.DurationSeconds,
		RemainingRatio * 100.0f);

	// --- 3) 本次生效参数（触发瞬间的快照，震动期间不变）---
	const FString ParamLine = FString::Printf(
		TEXT("  Params: Amp=%.3f deg  Period=%.4f s  Jitter=%.3f rad  EndRatio=%.2f  Curve=%s  Shot=%d"),
		Params.AmplitudeDegrees,
		Params.PeriodSeconds,
		Params.PhaseJitterRadians,
		Params.EndAmplitudeRatio,
		(Params.DecayCurve != nullptr) ? TEXT("yes") : TEXT("linear"),
		Roll.ShotIndex);

	const FString PhaseLine = FString::Printf(
		TEXT("  StartAmp=%.3f deg  PhaseOffset=%+.4f rad (%.1f deg)  Cycles≈%.2f"),
		Roll.StartAmplitude,
		Roll.PhaseOffset,
		FMath::RadiansToDegrees(Roll.PhaseOffset),
		(Params.PeriodSeconds > KINDA_SMALL_NUMBER) ? (Params.DurationSeconds / Params.PeriodSeconds) : 0.0f);

	// --- 4) 波形：把整段震动采样成 ASCII 折线，并用 '|' 标出当前时刻 ---
	//
	// 为什么值得做：Roll 的核心争议是"波形看起来对不对"（衰减够不够快、抖几次、
	// 回零是否干净）。把整条曲线摊开在屏幕上，比盯着一个会跳的数字有用得多。
	FString WaveLine;
	FString AxisLine;
	if (Params.DurationSeconds > KINDA_SMALL_NUMBER && Params.AmplitudeDegrees > KINDA_SMALL_NUMBER)
	{
		// 采样，并顺带求出峰值用于纵向归一化
		TArray<float> Samples;
		Samples.SetNumUninitialized(LyraRecoilDebugPrivate::RollCurveSamples);

		float Peak = KINDA_SMALL_NUMBER;
		for (int32 i = 0; i < LyraRecoilDebugPrivate::RollCurveSamples; ++i)
		{
			const float SampleTime = Params.DurationSeconds * (static_cast<float>(i) / static_cast<float>(LyraRecoilDebugPrivate::RollCurveSamples - 1));
			Samples[i] = ULyraCameraRollShake::EvaluateRollShake(Params, SampleTime);
			Peak = FMath::Max(Peak, FMath::Abs(Samples[i]));
		}

		// 当前时刻在采样序列里的下标
		const int32 CursorIndex = (Roll.bActive && Params.DurationSeconds > KINDA_SMALL_NUMBER)
			? FMath::Clamp(FMath::RoundToInt((Roll.ElapsedTime / Params.DurationSeconds) * (LyraRecoilDebugPrivate::RollCurveSamples - 1)), 0, LyraRecoilDebugPrivate::RollCurveSamples - 1)
			: INDEX_NONE;

		// 峰值映射成两行字符：正半轴在上、负半轴在下。
		// 用 '#' 描点、'|' 标当前时刻、'o' 两者重合。
		constexpr int32 HalfHeight = 3;
		constexpr TCHAR Chars[] = { TEXT(' '), TEXT('.'), TEXT(':'), TEXT('|') };

		TArray<FString> UpperRows;
		TArray<FString> LowerRows;
		UpperRows.SetNum(HalfHeight);
		LowerRows.SetNum(HalfHeight);

		for (int32 i = 0; i < LyraRecoilDebugPrivate::RollCurveSamples; ++i)
		{
			const float Normalized = Samples[i] / Peak;	// [-1, 1]
			const bool bIsCursor = (i == CursorIndex);

			auto PlotInto = [&](TArray<FString>& Rows, bool bPositiveHalf)
			{
				const float Value = bPositiveHalf ? Normalized : -Normalized;
				if (Value <= 0.0f)
				{
					for (int32 Row = 0; Row < HalfHeight; ++Row)
					{
						Rows[Row].AppendChar(bIsCursor ? TEXT('|') : TEXT(' '));
					}
					return;
				}

				// 落在第几行：值越大越靠近外沿
				const int32 Row = FMath::Clamp(FMath::FloorToInt(Value * HalfHeight), 0, HalfHeight - 1);
				const int32 Density = FMath::Clamp(FMath::RoundToInt(Value * 3.0f), 1, 3);

				for (int32 R = 0; R < HalfHeight; ++R)
				{
					// Row 0 是外沿（远离中轴），HalfHeight-1 是靠近中轴
					const bool bOnRow = (HalfHeight - 1 - R) == Row;
					if (bOnRow)
					{
						Rows[R].AppendChar(bIsCursor ? TEXT('|') : Chars[Density]);
					}
					else
					{
						Rows[R].AppendChar(bIsCursor ? TEXT('|') : TEXT(' '));
					}
				}
			};

			PlotInto(UpperRows, /*bPositiveHalf=*/ true);
			PlotInto(LowerRows, /*bPositiveHalf=*/ false);
		}

		FString Wave;
		for (int32 Row = 0; Row < HalfHeight; ++Row)
		{
			Wave += TEXT("  ") + UpperRows[Row] + TEXT("\n");
		}
		Wave += TEXT("  ") + FString::ChrN(LyraRecoilDebugPrivate::RollCurveSamples, TEXT('-')) + TEXT("  0\n");
		for (int32 Row = HalfHeight - 1; Row >= 0; --Row)
		{
			Wave += TEXT("  ") + LowerRows[Row] + TEXT("\n");
		}

		WaveLine = FString::Printf(
			TEXT("  Waveform t=0..%.3fs  peak=%.3f deg  (row density = amplitude, '|' = now)\n%s"),
			Params.DurationSeconds,
			Peak,
			*Wave);
	}
	else
	{
		WaveLine = TEXT("  Waveform: <no active roll shake>");
	}

	const FString PanelText = FString::Printf(
		TEXT("%s\n%s\n%s\n%s\n%s"),
		*Header, *LiveLine, *ParamLine, *PhaseLine, *WaveLine);

	const FColor PanelColor = (!bEnabled || !bProfileAllows)
		? FColor::Silver
		: (Roll.bActive ? FColor::Magenta : FColor::Silver);

	GEngine->AddOnScreenDebugMessage(
		static_cast<int32>(LyraRecoilDebugPrivate::RollPanelMessageKey),
		0.0f,
		PanelColor,
		PanelText);
#endif // !UE_BUILD_SHIPPING
}

//////////////////////////////////////////////////////////////////////////
// 可视化：世界内 DebugDraw（P5）
//////////////////////////////////////////////////////////////////////////

void ULyraRecoilDebug::DrawWorldDebug(const UWorld* World, const FVector& Origin, const FRotator& AimRotation, const ULyraRecoilProfile* Profile, const FRecoilRuntimeState& State)
{
#if ENABLE_DRAW_DEBUG
	// LyraGame.Build.cs 里定义了 SHIPPING_DRAW_DEBUG_ERROR=1 —— DrawDebug 系列一旦
	// 出现在 #if ENABLE_DRAW_DEBUG 之外就是编译错误，所以整段必须被夹在这里面。
	if (!LyraRecoilCVars::bDebugDraw || (World == nullptr))
	{
		return;
	}

	const float TraceLength = LyraRecoilDebugPrivate::DebugDrawTraceLength;

	// --- 1) 真实瞄准轴（绿）：ControlRotation，永远不受后坐力影响 ---
	// 这条线的存在本身就是"相机偏移没碰 ControlRotation"的可视证据。
	const FVector AimDir = AimRotation.Vector();
	DrawDebugLine(World, Origin, Origin + AimDir * TraceLength, FColor::Green, false, 0.0f, 0, 1.5f);

	// --- 2) 相机链当前偏移方向（红）：玩家画面上实际看到的那条 ---
	const FRotator OffsetRotation = AimRotation + FRotator(State.AccumulatedPitch, State.AccumulatedYaw, 0.0f);
	const FVector CameraDir = OffsetRotation.Vector();
	DrawDebugLine(World, Origin, Origin + CameraDir * TraceLength, FColor::Red, false, 0.0f, 0, 2.5f);
	DrawDebugPoint(World, Origin + CameraDir * TraceLength, 12.0f, FColor::Red, false, 0.0f);

	// --- 3) Pattern 点阵（黄）：前 PatternLength 发的弹道方向 ---
	if (Profile != nullptr)
	{
		for (int32 ShotIndex = 0; ShotIndex < Profile->PatternLength; ++ShotIndex)
		{
			const FRecoilShotKick Kick = FRecoilRuntimeState::ComputeShotKick(
				*Profile, ShotIndex, 1.0f, 1.0f, State.ActiveSeed);

			const FRotator ShotRotation = AimRotation + FRotator(Kick.Vertical, Kick.Horizontal, 0.0f);
			DrawDebugPoint(World, Origin + ShotRotation.Vector() * TraceLength, 6.0f, FColor::Yellow, false, 0.0f);
		}
	}

	// --- 4) 回正进度（青条）：底端为起点，满刻度 100cm ---
	constexpr float ProgressBarLength = 100.0f;
	const FVector BarBase = Origin + FVector(0.0f, 0.0f, 60.0f);
	const float Progress = FMath::Clamp(State.RecoveryProgress, 0.0f, 1.0f);

	DrawDebugLine(World, BarBase, BarBase + FVector(0.0f, 0.0f, ProgressBarLength), FColor::Silver, false, 0.0f, 0, 2.0f);
	DrawDebugLine(World, BarBase, BarBase + FVector(0.0f, 0.0f, ProgressBarLength * Progress), FColor::Cyan, false, 0.0f, 0, 3.0f);

	// --- 5) Roll 震动横滚示意（品红弧）---
	//
	// Roll 是绕视线轴的旋转，在 3D 里没法用一条"方向线"表达（它不改朝向，只改倾斜）。
	// 所以改用**绕瞄准轴的一段圆弧**来示意：弧通过相机上方向量倾斜 RollShake 角后的位置，
	// 弧长随角度增大 —— 看弧的倾角就等于看当前 Roll。
	const FRecoilRuntimeState& RollState = State;
	if (RollState.RollShake.bActive && !FMath::IsNearlyZero(RollState.RollShake.CurrentRoll, 1e-4f))
	{
		const float RollDegrees = RollState.RollShake.CurrentRoll;
		const float ArcRadius = 160.0f;

		// 以瞄准方向为轴，画一小段垂直于视线的圆弧（用两个关键方向点连成折线近似）
		const FVector AimDir2 = AimRotation.Vector();
		const FVector RightAxis = FRotationMatrix(AimRotation).GetUnitAxis(EAxis::Y);
		const FVector UpAxis = FRotationMatrix(AimRotation).GetUnitAxis(EAxis::Z);

		// 基准"上"方向，以及倾斜 Roll 度之后的"上"方向
		const FVector TiltedUp = UpAxis.RotateAngleAxis(RollDegrees, AimDir2);

		const FVector ArcCenter = Origin + AimDir2 * 120.0f;
		constexpr int32 ArcSteps = 12;
		FVector PrevPoint = ArcCenter + UpAxis * ArcRadius;
		for (int32 Step = 1; Step <= ArcSteps; ++Step)
		{
			const float Alpha = static_cast<float>(Step) / static_cast<float>(ArcSteps);
			const FVector Dir = FMath::Lerp(UpAxis, TiltedUp, Alpha).GetSafeNormal();
			const FVector Point = ArcCenter + Dir * ArcRadius;
			DrawDebugLine(World, PrevPoint, Point, FColor::Magenta, false, 0.0f, 0, 2.5f);
			PrevPoint = Point;
		}

		// 倾斜后的"上"方向本身也画一条短轴，便于看清横滚姿态
		DrawDebugLine(World,
			ArcCenter,
			ArcCenter - AimDir2 * 0.0f + TiltedUp * (ArcRadius + 40.0f),
			FColor::Magenta, false, 0.0f, 0, 2.0f);
		DrawDebugPoint(World, ArcCenter + TiltedUp * (ArcRadius + 40.0f), 10.0f, FColor::Magenta, false, 0.0f);

		// 顺带把 Right 轴也画出来做水平参照（白细线）
		DrawDebugLine(World, ArcCenter, ArcCenter + RightAxis * ArcRadius, FColor::White, false, 0.0f, 0, 1.0f);
	}

	// --- 6) 文字标签 ---
	const FString Label = FString::Printf(
		TEXT("Recoil %s  Shot=%d  Pitch=%.2f Yaw=%.2f  Roll=%+.3f  Prog=%.2f  Pose=%s"),
		LyraRecoilDebugPrivate::GetStateName(State.State),
		State.ShotIndex,
		State.AccumulatedPitch,
		State.AccumulatedYaw,
		State.GetCameraRollOffset(),
		Progress,
		LyraRecoilDebugPrivate::GetPoseName(State.LastPoseState));

	DrawDebugString(World, BarBase + FVector(0.0f, 0.0f, ProgressBarLength + 20.0f), Label, nullptr, FColor::Cyan, 0.0f, /*bDrawShadow=*/ true);
#endif // ENABLE_DRAW_DEBUG
}

//////////////////////////////////////////////////////////////////////////
// CSV 导出（P5）
//////////////////////////////////////////////////////////////////////////

bool ULyraRecoilDebug::DumpShotHistoryToCsv(const ULyraRecoilProfile* Profile, const FRecoilRuntimeState& State, FString& OutFilePath)
{
	OutFilePath.Reset();

	const TArray<FRecoilShotResult>& History = State.ShotHistory;
	if (History.Num() == 0)
	{
		UE_LOG(LogLyraRecoilDebug, Warning,
			TEXT("Recoil dump skipped: shot history is empty (fire at least one shot first, and remember the history resets when a new burst starts)"));
		return false;
	}

	FString Csv;
	Csv.Reserve(64 + History.Num() * 64);

	// 表头顺序（**契约**：改动须同步所有解析脚本与 Golden 数据）
	// 前 6 列 = FRecoilShotResult 字段顺序；第 7 列 RollShake 来自运行时状态的瞬时解。
	Csv += TEXT("ShotIndex,VerticalKick,HorizontalKick,AccumulatedPitch,AccumulatedYaw,TimeSinceFire,RollShake\n");

	// Roll 是"每发重置时钟"的独立通道，历史里没有它 —— 但可以把每发开火瞬间的
	// 解析解回放出来（ShotIndex 是确定性的），这样 CSV 能完整描述一次连发的三轴表现。
	for (const FRecoilShotResult& Shot : History)
	{
		float RollAtFire = 0.0f;
		if ((Profile != nullptr) && Profile->bEnableRollShake)
		{
			FCameraRollShakeParams RollParams;
			// 用与运行时同一口径装配参数：姿态倍率/全局倍率在历史里没存，
			// 取 1.0 表示"标称值"，与 Dump 用于比对设计值的目的相符。
			if (Profile->BuildRollShakeParams(Shot.ShotIndex, 1.0f, 1.0f, State.ActiveSeed, RollParams))
			{
				RollAtFire = ULyraCameraRollShake::EvaluateRollShake(RollParams, 0.0f);
			}
		}

		Csv += FString::Printf(
			TEXT("%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n"),
			Shot.ShotIndex,
			Shot.VerticalKick,
			Shot.HorizontalKick,
			Shot.AccumulatedPitch,
			Shot.AccumulatedYaw,
			Shot.TimeSinceFire,
			RollAtFire);
	}

	// 带毫秒，避免同一秒内连续导出两次互相覆盖。
	//
	// 但毫秒还不够 —— 自动化测试里两次 Dump 可能落在同一毫秒（实测撞过：文件名相同 →
	// 第二次直接覆盖第一次 → Lyra.Recoil.Scale.AffectsDump 断言"两个文件路径不同"失败，
	// 随后 17 条 half 断言连锁失败）。所以再加一道"目标文件已存在就顺延编号"的兜底，
	// 让「每次 Dump 产出一个独立文件」成为硬契约。
	const FDateTime Now = FDateTime::Now();
	const FString Timestamp = FString::Printf(TEXT("%s_%03d"), *Now.ToString(TEXT("%Y%m%d_%H%M%S")), Now.GetMillisecond());

	const FString DumpDir = FPaths::ProjectSavedDir();
	FString FilePath = DumpDir / FString::Printf(TEXT("RecoilDump_%s.csv"), *Timestamp);
	for (int32 Suffix = 2; FPaths::FileExists(FilePath); ++Suffix)
	{
		FilePath = DumpDir / FString::Printf(TEXT("RecoilDump_%s_%d.csv"), *Timestamp, Suffix);
	}

	if (!FFileHelper::SaveStringToFile(Csv, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogLyraRecoilDebug, Error, TEXT("Failed to write recoil dump: %s"), *FilePath);
		return false;
	}

	OutFilePath = FilePath;
	LyraRecoilDebugPrivate::LastDumpPath = FilePath;

	UE_LOG(LogLyraRecoilDebug, Display, TEXT("Recoil dump written: %s (profile=%s shots=%d)"),
		*FilePath,
		(Profile != nullptr) ? *Profile->GetName() : TEXT("<none>"),
		History.Num());

	return true;
}

//////////////////////////////////////////////////////////////////////////
// 世界查询（P5）
//////////////////////////////////////////////////////////////////////////

ULyraRangedWeaponInstance* ULyraRecoilDebug::FindLocalPlayerRangedWeapon(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (PC == nullptr)
	{
		return nullptr;
	}

	APawn* Pawn = PC->GetPawn();
	if (Pawn == nullptr)
	{
		return nullptr;
	}

	ULyraEquipmentManagerComponent* EquipmentManager = Pawn->FindComponentByClass<ULyraEquipmentManagerComponent>();
	if (EquipmentManager == nullptr)
	{
		return nullptr;
	}

	// 与 LyraWeaponStateComponent.cpp L36 取"当前武器"的方式保持一致
	return Cast<ULyraRangedWeaponInstance>(
		EquipmentManager->GetFirstInstanceOfType(ULyraRangedWeaponInstance::StaticClass()));
}

//////////////////////////////////////////////////////////////////////////
// 动作型控制台命令（P5）
//
// Enable/Scale/Debug/DebugDraw 是"变量型"，用 FAutoConsoleVariableRef；
// Dump/ReloadProfile 需要在执行时拿到世界，所以用 FAutoConsoleCommandWithWorldAndArgs。
//////////////////////////////////////////////////////////////////////////

namespace LyraRecoilConsole
{
	static void HandleDump(const TArray<FString>& Args, UWorld* World)
	{
		ULyraRangedWeaponInstance* Weapon = ULyraRecoilDebug::FindLocalPlayerRangedWeapon(World);
		if (Weapon == nullptr)
		{
			UE_LOG(LogLyraRecoilDebug, Warning,
				TEXT("Lyra.Recoil.Dump: no locally controlled ranged weapon found (dedicated server / no pawn / weapon not equipped)"));
			return;
		}

		FString FilePath;
		if (ULyraRecoilDebug::DumpShotHistoryToCsv(Weapon->GetRecoilProfile(), Weapon->GetRecoilState(), FilePath))
		{
			UE_LOG(LogLyraRecoilDebug, Display, TEXT("Lyra.Recoil.Dump: %s"), *FilePath);
		}
	}

	static void HandleReloadProfile(const TArray<FString>& Args, UWorld* World)
	{
		ULyraRangedWeaponInstance* Weapon = ULyraRecoilDebug::FindLocalPlayerRangedWeapon(World);
		if (Weapon == nullptr)
		{
			UE_LOG(LogLyraRecoilDebug, Warning,
				TEXT("Lyra.Recoil.ReloadProfile: no locally controlled ranged weapon found"));
			return;
		}

		Weapon->ReloadRecoilProfile();
	}

	static FAutoConsoleCommandWithWorldAndArgs DumpCommand(
		TEXT("Lyra.Recoil.Dump"),
		TEXT("Export the current burst's shot history of the local player's ranged weapon to Saved/RecoilDump_<timestamp>.csv"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HandleDump));

	static FAutoConsoleCommandWithWorldAndArgs ReloadProfileCommand(
		TEXT("Lyra.Recoil.ReloadProfile"),
		TEXT("Force-reload the local player's recoil profile asset from disk. For tuning when the asset was changed outside the editor (editor / PIE only)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&HandleReloadProfile));
}
