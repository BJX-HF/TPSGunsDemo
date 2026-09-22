// Copyright Epic Games, Inc. All Rights Reserved.

#include "LyraRangedWeaponInstance.h"
#include "NativeGameplayTags.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "Camera/LyraCameraComponent.h"
#include "Camera/LyraCameraModifier_WeaponRecoil.h"
#include "Camera/LyraPlayerCameraManager.h"
#include "Physics/PhysicalMaterialWithTags.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Weapons/LyraWeaponInstance.h"
#include "Weapons/Recoil/LyraRecoilDebug.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LyraRangedWeaponInstance)

DEFINE_LOG_CATEGORY_STATIC(LogLyraRecoilWeapon, Log, All);

namespace LyraRecoilWeaponPrivate
{
	/**
	 * 诊断跟踪开关：`Lyra.Recoil.Trace`（默认 0）。
	 *
	 * 刻意定义在本文件内、而不是挂到 ULyraRecoilDebug 上：
	 * 那是一个 UCLASS，加一个带 UFUNCTION 的入口会触发 UHT 重新生成反射代码，
	 * 而这里只需要一个控制台开关。放在用它的地方，改动面最小。
	 *
	 * 打开后每帧打印一行，用来区分两种**修法完全不同**的成因：
	 *   ① 上游状态链断了        —— push 本身就是 0
	 *   ② 偏移被别的东西覆盖了  —— push 非零，但 delta 是 0
	 * 排查完请敲 `Lyra.Recoil.Trace 0` 关掉（每帧一行会淹没日志）。
	 */
	static bool bRecoilTrace = false;
	static FAutoConsoleVariableRef CVarRecoilTrace(
		TEXT("Lyra.Recoil.Trace"),
		bRecoilTrace,
		TEXT("Log one line per frame comparing the offset pushed to the camera against the camera's actual POV.\n")
		TEXT("Use it to tell 'the offset chain is broken' apart from 'the offset is applied but overwritten'.\n")
		TEXT("Also logs camera location: final POV vs camera-stack (component) vs pawn, pawn-relative offset\n")
		TEXT("and per-frame delta, to catch translation injected by other camera modifiers while firing.\n")
		TEXT("Default: 0 - leave it off outside of a diagnostic session."),
		ECVF_Default);
}

UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_Lyra_Weapon_SteadyAimingCamera, "Lyra.Weapon.SteadyAimingCamera");

ULyraRangedWeaponInstance::ULyraRangedWeaponInstance(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	HeatToHeatPerShotCurve.EditorCurveData.AddKey(0.0f, 1.0f);
	HeatToCoolDownPerSecondCurve.EditorCurveData.AddKey(0.0f, 2.0f);
}

void ULyraRangedWeaponInstance::PostLoad()
{
	Super::PostLoad();

#if WITH_EDITOR
	UpdateDebugVisualization();
#endif
}

#if WITH_EDITOR
void ULyraRangedWeaponInstance::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateDebugVisualization();
}

void ULyraRangedWeaponInstance::UpdateDebugVisualization()
{
	// 资产散布模型下，调试量直接从 RecoilState 读。
	//
	// heat 在该模型里根本不存在，所以 Debug_MinHeat / Debug_MaxHeat 刻意写 0 ——
	// 在 Details 面板上「MinHeat=0 MaxHeat=0 而 MinSpread 非 0」就是
	// 「这把枪走的是资产散布」的识别特征，不用再去翻 bEnableProfileSpread。
	if (UsesProfileSpread())
	{
		Debug_MinHeat = 0.0f;
		Debug_MaxHeat = 0.0f;
		Debug_MinSpreadAngle = RecoilState.CurrentSpreadBaseAngle;
		Debug_MaxSpreadAngle = RecoilState.CurrentSpreadMaxAngle;
		Debug_CurrentHeat = 0.0f;
		Debug_CurrentSpreadAngle = RecoilState.CurrentSpreadAngle;
		Debug_CurrentSpreadAngleMultiplier =
			RecoilState.SpreadAimingMultiplier * RecoilState.SpreadMovementMultiplier;
		return;
	}

	ComputeHeatRange(/*out*/ Debug_MinHeat, /*out*/ Debug_MaxHeat);
	ComputeSpreadRange(/*out*/ Debug_MinSpreadAngle, /*out*/ Debug_MaxSpreadAngle);
	Debug_CurrentHeat = CurrentHeat;
	Debug_CurrentSpreadAngle = CurrentSpreadAngle;
	Debug_CurrentSpreadAngleMultiplier = CurrentSpreadAngleMultiplier;
}
#endif

//////////////////////////////////////////////////////////////////////////
// 散布查询（两条链路的唯一分叉点）
//////////////////////////////////////////////////////////////////////////

bool ULyraRangedWeaponInstance::UsesProfileSpread() const
{
	return (RecoilProfile != nullptr) && RecoilProfile->bEnableProfileSpread;
}

float ULyraRangedWeaponInstance::GetCalculatedSpreadAngle() const
{
	// 资产散布：锥角由 RecoilState 维护（含连射累加，不含玩家侧倍率）；
	// 玩家侧倍率走 GetCalculatedSpreadAngleMultiplier()，两者在弹道侧相乘 ——
	// 与原生链路的口径完全一致，所以 GA 与准星代码一行都不用改。
	if (UsesProfileSpread())
	{
		return RecoilState.CurrentSpreadAngle;
	}

	return CurrentSpreadAngle;
}

