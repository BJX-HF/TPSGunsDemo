from pathlib import Path

root = Path(r'D:\TPSGunsDemo\TPSGunsDemo')
def edit(relative, transform):
    p = root / relative
    p.write_text(transform(p.read_text(encoding='utf-8-sig')), encoding='utf-8', newline='\n')

def repair_step(text):
    start = text.index('int32 FLyraRecoilPreviewController::RunOneAtomicStep()')
    end = text.index('void FLyraRecoilPreviewController::FireShot', start)
    replacement = r'''int32 FLyraRecoilPreviewController::RunOneAtomicStep()
{
    const ULyraRecoilProfile* P = Snapshot.Profile;
    if (!P) { bSimulationComplete = true; return 0; }
    const float FrameEnd = FMath::Min(Config.FrameSeconds * (FrameIndex + 1), PlannedDurationSeconds);
    int32 Calls = 0;
    for (int32 Guard = 0; Guard < 16384; ++Guard)
    {
        // Input at a timestamp takes effect before firing at that same timestamp.
        float Pitch, Yaw, Alpha, Scale;
        EPoseState Pose;
        EvaluateInputAtTime(CurrentTimeSeconds, Pitch, Yaw, Pose, Alpha, Scale);
        RecoilState.SetGlobalScale(Scale);
        RecoilState.SetPoseMultiplier(FRecoilRuntimeState::ComputePoseMultiplier(*P, Pose, Alpha));
        RecoilState.SetSpreadPlayerMultipliers(P->GetSpreadAimingMultiplier(Alpha), Config.SpreadMovementMultiplier);
        RecoilState.SamplePlayerAim(Pitch, Yaw);
        while (NextFireIndex < FireTimes.Num() && FireTimes[NextFireIndex] <= CurrentTimeSeconds + 1.e-6f
            && FireTimes[NextFireIndex] <= PlannedDurationSeconds + 1.e-6f)
        {
            FireShot(FireTimes[NextFireIndex++]);
        }
        if (CurrentTimeSeconds >= FrameEnd - 1.e-6f) break;
        float Next = FrameEnd;
        if (NextFireIndex < FireTimes.Num() && FireTimes[NextFireIndex] > CurrentTimeSeconds + 1.e-6f)
            Next = FMath::Min(Next, FireTimes[NextFireIndex]);
        for (const auto& Input : Config.InputScript)
            if (Input.TimeSeconds > CurrentTimeSeconds + 1.e-6f)
            { Next = FMath::Min(Next, Input.TimeSeconds); break; }
        const float Delta = Next - CurrentTimeSeconds;
        if (!(Delta > 0)) { bNumericalFailure = true; Warnings.Add(TEXT("回放事件无法前进。")); break; }
        RecoilState.Advance(P, Delta);
        if (Config.bAdvanceSpread) RecoilState.AdvanceSpread(P, Delta, Pose);
        CurrentTimeSeconds = Next;
        ++Calls;
        if (SampleMode == ELyraRecoilPreviewSampleMode::EveryEvent && Next < FrameEnd - 1.e-6f)
            RecordSample(Next, false, SampleMode);
        if (bNumericalFailure) break;
    }
    if (bRecordFrameBoundarySamples) RecordSample(CurrentTimeSeconds, false, SampleMode);
    FrameEndSeconds = FrameEnd;
    bFrameInProgress = false;
    ++FrameIndex;
    ++ExecutedStepCount;
    LastAdvanceCallCount = Calls;
    bSimulationComplete = CurrentTimeSeconds >= PlannedDurationSeconds - 1.e-6f;
    return Calls;
}

'''
    return text[:start] + replacement + text[end:]

edit('Source/LyraEditor/Private/Recoil/LyraRecoilPreviewController.cpp', repair_step)

