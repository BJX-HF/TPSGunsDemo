// Copyright Epic Games, Inc. All Rights Reserved.

#include "Recoil/LyraRecoilPreviewController.h"
#include "Recoil/LyraRecoilPatternAdapter.h"

#include "Curves/CurveFloat.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogLyraRecoilPreview, Log, All);

namespace LyraRecoilPreviewPrivate
{
	/** 预览里"不做任何缩放"的倍率（理论累计 Kick 用）。 */
	constexpr float TheoreticalMultiplier = 1.0f;

	/** 帧步长下限，避免配置把 FrameSeconds 传成 0 造成死循环。 */
	constexpr float MinFrameSeconds = 1.0f / 1000.0f;

	/** 事件边界比较容差（秒）。 */
	constexpr float TimeTolerance = 1.0e-5f;

	/**
	 * 深复制一条 FRichCurve：键、切线、切线权重、插值模式、切线模式、外推、默认值。
	 *
	 * 计划 §8.3 要求"复制当前有效 RichCurve 的键、切线、插值、外推与默认值"，
	 * 因此这里不用采样点重建，直接整条结构复制。
	 */
	void CopyRichCurve(const FRichCurve& Source, FRichCurve& Destination)
	{
		Destination.SetKeys(Source.GetConstRefOfKeys());
		Destination.PreInfinityExtrap = Source.PreInfinityExtrap;
		Destination.PostInfinityExtrap = Source.PostInfinityExtrap;
		Destination.DefaultValue = Source.DefaultValue;
	}

	/** 收集一个 UObject 上全部 FRuntimeFloatCurve 属性。 */
	void GatherRuntimeFloatCurveProperties(UObject* Object, TArray<FStructProperty*>& OutProperties)
	{
		OutProperties.Reset();
		if (Object == nullptr)
		{
			return;
		}

		const UScriptStruct* RuntimeCurveStruct = FRuntimeFloatCurve::StaticStruct();
		for (TFieldIterator<FStructProperty> It(Object->GetClass()); It; ++It)
		{
			FStructProperty* Property = *It;
			if ((Property != nullptr) && (Property->Struct == RuntimeCurveStruct))
			{
				OutProperties.Add(Property);
			}
		}
	}

	/** 把一条 FRuntimeFloatCurve 的外部引用深复制成内联数据并断开引用。 */
	void InlineRuntimeFloatCurve(FRuntimeFloatCurve& RuntimeCurve, bool& bOutWasExternal)
	{
		bOutWasExternal = false;

		// GetRichCurveConst() 返回的可能是外部资产上的曲线（ExternalCurve 优先）。
		const FRichCurve* EffectiveCurve = RuntimeCurve.GetRichCurveConst();
		if (EffectiveCurve == nullptr)
		{
			return;
		}

		if (RuntimeCurve.ExternalCurve != nullptr)
		{
			bOutWasExternal = true;
			CopyRichCurve(*EffectiveCurve, RuntimeCurve.EditorCurveData);
			// 断开外部引用：这是"隔离快照"的关键一步，快照随后不再受共享曲线变动影响。
			// 只写副本字段，原资产上的引用与外部曲线对象都不动。
			RuntimeCurve.ExternalCurve = nullptr;
		}
	}
}

// ---------------------------------------------------------------------------
// 生命周期 / GC
// ---------------------------------------------------------------------------

FLyraRecoilPreviewController::FLyraRecoilPreviewController()
{
	Config.FrameSeconds = FMath::Max(Config.FrameSeconds, LyraRecoilPreviewPrivate::MinFrameSeconds);
}

FLyraRecoilPreviewController::~FLyraRecoilPreviewController()
{
	// 显式释放顺序：先解绑快照强引用，再交给 FGCObject 的析构。
	SnapshotStrongRef.Reset();
	Snapshot.Profile = nullptr;
}

void FLyraRecoilPreviewController::AddReferencedObjects(FReferenceCollector& Collector)
{
	// TSharedPtr / 裸指针都不保护 UObject：快照必须显式登记。
	// 用 TStrongObjectPtr::Get() 拿到完整类型的对象指针（增量 GC 下不要传裸 UObject*）。
	if (Snapshot.Profile != nullptr)
	{
		Collector.AddReferencedObject(Snapshot.Profile);
	}
}

FString FLyraRecoilPreviewController::GetReferencerName() const
{
	return TEXT("FLyraRecoilPreviewController");
}

// ---------------------------------------------------------------------------
// 快照
// ---------------------------------------------------------------------------

FLyraRecoilPreviewSnapshot FLyraRecoilPreviewController::BuildIsolatedSnapshot(ULyraRecoilProfile* SourceProfile)
{
	FLyraRecoilPreviewSnapshot Result;

	if (SourceProfile == nullptr)
	{
		Result.Error = TEXT("Profile 为空：预览需要一份 ULyraRecoilProfile 才能构建隔离快照。");
		return Result;
	}

	Result.SourceProfilePath = SourceProfile->GetPathName();

	// StaticDuplicateObject 到 Transient 包：副本与原资产、原包完全无关，
	// 之后对副本的任何写入（固定种子 / 单发模式覆盖）都不会标脏原资产。
	UObject* Duplicated = StaticDuplicateObject(
		SourceProfile, GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional);
	ULyraRecoilProfile* SnapshotProfile = Cast<ULyraRecoilProfile>(Duplicated);
	if (SnapshotProfile == nullptr)
	{
		Result.Error = FString::Printf(
			TEXT("无法复制 Profile 的隔离副本：%s"), *Result.SourceProfilePath);
		return Result;
	}
	SnapshotProfile->SetFlags(RF_Transient);
	SnapshotProfile->ClearFlags(RF_Public | RF_Standalone);

	// 深内联**全部** FRuntimeFloatCurve（VerticalKick / Recovery / Lift / Rebound /
	// RollShake 三条 / 未来新增字段），而不是只保护 VerticalKickCurve（计划 §8.3）。
	TArray<FStructProperty*> CurveProperties;
	LyraRecoilPreviewPrivate::GatherRuntimeFloatCurveProperties(SnapshotProfile, CurveProperties);

	for (FStructProperty* Property : CurveProperties)
	{
		FRuntimeFloatCurve* RuntimeCurve = Property->ContainerPtrToValuePtr<FRuntimeFloatCurve>(SnapshotProfile);
		if (RuntimeCurve == nullptr)
		{
			continue;
		}

		bool bWasExternal = false;
		LyraRecoilPreviewPrivate::InlineRuntimeFloatCurve(*RuntimeCurve, bWasExternal);

		Result.Info.CurvePropertyNames.Add(Property->GetFName());
		if (bWasExternal)
		{
			Result.Info.InlinedCurvePropertyNames.Add(Property->GetFName());
		}
	}

	// 兜底检查：内联后仍指向外部曲线的字段（原外部曲线无有效键时会保留原引用）。
	for (FStructProperty* Property : CurveProperties)
	{
		const FRuntimeFloatCurve* RuntimeCurve = Property->ContainerPtrToValuePtr<FRuntimeFloatCurve>(SnapshotProfile);
		if ((RuntimeCurve != nullptr) && (RuntimeCurve->ExternalCurve != nullptr))
		{
			Result.Info.UnresolvedExternalCurvePropertyNames.Add(Property->GetFName());
		}
	}

	Result.Info.CurvePropertyCount = Result.Info.CurvePropertyNames.Num();
	Result.Profile = SnapshotProfile;

	UE_LOG(LogLyraRecoilPreview, Verbose,
		TEXT("构建隔离快照 %s：曲线属性 %d 个，外部引用内联 %d 个，未内联 %d 个"),
		*Result.SourceProfilePath,
		Result.Info.CurvePropertyCount,
		Result.Info.InlinedCurvePropertyNames.Num(),
		Result.Info.UnresolvedExternalCurvePropertyNames.Num());

	return Result;
}

