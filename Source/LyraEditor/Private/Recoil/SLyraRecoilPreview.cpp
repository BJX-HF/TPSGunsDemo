// Copyright Epic Games, Inc. All Rights Reserved.

#include "Recoil/SLyraRecoilPreview.h"

#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "Weapons/Recoil/LyraRecoilProfile.h"

#define LOCTEXT_NAMESPACE "LyraRecoilPreview"

namespace LyraRecoilPreviewWidget
{
	/** 曲线画布颜色。 */
	const FLinearColor TheoreticalColor(0.95f, 0.75f, 0.20f, 1.0f);
	const FLinearColor CameraPitchColor(0.30f, 0.75f, 1.00f, 1.0f);
	const FLinearColor CameraYawColor(0.35f, 1.00f, 0.55f, 1.0f);
	const FLinearColor VisiblePitchColor(1.00f, 0.45f, 0.35f, 1.0f);
	const FLinearColor ControlPitchColor(0.80f, 0.40f, 1.0f, 1.0f);
	const FLinearColor GridColor(1.0f, 1.0f, 1.0f, 0.10f);
	const FLinearColor PlayheadColor(1.0f, 1.0f, 1.0f, 0.55f);

	/** 曲线画布左右留白（像素）。 */
	constexpr float CurvePanelPadding = 8.0f;
}

SLyraRecoilPreview::SLyraRecoilPreview()
	: Controller(MakeShared<FLyraRecoilPreviewController>())
{
}

SLyraRecoilPreview::~SLyraRecoilPreview()
{
	// 关闭清理：注销 ActiveTimer，禁止后台回调访问已销毁的 Slate（计划 §9）。
	Cleanup();
}

void SLyraRecoilPreview::Cleanup()
{
	if (ActiveTimerHandle.IsValid())
	{
		UnRegisterActiveTimer(ActiveTimerHandle.ToSharedRef());
		ActiveTimerHandle.Reset();
	}
	bIsPlaying = false;
}

void SLyraRecoilPreview::Construct(const FArguments& InArgs)
{
	TargetProfile = InArgs._Profile;

	// 自己刷新一次快照：主 agent 之后可再接入资产外部通知（这里不依赖它）。
	RefreshProfile();

	// 输入脚本 / 发射计划先按默认参数生成一次，保证打开面板就有可播放的结果。
	RebuildFireInputs(false);
	RebuildInputScript();

	ChildSlot
	[
		// Keep plot and parameter rows separate when high DPI reduces the tab height.
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
		SNew(SBox).MinDesiredHeight(360.0f)
		[
		SNew(SVerticalBox)

		// --- 播放控制 ---
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f)
			[
				SNew(SButton)
				.Text(this, &SLyraRecoilPreview::GetPlayheadText)
				.ToolTipText(LOCTEXT("PlayPauseTip", "播放 / 暂停（只推进播放位置，不改变模拟时步）"))
				.OnClicked_Lambda([this]()
				{
					SetPlaying(!bIsPlaying);
					return FReply::Handled();
				})
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("ResetButton", "重置回放"))
				.ToolTipText(LOCTEXT("ResetTip", "丢弃当前结果，从 t=0 用同一快照重放"))
				.OnClicked_Lambda([this]()
				{
					ResetReplay();
					return FReply::Handled();
				})
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("RefreshButton", "刷新快照"))
				.ToolTipText(LOCTEXT("RefreshTip", "从当前 Profile 重新构建隔离快照（含外部曲线深内联）"))
				.OnClicked_Lambda([this]()
				{
					RefreshProfile();
					return FReply::Handled();
				})
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("PlaybackSpeedLabel", "播放倍率"))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f)
			[
				SNew(SNumericEntryBox<float>)
				.AllowSpin(true)
				.MinValue(0.0f)
				.MaxValue(8.0f)
				.Delta(0.25f)
				.MinDesiredValueWidth(48.0f)
				.Value_Lambda([this]() { return TOptional<float>(PlaybackSpeed); })
				.OnValueChanged_Lambda([this](float NewValue)
				{
					PlaybackSpeed = FMath::Clamp(NewValue, 0.0f, 8.0f);
				})
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(2.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					return FText::FromString(FString::Printf(
						TEXT("模拟 %.3f s / 计划 %.3f s   采样 %d   发射 %d   原子步 %d"),
						Controller->GetLastSampleTimeSeconds(),
						Controller->GetSimulationDurationSeconds(),
						Controller->GetSampleCount(),
						Controller->GetShots().Num(),
						Controller->GetExecutedStepCount()));
				})
			]
		]

		// --- 主区：曲线 + 事件表 ---
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(4.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)

			+ SSplitter::Slot()
			.Value(0.62f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(2.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(LOCTEXT("Legend",
						"曲线（度）：理论 Kick 金 / Camera Pitch 蓝 / Camera Yaw 绿 / 可见 Pitch 红 / Control Pitch 紫。"
						"  数据表：逐发方向偏移与状态诊断。"))
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					// Plot has its own observed geometry and clip rectangle.
					SAssignNew(CurveArea, SBorder)
					.Padding(0.0f)
					[
						SNullWidget::NullWidget
					]
				]
			]

			+ SSplitter::Slot()
			.Value(0.38f)
			[
				BuildEventTable()
			]
		]

		// --- 参数区 ---
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SBox).HeightOverride(140)[SNew(SScrollBox)+SScrollBox::Slot()[BuildParameterPanel()]]
		]
		]
		]
	];

	// 播放由 ActiveTimer 驱动：周期 0 = 每帧一次。
	ActiveTimerHandle = RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SLyraRecoilPreview::HandleActiveTimer));
}

