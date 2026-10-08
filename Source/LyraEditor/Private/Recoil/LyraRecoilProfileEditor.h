// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorUndoClient.h"
#include "Recoil/LyraRecoilEditOperations.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/GCObject.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtr.h"

class FLyraRecoilProfileDetails;
class FSpawnTabArgs;
class IDetailCustomization;
class IDetailsView;
class SDockTab;
class SLyraRecoilShotTable;
class SLyraRecoilPatternGraph;
class SLyraRecoilPreview;
class SWidget;
class ULyraRecoilEditorSession;
class ULyraRecoilProfile;

/**
 * P2：ULyraRecoilProfile 专用资产编辑器（FAssetEditorToolkit）。
 *
 * 职责（计划 §11）：
 *  - 标签页/工具栏/菜单、Save 与 Save As、关闭与清理；
 *  - 拥有 ULyraRecoilEditorSession（Transient + RF_Transactional）与 FLyraRecoilEditOperations；
 *  - FGCObject 强引用 Profile 与 Session（TSharedPtr 不保护 UObject）；
 *  - 监听 Profile / 曲线依赖 / Undo-Redo / 对象替换，保存 DelegateHandle 并逐个解绑；
 *  - 修订冲突时丢弃草稿；
 *  - 协调服务：不承载反算公式（全部在 FLyraRecoilPatternAdapter）。
 *
 * 画布与预览在 P3/P4 接入：CreatePatternGraph()/CreatePreview() 目前是占位实现，
 * 但公开接口（GetProfile/GetSession/GetCumulativePoints/GetSelectedIndices/
 * SelectIndices/CommitPattern/CommitCumulative/RefreshFromAsset/GetRevision/
 * SetDiagnostics）已经稳定，供画布与预览直接使用。
 */
class FLyraRecoilProfileEditor : public FAssetEditorToolkit, public FGCObject, public FEditorUndoClient
{
public:
	virtual ~FLyraRecoilProfileEditor() override;
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override { PostUndo(bSuccess); }
	bool AdjustStrengthForRejectedTargets();
	/** 打开编辑器。同一资产优先复用已有工具包（见 FindEditorForProfile）。 */
	void InitEditor(const TSharedPtr<IToolkitHost>& InitToolkitHost, ULyraRecoilProfile* InProfile);

	//~ Begin FAssetEditorToolkit interface
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void MapToolkitCommands() override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual void OnClose() override;
	//~ End FAssetEditorToolkit interface

	//~ Begin FGCObject interface
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;
	//~ End FGCObject interface

	// ---------------------------------------------------------------------
	// 画布 / 表格 / 预览 共用公开接口（P3 预留）
	// ---------------------------------------------------------------------

	ULyraRecoilProfile* GetProfile() const { return Profile; }
	ULyraRecoilEditorSession* GetSession() const { return Session; }
	FLyraRecoilEditOperations& GetOperations() { return Operations; }

	/** 固定段累计点（只读缓存，由 RefreshFromAsset 重建）。 */
	const TArray<FVector2D>& GetCumulativePoints() const { return CumulativePoints; }

	const TArray<int32>& GetSelectedIndices() const;
	void SelectIndices(const TArray<int32>& Indices, bool bAppend);
	void ClearSelection();

	/** 画布提交入口：数据来自 Adapter 候选，图不能直接写 Profile。 */
	bool CommitPattern(const FLyraRecoilPatternData& Data, const FText& Description);
	bool CommitCumulative(const TArray<FVector2D>& Targets);

	/** 从资产重建累计点/节点映射/选择/表格/Details。 */
	void RefreshFromAsset();

	uint64 GetRevision() const;

	/** 诊断面板（错误不能只依赖颜色）。 */
	void SetDiagnostics(const TArray<FString>& InErrors);
	const TArray<FString>& GetDiagnostics() const { return Diagnostics; }
#if WITH_DEV_AUTOMATION_TESTS
	void StartGraphPerformanceCapture();
	FString GetGraphPerformanceSummary() const;
#endif

	// 结构命令（表格 / Details / 命令集共用同一条路径）
	bool InsertPoints(int32 Index, const TArray<FRecoilPatternPoint>& NewPoints, bool bFixedAtBoundary);
	bool DeleteSelectedPoints();
	/** Delta = -1 上移，+1 下移（显式重排，不按高度自动排序）。 */
	bool MoveSelectedRows(int32 Delta);
	bool SetPatternLengthValue(int32 NewLength);
	bool SortByCumulativePitch();

	/** 画布拖动提交：直接给出目标累计点。 */
	bool MoveSelectedPoints(const TArray<FVector2D>& Targets);

	/** 表格精确编辑：单发归一化值（X/Y）。 */
	bool SetPointNormalized(int32 Index, float X, float Y);

