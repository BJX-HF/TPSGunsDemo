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
	 *
	 * ★ 2026-09-21：本函数同时负责「一梭只抵扣一次」的收敛 ——
	 *   若 `bRecoveryCoverApplied` 已为 true，说明本梭的抵扣额度已经用掉了，
	 *   这次的（中途）回正**不再重复抵扣**，把 `RecoveryCompensationPitch` 置 0。
	 *   原因见 `bRecoveryCoverApplied` 的注释。
	 */
	static void FreezeCompensationForRecovery(FRecoilRuntimeState& RecoilState)
	{
		if (RecoilState.bRecoveryCoverApplied)
		{
			// 抵扣额度已用完：本次回正只衰减本发贡献，不再扣累计压枪量。
			RecoilState.RecoveryCompensationPitch = 0.0f;
			RecoilState.RecoveryCompensationYaw = 0.0f;
			return;
		}

		// 用累计量而非实时量 —— 停火后玩家必然松手，实时值会缩回 0。
		RecoilState.RecoveryCompensationPitch = RecoilState.RecoveryCoverPitch;
		// ★ 2026-09-21：水平轴改用**自己的**累计量。
		//   早先这里硬编码 0，于是 bCompensationAwareRecoveryYaw 打开也不生效
		//   （是个"打开也没用的开关"）。现在改由 Profile 开关决定是否消费：
		//   开关默认 false ⇒ 行为与硬编码 0 时逐位一致。
		RecoilState.RecoveryCompensationYaw = RecoilState.RecoveryCoverYaw;
		RecoilState.bRecoveryCoverApplied = true;
	}

	/** 推进一次回正插值。回正目标 = 本梭累计压枪量（见 ComputeRecoveryTarget）。 */
	static void ApplyRecoveryStep(FRecoilRuntimeState& RecoilState, const ULyraRecoilProfile& Profile)
	{
		const float Duration = FMath::Max(Profile.RecoveryTime, KINDA_SMALL_NUMBER);
		const float NormalizedTime = FMath::Clamp(RecoilState.RecoveryElapsed / Duration, 0.0f, 1.0f);
		const float Alpha = Profile.GetRecoveryAlpha(NormalizedTime);

		const float TargetPitch = FRecoilRuntimeState::ComputeRecoveryTarget(
			Profile, RecoilState.RecoveryCompensationPitch);
		const float TargetYaw = FRecoilRuntimeState::ComputeRecoveryTarget(
			Profile, RecoilState.RecoveryCompensationYaw,
			Profile.bCompensationAwareRecoveryYaw);

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
			// 回正结束：落到稳态偏移（= 本梭累计压枪量），回到 Idle，准备下一轮连发
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
		//   SteadyEnd  —— 从 ReboundEnd 收敛到「本梭累计压枪量」（本发的最终姿态）
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

		// Drop 段的终点必须与 ApplyRecoveryStep 收尾时落的值**完全一致**，
		// 否则在 Drop 结束那一帧会跳一下。
		//
		// ★ 2026-09-21（第二次修正）：读数源改回「已冻结的累计压枪量」。
		//
		//   上一版写的是 `bRecoveryCoverApplied ? 0 : RecoveryCoverPitch`，本意是
		//   「本函数可能在冻结之前被调用，那时读快照会拿到 0」。但实机时序恰好相反：
		//   FreezeCompensationForRecovery() 在 Settle→Drop 切换处就把标志置成了 true，
		//   于是**整个 Drop 段**每帧都推导出 0 ⇒ 目标 = 不抵扣 ⇒ 逻辑偏移冻结在峰值
		//   纹丝不动，直到收官那帧 ApplyRecoveryStep 用快照算对，一帧跳过去。
		//
		//   实机 trace 佐证（DA_Recoil_Rifle_S 连发）：
		//     Drop 段 23 帧 acc 恒为 15.000 ⇒ 收尾瞬跳 3.950 ⇒ 屏幕 −8.30（看地板）。
		//
		//   现在直接读 RecoveryCompensationPitch（= Freeze 时锁定的本梭累计压枪量），
		//   与 ApplyRecoveryStep 完全同源 ⇒ Drop 段平滑收敛，终点与收官值一致、不再跳。
		const float StageCoverPitch = State.RecoveryCompensationPitch;

		const float SteadyEndPitch = FRecoilRuntimeState::ComputeRecoveryTarget(
			Profile, StageCoverPitch);
		// 水平轴同源取自己的快照；默认开关 false ⇒ 本式返回 0，Yaw 回正回满。
		const float SteadyEndYaw = FRecoilRuntimeState::ComputeRecoveryTarget(
			Profile, State.RecoveryCompensationYaw,
			Profile.bCompensationAwareRecoveryYaw);

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

float FRecoilRuntimeState::ComputeRecoveryTarget(
	const ULyraRecoilProfile& Profile, float Cover, bool bApplyCover)
{
	// =====================================================================
	// 回正终止值的唯一实现（2026-09-21 大祥老师**二次**拍板的口径）
	// =====================================================================
	//
	//     终止值 = 本梭累计压枪量
	//
	// 语义：回正把「后坐力偏移」收敛到**玩家自己压下去的量**。
	//
	//   屏幕视角 = ControlRotation（含玩家压枪）+ 后坐力偏移，
	//   所以把偏移收敛到压枪量时，屏幕正好回到开枪前的位置 —— 这就是设计目标。
	//
	//     不压枪   ⇒ 终止值 = 0   ⇒ 偏移回满 ⇒ 屏幕回开枪前
	//     压 N 度   ⇒ 终止值 = N   ⇒ 玩家的 Ctrl 已低了 N 度，屏幕同样回开枪前
	//
	// ★ 与上一版的差别（实测坐实的错误）：
	//
	//   上一版写的是 `峰值 − 压枪量`，于是
	//       屏幕 = −压枪量 + (峰值 − 压枪量) = 峰值 − 2×压枪量
	//   压在真实弹道上就是**看地板**。实机 trace（DA_Recoil_Rifle_S 连发）：
	//       峰值 17.600、累计压枪 13.650、玩家 Ctrl 低了 13.650
	//       终值 = 17.600 − 13.650 = 3.950 ⇒ 屏幕 = −13.650 + 3.950 = −9.700（低于开枪前 9.7°）
	//   改成「偏移 = 压枪量」后：屏幕 = −13.650 + 13.650 = 0 ⇒ 精确回到开枪前 ✓
	//
	//   ⇒ **峰值不再参与本式**，因此形参 Peak 已移除。
	//
	// 两把闸门串联才允许抵扣 ——
	//   bCompensationAwareRecovery     总开关（默认 true）
	//   bApplyCover                    本轴开关（Yaw 默认 false，见 Profile 头文件）
	//   垂直轴恒传 true（默认实参）；
	//   水平轴默认不扣 —— 因为 MaxHorizontalKick 只有 2.0°，门槛 1.7°，
	//   而"转身追目标"随时超过 1.7°，否则 Yaw 回正会长期恒为 0。
	//
	// 不抵扣（总开关关 / 本轴不参与）时终止值 = 0 ⇒ 偏移完全回满、屏幕停在玩家压枪后的位置。
	return (Profile.bCompensationAwareRecovery && bApplyCover) ? Cover : 0.0f;
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
		// ★ 2026-09-21：同步把 P12/P14 消费的 AimCompensation* 归零。
		//   本梭已结束，旧基线不再代表"起点"；此处若留旧值会白送抵扣额度
		//   （"压一下再松手"能永久骗到更高的硬顶 + 更低的回正目标）。
		AimCompensationPitch = 0.0f;
		AimCompensationYaw = 0.0f;
		return;
	}

	// NormalizeAxis 处理绕圈（-180/180 附近）—— 少了它，玩家转半圈会被算成"压枪 358°"。
	// 用 FRotator::NormalizeAxis（等价于 FRotator::NormalizeAxis 的静态入口，见相机修改器同款用法）。
	const float PitchDelta = FRotator::NormalizeAxis(SampledAimPitch - AimPitchAtBurstStart);
	const float YawDelta = FRotator::NormalizeAxis(SampledAimYaw - AimYawAtBurstStart);

	// 取负：玩家往下压（Pitch 减小）→ 压枪量为正；往左拉（Yaw 减小）→ 压枪量为正。
	PlayerCompensationPitch = -PitchDelta;
	PlayerCompensationYaw = -YawDelta;

	// ---------------------------------------------------------------------
	// ★ 2026-09-21：单一写入口。
	//
	// 修复前存在**两份平行的压枪量实现**，各带一份基准：
	//   SamplePlayerAim()                  → 基准 AimPitchAtBurstStart → PlayerCompensationPitch
	//   ULyraRangedWeaponInstance 里那份    → 自带的起枪点字段            → AimCompensationPitch
	// （后者已删除 —— 武器实例现在只调本函数，见 LyraRangedWeaponInstance.cpp）
	//
	// 后果：调试面板显示的是前者，回正/P12 钳制消费的是后者，两者基准锁定时机不同
	// （ApplyShot 首帧 vs AddRecoil 首帧）⇒ 值可能不等、且纯数值单测（只调本函数）
	// 永远无法覆盖消费端 —— 6 个 Compensation 用例就是这么挂的。
	//
	// 现在本函数是压枪量的**唯一定义点**：算完顺手同步给消费端字段。
	// 两个轴各同步一份 —— 消费端（钳制 / 回正）分轴取用，开关各自独立。
	// ---------------------------------------------------------------------
	AimCompensationPitch = FMath::Max(0.0f, PlayerCompensationPitch);
	AimCompensationYaw = FMath::Max(0.0f, PlayerCompensationYaw);
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
	RecoveryCoverYaw = 0.0f;
	bRecoveryCoverApplied = false;
	// 压枪抵扣由武器实例每帧重写，这里清零只是保证"没驱动方时 = 旧行为"。
	AimCompensationPitch = 0.0f;
	AimCompensationYaw = 0.0f;
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

		// 压枪量以「本轮第一发的玩家瞄准」为基准重新起算。
		// 基准取最近一次采样值（武器实例每帧采样，所以最多滞后一帧）——
		// 这一帧的滞后在"往下压 4°"这种量级上完全看不出来，却让本层保持无 UWorld 依赖。
		AimPitchAtBurstStart = SampledAimPitch;
		AimYawAtBurstStart = SampledAimYaw;
		PlayerCompensationPitch = 0.0f;
		PlayerCompensationYaw = 0.0f;
		RecoveryCompensationPitch = 0.0f;
		RecoveryCompensationYaw = 0.0f;
		// 新一梭：累计抵扣量与"已抵扣"标志一并重置，本梭重新拥有一次抵扣额度。
		RecoveryCoverPitch = 0.0f;
		RecoveryCoverYaw = 0.0f;
		bRecoveryCoverApplied = false;
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

	// ◆ 累计本梭压枪量（单调不减、停火后自然冻结）。
	//
	//   ★ 2026-09-21：**必须放在状态机与插值子步循环之前**。
	//
	//   修复前这句在函数末尾（子步循环之后），于是插值模式出现时序错位：
	//   子步内部进入 Drop 段时会调 FreezeCompensationForRecovery() 读本值，
	//   而本值这一刻还没被本帧的采样更新 ⇒ 读到上一帧、乃至首帧的 0
	//   ⇒ 插值模式下的抵扣恒为 0（InterpolatedDropConsistency 就是这么挂的）。
	//
	//   非插值模式侥幸不受影响：它的回正发生在 switch 里，天然在累计之后。
	//
	//   不直接读 AimCompensationPitch 的原因：停火后玩家必然松手，
	//   ControlRotation 回升 ⇒ AimCompensationPitch 实时缩回 0；回正若读实时值，
	//   目标会在回正途中跳回旧值（非单调甩镜）。
	RecoveryCoverPitch = FMath::Max(RecoveryCoverPitch, AimCompensationPitch);
	RecoveryCoverYaw = FMath::Max(RecoveryCoverYaw, AimCompensationYaw);

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
		// 这段时间走完后的最终姿态 —— 也就是稳态偏移（本梭累计压枪量）。
		// 只推当前阶段会留下"半路态"，与"收敛"的验收目标不符。
		if ((SubStepCount >= MaxSubStepsPerAdvance) && (SubStepAccumulator >= FixedSubStepSeconds))
		{
			SubStepAccumulator = 0.0f;

			if (InterpStage != ERecoilInterpStage::None)
			{
				// 先冻结压枪量：下面算稳态值要用它，必须在"取稳态值之前"。
				// 长帧路径可能整段跳过 Settle→Drop 的切换，所以不能指望那里冻过。
				LyraRecoilStatePrivate::FreezeCompensationForRecovery(*this);

				// 稳态残留 = 本梭累计压枪量（与 Drop 段终点同一公式）
				//
				// Pitch / Yaw 都用本轴**已冻结的**累计量 + 本轴开关 ——
				// 水平默认不抵扣，传累计量是为了资产显式打开时口径一致。
				const float SteadyPitch = FRecoilRuntimeState::ComputeRecoveryTarget(
					*Profile, RecoveryCompensationPitch);
				const float SteadyYaw = FRecoilRuntimeState::ComputeRecoveryTarget(
					*Profile, RecoveryCompensationYaw,
					Profile->bCompensationAwareRecoveryYaw);

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
				// `Target = 峰值 × 残留比 + 压枪量`，而此刻 AccumulatedPitch
				// 已经是稳态值（含压枪量的终值）。若沿用 AccumulatedPitch，残留比会被
				// 再乘一次，终值整体偏小（实测 0.0312 而非 0.1250）。
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

	switch (State)
	{
	case ERecoilState::Accumulating:
	{
		// ★ 2026-09-21：插值模式且插值链仍在推进时，阶段切换完全交给上面的子步循环，
		//   本分支必须短路。
		//
		//   修复前这里对插值模式也生效，于是同一帧内发生**双重冻结**：
		//     1) 子步循环里 Settle→Drop 时 Freeze 一次 → 抵扣生效、bRecoveryCoverApplied = true
		//     2) 子步循环跑完回到这里，TimeSinceLastFire 已 > RecoveryDelay → 再 Freeze 一次
		//        → 走"额度已用尽"分支 → RecoveryCompensationPitch 被清回 0
		//   ⇒ 插值模式的抵扣凭空消失（InterpolatedDropConsistency 实测 0.0000 而非 0.3）。
		//
		//   条件与下面 Recovering 分支的同款短路保持一致：
		//   `IsInterpolatedSingleShot() && InterpStage != None`。
		//   必须带 `InterpStage != None` —— 否则插值链已收尾（或压根没启动）时
		//   本分支被永久跳过，State 会卡在 Accumulating 出不来。
		if (Profile->IsInterpolatedSingleShot() && (InterpStage != ERecoilInterpStage::None))
		{
			break;
		}

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
