// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/GCObject.h"
#include "UObject/StrongObjectPtr.h"

#include "Weapons/Recoil/LyraRecoilProfile.h"
#include "Weapons/Recoil/LyraRecoilState.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"

/**
 * P4 数值预览（开发计划 §10）——编辑器侧的确定性回放控制器。
 *
 * 设计边界（对应计划 §10.1 / §10.2，违反即验收失败）：
 *
 *  1. **隔离快照**：构造时 StaticDuplicateObject 出一份临时 ULyraRecoilProfile，
 *     并把其中**全部** FRuntimeFloatCurve 的有效外部 RichCurve 深复制成内联曲线。
 *     预览只读快照、只写快照（固定种子等），原资产、PIE 武器状态、全局 CVar 一律不碰。
 *  2. **不重写算法**：推进与开火全部调用现有 FRecoilRuntimeState（Reset / Advance /
 *     ApplyShot / SamplePlayerAim / ApplySpreadShot / AdvanceSpread / ComputeShotKick…），
 *     本文件不含任何一套平行后坐力数学。
 *  3. **真实调用顺序**：每段 Advance 前 SamplePlayerAim；每个发射时刻前再 SamplePlayerAim
 *     并刷新倍率；发射事件按 ULyraGameplayAbility_RangedWeapon::OnTargetDataReadyCallback
 *     的顺序取方向偏移（发前）、加热散布（AddSpread）、再 ApplyShot（AddRecoil）。
 *     预览**不**自行递增 ShotIndex、不自行判断进入回正。
 *  4. **长帧走真实路径**：把大 DeltaSeconds 原样交给 Advance，以触发其真实的长帧收尾分支；
 *     不把长帧切碎后声称覆盖了长帧保护。
 *  5. **时间轴重放**：seek 只能从 t=0 用同一快照重放，不允许插值状态伪造。
 *
 * 输出分三路，互不混淆（§10.1）：
 *   - 理论累计 Kick（倍率 1，未经相机上限/回弹/回正）→ Sample.TheoreticalKickPitch/Yaw
 *   - 相机链（CameraOffsetPitch/Yaw/Roll，含插值阶段与饱和）→ Sample.CameraOffset 系列 / Shot.Direction 系列
 *   - 显示合成（ControlRotation + CameraOffset）→ Sample.VisibleAnglePitch/Yaw + Shot.ControlRotation*
 *
 * GC：本类继承 FGCObject，快照由 TStrongObjectPtr 持有，两者都进 AddReferencedObjects。
 *     TSharedPtr 本身不保护 UObject，所以预览对象不能只靠 shared 引用存活。
 */

/** 预览单发模式覆盖。默认跟随资产（见 FLyraRecoilPreviewConfig::bOverrideSingleShotMode）。 */
enum class ELyraRecoilPreviewSingleShotMode : uint8
{
	/** 不覆盖，使用快照资产自己的 SingleShotMode */
	FromProfile = 0,
	InstantWrite,
	Interpolated
};

/**
 * 一条带时间戳的玩家输入/状态变更指令。
 *
 * - AimPitchDegrees / AimYawDegrees：该时刻起玩家 ControlRotation 的绝对值（度）。
 *   `TimeSeconds <= 0` 的条目作为 t=0 初值生效（否则首帧前玩家瞄准为 0）。
 * - PoseState / AimingAlpha：该时刻起生效的姿态与瞄准混合权重（[0,1]，超界会被 Clamp）。
 * - bOverrideGlobalScale / GlobalScale：可选的全局倍率变更（不写 CVar，只影响预览副本计算）。
 */
struct FLyraRecoilPreviewInputEntry
{
	float TimeSeconds = 0.0f;
	float AimPitchDegrees = 0.0f;
	float AimYawDegrees = 0.0f;
	EPoseState PoseState = EPoseState::Standing;
	float AimingAlpha = 0.0f;
	bool bOverrideGlobalScale = false;
	float GlobalScale = 1.0f;
};