float ULyraRangedWeaponInstance::GetCalculatedSpreadAngleMultiplier() const
{
	if (UsesProfileSpread())
	{
		// 资产模型刻意不提供"首发绝对精准"（bAllowFirstShotAccuracy 已废弃）：
		// 想要 0 散布就把基础角配成 0，而不是靠一个隐藏开关把倍率清空 ——
		// 后者会让「为什么准星忽然缩到 0」这类问题无从排查。
		return RecoilState.SpreadAimingMultiplier * RecoilState.SpreadMovementMultiplier;
	}

	return bHasFirstShotAccuracy ? 0.0f : CurrentSpreadAngleMultiplier;
}

float ULyraRangedWeaponInstance::GetSpreadExponent() const
{
	if (UsesProfileSpread())
	{
		return RecoilProfile->SpreadExponent;
	}

	return SpreadExponent;
}

void ULyraRangedWeaponInstance::OnEquipped()
{
	Super::OnEquipped();

	if (UsesProfileSpread())
	{
		// 资产散布：锥角初值由 FRecoilRuntimeState::Reset 统一给出（见那里的说明），
		// 这里只需要把移动倍率复位成 1.0，避免上一把枪的站定加成带过来。
		//
		// 刻意**不做** Lyra 那套 "heat 从 range 中点开始"的初始化 ——
		// 那正是本项目要干掉的行为：换个弹匣第一发的散布不是基础值，无法解释。
		SpreadMovementMultiplier = 1.0f;
	}
	else
	{
		// Start heat in the middle
		float MinHeatRange;
		float MaxHeatRange;
		ComputeHeatRange(/*out*/ MinHeatRange, /*out*/ MaxHeatRange);
		CurrentHeat = (MinHeatRange + MaxHeatRange) * 0.5f;

		// Derive spread
		CurrentSpreadAngle = HeatToSpreadCurve.GetRichCurveConst()->Eval(CurrentHeat);
	}

	// Default the multipliers to 1x
	CurrentSpreadAngleMultiplier = 1.0f;
	StandingStillMultiplier = 1.0f;
	JumpFallMultiplier = 1.0f;
	CrouchingMultiplier = 1.0f;

	// 后坐力：装备即清零，避免上一把枪的残留偏移带过来
	ResetRecoilState();
}

void ULyraRangedWeaponInstance::OnUnequipped()
{
	Super::OnUnequipped();

	// 后坐力：卸下时做两件事 ——
	//  1) 清零状态，否则再装备会从旧偏移开始；
	//  2) 让相机上可能残留的偏移走一段释放衰减。
	//
	// 注意这里**不再摘掉相机修改器**。摘掉等于让残留偏移在同一帧内消失，
	// 玩家看到的就是一次跳变（反馈里的"切枪震屏"）。
	// 修改器常驻在相机管理器上，由它自己把偏移衰减回 0 —— 详见 UCameraModifier_WeaponRecoil 的类注释。
	ResetRecoilState();
	ClearRecoilCameraOffset();
}

void ULyraRangedWeaponInstance::Tick(float DeltaSeconds)
{
	APawn* Pawn = GetPawn();
	check(Pawn != nullptr);
	
	const bool bMinSpread = UpdateSpread(DeltaSeconds);
	const bool bMinMultipliers = UpdateMultipliers(DeltaSeconds);

	// 资产散布模型刻意不提供"首发绝对精准"，因此这里强制关掉 FSA 判定。
	// 不这样做的话，bMinSpread 恒为 true（见 UpdateSpread）会让旧开关意外生效。
	bHasFirstShotAccuracy = !UsesProfileSpread() && bAllowFirstShotAccuracy && bMinMultipliers && bMinSpread;

	// 后坐力：推进时间轴（回正）→ 把结果推给相机链 → 数值面板
	UpdateRecoil(DeltaSeconds);

#if WITH_EDITOR
	UpdateDebugVisualization();
#endif
}

void ULyraRangedWeaponInstance::ComputeHeatRange(float& MinHeat, float& MaxHeat)
{
	float Min1;
	float Max1;
	HeatToHeatPerShotCurve.GetRichCurveConst()->GetTimeRange(/*out*/ Min1, /*out*/ Max1);

	float Min2;
	float Max2;
	HeatToCoolDownPerSecondCurve.GetRichCurveConst()->GetTimeRange(/*out*/ Min2, /*out*/ Max2);

	float Min3;
	float Max3;
	HeatToSpreadCurve.GetRichCurveConst()->GetTimeRange(/*out*/ Min3, /*out*/ Max3);

	MinHeat = FMath::Min(FMath::Min(Min1, Min2), Min3);
	MaxHeat = FMath::Max(FMath::Max(Max1, Max2), Max3);
}

void ULyraRangedWeaponInstance::ComputeSpreadRange(float& MinSpread, float& MaxSpread)
{
	HeatToSpreadCurve.GetRichCurveConst()->GetValueRange(/*out*/ MinSpread, /*out*/ MaxSpread);
}

