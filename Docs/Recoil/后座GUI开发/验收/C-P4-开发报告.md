# C-P4 开发报告：后坐力数值预览（开发计划 §10 / T13 / T14）

- 任务来源：`Docs/Recoil/后座GUI开发/开发计划纯文本.txt` §10、§11、§13（T13/T14）、§12 P4
- 参考实现口径：`Docs/Recoil/14_RecoveryToBurstStart.md`（2026-10-01）+ `Source/LyraGame/Weapons/Recoil/LyraRecoilState.cpp`
- 写入范围（独占）：
  - `Source/LyraEditor/Private/Recoil/LyraRecoilPreviewController.h`
  - `Source/LyraEditor/Private/Recoil/LyraRecoilPreviewController.cpp`
  - `Source/LyraEditor/Private/Recoil/SLyraRecoilPreview.h`
  - `Source/LyraEditor/Private/Recoil/SLyraRecoilPreview.cpp`
  - `Source/LyraEditor/Tests/LyraRecoilEditorPreviewTest.cpp`
  - `Docs/Recoil/后座GUI开发/验收/C-P4-开发报告.md`（本文件）
- 未修改：`LyraGame`、正式资产、Golden、开发计划、其他 agent 文件、`LyraEditor.Build.cs`

---

## 0. 本机验证状态（必须如实阅读）

**本轮没有完成编译与自动化测试执行。** 原因是环境问题，不是代码问题：

- 本次会话的 shell 工具在启动时即失败：
  `Error: SetNamedSecurityInfoW failed (Win32 5): grantWrite(D:\TPSGunsDemo\TPSGunsDemo)`。
  即 DSH 无法为工作区 `D:\TPSGunsDemo\TPSGunsDemo` 授予写入权限，任何 `pwsh` 命令都执行不了。
- 尝试按 `diagnose-windows-sandbox-acl` 技能用附带脚本诊断/修复该目录的文件权限时，
  需要更宽权限批准，而本会话**没有可用的审批通道**：
  `sandbox escalation to "danger-full-access" requires approval, but no approval channel is available`。

因此：

- **未运行** `build.ps1`、未运行 `UnrealEditor-Cmd.exe -ExecCmds=Automation RunTests`、
  未生成任何自动化报告；本报告不声称编译通过、不声称测试通过。
- 所有代码都是按源码逐个 API 核对的（对照 `D:\UE_5.8` 头文件与
  `Source/LyraGame` 现有实现），但**仍需主 agent 在可构建的环境里编译并跑测试**。
- 未验证项清单见 §7。

---

## 1. 交付物与接口

### 1.1 `FLyraRecoilPreviewController`（纯 C++，非 UObject）

`Source/LyraEditor/Private/Recoil/LyraRecoilPreviewController.h/.cpp`

对外 public API（便于主 agent 集成与测试直接调用）：

| 成员 | 作用 |
| --- | --- |
| `bool RefreshProfile(ULyraRecoilProfile*, FString& OutError, TArray<FName>& OutInlinedCurveNames)` | 重建隔离快照（含全部曲线深内联）并使缓存失效 |
| `static FLyraRecoilPreviewSnapshot BuildIsolatedSnapshot(ULyraRecoilProfile*)` | 可单独调用的快照工厂（测试用） |
| `void SetConfig(const FLyraRecoilPreviewConfig&)` | 写入全部预览输入并使缓存失效 |
| `bool Run()` | 从 `t=0` 完整跑一遍（内部仍是可中断的原子步） |
| `bool AdvanceBudget(double BudgetMilliseconds, bool& bOutCompleted)` | 编辑器主线程有限预算增量执行 |
| `bool RunLongFrameStep()` | 把 `LongFrameOverrideSeconds` 作为**单个** Delta 交给现有 `Advance` |
| `const TArray<FLyraRecoilPreviewSample>& GetSamples()` | 时间采样表（全部输出通道） |
| `const TArray<FLyraRecoilPreviewShot>& GetShots()` | 逐发结果（方向偏移 / 应用增量 / 姿态 / 起枪偏移 / 可见角） |
| `bool IsResultCacheValid()` / `uint32 ComputeRequestToken()` | 缓存失效校验 |
| `void SeekToTime(float)` / `void AdvancePlayhead(float,float)` | 播放位置（墙钟，不影响模拟时步） |

