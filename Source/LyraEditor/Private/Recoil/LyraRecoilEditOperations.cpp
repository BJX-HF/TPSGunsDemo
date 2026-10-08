// Copyright Epic Games, Inc. All Rights Reserved.

#include "Recoil/LyraRecoilEditOperations.h"

#include "Recoil/LyraRecoilEditorSession.h"
#include "Weapons/Recoil/LyraRecoilProfile.h"

#include "AssetToolsModule.h"
#include "Curves/CurveFloat.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "LyraRecoilEditOperations"

namespace LyraRecoilEditOperationsLocal
{
	/** 编辑器数值安全常量：与计划 §06 的极小分母阈值对应（此处只用于诊断，不参与运行时）。 */
	static constexpr double TinyDenominatorEpsilon = 1e-8;

	static bool CheckFiniteFloat(const TCHAR* Label, float Value, TArray<FString>& OutErrors)
	{
		if (!FMath::IsFinite(Value))
		{
			OutErrors.Add(FString::Printf(TEXT("%s 不是有限数（NaN/Inf），已阻止提交。"), Label));
			return false;
		}
		return true;
	}

	static bool CheckRichCurveFinite(const FRichCurve& RichCurve, const TCHAR* Label, TArray<FString>& OutErrors)
	{
		bool bFinite = true;
		for (int32 KeyIndex = 0; KeyIndex < RichCurve.Keys.Num(); ++KeyIndex)
		{
			const FRichCurveKey& Key = RichCurve.Keys[KeyIndex];
			if (!FMath::IsFinite(Key.Time) || !FMath::IsFinite(Key.Value)
				|| !FMath::IsFinite(Key.ArriveTangent) || !FMath::IsFinite(Key.LeaveTangent))
			{
				OutErrors.Add(FString::Printf(TEXT("%s[%d] 含非有限键值（Time/Value/Tangent），已阻止提交。"), Label, KeyIndex));
				bFinite = false;
			}
		}
		return bFinite;
	}

	static bool CheckRuntimeCurveFinite(const FRuntimeFloatCurve& Curve, const TCHAR* Label, TArray<FString>& OutErrors)
	{
		const FRichCurve* RichCurve = Curve.GetRichCurveConst();
		return RichCurve ? CheckRichCurveFinite(*RichCurve, Label, OutErrors) : true;
	}
}

void FLyraRecoilEditOperations::Initialize(ULyraRecoilProfile* InProfile, ULyraRecoilEditorSession* InSession)
{
	Profile = InProfile;
	Session = InSession;
}

uint64 FLyraRecoilEditOperations::BeginGesture() const
{
	const ULyraRecoilEditorSession* Sess = Session.Get();
	return Sess ? Sess->GetRevision() : 0;
}

bool FLyraRecoilEditOperations::ValidateGestureRevision(uint64 InStartRevision, TArray<FString>& OutErrors) const
{
	const ULyraRecoilEditorSession* Sess = Session.Get();
	if (!Sess)
	{
		// 无会话（纯服务测试）时不参与修订冲突判定。
		return true;
	}

	if (Sess->GetRevision() != InStartRevision)
	{
		OutErrors.Add(TEXT("资产或曲线依赖已在其他入口更新（修订冲突），本次草稿已丢弃。"));
		return false;
	}

	return true;
}

const TArray<FName>& FLyraRecoilEditOperations::GetCurvePropertyNames()
{
	static const TArray<FName> Names =
	{
		GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile, VerticalKickCurve),
		GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile, RecoveryCurve),
		GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile, LiftCurve),
		GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile, ReboundCurve),
		GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile, RollShake_AmplitudeCurve),
		GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile, RollShake_SegmentScaleCurve),
		GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile, RollShake_PeriodScaleCurve),
	};
	return Names;
}

FStructProperty* FLyraRecoilEditOperations::FindCurveProperty(FName PropertyName)
{
	if (!GetCurvePropertyNames().Contains(PropertyName))
	{
		return nullptr;
	}

	return FindFProperty<FStructProperty>(ULyraRecoilProfile::StaticClass(), PropertyName);
}

