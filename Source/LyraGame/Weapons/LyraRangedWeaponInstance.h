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
	float GetCalculatedSpreadAngle() const
	{
		return CurrentSpreadAngle;
	}

	float GetCalculatedSpreadAngleMultiplier() const
	{
		return bHasFirstShotAccuracy ? 0.0f : CurrentSpreadAngleMultiplier;
	}

	bool HasFirstShotAccuracy() const
	{
		return bHasFirstShotAccuracy;
	}

	float GetSpreadExponent() const
	{
		return SpreadExponent;
	}

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

	// Spread exponent, affects how tightly shots will cluster around the center line
	// when the weapon has spread (non-perfect accuracy). Higher values will cause shots
	// to be closer to the center (default is 1.0 which means uniformly within the spread range)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(ClampMin=0.1), Category="Spread|Fire Params")
	float SpreadExponent = 1.0f;

	// A curve that maps the heat to the spread angle
	// The X range of this curve typically sets the min/max heat range of the weapon
	// The Y range of this curve is used to define the min and maximum spread angle
	UPROPERTY(EditAnywhere, Category = "Spread|Fire Params")
	FRuntimeFloatCurve HeatToSpreadCurve;

	// A curve that maps the current heat to the amount a single shot will further 'heat up'
	// This is typically a flat curve with a single data point indicating how much heat a shot adds,
	// but can be other shapes to do things like punish overheating by adding progressively more heat.
	UPROPERTY(EditAnywhere, Category="Spread|Fire Params")
	FRuntimeFloatCurve HeatToHeatPerShotCurve;
	
	// A curve that maps the current heat to the heat cooldown rate per second
	// This is typically a flat curve with a single data point indicating how fast the heat
	// wears off, but can be other shapes to do things like punish overheating by slowing down
	// recovery at high heat.
	UPROPERTY(EditAnywhere, Category="Spread|Fire Params")
	FRuntimeFloatCurve HeatToCoolDownPerSecondCurve;

	// Time since firing before spread cooldown recovery begins (in seconds)
	UPROPERTY(EditAnywhere, Category="Spread|Fire Params", meta=(ForceUnits=s))
	float SpreadRecoveryCooldownDelay = 0.0f;

	// Should the weapon have perfect accuracy when both player and weapon spread are at their minimum value
	UPROPERTY(EditAnywhere, Category="Spread|Fire Params")
	bool bAllowFirstShotAccuracy = false;

	// Multiplier when in an aiming camera mode
	UPROPERTY(EditAnywhere, Category="Spread|Player Params", meta=(ForceUnits=x))
	float SpreadAngleMultiplier_Aiming = 1.0f;

	// Multiplier when standing still or moving very slowly
	// (starts to fade out at StandingStillSpeedThreshold, and is gone completely by StandingStillSpeedThreshold + StandingStillToMovingSpeedRange)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread|Player Params", meta=(ForceUnits=x))
	float SpreadAngleMultiplier_StandingStill = 1.0f;

	// Rate at which we transition to/from the standing still accuracy (higher values are faster, though zero is instant; @see FInterpTo)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread|Player Params")
	float TransitionRate_StandingStill = 5.0f;

	// Speeds at or below this are considered standing still
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread|Player Params", meta=(ForceUnits="cm/s"))
	float StandingStillSpeedThreshold = 80.0f;

	// Speeds no more than this above StandingStillSpeedThreshold are used to feather down the standing still bonus until it's back to 1.0
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread|Player Params", meta=(ForceUnits="cm/s"))
	float StandingStillToMovingSpeedRange = 20.0f;


	// Multiplier when crouching, smoothly blended to based on TransitionRate_Crouching
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread|Player Params", meta=(ForceUnits=x))
	float SpreadAngleMultiplier_Crouching = 1.0f;

	// Rate at which we transition to/from the crouching accuracy (higher values are faster, though zero is instant; @see FInterpTo)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread|Player Params")
	float TransitionRate_Crouching = 5.0f;


	// Spread multiplier while jumping/falling, smoothly blended to based on TransitionRate_JumpingOrFalling
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread|Player Params", meta=(ForceUnits=x))
	float SpreadAngleMultiplier_JumpingOrFalling = 1.0f;

	// Rate at which we transition to/from the jumping/falling accuracy (higher values are faster, though zero is instant; @see FInterpTo)
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Spread|Player Params")
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

	/** 姿态状态，复用 CharacterMovementComponent 的蹲/空中判定。 */
	EPoseState ComputeRecoilPoseState() const;

	/** 后坐力侧的总姿态倍率 = 姿态倍率 × 瞄准混合倍率。 */
	float ComputeRecoilPoseMultiplier() const;

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
