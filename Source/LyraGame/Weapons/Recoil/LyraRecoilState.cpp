// Copyright Epic Games, Inc. All Rights Reserved.

#include "Weapons/Recoil/LyraRecoilState.h"

#include "Camera/LyraCameraRollShake.h"
#include "Math/RandomStream.h"
#include "Templates/TypeHash.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LyraRecoilState)

namespace LyraRecoilStatePrivate
{
	/**
	 * 与调用顺序无关的确定性随机采样，取值 [-1, 1]。
	 *
	 * 关键点：用 (Seed, StepIndex) 的哈希**逐发**新建随机流，而不是复用同一个
	 * 有状态随机流。这样第 N 步的取值只取决于 (Seed, N)，不受"之前调过几次"影响，
	 * Golden 数据比对才能成立（开发计划 §3.3 决策 4/5）。
	 */
	static float DeterministicSignedRandom(int32 Seed, int32 Index)
	{
		const uint32 Combined = HashCombine(static_cast<uint32>(Seed), static_cast<uint32>(Index));
		FRandomStream Stream(static_cast<int32>(Combined));
		return Stream.FRandRange(-1.0f, 1.0f);
	}

	/**
	 * 冻结当前压枪量为「回正快照」。
	 *
	 * 为什么必须冻结：回正目标是 `f(峰值, 压枪量)`。如果回正期间还继续用**实时**压枪量，
	 * 玩家手指在恢复过程中再动一下，目标值就会跟着走 —— 表现为回正在半路突然改向。
	 * 冻结之后回正是一条确定的曲线，可被自动化测试逐点断言。
	 *
	 * 三条进入回正的路径都必须调用它：
	 *   1. 非插值模式：Accumulating → Recovering
	 *   2. 插值模式：Settle → Drop（Drop 段就是回正本身）
	 *   3. 长帧保护：时间被丢弃后直接跳到稳态残留
	 */
	static void FreezeCompensationForRecovery(FRecoilRuntimeState& RecoilState)
	{
		RecoilState.RecoveryCompensationPitch = RecoilState.PlayerCompensationPitch;
		RecoilState.RecoveryCompensationYaw = RecoilState.PlayerCompensationYaw;
	}

	/** 推进一次回正插值。回正目标 = 峰值 × 残留比，再扣掉压枪量（见 ComputeRecoveryTarget）。 */
	static void ApplyRecoveryStep(FRecoilRuntimeState& RecoilState, const ULyraRecoilProfile& Profile)
	{
		const float Duration = FMath::Max(Profile.RecoveryTime, KINDA_SMALL_NUMBER);
		const float NormalizedTime = FMath::Clamp(RecoilState.RecoveryElapsed / Duration, 0.0f, 1.0f);
		const float Alpha = Profile.GetRecoveryAlpha(NormalizedTime);

		const float TargetPitch = FRecoilRuntimeState::ComputeRecoveryTarget(
			Profile, RecoilState.RecoveryPeakPitch, RecoilState.RecoveryCompensationPitch);
		const float TargetYaw = FRecoilRuntimeState::ComputeRecoveryTarget(
			Profile, RecoilState.RecoveryPeakYaw, RecoilState.RecoveryCompensationYaw);

		RecoilState.RecoveryProgress = Alpha;
		RecoilState.AccumulatedPitch = FMath::Lerp(RecoilState.RecoveryPeakPitch, TargetPitch, Alpha);
		RecoilState.AccumulatedYaw = FMath::Lerp(RecoilState.RecoveryPeakYaw, TargetYaw, Alpha);

		// ---------------------------------------------------------------------
		// 回正阶段也要维护补间输出，但注意两套模式的口径不同：
		//
		//   InstantWrite ：相机直接读逻辑偏移，所以补间输出恒等拷贝过去即可。
		//                  这就是「切模式时既有行为零变化」的保证点。
		//
		//   Interpolated ：**绝不能被覆盖成逻辑偏移**，否则整个插值链在回正阶段崩掉。
		//                  它的推进由 AdvanceInterpolatedStages() 的「目标值差分」负责，
		//                  这里什么都不用做。
		// ---------------------------------------------------------------------
		if (!Profile.IsInterpolatedSingleShot())
		{
			RecoilState.CameraOffsetPitch = RecoilState.AccumulatedPitch;
			RecoilState.CameraOffsetYaw = RecoilState.AccumulatedYaw;
		}

		if (NormalizedTime >= 1.0f)
		{
			// 回正结束：落到稳态偏移（RecoilReturnRatio 决定的残留），回到 Idle，准备下一轮连发
			RecoilState.AccumulatedPitch = TargetPitch;
			RecoilState.AccumulatedYaw = TargetYaw;
			RecoilState.RecoveryProgress = 1.0f;
			RecoilState.State = ERecoilState::Idle;
			RecoilState.ShotIndex = 0;
			RecoilState.RecoveryElapsed = 0.0f;
			RecoilState.TimeSinceLastFire = 0.0f;
			RecoilState.LastVerticalKick = 0.0f;
			RecoilState.LastHorizontalKick = 0.0f;

			// 插值链一并收尾：补间输出落到稳态残留，阶段时间轴清空。
			// 不这样做的话，补间输出会永远停在「下降段最后一帧」的值上，
			// 与逻辑偏移产生偏差，下一轮连发就会从错误的位置开始抬。
			RecoilState.InterpStage = ERecoilInterpStage::None;
			RecoilState.StageElapsed = 0.0f;
			RecoilState.SubStepAccumulator = 0.0f;
			RecoilState.CameraOffsetPitch = TargetPitch;
			RecoilState.CameraOffsetYaw = TargetYaw;
			RecoilState.LastTargetPitch = TargetPitch;
			RecoilState.LastTargetYaw = TargetYaw;
		}
	}

