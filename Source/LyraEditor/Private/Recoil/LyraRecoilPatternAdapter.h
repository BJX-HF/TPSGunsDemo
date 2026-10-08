// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Weapons/Recoil/LyraRecoilTypes.h"

class ULyraRecoilProfile;

/**
 * P1：后坐力 GUI 编辑器的**无 UI 数据模型**（开发计划 §11 LyraRecoilPatternAdapter）。
 *
 * 职责边界：
 *  - Profile → 累计点 / 累计点 → 归一化字段 / 结构命令 / 剪贴板 的纯数值逻辑；
 *  - **不包含任何 Slate / UObject 写入 / 事务 / 通知代码**，因此可以脱离编辑器 UI 单测；
 *  - 所有函数只生成**候选数据**（FLyraRecoilPatternData），不修改传入的 UObject；
 *  - 失败时原子拒绝：输出参数保持源数据，调用方不会看到半写入结果。
 *
 * 单位与语义（与 §05 一致）：
 *  - H = RecoilPerShot_Horizontal，V = RecoilPerShot_Vertical，c[i] = GetVerticalKickCurveScale(i)；
 *  - 固定段 0 ≤ i < L：ΔYaw[i] = H × X[i]，ΔPitch[i] = V × Y[i] × c[i]，P[i] = P[i-1] + Δ[i]，P[-1] = (0,0)；
 *  - 反算：X = ΔYaw / H，Y = ΔPitch / (V × c[i])；
 *  - 中间量一律 double，仅在写回 float 字段时降精度，并做正向重建容差校验。
 */
struct FLyraRecoilPatternData
{
	/** 原始归一化逐发参数（与 ULyraRecoilProfile::PatternPoints 同序）。 */
	TArray<FRecoilPatternPoint> Points;

	/** 固定区间长度（与 ULyraRecoilProfile::PatternLength 同义）。 */
	int32 PatternLength = 0;
};

/**
 * 候选转换的诊断记录：只报告误差，不表示成功/失败。
 * 计划 §06 要求“在诊断记录最大误差”，阈值内的边界归并不得声称无损。
 */
struct FLyraRecoilConversionReport
{
	/** 候选写回 float 后正向重建与目标累计角度的最大绝对误差（度）。 */
	double MaxAngleRebuildError = 0.0;

	/** 归一化值因容差被归并到 [-1,1] / [0,1] 边界的最大误差。 */
	double MaxNormalizedClampError = 0.0;

	/** 发生过边界归并的发序号（去重、升序无关）。 */
	TArray<int32> BoundaryClampedIndices;
};

/**
 * 结构化剪贴板载荷（§07 “LyraRecoilClipboard/v1”）。
 * 同时保留原始归一化值与可选的源逐发角度，供“粘贴归一化参数 / 按角度粘贴”两种模式使用。
 */
struct FLyraRecoilClipboardData
{
	/** 源资产中的发序号（按序号升序存储；仅作元信息）。 */
	TArray<int32> SourceIndices;

	/** 原始归一化参数（按数组顺序即粘贴顺序）。 */
	TArray<FRecoilPatternPoint> Points;

	/** 与 Points 平行的源逐发角度（度）；载荷不带角度时为空。 */
	TArray<double> SourceYawDegrees;
	TArray<double> SourcePitchDegrees;
	TArray<double> SourceVerticalScales;

	/** 源资产的每发基础强度（仅元信息，粘贴时不直接套用）。 */
	double SourceHorizontal = 0.0;
	double SourceVertical = 0.0;

	/** 载荷是否携带完整的角度三元组（yaw/pitch/scale 全部存在）。 */
	bool bHasSourceAngles = false;
};

/**
 * 高级命令的尾段采样行：数组内尾段（L ≤ i < N）与数组外（i ≥ N）的逐发前后 Kick 对比。
 *
 * 数组内尾段：命令会同步重新归一化 Y 以保留原逐发垂直 Kick，X 原样保留（水平由随机游走决定）；
 * 数组外：垂直沿用最后一点 Y、曲线继续按发序号求值，因此不保证无损，必须由差异面板展示。
 */
struct FLyraRecoilTailSample
{
	/** 发序号（0 起）。 */
	int32 ShotIndex = 0;

	/** 该行使用的固定种子。 */
	int32 Seed = 0;

	/** 该发的水平随机游走归一化值（现有 ComputePatternHorizontal 结果，只读）。 */
	double WalkHorizontal = 0.0;

	double OldHorizontalKick = 0.0;
	double NewHorizontalKick = 0.0;

	double OldVerticalKick = 0.0;
	double NewVerticalKick = 0.0;

	/** true = 该发在数组外（i ≥ N）。 */
	bool bBeyondArray = false;
};

