// Copyright Epic Games, Inc. All Rights Reserved.

#include "LyraRecoilAssetGenCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LyraRecoilAssetGenCommandlet)

DEFINE_LOG_CATEGORY_STATIC(LogLyraRecoilAssetGen, Log, All);

namespace LyraRecoilAssetGen
{
	/** 一份资产的生成规格 */
	struct FProfileSpec
	{
		FString PackageName;
		FString AssetName;

		// 基础
		float RecoilPerShot_Vertical = 0.35f;
		float RecoilPerShot_Horizontal = 0.18f;

		// 恢复
		float RecoveryDelay = 0.15f;
		float RecoveryTime = 0.35f;
		float RecoilReturnRatio = 0.2f;

		// 上限
		float MaxVerticalKick = 8.0f;
		float MaxHorizontalKick = 4.0f;

		// Pattern
		int32 PatternLength = 8;
		float HorizontalRandomRange = 0.6f;

		// 倍率
		float PoseMultiplier_Aiming = 0.75f;
		float PoseMultiplier_Standing = 1.0f;
		float PoseMultiplier_Crouching = 0.8f;
		float PoseMultiplier_JumpingOrFalling = 1.5f;

		// ---- 单发后坐力模型（见 Docs/Recoil/10_SingleShotInterpolation.md）----
		// 默认 InstantWrite：与既有 24 个测试、3 份 Golden 基线完全一致，零回归风险。
		ERecoilSingleShotMode SingleShotMode = ERecoilSingleShotMode::InstantWrite;

		// 仅在 Interpolated 模式下生效
		float LiftDuration = 0.045f;    // t0 上抬段时长
		float ReboundDuration = 0.030f; // t1 回弹段时长
		float ReboundRatio = 0.72f;     // 回弹落点 = 峰值 * 本值（<1 才会下降）
		// Settle / Drop 两段复用 RecoveryDelay / RecoveryTime，不另设参数

		// 上抬曲线关键帧 (X, Y) 对：X = 归一化进度 [0,1]，Y = 上抬完成度 [0,1]
		// 默认 Ease-Out：起始快、后段收敛 —— 冲量驱动物理直觉，且低帧率下形状最稳
		TArray<TPair<float, float>> LiftCurveKeys = {
			{0.00f, 0.00f}, {0.15f, 0.35f}, {0.35f, 0.62f}, {0.60f, 0.83f}, {1.00f, 1.00f}
		};
		// 回弹曲线关键帧：默认线性（视觉上就是一段匀速回落）
		TArray<TPair<float, float>> ReboundCurveKeys = { {0.0f, 0.0f}, {1.0f, 1.0f} };

		// 垂直 Kick 曲线关键帧 (X, Y) 对
		TArray<TPair<float, float>> VerticalKickKeys;
	};

	/** 用关键帧数组覆盖一条曲线资产 */
	static void WriteCurve(FRuntimeFloatCurve& Curve, const TArray<TPair<float, float>>& Keys)
	{
		FRichCurve& RichCurve = Curve.EditorCurveData;
		RichCurve.Reset();
		for (const TPair<float, float>& Key : Keys)
		{
			RichCurve.AddKey(Key.Key, Key.Value);
		}
	}

	static FProfileSpec MakeRifleSpec()
	{
		FProfileSpec Spec;
		Spec.PackageName = TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle");
		Spec.AssetName = TEXT("DA_Recoil_Rifle");

		// 长按连发向：中等单发、明显累加、渐进增强
		Spec.RecoilPerShot_Vertical = 0.35f;
		Spec.RecoilPerShot_Horizontal = 0.18f;
		Spec.RecoveryDelay = 0.15f;
		Spec.RecoveryTime = 0.35f;
		Spec.RecoilReturnRatio = 0.20f;
		Spec.MaxVerticalKick = 8.0f;
		Spec.MaxHorizontalKick = 4.0f;
		Spec.PatternLength = 8;
		Spec.HorizontalRandomRange = 0.60f;
		Spec.VerticalKickKeys = { {0.0f, 0.70f}, {4.0f, 1.00f}, {12.0f, 1.35f} };

		return Spec;
	}

	static FProfileSpec MakePistolSpec()
	{
		FProfileSpec Spec;
		Spec.PackageName = TEXT("/Game/Weapons/Recoil/DA_Recoil_Pistol");
		Spec.AssetName = TEXT("DA_Recoil_Pistol");

		// 点射向：单发小、回正快、上限低、Pattern 短
		Spec.RecoilPerShot_Vertical = 0.42f;
		Spec.RecoilPerShot_Horizontal = 0.24f;
		Spec.RecoveryDelay = 0.08f;
		Spec.RecoveryTime = 0.22f;
		Spec.RecoilReturnRatio = 0.10f;
		Spec.MaxVerticalKick = 4.0f;
		Spec.MaxHorizontalKick = 2.5f;
		Spec.PatternLength = 5;
		Spec.HorizontalRandomRange = 0.45f;
		Spec.PoseMultiplier_Crouching = 0.85f;
		Spec.PoseMultiplier_JumpingOrFalling = 1.35f;
		Spec.VerticalKickKeys = { {0.0f, 0.85f}, {3.0f, 1.05f}, {8.0f, 1.20f} };

		return Spec;
	}