	/**
	 * 本阶段的总时长（秒）。
	 *
	 * 三段式的时长映射：
	 *   Lift    → LiftDuration      （新增参数）
	 *   Rebound → ReboundDuration   （新增参数）
	 *   Settle  → RecoveryDelay     （复用现有参数，刻意不引入第二个语义重复的旋钮）
	 *   Drop    → RecoveryTime      （复用现有参数）
	 */
	static float GetStageDuration(const ULyraRecoilProfile& Profile, ERecoilInterpStage Stage)
	{
		switch (Stage)
		{
		case ERecoilInterpStage::Lift:		return FMath::Max(0.0f, Profile.LiftDuration);
		case ERecoilInterpStage::Rebound:	return FMath::Max(0.0f, Profile.ReboundDuration);
		case ERecoilInterpStage::Settle:	return FMath::Max(0.0f, Profile.RecoveryDelay);
		case ERecoilInterpStage::Drop:		return FMath::Max(KINDA_SMALL_NUMBER, Profile.RecoveryTime);
		case ERecoilInterpStage::None:
		default:							return 0.0f;
		}
	}

	/** 阶段推进顺序：Lift → Rebound → Settle → Drop → (结束)。 */
	static ERecoilInterpStage GetNextStage(ERecoilInterpStage Stage)
	{
		switch (Stage)
		{
		case ERecoilInterpStage::Lift:		return ERecoilInterpStage::Rebound;
		case ERecoilInterpStage::Rebound:	return ERecoilInterpStage::Settle;
		case ERecoilInterpStage::Settle:	return ERecoilInterpStage::Drop;
		case ERecoilInterpStage::Drop:
		case ERecoilInterpStage::None:
		default:							return ERecoilInterpStage::None;
		}
	}

	/**
	 * 算出当前阶段「此刻应该达到的目标偏移」（度）。
	 *
	 * 这是参考文档 §2 伪码 `目标Pitch = 按上抬曲线插值(0, Pitch总幅度, 进度)` 的落地。
	 * 注意它是**绝对目标值**（含本发起始的逻辑偏移作为基底），而不是增量 ——
	 * 「帧间增量」由调用方对它做差得到，这正是文档要求的「目标值 → 帧间增量」范式。
	 */
	static void ComputeStageTarget(
		const FRecoilRuntimeState& State,
		const ULyraRecoilProfile& Profile,
		float& OutPitch,
		float& OutYaw)
	{
		// 本发的关键锚点：
		//   Base       —— 开火那一刻的逻辑偏移，是本发所有阶段的「地面」
		//   Peak       —— Base + 完整幅度，上抬段的终点
		//   ReboundEnd —— 从 Peak 回弹到 Peak × ReboundRatio
		//   SteadyEnd  —— 从 ReboundEnd 下降到 Peak × RecoilReturnRatio（残留在世界的最终值）
		const float BasePitch = State.InterpBasePitch;
		const float BaseYaw = State.InterpBaseYaw;
		const float PeakPitch = BasePitch + State.InterpShotAmplitudePitch;
		const float PeakYaw = BaseYaw + State.InterpShotAmplitudeYaw;

		const float ReboundEndPitch = PeakPitch * Profile.ReboundRatio;
		const float ReboundEndYaw = PeakYaw * Profile.ReboundRatio;

		// 稳态终点同样要扣压枪量：Drop 段就是"回正"，它的终点必须与
		// ApplyRecoveryStep 收尾时落的值**完全一致**，否则在 Drop 结束那一帧会跳一下。
		// 压枪量在 Settle → Drop 的切换处冻结（见 AdvanceInterpolatedSubStepBy）。
		const float SteadyEndPitch = FRecoilRuntimeState::ComputeRecoveryTarget(
			Profile, PeakPitch, State.RecoveryCompensationPitch);
		const float SteadyEndYaw = FRecoilRuntimeState::ComputeRecoveryTarget(
			Profile, PeakYaw, State.RecoveryCompensationYaw);

		const float Duration = GetStageDuration(Profile, State.InterpStage);
		const float Progress = (Duration > KINDA_SMALL_NUMBER)
			? FMath::Clamp(State.StageElapsed / Duration, 0.0f, 1.0f)
			: 1.0f;

		switch (State.InterpStage)
		{
		case ERecoilInterpStage::Lift:
		{
			// 从 Base 抬到 Peak，形状走上抬曲线
			const float Alpha = Profile.GetLiftAlpha(Progress);
			OutPitch = FMath::Lerp(BasePitch, PeakPitch, Alpha);
			OutYaw = FMath::Lerp(BaseYaw, PeakYaw, Alpha);
			break;
		}

		case ERecoilInterpStage::Rebound:
		{
			// 从 Peak 回弹到 ReboundEnd，形状走回弹曲线
			const float Alpha = Profile.GetReboundAlpha(Progress);
			OutPitch = FMath::Lerp(PeakPitch, ReboundEndPitch, Alpha);
			OutYaw = FMath::Lerp(PeakYaw, ReboundEndYaw, Alpha);
			break;
		}

		case ERecoilInterpStage::Settle:
		{
			// 稳定段：目标值恒为回弹终点。不需要曲线 —— 目标不变，帧间增量自然为 0。
			OutPitch = ReboundEndPitch;
			OutYaw = ReboundEndYaw;
			break;
		}

		case ERecoilInterpStage::Drop:
		{
			// 从回弹终点下降到稳态残留，形状走回正曲线（复用现有 RecoveryCurve）
			const float Alpha = Profile.GetRecoveryAlpha(Progress);
			OutPitch = FMath::Lerp(ReboundEndPitch, SteadyEndPitch, Alpha);
			OutYaw = FMath::Lerp(ReboundEndYaw, SteadyEndYaw, Alpha);
			break;
		}

		case ERecoilInterpStage::None:
		default:
		{
			OutPitch = State.CameraOffsetPitch;
			OutYaw = State.CameraOffsetYaw;
			break;
		}
	}
	}

