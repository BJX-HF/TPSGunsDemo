// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "LyraRecoilTypes.generated.h"

/**
 * 后坐力姿态状态。
 *
 * 由 ULyraRangedWeaponInstance::UpdateMultipliers() 已有的移动组件判定推导而来
 * （IsCrouching / IsFalling / 速度阈值），不重新造轮子。
 *
 * 注意：瞄准（Aiming）与姿态正交，不作为本枚举的成员——瞄准以 [0,1] 混合权重
 * 单独作用于 PoseMultiplier_Aiming，避免"蹲着瞄准"这类组合状态爆炸。
 */
UENUM(BlueprintType)
enum class EPoseState : uint8
{
	/** 站立（非蹲、非空中） */
	Standing		UMETA(DisplayName = "Standing"),

	/** 蹲伏 */
	Crouching		UMETA(DisplayName = "Crouching"),

	/** 跳跃或下落中 */
	JumpingOrFalling UMETA(DisplayName = "JumpingOrFalling"),

	/** 枚举成员数量哨兵，非有效姿态 */
	Count			UMETA(Hidden)
};

/**
 * 后坐力运行时状态机状态。
 *
 * Idle ──开火──► Accumulating ──停火 > RecoveryDelay──► Recovering ──归零──► Idle
 *                    ▲                                        │
 *                    └──────────── 再次开火 ───────────────────┘
 */
UENUM(BlueprintType)
enum class ERecoilState : uint8
{
	/** 无累积偏移，静止 */
	Idle			UMETA(DisplayName = "Idle"),

	/** 正在累积后坐力（开火中或停火未超过 RecoveryDelay） */
	Accumulating	UMETA(DisplayName = "Accumulating"),

	/** 正在回正 */
	Recovering		UMETA(DisplayName = "Recovering")
};

/**
 * 随机种子模式。
 *
 * Fixed 模式用于自动化测试与 Golden 数据比对：
 * 同一武器连发 N 发，弹道序列必须逐发一致（见计划 3.3 决策 5）。
 */
UENUM(BlueprintType)
enum class ERecoilRandomSeedMode : uint8
{
	/** 固定种子：序列完全可复现（自动化测试 / Golden 数据必须具备） */
	Fixed			UMETA(DisplayName = "Fixed"),

	/** 每次开火重新播种：手感更"活"，但不可复现 */
	Random			UMETA(DisplayName = "Random")
};

/**
 * 单发后坐力模型。
 *
 * 两套模型共存，由 ULyraRecoilProfile::SingleShotMode 选择：
 *
 *  InstantWrite  —— 开火瞬间把本发 Kick 直接写进累加偏移，**上抬耗时 = 0 秒**。
 *                   连发时轨迹是「楼梯」：阶梯宽度 = 射击间隔，高度 = 本发 Kick。
 *                   相机的观感由「打了多少发」堆出来，带宽发弹道形状。
 *
 *  Interpolated  —— 单发走「上抬 → 回弹 → 下降」的时间轴，每帧向相机推旋转增量。
 *                   买的是单发的「顿挫感」（抬起来 → 掉一下 → 稳一下 → 慢慢落回去）。
 *
 * 实现细节见 Docs/Recoil/10_SingleShotInterpolation.md。
 *
 * 注意：本枚举只影响**相机链**。弹道链（GetShotDirectionOffset）走的是纯函数，
 * 与模式无关 —— 这样两条链的语义边界保持清晰。
 */
UENUM(BlueprintType)
enum class ERecoilSingleShotMode : uint8
{
	/**
	 * 瞬时写入（默认）。开火帧直接累加，上抬 0 秒。
	 * 既有资产、Golden 数据、自动化测试基线都基于此模式，**不要改默认值**。
	 */
	InstantWrite	UMETA(DisplayName = "Instant Write"),

	/**
	 * 插值。单发按阶段时长 + 插值曲线逐帧补间到相机。
	 * 需要额外配置 LiftCurve（上抬曲线）与阶段时长，否则曲线缺省退化为线性。
	 */
	Interpolated	UMETA(DisplayName = "Interpolated")
};

/**
 * Pattern 中的一个归一化弹着点。
 *
 * 语义（重要，后续阶段不得更改）：
 *  - X 为水平方向，右为正，取值范围 [-1, 1]
 *  - Y 为垂直方向，上为正，取值范围 [0, 1]，0 表示该发不抬枪
 *
 * 该值只描述"形状"，实际幅度由 ULyraRecoilProfile::RecoilPerShot_Vertical /
 * RecoilPerShot_Horizontal 缩放。因此调整单发威力不会破坏 Pattern 形状。
 */
USTRUCT(BlueprintType)
struct FRecoilPatternPoint
{
	GENERATED_BODY()