FRuntimeFloatCurve* FLyraRecoilEditOperations::GetRuntimeCurve(ULyraRecoilProfile* Target, FName PropertyName)
{
	if (!Target)
	{
		return nullptr;
	}

	FStructProperty* Property = FindCurveProperty(PropertyName);
	if (!Property || Property->Struct != FRuntimeFloatCurve::StaticStruct())
	{
		return nullptr;
	}

	return Property->ContainerPtrToValuePtr<FRuntimeFloatCurve>(Target);
}

const FRuntimeFloatCurve* FLyraRecoilEditOperations::GetRuntimeCurve(const ULyraRecoilProfile* Target, FName PropertyName)
{
	return GetRuntimeCurve(const_cast<ULyraRecoilProfile*>(Target), PropertyName);
}

UCurveFloat* FLyraRecoilEditOperations::GetExternalCurve(const ULyraRecoilProfile* Target, FName PropertyName)
{
	const FRuntimeFloatCurve* Curve = GetRuntimeCurve(Target, PropertyName);
	return Curve ? Curve->ExternalCurve : nullptr;
}

bool FLyraRecoilEditOperations::IsCurveExternallyShared(const ULyraRecoilProfile* Target, FName PropertyName)
{
	return GetExternalCurve(Target, PropertyName) != nullptr;
}

bool FLyraRecoilEditOperations::CheckFiniteNumbers(const ULyraRecoilProfile& Target, TArray<FString>& OutErrors)
{
	bool bFinite = true;

	bFinite &= LyraRecoilEditOperationsLocal::CheckFiniteFloat(TEXT("RecoilPerShot_Vertical"), Target.RecoilPerShot_Vertical, OutErrors);
	bFinite &= LyraRecoilEditOperationsLocal::CheckFiniteFloat(TEXT("RecoilPerShot_Horizontal"), Target.RecoilPerShot_Horizontal, OutErrors);
	bFinite &= LyraRecoilEditOperationsLocal::CheckFiniteFloat(TEXT("MaxVerticalKick"), Target.MaxVerticalKick, OutErrors);
	bFinite &= LyraRecoilEditOperationsLocal::CheckFiniteFloat(TEXT("MaxHorizontalKick"), Target.MaxHorizontalKick, OutErrors);
	bFinite &= LyraRecoilEditOperationsLocal::CheckFiniteFloat(TEXT("HorizontalRandomRange"), Target.HorizontalRandomRange, OutErrors);

	for (int32 Index = 0; Index < Target.PatternPoints.Num(); ++Index)
	{
		const FRecoilPatternPoint& Point = Target.PatternPoints[Index];
		bFinite &= LyraRecoilEditOperationsLocal::CheckFiniteFloat(*FString::Printf(TEXT("PatternPoints[%d].X"), Index), Point.X, OutErrors);
		bFinite &= LyraRecoilEditOperationsLocal::CheckFiniteFloat(*FString::Printf(TEXT("PatternPoints[%d].Y"), Index), Point.Y, OutErrors);
	}

	// 曲线采样也必须是有限数：整数发序号各自判断（负曲线/外推由 Adapter 负责语义）。
	const int32 SampleCount = FMath::Max(Target.PatternPoints.Num(), Target.PatternLength);
	for (int32 Index = 0; Index < SampleCount; ++Index)
	{
		bFinite &= LyraRecoilEditOperationsLocal::CheckFiniteFloat(
			*FString::Printf(TEXT("GetVerticalKickCurveScale(%d)"), Index),
			Target.GetVerticalKickCurveScale(Index), OutErrors);
	}

	for (const FName& CurveName : GetCurvePropertyNames())
	{
		if (const FRuntimeFloatCurve* Curve = GetRuntimeCurve(&Target, CurveName))
		{
			bFinite &= LyraRecoilEditOperationsLocal::CheckRuntimeCurveFinite(*Curve, *CurveName.ToString(), OutErrors);

			// 外部共享曲线也要检查：只读展示，但非有限数同样不能进预览/图形。
			if (const UCurveFloat* External = Curve->ExternalCurve)
			{
				bFinite &= LyraRecoilEditOperationsLocal::CheckRichCurveFinite(
					External->FloatCurve,
					*FString::Printf(TEXT("%s.ExternalCurve"), *CurveName.ToString()),
					OutErrors);
			}
		}
	}

	return bFinite;
}

