// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Camera/LyraCameraShakeTypes.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"

#include "LyraRecoilState.generated.h"

// Lyra 的跨模块导出约定：本结构的静态算法函数需要被 LyraEditor 模块调用
// （LyraRecoilGoldenDumpCommandlet 导出 P3 Golden 数据），因此必须显式导出。
// 参见 Weapons/LyraWeaponInstance.h L11 的同款写法。
#define UE_API LYRAGAME_API

class ULyraRecoilProfile;

/**
 * 单发偏移增量（度）。纯数值类型，不进反射系统。
 * Pitch 向上为正，Yaw 向右为正。
 */
struct FRecoilShotKick
{
	float Vertical = 0.0f;
	float Horizontal = 0.0f;
};

/**
 * 插值模式下的单发阶段。
 *
 * 对应《FPS 相机镜头设计与实现》§2「后座」的四段式，本项目合并为三段：
 *
 *   参考文档:  t0 上抬  →  t1 瞬时回弹  →  t2 稳定  →  t3 下降 / 回正
 *   本项目:    Lift     →  Rebound      →  [RecoveryDelay] →  Drop
 *                                          ↑
 *                                  复用 RecoveryDelay，不引入重复语义的参数
 *
 * 注意：这个枚举**只在 SingleShotMode == Interpolated 时被使用**。
 * InstantWrite 模式下状态机仍然是 ERecoilState 的 Idle/Accumulating/Recovering 三态。
 */
UENUM()
enum class ERecoilInterpStage : uint8
{
	/** 未处于插值推进中（InstantWrite 模式，或插值模式尚未开火） */
	None		UMETA(Hidden),

	/** t0：从 0 抬到本发完整幅度，形状由 LiftCurve 决定 */
	Lift		UMETA(DisplayName = "Lift"),

	/** t1：从峰值回弹到「峰值 × ReboundRatio」，形状由 ReboundCurve 决定 */
	Rebound		UMETA(DisplayName = "Rebound"),

	/**
	 * 稳定段：偏移冻结在回弹终点，时长 = RecoveryDelay。
	 * 这一段不需要插值曲线 —— 目标值不变，所以「帧间增量」恒为 0。
	 */
	Settle		UMETA(DisplayName = "Settle"),

	/** t2：从回弹终点下降到「峰值 × RecoilReturnRatio」，形状由 RecoveryCurve 决定 */
	Drop		UMETA(DisplayName = "Drop")
};

/**
 * FRecoilRuntimeState
 *
 * 后坐力运行时核心算法层。**必须保持无 UWorld 依赖**（开发计划 §硬性规则 4），
 * 全部输入通过参数传入（Profile / DeltaSeconds / 倍率），因此可以脱离引擎世界
 * 做纯数值单测 —— 这是 P2/P3/P4 每阶段"可验证"的技术基础。
 *
 * 不持有任何 UObject 指针：Profile 每次调用时由外部传入。这样它就是一个能
 * 脱离引擎世界独立运行的纯数值结构体。
 * （本项目不做联机 —— 2026-09-17 决定；这个设计的价值在于"纯数值可测"，
 *   而不是"便于网络复制"。）
 *
 * === 两条输出链路（各自独立，不要互相污染）===
 *
 *  [相机链] ApplyShot() 累加到 AccumulatedPitch/Yaw，Advance() 负责回正。
 *           输出：GetCameraPitchOffset() / GetCameraYawOffset()，单位度。
 *           消费方：UCameraModifier_WeaponRecoil（只改显示层 POV）。
 *
 *  [弹道链] GetShotDirectionOffset() 是**纯函数**，返回该发的单发方向偏移，
 *           不做任何累加。消费方：ULyraGameplayAbility_RangedWeapon 的发射方向计算。
 *           弹道偏移的形状由 PatternPoints 决定（Y 编码"越打越高"的爬升），
 *           不做回正 —— 这正是决策 3 里"0 = 相机回正、弹道不回正"的含义。
 *
 * === 状态机 ===
 *   Idle ──开火──► Accumulating ──停火 > RecoveryDelay──► Recovering ──回正完成──► Idle
 *                      ▲                                        │
 *                      └──────────── 再次开火 ──────────────────┘
 *   注：Idle 表示"回正已完成、数值不再变化"，此时可能仍残留
 *       Peak × RecoilReturnRatio 的稳态偏移（RecoilReturnRatio > 0 时）。
 */