/** 一轮连发：从 StartTimeSeconds 起，每 60/RPM 秒一发，共 ShotCount 发。 */
struct FLyraRecoilPreviewFireInput
{
	float StartTimeSeconds = 0.0f;
	int32 ShotCount = 0;
	float RPM = 600.0f;
};

/** 控制器可独立构造的配置（测试可直接填，无需任何资产/世界）。 */
struct FLyraRecoilPreviewConfig
{
	/** 预览发数上限（计划 §10.3 保护值：4096 发）。 */
	static constexpr int32 MaxShots = 4096;
	/** 预览时长上限（计划 §10.3 保护值：120 秒）。 */
	static constexpr float MaxSimulationSeconds = 120.0f;

	/** 基准默认射速（RPM），供 UI 初始值；FireInputs 为空时也用它生成连发。 */
	float RPM = 600.0f;

	/** 默认连发发数（FireInputs 为空时使用）。 */
	int32 ShotCount = 20;

	/**
	 * 外层模拟步长（秒）。默认 1/60；30Hz / 60Hz / 120Hz 基准即 1/30、1/60、1/120。
	 * 发射边界会**额外**把该步拆开，所以射速不受步长影响（§10.2）。
	 */
	float FrameSeconds = 1.0f / 60.0f;

	/** 多轮连发/精确时刻控制。为空时按 RPM + ShotCount 从 t=0 起连发。 */
	TArray<FLyraRecoilPreviewFireInput> FireInputs;

	/** 带时间戳的玩家输入脚本（按时间排序后逐条生效）。 */
	TArray<FLyraRecoilPreviewInputEntry> InputScript;

	/** 起枪角（度）：t=0 时玩家与控制角 Pitch（也写进首帧 ControlRotation）。 */
	float BurstStartAnglePitch = 0.0f;

	/** 起枪角 Yaw（度）。 */
	float BurstStartAngleYaw = 0.0f;

	/** 全局调试倍率（预览副本内生效，不改 CVar）。 */
	float GlobalScale = 1.0f;

	/** 固定种子覆盖模式（写进快照副本，不动原资产）。 */
	ERecoilRandomSeedMode SeedModeOverride = ERecoilRandomSeedMode::Fixed;

	/** 固定种子值（写进快照副本）。 */
	int32 FixedSeedOverride = 20260917;

	/** 单发模式覆盖。FromProfile = 不覆盖。 */
	ELyraRecoilPreviewSingleShotMode SingleShotModeOverride = ELyraRecoilPreviewSingleShotMode::FromProfile;

	/** 停火后继续推进的尾时长（秒），用于观察 Idle / 回正终点。 */
	float TailSeconds = 2.0f;

	/**
	 * 长帧覆盖（秒）。> 0 时把该值作为**最后一次** Advance 的 DeltaSeconds，
	 * 原样交给 FRecoilRuntimeState::Advance（不切碎），用于覆盖长帧收尾分支。
	 * 参见 RunLongFrameStep()。
	 */
	float LongFrameOverrideSeconds = 0.0f;

	/** 是否推进散布链（快照资产开启 bEnableProfileSpread 时才实际生效）。 */
	bool bAdvanceSpread = true;

	/** 散布移动倍率（站立/移动链路中的移动项，预览由参数给出）。 */
	float SpreadMovementMultiplier = 1.0f;

	/** 是否用资产自带的验证器做一次策略校验（结果进 Warnings，不阻断回放）。 */
	bool bValidateProfile = true;

