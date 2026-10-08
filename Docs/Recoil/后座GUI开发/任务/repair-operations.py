from pathlib import Path
p=Path(r'D:\TPSGunsDemo\TPSGunsDemo\Source\LyraEditor\Private\Recoil\LyraRecoilEditOperations.cpp')
s=p.read_text(encoding='utf-8')
start=s.index('void FLyraRecoilEditOperations::ApplyPatternData')
end=s.index('void FLyraRecoilEditOperations::NotifyPropertyChanged',start)
s=s[:start]+'''void FLyraRecoilEditOperations::ApplyPatternData(ULyraRecoilProfile* Target, const FLyraRecoilPatternData& Data)
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

''' + s[end:]
s=s.replace('bool bValid = Target->ValidateProfile(OutErrors);\n\tbValid &= FLyraRecoilPatternAdapter::Validate(*Target, OutErrors);\n\tbValid &= CheckFiniteNumbers(*Target, OutErrors);','bool bValid = FLyraRecoilPatternAdapter::Validate(*Target, OutErrors);')
s=s.replace('bool bCandidateValid = Candidate->ValidateProfile(CandidateErrors);\n\tbCandidateValid &= FLyraRecoilPatternAdapter::Validate(*Candidate, CandidateErrors);\n\tbCandidateValid &= CheckFiniteNumbers(*Candidate, CandidateErrors);','bool bCandidateValid = FLyraRecoilPatternAdapter::Validate(*Candidate, CandidateErrors);')
s=s.replace('return InsertPoints(Target->PatternPoints.Num(), Points, /*bFixedAtBoundary=*/false, OutErrors);','const auto* Sess=Session.Get();\n\tconst int32 Index=Sess && !Sess->GetSelectedIndices().IsEmpty()?Sess->GetSelectedIndices()[0]:Target->PatternPoints.Num();\n\treturn InsertPoints(Index, Points, /*bFixedAtBoundary=*/false, OutErrors);')
s=s.replace('const int32 FirstTargetIndex = Target->PatternPoints.Num();','const auto* Sess=Session.Get();\n\tconst int32 FirstTargetIndex=Sess && !Sess->GetSelectedIndices().IsEmpty()?Sess->GetSelectedIndices()[0]:Target->PatternPoints.Num();')
s=s.replace('\t\tTarget->Modify();\n\t\tCurve->EditorCurveData', '\t\tTGuardValue<bool> Guard(bIsCommitting,true);\n\t\tTarget->Modify();\n\t\tTarget->PreEditChange(Property);\n\t\tCurve->EditorCurveData')
s=s.replace('\t\tTarget->Modify();\n\t\tCurve->ExternalCurve = NewCurve;', '\t\tTGuardValue<bool> Guard(bIsCommitting,true);\n\t\tTarget->Modify();\n\t\tTarget->PreEditChange(Property);\n\t\tCurve->ExternalCurve = NewCurve;')
# All curve copying uses the complete adapter's numerical safety checks.
s=s.replace('const bool bFinite = LyraRecoilEditOperationsLocal::CheckRuntimeCurveFinite(*Curve, *PropertyName.ToString(), OutErrors);','const bool bFinite = FLyraRecoilPatternAdapter::Validate(*Target, OutErrors);')
insert=s.index('bool FLyraRecoilEditOperations::CommitPointNormalized')
s=s[:insert]+'''bool FLyraRecoilEditOperations::CommitStrengthAdjustment(const TArray<FVector2D>& Targets,TArray<FString>& Errors)
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

''' + s[insert:]
p.write_text(s,encoding='utf-8')