void ULyraRangedWeaponInstance::AddSpread()
{
	// ---------------------------------------------------------------------
	// 资产散布链路（姿态-角度直接模型，见 Docs/Recoil/12_SpreadInProfile.md）
	//
	// ★ 注意本函数的**调用时机在 TraceBulletsInCartridge 之后**
	//   （GA 的 OnTargetDataReadyCallback 里：TraceBulletsInCartridge 在发射流程前段，
	//    AddSpread 在扣弹成功之后）。也就是说这里加热的是**下一发**要用的锥角，
	//    本发用的是"加热前"的值 —— 这是 Lyra 原生语义，本项目刻意保持不变，
	//    否则"第一发按基础散布打出去"这条最基本的手感会变。
	//
	//   也正因如此，FRecoilShotResult::SpreadAngle 不能在这里取，必须由弹道侧
	//   在发弹那一刻留下快照（RecoilState.PendingShotSpreadAngle）。
	// ---------------------------------------------------------------------
	if (UsesProfileSpread())
	{
		RecoilState.ApplySpreadShot(RecoilProfile, ComputeRecoilPoseState());

#if WITH_EDITOR
		UpdateDebugVisualization();
#endif
		return;
	}

	// --- Lyra 原生 heat 链路（保留作为回退，见头文件里的迁移对照表）---

	// Sample the heat up curve
	const float HeatPerShot = HeatToHeatPerShotCurve.GetRichCurveConst()->Eval(CurrentHeat);
	CurrentHeat = ClampHeat(CurrentHeat + HeatPerShot);

	// Map the heat to the spread angle
	CurrentSpreadAngle = HeatToSpreadCurve.GetRichCurveConst()->Eval(CurrentHeat);

#if WITH_EDITOR
	UpdateDebugVisualization();
#endif
}

float ULyraRangedWeaponInstance::GetDistanceAttenuation(float Distance, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags) const
{
	const FRichCurve* Curve = DistanceDamageFalloff.GetRichCurveConst();
	return Curve->HasAnyData() ? Curve->Eval(Distance) : 1.0f;
}

float ULyraRangedWeaponInstance::GetPhysicalMaterialAttenuation(const UPhysicalMaterial* PhysicalMaterial, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags) const
{
	float CombinedMultiplier = 1.0f;
	if (const UPhysicalMaterialWithTags* PhysMatWithTags = Cast<const UPhysicalMaterialWithTags>(PhysicalMaterial))
	{
		for (FGameplayTag MaterialTag : PhysMatWithTags->Tags)
		{
			if (const float* pTagMultiplier = MaterialDamageMultiplier.Find(MaterialTag))
			{
				CombinedMultiplier *= *pTagMultiplier;
			}
		}
	}

	return CombinedMultiplier;
}

bool ULyraRangedWeaponInstance::UpdateSpread(float DeltaSeconds)
{
	// 资产散布：加热在 AddSpread()、回落在 UpdateRecoil()，本函数无事可做。
	//
	// 恒返回 true 是"已到最小散布"的语义 —— 资产模型没有 FSA 开关，
	// Tick 里已经用 !UsesProfileSpread() 把 FSA 判定整体关掉了，所以这个返回值不会被消费。
	if (UsesProfileSpread())
	{
		return true;
	}

	const float TimeSinceFired = GetWorld()->TimeSince(LastFireTime);

	if (TimeSinceFired > SpreadRecoveryCooldownDelay)
	{
		const float CooldownRate = HeatToCoolDownPerSecondCurve.GetRichCurveConst()->Eval(CurrentHeat);
		CurrentHeat = ClampHeat(CurrentHeat - (CooldownRate * DeltaSeconds));
		CurrentSpreadAngle = HeatToSpreadCurve.GetRichCurveConst()->Eval(CurrentHeat);
	}
	
	float MinSpread;
	float MaxSpread;
	ComputeSpreadRange(/*out*/ MinSpread, /*out*/ MaxSpread);

	return FMath::IsNearlyEqual(CurrentSpreadAngle, MinSpread, KINDA_SMALL_NUMBER);
}

float ULyraRangedWeaponInstance::ComputeAimingAlpha() const
{
	// 这段判定原本内联在 UpdateMultipliers() 里；后坐力姿态倍率需要同一份数据，
	// 所以提取成共用函数，避免两处逻辑随时间漂移（开发计划 §P4「不重复造轮子」）。
	const APawn* Pawn = GetPawn();
	if (Pawn == nullptr)
	{
		return 0.0f;
	}

	const ULyraCameraComponent* CameraComponent = ULyraCameraComponent::FindCameraComponent(Pawn);
	if (CameraComponent == nullptr)
	{
		return 0.0f;
	}

	float TopCameraWeight = 0.0f;
	FGameplayTag TopCameraTag;
	CameraComponent->GetBlendInfo(/*out*/ TopCameraWeight, /*out*/ TopCameraTag);

	return (TopCameraTag == TAG_Lyra_Weapon_SteadyAimingCamera) ? TopCameraWeight : 0.0f;
}