	bool operator==(const FLyraRecoilPreviewConfig& Other) const
	{
		return FMath::IsNearlyEqual(RPM, Other.RPM)
			&& ShotCount == Other.ShotCount
			&& FMath::IsNearlyEqual(FrameSeconds, Other.FrameSeconds)
			&& FireInputs.Num() == Other.FireInputs.Num()
			&& InputScript.Num() == Other.InputScript.Num()
			&& BurstStartAnglePitch == Other.BurstStartAnglePitch
			&& BurstStartAngleYaw == Other.BurstStartAngleYaw
			&& GlobalScale == Other.GlobalScale
			&& SeedModeOverride == Other.SeedModeOverride
			&& FixedSeedOverride == Other.FixedSeedOverride
			&& SingleShotModeOverride == Other.SingleShotModeOverride
			&& TailSeconds == Other.TailSeconds
			&& LongFrameOverrideSeconds == Other.LongFrameOverrideSeconds
			&& bAdvanceSpread == Other.bAdvanceSpread
			&& SpreadMovementMultiplier == Other.SpreadMovementMultiplier
			&& bValidateProfile == Other.bValidateProfile;
	}

	bool operator!=(const FLyraRecoilPreviewConfig& Other) const { return !(*this == Other); }
};

/** 预览快照的构建诊断（曲线是否真的深复制成了内联）。 */
struct FLyraRecoilPreviewSnapshotInfo
{
	/** 快照里被处理的 FRuntimeFloatCurve 属性名（资产上的实际字段名）。 */
	TArray<FName> CurvePropertyNames;

	/** 其中原本指向外部 UCurveFloat、已被内联的属性名。 */
	TArray<FName> InlinedCurvePropertyNames;

	/** 本快照仍引用外部曲线的属性名（入参为空曲线时为原引用，无数据可复制）。 */
	TArray<FName> UnresolvedExternalCurvePropertyNames;

	/** 资产上的关键字段名清单之外的属性个数（仅用于报告，不参与判定）。 */
	int32 CurvePropertyCount = 0;

	bool IsFullyInlined() const { return UnresolvedExternalCurvePropertyNames.Num() == 0; }
};

/** 一个模拟采样点。时间戳可能与相邻点相同（发射事件发生在该时刻）。 */
struct FLyraRecoilPreviewSample
{
	/** 模拟时间（秒）。不是墙钟时间。 */
	float TimeSeconds = 0.0f;

	/** 理论累计 Kick（度）：累加 ComputeShotKick(倍率=1) 的结果，不经过相机上限/回弹/回正。 */
	float TheoreticalKickPitch = 0.0f;
	float TheoreticalKickYaw = 0.0f;

	/** 相机链补间输出（度）。 */
	float CameraOffsetPitch = 0.0f;
	float CameraOffsetYaw = 0.0f;
	float CameraOffsetRoll = 0.0f;

	/** 逻辑累计偏移（度），未补间。用于诊断与逐发比对。 */
	float AccumulatedPitch = 0.0f;
	float AccumulatedYaw = 0.0f;

	/** 玩家控制角（度）。由输入脚本维护，不会被预览的相机偏移反向写入。 */
	float ControlRotationPitch = 0.0f;
	float ControlRotationYaw = 0.0f;

	/** 可见角度（度）= ControlRotation + CameraOffset。仅作显示合成值。 */
	float VisibleAnglePitch = 0.0f;
	float VisibleAngleYaw = 0.0f;

	/** 状态机真实快照。 */
	ERecoilState State = ERecoilState::Idle;
	ERecoilInterpStage InterpStage = ERecoilInterpStage::None;
	int32 ShotIndex = 0;
	float RecoveryPeakPitch = 0.0f;
	float RecoveryPeakYaw = 0.0f;
	float RecoveryCoverPitch = 0.0f;
	float RecoveryCoverYaw = 0.0f;
	float RecoveryProgress = 0.0f;
	float LastSubStepCount = 0.0f;

	/** 本采样点是否紧跟一次发射事件（用于表格定位）。 */
	bool bAfterFireEvent = false;
};

/** 逐发结果。方向偏移按真实发射调用顺序在发射前采集，不与相机链混淆。 */
struct FLyraRecoilPreviewShot
{
	float BeforeCameraOffsetPitch = 0;
	float BeforeCameraOffsetYaw = 0;
	float BeforeAccumulatedPitch = 0;
	int32 ActualShotIndex = 0;
	int32 ShotIndex = 0;
	float TimeSeconds = 0.0f;

