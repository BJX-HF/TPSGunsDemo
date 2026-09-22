// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Curves/CurveFloat.h"
#include "Engine/DataAsset.h"
#include "Camera/LyraCameraShakeTypes.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"

#include "LyraRecoilProfile.generated.h"

// Lyra 的跨模块导出约定：本类需要被 LyraEditor 模块（资产生成 Commandlet、
// 资产校验 Automation Test）调用，因此必须显式导出。
// 参见 Weapons/LyraWeaponInstance.h L11 的同款写法。
#define UE_API LYRAGAME_API

class UObject;

/**
 * ULyraRecoilProfile
 *
 * 后坐力手感配置资产。**后坐力系统的唯一数值来源**（见开发计划 §P1）。
 *
 * 设计约束（不可违反）：
 *  - 所有可调手感参数都必须是本资产的 UPROPERTY。运行时代码里不允许出现任何
 *    影响手感的字面量常数（0/1 这类结构性常数除外）。
 *  - 本类只做"取值查询"，不做任何状态累积。状态在 FRecoilRuntimeState 里。
 *  - 资产是只读的：运行时不会写回本对象，因此天然线程安全、可被多把武器共享。
 *
 * 单位约定：所有角度参数单位为"度"；Pitch 向上为正，Yaw 向右为正。
 *
 * 参数备注（编辑器里鼠标悬停在参数名上看到的悬浮提示，2026-09-23 补齐）：
 *  - 每个 UPROPERTY 都带**显式** meta = (ToolTip = "...")，那才是编辑器显示的文案。
 *  - UHT 规则（UhtParsingScope.AddFormattedCommentsAsTooltipMetaData）：
 *    **属性一旦有显式 ToolTip，上方注释就不再参与生成 tooltip**。
 *    所以「改备注」要改 meta，改注释不会有任何效果。
 *  - 上面那些长篇块注释（星号开头的 JavaDoc 风格）仍然保留，作用是代码文档，
 *    不显示在编辑器里。
 *  - ToolTip 里嵌的 \n 会渲染成换行；不要在里面写双引号（用「」代替）。
 *
 * 创建方式（P1 手动验收项）：
 *  Content Browser 右键 → Miscellaneous → Data Asset → 选择 ULyraRecoilProfile。
 */
UCLASS(BlueprintType, meta = (DisplayName = "Lyra Recoil Profile"))
class UE_API ULyraRecoilProfile : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:

	ULyraRecoilProfile();

	//~UObject interface
	virtual void PostLoad() override;
	//~End of UObject interface