	static FProfileSpec MakeShotgunSpec()
	{
		FProfileSpec Spec;
		Spec.PackageName = TEXT("/Game/Weapons/Recoil/DA_Recoil_Shotgun");
		Spec.AssetName = TEXT("DA_Recoil_Shotgun");

		// 单发大推力向：单发极重、回正慢、上限高、Pattern 极短
		Spec.RecoilPerShot_Vertical = 1.60f;
		Spec.RecoilPerShot_Horizontal = 0.55f;
		Spec.RecoveryDelay = 0.30f;
		Spec.RecoveryTime = 0.55f;
		Spec.RecoilReturnRatio = 0.35f;
		Spec.MaxVerticalKick = 11.0f;
		Spec.MaxHorizontalKick = 5.0f;
		Spec.PatternLength = 3;
		Spec.HorizontalRandomRange = 0.80f;
		Spec.PoseMultiplier_Aiming = 0.85f;
		Spec.PoseMultiplier_Crouching = 0.70f;
		Spec.PoseMultiplier_JumpingOrFalling = 1.80f;
		Spec.VerticalKickKeys = { {0.0f, 1.00f}, {2.0f, 1.15f}, {5.0f, 1.25f} };

		return Spec;
	}

	/**
	 * Rifle_S：默认携带枪 A —— 走【插值】单发模型。
	 *
	 * 定位：连发手感向。上抬过程被显式三段化（Lift -> Rebound -> Settle -> Drop），
	 * 让玩家能"看见"枪口抬起再压回来，而不是瞬间跳到峰值。
	 * 曲线用默认 Ease-Out（LiftCurve 构造函数已有 5 帧），此处不覆盖。
	 */
	static FProfileSpec MakeRifleSSpec()
	{
		FProfileSpec Spec;
		Spec.PackageName = TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle_S");
		Spec.AssetName = TEXT("DA_Recoil_Rifle_S");

		Spec.RecoilPerShot_Vertical = 0.32f;
		Spec.RecoilPerShot_Horizontal = 0.17f;
		Spec.RecoveryDelay = 0.12f;   // 插值模式下 = Settle 段时长
		Spec.RecoveryTime = 0.32f;    // 插值模式下 = Drop 段时长
		Spec.RecoilReturnRatio = 0.22f;
		Spec.MaxVerticalKick = 7.5f;
		Spec.MaxHorizontalKick = 3.8f;
		Spec.PatternLength = 8;
		Spec.HorizontalRandomRange = 0.55f;

		// ★ 关键差异：走插值
		Spec.SingleShotMode = ERecoilSingleShotMode::Interpolated;
		Spec.LiftDuration = 0.045f;
		Spec.ReboundDuration = 0.030f;
		Spec.ReboundRatio = 0.72f;
		Spec.LiftCurveKeys = { {0.00f, 0.00f}, {0.15f, 0.35f}, {0.35f, 0.62f}, {0.60f, 0.83f}, {1.00f, 1.00f} };
		Spec.ReboundCurveKeys = { {0.0f, 0.0f}, {1.0f, 1.0f} };

		Spec.VerticalKickKeys = { {0.0f, 0.72f}, {4.0f, 1.00f}, {12.0f, 1.30f} };

		return Spec;
	}

	/**
	 * Rifle_7：默认携带枪 B —— 走【瞬间写入】单发模型。
	 *
	 * 定位：干脆利落向。和后坐力参考文档 §2 的原始模型逐位一致：
	 * 开火瞬间直接把 Kick 写进偏移，然后走统一的 RecoveryDelay/Time 回正。
	 * 与 Rifle_S 同场对比，策划能在同一把武器配置表里直接看出两套模型的差别。
	 */
	static FProfileSpec MakeRifle7Spec()
	{
		FProfileSpec Spec;
		Spec.PackageName = TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle_7");
		Spec.AssetName = TEXT("DA_Recoil_Rifle_7");

		Spec.RecoilPerShot_Vertical = 0.38f;
		Spec.RecoilPerShot_Horizontal = 0.20f;
		Spec.RecoveryDelay = 0.14f;
		Spec.RecoveryTime = 0.30f;
		Spec.RecoilReturnRatio = 0.18f;
		Spec.MaxVerticalKick = 8.0f;
		Spec.MaxHorizontalKick = 4.0f;
		Spec.PatternLength = 8;
		Spec.HorizontalRandomRange = 0.62f;

		// ★ 显式写死 InstantWrite（也就是构造默认值，这里写明意图）
		Spec.SingleShotMode = ERecoilSingleShotMode::InstantWrite;

		Spec.VerticalKickKeys = { {0.0f, 0.75f}, {4.0f, 1.02f}, {12.0f, 1.32f} };

		return Spec;
	}

