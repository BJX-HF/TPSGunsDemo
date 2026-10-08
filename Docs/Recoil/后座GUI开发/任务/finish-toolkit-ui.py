from pathlib import Path
p=Path(r'D:\TPSGunsDemo\TPSGunsDemo\Source\LyraEditor\Private\Recoil\LyraRecoilProfileEditor.cpp')
s=p.read_text(encoding='utf-8')
s=s.replace('#include "IDetailsView.h"','#include "IDetailsView.h"\n#include "HAL/PlatformApplicationMisc.h"')
for name,angle in [('OnPasteCommand',False),('OnPasteByAngleCommand',True)]:
 start=s.index('void FLyraRecoilProfileEditor::'+name+'()'); end=s.index('\nvoid ',start+5)
 body='''void FLyraRecoilProfileEditor::NAME()
{
    if (!Profile) return;
    FString Payload; FPlatformApplicationMisc::ClipboardPaste(Payload);
    TArray<FString> Errors;
    TArray<FRecoilPatternPoint> Points;
    const auto& Selected=GetSelectedIndices();
    const int32 Index=Selected.IsEmpty()?Profile->PatternPoints.Num():Selected[0];
    CONVERT
    InsertPoints(Index,Points,false);
}
'''.replace('NAME',name)
 if angle:body=body.replace('CONVERT','FLyraRecoilClipboardData Clipboard;\n    if (!FLyraRecoilPatternAdapter::ParseClipboardEx(Payload,Clipboard,Errors)\n        || !FLyraRecoilPatternAdapter::ConvertClipboardAngles(*Profile,Clipboard,Index,Points,Errors))\n    { SetDiagnostics(Errors); return; }')
 else:body=body.replace('CONVERT','if (!FLyraRecoilPatternAdapter::ParseClipboard(Payload,Points,Errors)) { SetDiagnostics(Errors); return; }')
 s=s[:start]+body+s[end:]
start=s.index('\t// 执行前必须展示重排与曲线索引变化：先确认，再提交。');end=s.index('\tFLyraRecoilPatternData Candidate;',start)
s=s[:start]+s[end:]
p.write_text(s,encoding='utf-8')
p=Path(r'D:\TPSGunsDemo\TPSGunsDemo\Source\LyraEditor\Private\Recoil\SLyraRecoilShotTable.cpp')
s=p.read_text(encoding='utf-8')
s=s.replace('#include "Widgets/Layout/SBorder.h"','#include "Widgets/Layout/SBorder.h"\n#include "Widgets/Layout/SScrollBox.h"\n#include "Widgets/Layout/SBox.h"\n#include "Widgets/Layout/SWrapBox.h"')
start=s.index('// 结构命令工具栏');end=s.index('\n\t\t+ SVerticalBox::Slot()\n\t\t.FillHeight',start)
part=s[start:end].replace('SNew(SHorizontalBox)','SNew(SWrapBox).UseAllottedSize(true)').replace('SHorizontalBox::Slot().AutoWidth()','SWrapBox::Slot()')
s=s[:start]+part+s[end:]
s=s.replace('SAssignNew(ListView, SListView<TSharedPtr<FLyraRecoilShotRow>>)','SNew(SScrollBox).Orientation(Orient_Horizontal)\n                +SScrollBox::Slot()[SNew(SBox).WidthOverride(1140)\n                [SAssignNew(ListView, SListView<TSharedPtr<FLyraRecoilShotRow>>)')
s=s.replace('.OnSelectionChanged(this, &SLyraRecoilShotTable::OnSelectionChanged)\n\t\t\t]', '.OnSelectionChanged(this, &SLyraRecoilShotTable::OnSelectionChanged)]]\n\t\t\t]')
p.write_text(s,encoding='utf-8')