USTRUCT(BlueprintType)
struct UE_API FRecoilRuntimeState
{
	GENERATED_BODY()

public:

	// ---------------------------------------------------------------------
	// 运行时状态（P2 单测断言对象 / P4 姿态验收数据源 / P5 屏幕面板与 CSV 导出数据源）
	// ---------------------------------------------------------------------

	/** 当前第几发（0 起）。回正完成或 Reset 后归零，Accumulating/Recovering 期间保持。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|State")
	int32 ShotIndex = 0;

	/** 相机链当前垂直偏移（度，向上为正）。Accumulating 期间累加，Recovering 期间回正。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|State")
	float AccumulatedPitch = 0.0f;

	/** 相机链当前水平偏移（度，向右为正）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|State")
	float AccumulatedYaw = 0.0f;

	// ---------------------------------------------------------------------
	// 相机补间输出（Interpolated 模式专用）
	//
	// === 为什么需要这第二套偏移 ===
	//
	// AccumulatedPitch/Yaw 是「逻辑偏移」—— 这一发「应该」抬到哪。它同时被三处消费：
	//   1. 回正逻辑（峰值取它，回正目标 = 峰值 × RecoilReturnRatio）
	//   2. ShotHistory 记录（CSV 前 6 列契约）
	//   3. Golden 数据与 24 个自动化测试
	//
	// 如果插值模式直接改写它，上面三处全部要重导基线，而且会丢掉「有没有改坏」的判据。
	// 所以新增这一对**只给相机链读**的补间输出：
	//
	//   相机链  →  只读 CameraOffsetPitch/Yaw
	//   其它所有消费方  →  继续读 AccumulatedPitch/Yaw
	//
	// InstantWrite 模式下，CameraOffsetPitch/Yaw **恒等于** AccumulatedPitch/Yaw
	// （同步在 ApplyShot / ApplyRecoveryStep 里完成），所以既有行为零变化。
	// ---------------------------------------------------------------------

	/** 相机链补间输出（度）。Interpolated 模式下逐帧补间；InstantWrite 下恒等于 AccumulatedPitch。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|State")
	float CameraOffsetPitch = 0.0f;

	/** 相机链补间输出的水平分量（度）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|State")
	float CameraOffsetYaw = 0.0f;

	/** 当前状态机状态。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|State")
	ERecoilState State = ERecoilState::Idle;

	/** 距离上一次开火经过的秒数。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|State")
	float TimeSinceLastFire = 0.0f;

	/** 回正进度 [0,1]，= RecoveryCurve 在归一化时间处的取值。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|State")
	float RecoveryProgress = 0.0f;

	/** 最近一次开火使用的总姿态倍率（含瞄准混合）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|State")
	float CurrentPoseMultiplier = 1.0f;

	/** 最近一次开火时的姿态。只用于调试显示与逐发记录，不参与任何计算。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|State")
	EPoseState LastPoseState = EPoseState::Standing;

	/** 最近一发的垂直增量（度），供调试面板显示。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|State")
	float LastVerticalKick = 0.0f;

	/** 最近一发的水平增量（度），供调试面板显示。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|State")
	float LastHorizontalKick = 0.0f;

	// ---------------------------------------------------------------------
	// 散布状态（Spread）—— 姿态-角度直接模型
	//
	// 与 Pitch/Yaw 累加链互相独立，但**共用同一个时序**（都在 Tick → UpdateRecoil 里推进），
	// 这样一帧之内「后坐力偏移」与「散布锥角」看到的是同一个姿态与同一个 DeltaSeconds。
	//
	// 完整模型（详见 Docs/Recoil/12_SpreadInProfile.md）：
	//
	//   CurrentSpreadAngle ──每发 +AddPerShot──► 钳到 [Base, Max]
	//                      ◄──停火 -RecoverRate×dt──
	//
	//   最终锥角 = CurrentSpreadAngle × SpreadAimingMultiplier × SpreadMovementMultiplier
	//
	// 注意 CurrentSpreadAngle 存的是**绝对角度**（不是"相对基础角的增量"）：
	// 因为 Base/Max 会随姿态切换，存增量的话换姿态时要重新解释，容易出错。
	// ---------------------------------------------------------------------

	/** 当前散布角（度，全锥角）。含连射累加，**不含**玩家侧倍率（瞄准/移动）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Spread")
	float CurrentSpreadAngle = 0.0f;

	/** 当前姿态的基础散布角（度）。回落目标。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Spread")
	float CurrentSpreadBaseAngle = 0.0f;

	/** 当前姿态的上限散布角（度）。连射封顶值。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Spread")
	float CurrentSpreadMaxAngle = 0.0f;

	/** 最近一次开火时解析出的姿态散布参数快照（调试面板显示用）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Spread")
	FRecoilSpreadParams CurrentSpreadParams;

	/** 当前瞄准倍率（由武器实例每帧写入）。调试显示用，计算最终锥角时也用它。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Spread")
	float SpreadAimingMultiplier = 1.0f;

	/** 当前移动倍率（由武器实例每帧写入）。调试显示用，计算最终锥角时也用它。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Spread")
	float SpreadMovementMultiplier = 1.0f;

	/**
	 * 待记录的本发散布角（度）。
	 *
	 * 由弹道链（LyraGameplayAbility_RangedWeapon::TraceBulletsInCartridge）在发射前写入，
	 * ApplyShot 时搬到 FRecoilShotResult::SpreadAngle 落进 ShotHistory。
	 *
	 * 为什么需要这个"中转字段"：Lyra 的既有顺序是「先按当前散布打出去 → 再 AddSpread 加热」，
	 * 而 AddRecoil/ApplyShot 发生在 AddSpread **之后**，此刻的 CurrentSpreadAngle 已经被
	 * 本发加热过了。直接用它会记错（记成"下一发用的值"），所以必须由弹道侧在发弹那一刻取一次快照。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Spread")
	float PendingShotSpreadAngle = 0.0f;

	/** 本发开火瞬间的最终散布角（度，含倍率）。供面板显示与落差排查。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Spread")
	float LastSpreadAngle = 0.0f;

	// ---------------------------------------------------------------------
	// Roll 震屏状态（独立通道，与上面的 Pitch/Yaw 累加-回正无关）
	//
	// 对应《FPS 相机镜头设计与实现》§2.1：开火瞬间的爆发震颤 + 后续释放。
	// 走"每发重置时钟 → 按衰减包络 × 周期项直接求解"的解析模型，
	// **不累加、不回正**，因此它的状态不与 AccumulatedPitch/Yaw 共享任何逻辑。
	// ---------------------------------------------------------------------

	/** Roll 震动运行时状态。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|RollShake")
	FCameraRollShakeState RollShake;

	// ---------------------------------------------------------------------
	// 内部推进状态
	// ---------------------------------------------------------------------

	/** 进入 Recovering 时的垂直峰值。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Internal")
	float RecoveryPeakPitch = 0.0f;

	/** 进入 Recovering 时的水平峰值。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Internal")
	float RecoveryPeakYaw = 0.0f;

	/**
	 * 进入 Recovering 时「本发开始那一刻已经累加好的偏移」（度）。
	 *
	 * 回正目标 = RecoveryBase + (RecoveryPeak − RecoveryBase) × RecoilReturnRatio，
	 * 也就是「**只把本发这一下的贡献衰减掉，不衰减之前连发累加出来的偏移**」。
	 *
	 * ★ 2026-09-20 修复引入（见 Docs/Recoil/11_BurstAccumulationFix.md）：
	 * 旧公式是 `RecoveryPeak × RecoilReturnRatio`（把**整条已累加偏移**乘一次回正比）。
	 * 连发时每发的 Rebound 与回正都按这个口径把**总偏移**往低压，
	 * 于是偏移变成「每发 × 0.72」的几何衰减、几发后就不再上涨 ——
	 * 玩家看到的就是「连发时后坐力失效 / 变成 0」。
	 *
	 * InstantWrite 模式下本值恒为 0，旧公式 = 新公式，既有行为零变化；
	 * Golden 与既有 30 个自动化用例全部建立在该模式下，不受影响。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Internal")
	float RecoveryBasePitch = 0.0f;

	/** 进入 Recovering 时的水平基底（度）。语义同 RecoveryBasePitch。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Internal")
	float RecoveryBaseYaw = 0.0f;

	/**
	 * 本梭的玩家压枪量（度，向下压枪为正，恒 ≥ 0）。
	 *
	 * === 为什么钳制要减掉它 ===
	 *
	 * 垂直钳制（`MaxVerticalKick`）的本意是「**玩家压不住枪的时候，不让镜头飞太高**」——
	 * 那被钳的量就应该是「**镜头实际抬升量**」：
	 *
	 *     镜头实际抬升 = 起枪点 + 偏移 − 当前瞄准 = 偏移 − 压枪量
	 *
	 * 所以正确形式是钳制**净值** `偏移 − 压枪量 ≤ MaxVerticalKick`，
	 * 等价于把裸偏移的允许上限抬到 `MaxVerticalKick + 压枪量`。
	 *
	 * 旧代码钳的是**裸偏移**，于是压枪的人实际只拿到 `MaxVerticalKick − 压枪量` 的净抬升：
	 * 一压枪就**不到上限就封顶**，封顶后镜头不再上抬 —— 体感就是
	 * 「压着枪连发，打着打着后坐力就没了」。
	 *
	 * ★ 2026-09-20 追加（见 Docs/Recoil/11_BurstAccumulationFix.md §12）。
	 *
	 * 这个值由武器实例每帧写入（只有它拿得到 Pawn / Controller），算法层**只消费不推导** ——
	 * 保持「纯数值层无 UWorld 依赖」这条硬性规则。
	 * 默认 0 ⇒ 新旧公式逐位一致，既有 30 个用例零回归。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Internal")
	float AimCompensationPitch = 0.0f;

	/** 回正已经进行的秒数（不含 RecoveryDelay）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Internal")
	float RecoveryElapsed = 0.0f;

	// ---------------------------------------------------------------------
	// 插值模式阶段时间轴（Interpolated 专用）
	//
	// 时间推进走**固定子步长**：Advance() 把 DeltaSeconds 攒进 SubStepAccumulator，
	// 每攒够 1/60 秒执行一次「子步」。这样无论渲染帧率多少，单发轨迹被采样的
	// 点密度恒定为 60Hz —— 20fps 与 144fps 跑出同一条曲线。
	//
	// 详见 Docs/Recoil/10_SingleShotInterpolation.md §4。
	// ---------------------------------------------------------------------

	/** 当前所处的插值阶段。None 表示未在插值推进中。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Interp")
	ERecoilInterpStage InterpStage = ERecoilInterpStage::None;

	/** 当前阶段内已经过的时间（秒）。每切阶段时减去上一阶段的时长。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Interp")
	float StageElapsed = 0.0f;

	/** 本发上抬的完整幅度（度）。t0 从 0 插值到它，后续阶段都以它为基准换算。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Interp")
	float InterpShotAmplitudePitch = 0.0f;

	/** 本发上抬的完整水平幅度（度）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Interp")
	float InterpShotAmplitudeYaw = 0.0f;

	/** 本发开始时的逻辑偏移（度）。上抬是从这里「再抬高 幅度」，不是从 0 开始。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Interp")
	float InterpBasePitch = 0.0f;

	/** 本发开始时的逻辑水平偏移（度）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Interp")
	float InterpBaseYaw = 0.0f;

	/**
	 * 上一子步算出的目标值（度）。「帧间增量」就是靠它做差得到的。
	 * 每执行一次子步就更新，跨帧保留。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Interp")
	float LastTargetPitch = 0.0f;

	/** 上一子步算出的水平目标值（度）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Interp")
	float LastTargetYaw = 0.0f;

	/**
	 * 固定子步长时间累加器（秒）。攒够一个子步就消耗一次。
	 * 帧率不稳时，这一帧攒不够就留到下一帧，攒超了就一次推多个子步。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Interp")
	float SubStepAccumulator = 0.0f;

	/**
	 * 本次 Advance 实际执行的子步次数。调试面板显示用。
	 * 正常情况下 60fps 时为 1，30fps 时为 2；触到上限说明发生了长帧卡顿。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Interp")
	int32 LastSubStepCount = 0;

	/** 本轮连发使用的随机种子。由 Profile 的 RandomSeedMode 决定。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Internal")
	int32 ActiveSeed = 0;

	/** 全局调试倍率（来自 Lyra.Recoil.Scale CVar，默认 1）。不参与手感配置。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Internal")
	float GlobalScale = 1.0f;

	/** 本轮连发的逐发记录。新连发开始或 Reset 时清空。P5 CSV 导出的数据源。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Internal")
	TArray<FRecoilShotResult> ShotHistory;

public:

	// ---------------------------------------------------------------------
	// 帧率保护参数（参考文档 §2：关键逻辑放在固定步长更新中）
	//
	// 这两个值是**结构性常数**，不是手感参数 —— 所以放这里而不是 Profile 上。
	// 手感由阶段时长与曲线决定；这里只负责「无论帧率多少，采样密度一致」。
	// ---------------------------------------------------------------------

	/**
	 * 固定子步长（秒）= 1/60。
	 *
	 * 单发阶段很短（上抬常见 0.03~0.08 秒），若直接拿渲染帧的 DeltaSeconds 推进，
	 * 30fps 下一整段上抬只能采到 2 个点，插值曲线的形状会被抹平成直线。
	 * 以 1/60 秒为单位切分后，采样密度与显示器帧率解耦。
	 */
	static constexpr float FixedSubStepSeconds = 1.0f / 60.0f;