	/**
	 * 把规格应用到资产实例上。
	 * 曲线与 Pattern 数组在构造函数里已有默认值，这里只覆盖差异部分，
	 * 保证生成出来的资产与"手动右键新建"的资产结构完全一致。
	 */
	static void ApplySpec(ULyraRecoilProfile& Profile, const FProfileSpec& Spec)
	{
		Profile.RecoilPerShot_Vertical = Spec.RecoilPerShot_Vertical;
		Profile.RecoilPerShot_Horizontal = Spec.RecoilPerShot_Horizontal;

		Profile.RecoveryDelay = Spec.RecoveryDelay;
		Profile.RecoveryTime = Spec.RecoveryTime;
		Profile.RecoilReturnRatio = Spec.RecoilReturnRatio;

		Profile.MaxVerticalKick = Spec.MaxVerticalKick;
		Profile.MaxHorizontalKick = Spec.MaxHorizontalKick;

		Profile.HorizontalRandomRange = Spec.HorizontalRandomRange;

		Profile.PoseMultiplier_Aiming = Spec.PoseMultiplier_Aiming;
		Profile.PoseMultiplier_Standing = Spec.PoseMultiplier_Standing;
		Profile.PoseMultiplier_Crouching = Spec.PoseMultiplier_Crouching;
		Profile.PoseMultiplier_JumpingOrFalling = Spec.PoseMultiplier_JumpingOrFalling;

		// ---- 单发后坐力模型 ----
		Profile.SingleShotMode = Spec.SingleShotMode;
		Profile.LiftDuration = Spec.LiftDuration;
		Profile.ReboundDuration = Spec.ReboundDuration;
		Profile.ReboundRatio = Spec.ReboundRatio;
		WriteCurve(Profile.LiftCurve, Spec.LiftCurveKeys);
		WriteCurve(Profile.ReboundCurve, Spec.ReboundCurveKeys);

		// 重叠写垂直 Kick 曲线
		WriteCurve(Profile.VerticalKickCurve, Spec.VerticalKickKeys);

		// 固定 Pattern 必须落进 PatternPoints 范围内
		Profile.PatternLength = FMath::Clamp(Spec.PatternLength, 0, Profile.PatternPoints.Num());
	}

	static bool SaveProfileAsset(ULyraRecoilProfile& Profile, const FString& PackageName)
	{
		UPackage* Package = Profile.GetOutermost();
		if (Package == nullptr)
		{
			UE_LOG(LogLyraRecoilAssetGen, Error, TEXT("Asset %s has no outer package, aborting save"), *Profile.GetPathName());
			return false;
		}

		FAssetRegistryModule::AssetCreated(&Profile);
		Package->MarkPackageDirty();

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());

		const bool bSaved = UPackage::SavePackage(Package, &Profile, *Filename, SaveArgs);
		if (!bSaved)
		{
			UE_LOG(LogLyraRecoilAssetGen, Error, TEXT("Failed to save asset: %s -> %s"), *PackageName, *Filename);
		}