	/**
	 * 执行一次子步进，默认步长 1/60 秒。
	 *
	 *   1. 先做跨界钳制推进阶段时间（「预计时间会跨过阶段终点 → 钳到终点」）
	 *   2. 再按新的阶段/时刻算目标值，取帧间增量推给相机补间输出
	 *   3. 最后检查是否该切阶段
	 *
	 * 常规子步**只推固定步长**，不跳步 —— 这是「采样密度与帧率解耦」的实现点。
	 * 唯一例外是尾料冲刷时传入的实际零头（见 Advance 里的 Residual Flush），
	 * 那个步长只可能**小于** 1/60，且只在一帧的最后发生一次。
	 */
	static void AdvanceInterpolatedSubStepBy(
		FRecoilRuntimeState& State,
		const ULyraRecoilProfile& Profile,
		float StepSeconds)
	{
		if (State.InterpStage == ERecoilInterpStage::None)
		{
			return;
		}

		// --- 1) 推进本阶段时间，含跨界钳制 ---
		//
		// 参考文档 §2 原话：
		//   「阶段结束时可能出现离散时间跨越边界的情况，例如某阶段目标时长为 0.06 s，
		//     而固定步长无法刚好命中该时刻。如果不处理，插值曲线可能无法完整跑到终点。
		//     常见处理方式是在即将跨越阶段终点时，将累计时间钳制到阶段终点，
		//     确保曲线能取到 100% 的终值。」
		//
		// 注意：子步长是固定的 1/60，所以这里的钳制与「本帧 DeltaSeconds 多大」无关 ——
		// 正因如此，同一份配置在任何帧率下都会在同一处被钳制，轨迹才会逐点一致。
		//
		// 尾段吸附（Tail Snap）—— 这里是「阶段终值必然被采样到」的保证点：
		//
		// 子步长 1/60 与阶段时长之间通常不是整数倍关系（例如 0.05s 只能容纳
		// 2.9999 个 1/60，float32 下更只剩 2 个整步），若只做"钳到不超过终点"，
		// 那么进度会停在 2/3 处：**终值 100% 永远不会被取到**。
		// 表现为上抬永远抬不到位、回弹永远弹不到点 —— 幅度整体缩水。
		//
		// 因此这里改成：只走一步就会跨过终点时，直接把这一步走到终点（吸附），
		// 而不是先停在前一格、再靠下一步的 Overflow 去补。
		// 吸附与帧率无关（只取决于固定步长与阶段时长），所以帧率不变性不受影响。
		const float StageDuration = GetStageDuration(Profile, State.InterpStage);
		const float Advanced = State.StageElapsed + StepSeconds;

		// 距终点已不足一个子步 → 直接吸附到终点，确保终值被采到
		if (Advanced < StageDuration && (StageDuration - Advanced) < StepSeconds)
		{
			State.StageElapsed = StageDuration;
		}
		else
		{
			State.StageElapsed = FMath::Min(Advanced, StageDuration);
		}

		// --- 2) 按当前时刻算目标值，取增量推给相机 ---
		float TargetPitch = 0.0f;
		float TargetYaw = 0.0f;
		ComputeStageTarget(State, Profile, TargetPitch, TargetYaw);

		// 「本帧增量 = 目标值 − 上一帧目标值」—— 参考文档 §2 伪码的核心两行。
		// 之所以推增量而不是直接写绝对值：相机上玩家自己的鼠标输入也在累积，
		// 直接设绝对值会把玩家输入冲掉。
		State.CameraOffsetPitch += (TargetPitch - State.LastTargetPitch);
		State.CameraOffsetYaw += (TargetYaw - State.LastTargetYaw);

		State.LastTargetPitch = TargetPitch;
		State.LastTargetYaw = TargetYaw;

		// 同步逻辑偏移：让它始终等于「本阶段当前的目标值」。
		// 这样峰值/回正/弹道链/CSV/Golden 读到的都是「这一刻后坐力应该在哪」，
		// 语义与 InstantWrite 模式完全一致 —— 两个模式只在**相机怎么跟上**这件事上不同。
		State.AccumulatedPitch = FMath::Clamp(TargetPitch, -Profile.MaxVerticalKick, Profile.MaxVerticalKick);
		State.AccumulatedYaw = FMath::Clamp(TargetYaw, -Profile.MaxHorizontalKick, Profile.MaxHorizontalKick);

		// --- 3) 阶段推进 ---
		if (State.StageElapsed >= StageDuration)
		{
			const ERecoilInterpStage NextStage = GetNextStage(State.InterpStage);

			if (NextStage == ERecoilInterpStage::None)
			{
				// 下降段走完：交回常规回正状态机做统一收尾。
				// 这里刻意不直接置 Idle —— 由 ApplyRecoveryStep 负责落稳态、
				// 清计数器、回 Idle，避免两处各写一遍收尾逻辑。
				//
				// ★ 关键：必须把 RecoveryPeakPitch 设成**本发峰值**，不能沿用旧值。
				// ApplyRecoveryStep 会算 `Target = 峰值 × 残留比 + 压枪量`，
				// 而此刻 AccumulatedPitch 已经是「峰值 × 回正比」的稳态值了。
				// 若让 RecoveryPeakPitch 停留在 ReboundEnd 之类的中间值上，
				// 回正比就会被**再乘一次**，最终残留值整体偏小（实测 0.075 而非 0.125）。
				const float PeakPitch = State.InterpBasePitch + State.InterpShotAmplitudePitch;
				const float PeakYaw = State.InterpBaseYaw + State.InterpShotAmplitudeYaw;

				State.InterpStage = ERecoilInterpStage::None;
				State.StageElapsed = 0.0f;
				State.RecoveryPeakPitch = PeakPitch;
				State.RecoveryPeakYaw = PeakYaw;
				State.RecoveryElapsed = Profile.RecoveryTime;
				State.State = ERecoilState::Recovering;
				ApplyRecoveryStep(State, Profile);
			}
			else
			{
				// 跨阶段时把超出部分**带过去**，而不是丢弃 ——
				// 丢弃会让每个阶段都少走一点时间，多段累积后终点就跑不满了。
				const float Overflow = State.StageElapsed - StageDuration;
				State.InterpStage = NextStage;
				State.StageElapsed = FMath::Min(Overflow, GetStageDuration(Profile, NextStage));

				// 进入 Drop（= 回正段）的那一刻冻结压枪量，Drop 段与随后的收官都用它。
				// 放在这里而不是"停火时"：本模式下 Settle 段就是 RecoveryDelay，
				// 它的结束正好是"停火超过 RecoveryDelay"的同一时刻，语义对齐。
				if (NextStage == ERecoilInterpStage::Drop)
				{
					FreezeCompensationForRecovery(State);
				}

				// 注意：切换阶段后**不重置 LastTargetPitch/Yaw**。
				// 这是保证「无跳变」的关键 —— 新阶段的起点目标值等于上一阶段的终点目标值
				// （Lift 终点 = Peak，Rebound 起点 = Peak；Rebound 终点 = ReboundEnd，
				//  Settle 起点 = ReboundEnd），所以差分为 0，补间输出连续。
				// 一旦在这里重置缓存，就会产生一个「从旧值跳回新值」的巨大阶跃。
			}
		}
	}