	/**
	 * 单次 Advance 允许执行的最大子步数（防 spiral of death）。
	 *
	 * 若某一帧卡了 500ms，按 1/60 切分会得到 30 个子步 —— 本该卡一帧，结果推 30 次，
	 * 卡得更久。上限 8 个 = 133ms，已覆盖到 7.5fps；再低的帧率下玩家感知不到单发形状，
	 * 继续推只是白白拖慢恢复。超出的时间直接丢弃，同时把当前阶段钳到终点，
	 * 保证状态机不会停在半路。
	 */
	static constexpr int32 MaxSubStepsPerAdvance = 8;

public:

	// ---------------------------------------------------------------------
	// 生命周期
	// ---------------------------------------------------------------------

	/** 全量清零并重新播种。装备/卸下武器时调用。 */
	void Reset(const ULyraRecoilProfile* Profile);

	/** 只清空逐发记录，保留当前偏移与状态。 */
	void ClearHistory();

	/** 设置全局调试倍率（由 CVar 驱动，默认 1.0）。 */
	void SetGlobalScale(float InScale) { GlobalScale = FMath::Max(0.0f, InScale); }

	/**
	 * 设置本梭的玩家压枪量（度，向下压为正）。负值按 0 处理。
	 *
	 * 由武器实例每帧写入，算法层只消费（见 AimCompensationPitch 的说明）。
	 * 默认 0 ⇒ 垂直钳制退化成旧的「裸偏移 ≤ MaxVerticalKick」，零行为变化。
	 */
	void SetAimCompensationPitch(float InCompensationDegrees)
	{
		AimCompensationPitch = FMath::Max(0.0f, InCompensationDegrees);
	}

