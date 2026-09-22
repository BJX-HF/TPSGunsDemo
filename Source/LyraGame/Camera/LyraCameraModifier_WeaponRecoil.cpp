// Copyright Epic Games, Inc. All Rights Reserved.

#include "Camera/LyraCameraModifier_WeaponRecoil.h"

#include "Camera/CameraTypes.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LyraCameraModifier_WeaponRecoil)

UCameraModifier_WeaponRecoil::UCameraModifier_WeaponRecoil()
	: Super()
{
	// 默认优先级，不与其他相机修改器（如 UI Camera）抢位
	Priority = 0;

	// 构造后可立即生效，不需要 EnableModifier() 过渡
	Alpha = 1.0f;
	AlphaInTime = 0.0f;
	AlphaOutTime = 0.0f;
}

bool UCameraModifier_WeaponRecoil::HasAnyAppliedOffset() const
{
	return !FMath::IsNearlyZero(AppliedPitchDegrees, ReleaseEpsilon)
		|| !FMath::IsNearlyZero(AppliedYawDegrees, ReleaseEpsilon)
		|| !FMath::IsNearlyZero(AppliedRollDegrees, ReleaseEpsilon);
}

bool UCameraModifier_WeaponRecoil::IsTargetZero() const
{
	return FMath::IsNearlyZero(PitchOffsetDegrees, ReleaseEpsilon)
		&& FMath::IsNearlyZero(YawOffsetDegrees, ReleaseEpsilon)
		&& FMath::IsNearlyZero(RollOffsetDegrees, ReleaseEpsilon);
}

void UCameraModifier_WeaponRecoil::SetRecoilOffset(float InPitchDegrees, float InYawDegrees, float InRollDegrees)
{
	PitchOffsetDegrees = InPitchDegrees;
	YawOffsetDegrees = InYawDegrees;
	RollOffsetDegrees = InRollDegrees;

	// 释放衰减进行中，而新的目标也是零 —— 让它把余量自然走完，不要硬拽回 0。
	// 这是"切枪后立刻又装上另一把没开火的枪"的常见路径：此时新枪每帧推的都是 0，
	// 若在这里取消释放，上一把枪的残留就会在一帧内消失（又是那个跳变）。
	if (bReleasing && IsTargetZero())
	{
		return;
	}

	// 有实质目标：取消释放，直接跟上（零延迟，保持单发插值曲线的形状不被抹圆）
	bReleasing = false;
	ReleaseElapsedSeconds = 0.0f;

	AppliedPitchDegrees = PitchOffsetDegrees;
	AppliedYawDegrees = YawOffsetDegrees;
	AppliedRollDegrees = RollOffsetDegrees;
}

void UCameraModifier_WeaponRecoil::SetRollOffset(float InRollDegrees)
{
	// 走同一条规则，避免"只改 Roll"时绕过释放判定造成跳变
	SetRecoilOffset(PitchOffsetDegrees, YawOffsetDegrees, InRollDegrees);
}

void UCameraModifier_WeaponRecoil::ClearRecoilOffset()
{
	PitchOffsetDegrees = 0.0f;
	YawOffsetDegrees = 0.0f;
	RollOffsetDegrees = 0.0f;

	// 只有"当前确实有偏移"才需要衰减。已经归零的情况下直接停，省掉一段空转。
	if (bReleasing || !HasAnyAppliedOffset())
	{
		if (!HasAnyAppliedOffset())
		{
			bReleasing = false;
			ReleaseElapsedSeconds = 0.0f;
			AppliedPitchDegrees = 0.0f;
			AppliedYawDegrees = 0.0f;
			AppliedRollDegrees = 0.0f;
		}
		return;
	}

	bReleasing = true;
	ReleaseElapsedSeconds = 0.0f;
	ReleaseStartPitch = AppliedPitchDegrees;
	ReleaseStartYaw = AppliedYawDegrees;
	ReleaseStartRoll = AppliedRollDegrees;
}

bool UCameraModifier_WeaponRecoil::ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV)
{
	Super::ModifyCamera(DeltaTime, InOutPOV);

	if (bReleasing)
	{
		// 释放衰减：从起点按 (1-t)^2 收到 0。二次曲线让收尾更软，避免末段"顿"一下。
		ReleaseElapsedSeconds += FMath::Max(0.0f, DeltaTime);

		// 注意：这里刻意命名为 ReleaseAlpha 而不是 Alpha —— 基类 UCameraModifier 已有一个
		// float Alpha 成员（混合权重），局部变量同名会触发 C4458（UE 把它当错误）。
		const float ReleaseAlpha = FMath::Clamp(ReleaseElapsedSeconds / ReleaseDurationSeconds, 0.0f, 1.0f);
		const float Falloff = FMath::Square(1.0f - ReleaseAlpha);

		AppliedPitchDegrees = ReleaseStartPitch * Falloff;
		AppliedYawDegrees = ReleaseStartYaw * Falloff;
		AppliedRollDegrees = ReleaseStartRoll * Falloff;

		if (ReleaseAlpha >= 1.0f)
		{
			bReleasing = false;
			ReleaseElapsedSeconds = 0.0f;
			AppliedPitchDegrees = 0.0f;
			AppliedYawDegrees = 0.0f;
			AppliedRollDegrees = 0.0f;
		}
	}

	if (!HasAnyAppliedOffset())
	{
		// 没有偏移时不报告"我改了相机"，让后续修改器/最终结果不受影响
		return false;
	}

	// 只动显示层 POV。ControlRotation 保持不变 —— 这是本方案与 AddPitchInput 的根本区别。
	InOutPOV.Rotation.Pitch += AppliedPitchDegrees;
	InOutPOV.Rotation.Yaw += AppliedYawDegrees;

	// Roll 与 Pitch/Yaw 同源同理：只改显示层，玩家瞄准方向不受影响。
	InOutPOV.Rotation.Roll += AppliedRollDegrees;

	// 保持 Rotator 合法区间，避免长时间累加后出现 Pitch 越界
	InOutPOV.Rotation.Pitch = FRotator::NormalizeAxis(InOutPOV.Rotation.Pitch);
	InOutPOV.Rotation.Yaw = FRotator::NormalizeAxis(InOutPOV.Rotation.Yaw);

	// This modifier is additive and must not consume the camera-modifier chain.
	// Returning true here suppressed later camera shakes while recoil was non-zero;
	// the residual shake (most visibly Roll) then appeared in one frame as recovery
	// reached zero and this function started returning false.
	return false;
}
