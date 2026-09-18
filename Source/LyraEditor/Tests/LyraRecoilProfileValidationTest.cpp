// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"

/**
 * P1 自动验证：Lyra.Recoil.Profile.Validation
 *
 * 计划 §P1 要求：
 *   - 遍历 Content 下所有 ULyraRecoilProfile 资产
 *   - 断言曲线非空、RecoveryTime > 0、PatternLength <= PatternPoints.Num()、
 *     倍率 ∈ (0,5]、上限 > 0
 *   - 一条不合格即 FAIL 并打印资产路径
 *   - 额外：Rifle / Pistol / Shotgun 三份交付资产必须存在（计划 P1 交付物清单）
 *
 * 放在 LyraEditor 模块而非 LyraGame：本测试需要 IAssetRegistry 扫描 Content，
 * 属于编辑器侧的内容校验职责；LyraEditor 已通过 UnrealEd 传递依赖 AssetRegistry
 * （参见 ContentValidationCommandlet.cpp L5-L7 的同款用法），无需改 Build.cs。
 */
namespace LyraRecoilProfileValidation
{
	/** 扫描根路径 */
	static const TCHAR* ProfileSearchPath = TEXT("/Game");

	/** 计划 §P1 交付物清单中的三份必需资产 */
	static const TArray<FString>& GetRequiredProfilePackages()
	{
		static const TArray<FString> Required =
		{
			TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle"),
			TEXT("/Game/Weapons/Recoil/DA_Recoil_Pistol"),
			TEXT("/Game/Weapons/Recoil/DA_Recoil_Shotgun")
		};
		return Required;
	}

	static void GatherProfiles(IAssetRegistry& AssetRegistry, TArray<FAssetData>& OutAssets)
	{
		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.bRecursiveClasses = true;
		Filter.bIncludeOnlyOnDiskAssets = true;
		Filter.ClassPaths.Add(ULyraRecoilProfile::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(FName(ProfileSearchPath));

		AssetRegistry.GetAssets(Filter, OutAssets);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLyraRecoilProfileValidationTest, "Lyra.Recoil.Profile.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLyraRecoilProfileValidationTest::RunTest(const FString& Parameters)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> FoundAssets;
	LyraRecoilProfileValidation::GatherProfiles(AssetRegistry, FoundAssets);

	// 收集已存在的资产包名
	TSet<FString> FoundPackageNames;
	for (const FAssetData& AssetData : FoundAssets)
	{
		FoundPackageNames.Add(AssetData.PackageName.ToString());
	}

	TestTrue(TEXT("At least one ULyraRecoilProfile asset exists under /Game (missing? run -run=LyraRecoilAssetGen first)"),
		FoundAssets.Num() > 0);

	// --- 交付物清单校验 ---
	for (const FString& RequiredPackage : LyraRecoilProfileValidation::GetRequiredProfilePackages())
	{
		TestTrue(
			FString::Printf(TEXT("Required asset exists: %s"), *RequiredPackage),
			FoundPackageNames.Contains(RequiredPackage));
	}

	// --- 逐资产内容校验 ---
	for (const FAssetData& AssetData : FoundAssets)
	{
		const FString AssetPath = AssetData.GetObjectPathString();

		UObject* LoadedObject = AssetData.GetAsset();
		const ULyraRecoilProfile* Profile = Cast<ULyraRecoilProfile>(LoadedObject);
		if (Profile == nullptr)
		{
			AddError(FString::Printf(TEXT("Failed to load asset or wrong type: %s"), *AssetPath));
			continue;
		}

		TArray<FString> Errors;
		const bool bValid = Profile->ValidateProfile(Errors);

		for (const FString& Error : Errors)
		{
			AddError(FString::Printf(TEXT("Profile validation failed %s :: %s"), *AssetPath, *Error));
		}

		TestTrue(FString::Printf(TEXT("资产配置自洽：%s"), *AssetPath), bValid);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