	/** GetShotDirectionOffset / ComputeShotKickGated 的输出（度）。这是"这一发往哪偏"，不是弹着点。 */
	float DirectionOffsetPitch = 0.0f;
	float DirectionOffsetYaw = 0.0f;

	/** ApplyShot 记录的本发实际增量（含姿态/瞄准倍率与全局缩放）。 */
	float AppliedKickPitch = 0.0f;
	float AppliedKickYaw = 0.0f;

	/** 理论本发增量（倍率 1），仅用于展示与诊断。 */
	float TheoreticalKickPitch = 0.0f;
	float TheoreticalKickYaw = 0.0f;

	/** 本发**之前**的理论累计 Kick（倍率 1），用于区分"本发贡献"与"累计残留"。 */
	float TheoreticalBeforeKickPitch = 0.0f;
	float TheoreticalBeforeKickYaw = 0.0f;

	/** 本轮连发起点偏移（度）。新一轮由 ApplyShot 内部按当前可见偏移锁定。 */
	float BurstStartPitchOffset = 0.0f;
	float BurstStartYawOffset = 0.0f;

	/** 开火瞬间的玩家控制角（度）。 */
	float ControlRotationPitch = 0.0f;
	float ControlRotationYaw = 0.0f;

	/** 开火瞬间的可见角度（度）= ControlRotation + CameraOffset。 */
	float VisibleAnglePitch = 0.0f;
	float VisibleAngleYaw = 0.0f;

	EPoseState PoseState = EPoseState::Standing;
	float PoseMultiplier = 1.0f;
	float GlobalScale = 1.0f;
	float SpreadAngle = 0.0f;

	/** 是否为本轮连发的第一发（ApplyShot 判定进入新一轮）。 */
	bool bStartedNewBurst = false;
};

/**
 * 隔离预览快照。
 *
 * 计划 §10.1：创建原 Profile 的隔离临时副本作为预览输入，深复制有效外部曲线到临时内联曲线，
 * 以固定本次回放快照；只在副本上覆盖固定种子等预览选项。
 */
struct FLyraRecoilPreviewSnapshot
{
	/**
	 * 隔离副本（Transient 包内）。nullptr 表示构建失败。
	 * 不使用 UPROPERTY：本类型是普通 C++ 结构体，GC 引用由持有者显式登记
	 * （FLyraRecoilPreviewController::AddReferencedObjects / TStrongObjectPtr）。
	 */
	TObjectPtr<ULyraRecoilProfile> Profile = nullptr;

	/** 深度内联的外部曲线数量与诊断。 */
	FLyraRecoilPreviewSnapshotInfo Info;

	/** 构建时的原资产路径，仅用于报告。 */
	FString SourceProfilePath;

	/** 构建失败原因（为空表示成功）。 */
	FString Error;

	bool IsValid() const { return Profile != nullptr && Error.IsEmpty(); }
};

/** 采样序列的取样方式。 */
enum class ELyraRecoilPreviewSampleMode : uint8
{
	/**
	 * 每个事件边界都存样本（默认）：每段 Advance 之后一个样本，
	 * 发射事件之后紧跟一个 `bAfterFireEvent = true` 的样本。
	 */
	EveryEvent,
	/** 只保留整数帧边界样本（发射即刻样本不再单独入表），用于"同时间步直接调用"的逐点比对。 */
	FrameBoundaryOnly
};

/**
 * 确定性回放控制器。可在没有 UObject / Slate / 世界的情况下独立构造并运行，
 * 供自动化测试直接调用（见 LyraRecoilEditorPreviewTest.cpp）。
 *
 * 注意：这不是 UObject，构造它不会创建资产；快照由它自己持有并做 GC 引用。
 */
class FLyraRecoilPreviewController : public FGCObject
{
public:

