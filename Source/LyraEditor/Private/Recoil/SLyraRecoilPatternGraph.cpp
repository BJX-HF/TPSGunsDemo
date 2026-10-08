// Copyright Epic Games, Inc. All Rights Reserved.
// Independently implemented from the task15 interaction contract. No Crystal runtime or graph UObject is used.
#include "Recoil/SLyraRecoilPatternGraph.h"
#include "Recoil/LyraRecoilProfileEditor.h"
#include "Weapons/Recoil/LyraRecoilState.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "InputCoreTypes.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS
void SLyraRecoilPatternGraph::StartPerformanceCapture()
{
 PaintMetric={}; MoveMetric={}; ReleaseMetric={}; HitMetric={}; bCapturePerformance=true;
}
FString SLyraRecoilPatternGraph::GetPerformanceSummary() const
{
 FString Result;
 const auto Append=[&](const TCHAR* Name,const FPerformanceMetric& Metric)
 { Result+=FString::Printf(TEXT("Graph %s count=%llu avgMs=%.6f maxMs=%.6f\n"),Name,Metric.Count,Metric.Count?Metric.TotalMs/Metric.Count:0,Metric.MaxMs); };
 Append(TEXT("Paint"),PaintMetric); Append(TEXT("Move"),MoveMetric);
 Append(TEXT("Release"),ReleaseMetric); Append(TEXT("Hit"),HitMetric);
 return Result;
}
#endif

