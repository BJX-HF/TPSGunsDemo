from pathlib import Path
root = Path(r'D:\TPSGunsDemo\TPSGunsDemo\Source\LyraEditor\Private\Recoil')
p = root / 'LyraRecoilProfileEditor.cpp'
s = p.read_text(encoding='utf-8')
def replace_function(name, body):
    global s
    start = s.index(name)
    brace = s.index('{', start)
    level = 1
    end = brace + 1
    while level:
        level += (s[end] == '{') - (s[end] == '}')
        end += 1
    s = s[:brace] + body + s[end:]
replace_function('void FLyraRecoilProfileEditor::OnDetailsFinishedChangingProperties', '''{
    // OnObjectPropertyChanged is the single refresh route for ordinary Details edits.
}''')
replace_function('void FLyraRecoilProfileEditor::RebindCurveDependencies', '''{
    UnbindCurveDependencies();
    if (!Profile) return;
    for (FName Name : FLyraRecoilEditOperations::GetCurvePropertyNames())
    {
        if (FLyraRecoilEditOperations::GetExternalCurve(Profile, Name))
        {
            CurveDependencyHandles.Add(FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
                this, &FLyraRecoilProfileEditor::HandleCurvePropertyChanged));
            break; // One global delegate, filtered against all current references.
        }
    }
}''')
replace_function('void FLyraRecoilProfileEditor::HandleObjectTransacted', '''{
    // Undo/redo restoration is refreshed once, after all transaction objects restore, in PostUndo.
}''')
replace_function('TSharedRef<SWidget> FLyraRecoilProfileEditor::CreatePreview', '''{
    return SAssignNew(PreviewWidget, SLyraRecoilPreview).Profile(Profile);
}''')
replace_function('TSharedRef<SWidget> FLyraRecoilProfileEditor::CreatePatternGraph', '''{
    return SNew(SVerticalBox)
    + SVerticalBox::Slot().AutoHeight()
    [ SNew(SHorizontalBox)
      + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("适配全部 Home")))
        .OnClicked_Lambda([this] { if (PatternGraph) PatternGraph->FitAll(); return FReply::Handled(); })]
      + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("适配选择 F")))
        .OnClicked_Lambda([this] { if (PatternGraph) PatternGraph->FitSelection(); return FReply::Handled(); })]
      + SHorizontalBox::Slot().AutoWidth()[SNew(SCheckBox)
        .IsChecked_Lambda([this] { return PatternGraph && PatternGraph->IsSnapEnabled() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { if (PatternGraph) PatternGraph->SetSnapEnabled(State==ECheckBoxState::Checked); })
        [SNew(STextBlock).Text(FText::FromString(TEXT("吸附 G")))]]
      + SHorizontalBox::Slot().AutoWidth()[SNew(SNumericEntryBox<double>).MinDesiredValueWidth(60).MinValue(.000001).MaxValue(10000)
        .Value_Lambda([this]() -> TOptional<double> { return PatternGraph ? PatternGraph->GetSnapStep() : .1; })
        .OnValueCommitted_Lambda([this](double Value,ETextCommit::Type) { if (PatternGraph) PatternGraph->SetSnapStep(Value); })]
    ]
    + SVerticalBox::Slot().FillHeight(1)[SAssignNew(PatternGraph, SLyraRecoilPatternGraph).Editor(SharedThis(this))]
    + SVerticalBox::Slot().AutoHeight()[SNew(SButton).Text(FText::FromString(TEXT("高级：调整基础强度以容纳被拒绝的目标")))
        .IsEnabled_Lambda([this] { return PatternGraph && !PatternGraph->GetRejectedTargets().IsEmpty(); })
        .OnClicked_Lambda([this] { AdjustStrengthForRejectedTargets(); return FReply::Handled(); })]
    + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true)
        .Text_Lambda([this] { return FText::FromString(FString::Join(Diagnostics,TEXT("\\n"))); })];
}''')
s = s.replace('void FLyraRecoilProfileEditor::HandleProfilePropertyChanged', '''FLyraRecoilProfileEditor::~FLyraRecoilProfileEditor()
{
    UnbindExternalListeners();
    UnregisterEditorInstance();
    if (PreviewWidget) PreviewWidget->Cleanup();
    if (GEditor) GEditor->UnregisterForUndo(this);
}

void FLyraRecoilProfileEditor::PostUndo(bool bSuccess)
{
    if (!bSuccess || !Profile) return;
    CancelActiveGesture();
    if (PatternGraph) PatternGraph->CancelDrag();
    if (Session) Session->BumpRevision();
    RebindCurveDependencies();
    RefreshFromAsset();
}

bool FLyraRecoilProfileEditor::AdjustStrengthForRejectedTargets()
{
    if (!Profile || !PatternGraph) return false;
    FLyraRecoilStrengthAdjustment Adjustment;
    FLyraRecoilPatternData Candidate;
    TArray<FString> Errors;
    const auto Targets = PatternGraph->GetRejectedTargets();
    if (!FLyraRecoilPatternAdapter::BuildStrengthAdjustmentCandidate(*Profile,Targets,Adjustment,Candidate,Errors))
    { SetDiagnostics(Errors); return false; }
    const FText Message = FText::FromString(FString::Join(Adjustment.Differences,TEXT("\\n"))
        + TEXT("\\n\\n随机尾段采样仅供比较；上限、姿态、曲线、随机区间保持当前值。确认提交一个事务？"));
    if (FMessageDialog::Open(EAppMsgType::YesNo,Message)!=EAppReturnType::Yes) return false;
    const bool bCommitted=Operations.CommitStrengthAdjustment(Targets,Errors);
    SetDiagnostics(Errors);
    if (bCommitted) RefreshFromAsset();
    return bCommitted;
}

void FLyraRecoilProfileEditor::HandleProfilePropertyChanged''')
needle = '\t// 外部修改到来时先终止本地拖动，再接收最终资产值，不覆盖外部新值。\n\tCancelActiveGesture();'
s = s.replace(needle, needle + '''
    if (PatternGraph) PatternGraph->CancelDrag();
    if (Session && (!Event.MemberProperty || (Event.MemberProperty->GetFName()==GET_MEMBER_NAME_CHECKED(ULyraRecoilProfile,PatternPoints)
        && (Event.ChangeType!=EPropertyChangeType::ValueSet || Event.GetArrayIndex(TEXT("PatternPoints"))==INDEX_NONE))))
    {
        Session->RebuildNodeIds();
        Session->AddDiagnostic(TEXT("外部数组结构无法证明未变：节点身份已重建，选择已清空。"));
    }''')
