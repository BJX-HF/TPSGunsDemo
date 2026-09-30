#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Weapons/Recoil/LyraWeaponVisualRecoilState.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeaponVisualV1ShotContractTest,
	"Lyra.Recoil.WeaponVisualV1.ShotContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeaponVisualV1ShotContractTest::RunTest(const FString& Parameters)
{
	FWeaponVisualRecoilSettings Settings;
	Settings.bEnabled = true;
	FWeaponVisualRecoilState State;
	TestEqual(TEXT("No committed event means no visual shot"), State.GetPose().ShotSerial, 0);
	TestTrue(TEXT("First committed shot accepted"), State.ApplyShot(Settings, 1, 1.0f, 1.0f, 1.0f));
	TestFalse(TEXT("Same serial cannot trigger twice"), State.ApplyShot(Settings, 1, 1.0f, 1.0f, 1.0f));
	TestEqual(TEXT("Serial stays at one"), State.GetPose().ShotSerial, 1);
	State.Advance(Settings, Settings.AttackDuration);
	TestTrue(TEXT("Pitch visible"), State.GetPose().PitchDegrees > 0.0f);
	TestTrue(TEXT("Back visible"), State.GetPose().BackCm > 0.0f);
	State.Reset();
	TestEqual(TEXT("Unequip clears serial"), State.GetPose().ShotSerial, 0);
	TestEqual(TEXT("Unequip clears pitch"), State.GetPose().PitchDegrees, 0.0f);
	TestEqual(TEXT("Unequip clears back"), State.GetPose().BackCm, 0.0f);
	Settings.bEnabled = false;
	TestFalse(TEXT("Disabled weapon cannot trigger"), State.ApplyShot(Settings, 1, 1.0f, 1.0f, 1.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeaponVisualV1FrameRateTest,
	"Lyra.Recoil.WeaponVisualV1.FrameRate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeaponVisualV1FrameRateTest::RunTest(const FString& Parameters)
{
	FWeaponVisualRecoilSettings Settings;
	Settings.bEnabled = true;
	Settings.AttackDuration = 0.05f;
	auto Run = [&Settings](int32 FPS)
	{
		FWeaponVisualRecoilState State;
		State.ApplyShot(Settings, 1, 1.0f, 1.0f, 1.0f);
		float Peak = 0.0f;
		for (int32 Frame = 0; Frame < FPS; ++Frame)
		{
			State.Advance(Settings, 1.0f / FPS);
			Peak = FMath::Max(Peak, State.GetPose().PitchDegrees);
		}
		return Peak;
	};
	const float Peak30 = Run(30);
	const float Peak60 = Run(60);
	const float Peak120 = Run(120);
	TestTrue(TEXT("30 FPS reaches a peak"), Peak30 > 0.0f);
	TestTrue(TEXT("30/60 FPS peaks close"), FMath::Abs(Peak30 - Peak60) < 0.02f);
	TestTrue(TEXT("60/120 FPS peaks close"), FMath::Abs(Peak60 - Peak120) < 0.02f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeaponVisualV2SixAxisTest,
	"Lyra.Recoil.WeaponVisualV2.SixAxisMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeaponVisualV2SixAxisTest::RunTest(const FString& Parameters)
{
	FWeaponVisualRecoilSettings Settings;
	Settings.bEnabled = true;
	Settings.UpCmPerShot = 1.0f;
	Settings.SideCmPerShot = 1.0f;
	Settings.RollFromYaw = 0.5f;
	Settings.ADSVisualScale = 0.5f;
	Settings.MaxYawDegrees = 0.4f;
	Settings.MaxRollDegrees = 0.2f;
	Settings.MaxSideCm = 1.0f;
	FWeaponVisualRecoilState Left;
	FWeaponVisualRecoilState Right;
	FWeaponVisualRecoilState Center;
	TestTrue(TEXT("Left shot accepted"), Left.ApplyShot(Settings, 1, 1.0f, 0.8f, 1.0f, -1.0f));
	TestTrue(TEXT("Right shot accepted"), Right.ApplyShot(Settings, 1, 1.0f, 0.8f, 1.0f, 1.0f));
	TestTrue(TEXT("Zero horizontal shot accepted"), Center.ApplyShot(Settings, 1, 1.0f, 0.8f, 1.0f, 0.0f));
	Left.Advance(Settings, 0.1f);
	Right.Advance(Settings, 0.1f);
	Center.Advance(Settings, 0.1f);
	TestTrue(TEXT("Actual left horizontal kick makes left visual yaw"), Left.GetPose().YawDegrees < 0.0f);
	TestTrue(TEXT("Actual left horizontal kick makes left side travel"), Left.GetPose().SideCm < 0.0f);
	TestTrue(TEXT("Right kick reverses yaw and side"), Right.GetPose().YawDegrees > 0.0f && Right.GetPose().SideCm > 0.0f);
	TestTrue(TEXT("Zero horizontal kick adds no side travel"), Center.GetPose().SideCm == 0.0f);
	TestTrue(TEXT("Roll follows configured yaw sign"), Left.GetPose().RollDegrees < 0.0f && Right.GetPose().RollDegrees > 0.0f);
	TestTrue(TEXT("Up and back remain positive"), Left.GetPose().UpCm > 0.0f && Left.GetPose().BackCm > 0.0f);
	TestTrue(TEXT("Yaw capped during attack"), FMath::Abs(Left.GetPose().YawDegrees) <= Settings.MaxYawDegrees + KINDA_SMALL_NUMBER);
	TestTrue(TEXT("Roll capped during attack"), FMath::Abs(Left.GetPose().RollDegrees) <= Settings.MaxRollDegrees + KINDA_SMALL_NUMBER);
	for (int32 Serial = 2; Serial <= 30; ++Serial)
	{
		Left.ApplyShot(Settings, Serial, 1.0f, 1.0f, 1.0f, -1.0f);
		Left.Advance(Settings, 0.1f);
	}
	TestTrue(TEXT("Long burst remains within side clamp"), FMath::Abs(Left.GetPose().SideCm) <= Settings.MaxSideCm + KINDA_SMALL_NUMBER);
	TestTrue(TEXT("Long burst remains within back clamp"), Left.GetPose().BackCm <= Settings.MaxBackCm + KINDA_SMALL_NUMBER);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeaponVisualV2AlignmentTest,
	"Lyra.Recoil.WeaponVisualV2.AlignmentAndADS",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWeaponVisualV2AlignmentTest::RunTest(const FString& Parameters)
{
	FWeaponVisualRecoilSettings Settings;
	Settings.bEnabled = true;
	Settings.ADSVisualScale = 0.5f;
	Settings.CameraPitchAlignment = 0.5f;
	Settings.CameraYawAlignment = 0.5f;
	Settings.MaxAlignmentDegrees = 0.2f;
	FWeaponVisualRecoilState Hip;
	FWeaponVisualRecoilState ADS;
	Hip.ApplyShot(Settings, 1, 1.0f, 1.0f, 1.0f, 0.4f, 0.0f);
	ADS.ApplyShot(Settings, 1, 1.0f, 1.0f, 1.0f, 0.4f, 1.0f);
	Hip.Advance(Settings, 0.1f);
	ADS.Advance(Settings, 0.1f);
	TestTrue(TEXT("Separate ADS visual scale reduces pitch"), ADS.GetPose().PitchDegrees < Hip.GetPose().PitchDegrees);
	TestTrue(TEXT("Separate ADS visual scale reduces displacement"), ADS.GetPose().BackCm < Hip.GetPose().BackCm);
	FWeaponVisualRecoilState Control = Hip;
	Control.Advance(Settings, FWeaponVisualRecoilState::FixedStepSeconds);
	Hip.Advance(Settings, FWeaponVisualRecoilState::FixedStepSeconds, 100.0f, -100.0f);
	TestTrue(TEXT("Camera pitch alignment independently capped"), FMath::IsNearlyEqual(Hip.GetPose().PitchDegrees - Control.GetPose().PitchDegrees, Settings.MaxAlignmentDegrees, 0.001f));
	TestTrue(TEXT("Camera yaw alignment independently capped"), FMath::IsNearlyEqual(Hip.GetPose().YawDegrees - Control.GetPose().YawDegrees, -Settings.MaxAlignmentDegrees, 0.001f));
	Hip.Advance(Settings, FWeaponVisualRecoilState::FixedStepSeconds, 0.0f, 0.0f);
	Control.Advance(Settings, FWeaponVisualRecoilState::FixedStepSeconds);
	TestTrue(TEXT("Alignment does not feed into mechanical state"), FMath::IsNearlyEqual(Hip.GetPose().PitchDegrees, Control.GetPose().PitchDegrees, 0.001f));
	return true;
}

#endif