bool FLyraRecoilEditOperations::ValidateProfileWithAdapter(TArray<FString>& OutErrors) const
{
	const ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile，无法校验。"));
		return false;
	}

	const int32 StartNum = OutErrors.Num();

	bool bValid = FLyraRecoilPatternAdapter::Validate(*Target, OutErrors);

	return bValid && OutErrors.Num() == StartNum;
}

void FLyraRecoilEditOperations::ApplyPatternData(ULyraRecoilProfile* Target, const FLyraRecoilPatternData& Data)
{
    check(Target);
    FArrayProperty* Array = FindFProperty<FArrayProperty>(ULyraRecoilProfile::StaticClass(),GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile,PatternPoints));
    FProperty* Length = FindFProperty<FProperty>(ULyraRecoilProfile::StaticClass(),GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile,PatternLength));
    const int32 OldNum=Target->PatternPoints.Num();
    const bool bLengthChanged=Target->PatternLength!=Data.PatternLength;
    bool bPointsChanged=OldNum!=Data.Points.Num();
    for (int32 I=0; !bPointsChanged && I<OldNum; ++I)
        bPointsChanged=Target->PatternPoints[I].X!=Data.Points[I].X || Target->PatternPoints[I].Y!=Data.Points[I].Y;
    if (bPointsChanged) Target->PreEditChange(Array);
    if (bLengthChanged) Target->PreEditChange(Length);
    if (OldNum!=Data.Points.Num()) Target->PatternPoints=Data.Points;
    else for (int32 I=0; I<OldNum; ++I)
    {
        if (Target->PatternPoints[I].X!=Data.Points[I].X) Target->PatternPoints[I].X=Data.Points[I].X;
        if (Target->PatternPoints[I].Y!=Data.Points[I].Y) Target->PatternPoints[I].Y=Data.Points[I].Y;
    }
    Target->PatternLength=Data.PatternLength; // Set final L before either notification sanitizes it.
    if (bPointsChanged)
    {
        const auto Type=Data.Points.IsEmpty()?EPropertyChangeType::ArrayClear:
            Data.Points.Num()>OldNum?EPropertyChangeType::ArrayAdd:
            Data.Points.Num()<OldNum?EPropertyChangeType::ArrayRemove:EPropertyChangeType::ValueSet;
        FPropertyChangedEvent Event(Array,Type);
        FEditPropertyChain Chain;
        Chain.AddTail(Array);
        FPropertyChangedChainEvent ChainEvent(Chain,Event);
        Target->PostEditChangeChainProperty(ChainEvent);
    }
    if (bLengthChanged) NotifyPropertyChanged(Target,Length);
}

void FLyraRecoilEditOperations::NotifyPropertyChanged(ULyraRecoilProfile* Target, FProperty* Property)
{
	if (!Target || !Property)
	{
		return;
	}

	FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
	Target->PostEditChangeProperty(Event);
	Target->MarkPackageDirty();
}

