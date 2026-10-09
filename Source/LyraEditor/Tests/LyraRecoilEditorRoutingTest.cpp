// Copyright Epic Games, Inc. All Rights Reserved.
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "AssetDefinition_LyraRecoilProfile.h"
#include "AssetDefinitionRegistry.h"
#include "Recoil/LyraRecoilEditorRouting.h"
#include "Recoil/LyraRecoilProfileEditor.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/SimpleAssetEditor.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRecoilEditorDefaultRouting,
    "Lyra.Recoil.Editor.Routing.DefaultGUIAndRawDetailsRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRecoilEditorDefaultRouting::RunTest(const FString&)
{
    const auto* Definition = UAssetDefinitionRegistry::Get()->GetAssetDefinitionForClass(ULyraRecoilProfile::StaticClass());
    if (!TestTrue(TEXT("Recoil class resolves to native GUI AssetDefinition"),
        Definition && Definition->IsA<UAssetDefinition_LyraRecoilProfile>())) return false;

    TStrongObjectPtr<ULyraRecoilProfile> Profile(NewObject<ULyraRecoilProfile>(GetTransientPackage(), NAME_None, RF_Transactional));
    Profile->PatternPoints = { FRecoilPatternPoint(.2f, .5f), FRecoilPatternPoint(-.3f, .7f) };
    Profile->PatternLength = 2;
    const auto OriginalPoints = Profile->PatternPoints;
    const bool bOriginalDirty = Profile->GetOutermost()->IsDirty();
    auto* Assets = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    TestTrue(TEXT("Ordinary engine open succeeds"), Assets->OpenEditorForAsset(Profile.Get()));
    auto GUI = FLyraRecoilProfileEditor::FindEditorForProfile(Profile.Get());
    const bool bGUIOpened = TestTrue(TEXT("Ordinary engine open creates recoil GUI"), GUI.IsValid());
    if (bGUIOpened)
    {
        auto Raw = LyraRecoilEditorRouting::OpenRawDetails(Profile.Get());
        TestTrue(TEXT("Raw Details editor opens for the same profile"), Raw.IsValid() && Assets->FindEditorsForAsset(Profile.Get()).Contains(Raw.Get()));
        TestTrue(TEXT("Repeated raw open reuses its editor"), LyraRecoilEditorRouting::OpenRawDetails(Profile.Get()) == Raw);
        TestTrue(TEXT("Returning from raw Details reuses the GUI"), LyraRecoilEditorRouting::OpenGUI(Profile.Get()) == GUI);
        TestEqual(TEXT("Switching pages preserves selection revision"), GUI->GetRevision(), uint64(1));
        Raw.Reset();
    }
    TestEqual(TEXT("Switching pages does not dirty the asset"), Profile->GetOutermost()->IsDirty(), bOriginalDirty);
    TestEqual(TEXT("Switching preserves length"), Profile->PatternLength, 2);
    for (int32 I = 0; I < OriginalPoints.Num(); ++I)
    {
        TestEqual(TEXT("Switching preserves raw X"), Profile->PatternPoints[I].X, OriginalPoints[I].X);
        TestEqual(TEXT("Switching preserves raw Y"), Profile->PatternPoints[I].Y, OriginalPoints[I].Y);
    }
    Assets->CloseAllEditorsForAsset(Profile.Get());
    GUI.Reset();
    return true;
}
#endif