def repair_shot(text):
    start = text.index('\t// --- 理论累计 Kick（倍率 1')
    end = text.index('\t// --- 散布加热', start)
    text = text[:start] + text[end:]
    marker = '\tRecoilState.ApplyShot(SnapshotProfile, PoseMultiplier, Pose);'
    text = text.replace(marker, r'''
    if (!RecoilState.ApplyShot(SnapshotProfile, PoseMultiplier, Pose))
    { bNumericalFailure = true; return; }
    const FRecoilShotResult& Applied = RecoilState.ShotHistory.Last();
    const FRecoilShotKick TheoreticalKick = FRecoilRuntimeState::ComputeShotKick(
        *SnapshotProfile, Applied.ShotIndex, 1.0f, 1.0f, RecoilState.ActiveSeed);
    TheoreticalBeforeShotPitch = TheoreticalKickPitch;
    TheoreticalBeforeShotYaw = TheoreticalKickYaw;
    TheoreticalKickPitch += TheoreticalKick.Vertical;
    TheoreticalKickYaw += TheoreticalKick.Horizontal;''')
    # Existing ShotIndex is explicitly the projectile's pre-ApplyShot index.
    # ActualShotIndex is a separate diagnostic for camera/theoretical channels.
    text = text.replace('\tShot.ShotIndex = ShotIndexAtFire;', '\tShot.ShotIndex = ShotIndexAtFire;\n\tShot.ActualShotIndex = Applied.ShotIndex;')
    marker = '\tSamples.Add(Sample);'
    text = text.replace(marker, r'''
    const float Values[] = { Sample.TimeSeconds, Sample.TheoreticalKickPitch, Sample.TheoreticalKickYaw,
        Sample.CameraOffsetPitch, Sample.CameraOffsetYaw, Sample.CameraOffsetRoll,
        Sample.AccumulatedPitch, Sample.AccumulatedYaw, Sample.ControlRotationPitch,
        Sample.ControlRotationYaw, Sample.VisibleAnglePitch, Sample.VisibleAngleYaw,
        Sample.RecoveryPeakPitch, Sample.RecoveryPeakYaw, Sample.RecoveryCoverPitch,
        Sample.RecoveryCoverYaw, Sample.RecoveryProgress, RecoilState.CurrentSpreadAngle };
    for (float Value : Values)
        if (!FMath::IsFinite(Value))
        { bNumericalFailure = true; Warnings.AddUnique(TEXT("运行时回放产生非有限角度，已拒绝输出绘图数据。")); return; }
    Samples.Add(Sample);''')
    return text

edit('Source/LyraEditor/Private/Recoil/LyraRecoilPreviewController.cpp', repair_shot)
edit('Source/LyraEditor/Private/Recoil/LyraRecoilPreviewController.h', lambda t: t.replace('struct FLyraRecoilPreviewShot\n{', 'struct FLyraRecoilPreviewShot\n{\n\tint32 ActualShotIndex = 0;'))