bool ULyraRangedWeaponInstance::UpdateMultipliers(float DeltaSeconds)
{
	const float MultiplierNearlyEqualThreshold = 0.05f;

	APawn* Pawn = GetPawn();
	check(Pawn != nullptr);
	UCharacterMovementComponent* CharMovementComp = Cast<UCharacterMovementComponent>(Pawn->GetMovementComponent());

	// See if we are standing still, and if so, smoothly apply the bonus
	const float PawnSpeed = Pawn->GetVelocity().Size();
	const float MovementTargetValue = FMath::GetMappedRangeValueClamped(
		/*InputRange=*/ FVector2D(StandingStillSpeedThreshold, StandingStillSpeedThreshold + StandingStillToMovingSpeedRange),
		/*OutputRange=*/ FVector2D(SpreadAngleMultiplier_StandingStill, 1.0f),
		/*Alpha=*/ PawnSpeed);
	StandingStillMultiplier = FMath::FInterpTo(StandingStillMultiplier, MovementTargetValue, DeltaSeconds, TransitionRate_StandingStill);
	const bool bStandingStillMultiplierAtMin = FMath::IsNearlyEqual(StandingStillMultiplier, SpreadAngleMultiplier_StandingStill, SpreadAngleMultiplier_StandingStill*0.1f);

	// --- 资产散布的"移动倍率"（独立一份，不并进上面的 CombinedMultiplier）---
	//
	// 为什么不复用 StandingStillMultiplier：那一个会被乘进 CurrentSpreadAngleMultiplier，
	// 而后者在资产模型下已经不再被读取（锥角走 RecoilState）。两套模型各存一份，
	// 好处是"切换开关"这件事只影响一行判定，两个状态都不会互相污染。
	//
	// 阈值 / 带宽 / 过渡速率全部来自 Profile（不是本类的成员），
	// 所以在资产上改完立刻生效，不需要重启 PIE。
	if (UsesProfileSpread())
	{
		const float ProfileMovementTarget = RecoilProfile->GetSpreadMovementMultiplierTarget(PawnSpeed);
		SpreadMovementMultiplier = FMath::FInterpTo(
			SpreadMovementMultiplier,
			ProfileMovementTarget,
			DeltaSeconds,
			RecoilProfile->SpreadTransitionRate_StandingStill);
	}

	// See if we are crouching, and if so, smoothly apply the bonus
	const bool bIsCrouching = (CharMovementComp != nullptr) && CharMovementComp->IsCrouching();
	const float CrouchingTargetValue = bIsCrouching ? SpreadAngleMultiplier_Crouching : 1.0f;
	CrouchingMultiplier = FMath::FInterpTo(CrouchingMultiplier, CrouchingTargetValue, DeltaSeconds, TransitionRate_Crouching);
	const bool bCrouchingMultiplierAtTarget = FMath::IsNearlyEqual(CrouchingMultiplier, CrouchingTargetValue, MultiplierNearlyEqualThreshold);

	// See if we are in the air (jumping/falling), and if so, smoothly apply the penalty
	const bool bIsJumpingOrFalling = (CharMovementComp != nullptr) && CharMovementComp->IsFalling();
	const float JumpFallTargetValue = bIsJumpingOrFalling ? SpreadAngleMultiplier_JumpingOrFalling : 1.0f;
	JumpFallMultiplier = FMath::FInterpTo(JumpFallMultiplier, JumpFallTargetValue, DeltaSeconds, TransitionRate_JumpingOrFalling);
	const bool bJumpFallMultiplerIs1 = FMath::IsNearlyEqual(JumpFallMultiplier, 1.0f, MultiplierNearlyEqualThreshold);

	// Determine if we are aiming down sights, and apply the bonus based on how far into the camera transition we are
	const float AimingAlpha = ComputeAimingAlpha();
	const float AimingMultiplier = FMath::GetMappedRangeValueClamped(
		/*InputRange=*/ FVector2D(0.0f, 1.0f),
		/*OutputRange=*/ FVector2D(1.0f, SpreadAngleMultiplier_Aiming),
		/*Alpha=*/ AimingAlpha);
	const bool bAimingMultiplierAtTarget = FMath::IsNearlyEqual(AimingMultiplier, SpreadAngleMultiplier_Aiming, KINDA_SMALL_NUMBER);

	// Combine all the multipliers
	const float CombinedMultiplier = AimingMultiplier * StandingStillMultiplier * CrouchingMultiplier * JumpFallMultiplier;
	CurrentSpreadAngleMultiplier = CombinedMultiplier;

	// need to handle these spread multipliers indicating we are not at min spread
	return bStandingStillMultiplierAtMin && bCrouchingMultiplierAtTarget && bJumpFallMultiplerIs1 && bAimingMultiplierAtTarget;
}

//////////////////////////////////////////////////////////////////////////
// 后坐力系统
//////////////////////////////////////////////////////////////////////////

void ULyraRangedWeaponInstance::AddRecoil()
{
	if (!ULyraRecoilDebug::IsRecoilEnabled())
	{
		return;
	}

	const ULyraRecoilProfile* Profile = RecoilProfile;
	if (Profile == nullptr)
	{
		// 没配资产 = 本武器不使用后坐力。静默跳过，不打扰原有的 spread 链路。
		return;
	}

	RecoilState.SetGlobalScale(ULyraRecoilDebug::GetGlobalScale());

	// 开火前必须重新采一次玩家瞄准：鼠标输入与武器 tick 不一定同帧，
	// 用上一帧的值会让"第一发的基准"偏掉，整轮连发的压枪量跟着偏。
	//
	// ★ 2026-09-21：本轮连发的基准（AimPitchAtBurstStart）由 ApplyShot() 内部在
	//   `State == Idle` 时从 SampledAimPitch 锁定，无需在此另存一份。
	SampleRecoilPlayerAim();

	RecoilState.ApplyShot(Profile, ComputeRecoilPoseMultiplier(), ComputeRecoilPoseState());
}

