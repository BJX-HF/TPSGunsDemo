from pathlib import Path
root=Path(r'D:\TPSGunsDemo\TPSGunsDemo\Source\LyraEditor\Private\Recoil')
p=root/'SLyraRecoilPreview.cpp'
s=p.read_text(encoding='utf-8')
s=s.replace('#include "Widgets/Layout/SBorder.h"','#include "Widgets/Layout/SBorder.h"\n#include "Widgets/Layout/SBox.h"')
s=s.replace('// 曲线由本控件的 OnPaint 直接绘制（自绘层），这里只占位并让出空间。\n\t\t\t\t\tSNew(SBorder)', '// Plot has its own observed geometry and clip rectangle.\n\t\t\t\t\tSAssignNew(CurveArea, SBorder)')
s=s.replace('\t\t\tBuildParameterPanel()','\t\t\tSNew(SBox).HeightOverride(140)[SNew(SScrollBox)+SScrollBox::Slot()[BuildParameterPanel()]]')
s=s.replace('\tController->Run();','\t// Simulation is incrementally computed by the active timer.')
s=s.replace('\t// 先把结果准备好，保证第一帧就有曲线（预算增量，不会长时间阻塞）。\n\tEnsureResults();','\t// Painting only reads cached results; no simulation work on the paint path.')
s=s.replace('const FVector2f PanelSize = FVector2f(AllottedGeometry.GetLocalSize());','if (!CurveArea) return ResultLayer;\n\tconst FGeometry PlotGeometry=CurveArea->GetCachedGeometry();\n\tconst FVector2f PanelSize = FVector2f(PlotGeometry.GetLocalSize());\n\tif (PanelSize.X<2 || PanelSize.Y<2) return ResultLayer;')
s=s.replace('const FPaintGeometry PaintGeometry = AllottedGeometry.ToPaintGeometry();','const FPaintGeometry PaintGeometry = PlotGeometry.ToPaintGeometry();\n\tOutDrawElements.PushClip(FSlateClippingZone(PlotGeometry));')
s=s.replace('\treturn CurrentLayer;','\tOutDrawElements.PopClip();\n\treturn CurrentLayer;')
s=s.replace('Sample.CameraOffsetPitch, Sample.CameraOffsetYaw, Sample.VisibleAnglePitch));','Sample.CameraOffsetPitch, Sample.CameraOffsetYaw, Sample.VisibleAnglePitch));\n\t\tMinValue=FMath::Min(MinValue,Sample.TheoreticalKickPitch);')
# The preceding broad replacement hit both Min and Max lines; explicit extra max covers theory.
s=s.replace('\tMinValue = FMath::Min(MinValue, 0.0f);','\tfor (const auto& Sample:Samples) MaxValue=FMath::Max(MaxValue,Sample.TheoreticalKickPitch);\n\tMinValue = FMath::Min(MinValue, 0.0f);')
p.write_text(s,encoding='utf-8')
p=root/'LyraRecoilProfileEditor.cpp'
s=p.read_text(encoding='utf-8')
start=s.index('\tconst TSharedRef<FTabManager::FLayout> Layout')
end=s.index('\n\tFAssetEditorToolkit::InitAssetEditor',start)
s=s[:start]+'''	const TSharedRef<FTabManager::FLayout> Layout=FTabManager::NewLayout("LyraRecoilProfileEditor_Layout_v2")
        ->AddArea(FTabManager::NewPrimaryArea()->SetOrientation(Orient_Vertical)
        ->Split(FTabManager::NewSplitter()->SetOrientation(Orient_Horizontal)->SetSizeCoefficient(.6f)
            ->Split(FTabManager::NewStack()->SetSizeCoefficient(.3f)->AddTab(LyraRecoilProfileEditorTabs::TableTabId,ETabState::OpenedTab))
            ->Split(FTabManager::NewStack()->SetSizeCoefficient(.4f)->AddTab(LyraRecoilProfileEditorTabs::GraphTabId,ETabState::OpenedTab))
            ->Split(FTabManager::NewStack()->SetSizeCoefficient(.3f)->AddTab(LyraRecoilProfileEditorTabs::DetailsTabId,ETabState::OpenedTab)))
        ->Split(FTabManager::NewStack()->SetSizeCoefficient(.4f)->AddTab(LyraRecoilProfileEditorTabs::PreviewTabId,ETabState::OpenedTab)));
''' + s[end:]
s=s.replace('\t\t\t\tProfile = NewProfile;','\t\t\t\tRemoveEditingObject(Profile);\n\t\t\t\tProfile = NewProfile;\n\t\t\t\tAddEditingObject(Profile);')
# Save and external updates discard graph drafts as well as the session draft.
marker='void FLyraRecoilProfileEditor::CancelActiveGesture()\n{'
s=s.replace(marker,marker+'\n\tif (PatternGraph) PatternGraph->CancelDrag();')
p.write_text(s,encoding='utf-8')
p=Path(r'D:\TPSGunsDemo\TPSGunsDemo\Source\LyraEditor\Tests\LyraRecoilEditorRegressionTest.cpp')
s=p.read_text(encoding='utf-8').replace('Row.Contains(TEXT("seed="))','Row.Contains(TEXT("种子"))')
p.write_text(s,encoding='utf-8')