bool FLyraRecoilPreviewController::RefreshProfile(
	ULyraRecoilProfile* InProfile,
	FString& OutError,
	TArray<FName>& OutInlinedCurveNames)
{
	// 旧快照必须彻底释放，避免"新旧参数混合"（计划 §10.3）。
	SnapshotStrongRef.Reset();
	Snapshot = FLyraRecoilPreviewSnapshot();

	OutInlinedCurveNames.Reset();
	OutError.Reset();

	Snapshot = BuildIsolatedSnapshot(InProfile);
	OutError = Snapshot.Error;
	OutInlinedCurveNames = Snapshot.Info.InlinedCurvePropertyNames;

	if (Snapshot.Profile == nullptr)
	{
		InvalidateCache();
		return false;
	}

	SnapshotStrongRef = TStrongObjectPtr<ULyraRecoilProfile>(Snapshot.Profile);
	SnapshotSourcePath = Snapshot.SourceProfilePath;
	SourceSingleShotMode = Snapshot.Profile->SingleShotMode;

	ApplyPreviewOptionsToSnapshot();
	ValidateSnapshotProfile();
	InvalidateCache();
	if (!bSnapshotValid)
	{
		OutError = FString::Join(Warnings, TEXT("\n"));
	}
	return bSnapshotValid;
}

void FLyraRecoilPreviewController::ApplyPreviewOptionsToSnapshot()
{
	ULyraRecoilProfile* SnapshotProfile = Snapshot.Profile;
	if (SnapshotProfile == nullptr)
	{
		return;
	}

	// ★ 固定种子只在副本上覆盖（计划 §10.2）：新轮次可能 ResolveSeed，
	//   所以必须把副本的 RandomSeedMode 也固定住，而不是只设一次 ActiveSeed。
	SnapshotProfile->RandomSeedMode = Config.SeedModeOverride;
	SnapshotProfile->FixedRandomSeed = Config.FixedSeedOverride;

	switch (Config.SingleShotModeOverride)
	{
	case ELyraRecoilPreviewSingleShotMode::InstantWrite:
		SnapshotProfile->SingleShotMode = ERecoilSingleShotMode::InstantWrite;
		break;
	case ELyraRecoilPreviewSingleShotMode::Interpolated:
		SnapshotProfile->SingleShotMode = ERecoilSingleShotMode::Interpolated;
		break;
	case ELyraRecoilPreviewSingleShotMode::FromProfile:
	default:
		SnapshotProfile->SingleShotMode = SourceSingleShotMode;
		break;
	}
}

void FLyraRecoilPreviewController::ValidateSnapshotProfile()
{
	Warnings.Reset();

	const ULyraRecoilProfile* SnapshotProfile = Snapshot.Profile;
	bSnapshotValid = false;
	if (SnapshotProfile == nullptr)
	{
		return;
	}

	// 复用资产自带的验证器（计划 §6：GUI 自身的校验需覆盖现有 ValidateProfile）。
	TArray<FString> Errors;
	bSnapshotValid = FLyraRecoilPatternAdapter::Validate(*SnapshotProfile, Errors);
	if (!bSnapshotValid)
	{
		for (const FString& Error : Errors)
		{
			Warnings.Add(FString::Printf(TEXT("ValidateProfile: %s"), *Error));
		}
	}
}

// ---------------------------------------------------------------------------
// 配置 / 缓存
// ---------------------------------------------------------------------------

void FLyraRecoilPreviewController::SetConfig(const FLyraRecoilPreviewConfig& InConfig)
{
	Config = InConfig;
	Config.InputScript.StableSort([](const FLyraRecoilPreviewInputEntry& A, const FLyraRecoilPreviewInputEntry& B) { return A.TimeSeconds < B.TimeSeconds; });

	ApplyPreviewOptionsToSnapshot();
	ValidateSnapshotProfile();
	bInputsValid = ValidateInputs();
	InvalidateCache();
}