	FLyraRecoilPreviewController();
	virtual ~FLyraRecoilPreviewController() override;

	// 非可拷贝（持有 GC 引用与内部累积状态）。
	FLyraRecoilPreviewController(const FLyraRecoilPreviewController&) = delete;
	FLyraRecoilPreviewController& operator=(const FLyraRecoilPreviewController&) = delete;

	//~ FGCObject interface
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;
	//~ End of FGCObject interface

	// ---------------------------------------------------------------------
	// 配置
	// ---------------------------------------------------------------------

	const FLyraRecoilPreviewConfig& GetConfig() const { return Config; }

	/** 设置配置并让缓存失效（下次 Run/AdvanceBudget 从 t=0 用新参数重算）。 */
	void SetConfig(const FLyraRecoilPreviewConfig& InConfig);

	/** 取样模式（默认每个事件边界）。修改后同样使结果失效。 */
	void SetSampleMode(ELyraRecoilPreviewSampleMode InMode);
	ELyraRecoilPreviewSampleMode GetSampleMode() const { return SampleMode; }

	/** 是否在帧边界额外记录样本（供 30/60/120Hz 基准大表比对）。 */
	bool IsFrameBoundarySamplingEnabled() const { return bRecordFrameBoundarySamples; }
	void SetFrameBoundarySamplingEnabled(bool bEnabled);

	// ---------------------------------------------------------------------
	// 快照
	// ---------------------------------------------------------------------

	/**
	 * 用当前 Profile 重建隔离快照并清空结果。
	 *
	 * @param InProfile              原资产（不会被修改；可为 nullptr，此时清空所有结果）
	 * @param OutError               失败原因
	 * @param OutInlinedCurveNames   实际被内联的外部曲线属性名
	 * @return 快照可用返回 true
	 */
	bool RefreshProfile(
		ULyraRecoilProfile* InProfile,
		FString& OutError,
		TArray<FName>& OutInlinedCurveNames);

	/** 当前快照（可能为 nullptr）。 */
	const FLyraRecoilPreviewSnapshot& GetSnapshot() const { return Snapshot; }

	/** 当前快照的隔离副本（可能为 nullptr）。只读用途；预览从不写回原资产。 */
	const ULyraRecoilProfile* GetSnapshotProfile() const { return Snapshot.Profile; }

	/** 构建隔离快照（含全部 FRuntimeFloatCurve 深内联）。不修改原资产。 */
	static FLyraRecoilPreviewSnapshot BuildIsolatedSnapshot(ULyraRecoilProfile* SourceProfile);

	// ---------------------------------------------------------------------
	// 回放（预算增量执行；编辑器主线程）
	// ---------------------------------------------------------------------

	/**
	 * 完整执行一次回放（内部仍按可中断的原子步推进，预算不限）。
	 * @return 结果可用返回 true
	 */
	bool Run();

	/**
	 * 按毫秒预算增量执行：每次调用最多消耗 BudgetMilliseconds 的**墙钟**时间，
	 * 但只落在原子步边界上，保证与不设预算的 Run() 产生完全相同的事件序列与逐发结果。
	 *
	 * @param BudgetMilliseconds  本帧允许的执行时间（<=0 时按 1 次原子步处理）
	 * @param bOutCompleted       本次调用是否已经把整个时间轴跑完
	 * @return 结果可用返回 true
	 */
	bool AdvanceBudget(double BudgetMilliseconds, bool& bOutCompleted);

	/**
	 * 长帧步骤：把 config.LongFrameOverrideSeconds 作为**单个** DeltaSeconds 交给 Advance，
	 * 原样触发 FRecoilRuntimeState 内部的长帧保护（不切碎、不伪覆盖）。
	 * 需要该项 > 0；执行后把结果追加到样本表并把时间轴收尾（后续普通步不再推进）。
	 * 典型用法：先 Run() 打完连发，再在停火段调用本函数，覆盖 Settle → Drop 被跳过的分支。
	 */
	bool RunLongFrameStep();

