// Copyright Epic Games, Inc. All Rights Reserved.
// Opt-in acceptance driver: a real equipped PIE weapon ticks normally. Never included in LyraGame.
#include "CoreMinimal.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Editor.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "AbilitySystem/LyraAbilitySystemComponent.h"
#include "UObject/StructOnScope.h"
#include "Weapons/LyraRangedWeaponInstance.h"
#include "Weapons/LyraGameplayAbility_RangedWeapon.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
namespace RecoilGUIFixture { ULyraRecoilProfile* GetProfile(); }

namespace RecoilPIEQA
{
static TWeakObjectPtr<ULyraRangedWeaponInstance> Weapon;
static TWeakObjectPtr<ULyraRecoilProfile> Original;
static TStrongObjectPtr<ULyraRecoilProfile> TestProfile;
static TStrongObjectPtr<ULyraRecoilProfile> SavedGUIProfile;
static FRotator OriginalAim;
static FString Report, ReportFile;
static FTSTicker::FDelegateHandle TickHandle;
static int32 Case=0, Round=0, Fired=0, Failed=0;
static double Started=0;
static double LastFireElapsed=0;
static bool bReleasePending=false;
static bool bPulled=false;
static bool bCheckingGUI=false;
static TWeakObjectPtr<ULyraAbilitySystemComponent> AbilitySystem;
static FGameplayAbilitySpecHandle FireHandle;
static int32 OriginalAmmo=0;
static int32 AmmoOperation(FName Function, int32 Count=0);
static void Flush();

// 0: the prior normal ability is still finishing; 1: fired; -1: activation failed.
static int32 FireOnce(double Elapsed)
{
    auto* Spec=AbilitySystem->FindAbilitySpecFromHandle(FireHandle);
    const bool bWasActive=Spec && Spec->IsActive();
    if (bWasActive) return 0;
    if (Spec) Spec->InputPressed=true;
    const bool bActivated=Spec && AbilitySystem->TryActivateAbility(FireHandle);
    // Release on the next ticker frame, after the blueprint has registered its input task.
    bReleasePending=bActivated;
    if (!bActivated)
    {
        FGameplayTagContainer Tags; AbilitySystem->GetOwnedGameplayTags(Tags);
        Report+=FString::Printf(TEXT("FAIL fire fired=%d elapsed=%.3f ability=%s wasActive=%d ammo=%d ownerTags=%s\n"),
            Fired,Elapsed,Spec?*GetNameSafe(Spec->Ability):TEXT("missing"),bWasActive,AmmoOperation(TEXT("GetStatTagStackCount")),*Tags.ToStringSimple());
        Flush();
    }
    if (bActivated) LastFireElapsed=Elapsed;
    return bActivated?1:-1;
}

static APawn* Pawn() { return Weapon.IsValid()?Cast<APawn>(Weapon->GetOuter()):nullptr; }
static int32 AmmoOperation(FName Function, int32 Count)
{
    UObject* Item=Weapon->GetInstigator();
    if (!Item) return 0;
    UFunction* Fn=Item->FindFunction(Function); check(Fn);
    FStructOnScope Params(Fn);
    const FGameplayTag Tag=FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
    auto* TagProperty=FindFProperty<FStructProperty>(Fn,TEXT("Tag"));
    TagProperty->CopyCompleteValue(TagProperty->ContainerPtrToValuePtr<void>(Params.GetStructMemory()),&Tag);
    if (auto* Int=FindFProperty<FIntProperty>(Fn,TEXT("StackCount"))) Int->SetPropertyValue_InContainer(Params.GetStructMemory(),Count);
    Item->ProcessEvent(Fn,Params.GetStructMemory());
    if (auto* Int=FindFProperty<FIntProperty>(Fn,TEXT("ReturnValue"))) return Int->GetPropertyValue_InContainer(Params.GetStructMemory());
    return 0;
}

static void Flush()
{
    FFileHelper::SaveStringToFile(Report,*ReportFile);
}
static void Bind(ULyraRecoilProfile* P)
{
    auto* Property=FindFProperty<FObjectProperty>(ULyraRangedWeaponInstance::StaticClass(),TEXT("RecoilProfile"));
    check(Property);
    Property->SetObjectPropertyValue_InContainer(Weapon.Get(),P);
}
static APlayerController* Controller()
{
    return Pawn()?Cast<APlayerController>(Pawn()->GetController()):nullptr;
}
static float Pull() { static const float Values[]={0,4,10,11}; return Values[Case%4]; }
static float StartPitch() { return ((Case/4)%2)*30.f; }
static void BeginCase()
{
    TestProfile->SingleShotMode=(Case/8)==0?ERecoilSingleShotMode::InstantWrite:ERecoilSingleShotMode::Interpolated;
    // Reset only at the start of a distinct test case. Both bursts below share the real weapon state.
    const_cast<FRecoilRuntimeState&>(Weapon->GetRecoilState()).Reset(TestProfile.Get());
    Controller()->SetControlRotation(FRotator(StartPitch(),0,0));
    AmmoOperation(TEXT("AddStatTagStack"),25);
    Round=0; Fired=0; bPulled=false; bReleasePending=false; LastFireElapsed=0; Started=FPlatformTime::Seconds();
}
static bool Setup()
{
    for (TObjectIterator<ULyraRangedWeaponInstance> It; It; ++It)
    {
        APawn* Owner=Cast<APawn>(It->GetOuter());
        APlayerController* Player=Owner?Cast<APlayerController>(Owner->GetController()):nullptr;
        if (Owner && Owner->GetWorld()==GEditor->PlayWorld && Player && Player->IsLocalController() && It->GetRecoilProfile())
        { Weapon=*It; break; }
    }
    if (!Weapon.IsValid() || !Controller()) { Weapon.Reset(); return false; }
    AbilitySystem=Controller()->PlayerState?Controller()->PlayerState->FindComponentByClass<ULyraAbilitySystemComponent>():nullptr;
    if (!AbilitySystem.IsValid()) { Weapon.Reset(); return false; }
    FireHandle=FGameplayAbilitySpecHandle();
    for (auto& Spec:AbilitySystem->GetActivatableAbilities())
        if (Spec.SourceObject.Get()==Weapon.Get() && Spec.Ability && Spec.Ability->IsA<ULyraGameplayAbility_RangedWeapon>())
        { FireHandle=Spec.Handle; break; }
    if (!FireHandle.IsValid())
    {
        for (auto& Spec:AbilitySystem->GetActivatableAbilities())
            UE_LOG(LogTemp,Display,TEXT("RecoilPIEQA pending ability=%s source=%s tags=%s weapon=%s"),
                *GetNameSafe(Spec.Ability),*GetNameSafe(Spec.SourceObject.Get()),*Spec.GetDynamicSpecSourceTags().ToStringSimple(),*Weapon->GetName());
        Weapon.Reset(); return false;
    }
    OriginalAmmo=AmmoOperation(TEXT("GetStatTagStackCount"));
    AbilitySystem->AddLooseGameplayTag(FGameplayTag::RequestGameplayTag(TEXT("Cheat.GodMode")));
    Original=Weapon->GetRecoilProfile(); OriginalAim=Controller()->GetControlRotation();
    SavedGUIProfile.Reset(RecoilGUIFixture::GetProfile());
    const FString Folder=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("RecoilGUI/PIE"));
    IFileManager::Get().MakeDirectory(*Folder,true);
    FPackageName::RegisterMountPoint(TEXT("/RecoilPIEQA/"),Folder+TEXT("/"));
    const FString Name=TEXT("Matrix_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    auto* Package=CreatePackage(*(TEXT("/RecoilPIEQA/")+Name));
    auto* P=DuplicateObject<ULyraRecoilProfile>(Original.Get(),Package,*Name);
    P->ClearFlags(RF_Transient); P->SetFlags(RF_Public|RF_Standalone);
    P->PatternPoints.Init(FRecoilPatternPoint(0,1),10); P->PatternLength=10;
    P->RecoilPerShot_Horizontal=0; P->RecoilPerShot_Vertical=1;
    P->VerticalKickCurve.ExternalCurve=nullptr; P->VerticalKickCurve.EditorCurveData.Reset();
    P->VerticalKickCurve.EditorCurveData.SetDefaultValue(1);
    P->MaxVerticalKick=50; P->RecoveryDelay=.3f; P->RecoveryTime=.4f;
    P->LiftDuration=.03f; P->ReboundDuration=.02f; P->ReboundRatio=1;
    P->bCompensationAwareRecovery=true;
    P->PoseMultiplier_Standing=1; P->PoseMultiplier_Crouching=1;
    P->PoseMultiplier_JumpingOrFalling=1; P->PoseMultiplier_Aiming=1;
    FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone;
    if (!UPackage::SavePackage(Package,P,*(Folder/(Name+TEXT(".uasset"))),Args))
    { UE_LOG(LogTemp,Error,TEXT("RecoilPIEQA fixture SavePackage failed.")); return false; }
    TWeakObjectPtr<ULyraRecoilProfile> Old=P;
    P=nullptr;
    if (!UPackageTools::UnloadPackages({Package}))
    { UE_LOG(LogTemp,Error,TEXT("RecoilPIEQA fixture UnloadPackages failed.")); return false; }
    CollectGarbage(RF_NoFlags);
    if (Old.IsValid()) { UE_LOG(LogTemp,Error,TEXT("RecoilPIEQA fixture survived unload/GC.")); return false; }
    TestProfile.Reset(LoadObject<ULyraRecoilProfile>(nullptr,*(TEXT("/RecoilPIEQA/")+Name+TEXT(".")+Name)));
    if (!TestProfile.IsValid()) { UE_LOG(LogTemp,Error,TEXT("RecoilPIEQA fixture disk reload failed.")); return false; }
    Bind(TestProfile.Get());
    Report=FString::Printf(TEXT("Real PIE world=%s weapon=%s original=%s\nSaved/unloaded/reloaded fixture=%s\nOnly scenario fixture fields differ; Roll/Spread/WeaponVisual copied unchanged.\n"),
        *GEditor->PlayWorld->GetPathName(),*Weapon->GetPathName(),*Original->GetPathName(),*TestProfile->GetPathName());
    ReportFile=Folder/TEXT("MatrixReport.txt");
    Case=0; Failed=0; BeginCase(); Flush();
    if (SavedGUIProfile.IsValid())
    {
        bCheckingGUI=true; Bind(SavedGUIProfile.Get());
        const_cast<FRecoilRuntimeState&>(Weapon->GetRecoilState()).Reset(SavedGUIProfile.Get());
        Controller()->SetControlRotation(FRotator(0,0,0)); Fired=0; Started=FPlatformTime::Seconds();
    }
    // Let the existing equip/initialization abilities finish before sending fire input.
    Started=FPlatformTime::Seconds()+1.0;
    return true;
}
static bool Tick(float)
{
    if (!GEditor || !GEditor->PlayWorld) return true; // Armed until the user starts normal PIE.
    if (!Weapon.IsValid())
    {
        if (!Setup() && Weapon.IsValid())
        { UE_LOG(LogTemp,Error,TEXT("RecoilPIEQA saved fixture setup failed.")); return false; }
        return true;
    }
    if (!Controller()) return false;
    const double Elapsed=FPlatformTime::Seconds()-Started;
    if (bReleasePending)
    {
        if (auto* Spec=AbilitySystem->FindAbilitySpecFromHandle(FireHandle))
        {
            Spec->InputPressed=false;
            static_cast<UAbilitySystemComponent*>(AbilitySystem.Get())->AbilitySpecInputReleased(*Spec);
        }
        bReleasePending=false;
    }
    if (Elapsed>12.0)
    { Report+=TEXT("FAIL firing driver timed out waiting for normal ability completion\n"); Flush(); return false; }
    if (bCheckingGUI)
    {
        if (Fired<8 && Elapsed>=(Fired?LastFireElapsed+.12:0))
        {
            const int32 Result=FireOnce(Elapsed);
            if (Result<0)
            { Report+=TEXT("FAIL saved GUI profile fire ability\n"); Flush(); return false; }
            if (Result>0) ++Fired;
        }
        if (Fired<8 || Elapsed<LastFireElapsed+.8) return true;
        const auto& History=Weapon->GetRecoilState().ShotHistory;
        bool bPass=History.Num()==8;
        for (const auto& Shot:History)
        {
            const auto Kick=FRecoilRuntimeState::ComputeShotKick(*SavedGUIProfile,Shot.ShotIndex,Shot.PoseMultiplier,Weapon->GetRecoilState().GlobalScale,Weapon->GetRecoilState().ActiveSeed);
            bPass &= FMath::IsNearlyEqual(Kick.Horizontal,Shot.HorizontalKick,1.e-5f) && FMath::IsNearlyEqual(Kick.Vertical,Shot.VerticalKick,1.e-5f);
        }
        if (!bPass) ++Failed;
        Report+=FString::Printf(TEXT("%s GUI-saved disk Profile=%s loaded by actual weapon, shots=%d editedRawX[7]=%.9g lastActualYaw=%.9g\n"),
            bPass?TEXT("PASS"):TEXT("FAIL"),*SavedGUIProfile->GetPathName(),History.Num(),
            SavedGUIProfile->PatternPoints.IsValidIndex(7)?SavedGUIProfile->PatternPoints[7].X:0,History.IsEmpty()?0:History.Last().HorizontalKick);
        Flush(); bCheckingGUI=false; Bind(TestProfile.Get()); SavedGUIProfile.Reset(); BeginCase(); return true;
    }
    if (Fired<10 && Elapsed>=(Fired?LastFireElapsed+.15:0))
    {
        // Activate the existing firing ability. It owns trace, ammo cost, AddSpread and AddRecoil.
        // No LyraGame export or algorithm changes are needed for this Editor-only driver.
        const int32 Result=FireOnce(Elapsed);
        if (Result<0)
        { Report+=TEXT("FAIL real firing ability activation\n"); Flush(); return false; }
        if (Result>0) ++Fired;
        if (Weapon->GetRecoilState().ShotHistory.Num()!=Fired)
        { Report+=TEXT("FAIL real firing ability did not commit exactly one shot\n"); Flush(); return false; }
    }
    if (!bPulled && Fired==10 && Elapsed>=LastFireElapsed+.15)
    {
        const float Peak=Weapon->GetRecoilState().CameraOffsetPitch;
        FRotator Aim=Controller()->GetControlRotation();
        const float PeakVisible=FRotator::NormalizeAxis(Aim.Pitch)+Peak;
        // Apply a relative player pull. InstantWrite already moved ControlRotation during firing.
        Aim.Pitch-=Pull(); Controller()->SetControlRotation(Aim);
        Report+=FString::Printf(TEXT("case=%d round=%d peakCamera=%.6f peakVisible=%.6f pulled=%.1f\n"),Case,Round,Peak,PeakVisible,Pull());
        bPulled=true;
    }
    if (Fired<10 || Elapsed<LastFireElapsed+1.10) return true;
    const auto& State=Weapon->GetRecoilState();
    const float Visible=FRotator::NormalizeAxis(Controller()->GetControlRotation().Pitch)+State.CameraOffsetPitch;
    const float Expected=StartPitch()-(Round+1)*FMath::Max(0.f,Pull()-10.f);
    const bool bPass=FMath::IsNearlyEqual(Visible,Expected,.05f) && State.State==ERecoilState::Idle;
    if (!bPass) ++Failed;
    Report+=FString::Printf(TEXT("%s mode=%s start=%.0f pull=%.0f round=%d visible=%.6f expected=%.6f camera=%.6f state=%d shots=%d visualSerial=%d\n"),
        bPass?TEXT("PASS"):TEXT("FAIL"),Case<8?TEXT("InstantWrite"):TEXT("Interpolated"),StartPitch(),Pull(),Round+1,
        Visible,Expected,State.CameraOffsetPitch,int32(State.State),Fired,Weapon->GetWeaponVisualRecoilPose().ShotSerial);
    Flush();
    if (++Round<2) { AmmoOperation(TEXT("AddStatTagStack"),25); Fired=0; bPulled=false; LastFireElapsed=0; Started=FPlatformTime::Seconds(); return true; }
    if (++Case<16) { BeginCase(); return true; }
    Report+=FString::Printf(TEXT("COMPLETE cases=16 bursts=32 failed=%d\n"),Failed); Flush();
    UE_LOG(LogTemp,Display,TEXT("RecoilPIEQA complete: 32 bursts, failed=%d, report=%s"),Failed,*ReportFile);
    Bind(Original.Get()); const_cast<FRecoilRuntimeState&>(Weapon->GetRecoilState()).Reset(Original.Get());
    AmmoOperation(TEXT("RemoveStatTagStack"),AmmoOperation(TEXT("GetStatTagStackCount")));
    AmmoOperation(TEXT("AddStatTagStack"),OriginalAmmo);
    AbilitySystem->RemoveLooseGameplayTag(FGameplayTag::RequestGameplayTag(TEXT("Cheat.GodMode")));
    Controller()->SetControlRotation(OriginalAim); TestProfile.Reset();
    return false;
}
static void Arm()
{
    if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    Weapon.Reset(); TestProfile.Reset();
    SavedGUIProfile.Reset(); bCheckingGUI=false;
    TickHandle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
    UE_LOG(LogTemp,Display,TEXT("RecoilPIEQA armed; start normal PIE. Saved fixtures only; 16 cases / 32 bursts."));
}
static FAutoConsoleCommand ArmCommand(TEXT("Lyra.Recoil.GUIQA.PIEArm"),TEXT("Arm opt-in real equipped PIE weapon acceptance matrix."),FConsoleCommandDelegate::CreateStatic(&Arm));
}
#endif