bool FLyraRecoilPreviewController::ValidateInputs()
{
	bool bValid = true;
	auto Check = [this, &bValid](const TCHAR* Name, double Value, double Minimum, double Maximum)
	{
		if (!FMath::IsFinite(Value) || Value < Minimum || Value > Maximum)
		{
			Warnings.Add(FString::Printf(TEXT("预览输入 %s = %.9g 非法，允许范围 [%.9g, %.9g]。"), Name, Value, Minimum, Maximum));
			bValid = false;
		}
	};
	Check(TEXT("RPM"), Config.RPM, 1.e-4, 1.e7);
	Check(TEXT("FrameSeconds"), Config.FrameSeconds, 1.e-3, 120.0);
	Check(TEXT("TailSeconds"), Config.TailSeconds, 0.0, MAX_flt);
	Check(TEXT("GlobalScale"), Config.GlobalScale, 0.0, MAX_flt);
	Check(TEXT("SpreadMovementMultiplier"), Config.SpreadMovementMultiplier, 0.0, MAX_flt);
	Check(TEXT("BurstStartPitch"), Config.BurstStartAnglePitch, -MAX_flt, MAX_flt);
	Check(TEXT("BurstStartYaw"), Config.BurstStartAngleYaw, -MAX_flt, MAX_flt);
	Check(TEXT("LongFrame"), Config.LongFrameOverrideSeconds, 0.0, 120.0);
	Check(TEXT("ShotCount"), Config.ShotCount, 0, MAX_int32);
	if (Config.InputScript.Num() > 4096 || Config.FireInputs.Num() > 4096)
	{
		Warnings.Add(TEXT("预览脚本或连发轮次超过4096条。"));
		bValid = false;
	}
	for (const auto& Entry : Config.InputScript)
	{
		Check(TEXT("Input.Time"), Entry.TimeSeconds, 0, 120);
		Check(TEXT("Input.Pitch"), Entry.AimPitchDegrees, -MAX_flt, MAX_flt);
		Check(TEXT("Input.Yaw"), Entry.AimYawDegrees, -MAX_flt, MAX_flt);
		Check(TEXT("Input.Alpha"), Entry.AimingAlpha, 0, 1);
		Check(TEXT("Input.Scale"), Entry.GlobalScale, 0, MAX_flt);
		Check(TEXT("Input.Pose"), static_cast<uint8>(Entry.PoseState), 0, 2);
	}
	for (const auto& Entry : Config.FireInputs)
	{
		Check(TEXT("Fire.Time"), Entry.StartTimeSeconds, 0, 120);
		Check(TEXT("Fire.Count"), Entry.ShotCount, 0, MAX_int32);
		Check(TEXT("Fire.RPM"), Entry.RPM, 1.e-4, 1.e7);
	}
	return bValid;
}

void FLyraRecoilPreviewController::SetSampleMode(ELyraRecoilPreviewSampleMode InMode)
{
	if (SampleMode != InMode)
	{
		SampleMode = InMode;
		InvalidateCache();
	}
}

void FLyraRecoilPreviewController::SetFrameBoundarySamplingEnabled(bool bEnabled)
{
	if (bRecordFrameBoundarySamples != bEnabled)
	{
		bRecordFrameBoundarySamples = bEnabled;
		InvalidateCache();
	}
}

void FLyraRecoilPreviewController::InvalidateCache()
{
	bResultDirty = true;
	CachedRequestToken = 0;

	// 任何资产 / 曲线 / 预览输入改变都必须从头重算，避免新旧参数混合（计划 §10.3）。
	Samples.Reset();
	Shots.Reset();
	EventSampleIndices.Reset();
	FireTimes.Reset();
	PlannedDurationSeconds = 0.0f;
	CurrentTimeSeconds = 0.0f;
	NextFireIndex = 0;
	FrameIndex = 0;
	ExecutedStepCount = 0;
	LastAdvanceCallCount = 0;
	TheoreticalKickPitch = 0.0f;
	TheoreticalKickYaw = 0.0f;
	TheoreticalBeforeShotPitch = 0.0f;
	TheoreticalBeforeShotYaw = 0.0f;
	bSimulationComplete = false;
	bTruncatedByLimit = false;
	bReplayInitialized = false;
	bLongFrameConsumed = false;
	TruncationReason.Reset();
	PlayheadTimeSeconds = 0.0f;
}

uint32 FLyraRecoilPreviewController::ComputeRequestToken() const
{
	// 令牌 = 快照指纹 + 配置指纹。任何一项变化都会让旧结果失效。
	uint32 Token = GetTypeHash(SnapshotSourcePath);

	if (const ULyraRecoilProfile* SnapshotProfile = Snapshot.Profile)
	{
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->RecoilPerShot_Vertical));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->RecoilPerShot_Horizontal));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->PatternLength));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->PatternPoints.Num()));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->HorizontalRandomRange));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->MaxVerticalKick));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->MaxHorizontalKick));
		Token = HashCombine(Token, GetTypeHash(static_cast<uint8>(SnapshotProfile->SingleShotMode)));
		Token = HashCombine(Token, GetTypeHash(static_cast<uint8>(SnapshotProfile->RandomSeedMode)));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->FixedRandomSeed));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->LiftDuration));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->ReboundDuration));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->ReboundRatio));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->RecoveryDelay));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->RecoveryTime));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->bCompensationAwareRecovery));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->bCompensationAwareRecoveryYaw));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->GetPatternPoint(0).X));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->GetPatternPoint(0).Y));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->VerticalKickCurve.GetRichCurveConst()->GetNumKeys()));
		Token = HashCombine(Token, GetTypeHash(SnapshotProfile->GetVerticalKickCurveScale(0)));
	}

	Token = HashCombine(Token, GetTypeHash(Config.RPM));
	Token = HashCombine(Token, GetTypeHash(Config.ShotCount));
	Token = HashCombine(Token, GetTypeHash(Config.FrameSeconds));
	Token = HashCombine(Token, GetTypeHash(Config.BurstStartAnglePitch));
	Token = HashCombine(Token, GetTypeHash(Config.BurstStartAngleYaw));
	Token = HashCombine(Token, GetTypeHash(Config.GlobalScale));
	Token = HashCombine(Token, GetTypeHash(static_cast<uint8>(Config.SeedModeOverride)));
	Token = HashCombine(Token, GetTypeHash(Config.FixedSeedOverride));
	Token = HashCombine(Token, GetTypeHash(static_cast<uint8>(Config.SingleShotModeOverride)));
	Token = HashCombine(Token, GetTypeHash(Config.TailSeconds));
	Token = HashCombine(Token, GetTypeHash(Config.LongFrameOverrideSeconds));
	Token = HashCombine(Token, GetTypeHash(Config.bAdvanceSpread));
	Token = HashCombine(Token, GetTypeHash(Config.SpreadMovementMultiplier));
	Token = HashCombine(Token, GetTypeHash(Config.bValidateProfile));
	Token = HashCombine(Token, GetTypeHash(Config.FireInputs.Num()));
	Token = HashCombine(Token, GetTypeHash(Config.InputScript.Num()));
	Token = HashCombine(Token, GetTypeHash(static_cast<uint8>(SampleMode)));
	Token = HashCombine(Token, GetTypeHash(bRecordFrameBoundarySamples));

	// 输入脚本与多轮发射的逐项指纹。
	for (const FLyraRecoilPreviewInputEntry& Entry : Config.InputScript)
	{
		Token = HashCombine(Token, GetTypeHash(Entry.TimeSeconds));
		Token = HashCombine(Token, GetTypeHash(Entry.AimPitchDegrees));
		Token = HashCombine(Token, GetTypeHash(Entry.AimYawDegrees));
		Token = HashCombine(Token, GetTypeHash(static_cast<uint8>(Entry.PoseState)));
		Token = HashCombine(Token, GetTypeHash(Entry.AimingAlpha));
		Token = HashCombine(Token, GetTypeHash(Entry.bOverrideGlobalScale));
		Token = HashCombine(Token, GetTypeHash(Entry.GlobalScale));
	}
	for (const FLyraRecoilPreviewFireInput& Entry : Config.FireInputs)
	{
		Token = HashCombine(Token, GetTypeHash(Entry.StartTimeSeconds));
		Token = HashCombine(Token, GetTypeHash(Entry.ShotCount));
		Token = HashCombine(Token, GetTypeHash(Entry.RPM));
	}

	return Token;
}