`AddReferencedObjects()` + `TStrongObjectPtr<ULyraRecoilProfile> SnapshotStrongRef` 双重保护快照 UObject。

### 1.2 `SLyraRecoilPreview`（Slate）

`Source/LyraEditor/Private/Recoil/SLyraRecoilPreview.h/.cpp`

- `SLATE_ARGUMENT(ULyraRecoilProfile*, Profile)`：按接口要求接收 `.Profile(...)`。
- `RefreshProfile()`：主 agent 接入资产外部通知后可直接调用（当前先由 Controller 自身刷新）。
- `SetProfile()` / `GetController()` / `SetPlaying()` / `ResetReplay()` / `Cleanup()`。
- 布局：播放控制条（播放/暂停、重置回放、刷新快照、播放倍率）+ Slate 时间曲线（自绘多段线）
  + 发射事件/诊断表 + 参数编辑区。
- `OnPaint` 用 `FSlateDrawElement::MakeLines` 画 4 条曲线：理论累计 Kick、CameraOffset Pitch、
  CameraOffset Yaw、可见 Pitch，以及零线、网格与播放头。
- 播放由 `RegisterActiveTimer` 驱动，`Cleanup()`/析构调用 `UnRegisterActiveTimer`（计划 §9 预览清理）。

---

## 2. 逐项功能对照（计划 §10.1 输出通道 / §10.2 时序合同 / §10.3 必备场景）

### 2.1 隔离瞬态 Profile 快照

| 要求 | 实现 |
| --- | --- |
| 隔离临时副本 | `StaticDuplicateObject(Source, GetTransientPackage(), NAME_None, RF_Transient\|RF_Transactional)` |
| 全部 `FRuntimeFloatCurve` 有效 RichCurve 深复制成内联 | `TFieldIterator<FStructProperty>` 枚举 `FRuntimeFloatCurve` 属性，`CopyRichCurve()` 整条复制 `Keys`(键/切线/切线权重/插值模式/切线模式) + `PreInfinityExtrap` + `PostInfinityExtrap` + `DefaultValue`，随后 `ExternalCurve = nullptr` |
| 固定种子模式在副本 | `ApplyPreviewOptionsToSnapshot()`：写副本的 `RandomSeedMode` + `FixedRandomSeed`（新轮次 `ResolveSeed` 也稳定） |
| 单发模式覆盖 | 同上写副本 `SingleShotMode` |
| GC 通过 FGCObject/TStrongObjectPtr | 控制器继承 `FGCObject`，`AddReferencedObjects` 登记 `Snapshot.Profile` 与 `StrongRef` |
| 不写原 Profile | 上述全部写入都在副本上；`RefreshProfile` 不调用 `Modify()`，不触碰原对象字段 |
| 不写 CVar | 全局倍率走 `FLyraRecoilPreviewConfig::GlobalScale` → `SetGlobalScale()`，从不调用 `ULyraRecoilDebug` 或 CVar |
| 不启动 PIE | 无 `UWorld` 依赖，只用 Transient 对象与纯数值结构体 |
| 使用现有 `FRecoilRuntimeState` 全部算法 | 见 2.3，无一行复制的后坐力数学 |

快照诊断（`FLyraRecoilPreviewSnapshotInfo`）：曲线属性总数、被内联的属性名、
未内联的外部引用属性名（原外部曲线无有效键时会保留原引用并如实报出）。

### 2.2 输入项