def repair_adapter(text):
    text = text.replace('#include "Weapons/Recoil/LyraRecoilProfile.h"', '#include "Weapons/Recoil/LyraRecoilProfile.h"\n#include "Weapons/Recoil/LyraRecoilState.h"')
    text = text.replace('!IsFiniteValue(static_cast<double>(Key.LeaveTangent)))', '!IsFiniteValue(static_cast<double>(Key.LeaveTangent)) ||\n\t\t\t\t!IsFiniteValue(Key.ArriveTangentWeight) || !IsFiniteValue(Key.LeaveTangentWeight))')
    text = text.replace('\t\tfor (int32 KeyIndex = 0; KeyIndex < RichCurve->Keys.Num(); ++KeyIndex)', '\t\tif (!IsFiniteValue(RichCurve->DefaultValue)) Errors.Add(FString::Printf(TEXT("%s default value is not finite"), CurveName));\n\t\tfor (int32 KeyIndex = 0; KeyIndex < RichCurve->Keys.Num(); ++KeyIndex)')
    marker = '\tif (NewHorizontal != OldHorizontal)'
    pos = text.index(marker, text.index('bool FLyraRecoilPatternAdapter::BuildStrengthAdjustmentCandidate'))
    tail = r'''
    // Preserve the effective vertical angle of every array tail shot, even when L is zero.
    for (int32 Index = FixedLength; Index < Candidate.Num(); ++Index)
    {
        const double C = Profile.GetVerticalKickCurveScale(Index);
        const double Target = OldVertical * Source.Points[Index].Y * C;
        const double Denominator = NewVertical * C;
        if (!IsFiniteValue(C) || !IsFiniteValue(Target) || !IsFiniteValue(Denominator))
        { Errors.Add(TEXT("数组尾段曲线或角度非有限。")); return false; }
        if (FMath::Abs(Denominator) > DenominatorEpsilon)
        {
            const double Y = Target / Denominator;
            if (!CheckNormalizedValue(Index, TEXT("Y"), Y, 0, 1, Report, Errors)) return false;
            const float Stored = static_cast<float>(FMath::Clamp(Y, 0.0, 1.0));
            if (Stored != Candidate[Index].Y)
            {
                OutAdjustment.Differences.Add(FString::Printf(TEXT("尾段第%d发Y: %.8g -> %.8g，垂直角度保持%.8g°"), Index+1, Candidate[Index].Y, Stored, Target));
                if (FMath::Abs(OldVertical*C) <= DenominatorEpsilon && Stored == 0 && Candidate[Index].Y != 0)
                    OutAdjustment.HiddenValuesForcedToZero.AddUnique(Index);
                Candidate[Index].Y = Stored;
            }
            if (FMath::Abs(NewVertical*Candidate[Index].Y*C-Target) > GetAngleTolerance(Target))
            { Errors.Add(TEXT("数组尾段角度重建误差超限。")); return false; }
        }
        // Tail X remains byte-for-byte unchanged.
    }
    for (int32 Seed : { 17, 42, 20260917 })
        for (int32 Index = FixedLength; Index < Source.Points.Num()+8; ++Index)
        {
            const double Walk = FRecoilRuntimeState::ComputePatternHorizontal(Profile, Index, Seed);
            const double C = Profile.GetVerticalKickCurveScale(Index);
            const double BeforeY = Profile.GetPatternPoint(Index).Y;
            const double AfterY = Candidate.IsEmpty() ? 1.0 : Candidate[FMath::Min(Index, Candidate.Num()-1)].Y;
            const double BeforeV=OldVertical*BeforeY*C, AfterV=NewVertical*AfterY*C;
            if (!IsFiniteValue(Walk) || !IsFiniteValue(BeforeV) || !IsFiniteValue(AfterV))
            { Errors.Add(TEXT("尾段前后比较出现非有限值。")); return false; }
            OutAdjustment.Differences.Add(FString::Printf(TEXT("种子%d，第%d发%s：ΔYaw %.6g -> %.6g°；ΔPitch %.6g -> %.6g°"),
                Seed, Index+1, Index >= Source.Points.Num() ? TEXT("（数组外）") : TEXT(""),
                OldHorizontal*Walk, NewHorizontal*Walk, BeforeV, AfterV));
        }

'''
    return text[:pos] + tail + text[pos:]
edit('Source/LyraEditor/Private/Recoil/LyraRecoilPatternAdapter.cpp', repair_adapter)

def repair_shadow(text):
    start = text.index('\t\tTArray<FVector2D> RoundTargets;')
    end = text.index('\n\t}', start)
    return text[:start] + text[start:end].replace('Errors', 'RoundErrors') + text[end:]
edit('Source/LyraEditor/Tests/LyraRecoilEditorMappingTest.cpp', repair_shadow)
edit('Source/LyraEditor/Private/Recoil/LyraRecoilProfileEditor.cpp', lambda t: t.replace('"Styling/SlateIcon.h"', '"Textures/SlateIcon.h"'))