	/**
	 * 垂直钳制**实际生效**的上限（度）= `MaxVerticalKick + 压枪抵扣`。
	 *
	 * 钳制净值 `偏移 − 压枪量 ≤ MaxVerticalKick` 的等价实现。
	 * 抵扣量夹在「1 个 MaxVerticalKick」以内，于是裸偏移的硬顶 = `2 × MaxVerticalKick` ——
	 * 保留一个安全阀，避免本梭结束后的回正从一个离谱的值开始回落。
	 *
	 * 定义在 .cpp：`ULyraRecoilProfile` 在本头文件里只有前向声明。
	 */
	float GetEffectiveVerticalKickLimit(const ULyraRecoilProfile& Profile) const;

	/**
	 * 设置当前姿态倍率。
	 * 弹道链（GetShotDirectionOffset）走的是"这一刻"的倍率，所以调用方在取弹道偏移
	 * 之前必须先刷新它，否则第一发会用到上一发缓存的值。
	 */
	void SetPoseMultiplier(float InMultiplier) { CurrentPoseMultiplier = FMath::Max(0.0f, InMultiplier); }

	// ---------------------------------------------------------------------
	// 散布（Spread）
	// ---------------------------------------------------------------------

	/**
	 * 设置玩家侧的两个散布倍率（瞄准 / 移动）。由武器实例每帧写入，算法层只消费。
	 *
	 * 做成"外部写入"而不是"内部推导"，理由与 AimCompensationPitch 完全相同：
	 * 这两个倍率要读 Pawn 的速度与相机栈混合权重，而本结构体必须保持无 UWorld 依赖。
	 */
	void SetSpreadPlayerMultipliers(float InAimingMultiplier, float InMovementMultiplier)
	{
		SpreadAimingMultiplier = FMath::Max(0.0f, InAimingMultiplier);
		SpreadMovementMultiplier = FMath::Max(0.0f, InMovementMultiplier);
	}

