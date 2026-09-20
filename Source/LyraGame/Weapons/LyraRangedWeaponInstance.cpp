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
	ComputeHeatRange(/*out*/ Debug_MinHeat, /*out*/ Debug_MaxHeat);
	ComputeSpreadRange(/*out*/ Debug_MinSpreadAngle, /*out*/ Debug_MaxSpreadAngle);
	Debug_CurrentHeat = CurrentHeat;
	Debug_CurrentSpreadAngle = CurrentSpreadAngle;
	Debug_CurrentSpreadAngleMultiplier = CurrentSpreadAngleMultiplier;
}
#endif

void ULyraRangedWeaponInstance::OnEquipped()
{
	Super::OnEquipped();

	// Start heat in the middle
	float MinHeatRange;
	float MaxHeatRange;
	ComputeHeatRange(/*out*/ MinHeatRange, /*out*/ MaxHeatRange);
	CurrentHeat = (MinHeatRange + MaxHeatRange) * 0.5f;

	// Derive spread
	CurrentSpreadAngle = HeatToSpreadCurve.GetRichCurveConst()->Eval(CurrentHeat);

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

	bHasFirstShotAccuracy = bAllowFirstShotAccuracy && bMinMultipliers && bMinSpread;

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

void ULyraRangedWeaponInstance::UpdateRecoil(float DeltaSeconds)
{
	RecoilState.SetGlobalScale(ULyraRecoilDebug::GetGlobalScale());

	// 采样必须在 Advance 之前：本帧玩家压了多少枪，要参与本帧的回正目标计算。
	SampleRecoilPlayerAim();

	RecoilState.Advance(RecoilProfile, DeltaSeconds);

	UpdateRecoilCameraModifier();

	// 工具层：屏幕面板（Lyra.Recoil.Debug）+ 世界内可视化（Lyra.Recoil.DebugDraw）
	ULyraRecoilDebug::DrawDebugPanel(GetWorld(), RecoilProfile, RecoilState);
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
	//   delta = POV − Ctrl，即修改器**实际**作用到显示层的角度
	//
	// 注意 POV 是"上一帧"的结果（相机管理器的 tick 与武器 tick 不同步），
	// 所以这里看的是趋势而不是逐帧精确对应。
	// ---------------------------------------------------------------------
	if (LyraRecoilWeaponPrivate::bRecoilTrace)
	{
		const APawn* TracePawn = GetPawn();
		const FRotator POVRot = CameraManager->GetCameraRotation();
		const FRotator CtrlRot = (TracePawn != nullptr) ? TracePawn->GetControlRotation() : FRotator::ZeroRotator;

		UE_LOG(LogLyraRecoilWeapon, Log,
			TEXT("[RecoilTrace] enable=%d mode=%s state=%d stage=%d | push=(%.4f,%.4f) | Ctrl=(%.3f,%.3f) POV=(%.3f,%.3f) delta=(%.4f,%.4f)"),
			ULyraRecoilDebug::IsRecoilEnabled() ? 1 : 0,
			(RecoilProfile != nullptr && RecoilProfile->IsInterpolatedSingleShot()) ? TEXT("Interpolated") : TEXT("InstantWrite"),
			static_cast<int32>(RecoilState.State),
			static_cast<int32>(RecoilState.InterpStage),
			RecoilState.GetCameraPitchOffset(),
			RecoilState.GetCameraYawOffset(),
			CtrlRot.Pitch, CtrlRot.Yaw,
			POVRot.Pitch, POVRot.Yaw,
			POVRot.Pitch - CtrlRot.Pitch,
			POVRot.Yaw - CtrlRot.Yaw);
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