FRecoilShotKick ULyraRangedWeaponInstance::GetRecoilShotDirectionOffset(int32 ShotIndex)
{
	// 弹道链取"这一刻"的倍率：瞄具混合权重逐帧变化，用上一发缓存值会让第一发偏掉
	RecoilState.SetGlobalScale(ULyraRecoilDebug::GetGlobalScale());
	RecoilState.SetPoseMultiplier(ComputeRecoilPoseMultiplier());

	// 开关语义（关闭/无资产 → 零偏移）收在 ComputeShotKickGated 里，
	// 这样"不污染 Lyra 原有纯扩散逻辑"这条约束只有一个实现点，且可被纯数值单测覆盖。
	return FRecoilRuntimeState::ComputeShotKickGated(
		RecoilProfile,
		ShotIndex,
		RecoilState.CurrentPoseMultiplier,
		RecoilState.GlobalScale,
		RecoilState.ActiveSeed,
		ULyraRecoilDebug::IsRecoilEnabled());
}

void ULyraRangedWeaponInstance::NotifyShotSpreadUsed(float SpreadAngleDegrees)
{
	RecoilState.SetPendingShotSpreadAngle(SpreadAngleDegrees);
}

void ULyraRangedWeaponInstance::UpdateRecoil(float DeltaSeconds)
{
	RecoilState.SetGlobalScale(ULyraRecoilDebug::GetGlobalScale());

	// 采样必须在 Advance 之前：本帧玩家压了多少枪，要参与本帧的回正目标计算。
	//
	// ★ 2026-09-21 收敛为单一入口：SamplePlayerAim() 是压枪量的**唯一定义点**，
	//   它内部会同步写入 AimCompensationPitch（P12 钳制 / P14 回正抵扣的消费字段）。
	//   此处**不再**单独调 SetAimCompensationPitch() ——
	//   旧写法用了一份平行的基准字段（BurstStartAimPitch），与 SamplePlayerAim 的
	//   AimPitchAtBurstStart 各锁各的，会出现"面板显示值 ≠ 实际生效值"。
	SampleRecoilPlayerAim();

	RecoilState.Advance(RecoilProfile, DeltaSeconds);

	// ---------------------------------------------------------------------
	// 散布推进（资产模型专用）
	//
	// 顺序有讲究：
	//   1) 先写玩家侧倍率 —— 这样"本帧最终生效锥角"在面板/CSV 上读到的是同一个值；
	//   2) 再推回落 —— AdvanceSpread 依赖 TimeSinceLastFire，而它刚刚被 Advance 更新过，
	//      所以必须排在 Advance 之后，否则用的会是上一帧的停火时长。
	//
	// 姿态取"当下"的姿态（与后坐力 P4 口径一致，不做插值）：蹲下/起跳立刻换一组
	// Base/Max/RecoverRate，这正是"蹲下马上变准、跳起来立刻散"的实现点。
	// ---------------------------------------------------------------------
	if (UsesProfileSpread())
	{
		RecoilState.SetSpreadPlayerMultipliers(
			RecoilProfile->GetSpreadAimingMultiplier(ComputeAimingAlpha()),
			SpreadMovementMultiplier);

		RecoilState.AdvanceSpread(RecoilProfile, DeltaSeconds, ComputeRecoilPoseState());
	}

	UpdateRecoilCameraModifier();

	// 工具层：屏幕面板（Lyra.Recoil.Debug）+ 世界内可视化（Lyra.Recoil.DebugDraw）
	ULyraRecoilDebug::DrawDebugPanel(GetWorld(), RecoilProfile, RecoilState);
	// 散布面板（Lyra.Recoil.SpreadDebug）：两条链路都能显示 ——
	// 走资产时读 RecoilState，走 Lyra 原生 heat 时读本实例上的三个成员。
	ULyraRecoilDebug::DrawSpreadDebugPanel(
		GetWorld(), RecoilProfile, RecoilState,
		CurrentHeat, CurrentSpreadAngle, CurrentSpreadAngleMultiplier);
	DrawRecoilDebug();
}

EPoseState ULyraRangedWeaponInstance::ComputeRecoilPoseState() const
{
	const APawn* Pawn = GetPawn();
	if (Pawn == nullptr)
	{
		return EPoseState::Standing;
	}

	UCharacterMovementComponent* MovementComp = Cast<UCharacterMovementComponent>(Pawn->GetMovementComponent());
	if (MovementComp == nullptr)
	{
		return EPoseState::Standing;
	}

	// 判定来源与 UpdateMultipliers() 完全一致（IsFalling / IsCrouching），但后坐力侧刻意不做插值：
	// 每发按"当下"的姿态取倍率，这样 P4 的"各姿态累计位移比值 == 配置倍率比值"才是精确的。
	//
	// 判定优先级与倍率换算都收在 FRecoilRuntimeState 的纯静态函数里，便于脱离 Pawn 做纯数值单测。
	return FRecoilRuntimeState::ResolvePoseState(MovementComp->IsCrouching(), MovementComp->IsFalling());
}