	/** 常规固定子步入口：步长恒为 FixedSubStepSeconds。 */
	static void AdvanceInterpolatedSubStep(FRecoilRuntimeState& State, const ULyraRecoilProfile& Profile)
	{
		AdvanceInterpolatedSubStepBy(State, Profile, FRecoilRuntimeState::FixedSubStepSeconds);
	}
}

int32 FRecoilRuntimeState::ResolveSeed(const ULyraRecoilProfile* Profile)
{
	if (Profile == nullptr)
	{
		return 0;
	}

	// Fixed：可复现，自动化测试与 Golden 数据必须走这条
	if (Profile->RandomSeedMode == ERecoilRandomSeedMode::Fixed)
	{
		return Profile->FixedRandomSeed;
	}

	// Random：每轮连发重新播种（不是每发），保证一轮之内弹道仍然连贯
	return FMath::Rand();
}

float FRecoilRuntimeState::ComputeRecoveryTarget(const ULyraRecoilProfile& Profile, float Peak, float Compensation)
{
	// 既有行为（不扣压枪量）：终止值 = 峰值 × 残留比
	const float LegacyTarget = Peak * Profile.RecoilReturnRatio;

	// 玩家压枪的方向与后坐力相反时 Compensation 为正（见 SamplePlayerAim 的符号约定）。
	// 关闭开关 / 没有玩家输入 → Compensation 为 0 → 严格退回旧公式。
	const float EffectiveCompensation = Profile.bCompensationAwareRecovery ? Compensation : 0.0f;
	if (EffectiveCompensation == 0.0f)
	{
		return LegacyTarget;
	}

	// 双端钳制：上界 = 峰值（回正量最多归零），下界 = 原本的回正目标（回正量最多是原本那么多）。
	// 用 Min/Max 而不是写死 0 —— 这样垂直（峰值恒 ≥ 0）与水平（峰值可为负）共用一条公式。
	const float LowerBound = FMath::Min(Peak, LegacyTarget);
	const float UpperBound = FMath::Max(Peak, LegacyTarget);

	return FMath::Clamp(LegacyTarget + EffectiveCompensation, LowerBound, UpperBound);
}

void FRecoilRuntimeState::SamplePlayerAim(float InAimPitchDegrees, float InAimYawDegrees)
{
	SampledAimPitch = InAimPitchDegrees;
	SampledAimYaw = InAimYawDegrees;

	// Idle 时基准（AimPitch/YawAtBurstStart）已经是一轮连发之前的旧值，算出来的差值没有意义。
	// 真正需要压枪量的时刻只有 Accumulating / Recovering 两态。
	if (State == ERecoilState::Idle)
	{
		PlayerCompensationPitch = 0.0f;
		PlayerCompensationYaw = 0.0f;
		return;
	}

	// NormalizeAxis 处理绕圈（-180/180 附近）—— 少了它，玩家转半圈会被算成"压枪 358°"。
	// 用 FRotator::NormalizeAxis（等价于 FRotator::NormalizeAxis 的静态入口，见相机修改器同款用法）。
	const float PitchDelta = FRotator::NormalizeAxis(SampledAimPitch - AimPitchAtBurstStart);
	const float YawDelta = FRotator::NormalizeAxis(SampledAimYaw - AimYawAtBurstStart);

	// 取负：玩家往下压（Pitch 减小）→ 压枪量为正；往左拉（Yaw 减小）→ 压枪量为正。
	PlayerCompensationPitch = -PitchDelta;
	PlayerCompensationYaw = -YawDelta;
}