bool FLyraRecoilPreviewController::IsResultCacheValid() const
{
	return !bResultDirty
		&& (CachedRequestToken != 0)
		&& (CachedRequestToken == ComputeRequestToken());
}

// ---------------------------------------------------------------------------
// 模拟计划
// ---------------------------------------------------------------------------

void FLyraRecoilPreviewController::BuildPlan(FSimulationPlan& OutPlan) const
{
	OutPlan.FireTimes.Reset();
	OutPlan.PlannedDuration = 0.0f;
	OutPlan.bTruncated = false;
	OutPlan.TruncationReason.Reset();

	if (Snapshot.Profile == nullptr)
	{
		OutPlan.TruncationReason = TEXT("没有可用快照：请先提供 Profile 并调用 RefreshProfile()。");
		return;
	}

	// --- 发射时刻表 ---
	if (Config.FireInputs.Num() > 0)
	{
		for (const FLyraRecoilPreviewFireInput& Fire : Config.FireInputs)
		{
			const float Interval = (Fire.RPM > 0.0f) ? (60.0f / Fire.RPM) : 0.0f;
			const int32 Count = FMath::Max(0, Fire.ShotCount);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				OutPlan.FireTimes.Add(Fire.StartTimeSeconds + (Interval * static_cast<float>(Index)));
			}

			if (OutPlan.FireTimes.Num() > FLyraRecoilPreviewConfig::MaxShots)
			{
				// 计划 §10.3 的工具保护值：注意这里**只**裁掉规划出来的时间轴，
				// 不通过让 ApplyShot 失败来"自然停住"（那会留下半截状态，无法诊断）。
				OutPlan.FireTimes.SetNum(FLyraRecoilPreviewConfig::MaxShots);
				OutPlan.bTruncated = true;
				OutPlan.TruncationReason = FString::Printf(
					TEXT("发射数被截到预览上限 %d 发（计划 §10.3 工具保护值）。"),
					FLyraRecoilPreviewConfig::MaxShots);
				break;
			}
		}
	}
	else
	{
		const float Interval = (Config.RPM > 0.0f) ? (60.0f / Config.RPM) : 0.0f;
		const int32 Count = FMath::Clamp(Config.ShotCount, 0, FLyraRecoilPreviewConfig::MaxShots);
		if (Config.ShotCount > FLyraRecoilPreviewConfig::MaxShots)
		{
			OutPlan.bTruncated = true;
			OutPlan.TruncationReason = FString::Printf(
				TEXT("发射数被截到预览上限 %d 发（计划 §10.3 工具保护值）。"),
				FLyraRecoilPreviewConfig::MaxShots);
		}

		for (int32 Index = 0; Index < Count; ++Index)
		{
			OutPlan.FireTimes.Add(Interval * static_cast<float>(Index));
		}
	}

	OutPlan.FireTimes.Sort();

	// --- 时间轴长度 = 最后一次发射 + 尾时长 ---
	const float LastFireTime = (OutPlan.FireTimes.Num() > 0) ? OutPlan.FireTimes.Last() : 0.0f;
	const float TailSeconds = FMath::Max(0.0f, Config.TailSeconds);
	float Duration = LastFireTime + TailSeconds;

	if (Duration > FLyraRecoilPreviewConfig::MaxSimulationSeconds)
	{
		Duration = FLyraRecoilPreviewConfig::MaxSimulationSeconds;
		OutPlan.bTruncated = true;
		if (OutPlan.TruncationReason.IsEmpty())
		{
			OutPlan.TruncationReason = FString::Printf(
				TEXT("时间轴被截到预览上限 %.1f 秒（计划 §10.3 工具保护值）。"),
				FLyraRecoilPreviewConfig::MaxSimulationSeconds);
		}
	}

	OutPlan.PlannedDuration = FMath::Max(Duration, 0.0f);
}

