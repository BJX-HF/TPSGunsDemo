// Copyright Epic Games, Inc. All Rights Reserved.

#include "LyraRecoilGoldenDumpCommandlet.h"

#include "Dom/JsonObject.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LyraRecoilGoldenDumpCommandlet)

DEFINE_LOG_CATEGORY_STATIC(LogLyraRecoilGolden, Log, All);

namespace LyraRecoilGolden
{
	/** Golden 覆盖的发数（开发计划 §P3：固定种子连发 20 发） */
	static constexpr int32 ShotCount = 20;

	/** Golden 生成条件：姿态与全局倍率都取中性值，隔离姿态/调试因素 */
	static constexpr float PoseMultiplier = 1.0f;
	static constexpr float GlobalScale = 1.0f;

	static const TCHAR* const ProfilePackages[] =
	{
		TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle"),
		TEXT("/Game/Weapons/Recoil/DA_Recoil_Pistol"),
		TEXT("/Game/Weapons/Recoil/DA_Recoil_Shotgun"),
		// 默认携带的两把枪：一把走插值、一把走瞬间写入（见 10_SingleShotInterpolation.md §6）
		TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle_S"),
		TEXT("/Game/Weapons/Recoil/DA_Recoil_Rifle_7")
	};

	static FString GetRelativeOutputDirectory()
	{
		return TEXT("Source/LyraGame/Tests/Data");
	}

	/** 用完整对象路径加载，避免依赖 LoadObject 对包名的隐式补全行为 */
	static ULyraRecoilProfile* LoadProfile(const FString& PackageName)
	{
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *FPaths::GetBaseFilename(PackageName));
		return LoadObject<ULyraRecoilProfile>(nullptr, *ObjectPath);
	}

	static bool WriteGoldenFile(ULyraRecoilProfile& Profile, const FString& PackageName)
	{
		const int32 Seed = FRecoilRuntimeState::ResolveSeed(&Profile);

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("generatedBy"), TEXT("LyraRecoilGoldenDumpCommandlet"));
		Root->SetStringField(TEXT("note"), TEXT("Regression lock for FRecoilRuntimeState::ComputeShotKick. Re-run -run=LyraRecoilGoldenDump after changing the profile asset."));
		Root->SetStringField(TEXT("assetPath"), PackageName);
		Root->SetNumberField(TEXT("seed"), static_cast<double>(Seed));
		Root->SetNumberField(TEXT("shotCount"), static_cast<double>(ShotCount));
		Root->SetNumberField(TEXT("poseMultiplier"), static_cast<double>(PoseMultiplier));
		Root->SetNumberField(TEXT("globalScale"), static_cast<double>(GlobalScale));

		// 把参与计算的关键资产字段也写进去，测试可以据此判断 golden 是否已过期
		Root->SetNumberField(TEXT("patternLength"), static_cast<double>(Profile.PatternLength));
		Root->SetNumberField(TEXT("recoilPerShotVertical"), static_cast<double>(Profile.RecoilPerShot_Vertical));
		Root->SetNumberField(TEXT("recoilPerShotHorizontal"), static_cast<double>(Profile.RecoilPerShot_Horizontal));
		Root->SetNumberField(TEXT("horizontalRandomRange"), static_cast<double>(Profile.HorizontalRandomRange));

		// 单发后坐力模型：把模式与插值参数一并写入，Golden 才能识别"换了模型"。
		// 注意：单发 Kick 的数值本身由 ComputeShotKick 决定，与选择哪套模型**无关**
		//（两套模型只在"相机怎么跟上"这一步分叉），所以这些字段是**元信息**，
		// 用来确保 Golden 与资产配置同步，而不是参与数值比对。
		Root->SetStringField(TEXT("singleShotMode"),
			Profile.SingleShotMode == ERecoilSingleShotMode::Interpolated ? TEXT("Interpolated") : TEXT("InstantWrite"));
		Root->SetNumberField(TEXT("liftDuration"), static_cast<double>(Profile.LiftDuration));
		Root->SetNumberField(TEXT("reboundDuration"), static_cast<double>(Profile.ReboundDuration));
		Root->SetNumberField(TEXT("reboundRatio"), static_cast<double>(Profile.ReboundRatio));

		TArray<TSharedPtr<FJsonValue>> ShotArray;
		ShotArray.Reserve(ShotCount);

		for (int32 ShotIndex = 0; ShotIndex < ShotCount; ++ShotIndex)
		{
			const FRecoilShotKick Kick = FRecoilRuntimeState::ComputeShotKick(
				Profile, ShotIndex, PoseMultiplier, GlobalScale, Seed);

			TSharedRef<FJsonObject> ShotObject = MakeShared<FJsonObject>();
			ShotObject->SetNumberField(TEXT("index"), static_cast<double>(ShotIndex));
			ShotObject->SetNumberField(TEXT("vertical"), static_cast<double>(Kick.Vertical));
			ShotObject->SetNumberField(TEXT("horizontal"), static_cast<double>(Kick.Horizontal));

			ShotArray.Add(MakeShared<FJsonValueObject>(ShotObject));
		}

		Root->SetArrayField(TEXT("shots"), ShotArray);

		FString Serialized;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
		if (!FJsonSerializer::Serialize(Root, Writer, /*bCloseWriter=*/ true))
		{
			UE_LOG(LogLyraRecoilGolden, Error, TEXT("Failed to serialize golden JSON for %s"), *PackageName);
			return false;
		}

		const FString OutputFile = FPaths::ProjectDir() / GetRelativeOutputDirectory()
			/ FString::Printf(TEXT("RecoilGolden_%s.json"), *FPaths::GetBaseFilename(PackageName));

		if (!FFileHelper::SaveStringToFile(Serialized, *OutputFile))
		{
			UE_LOG(LogLyraRecoilGolden, Error, TEXT("Failed to write golden file: %s"), *OutputFile);
			return false;
		}

		UE_LOG(LogLyraRecoilGolden, Display, TEXT("WROTE %s (seed=%d shots=%d)"), *OutputFile, Seed, ShotCount);
		return true;
	}
}

ULyraRecoilGoldenDumpCommandlet::ULyraRecoilGoldenDumpCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

int32 ULyraRecoilGoldenDumpCommandlet::Main(const FString& FullCommandLine)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> Params;
	ParseCommandLine(*FullCommandLine, Tokens, Switches, Params);

	UE_LOG(LogLyraRecoilGolden, Display, TEXT("LyraRecoilGoldenDump started"));

	int32 NumWritten = 0;
	int32 NumFailed = 0;

	for (const TCHAR* PackageName : LyraRecoilGolden::ProfilePackages)
	{
		ULyraRecoilProfile* Profile = LyraRecoilGolden::LoadProfile(PackageName);
		if (Profile == nullptr)
		{
			UE_LOG(LogLyraRecoilGolden, Error, TEXT("Failed to load profile: %s (run -run=LyraRecoilAssetGen first)"), PackageName);
			++NumFailed;
			continue;
		}

		if (LyraRecoilGolden::WriteGoldenFile(*Profile, PackageName))
		{
			++NumWritten;
		}
		else
		{
			++NumFailed;
		}
	}

	UE_LOG(LogLyraRecoilGolden, Display, TEXT("Done. Written=%d Failed=%d"), NumWritten, NumFailed);

	return (NumFailed > 0) ? 1 : 0;
}