float ULyraRangedWeaponInstance::ComputeRecoilPoseMultiplier() const
{
	const ULyraRecoilProfile* Profile = RecoilProfile;
	if (Profile == nullptr)
	{
		return 1.0f;
	}

	// 姿态与瞄准正交：姿态倍率 × 瞄准混合倍率（见 LyraRecoilTypes.h 里 EPoseState 的说明）。
	// 换算逻辑收在纯静态函数里，便于脱离 Pawn 做纯数值单测。
	return FRecoilRuntimeState::ComputePoseMultiplier(*Profile, ComputeRecoilPoseState(), ComputeAimingAlpha());
}

void ULyraRangedWeaponInstance::SampleRecoilPlayerAim()
{
	// 本地玩家才有真实 ControlRotation（远程玩家的控制器不在本机）。
	// 非本地控制时直接跳过：压枪量保持 0，等价于"没人压枪"这个既有语义。
	const APawn* Pawn = GetPawn();
	if ((Pawn == nullptr) || !Pawn->IsLocallyControlled())
	{
		return;
	}

	// 只取显示相关的两个轴：Roll 是独立通道，不参与压枪量。
	const FRotator AimRotation = Pawn->GetControlRotation();
	RecoilState.SamplePlayerAim(AimRotation.Pitch, AimRotation.Yaw);
}

void ULyraRangedWeaponInstance::ResetRecoilState()
{
	RecoilState.Reset(RecoilProfile);
	RecoilState.SetGlobalScale(ULyraRecoilDebug::GetGlobalScale());
}

ALyraPlayerCameraManager* ULyraRangedWeaponInstance::GetOwningPlayerCameraManager() const
{
	const APawn* Pawn = GetPawn();
	if ((Pawn == nullptr) || !Pawn->IsLocallyControlled())
	{
		// 服务器上的远程玩家不需要本地相机链
		return nullptr;
	}

	const APlayerController* PC = Cast<const APlayerController>(Pawn->GetController());
	if (PC == nullptr)
	{
		return nullptr;
	}

	return Cast<ALyraPlayerCameraManager>(PC->PlayerCameraManager);
}

