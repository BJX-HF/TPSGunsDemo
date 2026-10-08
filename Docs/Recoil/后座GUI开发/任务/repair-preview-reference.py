from pathlib import Path
p=Path(r'D:\TPSGunsDemo\TPSGunsDemo\Source\LyraEditor\Tests\LyraRecoilEditorPreviewTest.cpp')
s=p.read_text(encoding='utf-8')
start=s.index('\tstatic void RunReference(')
end=s.index('\n\t/** 取采样序列',start)
s=s[:start]+'''	// Independent offline schedule: sort the union of explicit events and frame ends,
    // then call the production runtime at each boundary. Does not use controller stepping.
    static void RunReference(const ULyraRecoilProfile& Profile,const FLyraRecoilPreviewConfig& Config,TArray<FRefSample>& OutSamples)
    {
        OutSamples.Reset();
        TArray<float> Fires, Boundaries, Frames;
        if (Config.FireInputs.IsEmpty())
            for (int32 I=0;I<Config.ShotCount;++I) Fires.Add(I*60.f/Config.RPM);
        else for (const auto& Fire:Config.FireInputs)
            for (int32 I=0;I<Fire.ShotCount;++I) Fires.Add(Fire.StartTimeSeconds+I*60.f/Fire.RPM);
        Fires.Sort();
        const float Duration=(Fires.IsEmpty()?0:Fires.Last())+Config.TailSeconds;
        Boundaries.Add(0);
        for (float Time:Fires) Boundaries.Add(Time);
        for (const auto& Input:Config.InputScript) if (Input.TimeSeconds<=Duration) Boundaries.Add(Input.TimeSeconds);
        const int32 Count=FMath::CeilToInt(Duration/Config.FrameSeconds);
        for (int32 I=1;I<=Count;++I) { const float Time=FMath::Min(I*Config.FrameSeconds,Duration); Frames.Add(Time); Boundaries.Add(Time); }
        Boundaries.Sort();
        FRecoilRuntimeState State; State.Reset(&Profile);
        auto InputAt=[&](float Time)
        {
            FLyraRecoilPreviewInputEntry Input;
            Input.AimPitchDegrees=Config.BurstStartAnglePitch; Input.AimYawDegrees=Config.BurstStartAngleYaw;
            Input.GlobalScale=Config.GlobalScale;
            for (const auto& Entry:Config.InputScript) if (Entry.TimeSeconds<=Time+1.e-6f)
            {
                Input.AimPitchDegrees=Entry.AimPitchDegrees; Input.AimYawDegrees=Entry.AimYawDegrees;
                Input.PoseState=Entry.PoseState; Input.AimingAlpha=Entry.AimingAlpha;
                if (Entry.bOverrideGlobalScale) Input.GlobalScale=Entry.GlobalScale;
            }
            return Input;
        };
        auto SampleInput=[&](const FLyraRecoilPreviewInputEntry& Input)
        {
            State.SetGlobalScale(Input.GlobalScale);
            State.SetPoseMultiplier(FRecoilRuntimeState::ComputePoseMultiplier(Profile,Input.PoseState,Input.AimingAlpha));
            State.SetSpreadPlayerMultipliers(Profile.GetSpreadAimingMultiplier(Input.AimingAlpha),Config.SpreadMovementMultiplier);
            State.SamplePlayerAim(Input.AimPitchDegrees,Input.AimYawDegrees);
        };
        float Previous=0,TheoryPitch=0,TheoryYaw=0;
        int32 NextFire=0,NextFrame=0;
        bool bFired=false;
        SampleInput(InputAt(0));
        for (int32 Index=0;Index<Boundaries.Num();++Index)
        {
            const float Time=Boundaries[Index];
            if (Index>0 && FMath::IsNearlyEqual(Time,Boundaries[Index-1],1.e-6f)) continue;
            const auto OldInput=InputAt(Previous);
            SampleInput(OldInput);
            if (Time>Previous)
            {
                State.Advance(&Profile,Time-Previous);
                if (Config.bAdvanceSpread) State.AdvanceSpread(&Profile,Time-Previous,OldInput.PoseState);
            }
            const auto Input=InputAt(Time);
            SampleInput(Input);
            while (NextFire<Fires.Num() && Fires[NextFire]<=Time+1.e-6f)
            {
                if (Config.bAdvanceSpread) { State.SetPendingShotSpreadAngle(State.GetEffectiveSpreadAngle()); State.ApplySpreadShot(&Profile,Input.PoseState); }
                State.ApplyShot(&Profile,State.CurrentPoseMultiplier,Input.PoseState);
                const auto Kick=FRecoilRuntimeState::ComputeShotKick(Profile,State.ShotHistory.Last().ShotIndex,1,1,State.ActiveSeed);
                TheoryPitch+=Kick.Vertical; TheoryYaw+=Kick.Horizontal; ++NextFire; bFired=true;
            }
            if (NextFrame<Frames.Num() && FMath::IsNearlyEqual(Time,Frames[NextFrame],1.e-6f))
            {
                FRefSample Sample;
                Sample.Time=Time; Sample.TheoreticalKickPitch=TheoryPitch; Sample.TheoreticalKickYaw=TheoryYaw;
                Sample.AccumulatedPitch=State.AccumulatedPitch; Sample.AccumulatedYaw=State.AccumulatedYaw;
                Sample.CameraOffsetPitch=State.CameraOffsetPitch; Sample.CameraOffsetYaw=State.CameraOffsetYaw;
                Sample.CameraOffsetRoll=State.GetCameraRollOffset();
                Sample.VisiblePitch=Input.AimPitchDegrees+State.CameraOffsetPitch; Sample.VisibleYaw=Input.AimYawDegrees+State.CameraOffsetYaw;
                Sample.State=State.State; Sample.InterpStage=State.InterpStage; Sample.ShotIndex=State.ShotIndex;
                Sample.RecoveryPeakPitch=State.RecoveryPeakPitch; Sample.RecoveryCoverPitch=State.RecoveryCoverPitch;
                Sample.bFiredThisFrame=bFired; bFired=false;
                OutSamples.Add(Sample); ++NextFrame;
            }
            Previous=Time;
        }
    }
''' + s[end:]
# Explicitly compare frame samples to frame samples; UI defaults to all events.
s=s.replace('LyraRecoilPreviewTest::RunReference(*Controller.GetSnapshotProfile(), Config,', 'Controller.SetSampleMode(ELyraRecoilPreviewSampleMode::FrameBoundaryOnly);\n\t\tLyraRecoilPreviewTest::RunReference(*Controller.GetSnapshotProfile(), Config,')
# Actual task14 fixture uses a ten-degree bounded peak and incremental pulls.
s=s.replace('Profile->MaxVerticalKick = 20.0f;\n\t\t\tProfile->PatternLength = 10;', 'Profile->MaxVerticalKick = 20.0f;\n\t\t\tProfile->ReboundRatio = 1.0f;\n\t\t\tProfile->PatternLength = 10;')
s=s.replace('LyraRecoilPreviewTest::AddAim(Config, 0.02f, Case.BurstAngle - Case.DownPull, 0.0f);','for (int32 Shot=0;Shot<10;++Shot)\n\t\t\t\t\tLyraRecoilPreviewTest::AddAim(Config, Shot*.1f+.001f, Case.BurstAngle-Case.DownPull*(Shot+1)/10.f,0.0f);')
# With compensation disabled, only camera offset returns to zero; player input remains.
s=s.replace('FMath::IsNearlyEqual(Samples.Last().VisibleAnglePitch, 30.0f, 1.0e-3f)', 'FMath::IsNearlyEqual(Samples.Last().VisibleAnglePitch, 19.0f, 1.0e-3f)')
p.write_text(s,encoding='utf-8')
