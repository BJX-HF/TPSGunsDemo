#pragma once

#include "CoreMinimal.h"
#include "LyraWeaponVisualRecoilState.generated.h"

/** Semantic six-axis visual channel, mapped to the hand bone by each weapon's calibration. */
USTRUCT(BlueprintType)
struct FWeaponVisualRecoilSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ToolTip = "Enable the V1 pitch/back visual channel for this weapon."))
	bool bEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.0", ToolTip = "Independent visual strength; does not change camera or bullet trajectory."))
	float VisualScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.0", ToolTip = "Visual pitch degrees per degree of this shot's actual vertical kick."))
	float PitchFromVerticalKick = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Impulse", meta = (ClampMin = "0.0", ToolTip = "Visual yaw degrees per degree of this shot's actual signed horizontal kick."))
	float YawFromHorizontalKick = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Impulse", meta = (ToolTip = "Roll degrees per degree of signed visual yaw."))
	float RollFromYaw = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Impulse", meta = (ClampMin = "0.0", ForceUnits = cm, ToolTip = "Upward movement per shot before visual and pose scaling."))
	float UpCmPerShot = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Impulse", meta = (ClampMin = "0.0", ForceUnits = cm, ToolTip = "Signed sideways movement per shot; direction follows actual horizontal kick."))
	float SideCmPerShot = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Impulse", meta = (ClampMin = "0.0", ToolTip = "Additional visual ADS scale, blended from one using actual aiming alpha. Existing shot kick already includes the gameplay ADS multiplier."))
	float ADSVisualScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.0", ForceUnits = cm, ToolTip = "Backward movement in centimetres per shot, before stance and visual scales."))
	float BackCmPerShot = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.001", ForceUnits = s, ToolTip = "Seconds from shot to visual peak."))
	float AttackDuration = 0.04f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.0", ForceUnits = s, ToolTip = "Brief peak hold so the impact remains visible at lower frame rates."))
	float PeakHoldDuration = 0.025f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.001", ForceUnits = s, ToolTip = "Exponential return time constant in seconds."))
	float ReturnTime = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.0", ForceUnits = deg, ToolTip = "Maximum visual pitch magnitude during automatic fire."))
	float MaxPitchDegrees = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Clamp", meta = (ClampMin = "0.0", ForceUnits = deg, ToolTip = "Maximum absolute visual yaw during automatic fire."))
	float MaxYawDegrees = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Clamp", meta = (ClampMin = "0.0", ForceUnits = deg, ToolTip = "Maximum absolute visual roll during automatic fire."))
	float MaxRollDegrees = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.0", ForceUnits = cm, ToolTip = "Maximum backward movement during automatic fire."))
	float MaxBackCm = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Clamp", meta = (ClampMin = "0.0", ForceUnits = cm, ToolTip = "Maximum upward visual movement during automatic fire."))
	float MaxUpCm = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Clamp", meta = (ClampMin = "0.0", ForceUnits = cm, ToolTip = "Maximum absolute sideways visual movement during automatic fire."))
	float MaxSideCm = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Alignment", meta = (ClampMin = "0.0", ToolTip = "Weak low-frequency visual alignment to the existing camera pitch; zero disables it."))
	float CameraPitchAlignment = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Alignment", meta = (ClampMin = "0.0", ToolTip = "Weak low-frequency visual alignment to the existing camera yaw; zero disables it."))
	float CameraYawAlignment = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Alignment", meta = (ClampMin = "0.0", ForceUnits = deg, ToolTip = "Independent bound for each camera alignment term."))
	float MaxAlignmentDegrees = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ToolTip = "Calibrated hand bone-space direction for backward travel. Verify this on the actual rifle mesh before enabling."))
	FVector BackAxisBoneSpace = FVector(-1.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Axes", meta = (ToolTip = "Calibrated hand bone-space direction for upward travel."))
	FVector UpAxisBoneSpace = FVector(0.0f, 0.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Axes", meta = (ToolTip = "Calibrated hand bone-space direction for positive sideways travel."))
	FVector SideAxisBoneSpace = FVector(0.0f, 1.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ToolTip = "Hand bone-space rotation per degree of semantic visual pitch. Verify the sign before enabling."))
	FRotator PitchAxisBoneSpace = FRotator(1.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Axes", meta = (ToolTip = "Calibrated hand bone-space rotation per degree of positive visual yaw."))
	FRotator YawAxisBoneSpace = FRotator(0.0f, 1.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Axes", meta = (ToolTip = "Calibrated hand bone-space rotation per degree of positive visual roll."))
	FRotator RollAxisBoneSpace = FRotator(0.0f, 0.0f, 1.0f);

	// The IK target belongs to a separate skeleton branch and can have different
	// local axes from hand_r. Calibrate these independently before enabling V3.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK Axes", meta = (ToolTip = "ik_hand_r bone-space direction for backward travel; calibrate independently of hand_r."))
	FVector TargetBackAxisBoneSpace = FVector(-1.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK Axes", meta = (ToolTip = "ik_hand_r bone-space direction for upward travel."))
	FVector TargetUpAxisBoneSpace = FVector(0.0f, 0.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK Axes", meta = (ToolTip = "ik_hand_r bone-space direction for positive sideways travel."))
	FVector TargetSideAxisBoneSpace = FVector(0.0f, 1.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK Axes", meta = (ToolTip = "ik_hand_r bone-space rotation for semantic pitch."))
	FRotator TargetPitchAxisBoneSpace = FRotator(1.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK Axes", meta = (ToolTip = "ik_hand_r bone-space rotation for semantic yaw."))
	FRotator TargetYawAxisBoneSpace = FRotator(0.0f, 1.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK Axes", meta = (ToolTip = "ik_hand_r bone-space rotation for semantic roll."))
	FRotator TargetRollAxisBoneSpace = FRotator(0.0f, 0.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK", meta = (ToolTip = "Drive ik_hand_r before right-arm TwoBoneIK. Leave off until per-rifle axes and grip have been calibrated; off retains the V1 direct-hand prototype."))
	bool bDriveRightHandIKTarget = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK", meta = (ToolTip = "Refresh ik_hand_l from the weapon-space virtual bone after the right-hand recoil and before left-arm TwoBoneIK."))
	bool bFollowWeaponWithLeftHand = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Performance", meta = (ClampMin = "0", ToolTip = "Fade Gun Kick out above this character mesh LOD. Camera and bullet recoil continue unchanged."))
	int32 MaxVisualLOD = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Animation", meta = (ToolTip = "Animation curve whose 0..1 value suppresses visual gun kick during reload, equip, or other authored actions."))
	FName SuppressionCurveName = TEXT("DisableHandIKRetargeting");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Animation", meta = (ClampMin = "0.0", ForceUnits = s, ToolTip = "Seconds to restore visual recoil after the suppression curve clears."))
	float BlendInTime = 0.08f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Animation", meta = (ClampMin = "0.0", ForceUnits = s, ToolTip = "Seconds to fade visual recoil when a montage suppression curve rises."))
	float BlendOutTime = 0.04f;
};

USTRUCT(BlueprintType)
struct FWeaponVisualRecoilPose
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float PitchDegrees = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float YawDegrees = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float RollDegrees = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float BackCm = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float UpCm = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float SideCm = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float Alpha = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	int32 ShotSerial = 0;
};

/** Pure numeric state. A committed shot drives six mechanical axes; camera alignment is output-only. */
struct FWeaponVisualRecoilState
{
	static constexpr float FixedStepSeconds = 1.0f / 120.0f;

	void Reset();
	bool ApplyShot(const FWeaponVisualRecoilSettings& Settings, int32 InShotSerial, float VerticalKick, float PoseMultiplier, float GlobalScale, float HorizontalKick = 0.0f, float AimingAlpha = 0.0f);
	void Advance(const FWeaponVisualRecoilSettings& Settings, float DeltaSeconds, float CameraPitch = 0.0f, float CameraYaw = 0.0f);
	const FWeaponVisualRecoilPose& GetPose() const { return Pose; }

private:
	void Step(const FWeaponVisualRecoilSettings& Settings);
	void PublishPose(const FWeaponVisualRecoilSettings& Settings, float CameraPitch, float CameraYaw);

	FWeaponVisualRecoilPose Pose;
	FWeaponVisualRecoilPose MechanicalPose;
	FWeaponVisualRecoilPose AttackStart;
	FWeaponVisualRecoilPose AttackTarget;
	float AttackElapsed = 0.0f;
	float HoldElapsed = 0.0f;
	float StepAccumulator = 0.0f;
	bool bAttacking = false;
	bool bHolding = false;
};
