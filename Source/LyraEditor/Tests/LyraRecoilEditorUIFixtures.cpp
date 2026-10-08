// Copyright Epic Games, Inc. All Rights Reserved.
// Development-only reproducible UI fixture. Creates assets under Saved, never under Content.
#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Recoil/LyraRecoilProfileEditor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/SavePackage.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Containers/Ticker.h"

namespace RecoilGUIFixture
{
static TWeakObjectPtr<ULyraRecoilProfile> Profile;
static FString Folder;
ULyraRecoilProfile* GetProfile() { return Profile.Get(); }
static void Open()
{
    if (!GEditor) return;
    const FString Mount=TEXT("/RecoilGUIQA/");
    if (Folder.IsEmpty())
    {
        Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("RecoilGUI/Interactive"));
        IFileManager::Get().MakeDirectory(*Folder,true);
        FPackageName::RegisterMountPoint(Mount,Folder+TEXT("/"));
    }
    auto* Source=LoadObject<ULyraRecoilProfile>(nullptr,TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle.DA_Recoil_Rifle"));
    if (!Source) return;
    const FString Name=TEXT("QA_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    auto* Package=CreatePackage(*(Mount+Name));
    auto* Copy=DuplicateObject<ULyraRecoilProfile>(Source,Package,*Name);
    Copy->ClearFlags(RF_Transient); Copy->SetFlags(RF_Public|RF_Standalone|RF_Transactional);
    FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone;
    if (!UPackage::SavePackage(Package,Copy,*(Folder/(Name+TEXT(".uasset"))),Args)) return;
    Profile=Copy;
    GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Copy);
    UE_LOG(LogTemp,Display,TEXT("RecoilGUIQA opened %s under Saved; source untouched."),*Copy->GetPathName());
}
static void Report()
{
    auto* P=Profile.Get(); if (!P) return;
    auto Toolkit=FLyraRecoilProfileEditor::FindEditorForProfile(P);
    FString Text=FString::Printf(TEXT("asset=%s\ndirty=%d\nN=%d L=%d\nH=%.9g V=%.9g\n"),
        *P->GetPathName(),P->GetOutermost()->IsDirty(),P->PatternPoints.Num(),P->PatternLength,P->RecoilPerShot_Horizontal,P->RecoilPerShot_Vertical);
    for (int32 I=0;I<P->PatternPoints.Num();++I)
        Text+=FString::Printf(TEXT("%d X=%.9g Y=%.9g ID=%s\n"),I,P->PatternPoints[I].X,P->PatternPoints[I].Y,
            Toolkit?*Toolkit->GetSession()->GetNodeId(I).ToString():TEXT("closed"));
    if (Toolkit)
    {
        Text+=FString::Printf(TEXT("revision=%llu undoQueue=%d selected=%s\n"),Toolkit->GetRevision(),GEditor->Trans->GetQueueLength(),
            *FString::JoinBy(Toolkit->GetSelectedIndices(),TEXT(","),[](int32 I){return FString::FromInt(I);}));
        Text+=TEXT("diagnostics=\n")+FString::Join(Toolkit->GetDiagnostics(),TEXT("\n"));
        Text+=TEXT("\n")+Toolkit->GetGraphPerformanceSummary();
    }
    Text+=FString::Printf(TEXT("\nApplicationScale=%.2f\n"),FSlateApplication::Get().GetApplicationScale());
    for (auto Window:FSlateApplication::Get().GetTopLevelWindows())
        Text+=FString::Printf(TEXT("Window=%s DPI=%.2f size=%s\n"),*Window->GetTitle().ToString(),Window->GetDPIScaleFactor(),*Window->GetSizeInScreen().ToString());
    FFileHelper::SaveStringToFile(Text,*(Folder/TEXT("LatestReport.txt")));
    UE_LOG(LogTemp,Display,TEXT("RecoilGUIQA report: %s"),*Text);
}
static FAutoConsoleCommand OpenCommand(TEXT("Lyra.Recoil.GUIQA.Open"),TEXT("Open a saved temporary clone for GUI acceptance."),FConsoleCommandDelegate::CreateStatic(&Open));
static FAutoConsoleCommand ReportCommand(TEXT("Lyra.Recoil.GUIQA.Report"),TEXT("Write current temporary GUI fixture raw fields and node identities under Saved."),FConsoleCommandDelegate::CreateStatic(&Report));
static void Reopen()
{
    if (GEditor && Profile.IsValid()) GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Profile.Get());
}
static void Thousand()
{
    Open();
    if (!Profile.IsValid()) return;
    FLyraRecoilPatternData Data=FLyraRecoilPatternAdapter::Read(*Profile);
    Data.Points.Init(FRecoilPatternPoint(0,.5f),1000); Data.PatternLength=1000;
    auto Toolkit=FLyraRecoilProfileEditor::FindEditorForProfile(Profile.Get());
    if (Toolkit)
    {
        Toolkit->CommitPattern(Data,FText::FromString(TEXT("QA 1000-node temporary fixture")));
        Toolkit->StartGraphPerformanceCapture();
    }
    Report();
}
static FAutoConsoleCommand ReopenCommand(TEXT("Lyra.Recoil.GUIQA.Reopen"),TEXT("Reopen the current Saved-only GUI fixture."),FConsoleCommandDelegate::CreateStatic(&Reopen));
static FAutoConsoleCommand ThousandCommand(TEXT("Lyra.Recoil.GUIQA.Thousand"),TEXT("Open a Saved-only 1000-node GUI fixture."),FConsoleCommandDelegate::CreateStatic(&Thousand));
static void LoadSaved(const TArray<FString>& Args)
{
    if (Args.Num()!=1 || !Args[0].StartsWith(TEXT("QA_")) || Args[0].Len()!=35) return;
    for (TCHAR C:Args[0].Right(32)) if (!FChar::IsHexDigit(C)) return;
    Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("RecoilGUI/Interactive"));
    FPackageName::RegisterMountPoint(TEXT("/RecoilGUIQA/"),Folder+TEXT("/"));
    Profile=LoadObject<ULyraRecoilProfile>(nullptr,*(TEXT("/RecoilGUIQA/")+Args[0]+TEXT(".")+Args[0]));
    Reopen(); Report();
}
static FAutoConsoleCommand LoadSavedCommand(TEXT("Lyra.Recoil.GUIQA.LoadSaved"),TEXT("Load a specific Saved-only GUI fixture from a fresh editor process."),FConsoleCommandWithArgsDelegate::CreateStatic(&LoadSaved));
}
#endif