bool FLyraRecoilEditOperations::CommitPatternInternal(const FLyraRecoilPatternData& InData, const FText& Description,
	TArray<FString>& OutErrors, TFunctionRef<void(ULyraRecoilEditorSession&)> UpdateSession)
{
	ULyraRecoilProfile* Target = Profile.Get();
	ULyraRecoilEditorSession* Sess = Session.Get();

	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile，无法提交。"));
		return false;
	}

	// 1) 无变化：不建事务、不标脏、不发通知。
	const FLyraRecoilPatternData Current = FLyraRecoilPatternAdapter::Read(*Target);
	if (FLyraRecoilPatternAdapter::Equal(Current, InData))
	{
		return true;
	}

	// 2) 显式边界检查（Adapter 会再查一次；此处保证错误消息定位到 L/N）。
	if (InData.PatternLength < 0 || InData.PatternLength > InData.Points.Num())
	{
		OutErrors.Add(FString::Printf(TEXT("PatternLength=%d 超出 [0, PatternPoints.Num()=%d]，已拒绝提交。"),
			InData.PatternLength, InData.Points.Num()));
		return false;
	}

	// 3) 在临时候选 Profile 上做转换 / 字段范围 / ValidateProfile / 有限数检查。
	ULyraRecoilProfile* Candidate = DuplicateObject<ULyraRecoilProfile>(Target, GetTransientPackage());
	if (!Candidate)
	{
		OutErrors.Add(TEXT("无法创建临时候选 Profile，已拒绝提交。"));
		return false;
	}

	Candidate->PatternPoints = InData.Points;
	Candidate->PatternLength = InData.PatternLength;
	Candidate->SanitizePatternLength();

	// 候选不得被静默规范化：读回必须与请求一致，否则说明请求本身非法。
	const FLyraRecoilPatternData CandidateReadBack = FLyraRecoilPatternAdapter::Read(*Candidate);
	if (!FLyraRecoilPatternAdapter::Equal(CandidateReadBack, InData))
	{
		OutErrors.Add(TEXT("候选数据未通过一致性检查（PatternLength 或数组被规范化改变），已拒绝提交。"));
		return false;
	}

	TArray<FString> CandidateErrors;
	bool bCandidateValid = FLyraRecoilPatternAdapter::Validate(*Candidate, CandidateErrors);

	if (!bCandidateValid || CandidateErrors.Num() > 0)
	{
		OutErrors.Append(CandidateErrors);
		if (CandidateErrors.Num() == 0)
		{
			OutErrors.Add(TEXT("候选数据未通过校验，已拒绝提交。"));
		}
		return false;
	}

	// 4) 正向重建验证（用现有曲线求值接口，不手写替代实现）。
	TArray<FVector2D> CumulativePoints;
	if (!FLyraRecoilPatternAdapter::BuildCumulative(*Candidate, CumulativePoints, OutErrors))
	{
		return false;
	}

	// 5) 单个原子事务，只写受影响字段。
	{
		TGuardValue<bool> CommitGuard(bIsCommitting, true);

		FScopedTransaction Transaction(Description);
		Target->Modify();
		if (Sess)
		{
			Sess->Modify();
		}

		ApplyPatternData(Target, InData);

		if (Sess)
		{
			UpdateSession(*Sess);
			Sess->BumpRevision();
		}
	}

	return true;
}

bool FLyraRecoilEditOperations::CommitPattern(const FLyraRecoilPatternData& InData, const FText& Description, TArray<FString>& OutErrors)
{
	return CommitPatternInternal(InData, Description, OutErrors, [](ULyraRecoilEditorSession& Sess)
	{
		// 通用提交：结构可能变化（例如 EditCondition 之外的调用方），按长度重建映射。
		Sess.SyncNodeIdsPreservingMapping();
	});
}

bool FLyraRecoilEditOperations::CommitCumulative(const TArray<FVector2D>& Targets, const FText& Description, TArray<FString>& OutErrors)
{
	ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile，无法提交累计点。"));
		return false;
	}

	FLyraRecoilPatternData OutData;
	if (!FLyraRecoilPatternAdapter::MoveCumulative(*Target, Targets, OutData, OutErrors))
	{
		return false;
	}

	return CommitPattern(OutData, Description, OutErrors);
}