| 要求 | 配置字段 / 面板控件 |
| --- | --- |
| RPM | `Config.RPM` / “射速 RPM” |
| 发数 | `Config.ShotCount` / “发数” |
| 步长 | `Config.FrameSeconds` / “步长 Hz”（30/60/120 直接填） |
| 尾时长 | `Config.TailSeconds` / “尾时长 s” |
| 姿态 | `InputScript[].PoseState`（站立/蹲伏/空中按钮，全程生效） |
| 瞄准 Alpha | `InputScript[].AimingAlpha`（面板 “瞄准 Alpha”） |
| 全局倍率 | `Config.GlobalScale` + `InputScript[].bOverrideGlobalScale/GlobalScale` |
| 起枪角 | `Config.BurstStartAnglePitch/Yaw` |
| 带时间戳玩家输入脚本 | `Config.InputScript`（TimeSeconds/瞄准角/姿态/Alpha/倍率）；面板按“压枪量 + 起始时刻 + 保持时长”参数生成，Controller 侧接受任意脚本数组 |
| 两轮发射时刻 | `Config.FireInputs`（StartTimeSeconds/ShotCount/RPM 列表）+ “单轮/两轮”“每轮发数/轮间隔 s” |

> 说明：需求里的“带时间戳玩家输入脚本”在**数据层**是完整的 `TArray<FLyraRecoilPreviewInputEntry>`
> （每条含时间戳/瞄准角/姿态/Alpha/倍率），测试直接构造任意脚本；
> 面板为了可用性把常用形态（起枪角 → 压枪 → 松手）做成了参数化生成（`RebuildInputScript()`），
> 而不是让用户手写 JSON。需要任意脚本时由代码/主工具包直接填 `Config.InputScript` 即可。

### 2.3 真实发射调用顺序（对照 `LyraRangedWeaponInstance.cpp` + `LyraGameplayAbility_RangedWeapon.cpp`）

每次 `Advance` 之前（对应 `UpdateRecoil` 每帧）：

1. `EvaluateInputAtTime(SegmentStart, …)` 取脚本状态
2. `RecoilState.SetGlobalScale()`
3. `RecoilState.SetPoseMultiplier(FRecoilRuntimeState::ComputePoseMultiplier(...))`
4. `RecoilState.SetSpreadPlayerMultipliers(Profile.GetSpreadAimingMultiplier(α), 移动倍率)`
5. `RecoilState.SamplePlayerAim(pitch, yaw)` ← **Advance 前采样**
6. `RecoilState.Advance(Profile, Delta)`（长帧大 Delta 原样传入）
7. `RecoilState.AdvanceSpread(Profile, Delta, Pose)`（在 Advance 之后，与武器一致）

每个发射事件（对应 GA 的 `OnTargetDataReadyCallback`）：

1. 再次 `EvaluateInputAtTime` + `SetGlobalScale` + `SetPoseMultiplier` + **`SamplePlayerAim`**（开火前重采，与 `AddRecoil` 一致）
2. 取 `ShotIndexAtFire = RecoilState.ShotIndex`（**预览不自行递增**）
3. `ComputeShotKickGated(Profile, ShotIndexAtFire, CurrentPoseMultiplier, GlobalScale, ActiveSeed, true)` → 逐发方向偏移（弹道链）
4. `RecoilState.SetPendingShotSpreadAngle(GetEffectiveSpreadAngle())`（对应 `NotifyShotSpreadUsed`，在发射前）
5. 理论累计 Kick：`ComputeShotKick(Profile, ShotIndex, 1, 1, ActiveSeed)` 累加（倍率 1）
6. `RecoilState.ApplySpreadShot(Profile, Pose)`（对应 `AddSpread`，在 `AddRecoil` 之前）
7. `RecoilState.ApplyShot(Profile, PoseMultiplier, Pose)`（对应 `AddRecoil`）
8. `++NextFireIndex`、记录逐发结果、发射即刻采样

**新一轮 ShotIndex 种子时机与武器一致**：`ApplyShot` 内部在 `State == Idle || State == Recovering`
时判定新一轮 → `ShotIndex = 0`、`ShotHistory.Reset()`、`ActiveSeed = ResolveSeed(Profile)`、
锁定 `AimPitchAtBurstStart = SampledAimPitch`、清空压枪/覆盖账本。预览不做第二套判断，
只用 `bStartedNewBurst` 做展示标记。

### 2.4 事件边界与时间步

