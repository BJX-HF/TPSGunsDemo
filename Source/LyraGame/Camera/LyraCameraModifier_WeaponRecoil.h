// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Camera/CameraModifier.h"

#include "LyraCameraModifier_WeaponRecoil.generated.h"

#define UE_API LYRAGAME_API

struct FMinimalViewInfo;

/**
 * UCameraModifier_WeaponRecoil
 *
 * 后坐力**相机链**的输出端（开发计划 §P2 交付物）。
 *
 * 职责：把 ULyraRecoilProfile / FRecoilRuntimeState 算出来的视觉偏移施加到相机 POV 上。
 * 挂载方式：由 ULyraRangedWeaponInstance 通过 ALyraPlayerCameraManager::AddNewCameraModifier()
 *          挂上；**卸下武器时不再摘除**，只让它自然释放（原因见下）。
 *
 * 硬约束（开发计划 §硬性规则 5 / §3.3 决策 1）：
 *   只修改 FMinimalViewInfo（显示层），**绝不**使用 AddPitchInput / AddYawInput，
 *   绝不触碰 AController::ControlRotation。原因：
 *     1. ControlRotation 会被玩家输入持续改写，回正时无法区分
 *        "玩家自己拉的" 与 "后坐力抬的"，会把玩家视角一起拽走；
 *     2. 弹道方向取自 GetPlayerViewPoint()（由 ControlRotation 派生），
 *        改它会让相机链反过来污染弹道链，两条链再也分不开；
 *     3. 抬视角的量会被写进控制器的复制数据 —— 本项目不做联机（2026-09-17 决定），
 *        此条仅作为"不该碰 ControlRotation"的补充论据保留。
 *
 * 本类刻意不认识武器/资产：偏移值由外部每帧推送，所以它可以在任何
 * PlayerCameraManager 上独立复用与单测。
 *
 * === 释放衰减（Release Fade）===
 *
 * 为什么需要：武器卸下 / 切换 / 总开关关闭时，相机上可能还挂着几度残留偏移。
 * 若让它在一帧内归零，玩家看到的就是**一次跳变（表现为"震屏"）**。
 * 因此"偏移要消失"时不直接清零，而是走一段很短的释放衰减（见 ClearRecoilOffset）。
 *
 * 为什么平时不做平滑：**正常驱动时 Applied 直接等于 Target，零延迟**。
 * 单发插值模式（Interpolated）已经用 LiftCurve/ReboundCurve 精确描述了相机轨迹，
 * 在这里再叠一层平滑只会把曲线抹圆，等于和资产里的曲线打架。所以只衰减"释放"这一段。
 */
UCLASS(NotBlueprintable, meta = (DisplayName = "Weapon Recoil"))
class UE_API UCameraModifier_WeaponRecoil : public UCameraModifier
{
	GENERATED_BODY()

public:

	UCameraModifier_WeaponRecoil();

	/**
	 * 推送当前相机偏移（度）。由武器实例每帧在推进完状态后调用。
	 * 三个值都是"最终显示值"，本类不再做任何缩放（全局倍率已在状态层施加）。
	 *
	 * 正常路径下 Applied 会**直接跟上** Target，不加平滑（理由见类注释）。
	 *
	 * @param InPitchDegrees Pitch 偏移（Pitch/Yaw 累加-回正链）
	 * @param InYawDegrees   Yaw 偏移（Pitch/Yaw 累加-回正链）
	 * @param InRollDegrees  Roll 偏移（Roll 阻尼震动链，独立求解，不加回正）
	 */
	void SetRecoilOffset(float InPitchDegrees, float InYawDegrees, float InRollDegrees = 0.0f);

	/** 只推 Roll（给"只想动震屏、不动弹道偏移"的调试场景用）。 */
	void SetRollOffset(float InRollDegrees);

	/**
	 * 让相机偏移**归零**，但不是一帧砍断 —— 而是从当前值走一段释放衰减。
	 *
	 * 用于：卸下武器 / 切换武器 / 总开关关闭。这几处的共同点是"偏移应该消失"，
	 * 但此时相机上往往还有残留，硬清零就是玩家口中的"震屏"。
	 */
	void ClearRecoilOffset();

	float GetPitchOffsetDegrees() const { return PitchOffsetDegrees; }
	float GetYawOffsetDegrees() const { return YawOffsetDegrees; }
	float GetRollOffsetDegrees() const { return RollOffsetDegrees; }

	/**
	 * 当前真正施加到最终 POV 的 Pitch/Yaw。
	 * 正常驱动时等于目标值；卸枪/关闭系统的释放衰减期间会逐帧趋近 0。
	 * 第三人称相机模式用这两个值计算相机轨道位置，保证位置与最终可见朝向使用同一套角度。
	 */
	float GetAppliedPitchOffsetDegrees() const { return AppliedPitchDegrees; }
	float GetAppliedYawOffsetDegrees() const { return AppliedYawDegrees; }

	//~UCameraModifier interface
	virtual bool ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV) override;
	//~End of UCameraModifier interface

protected:

	/** 当前垂直偏移目标（度，向上为正） */
	float PitchOffsetDegrees = 0.0f;

	/** 当前水平偏移目标（度，向右为正） */
	float YawOffsetDegrees = 0.0f;

	/**
	 * 当前 Roll 偏移目标（度，顺时针为正）。
	 *
	 * 来源与 Pitch/Yaw 不同：它是 Roll 阻尼震动的**瞬时解**，围绕零点往复，
	 * 结束后归零；而 Pitch/Yaw 是累加量，会停在稳态偏移上。
	 * 两者在 POV 上直接相加，互不影响。
	 */
	float RollOffsetDegrees = 0.0f;

	// ---------------------------------------------------------------------
	// 实际施加到 POV 的值。平时恒等于上面的目标值；只在"释放衰减"期间与之分离。
	// ---------------------------------------------------------------------

	float AppliedPitchDegrees = 0.0f;
	float AppliedYawDegrees = 0.0f;
	float AppliedRollDegrees = 0.0f;

	/** 是否正在做释放衰减 */
	bool bReleasing = false;

	/** 释放衰减已经进行的时间（秒） */
	float ReleaseElapsedSeconds = 0.0f;

	/** 释放衰减的起点（衰减开始时 Applied 的快照） */
	float ReleaseStartPitch = 0.0f;
	float ReleaseStartYaw = 0.0f;
	float ReleaseStartRoll = 0.0f;

protected:

	/** 释放衰减时长（秒）。结构性常数，不是手感参数 —— 详见 .cpp 里的说明。 */
	static constexpr float ReleaseDurationSeconds = 0.18f;

	/** 判定"已经没有偏移"的阈值（度）。 */
	static constexpr float ReleaseEpsilon = 1e-3f;

	/** 当前是否有任何实际施加中的偏移。 */
	bool HasAnyAppliedOffset() const;

	/** 目标值是否整体为零。 */
	bool IsTargetZero() const;
};

#undef UE_API
