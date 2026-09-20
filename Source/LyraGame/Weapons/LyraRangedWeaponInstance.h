// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Curves/CurveFloat.h"

#include "LyraWeaponInstance.h"
#include "AbilitySystem/LyraAbilitySourceInterface.h"
#include "Weapons/Recoil/LyraRecoilState.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"

#include "LyraRangedWeaponInstance.generated.h"

class ALyraPlayerCameraManager;
class UCameraModifier_WeaponRecoil;
class ULyraRecoilProfile;
class UPhysicalMaterial;

/**
 * ULyraRangedWeaponInstance
 *
 * A piece of equipment representing a ranged weapon spawned and applied to a pawn
 */
UCLASS()
class ULyraRangedWeaponInstance : public ULyraWeaponInstance, public ILyraAbilitySourceInterface
{
	GENERATED_BODY()

public:
	ULyraRangedWeaponInstance(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	virtual void PostLoad() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;

	void UpdateDebugVisualization();
#endif

	int32 GetBulletsPerCartridge() const
	{
		return BulletsPerCartridge;
	}
	
	/** Returns the current spread angle (in degrees, diametrical) */
	float GetCalculatedSpreadAngle() const;

	float GetCalculatedSpreadAngleMultiplier() const;

	bool HasFirstShotAccuracy() const
	{
		return bHasFirstShotAccuracy;
	}

	float GetSpreadExponent() const;

	/**
	 * 本武器是否走「资产散布」（ULyraRecoilProfile 的姿态-角度直接模型）。
	 *
	 * 判据只有一个：配了 RecoilProfile 且该资产的 bEnableProfileSpread == true。
	 * 为 false 时，散布完全走 Lyra 原生 heat 模型 —— 既有的 30 个后坐力用例、
	 * 5 份 Golden 数据与既有武器蓝图手感全部不受影响。
	 *
	 * 注意它**不看** Lyra.Recoil.Enable：那个 CVar 管的是后坐力，散布要能单独调。
	 */
	bool UsesProfileSpread() const;

	float GetMaxDamageRange() const
	{
		return MaxDamageRange;
	}

	float GetBulletTraceSweepRadius() const
	{
		return BulletTraceSweepRadius;
	}

	//~ Begin 后坐力系统接口（开发计划 §P2 / §P3）

	/** 后坐力配置资产。为空时整套后坐力系统静默禁用，只剩 Lyra 原有扩散行为。 */
	ULyraRecoilProfile* GetRecoilProfile() const
	{
		return RecoilProfile;
	}

	/** 运行时后坐力状态（只读）。纯数值结构，无 UWorld 依赖。 */
	const FRecoilRuntimeState& GetRecoilState() const
	{
		return RecoilState;
	}

	/**
	 * 每发调用一次。由 ULyraGameplayAbility_RangedWeapon 在扣弹成功之后调用，
	 * 与现有 AddSpread() 并列（两者独立，互不影响）。
	 */
	void AddRecoil();

	/**
	 * P3 弹道链：取第 ShotIndex 发的弹道方向偏移（度）。
	 * 会在取值前刷新"这一刻"的姿态倍率，开销是一次乘法，无状态副作用。
	 */
	FRecoilShotKick GetRecoilShotDirectionOffset(int32 ShotIndex);

	/**
	 * 弹道链在**发弹前**调用：把本发实际使用的散布锥角（度，全锥角）记进运行时状态。
	 *
	 * 为什么需要这个显式回调：Lyra 的既有顺序是「先按当前散布打出去 → 再 AddSpread 加热」，
	 * 而 ApplyShot 发生在加热之后，那时读到的已经是"下一发的锥角"。所以在发弹那一刻
	 * 必须留下一份快照，否则 CSV 里的 SpreadAngle 列会整体错位一发。
	 *
	 * 未启用资产散布时写进去也不会被使用（CurrentSpreadAngle 恒为 0，列即为 0）。
	 */
	void NotifyShotSpreadUsed(float SpreadAngleDegrees);

	/**
	 * P5：从磁盘强制重载当前的 RecoilProfile 资产。
	 *
	 * 什么时候才需要它：资产被编辑器以外的东西改过（外部工具、命令行 Commandlet、
	 * 版本控制回滚）。**在编辑器里直接改资产不需要它** —— PIE 与编辑器同进程共享同一个
	 * UObject，改完下一帧就生效。
	 *
	 * 仅编辑器构建实现（重载依赖 GC 与包加载，不应出现在 Shipping 路径上）。
	 */
	void ReloadRecoilProfile();

	/** P5：世界内 DebugDraw（受 Lyra.Recoil.DebugDraw 控制）。 */
	void DrawRecoilDebug();

	//~ End 后坐力系统接口

protected:
#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, Category = "Spread|Fire Params")
	float Debug_MinHeat = 0.0f;

	UPROPERTY(VisibleAnywhere, Category = "Spread|Fire Params")
	float Debug_MaxHeat = 0.0f;

	UPROPERTY(VisibleAnywhere, Category="Spread|Fire Params", meta=(ForceUnits=deg))
	float Debug_MinSpreadAngle = 0.0f;

	UPROPERTY(VisibleAnywhere, Category="Spread|Fire Params", meta=(ForceUnits=deg))
	float Debug_MaxSpreadAngle = 0.0f;

	UPROPERTY(VisibleAnywhere, Category="Spread Debugging")
	float Debug_CurrentHeat = 0.0f;

	UPROPERTY(VisibleAnywhere, Category="Spread Debugging", meta = (ForceUnits=deg))
	float Debug_CurrentSpreadAngle = 0.0f;

	// The current *combined* spread angle multiplier
	UPROPERTY(VisibleAnywhere, Category = "Spread Debugging", meta=(ForceUnits=x))
	float Debug_CurrentSpreadAngleMultiplier = 1.0f;

#endif

	// =====================================================================
	// Lyra 原生散布字段（heat 三曲线模型）—— ★ 已废弃，仅作回退保留
	//
	// === 为什么还留着 ===
	//
	// 散布配置已迁移到 ULyraRecoilProfile 的「Recoil|Spread」组
	// （姿态-角度直接模型，见 Docs/Recoil/12_SpreadInProfile.md）。
	// 只要 `ULyraRecoilProfile::bEnableProfileSpread == true`，下面这一整段
	// **完全不参与计算**，只在关掉那个开关时充当回退配置。
	//
	// 刻意"保留 + 标注"而不是直接删掉，理由是：
	//   1) 删字段会让既有武器蓝图（B_WeaponInstance_Rifle 等）序列化数据静默丢失，
	//      回退路径就没了 —— 而回退路径正是"零回归"承诺的兑现方式；
	//   2) 关闭开关时必须得到**逐位一致**的旧行为，这一点只能靠旧字段还在来保证。
	//
	// 迁移对照表（旧 → 新）：
	//   HeatToSpreadCurve                → SpreadAngle_* / MaxSpreadAngle_*
	//   HeatToHeatPerShotCurve           → SpreadAddPerShot_*
	//   HeatToCoolDownPerSecondCurve     → SpreadRecoverRate_*
	//   SpreadRecoveryCooldownDelay      → SpreadRecoveryDelay
	//   SpreadExponent                   → SpreadExponent（Profile 侧同名）
	//   SpreadAngleMultiplier_Aiming     → SpreadMultiplier_Aiming
	//   SpreadAngleMultiplier_StandingStill + 2 个速度阈值 + TransitionRate_StandingStill
	//                                    → SpreadMultiplier_StandingStill + 3 个同名参数
	//   SpreadAngleMultiplier_Crouching / TransitionRate_Crouching
	//                                    → SpreadAngle_Crouching / MaxSpreadAngle_Crouching
	//   SpreadAngleMultiplier_JumpingOrFalling / TransitionRate_JumpingOrFalling
	//                                    → SpreadAngle_JumpingOrFalling / MaxSpreadAngle_JumpingOrFalling
	//   bAllowFirstShotAccuracy          → 无对应（新版不提供"首发绝对精准"开关）
	//
	// 分类统一挪到 "Spread (deprecated)" 下，Details 面板里会折到最底下。
	// =====================================================================

	// Spread exponent, affects how tightly shots will cluster around the center line
	// when the weapon has spread (non-perfect accuracy). Higher values will cause shots
	// to be closer to the center (default is 1.0 which means uniformly within the spread range)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread (deprecated)|Fire Params",
		meta=(ClampMin=0.1, DeprecationMessage="Moved to ULyraRecoilProfile::SpreadExponent (Recoil|Spread|Player). Only used when bEnableProfileSpread is false."))
	float SpreadExponent = 1.0f;

