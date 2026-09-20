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

	/** 推进一次回正插值。回正目标 = 基底 + 本发贡献 × RecoilReturnRatio − 本梭累计压枪量。 */
	static void ApplyRecoveryStep(FRecoilRuntimeState& RecoilState, const ULyraRecoilProfile& Profile)
	{
		const float Duration = FMath::Max(Profile.RecoveryTime, KINDA_SMALL_NUMBER);
		const float NormalizedTime = FMath::Clamp(RecoilState.RecoveryElapsed / Duration, 0.0f, 1.0f);
		const float Alpha = Profile.GetRecoveryAlpha(NormalizedTime);

		// ★ 2026-09-20 修复：回正只衰减「本发这一下的贡献」，不衰减之前连发累加出来的偏移。
		//   旧写法 `Peak × RecoilReturnRatio` 会把整条已累加偏移一起乘掉，
		//   连发时表现为每发都按比例把总偏移往低压 —— 详见
		//   Docs/Recoil/11_BurstAccumulationFix.md。
		//   InstantWrite 下 RecoveryBase == 0，公式退化成旧写法，零行为变化。
		const float TargetPitch = RecoilState.RecoveryBasePitch
			+ (RecoilState.RecoveryPeakPitch - RecoilState.RecoveryBasePitch) * Profile.RecoilReturnRatio
			- RecoilState.RecoveryCoverPitch;
		const float TargetYaw = RecoilState.RecoveryBaseYaw
			+ (RecoilState.RecoveryPeakYaw - RecoilState.RecoveryBaseYaw) * Profile.RecoilReturnRatio;

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
		//   ReboundEnd —— 从 Peak 回弹到 `Base + 幅度 × ReboundRatio`
		//   SteadyEnd  —— 从 ReboundEnd 下降到 `Base + 幅度 × RecoilReturnRatio`（本发在世界上的最终贡献）
		//
		// ★ 2026-09-20 修复：Rebound / Drop 的终点必须锚在「本发幅度」上，而不是「绝对峰值」。
		//
		//   旧写法 `Peak × ReboundRatio` 在**单发**场景下与 `Base + 幅度 × ReboundRatio` 完全等价
		//   （单发 Base == 0），因此单发手感、既有曲线与全部单发用例一字不变。
		//   但在**连发**场景下 Base ≠ 0，旧写法等于「把整条已累加偏移也乘 0.72」，
		//   于是每发的回弹都按几何级数吃掉前面积累的后坐力：
		//       Base(n) = 0.72 × Peak(n−1)   →   Peak(n) 收敛到 幅度/0.28，不再上涨
		//   表现为「连发时后坐力失效 / 一动鼠标压枪后坐力就像 0」。
		//   新写法让回弹量恒为 `幅度 × (1 − ReboundRatio)`（与打了几发无关），
		//   连发偏移因此单调上涨，与 InstantWrite 的累加语义一致 —— 这正是
		//   10_SingleShotInterpolation.md §7「逻辑偏移照常累加」想要的行为。
		const float BasePitch = State.InterpBasePitch;
		const float BaseYaw = State.InterpBaseYaw;
		const float PeakPitch = BasePitch + State.InterpShotAmplitudePitch;
		const float PeakYaw = BaseYaw + State.InterpShotAmplitudeYaw;

		const float ReboundEndPitch = BasePitch + State.InterpShotAmplitudePitch * Profile.ReboundRatio;
		const float ReboundEndYaw = BaseYaw + State.InterpShotAmplitudeYaw * Profile.ReboundRatio;

		const float SteadyEndPitch = BasePitch + State.InterpShotAmplitudePitch * Profile.RecoilReturnRatio
			- State.RecoveryCoverPitch;
		const float SteadyEndYaw = BaseYaw + State.InterpShotAmplitudeYaw * Profile.RecoilReturnRatio;

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

		// ★ 2026-09-20 修复：上限必须先夹、再同时写进「补间输出」与「逻辑偏移」。
		//
		//   旧写法只对逻辑偏移 Clamp，补间输出却拿**未夹**的目标做差分累加。
		//   一旦累加到上限（长时间连射必然发生），两者就会分叉：
		//   逻辑偏移停在 MaxVerticalKick，补间输出继续往上涨；
		//   等这一段走完、回正把它拉回稳态时，相机就会看到一次几度量级的跳变。
		//   现在夹一次、两处同写，两个输出恒等，上限语义也与 InstantWrite 完全一致
		//   （InstantWrite 的 ApplyShot 里就是对同一份值 Clamp 后拷贝给两个字段）。
		//
		//   这也是云文档 TPS_Recoil_Impl_v2.1 §7 强调的口径：
		//   「上限 clamp 要作用在**总和**上，而不是逐条叠加时就夹」——
		//   逐条夹会让后续脉冲的贡献被已饱和的值吞掉，同样表现为「到顶之后新发的后坐力消失」。
		//
		// ★ 2026-09-20 追加：垂直上限不再等于 MaxVerticalKick，而是
		//   `MaxVerticalKick + 玩家压枪抵扣`。钳制的本意是「玩家压不住枪时不让镜头飞太高」，
		//   所以该被钳的是**镜头实际抬升量（偏移 − 压枪量）**；钳裸偏移会让压枪的人
		//   不到上限就封顶 → 体感"压着枪打着打着后坐力就没了"。见 11_BurstAccumulationFix.md §12。
		const float VerticalLimit = State.GetEffectiveVerticalKickLimit(Profile);
		const float ClampedPitch = FMath::Clamp(TargetPitch, -VerticalLimit, VerticalLimit);
		const float ClampedYaw = FMath::Clamp(TargetYaw, -Profile.MaxHorizontalKick, Profile.MaxHorizontalKick);

		// 「本帧增量 = 目标值 − 上一帧目标值」—— 参考文档 §2 伪码的核心两行。
		// 之所以推增量而不是直接写绝对值：相机上玩家自己的鼠标输入也在累积，
		// 直接设绝对值会把玩家输入冲掉。
		State.CameraOffsetPitch += (ClampedPitch - State.LastTargetPitch);
		State.CameraOffsetYaw += (ClampedYaw - State.LastTargetYaw);

		State.LastTargetPitch = ClampedPitch;
		State.LastTargetYaw = ClampedYaw;

		// 同步逻辑偏移：让它始终等于「本阶段当前的目标值」。
		// 这样峰值/回正/弹道链/CSV/Golden 读到的都是「这一刻后坐力应该在哪」，
		// 语义与 InstantWrite 模式完全一致 —— 两个模式只在**相机怎么跟上**这件事上不同。
		State.AccumulatedPitch = ClampedPitch;
		State.AccumulatedYaw = ClampedYaw;

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
				// ApplyRecoveryStep 会算 `Target = RecoveryPeakPitch × RecoilReturnRatio`，
				// 而此刻 AccumulatedPitch 已经是「峰值 × 回正比」的稳态值了。
				// 若让 RecoveryPeakPitch 停留在 ReboundEnd 之类的中间值上，
				// 回正比就会被**再乘一次**，最终残留值整体偏小（实测 0.075 而非 0.125）。
				const float PeakPitch = State.InterpBasePitch + State.InterpShotAmplitudePitch;
				const float PeakYaw = State.InterpBaseYaw + State.InterpShotAmplitudeYaw;

				State.InterpStage = ERecoilInterpStage::None;
				State.StageElapsed = 0.0f;
				State.RecoveryPeakPitch = PeakPitch;
				State.RecoveryPeakYaw = PeakYaw;
				// ★ 基底必须一并带上，否则回正会把整条已累加偏移一起衰减掉（见 ApplyRecoveryStep）。
				State.RecoveryBasePitch = State.InterpBasePitch;
				State.RecoveryBaseYaw = State.InterpBaseYaw;
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

float FRecoilRuntimeState::GetEffectiveVerticalKickLimit(const ULyraRecoilProfile& Profile) const
{
	// 抵扣量夹在 [0, MaxVerticalKick]：既不允许负抵扣（负值已在 setter 里被夹成 0），
	// 也留一个"裸偏移硬顶 = 2 × MaxVerticalKick"的安全阀 ——
	// 否则玩家把视角一路压到底时，本梭结束后回正要从一个很大的值往回走，会甩镜头。
	const float Credit = FMath::Min(AimCompensationPitch, Profile.MaxVerticalKick);
	return Profile.MaxVerticalKick + Credit;
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
	RecoveryBasePitch = 0.0f;
	RecoveryBaseYaw = 0.0f;
	RecoveryCoverPitch = 0.0f;
	// 压枪抵扣由武器实例每帧重写，这里清零只是保证"没驱动方时 = 旧行为"。
	AimCompensationPitch = 0.0f;
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

	RollShake.Reset();
	State = ERecoilState::Idle;
	ActiveSeed = ResolveSeed(Profile);
	ShotHistory.Reset();

	// 散布：清零后按"站定"给一个合法的初始锥角，而不是留 0。
	//
	// 留 0 会导致"刚换枪的那一帧准星显示满精度"——因为状态在 OnEquipped 里
	// 就 Reset 了，而第一次 AdvanceSpread 要等到下一个 Tick 才跑。虽然只有一帧，
	// 但换枪瞬间的准星跳变是肉眼可见的（HUD 读的就是这个值）。
	ResetSpread();
	if ((Profile != nullptr) && Profile->bEnableProfileSpread)
	{
		const FRecoilSpreadParams Params = Profile->GetSpreadParams(EPoseState::Standing);
		CurrentSpreadParams = Params;
		CurrentSpreadBaseAngle = Params.BaseAngleDegrees;
		CurrentSpreadMaxAngle = Params.MaxAngleDegrees;
		CurrentSpreadAngle = Params.BaseAngleDegrees;
	}
}

void FRecoilRuntimeState::ResetSpread()
{
	CurrentSpreadAngle = 0.0f;
	CurrentSpreadBaseAngle = 0.0f;
	CurrentSpreadMaxAngle = 0.0f;
	CurrentSpreadParams = FRecoilSpreadParams();
	SpreadAimingMultiplier = 1.0f;
	SpreadMovementMultiplier = 1.0f;
	PendingShotSpreadAngle = 0.0f;
	LastSpreadAngle = 0.0f;
}

// ---------------------------------------------------------------------------
// 散布（姿态-角度直接模型，见 Docs/Recoil/12_SpreadInProfile.md）
// ---------------------------------------------------------------------------

void FRecoilRuntimeState::ApplySpreadShot(const ULyraRecoilProfile* Profile, EPoseState PoseState)
{
	if ((Profile == nullptr) || !Profile->bEnableProfileSpread)
	{
		// 未启用资产散布 → 完全不动手，把散布留给 Lyra 原生 heat 链路。
		return;
	}

	const FRecoilSpreadParams Params = Profile->GetSpreadParams(PoseState);
	CurrentSpreadParams = Params;
	CurrentSpreadBaseAngle = Params.BaseAngleDegrees;
	CurrentSpreadMaxAngle = Params.MaxAngleDegrees;

	// 基础角是"地板"：第一次开火从基础角起步，而不是从 0 起步。
	// （Reset 已经给过一次初值，但换姿态后 Base 可能变高，这里再兜一次。）
	if (CurrentSpreadAngle < CurrentSpreadBaseAngle)
	{
		CurrentSpreadAngle = CurrentSpreadBaseAngle;
	}

	CurrentSpreadAngle = FMath::Clamp(
		CurrentSpreadAngle + Params.AddPerShotDegrees,
		CurrentSpreadBaseAngle,
		CurrentSpreadMaxAngle);

	LastSpreadAngle = GetEffectiveSpreadAngle();
}

void FRecoilRuntimeState::AdvanceSpread(const ULyraRecoilProfile* Profile, float DeltaSeconds, EPoseState PoseState)
{
	if ((Profile == nullptr) || !Profile->bEnableProfileSpread || (DeltaSeconds <= 0.0f))
	{
		return;
	}

	// 姿态每帧重新解析：蹲下 / 起立 / 起跳会立刻换一组 Base/Max/RecoverRate。
	// 这里是"蹲下马上变准"这条手感的唯一实现点。
	const FRecoilSpreadParams Params = Profile->GetSpreadParams(PoseState);
	CurrentSpreadBaseAngle = Params.BaseAngleDegrees;
	CurrentSpreadMaxAngle = Params.MaxAngleDegrees;
	CurrentSpreadParams = Params;

	// 换姿态时新基础角可能**高于**当前锥角（例：站起来 Base 0.25 → 0.35，而当前只有 0.3）。
	// 先抬到新基础角，避免出现"锥角低于基础角"这种自相矛盾的状态。
	if (CurrentSpreadAngle < CurrentSpreadBaseAngle)
	{
		CurrentSpreadAngle = CurrentSpreadBaseAngle;
	}

	// 停火延迟内不回落：保持连射末端的锥角（点射节奏下这一条是"手感的停顿感"来源）。
	if (TimeSinceLastFire <= Profile->SpreadRecoveryDelay)
	{
		CurrentSpreadAngle = FMath::Clamp(CurrentSpreadAngle, CurrentSpreadBaseAngle, CurrentSpreadMaxAngle);
		return;
	}

	if (Params.RecoverRateDegreesPerSecond > 0.0f)
	{
		CurrentSpreadAngle -= Params.RecoverRateDegreesPerSecond * DeltaSeconds;
	}

	CurrentSpreadAngle = FMath::Clamp(CurrentSpreadAngle, CurrentSpreadBaseAngle, CurrentSpreadMaxAngle);
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
		// 新一梭：回正抵扣用的「累计压枪量」从零重新累积
		RecoveryCoverPitch = 0.0f;
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

		// ★ 本发基底 = 回正基底。回正目标 = 基底 + 本发贡献 × 回正比，
		//   所以「连发累加出来的偏移」永远不会被本发的回弹/回正吃掉。
		RecoveryBasePitch = InterpBasePitch;
		RecoveryBaseYaw = InterpBaseYaw;
	}
	else
	{
		// ---------------------------------------------------------------------
		// 瞬时写入模式（默认）：开火帧直接把本发 Kick 写进累加偏移，上抬耗时 = 0 秒。
		//
		// 这是既有行为，一字未改 —— 既有 24 个测试、3 份 Golden 数据、CSV 契约
		// 全部建立在这个语义上。
		// ---------------------------------------------------------------------
		// 垂直上限含玩家压枪抵扣（钳的是"镜头实际抬升量"，见 §12）。
		// InstantWrite 下压枪抵扣同样生效 —— 该模式下连发累加是 100%，
		// 不抵扣的话压枪的人会一样"到 Max 就封顶"。
		const float VerticalLimit = GetEffectiveVerticalKickLimit(*Profile);
		AccumulatedPitch = FMath::Clamp(AccumulatedPitch + Kick.Vertical, -VerticalLimit, VerticalLimit);
		AccumulatedYaw = FMath::Clamp(AccumulatedYaw + Kick.Horizontal, -Profile->MaxHorizontalKick, Profile->MaxHorizontalKick);

		// 补间输出恒等拷贝：相机链读的是 CameraOffsetPitch/Yaw，
		// 在瞬时写入模式下它必须与逻辑偏移完全一致，否则手感会凭空变化。
		CameraOffsetPitch = AccumulatedPitch;
		CameraOffsetYaw = AccumulatedYaw;

		// 瞬时写入模式的回正基底恒为 0 → 回正目标退化成旧的「峰值 × 回正比」，
		// 既有行为一字不变（Golden / CSV / 既有用例全部建立在这个模式上）。
		RecoveryBasePitch = 0.0f;
		RecoveryBaseYaw = 0.0f;
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
	// ★ 散布角取的是弹道侧在**发弹那一刻**留下的快照（PendingShotSpreadAngle），
	//   不是此刻的 CurrentSpreadAngle —— 因为 AddSpread 已经在本发之后加热过了。
	//   未启用资产散布时这个中转字段恒为 0，本列即为 0（语义 = "这一发不吃资产散布"）。
	Result.SpreadAngle = PendingShotSpreadAngle;
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
		// 这段时间走完后的最终姿态 —— 也就是稳态残留（本发基底 + 本发幅度 × RecoilReturnRatio − 本梭累计压枪量）。
		// 只推当前阶段会留下"半路态"，与"收敛"的验收目标不符。
		if ((SubStepCount >= MaxSubStepsPerAdvance) && (SubStepAccumulator >= FixedSubStepSeconds))
		{
			SubStepAccumulator = 0.0f;

			if (InterpStage != ERecoilInterpStage::None)
			{
				// 稳态残留 = 本发基底 + 本发幅度 × 回正残留比
				// （★ 2026-09-20 修复：旧写法 `本发峰值 × 回正残留比` 会把已累加偏移一起乘掉）
				const float SteadyPitch = InterpBasePitch + InterpShotAmplitudePitch * Profile->RecoilReturnRatio
					- RecoveryCoverPitch;
				const float SteadyYaw = InterpBaseYaw + InterpShotAmplitudeYaw * Profile->RecoilReturnRatio;

				// 补间输出直接落到稳态值：已经丢掉了时间，再推增量会让它与逻辑偏移脱节
				const float VerticalLimit = GetEffectiveVerticalKickLimit(*Profile);
				CameraOffsetPitch = FMath::Clamp(SteadyPitch, -VerticalLimit, VerticalLimit);
				CameraOffsetYaw = FMath::Clamp(SteadyYaw, -Profile->MaxHorizontalKick, Profile->MaxHorizontalKick);
				LastTargetPitch = CameraOffsetPitch;
				LastTargetYaw = CameraOffsetYaw;

				AccumulatedPitch = CameraOffsetPitch;
				AccumulatedYaw = CameraOffsetYaw;

				// 时间轴一次性收尾：转常规回正状态机，由它把 State 推到 Idle 并清计数器。
				//
				// ★ RecoveryPeakPitch 必须设成**本发峰值**：ApplyRecoveryStep 会算
				// `Target = RecoveryPeakPitch × RecoilReturnRatio`，而此刻 AccumulatedPitch
				// 已经是稳态值（峰值 × 回正比）。若沿用 AccumulatedPitch，回正比会被
				// 再乘一次，残留值变小一半以上（实测 0.0312 而非 0.1250）。
				const float PeakPitch = InterpBasePitch + InterpShotAmplitudePitch;
				const float PeakYaw = InterpBaseYaw + InterpShotAmplitudeYaw;

				InterpStage = ERecoilInterpStage::None;
				StageElapsed = 0.0f;
				RecoveryPeakPitch = PeakPitch;
				RecoveryPeakYaw = PeakYaw;
				// ★ 基底一并带上：长帧收敛也不允许把已累加偏移衰减掉。
				RecoveryBasePitch = InterpBasePitch;
				RecoveryBaseYaw = InterpBaseYaw;
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

	// ◆ 累计本梭压枪量（单调不减、停火后自然冻结）。
	//   不直接读 AimCompensationPitch 的原因：停火后玩家必然松手，
	//   ControlRotation 回升 ⇒ AimCompensationPitch 实时缩回 0；回正若读实时值，
	//   目标会在回正途中跳回旧值（非单调甩镜）。
	RecoveryCoverPitch = FMath::Max(RecoveryCoverPitch, AimCompensationPitch);

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

			// ★ 回正基底：InstantWrite 恒为 0（= 旧公式，零行为变化）；
			//   插值模式取「本发基底」，这样即使 RecoveryDelay 恰好撞上射速间隔、
			//   在连发中途误触发一次回正，也不会把已累加偏移按比例吃掉。
			if (Profile->IsInterpolatedSingleShot())
			{
				RecoveryBasePitch = InterpBasePitch;
				RecoveryBaseYaw = InterpBaseYaw;
			}
			else
			{
				RecoveryBasePitch = 0.0f;
				RecoveryBaseYaw = 0.0f;
			}

			RecoveryElapsed = TimeSinceLastFire - Profile->RecoveryDelay;
			State = ERecoilState::Recovering;

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