// ---------------------------------------------------------------------------
// 快照 / 结果
// ---------------------------------------------------------------------------

void SLyraRecoilPreview::SetProfile(ULyraRecoilProfile* InProfile)
{
	TargetProfile = InProfile;
	RefreshProfile();
}

void SLyraRecoilPreview::RefreshProfile()
{
	// 保留用户已经调过的预览参数（射速、发数、姿态、脚本等），只换快照。
	ULyraRecoilProfile* Profile = TargetProfile.Get();
	if (Profile == nullptr)
	{
		// 没有资产也要能显示参数区：退化为"空结果"，不伪造数据。
		Controller->RefreshProfile(nullptr, WarningsPlaceholder, InlinedPlaceholder);
		return;
	}

	FString Error;
	TArray<FName> InlinedCurves;
	if (!Controller->RefreshProfile(Profile, Error, InlinedCurves))
	{
		LastRefreshError = Error.IsEmpty() ? TEXT("无法构建隔离快照。") : Error;
		return;
	}

	LastRefreshError.Reset();
	LastInlinedCurves = InlinedCurves;

	// 重建后立刻按当前参数算一遍，保证面板与表格都有数据。
	// Simulation is incrementally computed by the active timer.
}

void SLyraRecoilPreview::ResetReplay()
{
	Controller->SeekToTime(0.0f);
	// Simulation is incrementally computed by the active timer.
	Controller->SeekToTime(0.0f);
}

void SLyraRecoilPreview::SetPlaying(bool bInPlaying)
{
	bIsPlaying = bInPlaying;
}

// ---------------------------------------------------------------------------
// 播放
// ---------------------------------------------------------------------------

EActiveTimerReturnType SLyraRecoilPreview::HandleActiveTimer(double InCurrentTime, float InDeltaTime)
{
	// 先按预算增量把模拟算完（编辑器主线程，有限预算）。
	EnsureResults();

	if (bIsPlaying)
	{
		// ★ 墙钟只推进播放位置；模拟时步由 Controller 自己按配置决定。
		Controller->AdvancePlayhead(InDeltaTime, PlaybackSpeed);

		if (Controller->GetPlayheadTimeSeconds() >= Controller->GetSimulationDurationSeconds())
		{
			bIsPlaying = false;
		}
	}

	return EActiveTimerReturnType::Continue;
}

void SLyraRecoilPreview::EnsureResults() const
{
	// 预算增量执行：每帧最多 4ms，避免大图阻塞编辑器主线程。
	bool bCompleted = false;
	Controller->AdvanceBudget(/*BudgetMilliseconds=*/4.0, bCompleted);
}

void SLyraRecoilPreview::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	LastPlaybackTickTime = InCurrentTime;
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

