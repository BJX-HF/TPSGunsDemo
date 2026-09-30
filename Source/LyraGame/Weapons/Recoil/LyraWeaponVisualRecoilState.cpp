#include "LyraWeaponVisualRecoilState.h"

void FWeaponVisualRecoilState::Reset()
{
	*this = FWeaponVisualRecoilState();
}

bool FWeaponVisualRecoilState::ApplyShot(const FWeaponVisualRecoilSettings& Settings, int32 InShotSerial, float VerticalKick, float PoseMultiplier, float GlobalScale, float HorizontalKick, float AimingAlpha)
{
	if (!Settings.bEnabled || InShotSerial <= Pose.ShotSerial || !FMath::IsFinite(VerticalKick) || !FMath::IsFinite(HorizontalKick))
	{
		return false;
	}

	// Actual shot kick already contains gameplay pose/global scale. Apply those
	// factors only to displacement bases; never multiply the kick twice.
	const float VisualStrength = FMath::Max(0.0f, Settings.VisualScale)
		* FMath::Lerp(1.0f, FMath::Max(0.0f, Settings.ADSVisualScale), FMath::Clamp(AimingAlpha, 0.0f, 1.0f));
	const float BaseStrength = VisualStrength * FMath::Max(0.0f, PoseMultiplier) * FMath::Max(0.0f, GlobalScale);
	const float PitchImpulse = VerticalKick * FMath::Max(0.0f, Settings.PitchFromVerticalKick) * VisualStrength;
	const float YawImpulse = HorizontalKick * FMath::Max(0.0f, Settings.YawFromHorizontalKick) * VisualStrength;
	const float RollImpulse = YawImpulse * Settings.RollFromYaw;
	const float SideSign = (HorizontalKick > 0.0f) ? 1.0f : ((HorizontalKick < 0.0f) ? -1.0f : 0.0f);

	AttackStart = MechanicalPose;
	AttackTarget = MechanicalPose;
	AttackTarget.PitchDegrees = FMath::Clamp(AttackStart.PitchDegrees + PitchImpulse, -FMath::Max(0.0f, Settings.MaxPitchDegrees), FMath::Max(0.0f, Settings.MaxPitchDegrees));
	AttackTarget.YawDegrees = FMath::Clamp(AttackStart.YawDegrees + YawImpulse, -FMath::Max(0.0f, Settings.MaxYawDegrees), FMath::Max(0.0f, Settings.MaxYawDegrees));
	AttackTarget.RollDegrees = FMath::Clamp(AttackStart.RollDegrees + RollImpulse, -FMath::Max(0.0f, Settings.MaxRollDegrees), FMath::Max(0.0f, Settings.MaxRollDegrees));
	AttackTarget.BackCm = FMath::Clamp(AttackStart.BackCm + FMath::Max(0.0f, Settings.BackCmPerShot) * BaseStrength, 0.0f, FMath::Max(0.0f, Settings.MaxBackCm));
	AttackTarget.UpCm = FMath::Clamp(AttackStart.UpCm + FMath::Max(0.0f, Settings.UpCmPerShot) * BaseStrength, 0.0f, FMath::Max(0.0f, Settings.MaxUpCm));
	AttackTarget.SideCm = FMath::Clamp(AttackStart.SideCm + FMath::Max(0.0f, Settings.SideCmPerShot) * SideSign * BaseStrength, -FMath::Max(0.0f, Settings.MaxSideCm), FMath::Max(0.0f, Settings.MaxSideCm));
	AttackElapsed = 0.0f;
	HoldElapsed = 0.0f;
	StepAccumulator = 0.0f;
	bAttacking = true;
	bHolding = false;
	Pose.Alpha = 1.0f;
	Pose.ShotSerial = InShotSerial;
	MechanicalPose.ShotSerial = InShotSerial;
	return true;
}

void FWeaponVisualRecoilState::Advance(const FWeaponVisualRecoilSettings& Settings, float DeltaSeconds, float CameraPitch, float CameraYaw)
{
	if (!Settings.bEnabled)
	{
		Reset();
		return;
	}
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0f)
	{
		return;
	}

	StepAccumulator += FMath::Min(DeltaSeconds, 0.25f);
	while (StepAccumulator >= FixedStepSeconds)
	{
		Step(Settings);
		StepAccumulator -= FixedStepSeconds;
	}
	PublishPose(Settings, CameraPitch, CameraYaw);
}