	// A curve that maps the heat to the spread angle
	// The X range of this curve typically sets the min/max heat range of the weapon
	// The Y range of this curve is used to define the min and maximum spread angle
	UPROPERTY(EditAnywhere, Category = "Spread (deprecated)|Fire Params",
		meta=(DeprecationMessage="Moved to ULyraRecoilProfile::SpreadAngle_* / MaxSpreadAngle_* (Recoil|Spread|<Pose>). Only used when bEnableProfileSpread is false."))
	FRuntimeFloatCurve HeatToSpreadCurve;

	// A curve that maps the current heat to the amount a single shot will further 'heat up'
	// This is typically a flat curve with a single data point indicating how much heat a shot adds,
	// but can be other shapes to do things like punish overheating by adding progressively more heat.
	UPROPERTY(EditAnywhere, Category="Spread (deprecated)|Fire Params",
		meta=(DeprecationMessage="Moved to ULyraRecoilProfile::SpreadAddPerShot_* (Recoil|Spread|<Pose>). Only used when bEnableProfileSpread is false."))
	FRuntimeFloatCurve HeatToHeatPerShotCurve;
	
	// A curve that maps the current heat to the heat cooldown rate per second
	// This is typically a flat curve with a single data point indicating how fast the heat
	// wears off, but can be other shapes to do things like punish overheating by slowing down
	// recovery at high heat.
	UPROPERTY(EditAnywhere, Category="Spread (deprecated)|Fire Params",
		meta=(DeprecationMessage="Moved to ULyraRecoilProfile::SpreadRecoverRate_* (Recoil|Spread|<Pose>). Only used when bEnableProfileSpread is false."))
	FRuntimeFloatCurve HeatToCoolDownPerSecondCurve;