	/**
	 * 弹道侧在发弹前取一次"本发实际使用的锥角"快照。
	 * 见 PendingShotSpreadAngle 的说明 —— 不这样做会记成下一发的值。
	 */
	void SetPendingShotSpreadAngle(float InAngleDegrees)
	{
		PendingShotSpreadAngle = FMath::Max(0.0f, InAngleDegrees);
	}

	/**
	 * 当前生效的**最终散布角**（度，全锥角）= CurrentSpreadAngle × 瞄准倍率 × 移动倍率。
	 *
	 * 这是喂给 VRandConeNormalDistribution 的值（再 ×0.5 转半角）。
	 * 未启用资产散布时 CurrentSpreadAngle 恒为 0，本函数返回 0 —— 此时散布由
	 * Lyra 原生 heat 链路负责，武器实例会走另一条分支。
	 */
	float GetEffectiveSpreadAngle() const
	{
		return CurrentSpreadAngle * SpreadAimingMultiplier * SpreadMovementMultiplier;
	}

	/** 记录一次开火的散布加热。Profile 为空或未启用资产散布时不改动任何状态。 */
	void ApplySpreadShot(const ULyraRecoilProfile* Profile, EPoseState PoseState);

	/**
	 * 推进散布回落。
	 *
	 * 基准时刻用 TimeSinceLastFire（须先由 Advance 更新）：
	 *   停火时间 <= SpreadRecoveryDelay → 不回落（保持当前锥角）
	 *   之后 → 按当前姿态的 RecoverRate 线性回落，钳到 [Base, Max]
	 *
	 * 姿态每帧传入（而不是沿用 LastPoseState）：蹲下/起立会立刻换一组
	 * Base/Max/RecoverRate，这正是"蹲下马上变准"这条手感的实现点。
	 */
	void AdvanceSpread(const ULyraRecoilProfile* Profile, float DeltaSeconds, EPoseState PoseState);