void ULyraRangedWeaponInstance::UpdateRecoilCameraModifier()
{
	ALyraPlayerCameraManager* CameraManager = GetOwningPlayerCameraManager();
	if (CameraManager == nullptr)
	{
		RecoilCameraModifier = nullptr;
		return;
	}

	if (RecoilCameraModifier == nullptr)
	{
		// 先找现成的，避免反复装备/卸下时在相机上堆出多个同类修改器
		RecoilCameraModifier = Cast<UCameraModifier_WeaponRecoil>(
			CameraManager->FindCameraModifierByClass(UCameraModifier_WeaponRecoil::StaticClass()));

		if (RecoilCameraModifier == nullptr)
		{
			RecoilCameraModifier = Cast<UCameraModifier_WeaponRecoil>(
				CameraManager->AddNewCameraModifier(UCameraModifier_WeaponRecoil::StaticClass()));
		}
	}

	if (RecoilCameraModifier == nullptr)
	{
		return;
	}

	if (ULyraRecoilDebug::IsRecoilEnabled())
	{
		// 三轴一起推给相机修改器：
		// Pitch/Yaw 是"累加-回正"的积分量，Roll 是"衰减包络 × 周期项"的瞬时解。
		// 语义不同，但都只作用在显示层 POV 上，所以在相机修改器里合流。
		//
		// **Pitch/Yaw 读的是补间输出，不是逻辑偏移**（见 LyraRecoilState.h 的说明）：
		//   InstantWrite 模式下两者恒等 —— 既有行为零变化；
		//   Interpolated 模式下补间输出是逐帧差分累加的结果，逻辑偏移是"这一发应该抬到哪"。
		// 相机每帧只能转一点，所以必须读补间输出。
		//
		// Roll 额外乘一个调试倍率（Lyra.Recoil.RollShake，默认 1）：
		// 这是**纯显示层缩放**，不写回 RecoilState，因此不会污染 ShotHistory / CSV / Golden 数据。
		// 0 就是"临时关掉 Roll 做 A/B 对比"，比改资产再重载快得多。
		const float RollDisplayOffset = RecoilState.GetCameraRollOffset() * ULyraRecoilDebug::GetRollShakeScale();

		RecoilCameraModifier->SetRecoilOffset(
			RecoilState.GetCameraPitchOffset(),
			RecoilState.GetCameraYawOffset(),
			RollDisplayOffset);
	}
	else
	{
		RecoilCameraModifier->ClearRecoilOffset();
	}

	// ---------------------------------------------------------------------
	// 诊断跟踪（Lyra.Recoil.Trace，默认关闭）。
	//
	// 排查"后坐力的偏移到底有没有真的上到相机上"时，光看屏幕/面板无法区分两种成因：
	//   ① 上游状态链断了 —— push 本身就是 0
	//   ② push 非零，但施加到显示层的量被别的东西覆盖了 —— delta 是 0
	// 这两类的修法完全不同，所以这里把三个量并排打出来：
	//   push  = 本帧推给相机修改器的目标（= 状态层的补间输出 CameraOffset）
	//   Ctrl  = 玩家的控制旋转（鼠标输入直接写的就是它，真值基准）
	//   POV   = 相机管理器最终给出的朝向（已含相机修改器的施加结果）
	//   delta = POV − Ctrl，即修改器**实际**作用到显示层的角度（UnwindDegrees 处理 ±360 环绕）
	//
	// 2026-09-22 追加**位置链**取证（用户反馈"开火时相机明显上移，不该只是旋转"）：
	// 本系统的修改器只写 Rotation，位置理论上只随 Pawn / 控制旋转走。要区分"真平移"与
	// "别的相机修改器注入的位置量"（头号嫌疑：GCN_Weapon_Rifle_Fire 播放的 Legacy CameraShake
	// —— CS_Weapon_Fire_Rifle，自带位置震荡），再打四个量：
	//   POVLoc   = 相机管理器最终位置（含全部修改器，渲染真值）
	//   StackLoc = 相机组件位置（相机模式栈输出，**不含**修改器）—— POVLoc−StackLoc 就是
	//              所有修改器合计的位置量；后坐力只写 Rotation，若这里非零，来源必然是别的修改器
	//   PawnLoc / CamRel = Pawn 世界位置 / POVLoc 换到 Pawn 本地系（人物移动时也成立）
	//   LocD     = POVLoc 逐帧差分（>500cm 视为瞬移/换局，重置基线）
	//
	// 注意 POV 是"上一帧"的结果（相机管理器的 tick 与武器 tick 不同步），
	// 所以这里看的是趋势而不是逐帧精确对应。
	// ---------------------------------------------------------------------
	if (LyraRecoilWeaponPrivate::bRecoilTrace)
	{
		const APawn* TracePawn = GetPawn();
		const FRotator POVRot = CameraManager->GetCameraRotation();
		const FRotator CtrlRot = (TracePawn != nullptr) ? TracePawn->GetControlRotation() : FRotator::ZeroRotator;

		static FVector LastPOVLoc = FVector::ZeroVector;
		static bool bHasLastPOVLoc = false;

		const FVector POVLoc = CameraManager->GetCameraLocation();
		FVector PawnLoc = FVector::ZeroVector;
		FVector CamRel = FVector::ZeroVector;
		FVector StackLoc = FVector::ZeroVector;
		if (TracePawn != nullptr)
		{
			PawnLoc = TracePawn->GetActorLocation();
			CamRel = TracePawn->GetActorTransform().InverseTransformPosition(POVLoc);
			if (const ULyraCameraComponent* TraceCamComp = TracePawn->FindComponentByClass<ULyraCameraComponent>())
			{
				StackLoc = TraceCamComp->GetComponentLocation();
			}
		}
		const FVector LocD = (bHasLastPOVLoc && FVector::DistSquared(POVLoc, LastPOVLoc) < FMath::Square(500.0f))
			? (POVLoc - LastPOVLoc)
			: FVector::ZeroVector;
		LastPOVLoc = POVLoc;
		bHasLastPOVLoc = true;

		UE_LOG(LogLyraRecoilWeapon, Log,
			TEXT("[RecoilTrace] enable=%d mode=%s state=%d stage=%d | push=(%.4f,%.4f,%.4f) | Ctrl=(%.3f,%.3f) POV=(%.3f,%.3f,%.3f) delta=(%.4f,%.4f,%.4f) | aimBase=%.3f aimNow=%.3f pushComp=%.3f | cover=%.3f coverUsed=%.3f applied=%d | peak=(%.3f,%.3f) acc=(%.3f,%.3f) | POVLoc=(%.2f,%.2f,%.2f) StackLoc=(%.2f,%.2f,%.2f) PawnLoc=(%.2f,%.2f,%.2f) CamRel=(%.3f,%.3f,%.3f) LocD=(%.3f,%.3f,%.3f)"),
			ULyraRecoilDebug::IsRecoilEnabled() ? 1 : 0,
			(RecoilProfile != nullptr && RecoilProfile->IsInterpolatedSingleShot()) ? TEXT("Interpolated") : TEXT("InstantWrite"),
			static_cast<int32>(RecoilState.State),
			static_cast<int32>(RecoilState.InterpStage),
			RecoilState.GetCameraPitchOffset(),
			RecoilState.GetCameraYawOffset(),
			RecoilState.GetCameraRollOffset() * ULyraRecoilDebug::GetRollShakeScale(),
			CtrlRot.Pitch, CtrlRot.Yaw,
			POVRot.Pitch, POVRot.Yaw, POVRot.Roll,
			FMath::UnwindDegrees(POVRot.Pitch - CtrlRot.Pitch),
			FMath::UnwindDegrees(POVRot.Yaw - CtrlRot.Yaw),
			FMath::UnwindDegrees(POVRot.Roll - CtrlRot.Roll),
			// ↓ 2026-09-21 追加：压枪量链路（排查"回正有没有按玩家压枪量扣"）
			RecoilState.AimPitchAtBurstStart,
			RecoilState.SampledAimPitch,
			RecoilState.PlayerCompensationPitch,
			RecoilState.RecoveryCoverPitch,
			RecoilState.RecoveryCompensationPitch,
			RecoilState.bRecoveryCoverApplied ? 1 : 0,
			RecoilState.RecoveryPeakPitch,
			RecoilState.RecoveryPeakYaw,
			RecoilState.AccumulatedPitch,
			RecoilState.AccumulatedYaw,
			// ↓ 2026-09-22 追加：位置链取证（排查"开火时相机是否真的在平移、是谁在动它"）
			POVLoc.X, POVLoc.Y, POVLoc.Z,
			StackLoc.X, StackLoc.Y, StackLoc.Z,
			PawnLoc.X, PawnLoc.Y, PawnLoc.Z,
			CamRel.X, CamRel.Y, CamRel.Z,
			LocD.X, LocD.Y, LocD.Z);
	}
}