EPoseState FRecoilRuntimeState::ResolvePoseState(bool bIsCrouching, bool bIsFalling)
{
	// 空中优先级最高：跳/落过程中不该因为"顺带蹲着"而拿到蹲伏的低后坐力
	if (bIsFalling)
	{
		return EPoseState::JumpingOrFalling;
	}

	if (bIsCrouching)
	{
		return EPoseState::Crouching;
	}

	return EPoseState::Standing;
}

float FRecoilRuntimeState::ComputePoseMultiplier(const ULyraRecoilProfile& Profile, EPoseState PoseState, float AimingAlpha)
{
	const float PoseMultiplier = Profile.GetPoseMultiplier(PoseState);
	const float AimingMultiplier = Profile.GetAimingBlendedMultiplier(AimingAlpha);

	return PoseMultiplier * AimingMultiplier;
}

void FRecoilRuntimeState::Reset(const ULyraRecoilProfile* Profile)
{
	ShotIndex = 0;
	AccumulatedPitch = 0.0f;
	AccumulatedYaw = 0.0f;
	CameraOffsetPitch = 0.0f;
	CameraOffsetYaw = 0.0f;
	RecoveryPeakPitch = 0.0f;
	RecoveryPeakYaw = 0.0f;
	RecoveryElapsed = 0.0f;
	RecoveryProgress = 0.0f;
	TimeSinceLastFire = 0.0f;
	CurrentPoseMultiplier = 1.0f;
	LastPoseState = EPoseState::Standing;
	LastVerticalKick = 0.0f;
	LastHorizontalKick = 0.0f;

	// 插值链：阶段时间轴与子步累加器一并清零。
	// 漏清 SubStepAccumulator 会让上一把枪残留的零头时间带到新枪的第一发上。
	InterpStage = ERecoilInterpStage::None;
	StageElapsed = 0.0f;
	InterpShotAmplitudePitch = 0.0f;
	InterpShotAmplitudeYaw = 0.0f;
	InterpBasePitch = 0.0f;
	InterpBaseYaw = 0.0f;
	LastTargetPitch = 0.0f;
	LastTargetYaw = 0.0f;
	SubStepAccumulator = 0.0f;
	LastSubStepCount = 0;

	// 压枪量链：基准与快照一并清零。
	// 漏清基准会让新枪的第一轮连发拿着上一把枪的瞄准算压枪量（压枪量会瞬间是一个离谱值）。
	PlayerCompensationPitch = 0.0f;
	PlayerCompensationYaw = 0.0f;
	RecoveryCompensationPitch = 0.0f;
	RecoveryCompensationYaw = 0.0f;
	AimPitchAtBurstStart = 0.0f;
	AimYawAtBurstStart = 0.0f;
	SampledAimPitch = 0.0f;
	SampledAimYaw = 0.0f;

	RollShake.Reset();
	State = ERecoilState::Idle;
	ActiveSeed = ResolveSeed(Profile);
	ShotHistory.Reset();
}

void FRecoilRuntimeState::ClearHistory()
{
	ShotHistory.Reset();
}

float FRecoilRuntimeState::ComputePatternHorizontal(const ULyraRecoilProfile& Profile, int32 ShotIndex, int32 Seed)
{
	const int32 ClampedIndex = FMath::Max(0, ShotIndex);

	if (Profile.IsFixedPatternShot(ClampedIndex))
	{
		// 固定区间：严格等于资产里配的值（P3 断言 #2 依据）
		return Profile.GetPatternPoint(ClampedIndex).X;
	}

	// 伪随机区间：从 PatternLength 起做确定性随机游走，逐步 Clamp 在 ±HorizontalRandomRange 内。
	// 循环是 O(ShotIndex)，但 ShotIndex 量级只有几十，且换来了"纯函数、可复现"这两个关键性质。
	const float Range = FMath::Max(0.0f, Profile.HorizontalRandomRange);

	float Walk = 0.0f;
	for (int32 StepIndex = Profile.PatternLength; StepIndex <= ClampedIndex; ++StepIndex)
	{
		Walk = FMath::Clamp(
			Walk + LyraRecoilStatePrivate::DeterministicSignedRandom(Seed, StepIndex) * Range,
			-Range, Range);
	}

	return Walk;
}

FRecoilShotKick FRecoilRuntimeState::ComputeShotKick(
	const ULyraRecoilProfile& Profile,
	int32 ShotIndex,
	float PoseMultiplier,
	float GlobalScale,
	int32 Seed)
{
	const int32 ClampedIndex = FMath::Max(0, ShotIndex);

	const FRecoilPatternPoint PatternPoint = Profile.GetPatternPoint(ClampedIndex);
	const float PatternHorizontal = ComputePatternHorizontal(Profile, ClampedIndex, Seed);
	const float KickCurveScale = Profile.GetVerticalKickCurveScale(ClampedIndex);
	const float Magnitude = FMath::Max(0.0f, PoseMultiplier) * FMath::Max(0.0f, GlobalScale);

	FRecoilShotKick Kick;
	Kick.Vertical = Profile.RecoilPerShot_Vertical * PatternPoint.Y * KickCurveScale * Magnitude;
	Kick.Horizontal = Profile.RecoilPerShot_Horizontal * PatternHorizontal * Magnitude;

	return Kick;
}