bool FLyraRecoilEditOperations::CommitStrengthAdjustment(const TArray<FVector2D>& Targets,TArray<FString>& Errors)
{
    ULyraRecoilProfile* Target=Profile.Get();
    if (!Target) return false;
    FLyraRecoilStrengthAdjustment Adjustment;
    FLyraRecoilPatternData Data;
    if (!FLyraRecoilPatternAdapter::BuildStrengthAdjustmentCandidate(*Target,Targets,Adjustment,Data,Errors)) return false;
    ULyraRecoilProfile* Candidate=DuplicateObject<ULyraRecoilProfile>(Target,GetTransientPackage());
    Candidate->RecoilPerShot_Horizontal=static_cast<float>(Adjustment.NewHorizontal);
    Candidate->RecoilPerShot_Vertical=static_cast<float>(Adjustment.NewVertical);
    Candidate->PatternPoints=Data.Points;
    Candidate->PatternLength=Data.PatternLength;
    if (!FLyraRecoilPatternAdapter::Validate(*Candidate,Errors)) return false;
    if (Candidate->RecoilPerShot_Horizontal==Target->RecoilPerShot_Horizontal
        && Candidate->RecoilPerShot_Vertical==Target->RecoilPerShot_Vertical
        && FLyraRecoilPatternAdapter::Equal(Data,FLyraRecoilPatternAdapter::Read(*Target))) return true;
    TGuardValue<bool> Guard(bIsCommitting,true);
    FScopedTransaction Transaction(LOCTEXT("AdjustStrength","调整基础强度及归一化参数"));
    Target->Modify();
    if (auto* Sess=Session.Get()) Sess->Modify();
    FProperty* H=FindFProperty<FProperty>(ULyraRecoilProfile::StaticClass(),GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile,RecoilPerShot_Horizontal));
    FProperty* V=FindFProperty<FProperty>(ULyraRecoilProfile::StaticClass(),GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile,RecoilPerShot_Vertical));
    const bool bH=Candidate->RecoilPerShot_Horizontal!=Target->RecoilPerShot_Horizontal;
    const bool bV=Candidate->RecoilPerShot_Vertical!=Target->RecoilPerShot_Vertical;
    if (bH) Target->PreEditChange(H);
    if (bV) Target->PreEditChange(V);
    Target->RecoilPerShot_Horizontal=Candidate->RecoilPerShot_Horizontal;
    Target->RecoilPerShot_Vertical=Candidate->RecoilPerShot_Vertical;
    ApplyPatternData(Target,Data);
    if (bH) NotifyPropertyChanged(Target,H);
    if (bV) NotifyPropertyChanged(Target,V);
    if (auto* Sess=Session.Get()) Sess->BumpRevision();
    return true;
}

bool FLyraRecoilEditOperations::CommitPointNormalized(int32 Index, float X, float Y, TArray<FString>& OutErrors)
{
	ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile，无法编辑该发。"));
		return false;
	}

	if (!Target->PatternPoints.IsValidIndex(Index))
	{
		OutErrors.Add(FString::Printf(TEXT("第 %d 发不存在（数组长度 %d）。"), Index + 1, Target->PatternPoints.Num()));
		return false;
	}

	FLyraRecoilPatternData Data = FLyraRecoilPatternAdapter::Read(*Target);
	Data.Points[Index] = FRecoilPatternPoint(X, Y);

	return CommitPatternInternal(Data,
		FText::Format(LOCTEXT("EditPoint", "编辑第 {0} 发归一化参数"), FText::AsNumber(Index + 1)),
		OutErrors, [](ULyraRecoilEditorSession&) {});
}

bool FLyraRecoilEditOperations::InsertPoints(int32 Index, const TArray<FRecoilPatternPoint>& NewPoints, bool bFixedAtBoundary,
	TArray<FString>& OutErrors)
{
	ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile，无法插入。"));
		return false;
	}

	const FLyraRecoilPatternData Source = FLyraRecoilPatternAdapter::Read(*Target);

	FLyraRecoilPatternData OutData;
	if (!FLyraRecoilPatternAdapter::Insert(Source, Index, NewPoints, bFixedAtBoundary, OutData, OutErrors))
	{
		return false;
	}

	TArray<FGuid> NewGuids;
	NewGuids.Reserve(NewPoints.Num());
	for (int32 PointIndex = 0; PointIndex < NewPoints.Num(); ++PointIndex)
	{
		NewGuids.Add(FGuid::NewGuid());
	}

	return CommitPatternInternal(OutData,
		FText::Format(LOCTEXT("InsertPoints", "插入 {0} 个节点"), FText::AsNumber(NewPoints.Num())),
		OutErrors,
		[Index, &NewGuids](ULyraRecoilEditorSession& Sess)
		{
			Sess.InsertNodeIds(Index, NewGuids);
		});
}

