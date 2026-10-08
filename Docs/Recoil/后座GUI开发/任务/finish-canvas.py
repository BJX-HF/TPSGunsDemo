from pathlib import Path
r=Path(r'D:\TPSGunsDemo\TPSGunsDemo\Source\LyraEditor\Private\Recoil')
p=r/'SLyraRecoilPatternGraph.h';s=p.read_text(encoding='utf-8')
if 'OnKeyUp' not in s:s=s.replace(' virtual void OnMouseCaptureLost',' virtual FReply OnKeyUp(const FGeometry&,const FKeyEvent&) override;\n virtual void OnMouseCaptureLost')
if 'bKeyboardGesture' not in s:s=s.replace(' FVector2D StartMouse',' bool bKeyboardGesture=false;\n FVector2D StartMouse')
p.write_text(s,encoding='utf-8')
p=r/'SLyraRecoilPatternGraph.cpp';s=p.read_text(encoding='utf-8')
s=s.replace('(bDragging || bBoxSelecting)','(bDragging || bBoxSelecting || bKeyboardGesture)')
s=s.replace('bDragging=bPanning=bBoxSelecting=false','bDragging=bPanning=bBoxSelecting=bKeyboardGesture=false')
s=s.replace('const auto& Points=bDragging?DraftPoints:E->GetCumulativePoints();','const auto& Points=(bDragging||bKeyboardGesture)?DraftPoints:E->GetCumulativePoints();\n TSet<int32> SelectedSet(E->GetSelectedIndices());')
s=s.replace('const bool bSelected=E->GetSelectedIndices().Contains(I);','const bool bSelected=SelectedSet.Contains(I);')
s=s.replace('for(double X=FMath::CeilToDouble(Low.X/Step)*Step;X<High.X;X+=Step)', 'int32 GridGuard=0;\n for(double X=FMath::CeilToDouble(Low.X/Step)*Step;X<High.X && GridGuard++<512;X+=Step)')
s=s.replace('for(double Y=FMath::CeilToDouble(High.Y/Step)*Step;Y<Low.Y;Y+=Step)', 'GridGuard=0;\n for(double Y=FMath::CeilToDouble(High.Y/Step)*Step;Y<Low.Y && GridGuard++<512;Y+=Step)')
start=s.index(' TArray<FVector2D> Targets=E->GetCumulativePoints(); Delta*=SnapStep')
end=s.index('\nint32 SLyraRecoilPatternGraph::OnPaint',start)
s=s[:start]+''' if (!bKeyboardGesture)
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
''' + s[end:]
s=s.replace(' if(bBoxSelecting) { BoxEnd=Local; return FReply::Handled(); }\n return FReply::Unhandled();',''' if(bBoxSelecting) { BoxEnd=Local; return FReply::Handled(); }
 if(auto E=Editor.Pin())
 {
  FString Hover=TEXT("中键平移；滚轮围绕指针缩放；Ctrl多选；Shift框选追加；方向键微调；Esc取消。");
  const auto* P=E->GetProfile();
  for(int32 I=0; P && I<E->GetCumulativePoints().Num(); ++I)
   if(FVector2D::DistSquared(ToScreen(E->GetCumulativePoints()[I]),Local)<=64)
   {
    const auto Point=E->GetCumulativePoints()[I]; const auto Raw=P->PatternPoints[I];
    Hover=FString::Printf(TEXT("第%d发 累计Yaw/Pitch=%.9g/%.9g°\\n原值X/Y=%.9g/%.9g；曲线采样=%.9g\\nH=%.9g V=%.9g。零或极小分母轴保持原值。"),I+1,Point.X,Point.Y,Raw.X,Raw.Y,P->GetVerticalKickCurveScale(I),P->RecoilPerShot_Horizontal,P->RecoilPerShot_Vertical);
    break;
   }
  SetToolTipText(FText::FromString(Hover));
 }
 return FReply::Unhandled();''')
p.write_text(s,encoding='utf-8')