int32 SLyraRecoilPreview::OnPaint(
	const FPaintArgs& Args,
	const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements,
	int32 LayerId,
	const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	// Painting only reads cached results; no simulation work on the paint path.

	const int32 ResultLayer = SCompoundWidget::OnPaint(
		Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);

	const TArray<FLyraRecoilPreviewSample>& Samples = Controller->GetSamples();
	if (Samples.Num() < 2)
	{
		return ResultLayer;
	}

	if (!CurveArea) return ResultLayer;
	// Cached/tick geometry uses desktop coordinates. Draw elements require paint
	// space relative to this window, especially for floating editors and real DPI.
	const FGeometry PlotGeometry=CurveArea->GetPaintSpaceGeometry();
	const FVector2f PanelSize = FVector2f(PlotGeometry.GetLocalSize());
	if (PanelSize.X<2 || PanelSize.Y<2) return ResultLayer;
	const float DrawWidth = FMath::Max(PanelSize.X - 2.0f * LyraRecoilPreviewWidget::CurvePanelPadding, 1.0f);
	const float DrawHeight = FMath::Max(PanelSize.Y - 2.0f * LyraRecoilPreviewWidget::CurvePanelPadding, 1.0f);

	const float Duration = FMath::Max(Controller->GetSimulationDurationSeconds(), KINDA_SMALL_NUMBER);

	// 纵轴范围：理论累计 Kick / 相机偏移 / 可见角度一起取，留 15% 余量。
	float MinValue = 0.0f;
	float MaxValue = 0.0f;
	for (const FLyraRecoilPreviewSample& Sample : Samples)
	{
		MinValue = FMath::Min(MinValue, FMath::Min3(
			Sample.CameraOffsetPitch, Sample.CameraOffsetYaw, Sample.VisibleAnglePitch));
		MinValue=FMath::Min(MinValue,FMath::Min(Sample.TheoreticalKickPitch,Sample.ControlRotationPitch));
		MaxValue = FMath::Max(MaxValue, FMath::Max3(
			Sample.CameraOffsetPitch, Sample.CameraOffsetYaw, Sample.VisibleAnglePitch));
		MaxValue=FMath::Max(MaxValue,FMath::Max(Sample.TheoreticalKickPitch,Sample.ControlRotationPitch));
	}
	for (const auto& Sample:Samples) MaxValue=FMath::Max(MaxValue,Sample.TheoreticalKickPitch);
	MinValue = FMath::Min(MinValue, 0.0f);
	MaxValue = FMath::Max(MaxValue, 1.0f);
	const float Range = FMath::Max(MaxValue - MinValue, KINDA_SMALL_NUMBER) * 1.15f;

	const float X0 = LyraRecoilPreviewWidget::CurvePanelPadding;
	const float Y0 = LyraRecoilPreviewWidget::CurvePanelPadding;

	auto TimeToX = [&](float InTime) { return X0 + (InTime / Duration) * DrawWidth; };
	auto ValueToY = [&](float InValue)
	{
		const float Normalized = (InValue - MinValue) / Range;
		return Y0 + DrawHeight - FMath::Clamp(Normalized, 0.0f, 1.0f) * DrawHeight;
	};

	const FPaintGeometry PaintGeometry = PlotGeometry.ToPaintGeometry();
	// The custom draw occurs after child painting, so reapply the scroll viewport clip.
	OutDrawElements.PushClip(FSlateClippingZone(AllottedGeometry));
	OutDrawElements.PushClip(FSlateClippingZone(PlotGeometry));
	int32 CurrentLayer = ResultLayer + 1;

	// --- 网格与零线 ---
	{
		TArray<FVector2f> ZeroLine;
		const float ZeroY = ValueToY(0.0f);
		ZeroLine.Add(FVector2f(X0, ZeroY));
		ZeroLine.Add(FVector2f(X0 + DrawWidth, ZeroY));
		FSlateDrawElement::MakeLines(
			OutDrawElements, CurrentLayer, PaintGeometry, ZeroLine,
			ESlateDrawEffect::None, LyraRecoilPreviewWidget::GridColor, true, 1.0f);

		for (int32 Step = 1; Step < 4; ++Step)
		{
			const float GridX = X0 + DrawWidth * (static_cast<float>(Step) / 4.0f);
			TArray<FVector2f> GridLine;
			GridLine.Add(FVector2f(GridX, Y0));
			GridLine.Add(FVector2f(GridX, Y0 + DrawHeight));
			FSlateDrawElement::MakeLines(
				OutDrawElements, CurrentLayer, PaintGeometry, GridLine,
				ESlateDrawEffect::None, LyraRecoilPreviewWidget::GridColor, true, 1.0f);
		}
		++CurrentLayer;
	}

	// --- 四条曲线：理论累计 Kick / CameraOffset Pitch / CameraOffset Yaw / 可见 Pitch ---
	auto DrawCurve = [&](TArray<FVector2f>& Points, const FLinearColor& Color, float Thickness)
	{
		if (Points.Num() >= 2)
		{
			FSlateDrawElement::MakeLines(
				OutDrawElements, CurrentLayer, PaintGeometry, Points,
				ESlateDrawEffect::None, Color, true, Thickness);
		}
	};

	TArray<FVector2f> TheoreticalPoints;
	TArray<FVector2f> CameraPitchPoints;
	TArray<FVector2f> CameraYawPoints;
	TArray<FVector2f> VisiblePitchPoints;
	TArray<FVector2f> ControlPitchPoints;
	TheoreticalPoints.Reserve(Samples.Num());
	CameraPitchPoints.Reserve(Samples.Num());
	CameraYawPoints.Reserve(Samples.Num());
	VisiblePitchPoints.Reserve(Samples.Num());
	ControlPitchPoints.Reserve(Samples.Num());

	for (const FLyraRecoilPreviewSample& Sample : Samples)
	{
		const float X = TimeToX(Sample.TimeSeconds);
		TheoreticalPoints.Add(FVector2f(X, ValueToY(Sample.TheoreticalKickPitch)));
		CameraPitchPoints.Add(FVector2f(X, ValueToY(Sample.CameraOffsetPitch)));
		CameraYawPoints.Add(FVector2f(X, ValueToY(Sample.CameraOffsetYaw)));
		VisiblePitchPoints.Add(FVector2f(X, ValueToY(Sample.VisibleAnglePitch)));
		ControlPitchPoints.Add(FVector2f(X, ValueToY(Sample.ControlRotationPitch)));
	}

	DrawCurve(TheoreticalPoints, LyraRecoilPreviewWidget::TheoreticalColor, 2.0f);
	DrawCurve(CameraPitchPoints, LyraRecoilPreviewWidget::CameraPitchColor, 1.5f);
	DrawCurve(CameraYawPoints, LyraRecoilPreviewWidget::CameraYawColor, 1.5f);
	DrawCurve(VisiblePitchPoints, LyraRecoilPreviewWidget::VisiblePitchColor, 1.0f);
	DrawCurve(ControlPitchPoints, LyraRecoilPreviewWidget::ControlPitchColor, 1.0f);
	++CurrentLayer;

	// --- 播放头 ---
	{
		const float PlayheadX = TimeToX(Controller->GetPlayheadTimeSeconds());
		TArray<FVector2f> PlayheadLine;
		PlayheadLine.Add(FVector2f(PlayheadX, Y0));
		PlayheadLine.Add(FVector2f(PlayheadX, Y0 + DrawHeight));
		FSlateDrawElement::MakeLines(
			OutDrawElements, CurrentLayer, PaintGeometry, PlayheadLine,
			ESlateDrawEffect::None, LyraRecoilPreviewWidget::PlayheadColor, true, 1.0f);
		++CurrentLayer;
	}

	OutDrawElements.PopClip();
	OutDrawElements.PopClip();
	return CurrentLayer;
}