	/** 归一化水平偏移，右为正，[-1, 1] */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recoil|Pattern", meta = (ClampMin = "-1.0", ClampMax = "1.0",
		ToolTip = "归一化水平偏移（右为正，-1~1）。实际水平偏移 = 本值 × RecoilPerShot_Horizontal。"))
	float X = 0.0f;

	/** 归一化垂直偏移，上为正，[0, 1] */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recoil|Pattern", meta = (ClampMin = "0.0", ClampMax = "1.0",
		ToolTip = "归一化垂直偏移（上为正，0~1）。实际抬枪角度 = 本值 × RecoilPerShot_Vertical；0 = 这一发不抬枪。"))
	float Y = 0.0f;

	FRecoilPatternPoint() = default;

	FRecoilPatternPoint(float InX, float InY)
		: X(InX)
		, Y(InY)
	{
	}
};

/**
 * 单个姿态下的散布参数（纯数值，角度单位）。
 *
 * 这是「姿态-角度直接模型」的参数单元（见 Docs/Recoil/12_SpreadInProfile.md）。
 * 参数**形状**参考 DLC36 的 FWeaponFireParam 散布族（StandScatteringArea /
 * StandMaxScatteringArea / StandShootAddScatter / StopFireRecoverScatterSpeed），
 * 但单位从「0~9 的面积值」改成了**度**：
 *
 *   「最大散布 2.0 度」就填 2.0，不需要再做面积 → 角度的换算。
 *
 * 由 ULyraRecoilProfile::GetSpreadParams(EPoseState) 解析出来。算法层只认这个结构，
 * 不认识 Profile —— 与 FCameraRollShakeParams 是同一个范式。
 *
 * ★ 角度口径：全部为**全锥角（直径角）**，与 Lyra 原有
 *   `ULyraRangedWeaponInstance::CurrentSpreadAngle` 一致。
 *   真正喂给变体锥采样时会在弹道侧 ×0.5 取半角
 *   （LyraGameplayAbility_RangedWeapon.cpp L419）。
 */
USTRUCT(BlueprintType)
struct FRecoilSpreadParams
{
	GENERATED_BODY()

	/** 基础散布角（度，全锥角）。停火足够久之后回落到的值。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Spread")
	float BaseAngleDegrees = 0.0f;

	/** 上限散布角（度，全锥角）。连射累加到此封顶；必须 >= BaseAngleDegrees。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Spread")
	float MaxAngleDegrees = 0.0f;

	/** 每发增量（度）。每扣一次扳机在当前散布角上加这么多。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Spread")
	float AddPerShotDegrees = 0.0f;

	/** 停火后的回落速率（度/秒）。0 = 打完不回正（永久残留）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Spread")
	float RecoverRateDegreesPerSecond = 0.0f;
};

/**
 * 单发开火的完整结果记录。
 *
 * 纯数据、无引擎依赖，是以下三处共用的唯一契约：
 *  1. P2 数值单测的逐发断言对象
 *  2. P3 弹道偏移注入的返回值
 *  3. P5 CSV 导出的行结构（列顺序与字段顺序一致）
 *
 * 单位统一为"度"（degree），Pitch 向上为正，Yaw 向右为正。
 */
USTRUCT(BlueprintType)
struct FRecoilShotResult
{
	GENERATED_BODY()

	/** 本次开火在该轮连发中的序号，从 0 开始；换弹/收枪后归零 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Shot")
	int32 ShotIndex = 0;

	/** 本发垂直增量（度，向上为正，已包含姿态/瞄准倍率与全局缩放） */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Shot")
	float VerticalKick = 0.0f;

	/** 本发水平增量（度，向右为正，已包含姿态/瞄准倍率与全局缩放） */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Shot")
	float HorizontalKick = 0.0f;

	/** 本发之后累计的垂直偏移（度） */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Shot")
	float AccumulatedPitch = 0.0f;

	/** 本发之后累计的水平偏移（度） */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Shot")
	float AccumulatedYaw = 0.0f;

	/** 距离上一次开火的时间（秒）；首发为 0 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Shot")
	float TimeSinceFire = 0.0f;

	/** 本发生效时的姿态 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Shot")
	EPoseState PoseState = EPoseState::Standing;

	/** 本发生效时的姿态倍率（含瞄准混合后的总倍率） */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Shot")
	float PoseMultiplier = 1.0f;

	/**
	 * 本发弹道**实际使用**的散布角（度，全锥角，已含姿态/瞄准/移动倍率）。
	 *
	 * 取的是「这发子弹飞出去时那一刻的锥角」，因此它对应的是**上一次 AddSpread 之后**
	 * 的值 —— Lyra 的既有顺序是「先按当前散布打出去，再 AddSpread 加热」（见
	 * LyraGameplayAbility_RangedWeapon.cpp：TraceBulletsInCartridge 在前，
	 * OnTargetDataReadyCallback 里的 AddSpread 在后），这个顺序本项目刻意保持不变。
	 *
	 * 未启用资产散布（ULyraRecoilProfile::bEnableProfileSpread == false）时恒为 0 ——
	 * 此时散布由 Lyra 原生 heat 模型驱动，本字段不参与记录（见 Docs/Recoil/12_SpreadInProfile.md）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Recoil|Shot")
	float SpreadAngle = 0.0f;
};