void FWeaponVisualRecoilState::Step(const FWeaponVisualRecoilSettings& Settings)
{
	if (bAttacking)
	{
		AttackElapsed += FixedStepSeconds;
		const float Alpha = FMath::Clamp(AttackElapsed / FMath::Max(Settings.AttackDuration, FixedStepSeconds), 0.0f, 1.0f);
		MechanicalPose.PitchDegrees = FMath::Lerp(AttackStart.PitchDegrees, AttackTarget.PitchDegrees, Alpha);
		MechanicalPose.YawDegrees = FMath::Lerp(AttackStart.YawDegrees, AttackTarget.YawDegrees, Alpha);
		MechanicalPose.RollDegrees = FMath::Lerp(AttackStart.RollDegrees, AttackTarget.RollDegrees, Alpha);
		MechanicalPose.BackCm = FMath::Lerp(AttackStart.BackCm, AttackTarget.BackCm, Alpha);
		MechanicalPose.UpCm = FMath::Lerp(AttackStart.UpCm, AttackTarget.UpCm, Alpha);
		MechanicalPose.SideCm = FMath::Lerp(AttackStart.SideCm, AttackTarget.SideCm, Alpha);
		if (Alpha >= 1.0f)
		{
			bAttacking = false;
			bHolding = true;
		}
	}
	else if (bHolding)
	{
		HoldElapsed += FixedStepSeconds;
		if (HoldElapsed >= FMath::Max(0.0f, Settings.PeakHoldDuration))
		{
			bHolding = false;
		}
	}
	else
	{
		const float Decay = FMath::Exp(-FixedStepSeconds / FMath::Max(Settings.ReturnTime, FixedStepSeconds));
		MechanicalPose.PitchDegrees *= Decay;
		MechanicalPose.YawDegrees *= Decay;
		MechanicalPose.RollDegrees *= Decay;
		MechanicalPose.BackCm *= Decay;
		MechanicalPose.UpCm *= Decay;
		MechanicalPose.SideCm *= Decay;
		if (FMath::Abs(MechanicalPose.PitchDegrees) < KINDA_SMALL_NUMBER) { MechanicalPose.PitchDegrees = 0.0f; }
		if (FMath::Abs(MechanicalPose.YawDegrees) < KINDA_SMALL_NUMBER) { MechanicalPose.YawDegrees = 0.0f; }
		if (FMath::Abs(MechanicalPose.RollDegrees) < KINDA_SMALL_NUMBER) { MechanicalPose.RollDegrees = 0.0f; }
		if (MechanicalPose.BackCm < KINDA_SMALL_NUMBER) { MechanicalPose.BackCm = 0.0f; }
		if (MechanicalPose.UpCm < KINDA_SMALL_NUMBER) { MechanicalPose.UpCm = 0.0f; }
		if (FMath::Abs(MechanicalPose.SideCm) < KINDA_SMALL_NUMBER) { MechanicalPose.SideCm = 0.0f; }
	}
}

void FWeaponVisualRecoilState::PublishPose(const FWeaponVisualRecoilSettings& Settings, float CameraPitch, float CameraYaw)
{
	Pose = MechanicalPose;
	const float AlignmentLimit = FMath::Max(0.0f, Settings.MaxAlignmentDegrees);
	if (FMath::IsFinite(CameraPitch))
	{
		Pose.PitchDegrees = FMath::Clamp(Pose.PitchDegrees + FMath::Clamp(CameraPitch * FMath::Max(0.0f, Settings.CameraPitchAlignment), -AlignmentLimit, AlignmentLimit), -FMath::Max(0.0f, Settings.MaxPitchDegrees), FMath::Max(0.0f, Settings.MaxPitchDegrees));
	}
	if (FMath::IsFinite(CameraYaw))
	{
		Pose.YawDegrees = FMath::Clamp(Pose.YawDegrees + FMath::Clamp(CameraYaw * FMath::Max(0.0f, Settings.CameraYawAlignment), -AlignmentLimit, AlignmentLimit), -FMath::Max(0.0f, Settings.MaxYawDegrees), FMath::Max(0.0f, Settings.MaxYawDegrees));
	}
	Pose.Alpha = 1.0f;
}