// ---------------------------------------------------------------------------
// 面板构建
// ---------------------------------------------------------------------------

FText SLyraRecoilPreview::GetPlayheadText() const
{
	return bIsPlaying
		? LOCTEXT("PauseButton", "暂停")
		: LOCTEXT("PlayButton", "播放");
}

FText SLyraRecoilPreview::GetSingleShotModeText() const
{
	switch (Controller->GetConfig().SingleShotModeOverride)
	{
	case ELyraRecoilPreviewSingleShotMode::InstantWrite:
		return LOCTEXT("ModeInstant", "InstantWrite");
	case ELyraRecoilPreviewSingleShotMode::Interpolated:
		return LOCTEXT("ModeInterp", "Interpolated");
	case ELyraRecoilPreviewSingleShotMode::FromProfile:
	default:
		return LOCTEXT("ModeFromProfile", "跟随资产");
	}
}

FText SLyraRecoilPreview::GetPoseText() const
{
	// 与面板上的"脚本姿态"保持一致（InputScript 由 RebuildInputScript 统一生成）。
	switch (ScriptPoseState)
	{
	case EPoseState::Crouching:
		return LOCTEXT("PoseCrouch", "蹲伏");
	case EPoseState::JumpingOrFalling:
		return LOCTEXT("PoseAir", "空中");
	case EPoseState::Standing:
	default:
		return LOCTEXT("PoseStand", "站立");
	}
}

FText SLyraRecoilPreview::GetFrameRateText() const
{
	const float FrameSeconds = FMath::Max(Controller->GetConfig().FrameSeconds, KINDA_SMALL_NUMBER);
	return FText::FromString(FString::Printf(TEXT("%.1f Hz"), 1.0f / FrameSeconds));
}