- 外层步长 = `Config.FrameSeconds`（30/60/120Hz 直接给 1/30、1/60、1/120）。
- 帧内若存在发射时刻，则在**发射时刻**把该帧拆开：先 `Advance(t_fire - t_prev)`，
  再发射，再 `Advance(t_frameEnd - t_fire)`。
- 因此射速不受编辑器帧率影响；同一时间步下的事件序列与逐发结果与直接调用完全一致（见 §5 测试）。
- `Advance` 内部原样保留现有固定子步与长帧保护，预览不重写阶段算法。
- **长帧场景**由 `RunLongFrameStep()` 实现：把 `LongFrameOverrideSeconds`（如 0.5s）作为
  **单个** `DeltaSeconds` 传给 `FRecoilRuntimeState::Advance`，不切碎、不伪覆盖。

### 2.5 输出样本（分开三路，不是弹着点）

`FLyraRecoilPreviewSample` 逐点包含：

| 通道 | 字段 |
| --- | --- |
| 理论累计 Kick（倍率 1，未经相机上限/回弹/回正） | `TheoreticalKickPitch/Yaw` |
| 相机偏移 | `CameraOffsetPitch/Yaw/Roll`（另含未补间的 `AccumulatedPitch/Yaw`） |
| ControlRotation | `ControlRotationPitch/Yaw`（由输入脚本维护，不被预览反写） |
| 可见角度 | `VisibleAnglePitch/Yaw = ControlRotation + CameraOffset` |
| 状态诊断 | `State`、`InterpStage`、`ShotIndex`、`RecoveryPeakPitch/Yaw`、`RecoveryCoverPitch/Yaw`、`RecoveryProgress`、`LastSubStepCount`、`bAfterFireEvent` |

`FLyraRecoilPreviewShot` 逐发包含：`DirectionOffsetPitch/Yaw`（`ComputeShotKickGated` 输出，
发射前采集）、`AppliedKickPitch/Yaw`（`FRecoilShotResult`）、`TheoreticalKickPitch/Yaw`、
`BurstStartPitchOffset/YawOffset`、开火瞬间 `ControlRotation` 与 `VisibleAngle`、
`PoseState/PoseMultiplier/GlobalScale/SpreadAngle`、`bStartedNewBurst`。

**明确不是弹着点**：没有做散布采样、没有碰撞/弹道积分；`SpreadAngle` 只是数值通道。
时间图上也没有“散点图”。

### 2.6 数值检查与限额

- 输入侧：`HasAnyData`/曲线为空时走现有 `GetVerticalKickCurveScale` 的退化 1.0 逻辑；
  负曲线/零分母等行为完全由现有 `ComputeShotKick` 决定（预览不另立规则）。
- `BuildIsolatedSnapshot(nullptr)` → 明确返回错误字符串，不崩溃。
- 快照构建后调用现有 `ULyraRecoilProfile::ValidateProfile`，不合格项进 `Warnings`（不阻断回放，
  便于在面板上看清资产本身的问题；GUI 的保存校验仍由主工具包负责）。
- 限额：`MaxShots = 4096`、`MaxSimulationSeconds = 120`，超限会把发射表裁到上限 /
  时间轴截到 120s，并置 `bTruncatedByLimit()` + `GetTruncationReason()`（面板显示原因）。
- 输出有限性：测试断言最终输出的 `IsFinite`（非有限数不进入 Slate 绘制）。

### 2.7 缓存失效与预算增量执行

- 任何改变（配置、输入脚本、发射表、快照刷新、取样模式）都调用 `InvalidateCache()`：
  清空样本/逐发结果、复位模拟状态、`CachedRequestToken = 0`。
- 缓存有效性 = `!bResultDirty && CachedRequestToken == ComputeRequestToken()`；
  令牌由快照指纹（路径 + 关键字段 + 曲线键数与采样值）与配置指纹（含脚本/发射表逐项）组成。
- 预算增量：`AdvanceBudget(ms, bOutCompleted)` 用 `FPlatformTime::Seconds()` 计时，
  但**只在完整帧边界收手**（`bFrameInProgress`）。这样预算大小不改变事件序列与采样序列，
  `AdvanceBudget` 的结果与不限预算的 `Run()` 逐样本一致。
  代价：单个极长帧（例如 1 秒的大 Delta）无法被预算切开，会一次性执行完 —— 这是为了
  满足“预算不得改变模拟结果”而做出的取舍。
