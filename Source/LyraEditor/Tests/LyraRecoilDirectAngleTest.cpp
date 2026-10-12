// Copyright Epic Games, Inc. All Rights Reserved.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"
#include "Recoil/LyraRecoilPatternAdapter.h"
#include "Recoil/LyraRecoilEditOperations.h"
#include "Recoil/LyraRecoilPreviewController.h"
#include "UObject/StrongObjectPtr.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilDirectRuntime,"Lyra.Recoil.Angles.RuntimeAndTail",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilDirectRuntime::RunTest(const FString&)
{
 TStrongObjectPtr<ULyraRecoilProfile> P(NewObject<ULyraRecoilProfile>());
 TestTrue(TEXT("New profiles use degrees"),P->bUseDirectShotAngles);
 TStrongObjectPtr<ULyraRecoilProfile> Clone(DuplicateObject<ULyraRecoilProfile>(P.Get(),GetTransientPackage()));
 TestTrue(TEXT("Degree duplicate preserves units"),Clone->bUseDirectShotAngles);
 P->PatternPoints={FRecoilPatternPoint(-2.5f,6.626f),FRecoilPatternPoint(3,1)}; P->PatternLength=1;
 P->RecoilPerShot_Vertical=999; P->RecoilPerShot_Horizontal=999;
 P->VerticalKickCurve.EditorCurveData.Reset(); P->VerticalKickCurve.EditorCurveData.AddKey(0,2);
 P->TailPitchDegrees=3; P->bUseTailPitchCurve=false; P->HorizontalRandomRangeDegrees=1.5f;
 const auto Kick=FRecoilRuntimeState::ComputeShotKick(*P,0,.5f,2,42);
 TestEqual(TEXT("Stored Pitch is the per-shot angle with neutral net magnitude"),Kick.Vertical,6.626f);
 TestEqual(TEXT("Stored signed Yaw is direct"),Kick.Horizontal,-2.5f);
 TestEqual(TEXT("Array tail Pitch is direct despite curve"),P->GetShotPitchDegrees(1),1.f);
 TestEqual(TEXT("Outside-array Pitch uses its own angle"),P->GetShotPitchDegrees(22),3.f);
 P->bUseTailPitchCurve=true;
 TestEqual(TEXT("Optional curve only scales outside-array tail"),P->GetShotPitchDegrees(22),6.f);
 for(int32 I=1;I<80;++I)
  TestTrue(TEXT("Random tail is bounded in degrees"),FMath::Abs(FRecoilRuntimeState::ComputeShotKick(*P,I,1,1,42).Horizontal)<=1.50001f);
 P->bUseTailPitchCurve=false; P->VerticalKickCurve.EditorCurveData.Reset();
 TArray<FString> Errors;
 TestTrue(TEXT("Degree configuration does not require a fixed-shot multiplier curve"),FLyraRecoilPatternAdapter::Validate(*P,Errors));
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilDirectMigration,"Lyra.Recoil.Angles.LegacyMigration",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilDirectMigration::RunTest(const FString&)
{
 TStrongObjectPtr<ULyraRecoilProfile> P(NewObject<ULyraRecoilProfile>());
 P->bUseDirectShotAngles=false; P->PatternPoints={FRecoilPatternPoint(.8f,.3f),FRecoilPatternPoint(-.6f,.9f),FRecoilPatternPoint(.2f,.7f)};
 P->PatternLength=2; P->RecoilPerShot_Vertical=4; P->RecoilPerShot_Horizontal=3; P->HorizontalRandomRange=.6f;
 P->VerticalKickCurve.EditorCurveData.Reset(); P->VerticalKickCurve.EditorCurveData.AddKey(0,0); P->VerticalKickCurve.EditorCurveData.AddKey(30,3);
 TArray<FRecoilShotKick> Before;
 for(int32 Seed:{17,42,20260917}) for(int32 I=0;I<100;++I) Before.Add(FRecoilRuntimeState::ComputeShotKick(*P,I,.65f,1.3f,Seed));
 TStrongObjectPtr<ULyraRecoilProfile> LegacyClone(DuplicateObject<ULyraRecoilProfile>(P.Get(),GetTransientPackage()));
 TestFalse(TEXT("Legacy duplicate does not trigger disk migration"),LegacyClone->bUseDirectShotAngles);
 TStrongObjectPtr<ULyraRecoilProfile> LegacyDefaults(NewObject<ULyraRecoilProfile>());
 LegacyDefaults->bUseDirectShotAngles=false;
 LegacyDefaults->PatternPoints=GetDefault<ULyraRecoilProfile>()->PatternPoints;
 LegacyDefaults->bUseTailPitchCurve=false;
 TStrongObjectPtr<ULyraRecoilProfile> DefaultClone(DuplicateObject<ULyraRecoilProfile>(LegacyDefaults.Get(),GetTransientPackage()));
 TestFalse(TEXT("Omitted default unit marker remains legacy"),DefaultClone->bUseDirectShotAngles);
 TestEqual(TEXT("Omitted default points are not rescaled"),DefaultClone->PatternPoints[0].Y,LegacyDefaults->PatternPoints[0].Y);
 TestFalse(TEXT("Omitted tail curve option remains disabled"),DefaultClone->bUseTailPitchCurve);
 TStrongObjectPtr<ULyraRecoilProfile> ExplicitTemplate(NewObject<ULyraRecoilProfile>(GetTransientPackage(),NAME_None,RF_NoFlags,LegacyDefaults.Get()));
 TestFalse(TEXT("Explicit template construction retains source units"),ExplicitTemplate->bUseDirectShotAngles);
 TestTrue(TEXT("Legacy conversion succeeds"),P->ConvertLegacyPatternToAngles());
 int32 N=0;
 for(int32 Seed:{17,42,20260917}) for(int32 I=0;I<100;++I)
 {
  const auto After=FRecoilRuntimeState::ComputeShotKick(*P,I,.65f,1.3f,Seed);
  TestTrue(FString::Printf(TEXT("Migration retains shot %d vertical for seed %d"),I,Seed),FMath::IsNearlyEqual(After.Vertical,Before[N].Vertical,1.e-4f));
  TestTrue(FString::Printf(TEXT("Migration retains shot %d horizontal for seed %d"),I,Seed),FMath::IsNearlyEqual(After.Horizontal,Before[N++].Horizontal,1.e-4f));
 }
 const auto Converted=FLyraRecoilPatternAdapter::Read(*P);
 TestTrue(TEXT("Repeated conversion is safe"),P->ConvertLegacyPatternToAngles());
 TestTrue(TEXT("Repeated conversion does not scale twice"),FLyraRecoilPatternAdapter::Equal(Converted,FLyraRecoilPatternAdapter::Read(*P)));
 P->bUseDirectShotAngles=false; P->PatternPoints.Reset();
 Before.Reset(); for(int32 I=0;I<10;++I) Before.Add(FRecoilRuntimeState::ComputeShotKick(*P,I,1,1,17));
 TestTrue(TEXT("Empty legacy array converts"),P->ConvertLegacyPatternToAngles());
 for(int32 I=0;I<10;++I) TestEqual(TEXT("Empty-array vertical fallback is retained"),P->GetShotPitchDegrees(I),Before[I].Vertical);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilDirectMapping,"Lyra.Recoil.Angles.CanvasMapping",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilDirectMapping::RunTest(const FString&)
{
 TStrongObjectPtr<ULyraRecoilProfile> P(NewObject<ULyraRecoilProfile>());
 P->PatternPoints={FRecoilPatternPoint(0,1),FRecoilPatternPoint(0,1)}; P->PatternLength=2;
 FLyraRecoilPatternData Candidate; TArray<FString> Errors;
 TestTrue(TEXT("Canvas accepts angles greater than one"),FLyraRecoilPatternAdapter::MoveCumulative(*P,{FVector2D(2.5,6.626),FVector2D(1.5,7.626)},Candidate,Errors));
 TestTrue(TEXT("First per-shot angle is stored directly"),FMath::IsNearlyEqual(Candidate.Points[0].Y,6.626f,1.e-5f));
 TestEqual(TEXT("Second stored angle is the adjacent difference"),Candidate.Points[1].Y,1.f);
 TestEqual(TEXT("Horizontal signed difference is direct"),Candidate.Points[1].X,-1.f);
 Errors.Reset();
 TestFalse(TEXT("Descending cumulative Pitch rejects a negative shot"),FLyraRecoilPatternAdapter::MoveCumulative(*P,{FVector2D(0,5),FVector2D(0,4)},Candidate,Errors));
 TestTrue(TEXT("Rejected movement returns unchanged data"),FLyraRecoilPatternAdapter::Equal(Candidate,FLyraRecoilPatternAdapter::Read(*P)));
 TestTrue(TEXT("Diagnostic explains neighboring shots"),!Errors.IsEmpty() && Errors[0].Contains(TEXT("相邻")));
 Errors.Reset();
 TestFalse(TEXT("Unrepresentable angle is rejected"),FLyraRecoilPatternAdapter::MoveCumulative(*P,{FVector2D(std::numeric_limits<double>::infinity(),1),FVector2D(0,2)},Candidate,Errors));
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilDirectClipboard,"Lyra.Recoil.Angles.ClipboardUnits",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilDirectClipboard::RunTest(const FString&)
{
 TStrongObjectPtr<ULyraRecoilProfile> Source(NewObject<ULyraRecoilProfile>());
 Source->PatternPoints={FRecoilPatternPoint(-2.5f,6.626f)}; Source->PatternLength=1;
 TArray<FString> Errors; FLyraRecoilClipboardData Clip;
 const FString DegreePayload=FLyraRecoilPatternAdapter::Copy(*Source,{0});
 TestTrue(TEXT("v2 degree clipboard parses"),FLyraRecoilPatternAdapter::ParseClipboardEx(DegreePayload,Clip,Errors));
 TestTrue(TEXT("Clipboard explicitly marks degree units"),Clip.bUsesDirectAngles);
 TestEqual(TEXT("Degree payload retains angle greater than one"),Clip.Points[0].Y,6.626f);
 FString Bad=DegreePayload.Replace(TEXT("degrees"),TEXT("radians"));
 TestFalse(TEXT("Unknown v2 unit is rejected"),FLyraRecoilPatternAdapter::ParseClipboardEx(Bad,Clip,Errors));
 Source->bUseDirectShotAngles=false; Source->PatternPoints={FRecoilPatternPoint(.4f,.5f)};
 Source->RecoilPerShot_Horizontal=4; Source->RecoilPerShot_Vertical=10;
 Source->VerticalKickCurve.EditorCurveData.Reset(); Source->VerticalKickCurve.EditorCurveData.AddKey(0,2);
 Errors.Reset();
 const FString LegacyPayload=FLyraRecoilPatternAdapter::Copy(*Source,{0});
 TestTrue(TEXT("Legacy v1 is accepted"),FLyraRecoilPatternAdapter::ParseClipboardEx(LegacyPayload,Clip,Errors));
 TestFalse(TEXT("v1 is explicitly legacy normalized units"),Clip.bUsesDirectAngles);
 TStrongObjectPtr<ULyraRecoilProfile> Target(NewObject<ULyraRecoilProfile>());
 TArray<FRecoilPatternPoint> Points;
 TestTrue(TEXT("v1 source angles convert to degrees"),FLyraRecoilPatternAdapter::ConvertClipboardAngles(*Target,Clip,0,Points,Errors));
 TestEqual(TEXT("Legacy pitch converted using source parameters"),Points[0].Y,10.f);
 TStrongObjectPtr<ULyraRecoilEditorSession> Session(NewObject<ULyraRecoilEditorSession>());
 Session->Initialize(Target.Get()); Session->SelectIndices({0},false);
 FLyraRecoilEditOperations Ops; Ops.Initialize(Target.Get(),Session.Get());
 const bool bPasted = Ops.PasteParameters(LegacyPayload,Errors);
 TestTrue(TEXT("Default paste converts cross-unit data"),bPasted);
 if (!bPasted) for (const FString& Error : Errors) AddError(Error);
 TestEqual(TEXT("Default paste stores degrees"),Target->PatternPoints[0].Y,10.f);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilDirectTransactions,"Lyra.Recoil.Angles.TransactionsAndPreview",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilDirectTransactions::RunTest(const FString&)
{
 TStrongObjectPtr<ULyraRecoilProfile> P(NewObject<ULyraRecoilProfile>(GetTransientPackage(),NAME_None,RF_Transactional));
 P->PatternPoints={FRecoilPatternPoint(0,1),FRecoilPatternPoint(0,1)}; P->PatternLength=2;
 TStrongObjectPtr<ULyraRecoilEditorSession> Session(NewObject<ULyraRecoilEditorSession>()); Session->Initialize(P.Get());
 FLyraRecoilEditOperations Ops; Ops.Initialize(P.Get(),Session.Get()); TArray<FString> Errors;
 TestTrue(TEXT("Table commits angle beyond normalized range"),Ops.CommitPointNormalized(0,-2,3,Errors));
 TestEqual(TEXT("Stored table pitch is degrees"),P->PatternPoints[0].Y,3.f);
 GEditor->UndoTransaction(); TestEqual(TEXT("Undo restores degrees"),P->PatternPoints[0].Y,1.f);
 GEditor->RedoTransaction(); TestEqual(TEXT("Redo restores degree edit"),P->PatternPoints[0].Y,3.f);
 TestFalse(TEXT("Negative upward angle is rejected atomically"),Ops.CommitPointNormalized(0,0,-1,Errors));
 TestEqual(TEXT("Rejected edit retains degrees"),P->PatternPoints[0].Y,3.f);
 FLyraRecoilPreviewController Preview; FString Error; TArray<FName> Names;
 TestTrue(TEXT("Degree profile creates preview snapshot"),Preview.RefreshProfile(P.Get(),Error,Names));
 FLyraRecoilPreviewConfig Config; Config.ShotCount=2; Config.TailSeconds=.2f; Preview.SetConfig(Config);
 TestTrue(TEXT("Degree preview simulates"),Preview.Run());
 TestTrue(TEXT("Degree preview produces samples"),!Preview.GetSamples().IsEmpty());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilDirectAssetMigration,"Lyra.Recoil.Angles.MigratedAssets",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilDirectAssetMigration::RunTest(const FString&)
{
 for(const TCHAR* Name:{TEXT("DA_Recoil_Rifle"),TEXT("DA_Recoil_Pistol"),TEXT("DA_Recoil_Shotgun"),TEXT("DA_Recoil_Rifle_S"),TEXT("DA_Recoil_Rifle_7")})
 {
  const FString Path=FString::Printf(TEXT("/Game/Weapons/Recoil/%s.%s"),Name,Name);
  auto* P=LoadObject<ULyraRecoilProfile>(nullptr,*Path);
  if(!TestNotNull(*Path,P)) continue;
  TestTrue(TEXT("Old on-disk asset loads into degree format"),P->bUseDirectShotAngles);
  for(int32 I=0;I<P->PatternPoints.Num();++I)
   TestEqual(TEXT("Array shots use the stored Pitch angle directly"),P->GetShotPitchDegrees(I),P->PatternPoints[I].Y);
  // After a user saves an upgraded package there is no legacy conversion to audit on the next load.
  if(!P->HasPreMigrationPointsForTests()) { AddInfo(Path+TEXT(" already saved in degree format; no legacy conversion on this load.")); continue; }
  TStrongObjectPtr<ULyraRecoilProfile> Legacy(NewObject<ULyraRecoilProfile>());
  Legacy->bUseDirectShotAngles=false; Legacy->PatternPoints=P->GetPreMigrationPointsForTests();
  Legacy->PatternLength=P->PatternLength;
  Legacy->RecoilPerShot_Vertical=P->RecoilPerShot_Vertical; Legacy->RecoilPerShot_Horizontal=P->RecoilPerShot_Horizontal;
  Legacy->HorizontalRandomRange=P->HorizontalRandomRange; Legacy->VerticalKickCurve=P->VerticalKickCurve;
  for(int32 Seed:{17,42,20260917}) for(int32 I=0;I<100;++I)
  {
   const auto Before=FRecoilRuntimeState::ComputeShotKick(*Legacy,I,.65f,1.3f,Seed);
   const auto After=FRecoilRuntimeState::ComputeShotKick(*P,I,.65f,1.3f,Seed);
   TestTrue(FString::Printf(TEXT("%s seed %d shot %d Pitch retains current on-disk configuration"),Name,Seed,I),FMath::IsNearlyEqual(Before.Vertical,After.Vertical,1.e-4f));
   TestTrue(FString::Printf(TEXT("%s seed %d shot %d Yaw retains current on-disk configuration"),Name,Seed,I),FMath::IsNearlyEqual(Before.Horizontal,After.Horizontal,1.e-4f));
  }
 }
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilDirectPersistence,"Lyra.Recoil.Angles.SaveUnloadReload",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilDirectPersistence::RunTest(const FString&)
{
 const FString Tag=FGuid::NewGuid().ToString(EGuidFormats::Digits);
 const FString Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("RecoilGUI/DirectAngleTests")/Tag);
 IFileManager::Get().MakeDirectory(*Folder,true);
 const FString Mount=TEXT("/RecoilDegreeQA/"); FPackageName::RegisterMountPoint(Mount,Folder+TEXT("/"));
 const FString Name=Mount+TEXT("Profile_")+Tag, File=Folder/(TEXT("Profile_")+Tag+TEXT(".uasset"));
 UPackage* Package=CreatePackage(*Name);
 auto* P=NewObject<ULyraRecoilProfile>(Package,TEXT("Profile"),RF_Public|RF_Standalone|RF_Transactional);
 P->PatternPoints={FRecoilPatternPoint(-2.5f,6.626f),FRecoilPatternPoint(1,2)}; P->PatternLength=2;
 P->TailPitchDegrees=3; P->HorizontalRandomRangeDegrees=1.25f; P->bUseTailPitchCurve=false;
 const auto Expected=FLyraRecoilPatternAdapter::Read(*P);
 FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone; Args.SaveFlags=SAVE_NoError;
 TestTrue(TEXT("Save versioned degree package"),UPackage::SavePackage(Package,P,*File,Args));
 TWeakObjectPtr<ULyraRecoilProfile> Previous(P); P=nullptr;
 TestTrue(TEXT("Unload saved degree package"),UPackageTools::UnloadPackages({Package})); Package=nullptr;
 CollectGarbage(RF_NoFlags); TestFalse(TEXT("Original degree instance released"),Previous.IsValid());
 UPackage* Reloaded=LoadPackage(nullptr,*Name,LOAD_None);
 auto* Loaded=Reloaded?FindObject<ULyraRecoilProfile>(Reloaded,TEXT("Profile")):nullptr;
 if(TestNotNull(TEXT("Degree package reloads from disk"),Loaded))
 {
  TestTrue(TEXT("Reload preserves degree unit marker"),Loaded->bUseDirectShotAngles);
  TestTrue(TEXT("Reload does not convert degrees a second time"),FLyraRecoilPatternAdapter::Equal(Expected,FLyraRecoilPatternAdapter::Read(*Loaded)));
  TestEqual(TEXT("Saved angle above one is unchanged"),Loaded->PatternPoints[0].Y,6.626f);
  TestEqual(TEXT("Independent array-external tail persists"),Loaded->GetShotPitchDegrees(20),3.f);
  TestEqual(TEXT("Random tail degree range persists"),Loaded->HorizontalRandomRangeDegrees,1.25f);
 }
 if(Reloaded) UPackageTools::UnloadPackages({Reloaded});
 FPackageName::UnRegisterMountPoint(Mount,Folder+TEXT("/"));
 if(TestTrue(TEXT("Temporary package is under Saved"),Folder.StartsWith(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())))) IFileManager::Get().Delete(*File);
 return true;
}
#endif