bool FLyraRecoilEditOperations::DeletePoints(const TArray<int32>& Indices, TArray<FString>& OutErrors)
{
	ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile，无法删除。"));
		return false;
	}

	TArray<int32> SortedIndices = Indices;
	SortedIndices.Sort();

	const FLyraRecoilPatternData Source = FLyraRecoilPatternAdapter::Read(*Target);

	FLyraRecoilPatternData OutData;
	if (!FLyraRecoilPatternAdapter::Delete(Source, SortedIndices, OutData, OutErrors))
	{
		return false;
	}

	return CommitPatternInternal(OutData,
		FText::Format(LOCTEXT("DeletePoints", "删除 {0} 个节点"), FText::AsNumber(SortedIndices.Num())),
		OutErrors,
		[&SortedIndices](ULyraRecoilEditorSession& Sess)
		{
			Sess.RemoveNodeIds(SortedIndices);
		});
}

bool FLyraRecoilEditOperations::ReorderPoints(const TArray<int32>& Order, TArray<FString>& OutErrors)
{
	ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile，无法重排。"));
		return false;
	}

	const FLyraRecoilPatternData Source = FLyraRecoilPatternAdapter::Read(*Target);

	FLyraRecoilPatternData OutData;
	if (!FLyraRecoilPatternAdapter::Reorder(Source, Order, OutData, OutErrors))
	{
		return false;
	}

	return CommitPatternInternal(OutData, LOCTEXT("ReorderPoints", "显式重排节点"), OutErrors,
		[&Order](ULyraRecoilEditorSession& Sess)
		{
			Sess.ReorderNodeIds(Order);
		});
}

bool FLyraRecoilEditOperations::SetPatternLength(int32 NewLength, TArray<FString>& OutErrors)
{
	ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile，无法修改固定区间。"));
		return false;
	}

	if (NewLength < 0 || NewLength > Target->PatternPoints.Num())
	{
		OutErrors.Add(FString::Printf(TEXT("固定段长度 %d 超出 [0, %d]。"), NewLength, Target->PatternPoints.Num()));
		return false;
	}

	const FLyraRecoilPatternData Source = FLyraRecoilPatternAdapter::Read(*Target);

	FLyraRecoilPatternData OutData;
	if (!FLyraRecoilPatternAdapter::SetPatternLength(Source, NewLength, OutData, OutErrors))
	{
		return false;
	}

	return CommitPatternInternal(OutData,
		FText::Format(LOCTEXT("SetPatternLength", "设置固定段长度为 {0}"), FText::AsNumber(NewLength)),
		OutErrors, [](ULyraRecoilEditorSession&) {});
}

bool FLyraRecoilEditOperations::BuildCumulativePitchSortOrder(TArray<int32>& OutOrder, TArray<FString>& OutErrors) const
{
	const ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile。"));
		return false;
	}

	// 排序键与数组外沿语义由 Adapter 统一实现，这里不复制数学。
	return FLyraRecoilPatternAdapter::MakeStablePitchOrder(*Target, OutOrder, OutErrors);
}

bool FLyraRecoilEditOperations::CopySelectionToClipboard(TArray<FString>& OutErrors) const
{
	const ULyraRecoilProfile* Target = Profile.Get();
	const ULyraRecoilEditorSession* Sess = Session.Get();

	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile。"));
		return false;
	}

	const TArray<int32> Indices = Sess ? Sess->GetSelectedIndices() : TArray<int32>();
	if (Indices.Num() == 0)
	{
		OutErrors.Add(TEXT("没有选中的节点可复制。"));
		return false;
	}

	const FString Payload = FLyraRecoilPatternAdapter::Copy(*Target, Indices);
	FPlatformApplicationMisc::ClipboardCopy(*Payload);
	return true;
}