	/** 是否已经跑完整个时间轴（含尾时长）。 */
	bool IsSimulationComplete() const { return bSimulationComplete; }

	/** 已经执行的原子步数（诊断用）。 */
	int32 GetExecutedStepCount() const { return ExecutedStepCount; }

	/** 从最后一次 Run/AdvanceBudget 起，实际写入的样本数。 */
	int32 GetSampleCount() const { return Samples.Num(); }

	/** 是否因为限额（发数 / 时长）被截断。 */
	bool IsTruncatedByLimit() const { return bTruncatedByLimit; }

	/** 截断原因的可读描述。 */
	FString GetTruncationReason() const { return TruncationReason; }

	// ---------------------------------------------------------------------
	// 结果查询
	// ---------------------------------------------------------------------

	const TArray<FLyraRecoilPreviewSample>& GetSamples() const { return Samples; }
	const TArray<FLyraRecoilPreviewShot>& GetShots() const { return Shots; }
	const TArray<FString>& GetWarnings() const { return Warnings; }
	const TArray<int32>& GetEventSampleIndices() const { return EventSampleIndices; }

	/** 时间轴的模拟总时长（秒）。 */
	float GetSimulationDurationSeconds() const { return PlannedDurationSeconds; }

	/** 时间轴上实际记录到的最晚时间（秒）。 */
	float GetLastSampleTimeSeconds() const { return Samples.Num() > 0 ? Samples.Last().TimeSeconds : 0.0f; }

	/** 发射时刻列表（模拟时间，秒）。 */
	const TArray<float>& GetFireTimes() const { return FireTimes; }

	/** 在采样表中取不晚于 InTime 的最后一个样本下标。 */
	int32 FindSampleIndexAtTime(float InTime) const;

	/**
	 * 取某个采样点上的可见角度（线性插值仅用于**显示**）。必须显式确认 is the display path：
	 * 控制器内部的状态推进永远走重放，不读这个函数。
	 */
	bool GetVisibleAngleAtTime(float InTime, float& OutPitch, float& OutYaw) const;

	// ---------------------------------------------------------------------
	// 时间线回放（只影响播放位置，不改变模拟步长）
	// ---------------------------------------------------------------------

	/** 跳转到指定模拟时间。**不**插值状态：内部按值记账，由持有方的播放循环决定是否重放。 */
	void SeekToTime(float InSimulationTimeSeconds);

	float GetPlayheadTimeSeconds() const { return PlayheadTimeSeconds; }

	/** 按播放倍率推进播放位置（墙钟 → 播放位置）。不改变模拟时步。 */
	void AdvancePlayhead(float InWallClockDeltaSeconds, float InPlaybackSpeed);

	// ---------------------------------------------------------------------
	// 缓存
	// ---------------------------------------------------------------------

	/**
	 * 缓存是否仍然有效：等于"上次完整回放所用的配置令牌 == 当前令牌"。
	 * 任何配置 / 输入脚本 / 快照变化都会让它失效（计划 §10.3 结尾）。
	 */
	bool IsResultCacheValid() const;

	/** 主动让缓存失效（外部改了快照或配置后调用）。 */
	void InvalidateCache();

	/** 当前配置令牌（含快照修订）。用于测试核对失效行为。 */
	uint32 ComputeRequestToken() const;

private:

	// ---------------------------------------------------------------------
	// 模拟内部
	// ---------------------------------------------------------------------

	/** 只读一次的模拟计划（事件表 + 限额检查）。 */
	struct FSimulationPlan
	{
		TArray<float> FireTimes;
		float PlannedDuration = 0.0f;
		bool bTruncated = false;
		FString TruncationReason;
	};

	/** 解析输入脚本 → 事件表。 */
	void BuildPlan(FSimulationPlan& OutPlan) const;

	/** 复位内部模拟状态（保留快照与配置）。 */
	void ResetSimulationState();