void SLyraRecoilPatternGraph::Construct(const FArguments& Args) { Editor=Args._Editor; }
FVector2D SLyraRecoilPatternGraph::ToScreen(FVector2D P) const { return ViewSize*.5+Pan+FVector2D(P.X*Scale,-P.Y*Scale); }
FVector2D SLyraRecoilPatternGraph::ToAngle(FVector2D P) const { P=(P-ViewSize*.5-Pan)/Scale; return FVector2D(P.X,-P.Y); }
void SLyraRecoilPatternGraph::Tick(const FGeometry& Geometry,double,float)
{
 ViewSize=Geometry.GetLocalSize();
 auto E=Editor.Pin();
 if((bDragging || bBoxSelecting || bKeyboardGesture) && (!E || E->GetRevision()!=StartRevision)) { CancelDrag(); if(E) E->CancelActiveGesture(); }
 if(bInitialFit && ViewSize.X>10 && ViewSize.Y>10) { FitAll(); bInitialFit=false; }
}
void SLyraRecoilPatternGraph::FitAll() { Fit(false); }
void SLyraRecoilPatternGraph::FitSelection() { Fit(true); }
void SLyraRecoilPatternGraph::Fit(bool bSelection)
{
 auto E=Editor.Pin(); if(!E) return;
 FVector2D Minimum=FVector2D::ZeroVector,Maximum=FVector2D::ZeroVector;
 for(int32 I=0;I<E->GetCumulativePoints().Num();++I)
 {
  if(bSelection && !E->GetSelectedIndices().Contains(I)) continue;
  const FVector2D P=E->GetCumulativePoints()[I];
  if(!FMath::IsFinite(P.X)||!FMath::IsFinite(P.Y)) continue;
  Minimum.X=FMath::Min(Minimum.X,P.X); Minimum.Y=FMath::Min(Minimum.Y,P.Y);
  Maximum.X=FMath::Max(Maximum.X,P.X); Maximum.Y=FMath::Max(Maximum.Y,P.Y);
 }
 if (!bSelection && E->GetProfile())
 {
  const auto* Profile=E->GetProfile();
  FVector2D Tail=E->GetCumulativePoints().IsEmpty()?FVector2D::ZeroVector:E->GetCumulativePoints().Last();
  for(int32 I=Profile->PatternLength;I<Profile->PatternLength+12;++I)
  {
   const auto Kick=FRecoilRuntimeState::ComputeShotKick(*Profile,I,1,1,Profile->FixedRandomSeed);
   Tail+=FVector2D(Kick.Horizontal,Kick.Vertical);
   if(!FMath::IsFinite(Tail.X)||!FMath::IsFinite(Tail.Y)) break;
   Minimum.X=FMath::Min(Minimum.X,Tail.X); Minimum.Y=FMath::Min(Minimum.Y,Tail.Y);
   Maximum.X=FMath::Max(Maximum.X,Tail.X); Maximum.Y=FMath::Max(Maximum.Y,Tail.Y);
  }
 }
 const FVector2D Extent=Maximum-Minimum;
 Scale=FMath::Clamp(FMath::Min((ViewSize.X-90)/FMath::Max(Extent.X,.2),(ViewSize.Y-100)/FMath::Max(Extent.Y,.2)),.01,100000.0);
 const FVector2D Middle=(Minimum+Maximum)*.5;
 Pan=FVector2D(-Middle.X*Scale,Middle.Y*Scale);
}
void SLyraRecoilPatternGraph::CancelDrag()
{
 bDragging=bPanning=bBoxSelecting=bKeyboardGesture=bDragMoved=false;
 BasePoints.Reset(); DraftPoints.Reset(); DraftErrors.Reset();
}
void SLyraRecoilPatternGraph::OnMouseCaptureLost(const FCaptureLostEvent&) { CancelDrag(); if(auto E=Editor.Pin()) E->CancelActiveGesture(); }
void SLyraRecoilPatternGraph::OnFocusLost(const FFocusEvent&) { CancelDrag(); if(auto E=Editor.Pin()) E->CancelActiveGesture(); }
void SLyraRecoilPatternGraph::SelectHit(int32 Index,bool bToggle)
{
 if(auto E=Editor.Pin())
 {
  TArray<int32> Selection=bToggle?E->GetSelectedIndices():TArray<int32>();
  if(bToggle && Selection.Contains(Index)) Selection.Remove(Index); else Selection.AddUnique(Index);
  E->SelectIndices(Selection,false);
 }
}
FReply SLyraRecoilPatternGraph::OnMouseButtonDown(const FGeometry& Geometry,const FPointerEvent& Event)
{
#if WITH_DEV_AUTOMATION_TESTS
 const double MetricStart=FPlatformTime::Seconds(); ON_SCOPE_EXIT { if(bCapturePerformance) HitMetric.Record(MetricStart); };
#endif
 auto E=Editor.Pin(); if(!E) return FReply::Unhandled();
 const FVector2D Local=Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
 StartMouse=Local; StartPan=Pan; StartRevision=E->GetRevision();
 if(Event.GetEffectingButton()==EKeys::MiddleMouseButton) { bPanning=true; return FReply::Handled().CaptureMouse(AsShared()); }
 if(Event.GetEffectingButton()!=EKeys::LeftMouseButton) return FReply::Unhandled();
 TArray<int32> Hits;
 const auto& Points=E->GetCumulativePoints();
 for(int32 I=0;I<Points.Num();++I) if(FVector2D::DistSquared(ToScreen(Points[I]),Local)<=64) Hits.Add(I);
 if(Hits.Num()>1)
 {
  FMenuBuilder Menu(true,nullptr);
  for(int32 Index:Hits) Menu.AddMenuEntry(FText::FromString(FString::Printf(TEXT("第%d发"),Index+1)),FText::GetEmpty(),FSlateIcon(),
   FUIAction(FExecuteAction::CreateSP(this,&SLyraRecoilPatternGraph::SelectHit,Index,Event.IsControlDown())));
  FSlateApplication::Get().PushMenu(AsShared(),FWidgetPath(),Menu.MakeWidget(),Event.GetScreenSpacePosition(),FPopupTransitionEffect::ContextMenu);
  return FReply::Handled().SetUserFocus(AsShared());
 }
 if(!Hits.IsEmpty())
 {
  DragAnchor=Hits[0];
  if(Event.IsControlDown() || !E->GetSelectedIndices().Contains(DragAnchor)) SelectHit(DragAnchor,Event.IsControlDown());
  if(!E->GetSelectedIndices().Contains(DragAnchor)) return FReply::Handled().SetUserFocus(AsShared());
  DragSelection=E->GetSelectedIndices(); BasePoints=Points; DraftPoints=Points; DraftErrors.Reset();
  bDragging=true; bDragMoved=false; E->BeginGesture();
 }
 else { bBoxSelecting=true; bAppendBox=Event.IsShiftDown(); BoxEnd=Local; }
 return FReply::Handled().SetUserFocus(AsShared()).CaptureMouse(AsShared());
}
void SLyraRecoilPatternGraph::UpdateDraft(FVector2D Local)
{
 auto E=Editor.Pin(); if(!E || !BasePoints.IsValidIndex(DragAnchor)) return;
 FVector2D Delta=ToAngle(Local)-ToAngle(StartMouse);
 if(bSnap)
 {
  FVector2D Anchor=BasePoints[DragAnchor]+Delta;
  Anchor.X=FMath::GridSnap(Anchor.X,SnapStep); Anchor.Y=FMath::GridSnap(Anchor.Y,SnapStep);
  Delta=Anchor-BasePoints[DragAnchor];
 }
 DraftPoints=BasePoints;
 for(int32 Index:DragSelection) if(DraftPoints.IsValidIndex(Index)) DraftPoints[Index]+=Delta;
 DraftErrors.Reset(); FLyraRecoilPatternData Candidate;
 if(E->GetProfile()) FLyraRecoilPatternAdapter::MoveCumulative(*E->GetProfile(),DraftPoints,Candidate,DraftErrors);
}
FReply SLyraRecoilPatternGraph::OnMouseMove(const FGeometry& Geometry,const FPointerEvent& Event)
{
#if WITH_DEV_AUTOMATION_TESTS
 const double MetricStart=FPlatformTime::Seconds(); ON_SCOPE_EXIT { if(bCapturePerformance) MoveMetric.Record(MetricStart); };
#endif
 const FVector2D Local=Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
 if(bPanning) { Pan=StartPan+Local-StartMouse; return FReply::Handled(); }
 if(bDragging)
 {
  bDragMoved |= FVector2D::DistSquared(Local,StartMouse)>=FSlateApplication::Get().GetDragTriggerDistanceSquared();
  if(bDragMoved) UpdateDraft(Local);
  return FReply::Handled();
 }
 if(bBoxSelecting) { BoxEnd=Local; return FReply::Handled(); }
 if(auto E=Editor.Pin())
 {
  FString Hover=TEXT("中键平移；滚轮围绕指针缩放；Ctrl多选；Shift框选追加；方向键微调；Esc取消。");
  const auto* P=E->GetProfile();
  for(int32 I=0; P && I<E->GetCumulativePoints().Num(); ++I)
   if(FVector2D::DistSquared(ToScreen(E->GetCumulativePoints()[I]),Local)<=64)
   {
    const auto Point=E->GetCumulativePoints()[I]; const auto Raw=P->PatternPoints[I];
    Hover=FString::Printf(TEXT("第%d发 累计Yaw/Pitch=%.9g/%.9g°\n原值X/Y=%.9g/%.9g；曲线采样=%.9g\nH=%.9g V=%.9g。零或极小分母轴保持原值。"),I+1,Point.X,Point.Y,Raw.X,Raw.Y,P->GetVerticalKickCurveScale(I),P->RecoilPerShot_Horizontal,P->RecoilPerShot_Vertical);
    break;
   }
  SetToolTipText(FText::FromString(Hover));
 }
 return FReply::Unhandled();
}
FReply SLyraRecoilPatternGraph::OnMouseButtonUp(const FGeometry& Geometry,const FPointerEvent& Event)
{
#if WITH_DEV_AUTOMATION_TESTS
 const double MetricStart=FPlatformTime::Seconds(); ON_SCOPE_EXIT { if(bCapturePerformance) ReleaseMetric.Record(MetricStart); };
#endif
 auto E=Editor.Pin(); if(!E) { CancelDrag(); return FReply::Handled().ReleaseMouseCapture(); }
 if(bPanning && Event.GetEffectingButton()==EKeys::MiddleMouseButton) { bPanning=false; return FReply::Handled().ReleaseMouseCapture(); }
 if(Event.GetEffectingButton()!=EKeys::LeftMouseButton) return FReply::Unhandled();
 if(bDragging)
 {
  const FVector2D Local=Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
  bDragMoved |= FVector2D::DistSquared(Local,StartMouse)>=FSlateApplication::Get().GetDragTriggerDistanceSquared();
  if(bDragMoved) UpdateDraft(Local);
  bDragging=false;
  if(!bDragMoved) E->CancelActiveGesture(); // A selection click must never snap or write the point.
  else if(StartRevision!=E->GetRevision()) E->SetDiagnostics({TEXT("资产已更新，拖动草稿已取消。")});
  else if(DraftErrors.IsEmpty()) { E->EndGesture(DraftPoints); RejectedTargets.Reset(); }
  else { RejectedTargets=DraftPoints; E->SetDiagnostics(DraftErrors); E->CancelActiveGesture(); }
 }
 else if(bBoxSelecting)
 {
  TArray<int32> Selection=bAppendBox?E->GetSelectedIndices():TArray<int32>();
  const FVector2D Min(FMath::Min(StartMouse.X,BoxEnd.X),FMath::Min(StartMouse.Y,BoxEnd.Y));
  const FVector2D Max(FMath::Max(StartMouse.X,BoxEnd.X),FMath::Max(StartMouse.Y,BoxEnd.Y));
  for(int32 I=0;I<E->GetCumulativePoints().Num();++I)
  { const auto P=ToScreen(E->GetCumulativePoints()[I]); if(P.X>=Min.X&&P.X<=Max.X&&P.Y>=Min.Y&&P.Y<=Max.Y) Selection.AddUnique(I); }
  E->SelectIndices(Selection,false);
 }
 CancelDrag(); return FReply::Handled().ReleaseMouseCapture();
}
FReply SLyraRecoilPatternGraph::OnMouseWheel(const FGeometry& Geometry,const FPointerEvent& Event)
{
 const FVector2D Local=Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
 const FVector2D Anchor=ToAngle(Local);
 Scale=FMath::Clamp(Scale*FMath::Pow(1.2,Event.GetWheelDelta()),.01,100000.0);
 Pan=Local-ViewSize*.5-FVector2D(Anchor.X*Scale,-Anchor.Y*Scale);
 return FReply::Handled();
}
FReply SLyraRecoilPatternGraph::OnKeyDown(const FGeometry&,const FKeyEvent& Event)
{
 auto E=Editor.Pin(); if(!E) return FReply::Unhandled(); const FKey Key=Event.GetKey();
 if(Key==EKeys::Escape) { CancelDrag(); E->CancelActiveGesture(); return FReply::Handled().ReleaseMouseCapture(); }
 if(Event.IsControlDown()||Event.IsAltDown()) return FReply::Unhandled();
 if(Key==EKeys::Home) { FitAll(); return FReply::Handled(); }
 if(Key==EKeys::F) { FitSelection(); return FReply::Handled(); }
 if(Key==EKeys::G) { bSnap=!bSnap; return FReply::Handled(); }
 FVector2D Delta=FVector2D::ZeroVector;
 if(Key==EKeys::Left) Delta.X=-1; if(Key==EKeys::Right) Delta.X=1;
 if(Key==EKeys::Up) Delta.Y=1; if(Key==EKeys::Down) Delta.Y=-1;
 if(Delta.IsNearlyZero()) return FReply::Unhandled();
 if (!bKeyboardGesture)
 {
  bKeyboardGesture=true; StartRevision=E->GetRevision(); BasePoints=E->GetCumulativePoints(); DraftPoints=BasePoints;
  E->BeginGesture();
 }
 Delta*=SnapStep*(Event.IsShiftDown()?10:1);
 for(int32 Index:E->GetSelectedIndices()) if(DraftPoints.IsValidIndex(Index)) DraftPoints[Index]+=Delta;
 DraftErrors.Reset(); FLyraRecoilPatternData Candidate;
 FLyraRecoilPatternAdapter::MoveCumulative(*E->GetProfile(),DraftPoints,Candidate,DraftErrors);
 return FReply::Handled();
}
FReply SLyraRecoilPatternGraph::OnKeyUp(const FGeometry&,const FKeyEvent& Event)
{
 const FKey Key=Event.GetKey();
 if (!bKeyboardGesture || (Key!=EKeys::Up && Key!=EKeys::Down && Key!=EKeys::Left && Key!=EKeys::Right)) return FReply::Unhandled();
 if(auto E=Editor.Pin())
 {
  if(DraftErrors.IsEmpty()) E->EndGesture(DraftPoints);
  else { RejectedTargets=DraftPoints; E->SetDiagnostics(DraftErrors); E->CancelActiveGesture(); }
 }
 CancelDrag(); return FReply::Handled();
}