- 编辑器主线程：`SLyraRecoilPreview::EnsureResults()` 每帧最多给 4ms。
- 播放墙钟不改变模拟时步：`AdvancePlayhead(dt, speed)` 只改 `PlayheadTimeSeconds`；
  模拟时间轴由 `Config.FrameSeconds` 与事件边界决定。

### 2.8 seek / 重放

`SeekToTime(t)` 只移动播放位置并做 Clamp；向后跳时丢弃结果并 `ResetSimulationState()`，
下一次 `Run()`/`AdvanceBudget()` 从 `t=0` 用**同一快照**完整重放。
**不插值旧样本、不搬运状态**；`GetVisibleAngleAtTime()` 的线性插值只在注释里明确标注为显示用途。

### 2.9 保护当前回正行为（T14）

预览调用的就是现有实现，因此最新的“回正到本轮起枪角、保留超压角度”语义原样生效：

```text
K = max(RecoveryPeak - BurstStart, 0)
终点 = BurstStart + clamp(冻结的压枪量, 0, K)
```

测试 §5.4 覆盖 0°/30° 起枪角 × 0/4/10/11° 压枪 × 两种单发模式 = 16 组，
断言可见终点等于 `A − max(P − K, 0)` 且等于文档表给出的 0/0/0/−1 与 30/30/30/29，
并额外覆盖 `bCompensationAwareRecovery = false`（回满到起枪角）。

---

## 3. 文件清单

| 文件 | 说明 |
| --- | --- |
| `Source/LyraEditor/Private/Recoil/LyraRecoilPreviewController.h` | 快照/配置/采样/逐发结构 + 控制器 public API |
| `Source/LyraEditor/Private/Recoil/LyraRecoilPreviewController.cpp` | 快照构建与曲线内联、事件表、原子步、发射事件、缓存、限额 |
| `Source/LyraEditor/Private/Recoil/SLyraRecoilPreview.h` | Slate 预览面板声明（`.Profile(...)` + `RefreshProfile()`） |
| `Source/LyraEditor/Private/Recoil/SLyraRecoilPreview.cpp` | 播放控制、时间曲线自绘、事件/诊断表、全部参数控件、Timer 清理 |
| `Source/LyraEditor/Tests/LyraRecoilEditorPreviewTest.cpp` | `Lyra.Recoil.Editor.Preview.*` 自动化测试 |
| `Docs/Recoil/后座GUI开发/验收/C-P4-开发报告.md` | 本报告 |

模块依赖：LyraEditor 现有 Private/Public 依赖已包含 `Slate`/`SlateCore`/`Json` 等，
本次未新增任何模块依赖，**未修改 `LyraEditor.Build.cs`**。
用到的头文件：`Curves/CurveFloat.h`、`UObject/UnrealType.h`、`UObject/Package.h`、
`Rendering/DrawElements.h`、`Styling/CoreStyle.h`、`Widgets/Input/SButton.h`、
`Widgets/Input/SNumericEntryBox.h`、`Widgets/Layout/SBorder.h|SScrollBox.h|SSplitter.h`、
`Widgets/SBoxPanel.h`、`Widgets/Text/STextBlock.h`。

---

## 4. 上游复用与许可

本次**没有复制或改写 UE5-CrystalRecoil 的任何代码**：预览控制器的算法全部调用本项目
`FRecoilRuntimeState` / `ULyraRecoilProfile`，事件时序参照本项目
`LyraRangedWeaponInstance.cpp` 与 `LyraGameplayAbility_RangedWeapon.cpp`。
因此本阶段无需新增 MIT 文本；`Lyra.Recoil.Editor.Preview.*` 也不依赖上游 Graph/UObject。

---

## 5. 测试清单（`Lyra.Recoil.Editor.Preview.*`，全部在 `LyraRecoilEditorPreviewTest.cpp`）