FRecoilShotKick FRecoilRuntimeState::ComputeShotKickGated(
	const ULyraRecoilProfile* Profile,
	int32 ShotIndex,
	float PoseMultiplier,
	float GlobalScale,
	int32 Seed,
	bool bRecoilEnabled)
{
	// 关闭或没配资产时必须是零偏移：Lyra 原有扩散逻辑不能被后坐力污染
	if (!bRecoilEnabled || (Profile == nullptr))
	{
		return FRecoilShotKick();
	}

	return ComputeShotKick(*Profile, ShotIndex, PoseMultiplier, GlobalScale, Seed);
}

bool FRecoilRuntimeState::ApplyShot(const ULyraRecoilProfile* Profile, float PoseMultiplier, EPoseState PoseState)
{
	if (Profile == nullptr)
	{
		return false;
	}

	// 上一轮已回正完成 → 这是一轮新的连发：索引归零、历史清空、按需换种子
	if (State == ERecoilState::Idle)
	{
		ShotIndex = 0;
		ShotHistory.Reset();
		ActiveSeed = ResolveSeed(Profile);

		// 压枪量以「本轮第一发的玩家瞄准」为基准重新起算。
		// 基准取最近一次采样值（武器实例每帧采样，所以最多滞后一帧）——
		// 这一帧的滞后在"往下压 4°"这种量级上完全看不出来，却让本层保持无 UWorld 依赖。
		AimPitchAtBurstStart = SampledAimPitch;
		AimYawAtBurstStart = SampledAimYaw;
		PlayerCompensationPitch = 0.0f;
		PlayerCompensationYaw = 0.0f;
		RecoveryCompensationPitch = 0.0f;
		RecoveryCompensationYaw = 0.0f;
	}

	const float TimeSincePreviousShot = TimeSinceLastFire;

	const FRecoilShotKick Kick = ComputeShotKick(*Profile, ShotIndex, PoseMultiplier, GlobalScale, ActiveSeed);

	LastVerticalKick = Kick.Vertical;
	LastHorizontalKick = Kick.Horizontal;
	CurrentPoseMultiplier = PoseMultiplier;
	LastPoseState = PoseState;

	if (Profile->IsInterpolatedSingleShot())
	{
		// ---------------------------------------------------------------------
		// 插值模式：把「本发要抬高多少」交给阶段时间轴，逻辑偏移由子步推进驱动。
		//
		// 关键点：**这里不直接累加 AccumulatedPitch**。
		// 原因是插值模式下逻辑偏移必须等于「本阶段当前的目标值」，否则积累的小误差
		// 会让峰值(回正基准)与补间输出对不上。真正写入发生在 AdvanceInterpolatedSubStep。
		//
		// Base 取「当前逻辑偏移」，所以连发时第二发是从第一发抬到的位置继续往上抬 ——
		// 这正是「连发持续被推高」观感的来源，也是补间输出无跳变的保证点。
		// ---------------------------------------------------------------------
		InterpBasePitch = AccumulatedPitch;
		InterpBaseYaw = AccumulatedYaw;
		InterpShotAmplitudePitch = Kick.Vertical;
		InterpShotAmplitudeYaw = Kick.Horizontal;

		InterpStage = ERecoilInterpStage::Lift;
		StageElapsed = 0.0f;

		// 缓存目标值必须**立刻**对齐到 Lift 起点（= Base），
		// 否则第一个子步的差分会算成「Base − 上一发遗留的目标值」，产生一个假阶跃。
		LastTargetPitch = InterpBasePitch;
		LastTargetYaw = InterpBaseYaw;

		// 子步累加器清零：新发的时间轴从零开始计时。
		// 保留旧零头会让第一发莫名其妙地多走一点时间（射速快时尤其明显）。
		SubStepAccumulator = 0.0f;
	}
	else
	{
		// ---------------------------------------------------------------------
		// 瞬时写入模式（默认）：开火帧直接把本发 Kick 写进累加偏移，上抬耗时 = 0 秒。
		//
		// 这是既有行为，一字未改 —— 既有 24 个测试、3 份 Golden 数据、CSV 契约
		// 全部建立在这个语义上。
		// ---------------------------------------------------------------------
		AccumulatedPitch = FMath::Clamp(AccumulatedPitch + Kick.Vertical, -Profile->MaxVerticalKick, Profile->MaxVerticalKick);
		AccumulatedYaw = FMath::Clamp(AccumulatedYaw + Kick.Horizontal, -Profile->MaxHorizontalKick, Profile->MaxHorizontalKick);

		// 补间输出恒等拷贝：相机链读的是 CameraOffsetPitch/Yaw，
		// 在瞬时写入模式下它必须与逻辑偏移完全一致，否则手感会凭空变化。
		CameraOffsetPitch = AccumulatedPitch;
		CameraOffsetYaw = AccumulatedYaw;
	}

	FRecoilShotResult Result;
	Result.ShotIndex = ShotIndex;
	Result.VerticalKick = Kick.Vertical;
	Result.HorizontalKick = Kick.Horizontal;
	Result.AccumulatedPitch = AccumulatedPitch;
	Result.AccumulatedYaw = AccumulatedYaw;
	Result.TimeSinceFire = TimeSincePreviousShot;
	Result.PoseState = PoseState;
	Result.PoseMultiplier = PoseMultiplier;
	ShotHistory.Add(Result);

	// ---------------------------------------------------------------------
	// Roll 震屏：独立通道，与本发的 Pitch/Yaw 累加互不影响。
	// 用本发的 ShotIndex 查连射增量与分段系数，用同一 ActiveSeed 生成相位扰动，
	// 保证"同一轮连发、同一种子"下整个震动序列可复现（自动化测试依赖这一点）。
	//
	// GlobalScale 一并传进去：Lyra.Recoil.Scale 是调参旋钮，玩家视角的"整体强弱"
	// 应当同时影响三轴，否则调 Scale 时 Roll 会显得与 Pitch/Yaw 脱节。
	// Roll 自己的独立旋钮（Lyra.Recoil.RollShake）则在武器实例更新相机时叠加，
	// 这样"关掉 Roll 做 A/B 对比"不需要改动资产，也不影响记录下来的历史数据。
	// ---------------------------------------------------------------------
	FCameraRollShakeParams RollParams;
	if (Profile->BuildRollShakeParams(ShotIndex, PoseMultiplier, GlobalScale, ActiveSeed, RollParams))
	{
		ULyraCameraRollShake::Trigger(RollShake, RollParams, ActiveSeed, ShotIndex);
	}
	else
	{
		// 总开关关闭 / 振幅为 0：明确停掉，避免上一发的震动残留
		RollShake.Stop();
	}

	++ShotIndex;
	TimeSinceLastFire = 0.0f;
	RecoveryElapsed = 0.0f;
	RecoveryProgress = 0.0f;
	State = ERecoilState::Accumulating;

	return true;
}