void ULyraRangedWeaponInstance::ClearRecoilCameraOffset()
{
	// 目的：让相机上可能残留的偏移**平滑归零**，而不是一帧砍断。
	//
	// 为什么不再 RemoveCameraModifier：
	//   摘掉修改器的瞬间，它正在施加的偏移会立刻消失。若此时玩家刚打完一轮、
	//   相机上还挂着几度残留（Rifle 的 MaxVerticalKick 是 8 度），
	//   这一点残留在一帧内归零就是一次可见的跳变 —— 也就是反馈里的"切枪震屏"。
	//   修改器本身是常驻在 PlayerCameraManager 上的，让它自己衰减回 0 即可。
	if (RecoilCameraModifier != nullptr)
	{
		RecoilCameraModifier->ClearRecoilOffset();
	}

	// 缓存指针可以丢掉：下一个武器实例会通过 FindCameraModifierByClass 重新拿到同一个修改器。
	RecoilCameraModifier = nullptr;
}

void ULyraRangedWeaponInstance::DrawRecoilDebug()
{
	// 屏幕面板（数值 + Roll 波形）不受 ENABLE_DRAW_DEBUG 限制：
	// 它们是纯 GEngine->AddOnScreenDebugMessage，只要不是 Shipping 构建就能用。
	// 分开判断是因为两者的 CVar 独立 —— 只想看波形时不必把世界线也画出来。
	if (const APawn* DebugPawn = GetPawn())
	{
		if (DebugPawn->IsLocallyControlled())
		{
			const UWorld* DebugWorld = GetWorld();

			if (ULyraRecoilDebug::IsDebugPanelEnabled())
			{
				ULyraRecoilDebug::DrawDebugPanel(DebugWorld, RecoilProfile, RecoilState);
			}

			if (ULyraRecoilDebug::IsRollDebugPanelEnabled())
			{
				ULyraRecoilDebug::DrawRollShakeDebugPanel(DebugWorld, RecoilProfile, RecoilState);
			}
		}
	}

#if ENABLE_DRAW_DEBUG
	// 注意：LyraGame.Build.cs 定义了 SHIPPING_DRAW_DEBUG_ERROR=1，
	// DrawDebug 系列必须包在 #if ENABLE_DRAW_DEBUG 里。
	if (!ULyraRecoilDebug::IsDebugDrawEnabled())
	{
		return;
	}

	const APawn* Pawn = GetPawn();
	if ((Pawn == nullptr) || !Pawn->IsLocallyControlled())
	{
		return;
	}

	// 起点取"眼睛高度"，与 GA 的 GetWeaponTargetingSourceLocation 同一量级，
	// 方便把画出来的方向与实际弹道对照。
	const FVector Origin = Pawn->GetActorLocation() + FVector(0.0f, 0.0f, Pawn->BaseEyeHeight);
	const FRotator AimRotation = Pawn->GetControlRotation();

	ULyraRecoilDebug::DrawWorldDebug(GetWorld(), Origin, AimRotation, RecoilProfile, RecoilState);
#endif // ENABLE_DRAW_DEBUG
}

void ULyraRangedWeaponInstance::ReloadRecoilProfile()
{
#if WITH_EDITOR
	if (RecoilProfile == nullptr)
	{
		UE_LOG(LogLyraRecoilWeapon, Warning, TEXT("ReloadRecoilProfile: no recoil profile assigned on %s"), *GetName());
		return;
	}

	ULyraRecoilProfile* OldProfile = RecoilProfile;
	UPackage* OldPackage = OldProfile->GetOutermost();
	if (OldPackage == nullptr)
	{
		UE_LOG(LogLyraRecoilWeapon, Error, TEXT("ReloadRecoilProfile: profile has no outer package"));
		return;
	}

	const FString PackageName = OldPackage->GetName();
	const FString AssetName = OldProfile->GetName();

	// 先断开引用。否则旧对象仍被本实例持有 → GC 判定可达 → 同名包不会被真正卸载，
	// 后面 LoadPackage 只会把内存里的旧包原样还回来 —— 看起来"成功"了，其实没重载。
	RecoilProfile = nullptr;

	ResetLoaders(OldPackage);
	OldPackage->SetDirtyFlag(false);
	OldPackage->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
	OldPackage->MarkAsGarbage();

	// bPerformFullPurge = true 是必须的：只标记不回收的话包还在内存里，重载会拿到旧数据
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPerformFullPurge=*/ true);

	UPackage* NewPackage = LoadPackage(nullptr, *PackageName, LOAD_None);
	ULyraRecoilProfile* NewProfile = (NewPackage != nullptr)
		? FindObject<ULyraRecoilProfile>(NewPackage, *AssetName)
		: nullptr;

	RecoilProfile = NewProfile;

	// 重载后偏移必须清零，否则会带着旧曲线的残留继续算
	ResetRecoilState();

	if (NewProfile != nullptr)
	{
		UE_LOG(LogLyraRecoilWeapon, Display, TEXT("ReloadRecoilProfile: reloaded %s from disk"), *PackageName);
	}
	else
	{
		UE_LOG(LogLyraRecoilWeapon, Error, TEXT("ReloadRecoilProfile: FAILED to reload %s (asset name=%s) - profile is now unset"), *PackageName, *AssetName);
	}
#endif // WITH_EDITOR
}