	/** 表格精确编辑：第 Index 个固定点的累计角度（bPitchAxis 决定轴）。 */
	bool SetCumulativeValue(int32 Index, bool bPitchAxis, double Value);

	/** 拖动生命周期：开始记录手势修订，结束提交或取消。 */
	void BeginGesture();
	bool EndGesture(const TArray<FVector2D>& Targets);
	void CancelActiveGesture();

	// 曲线所有权
	bool IsCurveShared(FName PropertyName) const;
	FString GetCurveOwnershipText(FName PropertyName) const;
	bool CopyCurveToInline(FName PropertyName);
	bool DuplicateCurveAsset(FName PropertyName);
	bool OpenSharedCurveAsset(FName PropertyName);

	/** 详情面板强制刷新（Details 定制的按钮回调使用）。 */
	void RefreshDetails();

	/** 逐发表格刷新（内部 + P3 画布选择同步使用）。 */
	void RefreshShotTable();

	// ---------------------------------------------------------------------
	// P3 / P4 占位（声明稳定，实现随后替换）
	// ---------------------------------------------------------------------

	/** P3 画布占位：当前为 SBorder + STextBlock。 */
	TSharedRef<SWidget> CreatePatternGraph();

	/** P4 时间预览占位：当前为 SBorder + STextBlock。 */
	TSharedRef<SWidget> CreatePreview();

	/** 同一资产的工具包复用；未打开时返回无效指针。 */
	static TSharedPtr<FLyraRecoilProfileEditor> FindEditorForProfile(const ULyraRecoilProfile* InProfile);

protected:
	//~ Begin FAssetEditorToolkit interface
	virtual void SaveAsset_Execute() override;
	virtual void SaveAssetAs_Execute() override;
	virtual bool CanSaveAsset() const override;
	//~ End FAssetEditorToolkit interface

private:
	TSharedRef<SDockTab> SpawnTableTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnGraphTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnDetailsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnPreviewTab(const FSpawnTabArgs& Args);

	TSharedRef<IDetailCustomization> MakeDetailsCustomization();

	// 命令处理（只在本工具包的命令列表中映射）
	void OnSelectAllCommand();
	void OnCopyCommand();
	void OnPasteCommand();
	void OnPasteByAngleCommand();
	void OnInsertPointCommand();
	void OnDeleteSelectedCommand();
	void OnMoveRowUpCommand();
	void OnMoveRowDownCommand();
	void OnSortByPitchCommand();
	void OnValidateCommand();

	bool CanDeleteSelection() const;
	bool CanReorderSelection() const;
	bool CanCopySelection() const;

	void OnDetailsFinishedChangingProperties(const FPropertyChangedEvent& Event);

	void BindExternalListeners();
	void UnbindExternalListeners();
	void RebindCurveDependencies();
	void UnbindCurveDependencies();

	void HandleProfilePropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
	void HandleCurvePropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
	void HandleObjectsReplaced(const FCoreUObjectDelegates::FReplacementObjectMap& ReplacementMap);
	void HandleObjectTransacted(UObject* Object, const FTransactionObjectEvent& Event);

	bool ValidateBeforeSave(TArray<FString>& OutErrors) const;
	bool ConfirmStructureChange(const FLyraRecoilPatternData& Candidate, const TArray<int32>& Order = {});
	void ShowNotification(const FString& Message, bool bIsError) const;

	void RegisterEditorInstance();
	void UnregisterEditorInstance();

	/** 打开中的工具包注册表（Details 定制通过它取回命令服务）。 */
	static TMap<TWeakObjectPtr<ULyraRecoilProfile>, TWeakPtr<FLyraRecoilProfileEditor>> EditorInstances;

	/** 会话与资产（FGCObject 同时引用）。 */
	TObjectPtr<ULyraRecoilEditorSession> Session;
	TObjectPtr<ULyraRecoilProfile> Profile;

	/** 统一编辑命令服务。 */
	FLyraRecoilEditOperations Operations;

	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<SLyraRecoilShotTable> ShotTable;
	TSharedPtr<SLyraRecoilPatternGraph> PatternGraph;
	TSharedPtr<SLyraRecoilPreview> PreviewWidget;

	/** 固定段累计点缓存（可重建）。 */
	TArray<FVector2D> CumulativePoints;

	/** 诊断文本。 */
	TArray<FString> Diagnostics;

	/** 手势修订号快照。 */
	uint64 GestureStartRevision = 0;

	/** 监听句柄：关闭或模块卸载时逐个解除。 */
	FDelegateHandle ProfilePropertyChangedHandle;
	FDelegateHandle ObjectsReplacedHandle;
	FDelegateHandle ObjectTransactedHandle;
	TArray<FDelegateHandle> CurveDependencyHandles;
};