void FRecoilRuntimeState::Advance(const ULyraRecoilProfile* Profile, float DeltaSeconds)
{
	if ((Profile == nullptr) || (DeltaSeconds <= 0.0f))
	{
		return;
	}

	TimeSinceLastFire += DeltaSeconds;

	// ---------------------------------------------------------------------
	// Roll 震屏推进：独立于下面的状态机。
	// 它不关心 Accumulating/Recovering/Idle —— 自己的时钟到点就停。
	// 顺序上先推 Roll 再走状态机，保证同一帧内两者的时间基准一致。
	//
	// 注意 Roll 吃的是**原始 DeltaSeconds**，不走下面的固定子步循环。
	// 原因是 Roll 是「衰减包络 × 周期项」的解析解（按当前时刻直接求解，
	// 不做累加也不做回正），帧率只会影响它的采样密度、不会累积误差；
	// 而 Pitch/Yaw 的插值是**逐帧增量累加**，帧率一变就会漂移。
	// 两者机制不同，所以处理方式也不同 —— 这不是疏漏，而是刻意区分。
	// ---------------------------------------------------------------------
	ULyraCameraRollShake::Advance(RollShake, DeltaSeconds);

	// ---------------------------------------------------------------------
	// 插值模式：走固定子步长循环（参考文档 §2「关键镜头逻辑放在固定步长更新中」）。
	//
	// 这里是「采样轨迹一样」的实现点：不再拿整帧 DeltaSeconds 直接推进阶段，
	// 而是攒进累加器、以 1/60 秒为单位切分执行。
	// 于是 20fps（一帧 50ms → 切 3 个子步）与 144fps（一帧 6.9ms → 隔帧攒够才推）
	// 在同一个总时长上，走过的子步序列完全相同，轨迹也就逐点一致。
	//
	// 防死亡螺旋：子步数上限 MaxSubStepsPerAdvance。
	// 若某帧卡了 500ms，不设上限会算出 30 个子步 —— 本该卡一帧，结果推 30 次，
	// 卡得更久。超出的时间**直接丢弃**（不补给后续帧，避免误差累积），
	// 同时把当前阶段钳到终点，保证状态机不会停在半路。
	// ---------------------------------------------------------------------
	if (Profile->IsInterpolatedSingleShot())
	{
		SubStepAccumulator += DeltaSeconds;

		int32 SubStepCount = 0;
		while ((SubStepAccumulator >= FixedSubStepSeconds) && (SubStepCount < MaxSubStepsPerAdvance))
		{
			SubStepAccumulator -= FixedSubStepSeconds;
			LyraRecoilStatePrivate::AdvanceInterpolatedSubStep(*this, *Profile);
			++SubStepCount;

			// 时间轴已收尾（Drop 段走完 → InterpStage 归空），后续子步没有意义
			if (InterpStage == ERecoilInterpStage::None)
			{
				break;
			}
		}

		LastSubStepCount = SubStepCount;

		// ---------------------------------------------------------------------
		// 关于「不满一个子步的零头」—— 这里**刻意不做冲刷**，务必不要"顺手补上"。
		//
		// 看起来顺手补上更"准确"（喂 0.05s 就真的走 0.05s），但它会毁掉本方案
		// 唯一的硬承诺：帧率不变性。
		//
		// 原因是零头大小 = 本帧时长对 1/60 取模，**它自己是和帧率强相关的**：
		//   20fps  一帧 0.0500s → 走 2 个子步，余 0.01667
		//   144fps 一帧 0.0069s → 隔几帧才凑够 1 个子步，余数分布完全不同
		// 一旦把零头当作额外子步推出去，不同帧率在同一时刻走过的**总子步数**就
		// 不再相等，轨迹随之漂移（实测 60fps vs 144fps 偏差约 0.006 度）。
		//
		// 因此零头保留在累加器里、留给下一帧，这正是「以固定步长为唯一时间单位」
		// 的代价与收益：牺牲不超过 1 个子步的相位延迟，换来逐点一致的轨迹。
		// 阶段终值仍由 AdvanceInterpolatedSubStepBy 里的「尾段吸附」保证采到，
		// 不会因为不舍零头而永远走不到 100%。
		// ---------------------------------------------------------------------

		// 触到上限说明发生了长帧卡顿：丢弃剩余时间并清空累加器，
		// 同时把整条插值时间轴**直接推到终点**，保证状态机不会停在半路。
		//
		// 注意这里落的是"整条时间轴的终点"，而不是"当前阶段的终点"：
		// 长帧语义是「这段时间我们放弃实时演算」，那就应该直接呈现
		// 这段时间走完后的最终姿态 —— 也就是稳态残留（Peak × RecoilReturnRatio）。
		// 只推当前阶段会留下"半路态"，与"收敛"的验收目标不符。
		if ((SubStepCount >= MaxSubStepsPerAdvance) && (SubStepAccumulator >= FixedSubStepSeconds))
		{
			SubStepAccumulator = 0.0f;

			if (InterpStage != ERecoilInterpStage::None)
			{
				// 先冻结压枪量：下面算稳态值要用它，必须在"取稳态值之前"。
				// 长帧路径可能整段跳过 Settle→Drop 的切换，所以不能指望那里冻过。
				LyraRecoilStatePrivate::FreezeCompensationForRecovery(*this);

				// 稳态残留 = 本发峰值 × 回正残留比，再扣压枪量（与 Drop 段终点同一公式）
				const float SteadyPitch = FRecoilRuntimeState::ComputeRecoveryTarget(
					*Profile, InterpBasePitch + InterpShotAmplitudePitch, RecoveryCompensationPitch);
				const float SteadyYaw = FRecoilRuntimeState::ComputeRecoveryTarget(
					*Profile, InterpBaseYaw + InterpShotAmplitudeYaw, RecoveryCompensationYaw);

				// 补间输出直接落到稳态值：已经丢掉了时间，再推增量会让它与逻辑偏移脱节
				CameraOffsetPitch = FMath::Clamp(SteadyPitch, -Profile->MaxVerticalKick, Profile->MaxVerticalKick);
				CameraOffsetYaw = FMath::Clamp(SteadyYaw, -Profile->MaxHorizontalKick, Profile->MaxHorizontalKick);
				LastTargetPitch = CameraOffsetPitch;
				LastTargetYaw = CameraOffsetYaw;

				AccumulatedPitch = CameraOffsetPitch;
				AccumulatedYaw = CameraOffsetYaw;

				// 时间轴一次性收尾：转常规回正状态机，由它把 State 推到 Idle 并清计数器。
				//
				// ★ RecoveryPeakPitch 必须设成**本发峰值**：ApplyRecoveryStep 会算
				// `Target = 峰值 × 残留比 + 压枪量`，而此刻 AccumulatedPitch
				// 已经是稳态值（含压枪量的终值）。若沿用 AccumulatedPitch，残留比会被
				// 再乘一次，终值整体偏小（实测 0.0312 而非 0.1250）。
				const float PeakPitch = InterpBasePitch + InterpShotAmplitudePitch;
				const float PeakYaw = InterpBaseYaw + InterpShotAmplitudeYaw;

				InterpStage = ERecoilInterpStage::None;
				StageElapsed = 0.0f;
				RecoveryPeakPitch = PeakPitch;
				RecoveryPeakYaw = PeakYaw;
				RecoveryElapsed = Profile->RecoveryTime;
				State = ERecoilState::Recovering;
				LyraRecoilStatePrivate::ApplyRecoveryStep(*this, *Profile);
			}
		}
	}
	else
	{
		LastSubStepCount = 0;
	}

	switch (State)
	{
	case ERecoilState::Accumulating:
	{
		if (TimeSinceLastFire > Profile->RecoveryDelay)
		{
			// 停火超过了延迟：以当前偏移为峰值，开始回正。
			// RecoveryElapsed 先补上本帧越过延迟的那部分时间，避免夹具抖动造成的进度台阶。
			RecoveryPeakPitch = AccumulatedPitch;
			RecoveryPeakYaw = AccumulatedYaw;
			RecoveryElapsed = TimeSinceLastFire - Profile->RecoveryDelay;
			State = ERecoilState::Recovering;

			// 压枪量在"开始回正"这一刻冻结 —— 停火延迟之内玩家还在压的枪依然算数。
			LyraRecoilStatePrivate::FreezeCompensationForRecovery(*this);

			LyraRecoilStatePrivate::ApplyRecoveryStep(*this, *Profile);
		}
		break;
	}

	case ERecoilState::Recovering:
	{
		// 插值模式下，下降段由子步循环自己走完并置 None；
		// 此时若这里再累一次 RecoveryElapsed，回正会走两遍（补间输出会跳）。
		// 所以只在「插值链已收尾」或「非插值模式」时才推进常规回正。
		if (Profile->IsInterpolatedSingleShot() && (InterpStage != ERecoilInterpStage::None))
		{
			break;
		}

		RecoveryElapsed += DeltaSeconds;
		LyraRecoilStatePrivate::ApplyRecoveryStep(*this, *Profile);
		break;
	}

	case ERecoilState::Idle:
	default:
	{
		// 回正已完成：数值冻结在稳态偏移上，不再变化。
		// 注意插值模式下这里仍要保证补间输出与逻辑偏移一致 ——
		// 否则相机链会永远停在上一个状态（Idle 下 ApplyRecoveryStep 不再被调用）。
		if (!Profile->IsInterpolatedSingleShot())
		{
			CameraOffsetPitch = AccumulatedPitch;
			CameraOffsetYaw = AccumulatedYaw;
		}
		break;
	}
	}
}

FRecoilShotKick FRecoilRuntimeState::GetShotDirectionOffset(const ULyraRecoilProfile& Profile, int32 InShotIndex) const
{
	return ComputeShotKick(Profile, InShotIndex, CurrentPoseMultiplier, GlobalScale, ActiveSeed);
}