> 共 **9 个测试入口**（`Lyra.Recoil.Editor.Preview.*`），其中 §5 表格第 5 行含 16 组参数化场景
> 与 1 个 Drop 中重开火场景。以下测试**代码已写、尚未运行**。

测试夹具：`MakeTestProfile(bInterpolated)` 造 transient Profile（12 点 Pattern、PatternLength=8、
固定种子 20260917、曲线带键、关闭 Roll 以保证可复现）。参考驱动 `RunReference()` 独立复刻
真实调用顺序，只调 `FRecoilRuntimeState`，作为逐样本基准。

| # | 测试名 | 输入 | 断言（输出） |
| --- | --- | --- | --- |
| 1 | `Lyra.Recoil.Editor.Preview.IsolatedSnapshot` | Profile 的 `VerticalKickCurve.ExternalCurve` 指向一个共享 `UCurveFloat`（键 (0,0.25)、(4,3)） | 副本不是原对象；`InlinedCurvePropertyNames` 含 `VerticalKickCurve`；副本 `ExternalCurve == nullptr`；副本 `GetVerticalKickCurveScale(0)==0.25`、`(2)≈1.625`；**改动共享曲线后副本仍为 0.25**（证明深复制）；原资产仍引用共享曲线；种子/模式覆盖只写副本（原资产 `RandomSeedMode`/`FixedRandomSeed`/`SingleShotMode` 不变）；`BuildIsolatedSnapshot(nullptr)` 返回错误且不崩 |
| 2 | `…Preview.MatchesDirectRuntimeCallOrder` | 3 种模式（InstantWrite / Interpolated / FromProfile）；6 发 @600RPM；脚本含 0→−1.5°(蹲伏,α0.5)@0.2s → −3°(空中,α1.0)@0.6s | 采样数一致；逐样本 `Time/TheoreticalKick/Accumulated/CameraOffset(P/Y/Roll)/Visible/State/InterpStage/ShotIndex` 全部在 1e-4 内相等；逐发 `DirectionOffset` 与直接调用 `ComputeShotKickGated` 同源；`MaxTheoretical ≥ MaxCamera`（两通道分离） |
| 3 | `…Preview.FrameRateConsistency` | 30/60/120Hz × 8 发 @600RPM × 尾 1.5s × 0.25s 起下压 4° | 每种帧率与同时间步直接调用逐样本一致；三种帧率最终可见角度/累计偏移在 1e-3 内一致（不要求中间样本位级一致） |
| 4 | `…Preview.SingleShotModes` | 两种覆盖模式 × 单发 1 发 / 连发 8 发 | 单发：只 1 发、末样本 Idle、`InterpStage==None`、可见角回 0；连发：8 发、过程出现 Accumulating 与 Recovering（插值模式另断言出现 Lift）、末样本回 0、输出有限 |
| 5 | `…Preview.CompensationVisibleEndpoints` | 0°/30° 起枪角 × 0/4/10/11° 压枪 × 两种模式（每发 1°、10 发、尾 2s）= 16 组；另 1 组关闭补偿；另插值模式下 Drop 段检查 | 可见终点 == `A − max(P−K, 0)`（K 取运行时 `RecoveryPeak − BurstStart`）且 == 文档表值 0/0/0/−1、30/30/30/29；与直接调用逐样本一致；末状态 Idle；`RecoveryCoverPitch ≥ 下压量`；插值模式必须出现过 `InterpStage == Drop` 与 `State == Recovering`；关闭补偿时回满到 30° |
| 5b | `…Preview.CompensationVisibleEndpoints`（Drop 中重开火段） | 插值模式：第一轮 10 发 @600RPM（0.9s 结束），1.05s 重开火 2 发 | 共 12 发；第 11 发 `ShotIndex==0` 且 `bStartedNewBurst`；重开火前已经进入过 `Recovering`；新一轮 `BurstStartPitchOffset` == 重开火瞬间累计偏移且非 0；重开火后重新出现 `Lift`；最终回到 `Idle` |
| 6 | `…Preview.TwoBurstsAndRefire` | 两轮各 5 发 @600RPM，第二轮 0.9s（第一轮 0.4s 结束、回正途中重开火） | 共 10 发；第 6 发 `ShotIndex==0` 且 `bStartedNewBurst`；第 2~5 发不是新一轮；第二轮 `BurstStartPitchOffset` == 重开火时刻的累计/相机偏移（≠0）；与直接调用逐样本一致 |
| 7 | `…Preview.SeedAndMultipliers` | 16 发 @600RPM（覆盖 i≥PatternLength 的随机尾段）；同种子两次、异种子一次、姿态+α+倍率组、倍率 0 组、蹲伏组 | 同种子逐发方向偏移逐位一致；异种子固定段不变、尾段变化；姿态/α/倍率不改变理论累计 Kick 但改变实际 Kick；倍率 0 时相机偏移恒 0 而理论 Kick 仍累加；蹲伏实际 Kick == 站立 × `PoseMultiplier_Crouching` |
| 8 | `…Preview.SaturationAndLongFrame` | 每发 5°、上限 6°（垂直）；水平上限 3°；插值模式 + `LongFrameOverrideSeconds=0.5` + 0.19s 起压枪 4° | 相机 Pitch ≤ `MaxVerticalKick` 且到达上限附近、理论上跳 > 上限（两通道分离）；Yaw ≤ `MaxHorizontalKick`；长帧：`RunLongFrameStep()` 生效、样本增加、时间 ≥0.5s、末状态 Idle、`InterpStage==None`、可见终点 ≤0（保留抵扣）、`RecoveryCoverPitch ≥ 4`；对照直接 `Advance(0.5f)` 语义同类 |
| 9 | `…Preview.CacheInvalidationAndBudget` | 同 Profile 上依次改 RPM / 输入脚本 / 刷新快照；不限预算 vs 0.5ms 预算；4096 发上限；seek 0.5T→0 重放；`AdvancePlayhead` | 每次更改后 `IsResultCacheValid()==false`、重跑后 true；重算结果与全新控制器逐发一致（无新旧参数混合）；预算模式需要多次调用、发数/样本数/逐样本与不限预算一致、末状态一致；5000 发被截到 4096 且 `IsTruncatedByLimit()` 与原因非空；时间轴 ≤120s；seek 重放后发数与最终相机偏移一致；`AdvancePlayhead` 改变播放位置但不改模拟结果 |