/**
 * 高级“调整基础强度以容纳候选”的分析结果（§06 末节）。
 * 只生成候选与差异，绝不自动修改任何资产字段。
 */
struct FLyraRecoilStrengthAdjustment
{
	/** 约束是否可满足（可满足即有可用候选；与“是否需要变化”无关）。 */
	bool bApplicable = false;

	double OldHorizontal = 0.0;
	double OldVertical = 0.0;
	double NewHorizontal = 0.0;
	double NewVertical = 0.0;

	/** 逐字段差异（人类可读，供差异面板展示）。 */
	TArray<FString> Differences;

	/** 原分母为 0 / 极小、新分母可逆，因此原始隐藏值必须被写成 0 的发序号。 */
	TArray<int32> HiddenValuesForcedToZero;

	/** 命令不可用的原因（零曲线、方向不相容、非有限等）。 */
	TArray<FString> BlockingReasons;

	/** 本次分析使用的目标累计点（固定段）。 */
	TArray<FVector2D> TargetCumulativePoints;
};

/**
 * 统一转换接口。静态无状态；所有函数都不写真实 UObject。
 */
class FLyraRecoilPatternAdapter
{
public:
	/** 反算锁定阈值（§06：|分母| ≤ 1e-8 视为不可逆，禁止用 epsilon 代分母）。 */
	static constexpr double DenominatorEpsilon = 1.0e-8;

	/** 归一化边界容差（§06 浮点边界：仅允许容差内舍入归并到边界）。 */
	static constexpr double NormalizedTolerance = 1.0e-6;

	/** 角度重建容差 = 绝对值 + 相对值 × |目标角度|。 */
	static constexpr double AngleToleranceAbsolute = 1.0e-5;
	static constexpr double AngleToleranceRelative = 1.0e-6;

	/** 剪贴板保护值（编辑器限制，不是运行时资产上限）。 */
	static constexpr int32 MaxClipboardPoints = 4096;
	static constexpr int32 MaxClipboardBytes = 1024 * 1024;

	// -----------------------------------------------------------------
	// 必需接口
	// -----------------------------------------------------------------

	/** 读取当前资产值（不 sanitize、不 clamp；L 的最终值以资产为准）。 */
	static FLyraRecoilPatternData Read(const ULyraRecoilProfile& Profile);

	/**
	 * 全 Profile 校验：现有 ValidateProfile + 编辑器附加的有限数检查。
	 * const 只读，不触发属性通知、不标脏；返回 false 时 Errors 至少追加一条。
	 */
	static bool Validate(const ULyraRecoilProfile& Profile, TArray<FString>& Errors);

	/**
	 * 只构建前 L 个固定点的累计角度（X = Yaw，Y = Pitch，单位度）。
	 * 失败时 OutPoints 为空、Errors 记录原因；H/V 允许为 0 或负（既有非法数据仍可展示）。
	 */
	static bool BuildCumulative(const ULyraRecoilProfile& Profile, TArray<FVector2D>& OutPoints, TArray<FString>& Errors);

	/**
	 * 用目标累计点反算固定段归一化字段，生成候选数据。
	 *  - Targets.Num() 必须等于 PatternLength；
	 *  - 未变化的增量轴**保留原始 float 值**（不往返重算）；
	 *  - 分母不可逆且目标增量为零时保留原始隐藏值；
	 *  - 目标增量非零但分母不可逆、超界、非有限时整组拒绝（OutData = 源数据）。
	 */
	static bool MoveCumulative(const ULyraRecoilProfile& Profile, const TArray<FVector2D>& Targets,
		FLyraRecoilPatternData& OutData, TArray<FString>& Errors);

	/** MoveCumulative 的诊断重载；额外输出最大重建/归并误差。 */
	static bool MoveCumulative(const ULyraRecoilProfile& Profile, const TArray<FVector2D>& Targets,
		FLyraRecoilPatternData& OutData, FLyraRecoilConversionReport& OutReport, TArray<FString>& Errors);

	/**
	 * 在 Index 处插入 NewPoints（§07 边界规则）。
	 *  - 0 ≤ Index ≤ N；
	 *  - Index < L → L' = L + m；Index == L → 默认尾段（L 不变），bFixedAtBoundary=true 时 L' = L + m；
	 *  - Index > L → L 不变；N' = N + m。
	 * 既有节点的归一化 X/Y 原样保留（曲线按新序号在新位置求值）。
	 */
	static bool Insert(const FLyraRecoilPatternData& Source, int32 Index, const TArray<FRecoilPatternPoint>& NewPoints,
		bool bFixedAtBoundary, FLyraRecoilPatternData& OutData, TArray<FString>& Errors);