		return bSaved;
	}

	static ULyraRecoilProfile* CreateOrLoadProfile(const FProfileSpec& Spec, bool bForce)
	{
		// ---------------------------------------------------------------------
		// 先按**磁盘存在性**判断是否已生成，再决定要不要碰包。
		//
		// 为什么不能只靠 CreatePackage + FindObject：
		// Commandlet 环境下 CreatePackage 会以一个"部分加载"的 UPackage 返回，
		// 之后任何 SavePackage 都会触发
		//   "Asset '...' cannot be saved as it has only been partially loaded"
		// 并且是 appError 级别的 Critical Error，直接把进程干掉 ——
		// 也就是说：**只要磁盘上已有同名资产，这条路径必崩**，
		// 导致后面还没处理的资产（例如新增的 Rifle_S / Rifle_7）永远轮不到。
		//
		// 因此这里先用文件系统判断：已存在就直接跳过（除非 -force），
		// 完全不碰包对象，新增资产才能顺利走到生成分支。
		// ---------------------------------------------------------------------
		const FString ExistingFilename = FPackageName::LongPackageNameToFilename(
			Spec.PackageName, FPackageName::GetAssetPackageExtension());
		const bool bExistsOnDisk = FPaths::FileExists(ExistingFilename);

		if (bExistsOnDisk && !bForce)
		{
			UE_LOG(LogLyraRecoilAssetGen, Display,
				TEXT("SKIPPED (already on disk, pass -force to overwrite): %s"), *Spec.PackageName);
			return nullptr;
		}

		if (bExistsOnDisk && bForce)
		{
			// -force 覆盖已有资产：必须**完整加载**后再改，否则保存会被拒绝。
			// LoadObject 会走正常加载流程，得到的对象是可保存的（非 partial）。
			const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *Spec.PackageName, *Spec.AssetName);
			UE_LOG(LogLyraRecoilAssetGen, Display, TEXT("FORCE-OVERWRITE (loading existing): %s"), *ObjectPath);
		}

		UPackage* Package = CreatePackage(*Spec.PackageName);
		if (Package == nullptr)
		{
			UE_LOG(LogLyraRecoilAssetGen, Error, TEXT("CreatePackage failed: %s"), *Spec.PackageName);
			return nullptr;
		}

		ULyraRecoilProfile* Profile = nullptr;

		if (bExistsOnDisk)
		{
			// 覆盖模式：完整加载已有资产
			const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *Spec.PackageName, *Spec.AssetName);
			Profile = LoadObject<ULyraRecoilProfile>(nullptr, *ObjectPath);
			if (Profile == nullptr)
			{
				UE_LOG(LogLyraRecoilAssetGen, Error, TEXT("-force: failed to load existing asset %s"), *ObjectPath);
				return nullptr;
			}
		}
		else
		{
			// 新建：包此时必定是干净的
			Profile = NewObject<ULyraRecoilProfile>(
				Package, *Spec.AssetName, RF_Public | RF_Standalone | RF_Transactional);
		}

		if (Profile == nullptr)
		{
			UE_LOG(LogLyraRecoilAssetGen, Error, TEXT("Failed to create/load profile: %s"), *Spec.PackageName);
			return nullptr;
		}

		ApplySpec(*Profile, Spec);
		return Profile;
	}
}

ULyraRecoilAssetGenCommandlet::ULyraRecoilAssetGenCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

int32 ULyraRecoilAssetGenCommandlet::Main(const FString& FullCommandLine)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> Params;
	ParseCommandLine(*FullCommandLine, Tokens, Switches, Params);

	const bool bForce = Switches.Contains(TEXT("force"));

	UE_LOG(LogLyraRecoilAssetGen, Display, TEXT("LyraRecoilAssetGen started (bForce=%s)"), bForce ? TEXT("true") : TEXT("false"));

	TArray<LyraRecoilAssetGen::FProfileSpec> Specs;
	Specs.Add(LyraRecoilAssetGen::MakeRifleSpec());
	Specs.Add(LyraRecoilAssetGen::MakePistolSpec());
	Specs.Add(LyraRecoilAssetGen::MakeShotgunSpec());
	// 默认携带的两把枪：Rifle_S 走插值 / Rifle_7 走瞬间写入
	Specs.Add(LyraRecoilAssetGen::MakeRifleSSpec());
	Specs.Add(LyraRecoilAssetGen::MakeRifle7Spec());

	int32 NumGenerated = 0;
	int32 NumSkipped = 0;
	int32 NumFailed = 0;

	for (const LyraRecoilAssetGen::FProfileSpec& Spec : Specs)
	{
		ULyraRecoilProfile* Profile = LyraRecoilAssetGen::CreateOrLoadProfile(Spec, bForce);
		if (Profile == nullptr)
		{
			++NumSkipped;
			continue;
		}

		// 生成时即做自洽校验，避免产出不合格资产
		TArray<FString> ValidationErrors;
		if (!Profile->ValidateProfile(ValidationErrors))
		{
			++NumFailed;
			for (const FString& Error : ValidationErrors)
			{
				UE_LOG(LogLyraRecoilAssetGen, Error, TEXT("Generated asset failed validation %s :: %s"), *Spec.PackageName, *Error);
			}
			continue;
		}

		if (LyraRecoilAssetGen::SaveProfileAsset(*Profile, Spec.PackageName))
		{
			++NumGenerated;
			UE_LOG(LogLyraRecoilAssetGen, Display, TEXT("CREATED %s (PatternLength=%d / PatternPoints=%d)"),
				*Spec.PackageName, Profile->PatternLength, Profile->PatternPoints.Num());
		}
		else
		{
			++NumFailed;
		}
	}

	UE_LOG(LogLyraRecoilAssetGen, Display, TEXT("Done. Created=%d Skipped=%d Failed=%d"), NumGenerated, NumSkipped, NumFailed);

	return (NumFailed > 0) ? 1 : 0;
}