int32 SLyraRecoilPatternGraph::OnPaint(const FPaintArgs&,const FGeometry& Geometry,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool) const
{
#if WITH_DEV_AUTOMATION_TESTS
 const double MetricStart=FPlatformTime::Seconds(); ON_SCOPE_EXIT { if(bCapturePerformance) PaintMetric.Record(MetricStart); };
#endif
 ViewSize=Geometry.GetLocalSize();
 const auto Brush=FAppStyle::GetBrush("WhiteBrush");
 FSlateDrawElement::MakeBox(Out,Layer,Geometry.ToPaintGeometry(),Brush,ESlateDrawEffect::None,FLinearColor(.025f,.038f,.06f));
 auto Line=[&](FVector2D A,FVector2D B,FLinearColor Color,float Width=1)
 {
  if(!FMath::IsFinite(A.X)||!FMath::IsFinite(A.Y)||!FMath::IsFinite(B.X)||!FMath::IsFinite(B.Y)) return;
  if(FMath::Max(FMath::Max(FMath::Abs(A.X),FMath::Abs(A.Y)),FMath::Max(FMath::Abs(B.X),FMath::Abs(B.Y)))>1.e6) return;
  TArray<FVector2f> Points={FVector2f(A),FVector2f(B)};
  FSlateDrawElement::MakeLines(Out,Layer+1,Geometry.ToPaintGeometry(),Points,ESlateDrawEffect::None,Color,true,Width);
 };
 auto Label=[&](FVector2D Position,const FString& Text,FLinearColor Color)
 { FSlateDrawElement::MakeText(Out,Layer+3,Geometry.ToPaintGeometry(FVector2f(200,20),FSlateLayoutTransform(FVector2f(Position))),Text,FAppStyle::GetFontStyle("SmallFont"),ESlateDrawEffect::None,Color); };
 double Step=SnapStep; while(Step*Scale<32) Step*=2; while(Step*Scale>160) Step*=.5;
 const FVector2D Low=ToAngle(FVector2D::ZeroVector),High=ToAngle(ViewSize);
 int32 GridGuard=0;
 for(double X=FMath::CeilToDouble(Low.X/Step)*Step;X<High.X && GridGuard++<512;X+=Step) { auto P=ToScreen(FVector2D(X,0)); Line(FVector2D(P.X,0),FVector2D(P.X,ViewSize.Y),FLinearColor(.09f,.13f,.18f)); }
 GridGuard=0;
 for(double Y=FMath::CeilToDouble(High.Y/Step)*Step;Y<Low.Y && GridGuard++<512;Y+=Step) { auto P=ToScreen(FVector2D(0,Y)); Line(FVector2D(0,P.Y),FVector2D(ViewSize.X,P.Y),FLinearColor(.09f,.13f,.18f)); }
 const FVector2D Origin=ToScreen(FVector2D::ZeroVector);
 Line(FVector2D(Origin.X,0),FVector2D(Origin.X,ViewSize.Y),FLinearColor(.4f,.5f,.6f));
 Line(FVector2D(0,Origin.Y),FVector2D(ViewSize.X,Origin.Y),FLinearColor(.4f,.5f,.6f));
 Label(FVector2D(10,8),TEXT("累计配置 Kick（度）  X=Yaw →  Y=Pitch ↑"),FLinearColor::White);
 Label(FVector2D(10,27),FString::Printf(TEXT("固定点可编辑；随机尾段虚线只读。吸附%s %.4g°"),bSnap?TEXT("开"):TEXT("关"),SnapStep),FLinearColor(.5f,.8f,.8f));
 auto E=Editor.Pin(); if(!E) return Layer+4;
 const auto& Points=(bDragging||bKeyboardGesture)?DraftPoints:E->GetCumulativePoints();
 TSet<int32> SelectedSet(E->GetSelectedIndices());
 FVector2D Previous=Origin;
 for(int32 I=0;I<Points.Num();++I)
 {
  const FVector2D P=ToScreen(Points[I]); Line(Previous,P,DraftErrors.IsEmpty()?FLinearColor(.2f,.7f,.65f):FLinearColor(1,.25f,.2f),2); Previous=P;
  if(P.X<0||P.Y<0||P.X>ViewSize.X||P.Y>ViewSize.Y||!FMath::IsFinite(P.X)||!FMath::IsFinite(P.Y)) continue;
  const bool bSelected=SelectedSet.Contains(I);
  FSlateDrawElement::MakeBox(Out,Layer+2,Geometry.ToPaintGeometry(FVector2f(10,10),FSlateLayoutTransform(FVector2f(P-FVector2D(5,5)))),Brush,ESlateDrawEffect::None,bSelected?FLinearColor(1,.7f,.15f):FLinearColor(.25f,.8f,.75f));
  Label(P+FVector2D(7,-9),FString::FromInt(I+1),FLinearColor::White);
  if(bDragging && BasePoints.IsValidIndex(I)) Line(ToScreen(BasePoints[I]),P,FLinearColor(.7f,.5f,.2f));
 }
 if(const ULyraRecoilProfile* Profile=E->GetProfile())
 {
  FVector2D Cumulative=Points.IsEmpty()?FVector2D::ZeroVector:Points.Last();
  for(int32 I=Profile->PatternLength;I<Profile->PatternLength+12;++I)
  {
   const auto Kick=FRecoilRuntimeState::ComputeShotKick(*Profile,I,1,1,Profile->FixedRandomSeed);
   if(!FMath::IsFinite(Kick.Horizontal)||!FMath::IsFinite(Kick.Vertical)) break;
   const FVector2D Next=Cumulative+FVector2D(Kick.Horizontal,Kick.Vertical);
   const FVector2D A=ToScreen(Cumulative),B=ToScreen(Next);
   for(int32 Dash=0;Dash<8;Dash+=2) Line(FMath::Lerp(A,B,Dash/8.0),FMath::Lerp(A,B,(Dash+1)/8.0),FLinearColor(.45f,.48f,.65f));
   Cumulative=Next;
  }
 }
 if(bBoxSelecting)
 {
  Line(StartMouse,FVector2D(BoxEnd.X,StartMouse.Y),FLinearColor::Yellow); Line(FVector2D(BoxEnd.X,StartMouse.Y),BoxEnd,FLinearColor::Yellow);
  Line(BoxEnd,FVector2D(StartMouse.X,BoxEnd.Y),FLinearColor::Yellow); Line(FVector2D(StartMouse.X,BoxEnd.Y),StartMouse,FLinearColor::Yellow);
 }
 if(!DraftErrors.IsEmpty()) Label(FVector2D(10,ViewSize.Y-28),DraftErrors[0],FLinearColor(1,.45f,.3f));
 return Layer+4;
}