	/** 清零散布状态。由 Reset 统一调用。 */
	void ResetSpread();

	// ---------------------------------------------------------------------
	// 推进
	// ---------------------------------------------------------------------

	/**
	 * 记录一次开火。会累加到相机链偏移上，**并触发 Roll 震动**，状态置为 Accumulating。
	 *
	 * Roll 震动是独立通道：本方法在累加 Pitch/Yaw 的同时，调用 Profile 的
	 * BuildRollShakeParams 装配本发实际生效的震动参数并 Trigger 一次 RollShake。
	 * 两者互不影响 —— Roll 不进 AccumulatedPitch/Yaw，Pitch/Yaw 也不影响 Roll 包络。
	 *
	 * @param Profile         手感配置资产
	 * @param PoseMultiplier  姿态倍率（含瞄准混合），见 ULyraRecoilProfile::GetPoseMultiplier
	 * @param PoseState       本发生效时的姿态，仅用于 ShotHistory 记录（不参与计算）
	 * @return 本发的完整结果记录；Profile 为空时返回 false 且不做任何改动
	 */
	bool ApplyShot(const ULyraRecoilProfile* Profile, float PoseMultiplier, EPoseState PoseState = EPoseState::Standing);

	/**
	 * 推进时间轴：处理 RecoveryDelay 到期、回正插值、回到 Idle，**并推进 Roll 震动时钟**。
	 * @param Profile       手感配置资产
	 * @param DeltaSeconds  时间增量（秒），由调用方从引擎 tick 取，本层不依赖 UWorld
	 */
	void Advance(const ULyraRecoilProfile* Profile, float DeltaSeconds);

	// ---------------------------------------------------------------------
	// 输出查询
	// ---------------------------------------------------------------------

	// 注：USTRUCT 内不允许出现 UFUNCTION，所以相机链的两个取值函数是普通 C++ 内联函数。
	// 需要 BP 暴露时请走 ULyraRangedWeaponInstance::GetRecoilState() 再读 UPROPERTY 字段。

