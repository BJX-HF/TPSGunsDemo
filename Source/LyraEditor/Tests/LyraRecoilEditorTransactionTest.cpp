// Copyright Epic Games, Inc. All Rights Reserved.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Recoil/LyraRecoilEditOperations.h"
#include "Recoil/LyraRecoilPreviewController.h"
#include "Recoil/LyraRecoilProfileEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Curves/CurveFloat.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace RecoilEditorTransactions
{
ULyraRecoilProfile* MakeProfile(UObject* Outer=GetTransientPackage(), FName Name=NAME_None)
{
    auto* P=NewObject<ULyraRecoilProfile>(Outer,Name,RF_Transactional);
    P->PatternPoints={FRecoilPatternPoint(.2f,.3f),FRecoilPatternPoint(-.1f,.5f),FRecoilPatternPoint(.7f,.8f)};
    P->PatternLength=2;
    P->RecoilPerShot_Horizontal=2;
    P->RecoilPerShot_Vertical=3;
    return P;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilEditorAtomicTransactions,"Lyra.Recoil.Editor.Transactions.AtomicUndoAndIdentity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilEditorAtomicTransactions::RunTest(const FString&)
{
    using namespace RecoilEditorTransactions;
    TStrongObjectPtr<ULyraRecoilProfile> P(MakeProfile());
    TStrongObjectPtr<ULyraRecoilEditorSession> Session(NewObject<ULyraRecoilEditorSession>());
    Session->Initialize(P.Get());
    Session->SelectIndices({1},false);
    const auto Original=FLyraRecoilPatternAdapter::Read(*P);
    const auto IDs=Session->GetNodeIds();
    FLyraRecoilEditOperations Ops; Ops.Initialize(P.Get(),Session.Get());
    TArray<FString> Diagnostics;
    const int32 Queue=GEditor->Trans->GetQueueLength();
    TestTrue(TEXT("No-op accepted"),Ops.CommitPattern(Original,FText::FromString(TEXT("No-op")),Diagnostics));
    Session->BeginDragDraft({FVector2D(1,1)}); Session->CancelDragDraft();
    TestEqual(TEXT("No-op and cancelled drafts create no transaction"),GEditor->Trans->GetQueueLength(),Queue);
    const uint64 Revision=Ops.BeginGesture(); Session->BumpRevision();
    TestFalse(TEXT("External revision rejects stale gesture"),Ops.ValidateGestureRevision(Revision,Diagnostics));
    Diagnostics.Reset();
    TestTrue(TEXT("Insert fixed node"),Ops.InsertPoints(1,{FRecoilPatternPoint(0,.2f)},false,Diagnostics));
    TestEqual(TEXT("One insertion transaction"),GEditor->Trans->GetQueueLength(),Queue+1);
    TestEqual(TEXT("Fixed length grows"),P->PatternLength,3);
    TestEqual(TEXT("Selected identity shifts"),Session->GetSelectedIndices()[0],2);
    const FGuid Inserted=Session->GetNodeId(1);
    for (int32 Repeat=0; Repeat<3; ++Repeat)
    {
        GEditor->UndoTransaction();
        TestTrue(TEXT("Undo restores exact raw data"),FLyraRecoilPatternAdapter::Equal(Original,FLyraRecoilPatternAdapter::Read(*P)));
        TestTrue(TEXT("Undo restores original identities"),Session->GetNodeIds()==IDs);
        TestEqual(TEXT("Undo restores selection"),Session->GetSelectedIndices()[0],1);
        GEditor->RedoTransaction();
        TestEqual(TEXT("Redo restores inserted identity"),Session->GetNodeId(1),Inserted);
        TestEqual(TEXT("Redo restores selected identity"),Session->GetNodeId(Session->GetSelectedIndices()[0]),IDs[1]);
    }
    TestTrue(TEXT("Reorder across boundary"),Ops.ReorderPoints({3,2,1,0},Diagnostics));
    TestEqual(TEXT("Reorder preserves selected identity"),Session->GetNodeId(Session->GetSelectedIndices()[0]),IDs[1]);
    const auto BeforeInvalid=FLyraRecoilPatternAdapter::Read(*P);
    const int32 BeforeReject=GEditor->Trans->GetQueueLength();
    TestFalse(TEXT("Out-of-range whole candidate rejected"),Ops.CommitPointNormalized(0,2,.5f,Diagnostics));
    TestEqual(TEXT("Rejected action has no Undo entry"),GEditor->Trans->GetQueueLength(),BeforeReject);
    TestTrue(TEXT("Rejected action writes nothing"),FLyraRecoilPatternAdapter::Equal(BeforeInvalid,FLyraRecoilPatternAdapter::Read(*P)));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilEditorToolkitUndo,"Lyra.Recoil.Editor.Transactions.ToolkitUndoNotifications",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilEditorToolkitUndo::RunTest(const FString&)
{
    TStrongObjectPtr<ULyraRecoilProfile> Profile(RecoilEditorTransactions::MakeProfile());
    TSharedPtr<FLyraRecoilProfileEditor> Toolkit=MakeShared<FLyraRecoilProfileEditor>();
    Toolkit->InitEditor(nullptr,Profile.Get());
    Toolkit->SelectIndices({1},false);
    const auto IDs=Toolkit->GetSession()->GetNodeIds();
    TestTrue(TEXT("Toolkit commits one normalized edit"),Toolkit->SetPointNormalized(1,.25f,.5f));
    const auto Revision=Toolkit->GetRevision();
    GEditor->UndoTransaction();
    TestEqual(TEXT("Toolkit undo restores value"),Profile->PatternPoints[1].X,-.1f);
    TestTrue(TEXT("PostEditUndo must not rebuild node IDs"),Toolkit->GetSession()->GetNodeIds()==IDs);
    TestTrue(TEXT("Undo preserves restored selection"),Toolkit->GetSelectedIndices()==TArray<int32>{1});
    TestTrue(TEXT("Undo revision stays monotonic"),Toolkit->GetRevision()>Revision);
    GEditor->RedoTransaction();
    TestEqual(TEXT("Toolkit redo restores value"),Profile->PatternPoints[1].X,.25f);
    TestTrue(TEXT("Redo preserves node IDs"),Toolkit->GetSession()->GetNodeIds()==IDs);
    TestTrue(TEXT("Redo preserves selection"),Toolkit->GetSelectedIndices()==TArray<int32>{1});
    CollectGarbage(RF_NoFlags);
    TestNotNull(TEXT("Toolkit protects Session during GC"),Toolkit->GetSession());
    Profile->GetOutermost()->SetDirtyFlag(false);
    Toolkit->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
    Toolkit.Reset();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilEditorCurveOwnership,"Lyra.Recoil.Editor.Transactions.SharedCurveOwnershipAndGC",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilEditorCurveOwnership::RunTest(const FString&)
{
    using namespace RecoilEditorTransactions;
    TStrongObjectPtr<ULyraRecoilProfile> A(MakeProfile()), B(MakeProfile());
    TStrongObjectPtr<UCurveFloat> Shared(NewObject<UCurveFloat>(GetTransientPackage(),NAME_None,RF_Transactional));
    Shared->FloatCurve.SetDefaultValue(.75f);
    const auto Handle=Shared->FloatCurve.AddKey(0,1);
    auto& Key=Shared->FloatCurve.GetKey(Handle);
    Key.InterpMode=RCIM_Cubic; Key.TangentMode=RCTM_Break;
    Key.ArriveTangent=.3f; Key.LeaveTangent=.7f;
    Key.TangentWeightMode=RCTWM_WeightedBoth;
    Key.ArriveTangentWeight=.2f; Key.LeaveTangentWeight=.4f;
    Shared->FloatCurve.AddKey(10,1.2f);
    Shared->FloatCurve.PreInfinityExtrap=RCCE_Linear;
    Shared->FloatCurve.PostInfinityExtrap=RCCE_Cycle;
    A->VerticalKickCurve.ExternalCurve=Shared.Get(); B->VerticalKickCurve.ExternalCurve=Shared.Get();
    const FRichCurve Original=Shared->FloatCurve;
    FLyraRecoilPreviewController Preview;
    FString Error; TArray<FName> Inlined;
    TestTrue(TEXT("Shared curves deep-copied into preview"),Preview.RefreshProfile(A.Get(),Error,Inlined));
    FLyraRecoilEditOperations Ops; Ops.Initialize(A.Get(),nullptr);
    TArray<FString> Errors;
    TestTrue(TEXT("Copy shared curve inline"),Ops.CopyCurveToInline(GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile,VerticalKickCurve),Errors));
    TestNull(TEXT("Copied Profile owns inline curve"),A->VerticalKickCurve.ExternalCurve.Get());
    TestTrue(TEXT("Full RichCurve equals original"),A->VerticalKickCurve.EditorCurveData==Original);
    TestTrue(TEXT("Shared curve remains identical"),Shared->FloatCurve==Original);
    TestEqual(TEXT("Other Profile retains reference"),B->VerticalKickCurve.ExternalCurve.Get(),Shared.Get());
    GEditor->UndoTransaction();
    TestEqual(TEXT("Undo restores shared ownership"),A->VerticalKickCurve.ExternalCurve.Get(),Shared.Get());
    GEditor->RedoTransaction();
    Shared->FloatCurve.GetKey(Handle).Value=2;
    TestTrue(TEXT("Snapshot stays isolated after external edit"),Preview.GetSnapshotProfile()->VerticalKickCurve.EditorCurveData==Original);
    CollectGarbage(RF_NoFlags);
    TestNotNull(TEXT("Snapshot survives forced GC"),Preview.GetSnapshotProfile());
    TestTrue(TEXT("Preview still runs"),Preview.Run());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilEditorPersistence,"Lyra.Recoil.Editor.Persistence.SaveUnloadReloadAndFailure",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FRecoilEditorPersistence::RunTest(const FString&)
{
    using namespace RecoilEditorTransactions;
    const FString Tag=FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("RecoilGUI/TestPackages")/Tag);
    IFileManager::Get().MakeDirectory(*Folder,true);
    const FString Mount=TEXT("/RecoilGUIQA/");
    FPackageName::RegisterMountPoint(Mount,Folder+TEXT("/"));
    const FString PackageName=Mount+TEXT("Profile_")+Tag;
    const FString Filename=Folder/TEXT("Profile_")+Tag+TEXT(".uasset");
    UPackage* Package=CreatePackage(*PackageName);
    ULyraRecoilProfile* Profile=MakeProfile(Package,TEXT("Profile"));
    Profile->SetFlags(RF_Public|RF_Standalone);
    FLyraRecoilEditOperations Ops; Ops.Initialize(Profile,nullptr);
    TArray<FString> Errors;
    TestTrue(TEXT("Edit normalized fields"),Ops.CommitPointNormalized(1,-.234567f,.654321f,Errors));
    Profile->VerticalKickCurve.EditorCurveData.SetDefaultValue(.8f);
    const auto Expected=FLyraRecoilPatternAdapter::Read(*Profile);
    const FRichCurve ExpectedCurve=Profile->VerticalKickCurve.EditorCurveData;
    FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone; Args.SaveFlags=SAVE_NoError;
    TestTrue(TEXT("Save actual temporary package"),UPackage::SavePackage(Package,Profile,*Filename,Args));
    TestFalse(TEXT("Successful save clears Dirty"),Package->IsDirty());
    const float OriginalVertical=Profile->RecoilPerShot_Vertical;
    // A separate save-as package retains the same runtime data without touching original disk contents.
    UPackage* CopyPackage=CreatePackage(*(Mount+TEXT("Copy_")+Tag));
    auto* Copy=DuplicateObject<ULyraRecoilProfile>(Profile,CopyPackage,TEXT("Profile"));
    TStrongObjectPtr<UPackage> CopyPackageGuard(CopyPackage);
    TStrongObjectPtr<ULyraRecoilProfile> CopyGuard(Copy);
    Copy->ClearFlags(RF_Transient); Copy->SetFlags(RF_Public|RF_Standalone);
    Copy->RecoilPerShot_Vertical=OriginalVertical+1;
    const FString CopyFilename=Folder/(TEXT("Copy_")+Tag+TEXT(".uasset"));
    TestTrue(TEXT("Save-as package"),UPackage::SavePackage(CopyPackage,Copy,*CopyFilename,Args));
    TestEqual(TEXT("Save-as leaves source values"),Profile->RecoilPerShot_Vertical,OriginalVertical);
    Package->MarkPackageDirty();
    const FString Blocked=Folder/TEXT("blocked-parent");
    FFileHelper::SaveStringToFile(TEXT("test-owned file prevents creating a directory"),*Blocked);
    const FString Impossible=Blocked/TEXT("Profile.uasset");
    AddExpectedError(TEXT("LogFileManager: Error moving file"),EAutomationExpectedErrorFlags::Contains,1);
    TestFalse(TEXT("Save failure returned"),UPackage::SavePackage(Package,Profile,*Impossible,Args));
    TestTrue(TEXT("Failed save retains Dirty"),Package->IsDirty());
    Package->SetDirtyFlag(false);
    Ops.Initialize(nullptr,nullptr);
    GEditor->Trans->Reset(FText::FromString(TEXT("Release isolated recoil test package")));
    TWeakObjectPtr<ULyraRecoilProfile> Previous(Profile);
    Profile=nullptr;
    TestTrue(TEXT("Unload actual saved package"),UPackageTools::UnloadPackages({Package}));
    Package=nullptr;
    CollectGarbage(RF_NoFlags);
    TestFalse(TEXT("Old Profile released"),Previous.IsValid());
    UPackage* Reloaded=LoadPackage(nullptr,*PackageName,LOAD_None);
    auto* Loaded=Reloaded?FindObject<ULyraRecoilProfile>(Reloaded,TEXT("Profile")):nullptr;
    if (TestNotNull(TEXT("Reloaded from disk"),Loaded))
    {
        TestTrue(TEXT("Raw array and L preserved exactly"),FLyraRecoilPatternAdapter::Equal(Expected,FLyraRecoilPatternAdapter::Read(*Loaded)));
        TestTrue(TEXT("Inline RichCurve preserved"),Loaded->VerticalKickCurve.EditorCurveData==ExpectedCurve);
        TestEqual(TEXT("Original disk source preserved"),Loaded->RecoilPerShot_Vertical,OriginalVertical);
        FLyraRecoilPreviewController Preview;
        FString Error; TArray<FName> Inlined;
        TestTrue(TEXT("Reloaded Profile snapshot"),Preview.RefreshProfile(Loaded,Error,Inlined));
        TestTrue(TEXT("Reloaded Profile runtime replay"),Preview.Run());
    }
    if (Reloaded) UPackageTools::UnloadPackages({Reloaded});
    CopyGuard.Reset();
    CopyPackageGuard.Reset();
    UPackageTools::UnloadPackages({CopyPackage});
    FPackageName::UnRegisterMountPoint(Mount,Folder+TEXT("/"));
    // Delete only the two files created by this test, using verified absolute paths.
    TestTrue(TEXT("Test path lies under Saved"),Folder.StartsWith(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())));
    IFileManager::Get().Delete(*Filename); IFileManager::Get().Delete(*CopyFilename); IFileManager::Get().Delete(*Blocked);
    return true;
}
#endif
