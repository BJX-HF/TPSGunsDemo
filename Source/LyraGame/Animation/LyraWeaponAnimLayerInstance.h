#pragma once

#include "Animation/AnimInstance.h"
#include "LyraWeaponAnimLayerInstance.generated.h"

/** Game-thread adapter for the shared linked layer's recoil and two-hand IK controls. */
UCLASS(Blueprintable)
class LYRAGAME_API ULyraWeaponAnimLayerInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	FVector GunKickHandTranslation = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	FRotator GunKickHandRotation = FRotator::ZeroRotator;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	FVector GunKickTargetTranslation = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	FRotator GunKickTargetRotation = FRotator::ZeroRotator;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float GunKickAlpha = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float GunKickDirectHandAlpha = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float GunKickTargetAlpha = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float GunKickLeftHandAlpha = 0.0f;

private:
	float GunKickBlendWeight = 0.0f;
};