s = s.replace('\tif (!bIsBoundDependency)\n\t{\n\t\treturn;\n\t}\n', '\tif (!bIsBoundDependency)\n\t{\n\t\treturn;\n\t}\n\tCancelActiveGesture();\n\tif (PatternGraph) PatternGraph->CancelDrag();\n')
s = s.replace('\t\t\t\tProfile = NewProfile;', '\t\t\t\tCancelActiveGesture();\n\t\t\t\tif (PatternGraph) PatternGraph->CancelDrag();\n\t\t\t\tUnregisterEditorInstance();\n\t\t\t\tProfile = NewProfile;\n\t\t\t\tRegisterEditorInstance();\n\t\t\t\tif (PreviewWidget) PreviewWidget->SetProfile(Profile);')
# Toolkit-local undo commands. PostUndo handles the coherent refresh.
start = s.index('void FLyraRecoilProfileEditor::MapToolkitCommands()')
brace = s.index('{',start)
s = s[:brace+1] + '''
    GetToolkitCommands()->MapAction(FGenericCommands::Get().Undo,
        FExecuteAction::CreateLambda([] { if (GEditor) GEditor->UndoTransaction(); }));
    GetToolkitCommands()->MapAction(FGenericCommands::Get().Redo,
        FExecuteAction::CreateLambda([] { if (GEditor) GEditor->RedoTransaction(); }));
''' + s[brace+1:]
p.write_text(s,encoding='utf-8')
