// Copyright Epic Games, Inc. All Rights Reserved.
// Independently implemented; interaction reference: CrystalRecoil @ d977456a8468ab15c7ca05ede8cddf72dd5e5bc5.
// Upstream MIT notice is archived in Docs/Recoil/ThirdParty/CrystalRecoil-LICENSE.txt.
#pragma once
#include "Widgets/SLeafWidget.h"
class FLyraRecoilProfileEditor;

class SLyraRecoilPatternGraph : public SLeafWidget
{
public:
 SLATE_BEGIN_ARGS(SLyraRecoilPatternGraph) {} SLATE_ARGUMENT(TSharedPtr<FLyraRecoilProfileEditor>, Editor) SLATE_END_ARGS()
 void Construct(const FArguments& Args);
 virtual bool SupportsKeyboardFocus() const override { return true; }
 virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(480,350); }
 virtual int32 OnPaint(const FPaintArgs&,const FGeometry&,const FSlateRect&,FSlateWindowElementList&,int32,const FWidgetStyle&,bool) const override;
 virtual FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent&) override;
 virtual FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent&) override;
 virtual FReply OnMouseMove(const FGeometry&,const FPointerEvent&) override;
 virtual FReply OnMouseWheel(const FGeometry&,const FPointerEvent&) override;
 virtual FReply OnKeyDown(const FGeometry&,const FKeyEvent&) override;
 virtual FReply OnKeyUp(const FGeometry&,const FKeyEvent&) override;
 virtual void OnMouseCaptureLost(const FCaptureLostEvent&) override;
 virtual void OnFocusLost(const FFocusEvent&) override;
 virtual void Tick(const FGeometry&,double,float) override;
 void FitAll();
 void FitSelection();
 void CancelDrag();
 void SetSnapEnabled(bool bValue) { bSnap=bValue; }
 bool IsSnapEnabled() const { return bSnap; }
 void SetSnapStep(double Value) { if(FMath::IsFinite(Value) && Value>0) SnapStep=Value; }
 double GetSnapStep() const { return SnapStep; }
 const TArray<FVector2D>& GetRejectedTargets() const { return RejectedTargets; }
#if WITH_DEV_AUTOMATION_TESTS
 void StartPerformanceCapture();
 FString GetPerformanceSummary() const;
#endif
private:
#if WITH_DEV_AUTOMATION_TESTS
 struct FPerformanceMetric
 {
  uint64 Count=0;
  double TotalMs=0,MaxMs=0;
  void Record(double Started) { const double Ms=(FPlatformTime::Seconds()-Started)*1000; ++Count; TotalMs+=Ms; MaxMs=FMath::Max(MaxMs,Ms); }
 };
 bool bCapturePerformance=false;
 mutable FPerformanceMetric PaintMetric,MoveMetric,ReleaseMetric,HitMetric;
#endif
 FVector2D ToScreen(FVector2D Angle) const;
 FVector2D ToAngle(FVector2D Screen) const;
 void Fit(bool bSelection);
 void UpdateDraft(FVector2D Local);
 void SelectHit(int32 Index,bool bToggle);
 TWeakPtr<FLyraRecoilProfileEditor> Editor;
 mutable FVector2D ViewSize=FVector2D(480,350);
 FVector2D Pan=FVector2D::ZeroVector;
 double Scale=100;
 double SnapStep=.05;
 bool bSnap=true,bDragging=false,bPanning=false,bBoxSelecting=false,bAppendBox=false,bInitialFit=true;
 bool bKeyboardGesture=false;
 bool bDragMoved=false;
 FVector2D StartMouse=FVector2D::ZeroVector,StartPan=FVector2D::ZeroVector,BoxEnd=FVector2D::ZeroVector;
 uint64 StartRevision=0;
 int32 DragAnchor=INDEX_NONE;
 TArray<int32> DragSelection;
 TArray<FVector2D> BasePoints,DraftPoints,RejectedTargets;
 TArray<FString> DraftErrors;
};