#if WITH_EDITOR
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// ---------------------------------------------------------------------
	// 基础（Base）
	// ---------------------------------------------------------------------

	/** 每发基础垂直 Kick（度）。Pattern 的 Y 分量乘以此值得到实际抬枪角度。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Base", meta = (ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "每发基础抬枪角度（度）：本发实际上抬 = Pattern 的 Y 分量 × 本值。\n调大 = 整体后坐力更猛。本值参与 Golden 基线，改完必须重导 Golden。"))
	float RecoilPerShot_Vertical = 0.35f;

	/** 每发基础水平 Kick（度）。Pattern 的 X 分量乘以此值得到实际水平偏移。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Base", meta = (ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "每发基础水平偏移（度）：本发实际水平偏移 = Pattern 的 X 分量 × 本值。\n调大 = 左右摆得更宽。本值参与 Golden 基线，改完必须重导 Golden。"))
	float RecoilPerShot_Horizontal = 0.18f;

	// ---------------------------------------------------------------------
	// 曲线（Curves）
	// ---------------------------------------------------------------------

	/**
	 * 射击序号 → 垂直 Kick 倍率。用于实现"渐强/渐弱"。
	 * X = ShotIndex（从 0 开始），Y = 垂直 Kick 倍率。
	 * 只作用于垂直分量，水平分量不受影响（保证 P3 的 Pattern 水平严格可比对）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Curves",
		meta = (ToolTip = "射击序号 → 垂直 Kick 倍率曲线（X = 第几发，从 0 起；Y = 倍率）。\n用来做「越打越强」或「前几发猛、后面软」。只作用于垂直分量，水平不受影响。\n曲线无数据时按倍率 1.0 处理（不缩放）。"))
	FRuntimeFloatCurve VerticalKickCurve;

	/**
	 * 回正进度曲线。X = 归一化回正时间 [0,1]，Y = 回正进度 [0,1]。
	 * 线性 (0,0)-(1,1) 为匀速回正；上凸为"快回—慢回"，下凸为"慢回—快回"。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Curves",
		meta = (ToolTip = "回正进度曲线（X = 归一化回正时间 0~1；Y = 已回正比例 0~1）。\n直线 = 匀速回正；上凸 = 先快后慢；下凸 = 先慢后快。曲线无数据时按线性处理。"))
	FRuntimeFloatCurve RecoveryCurve;

	// ---------------------------------------------------------------------
	// 单发后坐力模型（SingleShot）
	//
	// 对应《FPS 相机镜头设计与实现》§2「后座」：单发射击拆为多个阶段，
	// 各阶段时长可配、形状由插值曲线控制、每帧向相机推「相对上一帧」的旋转增量。
	//
	// 本项目落地为**三段式**（参考文档是四段）：
	//   t0 上抬 ──► t1 瞬时回弹 ──► [稳定] ──► t2 下降
	//                                  ↑
	//                          复用 RecoveryDelay，不引入重复语义的参数
	//
	// InstantWrite 模式下本组参数完全不参与计算（编辑器里已用 EditCondition 灰掉）。
	// 详细实现与调参说明见 Docs/Recoil/10_SingleShotInterpolation.md。
	// ---------------------------------------------------------------------

	/**
	 * 单发模型开关。
	 *
	 *  InstantWrite  ：瞬时写入（默认）。开火帧直接累加，上抬耗时 0 秒。
	 *  Interpolated  ：插值。按下面的阶段时长与曲线逐帧补间到相机。
	 *
	 * 默认刻意保持 InstantWrite —— 既有资产、3 份 Golden 数据与 24 个自动化测试
	 * 全部建立在瞬时写入语义上，改默认值会导致基线整体失效。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|SingleShot",
		meta = (ToolTip = "单发后坐力模型。\nInstant Write（默认）：开火当帧直接累加，上抬耗时 0 秒；19 份 Golden 与全部自动化测试基线建立在它上面。\nInterpolated：按阶段时长 + 曲线逐帧补间到相机，单发有「抬起来 → 掉一下 → 稳一下 → 慢慢落回」的顿挫感（当前两把步枪用的是它）。\n注意：改默认值会让既有 Golden 与自动化测试基线整体失效。"))
	ERecoilSingleShotMode SingleShotMode = ERecoilSingleShotMode::InstantWrite;

	/**
	 * t0 上抬段时长（秒）。从 0 抬到本发完整幅度所需的时间。
	 *
	 * 怎么调：
	 *   调长 → 上抬更「肉」，观感更拖沓，但每发的推进过程更可见
	 *   调短 → 更接近瞬时写入；短于一个固定子步长(1/60s)时基本退化为瞬时
	 *
	 * 连发注意：本值 + ReboundDuration 应小于射击间隔，否则连发时每发的上抬
	 * 都走不完就被下一发重置（观感变成「持续被推高」，见实现文档 §7）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|SingleShot",
		meta = (EditCondition = "SingleShotMode == ERecoilSingleShotMode::Interpolated",
			ForceUnits = s, ClampMin = "0.0",
			ToolTip = "【仅 Interpolated 生效】上抬段时长（秒）：从 0 抬到本发满幅所需的时间。\n调长 = 上抬更肉、更拖沓；调短 = 接近瞬时写入（短于 1/60 秒基本退化为瞬时）。\n连发注意：本值 + 回弹时长应小于射击间隔，否则每发的上抬还没走完就被下一发重置，观感变成「一直被推高」。"))
	float LiftDuration = 0.045f;

	/**
	 * t1 瞬时回弹段时长（秒）。上抬到顶后往回掉所需的时间。
	 * 对应参考文档 §2 的「瞬时回弹」—— 特征是「在进入稳定段之前回弹到一个比例」。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|SingleShot",
		meta = (EditCondition = "SingleShotMode == ERecoilSingleShotMode::Interpolated",
			ForceUnits = s, ClampMin = "0.0",
			ToolTip = "【仅 Interpolated 生效】瞬时回弹段时长（秒）：上抬到顶后往回掉所用的时间。\n短促的「掉一下」，常用 0.02 ~ 0.04。"))
	float ReboundDuration = 0.030f;

	/**
	 * 回弹比例 [0,1]。上抬峰值回弹到「峰值 × 本值」。
	 *   1.0 = 不回弹（上抬到顶直接平稳下降）
	 *   0.7 = 掉 30%，这是最有「一顿」感觉的区间
	 *   0.0 = 直接掉回零（不要这么配，会有明显断层）
	 *
	 * 参考文档 §2.2 参数表里的「瞬时回弹比例」。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|SingleShot",
		meta = (EditCondition = "SingleShotMode == ERecoilSingleShotMode::Interpolated",
			ClampMin = "0.0", ClampMax = "1.0",
			ToolTip = "【仅 Interpolated 生效】回弹比例 0~1：抬到峰值后回弹到「峰值 × 本值」。\n1.0 = 不回弹（直接平稳下降）；0.7 左右最有「一顿」的手感；0 = 直接掉回零（会有明显断层，不建议）。"))
	float ReboundRatio = 0.72f;

	/**
	 * 上抬曲线。X = t0 段归一化进度 [0,1]，Y = 上抬完成度 [0,1]。
	 *
	 * 默认 Ease-Out（先快后慢）—— 贴近枪机冲量驱动的物理过程，
	 * 且在前 1/3 段就完成约 60% 位移，低帧率下仍能保住「主要位移」而不是零位移。
	 * 无数据时退化为线性。
	 *
	 * 参考文档 §2 伪码里的「按上抬曲线插值(0, 总幅度, 进度)」。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|SingleShot",
		meta = (EditCondition = "SingleShotMode == ERecoilSingleShotMode::Interpolated",
		ToolTip = "【仅 Interpolated 生效】上抬曲线（X = 上抬段进度 0~1；Y = 上抬完成度 0~1）。\n默认先快后慢，贴近枪机冲量驱动的物理过程，低帧率下也能保住主要位移。曲线无数据时按线性处理。"))
	FRuntimeFloatCurve LiftCurve;

	/**
	 * 回弹曲线。X = t1 段归一化进度 [0,1]，Y = 回弹完成度 [0,1]。
	 * 回弹是个短促的「掉一下」，形状不敏感，**线性即可**，不建议在这里做花样。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|SingleShot",
		meta = (EditCondition = "SingleShotMode == ERecoilSingleShotMode::Interpolated",
		ToolTip = "【仅 Interpolated 生效】回弹曲线（X = 回弹段进度 0~1；Y = 回弹完成度 0~1）。\n回弹是个短促的「掉一下」，形状不敏感，保持线性即可。曲线无数据时按线性处理。"))
	FRuntimeFloatCurve ReboundCurve;

	// ---------------------------------------------------------------------
	// 恢复（Recovery）
	// ---------------------------------------------------------------------

	/**
	 * 停火后开始回正的延迟（秒）。此区间内状态保持 Accumulating。
	 *
	 * Interpolated 模式下，本参数同时充当参考文档 §2 的「t2 稳定段」——
	 * 三段的中间段。这不是巧合：停火延迟与稳定段在语义上是同一件事
	 * （「偏移先冻住不动，然后才开始回落」），所以本项目刻意不引入第二个参数。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Recovery", meta = (ForceUnits = s, ClampMin = "0.0",
		ToolTip = "停火后延迟多久才开始回正（秒）。这段时间里偏移冻住不动，状态仍算 Accumulating。\n调大 = 「顿」得更久、回正启动更晚；它同时也是 Interpolated 模型里的「稳定段」时长（刻意没再单开一个参数）。"))
	float RecoveryDelay = 0.15f;

	/**
	 * 回正总时长（秒）。必须 > 0。
	 * Interpolated 模式下它是 t2 下降段（参考文档的「下降 / 回正」）的时长，
	 * 形状由 RecoveryCurve 整形。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Recovery", meta = (ForceUnits = s, ClampMin = "0.01",
		ToolTip = "回正总时长（秒），必须 > 0：镜头从当前偏移走回终值所需的时间，形状由 RecoveryCurve 整形。\n调大 = 收得更慢更黏；调小 = 更快贴回准心。"))
	float RecoveryTime = 0.35f;

	/**
	 * ★ 2026-09-21 已删除 `RecoilReturnRatio`。
	 *
	 * 原字段语义是「不压枪时镜头残留 = 峰值 × Ratio」（0 = 完全回正 / 1 = 完全不回）。
	 * 大祥老师拍板删除，理由：这是不被要求的设计 —— 期望表现是**镜头停在哪完全由
	 * 「玩家压了多少」决定**，不由一个额外的残留比例二次缩放。
	 *
	 * 现行回正口径（唯一）：终止值 = min(本梭累计压枪量, 本轮后坐力峰值)（见 ComputeRecoveryTarget）。
	 * 场景对照：枪抬 10°、压 5° → 偏移停 5°（屏幕回开枪前）；完全不压 → 偏移回满 0°（同样回开枪前）。
	 */

	/**
	 * 回正时是否把「后坐力偏移」收敛到玩家的累计压枪量。**默认 true。**
	 *
	 * true（默认）：**终止值 = min(本梭累计压枪量, 本轮后坐力峰值)**
	 *   玩家往下压 4° → 终止值 = 4（因为 Ctrl 也低了 4°，屏幕恰好回到开枪前）
	 *   完全不压枪    → 终止值 = 0（偏移回满，屏幕回开枪前）
	 *   压过头        → 偏移最多保留到峰值，不反向补偿；屏幕保留超压角度
	 *
	 * false：不抵扣，回正目标直接 = 0（偏移完全回满），保留 A/B 对照能力。
	 *
	 * 累积量取的是**本梭累计**（单调不减、停火后冻结），不是实时值 ——
	 * 理由见 LyraRecoilState.h 里 RecoveryCoverPitch 的注释：停火后玩家必然松手，
	 * 读实时值会让回正目标在半路跳回旧值（非单调甩镜）。
	 *
	 * 实现与验收见 Docs/Recoil/11_BurstAccumulationFix.md §13。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Recovery",
		meta = (ToolTip = "回正时是否把「玩家压的枪」抵扣掉（默认开）。\n开：终止偏移 = min(本梭累计压枪量, 本轮峰值) —— 压多少认多少，屏幕回到开枪前；压过头则保留超压角度。\n关：一律回满到 0，保留 A/B 对照能力。"))
	bool bCompensationAwareRecovery = true;

	/**
	 * ★ 2026-09-21 已删除 `RecoilCompensationMinResidualRatio`（残留地板）。
	 *
	 * 原字段给"抵扣后可能为负的终止值"设一条下限，默认 0 = 不设地板 = 开关无效，
	 * 属于**不被要求的额外设计**（大祥老师 2026-09-21：「以后如果我没要求别做这种自以为是的设计」）。
	 *
	 * 现行口径：`终止值 = min(本梭累计压枪量, 本轮后坐力峰值)`。
	 */

	/**
	 * 水平（Yaw）轴是否**也**扣压枪量。**默认 false。**
	 *
	 * === 为什么默认关掉（2026-09-21）===
	 *
	 * 水平方向不存在"压枪"这个动作。玩家在连发中往左右动的鼠标是**转身追目标**，
	 * 不是对抗后坐力 —— 但采样口径对它一视同仁，全都被记成压枪量。
	 *
	 * 后果比垂直轴严重得多：垂直要把偏移顶到峰值才会归零，
	 * 而水平峰值上限 MaxHorizontalKick 只有 2.0°，实机里"转身超过它"是随时发生的动作
	 * ⇒ 水平回正量**长期恒为 0**。
	 *
	 * true ：两轴同规则（水平也收敛到「位移量」，保留 A/B 对照能力）。
	 * false（默认）：Yaw 偏移回满到 0。压枪量照常被记录
	 *        （调试面板仍能看数），只是不参与回正。
	 *
	 * 实现与验收见 Docs/Recoil/11_RecoveryCompensation.md §3.3。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Recovery",
		meta = (ToolTip = "水平（Yaw）轴是否也同样抵扣压枪量（默认关）。\n关：水平偏移一律回满到 0；压枪量照常被记录（调试面板仍能看数），只是不参与回正。\n开：两轴同规则 —— 但水平峰值上限通常只有几度，玩家转身很容易超过它，会导致水平长期不回正。"))
	bool bCompensationAwareRecoveryYaw = false;

	// ---------------------------------------------------------------------
	// 上限（Clamp）
	// ---------------------------------------------------------------------

	/** 垂直累加偏移上限（度）。必须 > 0。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Clamp", meta = (ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "垂直累加偏移上限（度），必须 > 0：连发抬枪抬到这么多就封顶，不再继续往上。\n调大 = 长按能抬得更高。玩家压枪时会实时抵扣这个上限，所以压枪的人能多抬一点。"))
	float MaxVerticalKick = 8.0f;

	/** 水平累加偏移上限（度）。必须 > 0。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Clamp", meta = (ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "水平累加偏移上限（度），必须 > 0：左右偏移到这么多就封顶。\n调大 = 连发时左右摆得更开。"))
	float MaxHorizontalKick = 4.0f;

	// ---------------------------------------------------------------------
	// Pattern
	// ---------------------------------------------------------------------

	/** 归一化 Pattern 点数组。索引即 ShotIndex。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Pattern",
		meta = (ToolTip = "归一化弹道图案点数组，索引 = 第几发（从 0 起）。\nX = 水平（右为正，-1~1），Y = 垂直（上为正，0~1）；只描述形状，实际幅度由 RecoilPerShot_Vertical / RecoilPerShot_Horizontal 缩放 —— 所以改威力不会破坏图案形状。\n本数组参与 Golden 基线，改完必须重导 Golden。"))
	TArray<FRecoilPatternPoint> PatternPoints;

	/** 固定 Pattern 覆盖的发数。必须 <= PatternPoints.Num()。超出后进入伪随机区间。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Pattern", meta = (ClampMin = "0",
		ToolTip = "固定图案覆盖的发数，必须 <= PatternPoints 的长度。\n前 N 发严格按图案走，之后的发进入伪随机游走区。本值参与 Golden 基线，改完必须重导 Golden。"))
	int32 PatternLength = 8;

	/**
	 * 固定 Pattern 之后的水平随机游走幅度（归一化单位，与 PatternPoints.X 同量纲）。
	 * 游走累计值被 Clamp 在 [-HorizontalRandomRange, +HorizontalRandomRange]。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Pattern", meta = (ClampMin = "0.0", ClampMax = "1.0",
		ToolTip = "固定图案之后的水平随机游走幅度（归一化单位，与 PatternPoints 的 X 同量纲）：游走累计值被夹在 ± 本值内。\n调大 = 后半程更飘；0 = 完全不额外打偏。本值参与 Golden 基线。"))
	float HorizontalRandomRange = 0.6f;

	// ---------------------------------------------------------------------
	// Roll 震屏（独立通道，与 Pitch/Yaw 的累加-回正无关）
	// ---------------------------------------------------------------------
	//
	// 对应《FPS 相机镜头设计与实现》§2.1：Roll 表现"开火瞬间的爆发感与后续释放"，
	// 是「衰减包络 × 周期震动」，按当前时刻直接求解，**不累加、不回正**。
	// 与上面的 Recoil|Base / Curves / Recovery 完全是两套机制，不要混着调。

	/** Roll 震动总开关。关闭时本武器的 Roll 恒为 0（不影响 Pitch/Yaw）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|RollShake",
		meta = (ToolTip = "Roll 震屏总开关（画面左右歪一下的那条独立通道）。\n关闭 = 本武器 Roll 恒为 0，Pitch / Yaw 完全不受影响。"))
	bool bEnableRollShake = true;

	/** 每发的基础 Roll 振幅（度）。实际振幅 = 本值 × 连射增量 × 分段系数 × 姿态倍率。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|RollShake",
		meta = (EditCondition = "bEnableRollShake", ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "每发基础 Roll 振幅（度）。实际振幅 = 本值 × 连射增量 × 分段系数 × 姿态倍率。\n调大 = 开火瞬间画面歪得更明显。临时试量级可用 Lyra.Recoil.RollShake <值>，不必改资产。"))
	float RollShake_Amplitude = 0.6f;

	/** 震动总时长（秒）。超过后本发震动结束，Roll 归零。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|RollShake",
		meta = (EditCondition = "bEnableRollShake", ForceUnits = s, ClampMin = "0.01",
		ToolTip = "震动总时长（秒）。超过后本发震动结束、Roll 归零。"))
	float RollShake_Duration = 0.22f;

	/** 震动周期（秒）。越小抖得越快；通常落在 0.04~0.10 之间。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|RollShake",
		meta = (EditCondition = "bEnableRollShake", ForceUnits = s, ClampMin = "0.01",
		ToolTip = "震动周期（秒）。越小抖得越快，通常落在 0.04 ~ 0.10。"))
	float RollShake_Period = 0.055f;

	/**
	 * 相位扰动范围（弧度）。为每次震动引入一个小的相位随机，避免连发时
	 * 每一次震颤都从完全相同的姿态开始（听感/视觉上会显得机械）。
	 * 0 = 完全可复现（自动化测试与 Golden 数据用 0）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|RollShake",
		meta = (EditCondition = "bEnableRollShake", ForceUnits = rad, ClampMin = "0.0", ClampMax = "3.14159",
		ToolTip = "相位随机扰动（弧度）：给每次震动一个随机起始相位，避免连发时每一下都抖得一模一样。\n0 = 完全可复现 —— 自动化测试与 Golden 数据必须用 0。"))
	float RollShake_PhaseJitter = 0.35f;

	/**
	 * 衰减曲线。X = 归一化震动时间 [0,1]，Y = 振幅保留比例 [0,1]。
	 * 线性 (0,1)-(1,0) 为匀速衰减；下凸为"先猛后缓"（更接近真实枪械的爆发后释放）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|RollShake",
		meta = (EditCondition = "bEnableRollShake",
		ToolTip = "衰减曲线（X = 归一化震动时间 0~1；Y = 振幅保留比例 0~1）。\n直线 = 匀速衰减；下凸 = 先猛后缓（更接近真实枪械的爆发后释放）。"))
	FRuntimeFloatCurve RollShake_AmplitudeCurve;

	/**
	 * 衰减终值比例 [0,1]。0 = 震到零；>0 时末段保留一个稳定偏角（模拟"被压住的镜头"）。
	 * 绝大多数情况应保持 0。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|RollShake",
		meta = (EditCondition = "bEnableRollShake", ClampMin = "0.0", ClampMax = "1.0",
		ToolTip = "衰减终值比例 0~1。0 = 震回零；大于 0 时末段保留一个稳定偏角（模拟「被压住的镜头」）。\n绝大多数情况保持 0。"))
	float RollShake_EndAmplitudeRatio = 0.0f;

	/**
	 * 连射附加振幅增量（度/发）。从第 RollShake_RampStartShot 发起，
	 * 每多打一发振幅增加本值，直到 RollShake_MaxAmplitudeBonus 封顶。
	 * 用来表现"压不住枪、越打越抖"。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|RollShake",
		meta = (EditCondition = "bEnableRollShake", ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "连射附加振幅（度/发）：从「起始发数」起，每多打一发振幅加这么多，直到封顶。\n用来表现「压不住枪、越打越抖」。"))
	float RollShake_AmplitudePerShot = 0.05f;

	/** 连射增量的起始发数（0 起）。此前不发散，避免点射也被加抖。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|RollShake",
		meta = (EditCondition = "bEnableRollShake", ClampMin = "0",
		ToolTip = "连射增量的起始发数（0 起）。在这之前不额外加抖，避免点射也被加上抖动。"))
	int32 RollShake_RampStartShot = 4;

	/** 连射增量上限（度）。与 RollShake_Amplitude 相加后作为最终振幅。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|RollShake",
		meta = (EditCondition = "bEnableRollShake", ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "连射增量上限（度）：基础振幅与本增量相加后到此封顶。"))
	float RollShake_MaxAmplitudeBonus = 0.9f;

	/**
	 * 分段系数曲线。X = 连射序号（0 起），Y = 整体振幅倍率。
	 * 用于"前几发轻、后几发重"这类分段手感；无数据时视为恒定 1.0。
	 * 与 RollShake_AmplitudePerShot 的区别：本曲线是**倍率**（可做先降后升），
	 * 增量是**线性叠加**（只增不减）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|RollShake",
		meta = (EditCondition = "bEnableRollShake",
		ToolTip = "分段系数曲线（X = 连射序号；Y = 整体振幅倍率），用来做「前几发轻、后几发重」。\n与「每发附加振幅」的区别：本曲线是倍率（可以先降后升），那个是只增不减的线性叠加。无数据 = 恒 1.0。"))
	FRuntimeFloatCurve RollShake_SegmentScaleCurve;

	/**
	 * 周期分段系数曲线。X = 连射序号，Y = 周期倍率。
	 * 用于"越打越快"（Y < 1 时周期变短、震动变急）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|RollShake",
		meta = (EditCondition = "bEnableRollShake",
		ToolTip = "周期分段系数曲线（X = 连射序号；Y = 周期倍率）。\nY 小于 1 = 周期变短、越打越快越急。无数据 = 恒 1.0。"))
	FRuntimeFloatCurve RollShake_PeriodScaleCurve;

	// ---------------------------------------------------------------------
	// 倍率（Multipliers）
	// ---------------------------------------------------------------------

	/** 瞄准时的后坐力倍率。与姿态倍率相乘。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Multipliers", meta = (ForceUnits = x, ClampMin = "0.01", ClampMax = "5.0",
		ToolTip = "瞄准时的后坐力倍率（与姿态倍率相乘）。1.0 = 不减免；0.75 = 瞄准时只有 75%。"))
	float PoseMultiplier_Aiming = 0.75f;

	/** 站立（非蹲、非空中）倍率。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Multipliers", meta = (ForceUnits = x, ClampMin = "0.01", ClampMax = "5.0",
		ToolTip = "站立（非蹲、非空中）时的后坐力倍率。"))
	float PoseMultiplier_Standing = 1.0f;

	/** 蹲伏倍率。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Multipliers", meta = (ForceUnits = x, ClampMin = "0.01", ClampMax = "5.0",
		ToolTip = "蹲伏时的后坐力倍率。小于 1 = 蹲下更稳。"))
	float PoseMultiplier_Crouching = 0.8f;

	/** 跳跃/下落倍率。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Multipliers", meta = (ForceUnits = x, ClampMin = "0.01", ClampMax = "5.0",
		ToolTip = "跳跃 / 下落时的后坐力倍率。大于 1 = 空中更难控。"))
	float PoseMultiplier_JumpingOrFalling = 1.5f;

	// ---------------------------------------------------------------------
	// 散布（Spread）—— 姿态-角度直接模型
	//
	// === 这一组参数在做什么 ===
	//
	// Lyra 原生散布是「heat → 三曲线」的间接模型：
	//   每发把 CurrentHeat 加一点（HeatToHeatPerShotCurve）→
	//   heat 查 HeatToSpreadCurve 得到角度 → 停火按 HeatToCoolDownPerSecondCurve 降温。
	// 想配「最大散布 2 度」得反推曲线端点，想配「每发加 0.3 度」得反推 heat 增量，
	// 而且**中间那层 heat 会在换枪（OnEquipped）时被初始化成 range 中点**，
	// 于是第一发的散布不是基础值 —— 这些都不直观。
	//
	// 本组参数把中间层删掉，直接配角度：
	//
	//     CurrentSpreadAngle ──每发 +AddPerShot──► 封顶 Max
	//                       ◄──停火 -RecoverRate×dt── Base
	//
	// 参数形状参考 DLC36 的 FWeaponFireParam 散布族（Stand/Move/Rush/Aim 四套），
	// 但姿态沿用 Lyra 原有的 EPoseState 三态（站定 / 蹲伏 / 空中），
	// 移动（站定 ↔ 跑动）不走独立姿态，而是用一组速度 ramp 倍率做插值 ——
	// 这样不必新增枚举、不必改 FRecoilRuntimeState::ResolvePoseState，
	// 也就不会碰任何既有测试的基线。
	//
	// === 开关语义 ===
	// bEnableProfileSpread == false（默认）时，本组参数**完全不参与计算**，
	// 散布 100% 走 Lyra 原生 heat 模型 —— 保证既有资产、既有手感零回归。
	// 打开它，则武器实例上的 Lyra 原生散布字段（HeatToSpreadCurve 等）全部被忽略，
	// 仅作为「关掉本开关后的回退配置」保留（标了 DeprecationMessage）。
	//
	// 详细设计、调参建议与验收口径见 Docs/Recoil/12_SpreadInProfile.md。
	// ---------------------------------------------------------------------

	/**
	 * 资产散布总开关（默认 false）。
	 *
	 *  false = 走 Lyra 原生 heat 散布模型，本组参数全部不生效（零回归）
	 *  true  = 走本资产的姿态-角度直接模型，武器实例上的 heat 三曲线被忽略
	 *
	 * 与 Lyra.Recoil.Enable 是**两件事**：那个关的是后坐力，这个管的是散布。
	 * 所以你可以「关掉后坐力、只调散布」做 A/B 对比。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread",
		meta = (DisplayName = "Enable Profile Spread",
		ToolTip = "散布总开关（默认关）。\n关：走 Lyra 原生的 heat 散布模型，下面 Recoil|Spread 这一整组参数完全不参与计算（零回归）。\n开：改走本资产的「姿态 → 直接配角度」模型，武器实例上的 heat 三曲线被忽略（仅作回退配置保留）。\n它和「关掉后坐力」是两件事 —— 所以可以只调散布做 A/B 对比。"))
	bool bEnableProfileSpread = false;

	// ---------------- 站定（Standing）----------------

	/** 站定基础散布角（度，全锥角）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Standing",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "站定时的基础散布角（度，全锥角）：停火够久之后回落到的值。"))
	float SpreadAngle_Standing = 0.35f;

	/** 站定上限散布角（度，全锥角）。必须 >= SpreadAngle_Standing。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Standing",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "站定时的散布上限（度，全锥角）：连射累加到这就封顶。必须 >= 基础散布角。"))
	float MaxSpreadAngle_Standing = 2.2f;

	/** 站定每发增量（度）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Standing",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "站定每开一枪散布增加多少（度）。调大 = 连发掉精度掉得更快。"))
	float SpreadAddPerShot_Standing = 0.28f;

	/** 站定回落速率（度/秒）。0 = 永不下落。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Standing",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = "deg/s", ClampMin = "0.0",
		ToolTip = "站定停火后散布回落速率（度/秒）。0 = 永不回落（永久残留）。"))
	float SpreadRecoverRate_Standing = 2.0f;

	// ---------------- 蹲伏（Crouching）----------------

	/** 蹲伏基础散布角（度，全锥角）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Crouching",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "蹲伏时的基础散布角（度，全锥角）。"))
	float SpreadAngle_Crouching = 0.25f;

	/** 蹲伏上限散布角（度，全锥角）。必须 >= SpreadAngle_Crouching。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Crouching",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "蹲伏时的散布上限（度，全锥角）。必须 >= 基础散布角。"))
	float MaxSpreadAngle_Crouching = 1.6f;

	/** 蹲伏每发增量（度）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Crouching",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "蹲伏每开一枪散布增加多少（度）。"))
	float SpreadAddPerShot_Crouching = 0.22f;

	/** 蹲伏回落速率（度/秒）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Crouching",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = "deg/s", ClampMin = "0.0",
		ToolTip = "蹲伏停火后散布回落速率（度/秒）。"))
	float SpreadRecoverRate_Crouching = 2.4f;

	// ---------------- 空中（JumpingOrFalling）----------------

	/** 空中基础散布角（度，全锥角）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|JumpingOrFalling",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "空中（跳跃 / 下落）时的基础散布角（度，全锥角）。"))
	float SpreadAngle_JumpingOrFalling = 2.5f;

	/** 空中上限散布角（度，全锥角）。必须 >= SpreadAngle_JumpingOrFalling。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|JumpingOrFalling",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "空中时的散布上限（度，全锥角）。必须 >= 基础散布角。"))
	float MaxSpreadAngle_JumpingOrFalling = 4.0f;

	/** 空中每发增量（度）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|JumpingOrFalling",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = deg, ClampMin = "0.0",
		ToolTip = "空中每开一枪散布增加多少（度）。"))
	float SpreadAddPerShot_JumpingOrFalling = 0.35f;

	/** 空中回落速率（度/秒）。落地后才有意义；空中通常给 0 让它不回落。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|JumpingOrFalling",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = "deg/s", ClampMin = "0.0",
		ToolTip = "空中停火后散布回落速率（度/秒）。空中一般给 0 让它不回落，落地后才有意义。"))
	float SpreadRecoverRate_JumpingOrFalling = 0.0f;

	// ---------------- 玩家侧（瞄准 / 移动）----------------
	//
	// 这两条链路是**倍率**，乘在最终锥角上；与原 Lyra 的对应关系：
	//   瞄准   ← SpreadAngleMultiplier_Aiming        （混合权重来自相机栈，逐帧连续）
	//   移动   ← SpreadAngleMultiplier_StandingStill + 速度阈值 + 过渡速率
	// 原字段有 4 个（含 3 个 TransitionRate_*），这里只保留**站定**这一路过渡，
	// 蹲伏与空中的姿态切换是瞬时的（与后坐力侧的姿态倍率口径一致：
	// 「每发按当下的姿态取值」，不做插值，P4 的比值断言才成立）。

	/** 瞄准满时的散布倍率。实际值 = Lerp(1, 本值, AimingAlpha)，AimingAlpha 为相机混合权重。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Player",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = x, ClampMin = "0.01", ClampMax = "5.0",
		ToolTip = "瞄准满时的散布倍率：实际 = Lerp(1, 本值, 瞄准混合权重)。小于 1 = 瞄准更准。"))
	float SpreadMultiplier_Aiming = 0.6f;

	/** 站定（速度 <= 阈值）时的散布倍率。速度升高后线性插值到 1.0。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Player",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = x, ClampMin = "0.01", ClampMax = "5.0",
		ToolTip = "站定（速度不超过阈值）时的散布倍率。速度升高后线性插值回 1.0。"))
	float SpreadMultiplier_StandingStill = 0.5f;

	/** 速度阈值（cm/s）。不超过它算「站定」。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Player",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = "cm/s", ClampMin = "0.0",
		ToolTip = "判定「站定」的速度阈值（cm/s）。不超过它算站定。"))
	float SpreadStandingStillSpeedThreshold = 80.0f;

	/** 阈值之上的过渡带宽（cm/s）。达到 阈值+带宽 时倍率回到 1.0。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Player",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = "cm/s", ClampMin = "0.0",
		ToolTip = "阈值之上的过渡带宽（cm/s）：速度到「阈值 + 带宽」时倍率回到 1.0。"))
	float SpreadStandingStillToMovingRange = 20.0f;

	/** 站定倍率的过渡速率（1/FInterpTo 的 InterpSpeed）。越大越跟手，0 = 瞬时。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Player",
		meta = (EditCondition = "bEnableProfileSpread", ClampMin = "0.0",
		ToolTip = "站定倍率的过渡速率：越大越跟手，0 = 瞬时切换。"))
	float SpreadTransitionRate_StandingStill = 5.0f;

	/** 停火后延迟多久开始回落（秒）。0 = 立即回落。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Player",
		meta = (EditCondition = "bEnableProfileSpread", ForceUnits = s, ClampMin = "0.0",
		ToolTip = "停火后延迟多久开始回落（秒）。0 = 立刻回落。"))
	float SpreadRecoveryDelay = 0.0f;

	/**
	 * 散布收敛指数。喂给 VRandConeNormalDistribution 的形状参数：
	 *   1.0 = 锥内均匀分布；> 1 = 更向中心聚拢（弹着更密集）。
	 * 只在 bEnableProfileSpread == true 时生效（关闭时用武器实例上的同名旧字段）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Spread|Player",
		meta = (EditCondition = "bEnableProfileSpread", ClampMin = "0.1",
		ToolTip = "散布收敛指数：1.0 = 锥内均匀分布；大于 1 = 更向中心聚拢，弹着更密集。"))
	float SpreadExponent = 1.0f;

	// ---------------------------------------------------------------------
	// 随机（Random）
	// ---------------------------------------------------------------------

	/** 随机种子模式。自动化测试与 Golden 数据必须使用 Fixed。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Random",
		meta = (ToolTip = "随机种子模式。\nFixed（默认）：序列完全可复现 —— 自动化测试与 Golden 数据必须用它。\nRandom：每次开火重新播种，手感更「活」但不可复现。"))
	ERecoilRandomSeedMode RandomSeedMode = ERecoilRandomSeedMode::Fixed;

	/** RandomSeedMode == Fixed 时使用的种子。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil|Random", meta = (EditCondition = "RandomSeedMode == ERecoilRandomSeedMode::Fixed",
		ToolTip = "Fixed 模式下使用的种子值。改它会让固定图案之后的伪随机段弹道整体变化 —— 本值参与 Golden 基线，改完必须重导 Golden。"))
	int32 FixedRandomSeed = 20260917;

public:

	// ---------------------------------------------------------------------
	// 查询接口（运行时只读，无状态）
	// ---------------------------------------------------------------------

	/** 取 VerticalKickCurve 在 ShotIndex 处的倍率。曲线无数据时返回 1（不做任何缩放）。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Query")
	float GetVerticalKickCurveScale(int32 ShotIndex) const;

	/** 取 RecoveryCurve 在 NormalizedTime 处的回正进度，已 Clamp 到 [0,1]。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Query")
	float GetRecoveryAlpha(float NormalizedTime) const;

	/**
	 * ★ 2026-09-21：`GetRecoilCompensationResidualFloor()` / `HasRecoilCompensationResidualFloor()`
	 * 随 `RecoilCompensationMinResidualRatio` 一并删除。回正终止值现为字面减法、无地板。
	 */

	/**
	 * 取 LiftCurve 在 NormalizedTime 处的上抬完成度，已 Clamp 到 [0,1]。
	 *
	 * 对应参考文档 §2 伪码的「按上抬曲线插值(0, 总幅度, 进度)」——
	 * 这里只返回归一化的进度值，实际幅度由调用方乘以本发 Kick 得到。
	 * 曲线无数据时退化为线性（返回 ClampedTime），保证系统永远可用。
	 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Query")
	float GetLiftAlpha(float NormalizedTime) const;

	/**
	 * 取 ReboundCurve 在 NormalizedTime 处的回弹完成度，已 Clamp 到 [0,1]。
	 * 曲线无数据时退化为线性。
	 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Query")
	float GetReboundAlpha(float NormalizedTime) const;

	/** 本资产是否走插值单发模型。等价于 SingleShotMode == Interpolated。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Query")
	bool IsInterpolatedSingleShot() const { return SingleShotMode == ERecoilSingleShotMode::Interpolated; }

	/** 取 ShotIndex 对应的归一化 Pattern 点。越界时返回最后一点。空数组时返回 (0,1)。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Query")
	FRecoilPatternPoint GetPatternPoint(int32 ShotIndex) const;

	/** ShotIndex 是否落在固定 Pattern 区间内。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Query")
	bool IsFixedPatternShot(int32 ShotIndex) const { return ShotIndex >= 0 && ShotIndex < PatternLength; }

	/** 取姿态倍率。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Query")
	float GetPoseMultiplier(EPoseState PoseState) const;

	/**
	 * 取第 ShotIndex 发的 Roll 分段系数（整体振幅倍率）。
	 * RollShake_SegmentScaleCurve 无数据时返回 1.0。
	 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Query")
	float GetRollShakeSegmentScale(int32 ShotIndex) const;

	/**
	 * 取第 ShotIndex 发的周期倍率。
	 * RollShake_PeriodScaleCurve 无数据时返回 1.0。
	 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Query")
	float GetRollShakePeriodScale(int32 ShotIndex) const;

	/**
	 * 按连射序号计算本发实际生效的 Roll 震动参数（纯查询，不改状态）。
	 *
	 * 振幅 = clamp(RollShake_Amplitude + 连射增量, 上限) × 分段系数 × PoseMultiplier × GlobalScale
	 * 周期 = RollShake_Period × 周期分段系数
	 *
	 * 这是 Roll 参数的唯一装配点：算法层只认 FCameraRollShakeParams，不认识 Profile。
	 *
	 * @param ShotIndex       本发序号（0 起）
	 * @param PoseMultiplier  姿态倍率（含瞄准混合），语义与 Pitch/Yaw 链一致
	 * @param GlobalScale     全局调试倍率
	 * @param Seed            本轮连发种子，用于相位扰动
	 * @param OutParams       写出的生效参数
	 * @return 本发是否需要产生震动（总开关关闭或振幅为 0 时返回 false）
	 */
	bool BuildRollShakeParams(
		int32 ShotIndex,
		float PoseMultiplier,
		float GlobalScale,
		int32 Seed,
		FCameraRollShakeParams& OutParams) const;

	/** 瞄准混合后的倍率：Lerp(1, PoseMultiplier_Aiming, AimingAlpha)。AimingAlpha 为 [0,1] 的相机混合权重。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Query")
	float GetAimingBlendedMultiplier(float AimingAlpha) const;

	// ---------------------------------------------------------------------
	// 散布查询（姿态-角度直接模型）
	// ---------------------------------------------------------------------

	/**
	 * 取某个姿态下的散布参数（基础角 / 上限角 / 每发增量 / 回落速率）。
	 *
	 * 出口处会把 Max 兜到 >= Base：资产填错时宁可让连射「一上来就顶到上限」，
	 * 也不要让第一发被钳到一个比基础角还小的值（那会出现「开火反而更准」的怪现象）。
	 * 资产校验（ValidateProfile）会独立报出这种配置错误。
	 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Spread|Query")
	FRecoilSpreadParams GetSpreadParams(EPoseState PoseState) const;

	/** 瞄准倍率：Lerp(1, SpreadMultiplier_Aiming, AimingAlpha)。语义与 GetAimingBlendedMultiplier 一致。 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Spread|Query")
	float GetSpreadAimingMultiplier(float AimingAlpha) const;

	/**
	 * 按 Pawn 速度求「站定 ↔ 移动」倍率的**目标值**（纯函数，无状态）。
	 *
	 * 实际值由调用方用自己的 FInterpTo 状态逼近这个目标（过渡速率见
	 * SpreadTransitionRate_StandingStill）—— 目标值本身是纯函数才可单测。
	 *
	 *   速度 <= 阈值                → SpreadMultiplier_StandingStill
	 *   阈值 < 速度 < 阈值+带宽     → 线性插值到 1.0
	 *   速度 >= 阈值+带宽           → 1.0
	 */
	UFUNCTION(BlueprintPure, Category = "Recoil|Spread|Query")
	float GetSpreadMovementMultiplierTarget(float PawnSpeed) const;

	/**
	 * 校验资产配置是否自洽。由 Lyra.Recoil.Profile.Validation 自动化测试与资产生成
	 * Commandlet 共用，因此不走 UFUNCTION（避免 BP 侧的纯函数语义冲突）。
	 * @param OutErrors 每条不合格项追加一条可读描述
	 * @return 全部合格返回 true
	 */
	bool ValidateProfile(TArray<FString>& OutErrors) const;

	/** 把 PatternLength 夹到合法范围。PostLoad / PostEditChangeProperty 会调用。 */
	void SanitizePatternLength();
};

#undef UE_API