---

## 6. 关键实现决策（与计划可能的差异，均已按“更保守”处理）

1. **`FRecoilRuntimeState` 未做任何改造**，也没有在 Editor 侧另写后坐力数学；
   所有需求都通过“按真实顺序调用 + 记录输出”实现。
2. **预算只在帧边界收手**（`bFrameInProgress`）。计划只要求“有限预算增量计算”，
   额外把“预算不影响结果”做成硬约束：`AdvanceBudget` 与 `Run()` 的采样序列完全一致。
   代价是单帧可能超过预算（当前默认 1/60s 帧，单帧成本很小）。
3. **长帧是独立入口**（`RunLongFrameStep()`）而不是把帧步长调大：
   计划 §10.2 要求“显式把大 DeltaSeconds 传给现有 Advance”，因此必须能构造一个真正的长帧。
4. **`FrameBoundaryOnly` 取样模式**默认关闭：默认每个事件边界都存样本（含发射即刻样本），
   便于逐发核对；`FrameBoundaryOnly` 留给“大表逐点比对”的用法。
5. **散布键略**：`Config.bAdvanceSpread` 默认 `false`，因为测试夹具 Profile 未开启
   `bEnableProfileSpread`；面板默认也不推进散布，避免把 Lyra 原生 heat 链路混进数值预览。
   需要时打开后，预览按 `AddSpread`→`AddRecoil` 顺序调用 `ApplySpreadShot`/`AdvanceSpread`。
6. **面板默认参数**：打开面板即 `RebuildFireInputs(false)`（单轮 5 发）+ `RebuildInputScript()`，
   保证首帧就有可播放结果；播放用 ActiveTimer，只推播放头。

---

## 7. 未验证项与限制

