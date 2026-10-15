#pragma once

#include "CoreMinimal.h"

namespace LyraWeaponSpreadMath
{
	/** Spread is a full cone angle everywhere except the final sampling/projection step. */
	inline float HalfAngleRadians(float FullAngleDegrees)
	{
		return FMath::DegreesToRadians(FMath::Clamp(FullAngleDegrees, 0.0f, 179.0f) * 0.5f);
	}

	/** Perspective projection of a camera-aligned cone; uses the actual constrained view. */
	inline float ScreenRadius(float FullAngleDegrees, const FVector2D& ViewSize, const FMatrix& Projection)
	{
		const double FocalPixels = 0.5 * FMath::Max(
			ViewSize.X * FMath::Abs(Projection.M[0][0]),
			ViewSize.Y * FMath::Abs(Projection.M[1][1]));
		return FMath::Tan(HalfAngleRadians(FullAngleDegrees)) * FocalPixels;
	}

	inline float WidgetRadius(float ScreenPixels, float ViewportScale)
	{
		return ViewportScale > UE_SMALL_NUMBER ? ScreenPixels / ViewportScale : 0.0f;
	}
}