	/**
	 * 相机链输出：当前应施加到相机的垂直偏移（度）。
	 *
	 * **读的是补间输出 CameraOffsetPitch，不是逻辑偏移 AccumulatedPitch。**
	 * 因为相机每帧只能「转一点」，而逻辑偏移是「这一发应该抬到哪」的目标值。
	 * InstantWrite 模式下两者恒等，所以这条链路对既有行为零影响。
	 */
	float GetCameraPitchOffset() const { return CameraOffsetPitch; }

	/** 相机链输出：当前应施加到相机的水平偏移（度）。语义同 GetCameraPitchOffset。 */
	float GetCameraYawOffset() const { return CameraOffsetYaw; }

	/**
	 * 相机链输出：当前应施加到相机的 Roll 偏移（度，顺时针为正）。
	 *
	 * 与 Pitch/Yaw 不同：这个值**不是累加出来的**，而是 Roll 震动在当前时刻的
	 * 瞬时解（衰减包络 × 周期项 × 振幅）。震动结束后恒为 0。
	 */
	float GetCameraRollOffset() const { return RollShake.CurrentRoll; }

	/**
	 * 弹道链输出（开发计划 §P3）：第 InShotIndex 发的弹道方向偏移（度）。
	 * 纯计算，不改变任何状态；与 ApplyShot 使用同一套算法，保证两条链同源。
	 */
	FRecoilShotKick GetShotDirectionOffset(const ULyraRecoilProfile& Profile, int32 InShotIndex) const;

	/**
	 * 单发偏移的唯一实现。相机链与弹道链都必须走这里。
	 *
	 *   Vertical   = RecoilPerShot_Vertical   × PatternY(ShotIndex) × VerticalKickCurve(ShotIndex) × Mult
	 *   Horizontal = RecoilPerShot_Horizontal × PatternX(ShotIndex)                              × Mult
	 *
	 * Mult = max(0, PoseMultiplier) × GlobalScale
	 */
	static FRecoilShotKick ComputeShotKick(
		const ULyraRecoilProfile& Profile,
		int32 ShotIndex,
		float PoseMultiplier,
		float GlobalScale,
		int32 Seed);

	/**
	 * 带总开关的弹道偏移计算 —— 弹道链的唯一入口。
	 *
	 * 把"开关关闭 / 没配资产 → 返回零偏移"这条规则收在一处，而不是散落在各个调用点。
	 * 好处是它可以被纯数值单测覆盖（P3 断言 #4：开关关闭时弹道偏移恒为 0），
	 * 同时保证"不污染 Lyra 原有扩散逻辑"这条约束只有一个实现点。
	 */
	static FRecoilShotKick ComputeShotKickGated(
		const ULyraRecoilProfile* Profile,
		int32 ShotIndex,
		float PoseMultiplier,
		float GlobalScale,
		int32 Seed,
		bool bRecoilEnabled);

	/**
	 * 取第 ShotIndex 发的归一化水平形状值。
	 * ShotIndex < PatternLength 时严格等于 PatternPoints[ShotIndex].X；
	 * 之后进入确定性随机游走，结果被 Clamp 在 ±HorizontalRandomRange 内。
	 */
	static float ComputePatternHorizontal(const ULyraRecoilProfile& Profile, int32 ShotIndex, int32 Seed);

	/** 解析 Profile 的随机种子模式，得到本轮连发实际使用的种子。 */
	static int32 ResolveSeed(const ULyraRecoilProfile* Profile);

	/**
	 * 由姿态判定推导姿态状态（纯函数，可脱离 Pawn 单测）。
	 * 优先级：空中 > 蹲伏 > 站立 —— 跳着的时候用空中倍率，蹲着跳不算蹲。
	 */
	static EPoseState ResolvePoseState(bool bIsCrouching, bool bIsFalling);

	/**
	 * 姿态倍率 = 姿态倍率 × 瞄准混合倍率（纯函数，可脱离 Pawn 单测）。
	 * 瞄准与姿态正交，所以用相乘而不是把它塞进 EPoseState（见 LyraRecoilTypes.h）。
	 */
	static float ComputePoseMultiplier(const ULyraRecoilProfile& Profile, EPoseState PoseState, float AimingAlpha);
};

#undef UE_API