**未验证（因无法编译/执行，见 §0）**：

1. 未编译：`LyraEditor` Win64 Development 未构建；C++ 语法/模板实例化/包含关系未由编译器确认。
2. 未运行任何自动化测试；§5 的 9 条用例全部处于“已写未跑”状态。
3. 未做 Slate 实机检查（正常 RHI 下的曲线绘制、控件布局、DPI、ActiveTimer 行为）。
4. 未验证 GC：`FGCObject` + `TStrongObjectPtr` 的引用保护未做强制 GC 实测。
5. 未验证长帧分支在**插值模式**下的真实数值（只断言了状态收敛与可见终点范围）。
6. 未接入资产外部通知：`SLyraRecoilPreview::RefreshProfile()` 已提供，但当前由 Controller
   自身刷新；主 agent 接入后需复验“外部改动 → 一次刷新”。
7. 未与真实 `ULyraRangedWeaponInstance`/PIE 对照（预览与 PIE 的一致性需 PIE 手测）。
8. `ValidateProfile` 未调用 A agent 的 `FLyraRecoilPatternAdapter::Validate`：
   该类在 `Source/LyraEditor/Private/Recoil/` 下当前**尚不存在**（该目录本次由本 agent 首次创建），
   因此无法 `#include` 其头文件。这里先用现有 `ULyraRecoilProfile::ValidateProfile`。
   等 A agent 落地后，可在 `ValidateSnapshotProfile()` 里追加一次 Adapter 校验调用即可。

**已知限制**：

1. `MaxVerticalKick` 极大（如 >100000）时 `FMath::CeilToInt(120/帧步长)` 可能溢出 —— 当前用
   `Samples.Reserve` 的 200000 上限与 4096/120s 限额兜住；未专门加 max kick 校验。
2. `AdvanceBudget` 以“整帧”为最小收手单位：极长帧（如 500ms）会单帧超预算。
   这与“预算不改变结果”的要求权衡后选择了确定性优先。
3. 面板的参数布局是固定列宽的 `SHorizontalBox`，窄窗口下需要滚动；
   未做自适应换行（计划里 DPI/窗口尺寸验收属 P5 人工项）。
4. `GetVisibleAngleAtTime()` 的线性插值仅供显示，测试与内部推进都不使用它。
5. 快照使用 `GetTransientPackage()` 作为 Outer：不落盘、不标脏；但也因此不参与
   序列化等价比较（那是 T11 保存重载测试的职责，与本阶段目标不同）。

---

## 8. 主 agent 接手建议（最短路径）

1. 在可构建环境执行（显式 D 盘参数）：

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\Docs\Recoil\Tools\build.ps1 -EngineRoot 'D:\UE_5.8' -Project 'D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject' -Target LyraEditor -Platform Win64 -Config Development -UBARootDir 'D:\TPSGunsDemo\TPSGunsDemo\Saved\UBACache'
   ```

2. 跑新增用例（建议先只跑 Editor.Preview，便于定位）：

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\Docs\Recoil\Tools\run-recoil-tests.ps1 -EngineRoot 'D:\UE_5.8' -Project 'D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject' -TestFilter 'Lyra.Recoil.Editor.Preview'
   ```

3. 读本轮 `index.json` 的 `failed/notRun/inProcess` 与用例清单，再跑完整 `Lyra.Recoil`（含 57 项基线）。
4. 若编译报错，优先怀疑点（按可能性排序）：
   - `FRuntimeFloatCurve::GetRichCurveConst()` 返回的指针在 `ExternalCurve` 已置空后的语义；
   - `TFieldIterator<FStructProperty>` 与 `FRuntimeFloatCurve::StaticStruct()` 的比较；
   - Slate 控件参数（`SNumericEntryBox` 的 `Value_Lambda` 返回 `TOptional<T>`）；
   - `Stat`/`HashCombine` 的重载（`GetTypeHash(float/bool/enum)`）。
5. 测试失败时先看 `MatchesDirectRuntimeCallOrder`：它是最强的合同测试，
   一旦失败就说明预览的调用顺序或事件拆分与运行时不一致。
