#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Math/PerspectiveMatrix.h"
#include "Weapons/LyraWeaponSpreadMath.h"
#include "SceneView.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraReticleProjectionTest,
	"Lyra.Recoil.Reticle.ProjectionAndDPI", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraReticleProjectionTest::RunTest(const FString& Parameters)
{
	// Compare against engine projection of actual cone-edge directions, not a second
	// copy of the radius formula. Include ADS, ultrawide, portrait and a split view offset.
	for (const FIntPoint Size : {FIntPoint(1920, 1080), FIntPoint(3440, 1440), FIntPoint(1080, 1920), FIntPoint(960, 1080)})
	for (const double FOV : {90.0, 60.0, 35.0})
	for (const float Angle : {0.0f, 2.0f, 5.0f, 20.0f})
	{
		const FMatrix Projection = FReversedZPerspectiveMatrix(FMath::DegreesToRadians(FOV * 0.5), double(Size.X), double(Size.Y), 10.0);
		const FIntRect Rect(FIntPoint(73, 41), FIntPoint(73, 41) + Size);
		const FVector2D Center = FVector2D(Rect.Min) + FVector2D(Size) * 0.5;
		const float Radius = LyraWeaponSpreadMath::ScreenRadius(Angle, FVector2D(Size), Projection);
		for (int32 Azimuth = 0; Azimuth < 360; Azimuth += 15)
		{
			// Projection-space forward is Z. The ray has exactly Angle/2 deviation.
			const double Theta = FMath::DegreesToRadians(double(Angle) * 0.5);
			const double Phi = FMath::DegreesToRadians(double(Azimuth));
			const FVector Edge(FMath::Sin(Theta) * FMath::Cos(Phi), FMath::Sin(Theta) * FMath::Sin(Phi), FMath::Cos(Theta));
			FVector2D Screen;
			TestTrue(TEXT("Cone edge projects"), FSceneView::ProjectWorldToScreen(Edge * 10000.0, Rect, Projection, Screen));
			TestTrue(TEXT("Radius matches engine-projected edge within 0.002px"), FMath::Abs((Screen - Center).Length() - Radius) < 0.002);
			for (const float Scale : {0.5f, 1.0f, 1.5f, 2.0f})
			{
				const float LayoutRadius = LyraWeaponSpreadMath::WidgetRadius(Radius, Scale);
				TestTrue(TEXT("Slate scaling restores projected pixel radius"), FMath::Abs(LayoutRadius * Scale - Radius) < 0.002f);
			}
		}
	}
	TestEqual(TEXT("No viewport scale produces no invalid layout"), LyraWeaponSpreadMath::WidgetRadius(10.0f, 0.0f), 0.0f);
	const FMatrix ADSProjection = FReversedZPerspectiveMatrix(FMath::DegreesToRadians(17.5), 1920.0, 1080.0, 10.0);
	TestTrue(TEXT("Wide spread during ADS is not capped at 96 pixels"), LyraWeaponSpreadMath::ScreenRadius(5.0f, FVector2D(1920, 1080), ADSProjection) > 96.0f);
	return true;
}
#endif
