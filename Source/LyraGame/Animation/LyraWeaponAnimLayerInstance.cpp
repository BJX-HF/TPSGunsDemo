#include "LyraWeaponAnimLayerInstance.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Character/LyraHealthComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Equipment/LyraEquipmentManagerComponent.h"
#include "GameplayTagContainer.h"
#include "Weapons/LyraRangedWeaponInstance.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilDebug.h"
#include "GameFramework/Pawn.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LyraWeaponAnimLayerInstance)

void ULyraWeaponAnimLayerInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	GunKickHandTranslation = FVector::ZeroVector;
	GunKickHandRotation = FRotator::ZeroRotator;
	GunKickTargetTranslation = FVector::ZeroVector;
	GunKickTargetRotation = FRotator::ZeroRotator;
	GunKickAlpha = 0.0f;
	GunKickDirectHandAlpha = 0.0f;
	GunKickTargetAlpha = 0.0f;
	GunKickLeftHandAlpha = 0.0f;

	AActor* Owner = GetOwningActor();
	if (Owner == nullptr)
	{
		GunKickBlendWeight = 0.0f;
		return;
	}
	if (const ULyraHealthComponent* Health = ULyraHealthComponent::FindHealthComponent(Owner))
	{
		if (Health->IsDeadOrDying())
		{
			GunKickBlendWeight = 0.0f;
			return;
		}
	}

	ULyraEquipmentManagerComponent* Equipment = Owner->FindComponentByClass<ULyraEquipmentManagerComponent>();
	ULyraRangedWeaponInstance* Weapon = Equipment ? Equipment->GetFirstInstanceOfType<ULyraRangedWeaponInstance>() : nullptr;
	const ULyraRecoilProfile* Profile = Weapon ? Weapon->GetRecoilProfile() : nullptr;
	if ((Profile == nullptr) || !Profile->WeaponVisual.bEnabled || !ULyraRecoilDebug::IsVisualEnabled())
	{
		GunKickBlendWeight = 0.0f;
		if (Weapon != nullptr && Profile != nullptr)
		{
			if (const APawn* Pawn = Cast<APawn>(Owner); Pawn != nullptr && Pawn->IsLocallyControlled())
			{
				ULyraRecoilDebug::RecordVisualFrame(GetWorld(), Profile, Weapon->GetRecoilState(),
					FWeaponVisualRecoilPose(), Weapon->GetGunKickAimingAlpha(), 0.0f, 0.0f, 0.0f, 0.0f);
			}
		}
		return;
	}

	const FWeaponVisualRecoilPose Pose = Weapon->GetWeaponVisualRecoilPose();
	const FWeaponVisualRecoilSettings& Settings = Profile->WeaponVisual;
	GunKickHandTranslation = Settings.BackAxisBoneSpace.GetSafeNormal() * Pose.BackCm
		+ Settings.UpAxisBoneSpace.GetSafeNormal() * Pose.UpCm
		+ Settings.SideAxisBoneSpace.GetSafeNormal() * Pose.SideCm;
	GunKickHandRotation = FRotator(
		Settings.PitchAxisBoneSpace.Pitch * Pose.PitchDegrees + Settings.YawAxisBoneSpace.Pitch * Pose.YawDegrees + Settings.RollAxisBoneSpace.Pitch * Pose.RollDegrees,
		Settings.PitchAxisBoneSpace.Yaw * Pose.PitchDegrees + Settings.YawAxisBoneSpace.Yaw * Pose.YawDegrees + Settings.RollAxisBoneSpace.Yaw * Pose.RollDegrees,
		Settings.PitchAxisBoneSpace.Roll * Pose.PitchDegrees + Settings.YawAxisBoneSpace.Roll * Pose.YawDegrees + Settings.RollAxisBoneSpace.Roll * Pose.RollDegrees);
	GunKickTargetTranslation = Settings.TargetBackAxisBoneSpace.GetSafeNormal() * Pose.BackCm
		+ Settings.TargetUpAxisBoneSpace.GetSafeNormal() * Pose.UpCm
		+ Settings.TargetSideAxisBoneSpace.GetSafeNormal() * Pose.SideCm;
	GunKickTargetRotation = FRotator(
		Settings.TargetPitchAxisBoneSpace.Pitch * Pose.PitchDegrees + Settings.TargetYawAxisBoneSpace.Pitch * Pose.YawDegrees + Settings.TargetRollAxisBoneSpace.Pitch * Pose.RollDegrees,
		Settings.TargetPitchAxisBoneSpace.Yaw * Pose.PitchDegrees + Settings.TargetYawAxisBoneSpace.Yaw * Pose.YawDegrees + Settings.TargetRollAxisBoneSpace.Yaw * Pose.RollDegrees,
		Settings.TargetPitchAxisBoneSpace.Roll * Pose.PitchDegrees + Settings.TargetYawAxisBoneSpace.Roll * Pose.YawDegrees + Settings.TargetRollAxisBoneSpace.Roll * Pose.RollDegrees);
	float Suppression = GetCurveValue(Settings.SuppressionCurveName);
	if (const USkeletalMeshComponent* Mesh = GetSkelMeshComponent())
	{
		if (Mesh->GetPredictedLODLevel() > Settings.MaxVisualLOD)
		{
			Suppression = 1.0f;
		}
		if (const UAnimInstance* MainInstance = Mesh->GetAnimInstance())
		{
			Suppression = FMath::Max(Suppression, MainInstance->GetCurveValue(Settings.SuppressionCurveName));
		}
	}
	static const FGameplayTag ReloadTag = FGameplayTag::RequestGameplayTag(TEXT("Event.Movement.Reload"), false);
	if (ReloadTag.IsValid())
	{
		if (const UAbilitySystemComponent* AbilitySystem = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Owner))
		{
			if (AbilitySystem->HasMatchingGameplayTag(ReloadTag))
			{
				Suppression = 1.0f;
			}
		}
	}
	const float TargetWeight = 1.0f - FMath::Clamp(Suppression, 0.0f, 1.0f);
	const float BlendTime = (TargetWeight < GunKickBlendWeight) ? Settings.BlendOutTime : Settings.BlendInTime;
	GunKickBlendWeight = (BlendTime <= 0.0f) ? TargetWeight : FMath::FInterpConstantTo(GunKickBlendWeight, TargetWeight, FMath::Max(0.0f, DeltaSeconds), 1.0f / BlendTime);
	GunKickAlpha = Pose.Alpha * GunKickBlendWeight;
	GunKickTargetAlpha = Settings.bDriveRightHandIKTarget ? GunKickAlpha : 0.0f;
	GunKickDirectHandAlpha = Settings.bDriveRightHandIKTarget ? 0.0f : GunKickAlpha;
	GunKickLeftHandAlpha = (Settings.bFollowWeaponWithLeftHand && ULyraRecoilDebug::IsVisualLeftIKEnabled()) ? GunKickAlpha : 0.0f;
	if (const APawn* Pawn = Cast<APawn>(Owner); Pawn != nullptr && Pawn->IsLocallyControlled())
	{
		ULyraRecoilDebug::RecordVisualFrame(GetWorld(), Profile, Weapon->GetRecoilState(), Pose,
			Weapon->GetGunKickAimingAlpha(), GunKickAlpha, GunKickTargetAlpha, GunKickDirectHandAlpha, GunKickLeftHandAlpha);
	}
}
