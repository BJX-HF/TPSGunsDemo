#pragma once

#include "CoreMinimal.h"
#include "LyraWeaponVisualRecoilState.generated.h"

/** 六轴武器外观后坐力，通过每把武器的轴向校准映射到手部骨骼。 */
USTRUCT(BlueprintType)
struct FWeaponVisualRecoilSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ToolTip = "启用这把武器的外观后坐力。\n控制枪身旋转和位移，不改变镜头后坐力或弹道；默认关闭。"))
	bool bEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.0", ToolTip = "武器外观后坐力的独立强度倍率。\n调大时枪身旋转和位移更明显，不改变镜头后坐力或弹道。"))
	float VisualScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.0", ToolTip = "本发实际垂直后坐力每增加1度，产生的枪身俯仰角度。\n调大时枪口上抬更明显，另受外观强度和瞄准外观倍率影响。"))
	float PitchFromVerticalKick = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Impulse", meta = (ClampMin = "0.0", ToolTip = "本发实际水平后坐力每增加1度，产生的枪身左右偏转角度。\n保留本发水平后坐力的正负方向，另受外观强度和瞄准外观倍率影响。"))
	float YawFromHorizontalKick = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Impulse", meta = (ToolTip = "本发枪身左右偏转每增加1度，产生的侧倾角度。\n保留偏转方向；设为负数可反转侧倾方向，设为0不产生此项侧倾。"))
	float RollFromYaw = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Impulse", meta = (ClampMin = "0.0", ForceUnits = cm, ToolTip = "每发枪身上移的基础距离（厘米）。\n实际距离还会乘以姿态、全局后坐力、外观强度和瞄准外观倍率。"))
	float UpCmPerShot = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Impulse", meta = (ClampMin = "0.0", ForceUnits = cm, ToolTip = "每发枪身侧移的基础距离（厘米）。\n方向跟随本发实际水平后坐力；水平后坐力为0时不产生侧移。实际距离还受姿态、全局和外观倍率影响。"))
	float SideCmPerShot = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Impulse", meta = (ClampMin = "0.0", ToolTip = "瞄准时额外施加的武器外观倍率。\n随实际瞄准程度从1平滑过渡到本值；小于1减弱外观后坐力。实际单发后坐力已包含玩法层的瞄准倍率，此项只额外影响枪身外观。"))
	float ADSVisualScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.0", ForceUnits = cm, ToolTip = "每发枪身后退的基础距离（厘米）。\n实际距离还会乘以姿态、全局后坐力、外观强度和瞄准外观倍率。"))
	float BackCmPerShot = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.001", ForceUnits = s, ToolTip = "从开火到枪身外观后坐力达到本发目标峰值的时间（秒）。\n越小冲击越快；连发时新一发会从当前姿态重新向新峰值推进。"))
	float AttackDuration = 0.04f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.0", ForceUnits = s, ToolTip = "到达外观峰值后保持的时间（秒）。\n短暂保持可让低帧率下的开火冲击更容易看见，保持结束后开始回正。"))
	float PeakHoldDuration = 0.025f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.001", ForceUnits = s, ToolTip = "枪身外观后坐力指数回正的时间常数（秒）。\n越小回正越快，越大回正越慢；这是衰减速度参数，并非完全回正所需的总时间。"))
	float ReturnTime = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.0", ForceUnits = deg, ToolTip = "枪身外观俯仰角度的绝对上限（度）。\n限制连发累积的抬枪幅度，也限制叠加镜头俯仰对齐后的最终角度。"))
	float MaxPitchDegrees = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Clamp", meta = (ClampMin = "0.0", ForceUnits = deg, ToolTip = "枪身左右偏转角度的绝对上限（度）。\n限制连发累积的左右摆动，也限制叠加镜头水平对齐后的最终角度。"))
	float MaxYawDegrees = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Clamp", meta = (ClampMin = "0.0", ForceUnits = deg, ToolTip = "枪身侧倾角度的绝对上限（度），用于限制连发时累积的左右侧倾。"))
	float MaxRollDegrees = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ClampMin = "0.0", ForceUnits = cm, ToolTip = "连发时枪身累积后退距离的上限（厘米）。"))
	float MaxBackCm = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Clamp", meta = (ClampMin = "0.0", ForceUnits = cm, ToolTip = "连发时枪身累积上移距离的上限（厘米）。"))
	float MaxUpCm = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Clamp", meta = (ClampMin = "0.0", ForceUnits = cm, ToolTip = "连发时枪身左右侧移距离的绝对上限（厘米）。"))
	float MaxSideCm = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Alignment", meta = (ClampMin = "0.0", ToolTip = "将现有镜头俯仰后坐力按本倍率叠加到枪身外观角度。\n设为0关闭；建议使用较小倍率，让枪身轻微跟随镜头。本项不反向影响镜头。"))
	float CameraPitchAlignment = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Alignment", meta = (ClampMin = "0.0", ToolTip = "将现有镜头水平后坐力按本倍率叠加到枪身左右偏转角度。\n设为0关闭；建议使用较小倍率，让枪身轻微跟随镜头。本项不反向影响镜头。"))
	float CameraYawAlignment = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Alignment", meta = (ClampMin = "0.0", ForceUnits = deg, ToolTip = "镜头对齐额外产生的角度上限（度）。\n俯仰与水平两个对齐分量分别限幅，叠加后的枪身角度仍受各自外观角度上限限制。"))
	float MaxAlignmentDegrees = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ToolTip = "右手骨骼局部空间中，枪身后退的方向向量。\n运行时会归一化此向量，只使用方向；启用前请在实际武器模型上校准。"))
	FVector BackAxisBoneSpace = FVector(-1.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Axes", meta = (ToolTip = "右手骨骼局部空间中，枪身上移的方向向量。\n运行时会归一化此向量，请按实际武器和骨骼朝向校准。"))
	FVector UpAxisBoneSpace = FVector(0.0f, 0.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Axes", meta = (ToolTip = "右手骨骼局部空间中，枪身正向侧移的方向向量。\n运行时会归一化此向量，反向侧移使用相反方向。"))
	FVector SideAxisBoneSpace = FVector(0.0f, 1.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual", meta = (ToolTip = "枪身外观俯仰每增加1度，映射到右手骨骼局部空间的旋转量。\n这是旋转映射系数，不会归一化；启用前请校准轴向和正负号。"))
	FRotator PitchAxisBoneSpace = FRotator(1.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Axes", meta = (ToolTip = "枪身正向水平偏转每增加1度，映射到右手骨骼局部空间的旋转量。\n按实际骨骼校准旋转轴、倍率和正负号。"))
	FRotator YawAxisBoneSpace = FRotator(0.0f, 1.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Axes", meta = (ToolTip = "枪身正向侧倾每增加1度，映射到右手骨骼局部空间的旋转量。\n按实际骨骼校准旋转轴、倍率和正负号。"))
	FRotator RollAxisBoneSpace = FRotator(0.0f, 0.0f, 1.0f);

	// 右手逆向运动学目标位于独立骨骼分支，局部轴向可能与右手骨骼不同；启用前应分别校准。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK Axes", meta = (ToolTip = "右手逆向运动学目标骨骼（ik_hand_r）局部空间中的后退方向。\n运行时会归一化；应独立于右手骨骼（hand_r）校准，不能直接假定两者轴向一致。"))
	FVector TargetBackAxisBoneSpace = FVector(-1.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK Axes", meta = (ToolTip = "右手逆向运动学目标骨骼（ik_hand_r）局部空间中的上移方向。\n运行时会归一化，请按目标骨骼的实际轴向校准。"))
	FVector TargetUpAxisBoneSpace = FVector(0.0f, 0.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK Axes", meta = (ToolTip = "右手逆向运动学目标骨骼（ik_hand_r）局部空间中的正向侧移方向。\n运行时会归一化，反向侧移使用相反方向。"))
	FVector TargetSideAxisBoneSpace = FVector(0.0f, 1.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK Axes", meta = (ToolTip = "枪身外观俯仰每增加1度，映射到右手逆向运动学目标骨骼（ik_hand_r）局部空间的旋转量。\n应独立校准目标骨骼的旋转轴、倍率和正负号。"))
	FRotator TargetPitchAxisBoneSpace = FRotator(1.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK Axes", meta = (ToolTip = "枪身水平偏转每增加1度，映射到右手逆向运动学目标骨骼（ik_hand_r）局部空间的旋转量。\n应独立校准目标骨骼的旋转轴、倍率和正负号。"))
	FRotator TargetYawAxisBoneSpace = FRotator(0.0f, 1.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK Axes", meta = (ToolTip = "枪身侧倾每增加1度，映射到右手逆向运动学目标骨骼（ik_hand_r）局部空间的旋转量。\n应独立校准目标骨骼的旋转轴、倍率和正负号。"))
	FRotator TargetRollAxisBoneSpace = FRotator(0.0f, 0.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK", meta = (ToolTip = "在右臂两骨骼逆向运动学求解前，驱动右手目标骨骼（ik_hand_r）应用外观后坐力。\n开启前请完成该武器的轴向和握持姿势校准；关闭时使用直接驱动右手骨骼的方式。"))
	bool bDriveRightHandIKTarget = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|IK", meta = (ToolTip = "右手应用外观后坐力后、左臂两骨骼逆向运动学求解前，使用武器空间虚拟骨骼刷新左手目标（ik_hand_l）。\n让左手跟随武器运动，保持握持位置。"))
	bool bFollowWeaponWithLeftHand = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Performance", meta = (ClampMin = "0", ToolTip = "允许显示武器外观后坐力的最大角色模型细节等级。\n当前细节等级编号大于本值时，淡出枪身外观后坐力；镜头后坐力和弹道继续正常运行。"))
	int32 MaxVisualLOD = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Animation", meta = (ToolTip = "用于抑制武器外观后坐力的动画曲线名称。\n曲线值0表示不抑制，1表示完全抑制；中间值按比例减弱，适用于换弹、装备等动画。"))
	FName SuppressionCurveName = TEXT("DisableHandIKRetargeting");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Animation", meta = (ClampMin = "0.0", ForceUnits = s, ToolTip = "动画抑制减弱后，武器外观后坐力恢复显示的混合时间参数（秒）。\n越大恢复越平缓，设为0立即恢复。"))
	float BlendInTime = 0.08f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon Visual|Animation", meta = (ClampMin = "0.0", ForceUnits = s, ToolTip = "动画抑制增强或超过允许的模型细节等级时，武器外观后坐力淡出的混合时间参数（秒）。\n越大淡出越平缓，设为0立即淡出。"))
	float BlendOutTime = 0.04f;
};

USTRUCT(BlueprintType)
struct FWeaponVisualRecoilPose
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float PitchDegrees = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float YawDegrees = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float RollDegrees = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float BackCm = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float UpCm = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float SideCm = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	float Alpha = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Weapon Visual")
	int32 ShotSerial = 0;
};

/** 纯数值状态。实际开火驱动六个机械轴；镜头对齐只影响输出姿态。 */
struct FWeaponVisualRecoilState
{
	static constexpr float FixedStepSeconds = 1.0f / 120.0f;

	void Reset();
	bool ApplyShot(const FWeaponVisualRecoilSettings& Settings, int32 InShotSerial, float VerticalKick, float PoseMultiplier, float GlobalScale, float HorizontalKick = 0.0f, float AimingAlpha = 0.0f);
	void Advance(const FWeaponVisualRecoilSettings& Settings, float DeltaSeconds, float CameraPitch = 0.0f, float CameraYaw = 0.0f);
	const FWeaponVisualRecoilPose& GetPose() const { return Pose; }

private:
	void Step(const FWeaponVisualRecoilSettings& Settings);
	void PublishPose(const FWeaponVisualRecoilSettings& Settings, float CameraPitch, float CameraYaw);

	FWeaponVisualRecoilPose Pose;
	FWeaponVisualRecoilPose MechanicalPose;
	FWeaponVisualRecoilPose AttackStart;
	FWeaponVisualRecoilPose AttackTarget;
	float AttackElapsed = 0.0f;
	float HoldElapsed = 0.0f;
	float StepAccumulator = 0.0f;
	bool bAttacking = false;
	bool bHolding = false;
};