void FLyraRecoilPreviewController::EvaluateInputAtTime(
	float InTimeSeconds,
	float& OutAimPitch,
	float& OutAimYaw,
	EPoseState& OutPose,
	float& OutAimingAlpha,
	float& OutGlobalScale) const
{
	OutAimPitch = Config.BurstStartAnglePitch;
	OutAimYaw = Config.BurstStartAngleYaw;
	OutPose = EPoseState::Standing;
	OutAimingAlpha = 0.0f;
	OutGlobalScale = Config.GlobalScale;

	// 脚本逐条覆盖：取所有 TimeSeconds <= InTimeSeconds 的最后一条（脚本按时间升序）。
	for (const FLyraRecoilPreviewInputEntry& Entry : Config.InputScript)
	{
		if (Entry.TimeSeconds <= InTimeSeconds + LyraRecoilPreviewPrivate::TimeTolerance)
		{
			OutAimPitch = Entry.AimPitchDegrees;
			OutAimYaw = Entry.AimYawDegrees;
			OutPose = Entry.PoseState;
			OutAimingAlpha = FMath::Clamp(Entry.AimingAlpha, 0.0f, 1.0f);
			if (Entry.bOverrideGlobalScale)
			{
				OutGlobalScale = Entry.GlobalScale;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// 回放
// ---------------------------------------------------------------------------

bool FLyraRecoilPreviewController::Run()
{
	if (Snapshot.Profile == nullptr || !bSnapshotValid || !bInputsValid)
	{
		Warnings.AddUnique(TEXT("没有可用快照：请先提供 Profile 并调用 RefreshProfile()。"));
		return false;
	}

	const uint32 Token = ComputeRequestToken();

	// 预算给 0 表示"不限预算地跑完"，但内部仍然是同一套原子步，
	// 保证与 AdvanceBudget() 的逐事件序列与逐发结果完全一致。
	bool bCompleted = false;
	while (!bCompleted)
	{
		if (!AdvanceBudget(0.0, bCompleted))
		{
			return false;
		}
	}

	bResultDirty = false;
	CachedRequestToken = Token;
	return true;
}

bool FLyraRecoilPreviewController::AdvanceBudget(double BudgetMilliseconds, bool& bOutCompleted)
{
	bOutCompleted = false;

	if (Snapshot.Profile == nullptr || !bSnapshotValid || !bInputsValid)
	{
		bOutCompleted = true;
		return false;
	}

	// 第一次进入（或 seek 之后重放）：构建事件表并复位状态。
	if (!bReplayInitialized)
	{
		FSimulationPlan Plan;
		BuildPlan(Plan);
		FireTimes = Plan.FireTimes;
		PlannedDurationSeconds = Plan.PlannedDuration;
		bTruncatedByLimit = Plan.bTruncated;
		TruncationReason = Plan.TruncationReason;
		ResetSimulationState();
		bReplayInitialized = true;

		if (PlannedDurationSeconds <= 0.0f && FireTimes.IsEmpty())
		{
			bSimulationComplete = true;
		}
	}

	if (bSimulationComplete)
	{
		bResultDirty = false;
		CachedRequestToken = ComputeRequestToken();
		bOutCompleted = true;
		return true;
	}

	const double BudgetSeconds = FMath::Max(0.0, BudgetMilliseconds) / 1000.0;
	const double Deadline = FPlatformTime::Seconds() + BudgetSeconds;

	// 至少要执行一个原子步，否则预算过小会让界面完全卡住；同时不允许把预算切在
	// "一帧还没走完"的位置 —— 原子步边界必须与帧/发射事件边界对齐，否则采样序列
	// 会随预算改变，逐发结果无法与不限预算的 Run() 对齐（计划 §10.2 确定性要求）。
	// 预算 = 0（Run() 的用法）时 BudgetSeconds 为 0，Deadline 落在当前时刻，
	// 因此内层循环每轮只走一帧，由外层 while 反复调用直到跑完。
	do
	{
		bFrameInProgress = false;
		RunOneAtomicStep();
		if (bNumericalFailure)
		{
			Samples.Reset();
			Shots.Reset();
			bOutCompleted = true;
			return false;
		}
	} while (!bSimulationComplete
		&& (FPlatformTime::Seconds() < Deadline));

	if (bSimulationComplete)
	{
		bResultDirty = false;
		CachedRequestToken = ComputeRequestToken();
	}

	bOutCompleted = bSimulationComplete;
	return true;
}

void FLyraRecoilPreviewController::ResetSimulationState()
{
	RecoilState.Reset(Snapshot.Profile);
	bNumericalFailure = false;

	CurrentTimeSeconds = 0.0f;
	NextFireIndex = 0;
	FrameIndex = 0;
	ExecutedStepCount = 0;
	LastAdvanceCallCount = 0;
	TheoreticalKickPitch = 0.0f;
	TheoreticalKickYaw = 0.0f;
	TheoreticalBeforeShotPitch = 0.0f;
	TheoreticalBeforeShotYaw = 0.0f;
	bSimulationComplete = false;
	bLongFrameConsumed = false;
	bFrameInProgress = false;

	Samples.Reset();
	Shots.Reset();
	EventSampleIndices.Reset();

	const float FrameSeconds = FMath::Max(Config.FrameSeconds, LyraRecoilPreviewPrivate::MinFrameSeconds);
	const int32 ReserveCount = FMath::Clamp(
		static_cast<int32>(PlannedDurationSeconds / FrameSeconds) + 2, 8, 200000);
	Samples.Reserve(ReserveCount);

	// t=0 的初始输入：第一次 Advance 前采样，与武器实例"每帧先采样再 Advance"一致。
	float AimPitch = 0.0f;
	float AimYaw = 0.0f;
	EPoseState Pose = EPoseState::Standing;
	float AimingAlpha = 0.0f;
	float GlobalScale = Config.GlobalScale;
	EvaluateInputAtTime(0.0f, AimPitch, AimYaw, Pose, AimingAlpha, GlobalScale);

	RecoilState.SetGlobalScale(GlobalScale);
	RecoilState.SetPoseMultiplier(FRecoilRuntimeState::ComputePoseMultiplier(*Snapshot.Profile, Pose, AimingAlpha));
	RecoilState.SetSpreadPlayerMultipliers(
		Snapshot.Profile->GetSpreadAimingMultiplier(AimingAlpha),
		Config.SpreadMovementMultiplier);
	RecoilState.SamplePlayerAim(AimPitch, AimYaw);
}

int32 FLyraRecoilPreviewController::RunOneAtomicStep()
{
    const ULyraRecoilProfile* P = Snapshot.Profile;
    if (!P) { bSimulationComplete = true; return 0; }
    const float FrameEnd = FMath::Min(Config.FrameSeconds * (FrameIndex + 1), PlannedDurationSeconds);
    int32 Calls = 0;
    for (int32 Guard = 0; Guard < 16384; ++Guard)
    {
        // Input at a timestamp takes effect before firing at that same timestamp.
        float Pitch, Yaw, Alpha, Scale;
        EPoseState Pose;
        EvaluateInputAtTime(CurrentTimeSeconds, Pitch, Yaw, Pose, Alpha, Scale);
        RecoilState.SetGlobalScale(Scale);
        RecoilState.SetPoseMultiplier(FRecoilRuntimeState::ComputePoseMultiplier(*P, Pose, Alpha));
        RecoilState.SetSpreadPlayerMultipliers(P->GetSpreadAimingMultiplier(Alpha), Config.SpreadMovementMultiplier);
        RecoilState.SamplePlayerAim(Pitch, Yaw);
        while (NextFireIndex < FireTimes.Num() && FireTimes[NextFireIndex] <= CurrentTimeSeconds + 1.e-6f
            && FireTimes[NextFireIndex] <= PlannedDurationSeconds + 1.e-6f)
        {
            FireShot(FireTimes[NextFireIndex++]);
        }
        if (CurrentTimeSeconds >= FrameEnd - 1.e-6f) break;
        float Next = FrameEnd;
        if (NextFireIndex < FireTimes.Num() && FireTimes[NextFireIndex] > CurrentTimeSeconds + 1.e-6f)
            Next = FMath::Min(Next, FireTimes[NextFireIndex]);
        for (const auto& Input : Config.InputScript)
            if (Input.TimeSeconds > CurrentTimeSeconds + 1.e-6f)
            { Next = FMath::Min(Next, Input.TimeSeconds); break; }
        const float Delta = Next - CurrentTimeSeconds;
        if (!(Delta > 0)) { bNumericalFailure = true; Warnings.Add(TEXT("回放事件无法前进。")); break; }
        RecoilState.Advance(P, Delta);
        if (Config.bAdvanceSpread) RecoilState.AdvanceSpread(P, Delta, Pose);
        CurrentTimeSeconds = Next;
        ++Calls;
        if (SampleMode == ELyraRecoilPreviewSampleMode::EveryEvent && Next < FrameEnd - 1.e-6f)
            RecordSample(Next, false, SampleMode);
        if (bNumericalFailure) break;
    }
    if (bRecordFrameBoundarySamples) RecordSample(CurrentTimeSeconds, false, SampleMode);
    bFrameInProgress = false;
    ++FrameIndex;
    ++ExecutedStepCount;
    LastAdvanceCallCount = Calls;
    bSimulationComplete = CurrentTimeSeconds >= PlannedDurationSeconds - 1.e-6f;
    return Calls;
}

void FLyraRecoilPreviewController::FireShot(float InTimeSeconds)
{
	const ULyraRecoilProfile* SnapshotProfile = Snapshot.Profile;
	if (SnapshotProfile == nullptr)
	{
		return;
	}

	// --- 开火前：采样玩家瞄准并刷新倍率（武器 AddRecoil 的真实顺序）---
	float AimPitch = 0.0f;
	float AimYaw = 0.0f;
	EPoseState Pose = EPoseState::Standing;
	float AimingAlpha = 0.0f;
	float GlobalScale = Config.GlobalScale;
	EvaluateInputAtTime(InTimeSeconds, AimPitch, AimYaw, Pose, AimingAlpha, GlobalScale);

	RecoilState.SetGlobalScale(GlobalScale);
	const float PoseMultiplier = FRecoilRuntimeState::ComputePoseMultiplier(*SnapshotProfile, Pose, AimingAlpha);
	RecoilState.SetPoseMultiplier(PoseMultiplier);
	RecoilState.SamplePlayerAim(AimPitch, AimYaw);

	// --- 发射前：方向偏移（弹道链，取"这一发"的序号；预览不自行递增 ShotIndex）---
	const int32 ShotIndexAtFire = RecoilState.ShotIndex;

	const FRecoilShotKick DirectionOffset = FRecoilRuntimeState::ComputeShotKickGated(
		SnapshotProfile,
		ShotIndexAtFire,
		RecoilState.CurrentPoseMultiplier,
		RecoilState.GlobalScale,
		RecoilState.ActiveSeed,
		/*bRecoilEnabled=*/true);
	if (!FMath::IsFinite(DirectionOffset.Vertical) || !FMath::IsFinite(DirectionOffset.Horizontal))
	{ bNumericalFailure=true; Warnings.AddUnique(TEXT("弹道偏移产生非有限数，已取消回放。")); return; }

	// --- 散布快照（发弹那一刻），顺序与武器一致：NotifyShotSpreadUsed 在发射前 ---
	if (Config.bAdvanceSpread)
	{
		RecoilState.SetPendingShotSpreadAngle(RecoilState.GetEffectiveSpreadAngle());
	}

	// --- 散布加热（AddSpread）在 AddRecoil 之前，与武器调用链一致 ---
	if (Config.bAdvanceSpread)
	{
		RecoilState.ApplySpreadShot(SnapshotProfile, Pose);
	}

	// --- 相机链累加（AddRecoil → ApplyShot）---
	const float CameraPitchBefore = RecoilState.CameraOffsetPitch;
	const float CameraYawBefore = RecoilState.CameraOffsetYaw;
	const float AccumulatedPitchBefore=RecoilState.AccumulatedPitch;


    if (!RecoilState.ApplyShot(SnapshotProfile, PoseMultiplier, Pose))
    { bNumericalFailure = true; return; }
    const FRecoilShotResult& Applied = RecoilState.ShotHistory.Last();
    const FRecoilShotKick TheoreticalKick = FRecoilRuntimeState::ComputeShotKick(
        *SnapshotProfile, Applied.ShotIndex, 1.0f, 1.0f, RecoilState.ActiveSeed);
    TheoreticalBeforeShotPitch = TheoreticalKickPitch;
    TheoreticalBeforeShotYaw = TheoreticalKickYaw;
    TheoreticalKickPitch += TheoreticalKick.Vertical;
    TheoreticalKickYaw += TheoreticalKick.Horizontal;

	FLyraRecoilPreviewShot Shot;
	Shot.BeforeCameraOffsetPitch=CameraPitchBefore;
	Shot.BeforeCameraOffsetYaw=CameraYawBefore;
	Shot.BeforeAccumulatedPitch=AccumulatedPitchBefore;
	Shot.ShotIndex = ShotIndexAtFire;
	Shot.ActualShotIndex = Applied.ShotIndex;
	Shot.TimeSeconds = InTimeSeconds;
	Shot.DirectionOffsetPitch = DirectionOffset.Vertical;
	Shot.DirectionOffsetYaw = DirectionOffset.Horizontal;
	Shot.TheoreticalKickPitch = TheoreticalKick.Vertical;
	Shot.TheoreticalKickYaw = TheoreticalKick.Horizontal;
	Shot.TheoreticalBeforeKickPitch = TheoreticalBeforeShotPitch;
	Shot.TheoreticalBeforeKickYaw = TheoreticalBeforeShotYaw;
	Shot.BurstStartPitchOffset = RecoilState.BurstStartPitchOffset;
	Shot.BurstStartYawOffset = RecoilState.BurstStartYawOffset;
	Shot.ControlRotationPitch = AimPitch;
	Shot.ControlRotationYaw = AimYaw;
	Shot.VisibleAnglePitch = AimPitch + RecoilState.CameraOffsetPitch;
	Shot.VisibleAngleYaw = AimYaw + RecoilState.CameraOffsetYaw;
	Shot.PoseState = Pose;
	Shot.PoseMultiplier = PoseMultiplier;
	Shot.GlobalScale = RecoilState.GlobalScale;
	Shot.SpreadAngle = RecoilState.PendingShotSpreadAngle;
	Shot.bStartedNewBurst = Applied.ShotIndex == 0;

	if (RecoilState.ShotHistory.Num() > 0)
	{
		const FRecoilShotResult& Result = RecoilState.ShotHistory.Last();
		Shot.AppliedKickPitch = Result.VerticalKick;
		Shot.AppliedKickYaw = Result.HorizontalKick;
	}
	else
	{
		// ApplyShot 失败时不伪造记录，只记可见增量。
		Shot.AppliedKickPitch = RecoilState.CameraOffsetPitch - CameraPitchBefore;
		Shot.AppliedKickYaw = RecoilState.CameraOffsetYaw - CameraYawBefore;
	}

	Shots.Add(Shot);

	// 发射即刻样本（与发射同时间戳）。逐发方向偏移与相机链结果都进表。
	RecordSample(InTimeSeconds, /*bAfterFire=*/true, SampleMode);
}

void FLyraRecoilPreviewController::RecordSample(
	float InTimeSeconds,
	bool bAfterFire,
	ELyraRecoilPreviewSampleMode Mode)
{
	if ((Mode == ELyraRecoilPreviewSampleMode::FrameBoundaryOnly) && bAfterFire)
	{
		return;
	}

	// 浮点安全：时间不允许倒退。
	if (Samples.Num() > 0)
	{
		InTimeSeconds = FMath::Max(InTimeSeconds, Samples.Last().TimeSeconds);
	}

	FLyraRecoilPreviewSample Sample;
	Sample.TimeSeconds = InTimeSeconds;
	Sample.TheoreticalKickPitch = TheoreticalKickPitch;
	Sample.TheoreticalKickYaw = TheoreticalKickYaw;
	Sample.CameraOffsetPitch = RecoilState.CameraOffsetPitch;
	Sample.CameraOffsetYaw = RecoilState.CameraOffsetYaw;
	Sample.CameraOffsetRoll = RecoilState.GetCameraRollOffset();
	Sample.AccumulatedPitch = RecoilState.AccumulatedPitch;
	Sample.AccumulatedYaw = RecoilState.AccumulatedYaw;

	float AimPitch = 0.0f;
	float AimYaw = 0.0f;
	EPoseState Pose = EPoseState::Standing;
	float AimingAlpha = 0.0f;
	float GlobalScale = Config.GlobalScale;
	EvaluateInputAtTime(InTimeSeconds, AimPitch, AimYaw, Pose, AimingAlpha, GlobalScale);

	Sample.ControlRotationPitch = AimPitch;
	Sample.ControlRotationYaw = AimYaw;
	Sample.VisibleAnglePitch = AimPitch + RecoilState.CameraOffsetPitch;
	Sample.VisibleAngleYaw = AimYaw + RecoilState.CameraOffsetYaw;

	Sample.State = RecoilState.State;
	Sample.InterpStage = RecoilState.InterpStage;
	Sample.ShotIndex = RecoilState.ShotIndex;
	Sample.RecoveryPeakPitch = RecoilState.RecoveryPeakPitch;
	Sample.RecoveryPeakYaw = RecoilState.RecoveryPeakYaw;
	Sample.RecoveryCoverPitch = RecoilState.RecoveryCoverPitch;
	Sample.RecoveryCoverYaw = RecoilState.RecoveryCoverYaw;
	Sample.RecoveryProgress = RecoilState.RecoveryProgress;
	Sample.LastSubStepCount = static_cast<float>(RecoilState.LastSubStepCount);
	Sample.bAfterFireEvent = bAfterFire;


    const float Values[] = { Sample.TimeSeconds, Sample.TheoreticalKickPitch, Sample.TheoreticalKickYaw,
        Sample.CameraOffsetPitch, Sample.CameraOffsetYaw, Sample.CameraOffsetRoll,
        Sample.AccumulatedPitch, Sample.AccumulatedYaw, Sample.ControlRotationPitch,
        Sample.ControlRotationYaw, Sample.VisibleAnglePitch, Sample.VisibleAngleYaw,
        Sample.RecoveryPeakPitch, Sample.RecoveryPeakYaw, Sample.RecoveryCoverPitch,
        Sample.RecoveryCoverYaw, Sample.RecoveryProgress, RecoilState.CurrentSpreadAngle };
    for (float Value : Values)
        if (!FMath::IsFinite(Value))
        { bNumericalFailure = true; Warnings.AddUnique(TEXT("运行时回放产生非有限角度，已拒绝输出绘图数据。")); return; }
    Samples.Add(Sample);

	if (bAfterFire)
	{
		EventSampleIndices.Add(Samples.Num() - 1);
	}
}

bool FLyraRecoilPreviewController::RunLongFrameStep()
{
	if (!bSnapshotValid || !bInputsValid || (Snapshot.Profile == nullptr) || (Config.LongFrameOverrideSeconds <= 0.0f) || bLongFrameConsumed)
	{
		return false;
	}

	if (!bReplayInitialized)
	{
		// 还没开始回放时先正常起一段，再走长帧。
		bool bCompleted = false;
		AdvanceBudget(0.0, bCompleted);
	}

	const ULyraRecoilProfile* SnapshotProfile = Snapshot.Profile;
	const float LongDelta = Config.LongFrameOverrideSeconds;

	float AimPitch = 0.0f;
	float AimYaw = 0.0f;
	EPoseState Pose = EPoseState::Standing;
	float AimingAlpha = 0.0f;
	float GlobalScale = Config.GlobalScale;
	EvaluateInputAtTime(CurrentTimeSeconds, AimPitch, AimYaw, Pose, AimingAlpha, GlobalScale);

	RecoilState.SetGlobalScale(GlobalScale);
	RecoilState.SetPoseMultiplier(FRecoilRuntimeState::ComputePoseMultiplier(*SnapshotProfile, Pose, AimingAlpha));
	RecoilState.SetSpreadPlayerMultipliers(
		SnapshotProfile->GetSpreadAimingMultiplier(AimingAlpha),
		Config.SpreadMovementMultiplier);
	RecoilState.SamplePlayerAim(AimPitch, AimYaw);

	// 一个**单次**大 Delta 原样交给 Advance（可超过 MaxSubStepsPerAdvance 对应的 ~133ms），
	// 从而覆盖其真实长帧收尾分支。绝不切碎后声称覆盖了长帧保护。
	RecoilState.Advance(SnapshotProfile, LongDelta);
	if (Config.bAdvanceSpread)
	{
		RecoilState.AdvanceSpread(SnapshotProfile, LongDelta, Pose);
	}

	CurrentTimeSeconds += LongDelta;
	bLongFrameConsumed = true;
	bSimulationComplete = true;

	// 时间轴保持自洽：帧游标推进到当前时刻，避免后续普通步回到过去。
	const float FrameSeconds = FMath::Max(Config.FrameSeconds, LyraRecoilPreviewPrivate::MinFrameSeconds);
	FrameIndex = FMath::Max(FrameIndex, FMath::CeilToInt(CurrentTimeSeconds / FrameSeconds));
	PlannedDurationSeconds = FMath::Max(PlannedDurationSeconds, CurrentTimeSeconds);

	RecordSample(CurrentTimeSeconds, /*bAfterFire=*/false, SampleMode);
	++ExecutedStepCount;
	LastAdvanceCallCount = 1;

	return !bNumericalFailure;
}

// ---------------------------------------------------------------------------
// 结果查询
// ---------------------------------------------------------------------------

int32 FLyraRecoilPreviewController::FindSampleIndexAtTime(float InTime) const
{
	if (Samples.Num() == 0)
	{
		return INDEX_NONE;
	}

	// 采样表按时间单调不减：二分查找最后一条不晚于 InTime 的样本。
	int32 Low = 0;
	int32 High = Samples.Num() - 1;
	int32 Result = INDEX_NONE;

	while (Low <= High)
	{
		const int32 Mid = Low + ((High - Low) / 2);
		if (Samples[Mid].TimeSeconds <= InTime + LyraRecoilPreviewPrivate::TimeTolerance)
		{
			Result = Mid;
			Low = Mid + 1;
		}
		else
		{
			High = Mid - 1;
		}
	}

	return Result;
}

bool FLyraRecoilPreviewController::GetVisibleAngleAtTime(float InTime, float& OutPitch, float& OutYaw) const
{
	if (Samples.Num() == 0)
	{
		return false;
	}

	const int32 Index = FindSampleIndexAtTime(InTime);
	if (Index == INDEX_NONE)
	{
		OutPitch = Samples[0].VisibleAnglePitch;
		OutYaw = Samples[0].VisibleAngleYaw;
		return true;
	}

	// 边界外直接取端点；其余情况线性插值，**仅供显示**。
	if ((Index >= Samples.Num() - 1) || (Samples[Index + 1].TimeSeconds <= Samples[Index].TimeSeconds))
	{
		OutPitch = Samples[Index].VisibleAnglePitch;
		OutYaw = Samples[Index].VisibleAngleYaw;
		return true;
	}

	const FLyraRecoilPreviewSample& A = Samples[Index];
	const FLyraRecoilPreviewSample& B = Samples[Index + 1];
	const float Alpha = FMath::Clamp(
		(InTime - A.TimeSeconds) / (B.TimeSeconds - A.TimeSeconds), 0.0f, 1.0f);

	OutPitch = FMath::Lerp(A.VisibleAnglePitch, B.VisibleAnglePitch, Alpha);
	OutYaw = FMath::Lerp(A.VisibleAngleYaw, B.VisibleAngleYaw, Alpha);
	return true;
}

// ---------------------------------------------------------------------------
// 时间线回放（墙钟，不改变模拟时步）
// ---------------------------------------------------------------------------

void FLyraRecoilPreviewController::SeekToTime(float InSimulationTimeSeconds)
{
	PlayheadTimeSeconds = FMath::Clamp(InSimulationTimeSeconds, 0.0f, PlannedDurationSeconds);

	// 向后跳（或跳到更早的位置）必须从 t=0 用**同一快照**重放：
	// 这里只丢弃结果并把内部状态复位，真正的重放由调用方的播放循环再调 Run()/AdvanceBudget() 完成。
	// 不插值旧样本、不搬状态，避免"用几个输出值伪造状态机"（计划 §10.2）。
	if (PlayheadTimeSeconds < CurrentTimeSeconds)
	{
		Samples.Reset();
		Shots.Reset();
		EventSampleIndices.Reset();
		ResetSimulationState();

		// bReplayInitialized 置 false：下一次 Run()/AdvanceBudget() 会重新构建事件表并
		// 从 t=0 用同一快照完整重放（计划 §10.2）。
		bReplayInitialized = false;
	}
}

void FLyraRecoilPreviewController::AdvancePlayhead(float InWallClockDeltaSeconds, float InPlaybackSpeed)
{
	// 播放速度只影响播放墙钟位置，不参与任何模拟步长（计划 §10.2）。
	const float Delta = FMath::Max(0.0f, InWallClockDeltaSeconds) * FMath::Max(0.0f, InPlaybackSpeed);
	PlayheadTimeSeconds = FMath::Clamp(
		PlayheadTimeSeconds + Delta, 0.0f, FMath::Max(PlannedDurationSeconds, 0.0f));
}
