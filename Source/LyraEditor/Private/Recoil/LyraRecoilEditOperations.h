// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Recoil/LyraRecoilPatternAdapter.h"
#include "Recoil/LyraRecoilEditorSession.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"

class ULyraRecoilEditorSession;
class ULyraRecoilProfile;
class UCurveFloat;
struct FRuntimeFloatCurve;

/**
 * P2 统一编辑命令服务。
 *
 * 所有对 ULyraRecoilProfile 的写操作都必须经过本类，以便：
 *  - 先做转换/校验（含 ValidateProfile 与有限数检查），失败不进入事务；
 *  - 合法且确有变化时才建立一个 FScopedTransaction（无变化不建事务、不标脏）；
 *  - 只写本次受影响的字段（未触碰字段不往返重算）；
 *  - 提交后按引擎属性通知路径标脏，并递增会话修订号。
 *
 * 本类不承载数学：累计点/结构/剪贴板语义全部委托给 FLyraRecoilPatternAdapter（A 交付）。
 * 本类也不直接创建 Slate；表格、Details、画布都通过它提交。
 */
class FLyraRecoilEditOperations
{
public:
	FLyraRecoilEditOperations() = default;

	void Initialize(ULyraRecoilProfile* InProfile, ULyraRecoilEditorSession* InSession);

	ULyraRecoilProfile* GetProfile() const { return Profile.Get(); }
	ULyraRecoilEditorSession* GetSession() const { return Session.Get(); }
	void SetSession(ULyraRecoilEditorSession* InSession) { Session = InSession; }

	/** 自提交作用域保护：工具包的属性监听在此时不再重复重建工作模型。 */
	bool IsCommitting() const { return bIsCommitting; }

	// ---------------------------------------------------------------------
	// 手势 / 修订
	// ---------------------------------------------------------------------

	/** 记录手势开始修订号。 */
	uint64 BeginGesture() const;

	/** 提交前比对修订号；不匹配则草稿作废。 */
	bool ValidateGestureRevision(uint64 InStartRevision, TArray<FString>& OutErrors) const;

	// ---------------------------------------------------------------------
	// 统一原子提交
	// ---------------------------------------------------------------------

	/** 提交整份 Pattern 数据（唯一写 PatternPoints/PatternLength 的入口）。 */
	bool CommitPattern(const FLyraRecoilPatternData& InData, const FText& Description, TArray<FString>& OutErrors);

	/** 用累计角度目标反算并提交固定段。 */
	bool CommitCumulative(const TArray<FVector2D>& Targets, const FText& Description, TArray<FString>& OutErrors);
	bool CommitStrengthAdjustment(const TArray<FVector2D>& Targets, TArray<FString>& OutErrors);

	/** 表格精确编辑：改单发归一化值。 */
	bool CommitPointNormalized(int32 Index, float X, float Y, TArray<FString>& OutErrors);

	/** 结构命令。 */
	bool InsertPoints(int32 Index, const TArray<FRecoilPatternPoint>& NewPoints, bool bFixedAtBoundary, TArray<FString>& OutErrors);
	bool DeletePoints(const TArray<int32>& Indices, TArray<FString>& OutErrors);
	bool ReorderPoints(const TArray<int32>& Order, TArray<FString>& OutErrors);
	bool SetPatternLength(int32 NewLength, TArray<FString>& OutErrors);

	/** 高级命令：显式重排为"按累计 Pitch 稳定排序"。调用方必须先展示差异。 */
	bool BuildCumulativePitchSortOrder(TArray<int32>& OutOrder, TArray<FString>& OutErrors) const;

	// ---------------------------------------------------------------------
	// 剪贴板
	// ---------------------------------------------------------------------

	bool CopySelectionToClipboard(TArray<FString>& OutErrors) const;
	/** 默认语义：粘贴原始归一化逐发参数，插到数组末尾（新节点新 ID）。 */
	bool PasteNormalizedFromClipboard(TArray<FString>& OutErrors);
	/** 显式“按角度粘贴”：用目标 H/V/曲线把载荷中的源逐发角度反算后插入。 */
	bool PasteByAngleFromClipboard(TArray<FString>& OutErrors);

	// ---------------------------------------------------------------------
	// 曲线所有权
	// ---------------------------------------------------------------------

	/** Profile 上所有 FRuntimeFloatCurve 字段名（7 条）。 */
	static const TArray<FName>& GetCurvePropertyNames();

	static FStructProperty* FindCurveProperty(FName PropertyName);
	static FRuntimeFloatCurve* GetRuntimeCurve(ULyraRecoilProfile* Target, FName PropertyName);
	static const FRuntimeFloatCurve* GetRuntimeCurve(const ULyraRecoilProfile* Target, FName PropertyName);
	static UCurveFloat* GetExternalCurve(const ULyraRecoilProfile* Target, FName PropertyName);
	static bool IsCurveExternallyShared(const ULyraRecoilProfile* Target, FName PropertyName);

	/** 复制当前有效 RichCurve（键/切线/插值/外推/默认值）为内联曲线，并清除外部引用。 */
	bool CopyCurveToInline(FName PropertyName, TArray<FString>& OutErrors);

	/** 只改 Profile 引用，不改被引用对象。 */
	bool ReplaceCurveReference(FName PropertyName, UCurveFloat* NewCurve, TArray<FString>& OutErrors);

	/** 通过引擎创建/保存流程生成新曲线资产后换引用；失败/取消不换引用。 */
	bool DuplicateCurveAsset(FName PropertyName, TArray<FString>& OutErrors);

	/** 显式打开共享曲线资产（该曲线有独立事务/Dirty/保存流程）。 */
	bool OpenSharedCurveAsset(FName PropertyName, TArray<FString>& OutErrors) const;

	// ---------------------------------------------------------------------
	// 校验
	// ---------------------------------------------------------------------

	/** ValidateProfile + Adapter 校验 + GUI 附加有限数检查。 */
	bool ValidateProfileWithAdapter(TArray<FString>& OutErrors) const;

private:
	/** 统一提交流程：校验 → 事务 → 最小字段写 → 通知 → 修订递增。 */
	bool CommitPatternInternal(const FLyraRecoilPatternData& InData, const FText& Description, TArray<FString>& OutErrors,
		TFunctionRef<void(ULyraRecoilEditorSession&)> UpdateSession);

	/** 在事务内把数据写入 Profile 并发一次完整属性通知。 */
	static void ApplyPatternData(ULyraRecoilProfile* Target, const FLyraRecoilPatternData& InData);
	static void NotifyPropertyChanged(ULyraRecoilProfile* Target, FProperty* Property);

	/** 有限数检查（NaN/Inf/溢出），不修改资产。 */
	static bool CheckFiniteNumbers(const ULyraRecoilProfile& Target, TArray<FString>& OutErrors);

	TWeakObjectPtr<ULyraRecoilProfile> Profile;
	TWeakObjectPtr<ULyraRecoilEditorSession> Session;

	/** 自提交防重入。 */
	bool bIsCommitting = false;
};
