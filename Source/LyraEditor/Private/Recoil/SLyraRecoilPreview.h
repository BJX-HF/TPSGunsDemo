// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

#include "Recoil/LyraRecoilPreviewController.h"

class FActiveTimerHandle;
class ULyraRecoilProfile;

/**
 * P4 数值预览面板（开发计划 §10）。
 *
 * 职责边界：
 *  - 只展示 / 驱动 FLyraRecoilPreviewController 的结果，不含任何后坐力数学；
 *  - 时间图（Slate 线段）、发射事件表、逐发方向偏移表、参数编辑区、播放控制；
 *  - 参数全部可调（射速、发数、步长、尾时长、姿态、瞄准 Alpha、全局倍率、起枪角、
 *    固定种子、单发模式覆盖、输入脚本、两轮发射）；
 *  - 播放用 ActiveTimer 驱动，**只推进播放位置**，不改变模拟时步；
 *  - 关闭时 UnRegisterActiveTimer 并清空委托（计划 §9「预览清理」）。
 *
 * 集成入口：
 *  - `SLATE_ARGUMENT(ULyraRecoilProfile*, Profile)`；
 *  - `RefreshProfile()` 供工具包在资产变化后调用（内部重建隔离快照，不写原资产）。
 */
class SLyraRecoilPreview : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SLyraRecoilPreview)
		: _Profile(nullptr)
	{}
		/** 要预览的 Profile 资产（不会被修改；外部可先用 Controller 自身刷新）。 */
		SLATE_ARGUMENT(ULyraRecoilProfile*, Profile)
	SLATE_END_ARGS()

	SLyraRecoilPreview();
	virtual ~SLyraRecoilPreview() override;

	void Construct(const FArguments& InArgs);

	//~ SWidget interface
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;
	//~ End of SWidget interface

	/** 资产变化后重建隔离快照并从头重算（不写原资产、不标脏）。 */
	void RefreshProfile();

	/** 换一个 Profile 并刷新。 */
	void SetProfile(ULyraRecoilProfile* InProfile);

	/** 预览控制器（测试与工具包可直接访问）。构造后始终有效。 */
	FLyraRecoilPreviewController& GetController() const { return Controller.Get(); }

	/** 是否正在播放。 */
	bool IsPlaying() const { return bIsPlaying; }

	/** 播放 / 暂停。 */
	void SetPlaying(bool bInPlaying);

	/** 重置回放（等价于点击"重置"）。 */
	void ResetReplay();

	/** 关闭清理：解绑 Timer 与委托。析构时自动调用，幂等。 */
	void Cleanup();

private:

	/** 确保时间曲线已经按当前配置算完（预算增量执行，编辑器主线程）。 */
	void EnsureResults() const;

	/** 播放用的 ActiveTimer 回调。 */
	EActiveTimerReturnType HandleActiveTimer(double InCurrentTime, float InDeltaTime);

	/** 当前单发模式的可读名。 */
	FText GetSingleShotModeText() const;

	/** 当前姿态的可读名。 */
	FText GetPoseText() const;

	/** 帧步长（秒）→ 频率文本。 */
	FText GetFrameRateText() const;

	/** 播放头文本。 */
	FText GetPlayheadText() const;

	/** 生成参数编辑区。 */
	TSharedRef<class SWidget> BuildParameterPanel();

	/** 生成发射事件与诊断表。 */
	TSharedRef<class SWidget> BuildEventTable();

	/** 姿态枚举 → 可读文本（不依赖控件级 lambda）。 */
	static FText GetPoseTextForState(EPoseState InState);

	/** 把当前输入脚本的姿态改为 InState（脚本为空时补一条 t=0 的条目）。 */
	void SetPoseState(EPoseState InState);

	/** 把当前输入脚本的瞄准 Alpha 改为 InValue（脚本为空时补一条 t=0 的条目）。 */
	void SetAimingAlpha(float InValue);

	/** 单发模式覆盖（写进隔离副本，不动原资产）。 */
	void SetSingleShotModeOverride(ELyraRecoilPreviewSingleShotMode InMode);

	/** 单轮 / 两轮发射脚本。 */
	void ApplyFireScript(bool bTwoBursts);

	/**
	 * 按面板上的"输入脚本参数"重建 InputScript：
	 * 起枪角 → 压枪（可选）→ 保持到最后一发。姿态 / 瞄准 Alpha 全程生效。
	 * 任何脚本参数变化都走这里，避免多个入口互相覆盖。
	 */
	void RebuildInputScript();

	/** 按当前每轮发数 / 轮间隔重建单轮或两轮发射时刻表。 */
	void RebuildFireInputs(bool bTwoBursts);

private:

	/** 预览控制器（非 UObject，可独立构造与 Run/AdvanceBudget）。始终有效。 */
	TSharedRef<FLyraRecoilPreviewController> Controller;
	TSharedPtr<class SBorder> CurveArea;

	/** 目标资产（弱引用：不延寿 UObject）。 */
	TWeakObjectPtr<ULyraRecoilProfile> TargetProfile;

	/** 播放状态。 */
	bool bIsPlaying = false;

	/** 本帧用来推进播放的墙钟时间（由 ActiveTimer 提供，避免重复计量）。 */
	double LastPlaybackTickTime = 0.0;

	/** 播放倍率（只影响播放墙钟速度，不影响模拟时步）。 */
	float PlaybackSpeed = 1.0f;

	// --- 输入脚本参数（用户可调；由 RebuildInputScript 统一生成 InputScript） ---
	float DownPullDegrees = 0.0f;
	float DownPullStartSeconds = 0.02f;
	float DownPullHoldSeconds = 0.5f;
	EPoseState ScriptPoseState = EPoseState::Standing;
	float ScriptAimingAlpha = 0.0f;

	// --- 多轮排布参数 ---
	int32 ShotsPerBurst = 5;
	float BurstGapSeconds = 0.35f;
	bool bTwoBursts = false;

	/** ActiveTimer 句柄：关闭时必须注销。 */
	TSharedPtr<FActiveTimerHandle> ActiveTimerHandle;

	/** 最近一次刷新快照的错误（空 = 正常）。 */
	FString LastRefreshError;

	/** 最近一次快照实际内联的外部曲线属性名。 */
	TArray<FName> LastInlinedCurves;

	/** 无资产时的占位输出（RefreshProfile 的 out 参数需要一个可写对象）。 */
	FString WarningsPlaceholder;
	TArray<FName> InlinedPlaceholder;
};