TSharedRef<SWidget> SLyraRecoilPreview::BuildEventTable()
{
	return SNew(SBorder)
		.Padding(4.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("ShotTableHeader",
					"#  时间(s)  方向偏移P/Y  本发KickP/Y  起枪偏移P/Y  可见角P/Y  姿态  倍率"))
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
					.Text_Lambda([this]()
					{
						const TArray<FLyraRecoilPreviewShot>& Shots = Controller->GetShots();
						if (Shots.Num() == 0)
						{
							return FText::FromString(TEXT("（没有发射记录；检查发数与时间轴）"));
						}

						FString Text;
						Text.Reserve(Shots.Num() * 96);
						for (const FLyraRecoilPreviewShot& Shot : Shots)
						{
							Text += FString::Printf(
								TEXT("%3d  %7.3f  %7.3f/%7.3f  %7.3f/%7.3f  %7.3f/%7.3f  %7.3f/%7.3f  %s  %.3f%s\n"),
								Shot.ActualShotIndex + 1,
								Shot.TimeSeconds,
								Shot.DirectionOffsetPitch, Shot.DirectionOffsetYaw,
								Shot.AppliedKickPitch, Shot.AppliedKickYaw,
								Shot.BurstStartPitchOffset, Shot.BurstStartYawOffset,
								Shot.VisibleAnglePitch, Shot.VisibleAngleYaw,
								*GetPoseTextForState(Shot.PoseState).ToString(),
								Shot.PoseMultiplier,
								Shot.bStartedNewBurst ? TEXT("  [新一轮]") : TEXT(""));
						}
						return FText::FromString(Text);
					})
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(2.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([this]()
				{
					// 诊断：状态、插值阶段、峰值/抵扣、上限、校验告警。
					const TArray<FLyraRecoilPreviewSample>& Samples = Controller->GetSamples();
					const FLyraRecoilPreviewSample* Last = Samples.Num() > 0 ? &Samples.Last() : nullptr;

					FString Text;
					if (Last != nullptr)
					{
						Text += FString::Printf(
							TEXT("末样本：t=%.3f  State=%d  InterpStage=%d  ShotIndex=%d  RecoveryPeakP=%.3f  CoverP=%.3f  SubSteps=%.0f\n"),
							Last->TimeSeconds,
							static_cast<int32>(Last->State),
							static_cast<int32>(Last->InterpStage),
							Last->ShotIndex,
							Last->RecoveryPeakPitch,
							Last->RecoveryCoverPitch,
							Last->LastSubStepCount);
					}

					if (Controller->IsTruncatedByLimit())
					{
						Text += FString::Printf(TEXT("限额：%s\n"), *Controller->GetTruncationReason());
					}
					if (!LastRefreshError.IsEmpty())
					{
						Text += FString::Printf(TEXT("快照错误：%s\n"), *LastRefreshError);
					}
					if (LastInlinedCurves.Num() > 0)
					{
						Text += FString::Printf(TEXT("已内联外部曲线 %d 条\n"), LastInlinedCurves.Num());
					}
					for (const FString& Warning : Controller->GetWarnings())
					{
						Text += FString::Printf(TEXT("告警：%s\n"), *Warning);
					}
					return FText::FromString(Text.IsEmpty() ? TEXT("（无诊断）") : Text);
				})
			]
		];
}

