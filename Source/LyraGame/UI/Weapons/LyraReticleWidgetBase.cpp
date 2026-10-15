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

	// Some PIE, split-screen, and Pixel Streaming viewports do not expose a
	// projection through ULocalPlayer yet, even though the player camera can
	// already project world positions. Keep the same cone-to-screen mapping as
	// a fallback instead of returning zero and collapsing all markers at the
	// reticle center.
	if (PC && PC->PlayerCameraManager)
	{
		constexpr float ProjectionDistance = 10000.0f;
		const float SpreadRadiusRadians = LyraWeaponSpreadMath::HalfAngleRadians(ComputeSpreadAngle());
		const float SpreadRadiusAtDistance = FMath::Tan(SpreadRadiusRadians) * ProjectionDistance;

		FVector CameraLocation;
		FRotator CameraRotation;
		PC->PlayerCameraManager->GetCameraViewPoint(CameraLocation, CameraRotation);
		const FVector CameraForward = CameraRotation.RotateVector(FVector::ForwardVector);
		const FVector CameraUp = CameraRotation.RotateVector(FVector::UpVector);
		const FVector EdgePoint = CameraLocation
			+ CameraForward * ProjectionDistance
			+ CameraUp * SpreadRadiusAtDistance;

		FVector2D ScreenPoint;
		if (PC->ProjectWorldLocationToScreen(EdgePoint, ScreenPoint, true))
		{
			int32 ViewportWidth = 0;
			int32 ViewportHeight = 0;
			PC->GetViewportSize(ViewportWidth, ViewportHeight);
			if (ViewportWidth > 0 && ViewportHeight > 0)
			{
				const FVector2D ScreenCenter(ViewportWidth * 0.5f, ViewportHeight * 0.5f);
				return (ScreenPoint - ScreenCenter).Length();
			}
		}
	}

	return 0.0f;
}

float ULyraReticleWidgetBase::ComputeSpreadRadiusInWidgetUnits() const
{
	return LyraWeaponSpreadMath::WidgetRadius(ComputeMaxScreenspaceSpreadRadius(),
		UWidgetLayoutLibrary::GetViewportScale(this));
}