	/** 执行一个原子步：整数帧边界 + 帧内发射拆分。返回执行的 Advance 调用次数。 */
	int32 RunOneAtomicStep();
	/** 当前时刻的输入脚本状态。 */
	void EvaluateInputAtTime(float InTimeSeconds, float& OutAimPitch, float& OutAimYaw,
		EPoseState& OutPose, float& OutAimingAlpha, float& OutGlobalScale) const;

	/** 追加一个采样点。 */
	void RecordSample(float InTimeSeconds, bool bAfterFire, ELyraRecoilPreviewSampleMode Mode);

	/** 执行一次发射事件（严格按真实调用顺序）。 */
	void FireShot(float InTimeSeconds);

	/** 写预览选项到快照副本（固定种子 / 单发模式）；从不写原资产。 */
	void ApplyPreviewOptionsToSnapshot();

	/** 记录策略校验结果。 */
	void ValidateSnapshotProfile();
	bool ValidateInputs();

private:

	// --- 配置与快照 ---
	FLyraRecoilPreviewConfig Config;
	FLyraRecoilPreviewSnapshot Snapshot;
	ERecoilSingleShotMode SourceSingleShotMode = ERecoilSingleShotMode::InstantWrite;
	bool bSnapshotValid = false;
	bool bInputsValid = true;
	bool bNumericalFailure = false;

	/** 快照的强引用（与 Snapshot.Profile 指向同一对象，双保险）。 */
	TStrongObjectPtr<ULyraRecoilProfile> SnapshotStrongRef;

	/** 快照构建时原资产的路径，用于日志与报告。 */
	FString SnapshotSourcePath;

	/** 结果缓存的令牌。 */
	uint32 CachedRequestToken = 0;

	/** 结果是否需要重算。 */
	bool bResultDirty = true;

	// --- 运行时状态（现有算法，原样使用） ---
	FRecoilRuntimeState RecoilState;

	// --- 模拟时间轴 ---
	TArray<float> FireTimes;
	float PlannedDurationSeconds = 0.0f;

	/** 已推进到的模拟时间。 */
	float CurrentTimeSeconds = 0.0f;

	/** 下一次发射在 FireTimes 中的下标。 */
	int32 NextFireIndex = 0;

	/** 当前所在的外层帧编号（用于帧边界取样与 30/60/120Hz 基准）。 */
	int32 FrameIndex = 0;

	/** 上一次原子步是否停在了帧内（预算续跑用）。 */
	bool bFrameInProgress = false;

	int32 ExecutedStepCount = 0;
	bool bSimulationComplete = false;
	bool bTruncatedByLimit = false;
	FString TruncationReason;

	/** 长帧覆盖是否已经用掉。 */
	bool bLongFrameConsumed = false;

	/** 内部模拟状态是否已按当前计划复位。 */
	bool bReplayInitialized = false;

	/** 本次调用实际执行的 Advance 次数（诊断）。 */
	int32 LastAdvanceCallCount = 0;

	// --- 结果 ---
	TArray<FLyraRecoilPreviewSample> Samples;
	TArray<FLyraRecoilPreviewShot> Shots;
	TArray<FString> Warnings;
	TArray<int32> EventSampleIndices;

	/** 理论累计 Kick（倍率 1）。 */
	float TheoreticalKickPitch = 0.0f;
	float TheoreticalKickYaw = 0.0f;

	/** 本发之前的理论累计值，用于逐发记录。 */
	float TheoreticalBeforeShotPitch = 0.0f;
	float TheoreticalBeforeShotYaw = 0.0f;

	// --- 播放位置（墙钟，不参与模拟） ---
	float PlayheadTimeSeconds = 0.0f;

	/** 取样模式（默认每个事件边界）。 */
	ELyraRecoilPreviewSampleMode SampleMode = ELyraRecoilPreviewSampleMode::EveryEvent;

	/** 是否在整数帧边界额外记录样本（默认开，供画时间曲线）。 */
	bool bRecordFrameBoundarySamples = true;
};