TSharedRef<SWidget> SLyraRecoilPreview::BuildParameterPanel()
{
	return SNew(SBorder)
		.Padding(4.0f)
		[
			SNew(SHorizontalBox)

			// 射速 / 发数 / 步长 / 尾时长
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(LOCTEXT("RpmLabel", "射速 RPM"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SNumericEntryBox<float>)
					.AllowSpin(true)
					.MinValue(1.0f)
					.MaxValue(3000.0f)
					.Delta(10.0f)
					.MinDesiredValueWidth(64.0f)
					.Value_Lambda([this]() { return TOptional<float>(Controller->GetConfig().RPM); })
					.OnValueChanged_Lambda([this](float NewValue)
					{
						FLyraRecoilPreviewConfig NewConfig = Controller->GetConfig();
						NewConfig.RPM = FMath::Clamp(NewValue, 1.0f, 3000.0f);
						Controller->SetConfig(NewConfig);
                        RebuildFireInputs(bTwoBursts);
						// Simulation is incrementally computed by the active timer.
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(LOCTEXT("ShotCountLabel", "发数"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(true)
					.MinValue(0)
					.MaxValue(FLyraRecoilPreviewConfig::MaxShots)
					.Delta(1)
					.MinDesiredValueWidth(64.0f)
					.Value_Lambda([this]() { return TOptional<int32>(Controller->GetConfig().ShotCount); })
					.OnValueChanged_Lambda([this](int32 NewValue)
					{
						FLyraRecoilPreviewConfig NewConfig = Controller->GetConfig();
						NewConfig.ShotCount = FMath::Clamp(NewValue, 0, FLyraRecoilPreviewConfig::MaxShots);
                        ShotsPerBurst=NewConfig.ShotCount; bTwoBursts=false;
						NewConfig.FireInputs.Reset();
						Controller->SetConfig(NewConfig);
						// Simulation is incrementally computed by the active timer.
					})
				]
			]

			// 步长 / 尾时长 / 起枪角
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text_Lambda([this]()
					{
						return FText::FromString(FString::Printf(TEXT("步长 Hz（当前 %s）"), *GetFrameRateText().ToString()));
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SNumericEntryBox<float>)
					.AllowSpin(true)
					.MinValue(1.0f)
					.MaxValue(1000.0f)
					.Delta(30.0f)
					.MinDesiredValueWidth(64.0f)
					.Value_Lambda([this]()
					{
						return TOptional<float>(1.0f / FMath::Max(Controller->GetConfig().FrameSeconds, KINDA_SMALL_NUMBER));
					})
					.OnValueChanged_Lambda([this](float NewValue)
					{
						FLyraRecoilPreviewConfig NewConfig = Controller->GetConfig();
						NewConfig.FrameSeconds = 1.0f / FMath::Clamp(NewValue, 1.0f, 1000.0f);
						Controller->SetConfig(NewConfig);
						// Simulation is incrementally computed by the active timer.
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(LOCTEXT("TailLabel", "尾时长 s"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SNumericEntryBox<float>)
					.AllowSpin(true)
					.MinValue(0.0f)
					.MaxValue(60.0f)
					.Delta(0.25f)
					.MinDesiredValueWidth(64.0f)
					.Value_Lambda([this]() { return TOptional<float>(Controller->GetConfig().TailSeconds); })
					.OnValueChanged_Lambda([this](float NewValue)
					{
						FLyraRecoilPreviewConfig NewConfig = Controller->GetConfig();
						NewConfig.TailSeconds = FMath::Clamp(NewValue, 0.0f, 60.0f);
						Controller->SetConfig(NewConfig);
						// Simulation is incrementally computed by the active timer.
					})
				]
			]

			// 起枪角 / 全局倍率 / 瞄准 Alpha
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(LOCTEXT("BurstAngleLabel", "起枪角 Pitch / Yaw"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SNumericEntryBox<float>)
						.AllowSpin(true)
						.MinValue(-90.0f)
						.MaxValue(90.0f)
						.Delta(1.0f)
						.MinDesiredValueWidth(56.0f)
						.Value_Lambda([this]() { return TOptional<float>(Controller->GetConfig().BurstStartAnglePitch); })
						.OnValueChanged_Lambda([this](float NewValue)
						{
							FLyraRecoilPreviewConfig NewConfig = Controller->GetConfig();
							NewConfig.BurstStartAnglePitch = NewValue;
							Controller->SetConfig(NewConfig);
							// Simulation is incrementally computed by the active timer.
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SNumericEntryBox<float>)
						.AllowSpin(true)
						.MinValue(-180.0f)
						.MaxValue(180.0f)
						.Delta(1.0f)
						.MinDesiredValueWidth(56.0f)
						.Value_Lambda([this]() { return TOptional<float>(Controller->GetConfig().BurstStartAngleYaw); })
						.OnValueChanged_Lambda([this](float NewValue)
						{
							FLyraRecoilPreviewConfig NewConfig = Controller->GetConfig();
							NewConfig.BurstStartAngleYaw = NewValue;
							Controller->SetConfig(NewConfig);
							// Simulation is incrementally computed by the active timer.
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(LOCTEXT("GlobalScaleLabel", "全局倍率"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SNumericEntryBox<float>)
					.AllowSpin(true)
					.MinValue(0.0f)
					.MaxValue(10.0f)
					.Delta(0.1f)
					.MinDesiredValueWidth(64.0f)
					.Value_Lambda([this]() { return TOptional<float>(Controller->GetConfig().GlobalScale); })
					.OnValueChanged_Lambda([this](float NewValue)
					{
						FLyraRecoilPreviewConfig NewConfig = Controller->GetConfig();
						NewConfig.GlobalScale = FMath::Max(0.0f, NewValue);
						Controller->SetConfig(NewConfig);
						// Simulation is incrementally computed by the active timer.
					})
				]
			]

			// 姿态 / 瞄准 Alpha / 输入脚本
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text_Lambda([this]()
					{
						return FText::FromString(FString::Printf(TEXT("姿态（当前 %s）"), *GetPoseText().ToString()));
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("PoseStanding", "站立"))
						.OnClicked_Lambda([this]() { SetPoseState(EPoseState::Standing); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("PoseCrouching", "蹲伏"))
						.OnClicked_Lambda([this]() { SetPoseState(EPoseState::Crouching); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("PoseAir", "空中"))
						.OnClicked_Lambda([this]() { SetPoseState(EPoseState::JumpingOrFalling); return FReply::Handled(); })
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(LOCTEXT("AimingAlphaLabel", "瞄准 Alpha"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SNumericEntryBox<float>)
					.AllowSpin(true)
					.MinValue(0.0f)
					.MaxValue(1.0f)
					.Delta(0.1f)
					.MinDesiredValueWidth(64.0f)
					.Value_Lambda([this]()
					{
						const TArray<FLyraRecoilPreviewInputEntry>& Script = Controller->GetConfig().InputScript;
						return TOptional<float>(Script.Num() > 0 ? Script.Last().AimingAlpha : 0.0f);
					})
					.OnValueChanged_Lambda([this](float NewValue)
					{
						SetAimingAlpha(FMath::Clamp(NewValue, 0.0f, 1.0f));
					})
				]
			]

			// 单发模式 / 两轮发射脚本 / 种子
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text_Lambda([this]()
					{
						return FText::FromString(FString::Printf(TEXT("单发模式（当前 %s）"), *GetSingleShotModeText().ToString()));
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("ModeFromProfileButton", "跟随资产"))
						.OnClicked_Lambda([this]()
						{
							SetSingleShotModeOverride(ELyraRecoilPreviewSingleShotMode::FromProfile);
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("ModeInstantButton", "瞬时"))
						.OnClicked_Lambda([this]()
						{
							SetSingleShotModeOverride(ELyraRecoilPreviewSingleShotMode::InstantWrite);
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("ModeInterpButton", "插值"))
						.OnClicked_Lambda([this]()
						{
							SetSingleShotModeOverride(ELyraRecoilPreviewSingleShotMode::Interpolated);
							return FReply::Handled();
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("OneBurstButton", "单轮"))
						.OnClicked_Lambda([this]() { ApplyFireScript(false); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("TwoBurstButton", "两轮"))
						.OnClicked_Lambda([this]() { ApplyFireScript(true); return FReply::Handled(); })
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(LOCTEXT("SeedLabel", "固定种子"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(true)
					.MinValue(0)
					.MaxValue(MAX_int32)
					.Delta(1)
					.MinDesiredValueWidth(96.0f)
					.Value_Lambda([this]() { return TOptional<int32>(Controller->GetConfig().FixedSeedOverride); })
					.OnValueChanged_Lambda([this](int32 NewValue)
					{
						FLyraRecoilPreviewConfig NewConfig = Controller->GetConfig();
						NewConfig.SeedModeOverride = ERecoilRandomSeedMode::Fixed;
						NewConfig.FixedSeedOverride = NewValue;
						Controller->SetConfig(NewConfig);
						// Simulation is incrementally computed by the active timer.
					})
				]
			]

			// 压枪脚本 / 多轮排布
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(LOCTEXT("DownPullLabel", "压枪量 deg / 起始 s / 时长 s"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SNumericEntryBox<float>)
						.AllowSpin(true)
						.MinValue(0.0f)
						.MaxValue(90.0f)
						.Delta(0.5f)
						.MinDesiredValueWidth(52.0f)
						.Value_Lambda([this]() { return TOptional<float>(DownPullDegrees); })
						.OnValueChanged_Lambda([this](float NewValue)
						{
							DownPullDegrees = FMath::Clamp(NewValue, 0.0f, 90.0f);
							RebuildInputScript();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SNumericEntryBox<float>)
						.AllowSpin(true)
						.MinValue(0.0f)
						.MaxValue(60.0f)
						.Delta(0.05f)
						.MinDesiredValueWidth(52.0f)
						.Value_Lambda([this]() { return TOptional<float>(DownPullStartSeconds); })
						.OnValueChanged_Lambda([this](float NewValue)
						{
							DownPullStartSeconds = FMath::Clamp(NewValue, 0.0f, 60.0f);
							RebuildInputScript();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SNumericEntryBox<float>)
						.AllowSpin(true)
						.MinValue(0.0f)
						.MaxValue(60.0f)
						.Delta(0.05f)
						.MinDesiredValueWidth(52.0f)
						.Value_Lambda([this]() { return TOptional<float>(DownPullHoldSeconds); })
						.OnValueChanged_Lambda([this](float NewValue)
						{
							DownPullHoldSeconds = FMath::Clamp(NewValue, 0.0f, 60.0f);
							RebuildInputScript();
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(LOCTEXT("BurstLayoutLabel", "每轮发数 / 轮间隔 s"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SNumericEntryBox<int32>)
						.AllowSpin(true)
						.MinValue(1)
						.MaxValue(FLyraRecoilPreviewConfig::MaxShots)
						.Delta(1)
						.MinDesiredValueWidth(52.0f)
						.Value_Lambda([this]() { return TOptional<int32>(ShotsPerBurst); })
						.OnValueChanged_Lambda([this](int32 NewValue)
						{
							ShotsPerBurst = FMath::Clamp(NewValue, 1, FLyraRecoilPreviewConfig::MaxShots);
							RebuildFireInputs(bTwoBursts);
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SNumericEntryBox<float>)
						.AllowSpin(true)
						.MinValue(0.0f)
						.MaxValue(60.0f)
						.Delta(0.05f)
						.MinDesiredValueWidth(52.0f)
						.Value_Lambda([this]() { return TOptional<float>(BurstGapSeconds); })
						.OnValueChanged_Lambda([this](float NewValue)
						{
							BurstGapSeconds = FMath::Clamp(NewValue, 0.0f, 60.0f);
							RebuildFireInputs(bTwoBursts);
						})
					]
				]
			]
		];
}

// ---------------------------------------------------------------------------
// 参数编辑辅助
// ---------------------------------------------------------------------------

FText SLyraRecoilPreview::GetPoseTextForState(EPoseState InState)
{
	switch (InState)
	{
	case EPoseState::Crouching:
		return LOCTEXT("PoseCrouchState", "蹲伏");
	case EPoseState::JumpingOrFalling:
		return LOCTEXT("PoseAirState", "空中");
	case EPoseState::Standing:
	default:
		return LOCTEXT("PoseStandState", "站立");
	}
}

void SLyraRecoilPreview::SetPoseState(EPoseState InState)
{
	ScriptPoseState = InState;
	RebuildInputScript();
}

void SLyraRecoilPreview::SetAimingAlpha(float InValue)
{
	ScriptAimingAlpha = FMath::Clamp(InValue, 0.0f, 1.0f);
	RebuildInputScript();
}

void SLyraRecoilPreview::SetSingleShotModeOverride(ELyraRecoilPreviewSingleShotMode InMode)
{
	FLyraRecoilPreviewConfig NewConfig = Controller->GetConfig();
	NewConfig.SingleShotModeOverride = InMode;
	Controller->SetConfig(NewConfig);
	// Simulation is incrementally computed by the active timer.
}

void SLyraRecoilPreview::RebuildInputScript()
{
	FLyraRecoilPreviewConfig NewConfig = Controller->GetConfig();
	NewConfig.InputScript.Reset();

	const float StartPitch = NewConfig.BurstStartAnglePitch;
	const float StartYaw = NewConfig.BurstStartAngleYaw;

	// t=0：起枪角。
	{
		FLyraRecoilPreviewInputEntry Entry;
		Entry.TimeSeconds = 0.0f;
		Entry.AimPitchDegrees = StartPitch;
		Entry.AimYawDegrees = StartYaw;
		Entry.PoseState = ScriptPoseState;
		Entry.AimingAlpha = ScriptAimingAlpha;
		NewConfig.InputScript.Add(Entry);
	}

	// 压枪：从 DownPullStartSeconds 起把 ControlRotation 往下拉 DownPullDegrees。
	if (DownPullDegrees > 0.0f)
	{
		FLyraRecoilPreviewInputEntry Pull;
		Pull.TimeSeconds = FMath::Max(0.0f, DownPullStartSeconds);
		Pull.AimPitchDegrees = StartPitch - DownPullDegrees;
		Pull.AimYawDegrees = StartYaw;
		Pull.PoseState = ScriptPoseState;
		Pull.AimingAlpha = ScriptAimingAlpha;
		NewConfig.InputScript.Add(Pull);

		// 松手（可选）：DownPullHoldSeconds > 0 时在之后恢复起枪角。
		if (DownPullHoldSeconds > 0.0f)
		{
			FLyraRecoilPreviewInputEntry Release;
			Release.TimeSeconds = Pull.TimeSeconds + DownPullHoldSeconds;
			Release.AimPitchDegrees = StartPitch;
			Release.AimYawDegrees = StartYaw;
			Release.PoseState = ScriptPoseState;
			Release.AimingAlpha = ScriptAimingAlpha;
			NewConfig.InputScript.Add(Release);
		}
	}

	Controller->SetConfig(NewConfig);
	// Simulation is incrementally computed by the active timer.
}

void SLyraRecoilPreview::RebuildFireInputs(bool bInTwoBursts)
{
	bTwoBursts = bInTwoBursts;

	FLyraRecoilPreviewConfig NewConfig = Controller->GetConfig();
	const float RPM = FMath::Max(NewConfig.RPM, 1.0f);
	NewConfig.FireInputs.Reset();
    NewConfig.ShotCount=ShotsPerBurst;

	const int32 PerBurst = FMath::Clamp(ShotsPerBurst, 1, FLyraRecoilPreviewConfig::MaxShots);

	if (!bTwoBursts)
	{
		FLyraRecoilPreviewFireInput Single;
		Single.StartTimeSeconds = 0.0f;
		Single.ShotCount = PerBurst;
		Single.RPM = RPM;
		NewConfig.FireInputs.Add(Single);
	}
	else
	{
		FLyraRecoilPreviewFireInput First;
		First.StartTimeSeconds = 0.0f;
		First.ShotCount = PerBurst;
		First.RPM = RPM;
		NewConfig.FireInputs.Add(First);

		// 第二轮在第一轮最后一发之后 BurstGapSeconds 开始 —— 通常落在回正途中，
		// 用于验收"回正中重开火以当前可见角度为新起点"。
		const float FirstBurstEnd = (60.0f / RPM) * static_cast<float>(PerBurst - 1);
		FLyraRecoilPreviewFireInput Second;
		Second.StartTimeSeconds = FirstBurstEnd + FMath::Max(0.05f, BurstGapSeconds);
		Second.ShotCount = PerBurst;
		Second.RPM = RPM;
		NewConfig.FireInputs.Add(Second);
	}

	Controller->SetConfig(NewConfig);
	// Simulation is incrementally computed by the active timer.
}

void SLyraRecoilPreview::ApplyFireScript(bool bInTwoBursts)
{
	RebuildFireInputs(bInTwoBursts);
}

#undef LOCTEXT_NAMESPACE