bool FLyraRecoilEditOperations::PasteNormalizedFromClipboard(TArray<FString>& OutErrors)
{
	ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile。"));
		return false;
	}

	FString ClipboardText;
	FPlatformApplicationMisc::ClipboardPaste(ClipboardText);
	if (ClipboardText.IsEmpty())
	{
		OutErrors.Add(TEXT("剪贴板为空。"));
		return false;
	}

	TArray<FRecoilPatternPoint> Points;
	if (!FLyraRecoilPatternAdapter::ParseClipboard(ClipboardText, Points, OutErrors))
	{
		// 失败零写入：解析失败直接返回，不建事务。
		return false;
	}

	if (Points.Num() == 0)
	{
		OutErrors.Add(TEXT("剪贴板载荷不含任何节点。"));
		return false;
	}

	// 默认语义：粘贴原始归一化逐发参数，插到数组末尾（k == N，归入尾段，L 不变）。
	const auto* Sess=Session.Get();
	const int32 Index=Sess && !Sess->GetSelectedIndices().IsEmpty()?Sess->GetSelectedIndices()[0]:Target->PatternPoints.Num();
	return InsertPoints(Index, Points, /*bFixedAtBoundary=*/false, OutErrors);
}

bool FLyraRecoilEditOperations::PasteByAngleFromClipboard(TArray<FString>& OutErrors)
{
	ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile。"));
		return false;
	}

	FString ClipboardText;
	FPlatformApplicationMisc::ClipboardPaste(ClipboardText);
	if (ClipboardText.IsEmpty())
	{
		OutErrors.Add(TEXT("剪贴板为空。"));
		return false;
	}

	FLyraRecoilClipboardData Clipboard;
	if (!FLyraRecoilPatternAdapter::ParseClipboardEx(ClipboardText, Clipboard, OutErrors))
	{
		return false;
	}

	if (!Clipboard.bHasSourceAngles)
	{
		OutErrors.Add(TEXT("该剪贴板载荷不含源逐发角度，无法按角度粘贴；请改用「粘贴归一化参数」。"));
		return false;
	}

	// 在目标资产的强度与曲线下重新反算；不复制源曲线引用。
	const auto* Sess=Session.Get();
	const int32 FirstTargetIndex=Sess && !Sess->GetSelectedIndices().IsEmpty()?Sess->GetSelectedIndices()[0]:Target->PatternPoints.Num();
	TArray<FRecoilPatternPoint> ConvertedPoints;
	if (!FLyraRecoilPatternAdapter::ConvertClipboardAngles(*Target, Clipboard, FirstTargetIndex, ConvertedPoints, OutErrors))
	{
		return false;
	}

	return InsertPoints(FirstTargetIndex, ConvertedPoints, /*bFixedAtBoundary=*/false, OutErrors);
}

bool FLyraRecoilEditOperations::CopyCurveToInline(FName PropertyName, TArray<FString>& OutErrors)
{
	ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile。"));
		return false;
	}

	FRuntimeFloatCurve* Curve = GetRuntimeCurve(Target, PropertyName);
	FStructProperty* Property = FindCurveProperty(PropertyName);
	if (!Curve || !Property)
	{
		OutErrors.Add(FString::Printf(TEXT("未知曲线属性：%s"), *PropertyName.ToString()));
		return false;
	}

	UCurveFloat* External = Curve->ExternalCurve;
	if (!External)
	{
		// 已经是内联曲线：无操作、不建事务、不标脏。
		return true;
	}

	// 复制当前有效 RichCurve 的全部内容（键、切线、插值、外推、默认值）。
	const FRichCurve* EffectiveCurve = Curve->GetRichCurveConst();
	if (!EffectiveCurve)
	{
		OutErrors.Add(TEXT("无法读取共享曲线的有效数据，已取消。"));
		return false;
	}

	const bool bFinite = FLyraRecoilPatternAdapter::Validate(*Target, OutErrors);
	if (!bFinite)
	{
		return false;
	}

	{
		FScopedTransaction Transaction(FText::Format(
			LOCTEXT("CopyCurveToInline", "将「{0}」复制为内联曲线"), FText::FromName(PropertyName)));

		TGuardValue<bool> Guard(bIsCommitting,true);
		Target->Modify();
		Target->PreEditChange(Property);
		Curve->EditorCurveData = *EffectiveCurve;
		Curve->ExternalCurve = nullptr;
		NotifyPropertyChanged(Target, Property);
	}

	if (ULyraRecoilEditorSession* Sess = Session.Get())
	{
		Sess->BumpRevision();
	}

	return true;
}