	/**
	 * 一次性删除选择集（按操作前索引计算）：N' = N − |D|，L' = L − |{i ∈ D 且 i < L}|。
	 * 剩余节点归一化值保留；删光后 N = L = 0，不自动补默认点。
	 */
	static bool Delete(const FLyraRecoilPatternData& Source, const TArray<int32>& Indices,
		FLyraRecoilPatternData& OutData, TArray<FString>& Errors);

	/**
	 * 显式重排：Order 必须是 [0, N-1] 的排列；N、L 数值不变，固定段仍是新顺序的前 L 个节点。
	 */
	static bool Reorder(const FLyraRecoilPatternData& Source, const TArray<int32>& Order,
		FLyraRecoilPatternData& OutData, TArray<FString>& Errors);

	/** 逐字段精确比较（用于“无变化不建事务”的零补丁判定）。 */
	static bool Equal(const FLyraRecoilPatternData& A, const FLyraRecoilPatternData& B);

	/**
	 * 生成版本化剪贴板 JSON（LyraRecoilClipboard/v1）。
	 * Indices 会按原发序号升序、去重；索引非法、非有限或载荷超过保护值时返回空串。
	 */
	static FString Copy(const ULyraRecoilProfile& Profile, const TArray<int32>& Indices);

	/**
	 * 严格解析剪贴板文本，只输出归一化参数。
	 * 未知版本 / 畸形 JSON / 非数字 / 非有限 / 超过 1 MiB 或 4096 点 → 整体拒绝，OutPoints 为空。
	 */
	static bool ParseClipboard(const FString& Text, TArray<FRecoilPatternPoint>& OutPoints, TArray<FString>& Errors);

	// -----------------------------------------------------------------
	// 辅助接口（GUI / 测试共用；不替代上面的必需接口）
	// -----------------------------------------------------------------

	/** 用显式数据（而不是资产当前值）构建累计点，供结构/候选预览复用曲线求值。 */
	static bool BuildCumulativeFor(const ULyraRecoilProfile& Profile, const FLyraRecoilPatternData& Data,
		TArray<FVector2D>& OutPoints, TArray<FString>& Errors);

	/** 显式设置 PatternLength（0…N）。数组与原始点不变；越界拒绝。 */
	static bool SetPatternLength(const FLyraRecoilPatternData& Source, int32 NewLength,
		FLyraRecoilPatternData& OutData, TArray<FString>& Errors);

	/** 解析完整剪贴板载荷（含源序号、源角度、源强度）。 */
	static bool ParseClipboardEx(const FString& Text, FLyraRecoilClipboardData& OutData, TArray<FString>& Errors);

	/**
	 * “按角度粘贴”：用目标资产的 H/V/曲线把源逐发角度反算成目标归一化参数。
	 * 从 FirstTargetIndex 开始按剪贴板顺序逐发求值；不可逆/超界/非有限时整体拒绝。
	 */
	static bool ConvertClipboardAngles(const ULyraRecoilProfile& TargetProfile, const FLyraRecoilClipboardData& Clipboard,
		int32 FirstTargetIndex, TArray<FRecoilPatternPoint>& OutPoints, TArray<FString>& Errors);

	/**
	 * 一次性“按累计 Pitch 排序”的稳定排序（§07）：返回 [0, N-1] 的排列，可直接交给 Reorder。
	 * 排序键用同一 ΔPitch 公式对全数组求累计；同值保持原顺序（稳定）。
	 */
	static bool MakeStablePitchOrder(const ULyraRecoilProfile& Profile, TArray<int32>& OutOrder, TArray<FString>& Errors);

	/**
	 * 高级“调整基础强度以容纳候选”：计算 H'/V' 与重新归一化后的候选数据，并列出差异。
	 *  - 只纳入可逆且符号相容的目标发；零曲线要求该发目标垂直增量为零；
	 *  - 不修改 HorizontalRandomRange、姿态、上限、曲线；
	 *  - 返回 true 表示候选已生成（bApplicable == true）；约束不可满足时返回 false，
	 *    原因写入 OutAdjustment.BlockingReasons 与 Errors，OutData 保持源数据。
	 */
	static bool BuildStrengthAdjustmentCandidate(const ULyraRecoilProfile& Profile, const TArray<FVector2D>& Targets,
		FLyraRecoilStrengthAdjustment& OutAdjustment, FLyraRecoilPatternData& OutData, TArray<FString>& Errors);

	/** 角度重建容差：1e-5 + 1e-6 × |TargetAngle|。 */
	static double GetAngleTolerance(double TargetAngle);

	/** 剪贴板标识与版本（严格校验用）。 */
	static const TCHAR* GetClipboardFormatName();
	static int32 GetClipboardFormatVersion();
};