	// Time since firing before spread cooldown recovery begins (in seconds)
	UPROPERTY(EditAnywhere, Category="Spread (deprecated)|Fire Params", meta=(ForceUnits=s,
		DeprecationMessage="Moved to ULyraRecoilProfile::SpreadRecoveryDelay (Recoil|Spread|Player). Only used when bEnableProfileSpread is false."))
	float SpreadRecoveryCooldownDelay = 0.0f;

	// Should the weapon have perfect accuracy when both player and weapon spread are at their minimum value
	UPROPERTY(EditAnywhere, Category="Spread (deprecated)|Fire Params",
		meta=(DeprecationMessage="No equivalent in the profile-driven spread model: the new model has no 'perfect first shot' switch. Only used when bEnableProfileSpread is false."))
	bool bAllowFirstShotAccuracy = false;

	// Multiplier when in an aiming camera mode
	UPROPERTY(EditAnywhere, Category="Spread (deprecated)|Player Params", meta=(ForceUnits=x,
		DeprecationMessage="Moved to ULyraRecoilProfile::SpreadMultiplier_Aiming. Only used when bEnableProfileSpread is false."))
	float SpreadAngleMultiplier_Aiming = 1.0f;

	// Multiplier when standing still or moving very slowly
	// (starts to fade out at StandingStillSpeedThreshold, and is gone completely by StandingStillSpeedThreshold + StandingStillToMovingSpeedRange)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread (deprecated)|Player Params", meta=(ForceUnits=x,
		DeprecationMessage="Moved to ULyraRecoilProfile::SpreadMultiplier_StandingStill. Only used when bEnableProfileSpread is false."))
	float SpreadAngleMultiplier_StandingStill = 1.0f;

	// Rate at which we transition to/from the standing still accuracy (higher values are faster, though zero is instant; @see FInterpTo)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread (deprecated)|Player Params",
		meta=(DeprecationMessage="Moved to ULyraRecoilProfile::SpreadTransitionRate_StandingStill. Only used when bEnableProfileSpread is false."))
	float TransitionRate_StandingStill = 5.0f;

	// Speeds at or below this are considered standing still
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread (deprecated)|Player Params", meta=(ForceUnits="cm/s",
		DeprecationMessage="Moved to ULyraRecoilProfile::SpreadStandingStillSpeedThreshold. Only used when bEnableProfileSpread is false."))
	float StandingStillSpeedThreshold = 80.0f;

	// Speeds no more than this above StandingStillSpeedThreshold are used to feather down the standing still bonus until it's back to 1.0
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread (deprecated)|Player Params", meta=(ForceUnits="cm/s",
		DeprecationMessage="Moved to ULyraRecoilProfile::SpreadStandingStillToMovingRange. Only used when bEnableProfileSpread is false."))
	float StandingStillToMovingSpeedRange = 20.0f;


	// Multiplier when crouching, smoothly blended to based on TransitionRate_Crouching
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread (deprecated)|Player Params", meta=(ForceUnits=x,
		DeprecationMessage="Moved to ULyraRecoilProfile::SpreadAngle_Crouching (per-pose angle, no multiplier). Only used when bEnableProfileSpread is false."))
	float SpreadAngleMultiplier_Crouching = 1.0f;

	// Rate at which we transition to/from the crouching accuracy (higher values are faster, though zero is instant; @see FInterpTo)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread (deprecated)|Player Params",
		meta=(DeprecationMessage="Dropped: crouch switches pose instantly in the profile-driven model (same rule as the recoil pose multipliers). Only used when bEnableProfileSpread is false."))
	float TransitionRate_Crouching = 5.0f;


	// Spread multiplier while jumping/falling, smoothly blended to based on TransitionRate_JumpingOrFalling
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread (deprecated)|Player Params", meta=(ForceUnits=x,
		DeprecationMessage="Moved to ULyraRecoilProfile::SpreadAngle_JumpingOrFalling (per-pose angle, no multiplier). Only used when bEnableProfileSpread is false."))
	float SpreadAngleMultiplier_JumpingOrFalling = 1.0f;

	// Rate at which we transition to/from the jumping/falling accuracy (higher values are faster, though zero is instant; @see FInterpTo)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread (deprecated)|Player Params",
		meta=(DeprecationMessage="Dropped: jump/fall switches pose instantly in the profile-driven model. Only used when bEnableProfileSpread is false."))
	float TransitionRate_JumpingOrFalling = 5.0f;

	// ---------------------------------------------------------------------
	// 后坐力（数据驱动：全部手感参数都在 RecoilProfile 资产里，本类不含任何可调魔数）
	// ---------------------------------------------------------------------

	/** 后坐力手感配置。留空 = 本武器不使用后坐力系统。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recoil", meta = (DisplayName = "Recoil Profile"))
	TObjectPtr<ULyraRecoilProfile> RecoilProfile;

	// Number of bullets to fire in a single cartridge (typically 1, but may be more for shotguns)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Weapon Config")
	int32 BulletsPerCartridge = 1;

	// The maximum distance at which this weapon can deal damage
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Weapon Config", meta=(ForceUnits=cm))
	float MaxDamageRange = 25000.0f;

	// The radius for bullet traces sweep spheres (0.0 will result in a line trace)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Weapon Config", meta=(ForceUnits=cm))
	float BulletTraceSweepRadius = 0.0f;

	// A curve that maps the distance (in cm) to a multiplier on the base damage from the associated gameplay effect
	// If there is no data in this curve, then the weapon is assumed to have no falloff with distance
	UPROPERTY(EditAnywhere, Category = "Weapon Config")
	FRuntimeFloatCurve DistanceDamageFalloff;

	// List of special tags that affect how damage is dealt
	// These tags will be compared to tags in the physical material of the thing being hit
	// If more than one tag is present, the multipliers will be combined multiplicatively
	UPROPERTY(EditAnywhere, Category = "Weapon Config")
	TMap<FGameplayTag, float> MaterialDamageMultiplier;

private:
	// Time since this weapon was last fired (relative to world time)
	double LastFireTime = 0.0;

	// 本梭**首发那一刻**的玩家瞄准俯仰（度）。压枪量的基线。
	//
	// 为什么必须有基线：后坐力偏移与 ControlRotation 是两条独立量（云文档 §6.4 / 红线 R2），
	// 只有"相对起枪点的俯仰差"才能区分「玩家往下压了 3°」和「后坐力把视角推高了 3°」。
	// 每梭首发时刷新（见 AddRecoil），Reset 时清零。
	float BurstStartAimPitch = 0.0f;

	// The current heat
	float CurrentHeat = 0.0f;

	// The current spread angle (in degrees, diametrical)
	float CurrentSpreadAngle = 0.0f;

	// Do we currently have first shot accuracy?
	bool bHasFirstShotAccuracy = false;

	// The current *combined* spread angle multiplier
	float CurrentSpreadAngleMultiplier = 1.0f;

	// The current standing still multiplier
	float StandingStillMultiplier = 1.0f;

	// 资产散布模型下的"移动倍率"（站定 ↔ 跑动的 FInterpTo 状态）。
	//
	// 与上面的 StandingStillMultiplier 是同一件事的两套实现：那一个走 Lyra 原生 heat 链路，
	// 这一个走资产散布链路。之所以不共用：原生链路把它和瞄准/蹲/空中的倍率**相乘**成一个
	// CurrentSpreadAngleMultiplier；资产链路只需要"瞄准 × 移动"两个，乘的层次不同，
	// 强行合流会让两边的语义都变模糊。
	float SpreadMovementMultiplier = 1.0f;

	// The current jumping/falling multiplier
	float JumpFallMultiplier = 1.0f;

	// The current crouching multiplier
	float CrouchingMultiplier = 1.0f;

	// 后坐力运行时状态。放在本类而不是 CameraModifier 上：状态必须与武器一一对应
	// （换枪即重置，见 OnEquipped/OnUnequipped），且要被 HUD、CSV 导出、调试面板直接读到。
	// 注意：本项目不做联机（2026-09-17 决定），本结构体没有 Replicated 标记。
	UPROPERTY(Transient)
	FRecoilRuntimeState RecoilState;

	// 当前挂在 PlayerCameraManager 上的后坐力相机修改器（仅本地玩家有效）
	UPROPERTY(Transient)
	TObjectPtr<UCameraModifier_WeaponRecoil> RecoilCameraModifier;

public:
	void Tick(float DeltaSeconds);

	//~ULyraEquipmentInstance interface
	virtual void OnEquipped();
	virtual void OnUnequipped();
	//~End of ULyraEquipmentInstance interface

	void AddSpread();

	//~ILyraAbilitySourceInterface interface
	virtual float GetDistanceAttenuation(float Distance, const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr) const override;
	virtual float GetPhysicalMaterialAttenuation(const UPhysicalMaterial* PhysicalMaterial, const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr) const override;
	//~End of ILyraAbilitySourceInterface interface

private:
	void ComputeSpreadRange(float& MinSpread, float& MaxSpread);
	void ComputeHeatRange(float& MinHeat, float& MaxHeat);

	inline float ClampHeat(float NewHeat)
	{
		float MinHeat;
		float MaxHeat;
		ComputeHeatRange(/*out*/ MinHeat, /*out*/ MaxHeat);

		return FMath::Clamp(NewHeat, MinHeat, MaxHeat);
	}

	// Updates the spread and returns true if the spread is at minimum
	bool UpdateSpread(float DeltaSeconds);

	// Updates the multipliers and returns true if they are at minimum
	bool UpdateMultipliers(float DeltaSeconds);

	/** 瞄准混合权重 [0,1]，来自相机栈顶层模式。spread 与后坐力共用同一份判定。 */
	float ComputeAimingAlpha() const;

	//~ Begin 后坐力内部实现

	/** 每帧推进后坐力状态并把结果推给相机链。 */
	void UpdateRecoil(float DeltaSeconds);

	/**
	 * 采样玩家当前的瞄准（ControlRotation）交给后坐力状态，用于计算"压枪量"。
	 *
	 * 每帧 + 每次开火前各调一次（见 UpdateRecoil / AddRecoil）。
	 * 这是**唯一**一处让算法层知道"玩家往哪压了"的地方 —— FRecoilRuntimeState 本身
	 * 依旧不碰 UWorld，只接受数值，所以纯数值单测的隔离性没有被破坏。
	 */
	void SampleRecoilPlayerAim();

	/** 姿态状态，复用 CharacterMovementComponent 的蹲/空中判定。 */
	EPoseState ComputeRecoilPoseState() const;

	/** 后坐力侧的总姿态倍率 = 姿态倍率 × 瞄准混合倍率。 */
	float ComputeRecoilPoseMultiplier() const;

	/**
	 * 取当前玩家瞄准俯仰（度）。无 Pawn / Controller 时返回 false 且不改动 OutPitchDegrees。
	 *
	 * 读的是 `ControlRotation` —— 它**不含**后坐力偏移（偏移只作用于显示层 POV），
	 * 所以这个值就是"玩家自己瞄到哪"，正是压枪量的正确来源。
	 */
	bool TryGetAimPitch(float& OutPitchDegrees) const;

	/**
	 * 本梭的玩家压枪量（度，向下压枪为正，恒 ≥ 0）= `起枪点俯仰 − 当前俯仰`。
	 *
	 * 只在「本梭进行中」返回非零：`Idle` 时返回 0，避免上一梭的旧基线在休火期继续放宽上限。
	 * 向上抬头（俯仰变大）按 0 处理 —— 不抬头也白送抵扣额度。
	 */
	float ComputeAimCompensationPitch() const;

	/** 清零后坐力状态。装备/卸下时调用。 */
	void ResetRecoilState();

	/** 确保相机修改器已挂上，并同步当前偏移（或在不生效时清零）。 */
	void UpdateRecoilCameraModifier();

	/**
	 * 让相机上的后坐力偏移**平滑归零**，并断开本实例对修改器的缓存引用。
	 *
	 * 刻意**不**调用 RemoveCameraModifier：摘掉修改器会让残留偏移在同一帧内消失，
	 * 表现为一次可见的跳变（"切枪震屏"）。修改器常驻在 PlayerCameraManager 上，
	 * 由它自己走释放衰减回 0 —— 详见 UCameraModifier_WeaponRecoil 的类注释。
	 */
	void ClearRecoilCameraOffset();

	/** 取本武器持有者所在的 Lyra 相机管理器；无本地相机时返回 nullptr。 */
	ALyraPlayerCameraManager* GetOwningPlayerCameraManager() const;

	//~ End 后坐力内部实现
};