bool FLyraRecoilEditOperations::ReplaceCurveReference(FName PropertyName, UCurveFloat* NewCurve, TArray<FString>& OutErrors)
{
	ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile。"));
		return false;
	}

	FRuntimeFloatCurve* Curve = GetRuntimeCurve(Target, PropertyName);
	FStructProperty* Property = FindCurveProperty(PropertyName);
	if (!Curve || !Property)
	{
		OutErrors.Add(FString::Printf(TEXT("未知曲线属性：%s"), *PropertyName.ToString()));
		return false;
	}

	if (Curve->ExternalCurve == NewCurve)
	{
		return true;
	}

	// 只修改 Profile 引用，不修改被引用对象。
	{
		FScopedTransaction Transaction(LOCTEXT("ReplaceCurveReference", "更换曲线引用"));

		TGuardValue<bool> Guard(bIsCommitting,true);
		Target->Modify();
		Target->PreEditChange(Property);
		Curve->ExternalCurve = NewCurve;
		NotifyPropertyChanged(Target, Property);
	}

	if (ULyraRecoilEditorSession* Sess = Session.Get())
	{
		Sess->BumpRevision();
	}

	return true;
}

bool FLyraRecoilEditOperations::DuplicateCurveAsset(FName PropertyName, TArray<FString>& OutErrors)
{
	ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile。"));
		return false;
	}

	const FRuntimeFloatCurve* Curve = GetRuntimeCurve(Target, PropertyName);
	if (!Curve)
	{
		OutErrors.Add(FString::Printf(TEXT("未知曲线属性：%s"), *PropertyName.ToString()));
		return false;
	}

	const FRichCurve* EffectiveCurve = Curve->GetRichCurveConst();
	if (!EffectiveCurve || !EffectiveCurve->HasAnyData())
	{
		OutErrors.Add(TEXT("当前曲线没有可复制的键，已取消创建新资产。"));
		return false;
	}

	const FString SourcePackagePath = FPackageName::GetLongPackagePath(Target->GetOutermost()->GetName());
	const FString BaseName = FString::Printf(TEXT("%s_%s"), *Target->GetName(), *PropertyName.ToString());

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	FString NewPackageName;
	FString NewAssetName;
	AssetToolsModule.Get().CreateUniqueAssetName(SourcePackagePath / BaseName, TEXT(""), NewPackageName, NewAssetName);

	UObject* NewAsset = AssetToolsModule.Get().CreateAsset(NewAssetName, SourcePackagePath, UCurveFloat::StaticClass(), nullptr);
	UCurveFloat* NewCurve = Cast<UCurveFloat>(NewAsset);
	if (!NewCurve)
	{
		OutErrors.Add(TEXT("创建新曲线资产失败，未更换引用。"));
		return false;
	}

	NewCurve->FloatCurve = *EffectiveCurve;
	NewCurve->MarkPackageDirty();

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(NewCurve->GetOutermost());
	if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty=*/true))
	{
		// 已创建但未保存：不换引用，并把状态说明清楚。
		OutErrors.Add(FString::Printf(
			TEXT("新曲线资产 %s 已创建但保存失败；Profile 引用未更换，请手工处理该未引用资产。"),
			*NewCurve->GetPathName()));
		return false;
	}

	return ReplaceCurveReference(PropertyName, NewCurve, OutErrors);
}

bool FLyraRecoilEditOperations::OpenSharedCurveAsset(FName PropertyName, TArray<FString>& OutErrors) const
{
	const ULyraRecoilProfile* Target = Profile.Get();
	if (!Target)
	{
		OutErrors.Add(TEXT("没有正在编辑的 Profile。"));
		return false;
	}

	UCurveFloat* External = GetExternalCurve(Target, PropertyName);
	if (!External)
	{
		OutErrors.Add(TEXT("该曲线当前为内联曲线，没有可打开的共享资产。"));
		return false;
	}

	if (!GEditor)
	{
		OutErrors.Add(TEXT("编辑器不可用，无法打开共享曲线资产。"));
		return false;
	}

	GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(External);
	return true;
}

#undef LOCTEXT_NAMESPACE
