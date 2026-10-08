// Copyright Epic Games, Inc. All Rights Reserved.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Recoil/LyraRecoilPatternAdapter.h"
#include "Recoil/LyraRecoilPreviewController.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilEditorInputBoundary,"Lyra.Recoil.Editor.Preview.SubframeInputBoundary",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilEditorInputBoundary::RunTest(const FString&)
{
    TStrongObjectPtr<ULyraRecoilProfile> Profile(NewObject<ULyraRecoilProfile>());
    Profile->SingleShotMode=ERecoilSingleShotMode::InstantWrite;
    Profile->RecoveryDelay=1;
    FLyraRecoilPreviewConfig Config;
    Config.ShotCount=1; Config.TailSeconds=.1f; Config.FrameSeconds=.1f;
    FLyraRecoilPreviewInputEntry Change;
    Change.TimeSeconds=.017f; Change.AimPitchDegrees=-.4f; Change.PoseState=EPoseState::Crouching;
    Change.bOverrideGlobalScale=true; Change.GlobalScale=.5f;
    Config.InputScript.Add(Change);
    FLyraRecoilPreviewController Preview;
    FString Error; TArray<FName> Names;
    TestTrue(TEXT("Snapshot"),Preview.RefreshProfile(Profile.Get(),Error,Names));
    Preview.SetConfig(Config);
    TestTrue(TEXT("Replay"),Preview.Run());
    // Hand-scheduled independent calls at 0, .017, .1. The .017 input must be
    // sampled before Advance over the remainder, rather than delayed to .1.
    const auto* P=Preview.GetSnapshotProfile();
    FRecoilRuntimeState State;
    State.Reset(P); State.SamplePlayerAim(0,0);
    State.SetGlobalScale(1); State.SetPoseMultiplier(1);
    State.ApplyShot(P,1,EPoseState::Standing);
    State.Advance(P,.017f);
    State.SamplePlayerAim(-.4f,0);
    State.SetGlobalScale(.5f);
    State.SetPoseMultiplier(FRecoilRuntimeState::ComputePoseMultiplier(*P,EPoseState::Crouching,0));
    State.Advance(P,.083f);
    const auto& Last=Preview.GetSamples().Last();
    TestTrue(TEXT("Exact subframe compensation matches production runtime"),FMath::IsNearlyEqual(Last.RecoveryCoverPitch,State.RecoveryCoverPitch,1.e-5f));
    TestTrue(TEXT("Exact subframe output matches production runtime"),FMath::IsNearlyEqual(Last.CameraOffsetPitch,State.CameraOffsetPitch,1.e-5f));
    bool bBoundary=false;
    for (const auto& Sample:Preview.GetSamples()) bBoundary|=FMath::IsNearlyEqual(Sample.TimeSeconds,.017f,1.e-6f);
    TestTrue(TEXT("Input boundary is sampled"),bBoundary);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilEditorInvalidPreview,"Lyra.Recoil.Editor.Preview.RejectNonfiniteAndRestoreSourceMode",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilEditorInvalidPreview::RunTest(const FString&)
{
    TStrongObjectPtr<ULyraRecoilProfile> Profile(NewObject<ULyraRecoilProfile>());
    Profile->SingleShotMode=ERecoilSingleShotMode::Interpolated;
    FLyraRecoilPreviewController Preview;
    FString Error; TArray<FName> Names;
    TestTrue(TEXT("Valid snapshot"),Preview.RefreshProfile(Profile.Get(),Error,Names));
    FLyraRecoilPreviewConfig Config;
    Config.SingleShotModeOverride=ELyraRecoilPreviewSingleShotMode::InstantWrite;
    Preview.SetConfig(Config);
    Config.SingleShotModeOverride=ELyraRecoilPreviewSingleShotMode::FromProfile;
    Preview.SetConfig(Config);
    TestEqual(TEXT("FromProfile restores source mode after temporary override"),Preview.GetSnapshotProfile()->SingleShotMode,ERecoilSingleShotMode::Interpolated);
    Config.RPM=std::numeric_limits<float>::quiet_NaN();
    Preview.SetConfig(Config);
    TestFalse(TEXT("NaN RPM rejects simulation"),Preview.Run());
    TestEqual(TEXT("Invalid input has no plot samples"),Preview.GetSamples().Num(),0);
    Config=FLyraRecoilPreviewConfig(); Config.FrameSeconds=0;
    Preview.SetConfig(Config);
    TestFalse(TEXT("Zero step rejects simulation"),Preview.Run());
    Config=FLyraRecoilPreviewConfig(); Config.GlobalScale=std::numeric_limits<float>::infinity();
    Preview.SetConfig(Config);
    TestFalse(TEXT("Infinite scale rejects simulation"),Preview.Run());
    auto Handle=Profile->VerticalKickCurve.EditorCurveData.AddKey(22,1);
    Profile->VerticalKickCurve.EditorCurveData.GetKey(Handle).LeaveTangentWeight=std::numeric_limits<float>::quiet_NaN();
    TestFalse(TEXT("Nonfinite curve weight rejects snapshot"),Preview.RefreshProfile(Profile.Get(),Error,Names));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilEditorStrengthTail,"Lyra.Recoil.Editor.Mapping.StrengthAdjustmentPreservesStoredTail",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilEditorStrengthTail::RunTest(const FString&)
{
    TStrongObjectPtr<ULyraRecoilProfile> P(NewObject<ULyraRecoilProfile>());
    P->PatternPoints={FRecoilPatternPoint(.5f,.5f),FRecoilPatternPoint(.75f,.8f),FRecoilPatternPoint(-.25f,.6f)};
    P->PatternLength=1; P->RecoilPerShot_Horizontal=1; P->RecoilPerShot_Vertical=1;
    P->VerticalKickCurve.EditorCurveData.Reset(); P->VerticalKickCurve.EditorCurveData.AddKey(0,1);
    TArray<FString> Errors; FLyraRecoilPatternData Candidate; FLyraRecoilStrengthAdjustment Adjustment;
    TestTrue(TEXT("Candidate accommodates larger target"),FLyraRecoilPatternAdapter::BuildStrengthAdjustmentCandidate(*P,{FVector2D(2,3)},Adjustment,Candidate,Errors));
    TestEqual(TEXT("Tail X remains exact"),Candidate.Points[1].X,P->PatternPoints[1].X);
    TestEqual(TEXT("Last tail X remains exact"),Candidate.Points[2].X,P->PatternPoints[2].X);
    for (int32 I=1;I<3;++I)
        TestTrue(TEXT("Each stored tail vertical kick retained"),FMath::IsNearlyEqual(Adjustment.NewVertical*Candidate.Points[I].Y,static_cast<double>(P->PatternPoints[I].Y),1.e-6));
    TestTrue(TEXT("Difference preview includes tail samples"),Adjustment.Differences.ContainsByPredicate([](const FString& Row) { return Row.Contains(TEXT("种子")); }));
    TestEqual(TEXT("Real Profile untouched"),P->RecoilPerShot_Vertical,1.f);
    return true;
}
#endif
