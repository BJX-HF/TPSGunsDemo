// Copyright Epic Games, Inc. All Rights Reserved.

#include "LyraReticleWidgetBase.h"

#include "Inventory/LyraInventoryItemInstance.h"
#include "Weapons/LyraRangedWeaponInstance.h"
#include "Weapons/LyraWeaponInstance.h"
#include "Weapons/LyraWeaponSpreadMath.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "SceneView.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LyraReticleWidgetBase)

ULyraReticleWidgetBase::ULyraReticleWidgetBase(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void ULyraReticleWidgetBase::InitializeFromWeapon(ULyraWeaponInstance* InWeapon)
{
	WeaponInstance = InWeapon;
	InventoryInstance = nullptr;
	if (WeaponInstance)
	{
		InventoryInstance = Cast<ULyraInventoryItemInstance>(WeaponInstance->GetInstigator());
	}
	OnWeaponInitialized();
}


float ULyraReticleWidgetBase::ComputeSpreadAngle() const
{
	if (const ULyraRangedWeaponInstance* RangedWeapon = Cast<const ULyraRangedWeaponInstance>(WeaponInstance))
	{
		return RangedWeapon->GetEffectiveSpreadAngle();
	}
	else
	{
		return 0.0f;
	}
}

bool ULyraReticleWidgetBase::HasFirstShotAccuracy() const
{
	if (const ULyraRangedWeaponInstance* RangedWeapon = Cast<const ULyraRangedWeaponInstance>(WeaponInstance))
	{
		return RangedWeapon->HasFirstShotAccuracy();
	}
	else
	{
		return false;
	}
}

float ULyraReticleWidgetBase::ComputeMaxScreenspaceSpreadRadius() const
{
	const APlayerController* PC = GetOwningPlayer();
	const ULocalPlayer* Player = PC ? PC->GetLocalPlayer() : nullptr;
	if (Player && Player->ViewportClient && Player->ViewportClient->Viewport)
	{
		FSceneViewProjectionData Projection;
		if (Player->GetProjectionData(Player->ViewportClient->Viewport, Projection))
		{
			// r = tan(full cone angle / 2) * focal length in pixels.
			// The projection matrix already contains ADS FOV, aspect-ratio constraints,
			// and the player's split-screen view. No arbitrary world distance is needed.
			const FIntRect View = Projection.GetConstrainedViewRect();
			return LyraWeaponSpreadMath::ScreenRadius(ComputeSpreadAngle(),
				FVector2D(View.Width(), View.Height()), Projection.ProjectionMatrix);
		}
	}
	return 0.0f;
}

float ULyraReticleWidgetBase::ComputeSpreadRadiusInWidgetUnits() const
{
	return LyraWeaponSpreadMath::WidgetRadius(ComputeMaxScreenspaceSpreadRadius(),
		UWidgetLayoutLibrary::GetViewportScale(this));
}
