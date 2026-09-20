# 后坐力系统 · 当前进度指南

> 这份文档是给大祥老师用的**操作手册 + 状态看板**。
> 每次推进都会更新，看这一份就知道"现在到哪了、怎么复跑、要拍板什么"。
>
> 最后更新：2026-09-20（P0–P5 / P8 / P9 完成；**P10 回正扣减压枪量落地**；两把枪统一四段式单发）

---

## 0. 一句话现状

**P0–P5 已完成，自动验证全绿**。调参闭环（DebugDraw / CSV 导出 / 曲线叠加 / 热重载）已经打通。

**P7 已经动起来了**：Lyra 那把自动步枪被复制成 **两把后坐力手感不同的枪**，
共用模型/动画/开火能力，PIE 进 `L_ShooterPerf` 就在快捷栏里，可直接切换。
配方与手算数据看 → **[07_TuningRecipe.md](07_TuningRecipe.md)**。

**P8 已落地（2026-09-17 追加）**：在 Pitch/Yaw 之外新增**完全独立的 Roll 震屏通道**，
依据《FPS 相机镜头设计与实现（脱敏版）》§1/§2/§2.1/§2.2/§7。
含实时调试面板（实时值 + 生效参数 + ASCII 波形）。详见 → **[08_CameraRollShake.md](08_CameraRollShake.md)**。

**P9 已落地（2026-09-17 追加）**：单发 Pitch/Yaw 现在有**两套并存的模型**，按枪切换 ——
`InstantWrite`（原模型，默认，零回归）/ `Interpolated`（新增四段式：Lift → Rebound → Settle → Drop）。
依据参考文档 §2 与 09 号差距分析。含固定子步长 1/60 的**帧率不变性**保证。
详见 → **[10_SingleShotInterpolation.md](10_SingleShotInterpolation.md)**；操作向说明 → **[后坐力系统调试.html §10](后坐力系统调试.html#model)**。

**P10 已落地（2026-09-20 追加）**：修掉「回正把玩家压的枪还回去」这个 bug ——
回正量现在会**扣掉玩家在连发期间压的那部分角度**；压过头则停在最后一发的位置。
Pitch / Yaw 两轴同规则，含 `bCompensationAwareRecovery` 总开关。
详见 → **[11_RecoveryCompensation.md](11_RecoveryCompensation.md)**。

**同日（2026-09-20）**：两把枪的单发模型**统一为四段式 `Interpolated`**（此前槽位 1 是 `InstantWrite`）。

| 槽位 | 武器 | 后坐力资产 | 形状 | 强度 | 单发模型 |
| --- | --- | --- | --- | --- | --- |
| 0（出生默认） | `ID_Rifle` | `DA_Recoil_Rifle_S` | S 型（一个弯） | 小：12 发垂直 2.34° | **`Interpolated`** |
| 1 | `ID_Rifle_7` | `DA_Recoil_Rifle_7` | 7 字型（斜线 + 顶部横杠） | 稍大：12 发垂直 2.59°，水平峰值 1.40° | **`Interpolated`** |

同时 `Config/DefaultEngine.ini` 的 `EditorStartupMap` 已指向 `/ShooterCore/Maps/L_ShooterPerf`。

**开发阶段本身到此结束** —— 2026-09-17 决定 **不做联机**，原 P6「联机同步」已从计划中剔除。
剩下是 P7（继续调参 + 固化）、P8/P9（镜头与单发模型收尾），以及一直卡着的**手动验收**（需要你人在机器前做一次 PIE 手测）。

```
P0 ✅ 勘察与接口冻结   P1 ✅ 数据层        P2 ✅ 运行时核心+相机Kick
P3 ✅ 弹道 Pattern     P4 ✅ 恢复+姿态倍率  P5 ✅ 调试与可视化工具链
P6 联机同步 —— 已剔除（本项目不做联机）      P7 ⬜ 手感调参 + 固化（可选）
P8 ✅ 相机镜头 Roll 震屏 + 镜头模式（2026-09-17 追加）
P9 ✅ 单发插值模型 · 两套并存（2026-09-17 追加）   测试 30/30 全绿
P10 ✅ 回正扣减压枪量（2026-09-20 追加）          测试 37/37 全绿
```

---

## 1. 必须知道的坑（本机环境）

### 坑 1：编译必须加 `-NoUBA`

```powershell
# ❌ 会失败（Result: Failed (OtherCompilationError)，但代码其实是好的）
& "D:\UE_5.8\Engine\Build\BatchFiles\Build.bat" LyraEditor Win64 Development -Project="..." -WaitMutex

# ✅ 这样才对（下面的脚本已经带上了）
& "D:\UE_5.8\Engine\Build\BatchFiles\Build.bat" LyraEditor Win64 Development -Project="..." -WaitMutex -NoUBA
```

原因：系统拦截了进程的**文件删除类**系统调用（`SetFileInformationByHandle(FileDispositionInfo)`），
UBA 清理临时文件被拒 → 整轮构建被判失败。加 `-NoUBA` 走传统本地 executor 就正常（全量约 3.5 分钟，增量 5 秒 ~ 2 分钟）。

### 坑 2：`-ExecCmds` 里塞多个分号命令会挂住编辑器

```powershell
# ❌ 实测不会执行，编辑器一直挂着直到被杀（Quit 也不生效）
... -ExecCmds="Lyra.Recoil.Dump; Lyra.Recoil.ReloadProfile; Quit"

# ✅ 单命令 + 内建退出条件（自动化测试就是这么跑的）
... -ExecCmds="Automation RunTests Lyra.Recoil" -TestExit="Automation Test Queue Empty"
```

要跑多个命令就**拆成多次调用**。

### 坑 3：本会话的 Bash 工具在本机不可用

`ls` / `dirname` / `cd` 全部报 `command not found`。用 **PowerShell 工具**，或绝对路径 Python：
`C:\Users\Administrator\.workbuddy\binaries\python\versions\3.13.12\python.exe -c "..."`

### 坑 4：LyraEditor 调 LyraGame 的类型，必须显式导出

```cpp
#define UE_API LYRAGAME_API      // 放在所有 #include 与 .generated.h 之后
UCLASS(...) class UE_API UMyClass { ... };
#undef UE_API                    // 放在文件末尾
```

踩过两次：`ULyraRecoilProfile`（P1）、`FRecoilRuntimeState`（P3）。
症状是**编译通过、链接失败** `error LNK2019: unresolved external symbol`。

### 坑 5：编辑器开着的时候 `Build.bat` 编不了

```
Unable to build while Live Coding is active. Exit the editor and game,
or press Ctrl+Alt+F11 if iterating on code in the editor or game
Result: Failed (OtherCompilationError)
```

**这不是代码错误** —— UE 编辑器（带 Live Coding）正在运行时，UBT 会拒绝命令行构建。
处理：关掉编辑器再跑 `build.ps1`，**或者在编辑器里按 `Ctrl+Alt+F11` 走 Live Coding 编译**。

### 另外两条

- **USTRUCT 内不能有 `UFUNCTION`** —— UHT 直接报错。
- **`UE_LOG` / `AddError` / `AddInfo` 一律写英文** —— 控制台按 ANSI 输出，中文全乱码，日志就没法当证据。C++ 注释与文档可以中文。

---

## 2. 标准命令（直接复制就能跑）

脚本目录：`D:\TPSGunsDemo\TPSGunsDemo\Docs\Recoil\Tools\`

| 用途 | 脚本 |
| --- | --- |
| 编译 | `build.ps1` |
| 生成/重建资产（幂等） | `gen-recoil-assets.ps1`（`-Force` 才覆盖，会丢手改数值） |
| 重导 Golden 数据 | `gen-recoil-golden.ps1` |
| 跑自动化测试 | `run-recoil-tests.ps1`（`-TestFilter "Lyra.Recoil.Pose"` 可只跑一组） |
| **一条命令跑完全链路** | `run-all-checks.ps1`（编译 → 资产 → Golden → 测试） |
| 把 CSV 画成曲线 | `python plot-recoil-csv.py <csv> [<csv2> ...] -o out.html` |

```powershell
# 统一调用形式
powershell -ExecutionPolicy Bypass -File "D:\TPSGunsDemo\TPSGunsDemo\Docs\Recoil\Tools\run-all-checks.ps1"
```

> 测试结果在 `-Cmd` 模式下写在 `Saved/Logs/TPSGunsDemo.log`，
> 脚本会自动摘出 `LogAutomationController` 那几行并汇总 Succeeded / Failed。

### 游戏内控制台命令速查

| 命令 | 作用 |
| --- | --- |
| `Lyra.Recoil.Enable 0/1` | 总开关（关闭 → 弹道偏移恒为 0、相机不动） |
| `Lyra.Recoil.Scale <f>` | 全局调试倍率（不改资产），**三轴都乘** |
| `Lyra.Recoil.Debug 0/1` | 屏幕左上角数值面板 |
| `Lyra.Recoil.DebugDraw 0/1` | 世界内可视化（绿线瞄准轴 / 红线相机偏移 / 黄点 Pattern / 青条回正进度 / **品红弧 = Roll 倾角**） |
| `Lyra.Recoil.RollShake <f>` | **【P8】** Roll 实时振幅倍率。`0` = 临时关掉 Roll（做 A/B 对比），`2` = 放大。**纯显示层，不污染 CSV / Golden** |
| `Lyra.Recoil.RollDebug 0/1` | **【P8】** Roll 实时面板（实时值 + 生效参数 + ASCII 波形），与主面板独立可同开 |
| `Lyra.Recoil.Dump` | 导出本轮连发到 `Saved/RecoilDump_<时间戳>.csv`（表头含 `RollShake` 列） |
| `Lyra.Recoil.ReloadProfile` | 从磁盘强制重载资产（**编辑器里改资产不需要它**，见 §8.4） |

---

## 3. 阶段状态表

| 阶段 | 名称 | 状态 | 自动验证 | 手动验收 | 验收请求 |
| --- | --- | --- | --- | --- | --- |
| P0 | 勘察与接口冻结 | 待验收 | ✅ 编译 0 error | ⏳ | [P0](Acceptance/P0_验收请求.md) |
| P1 | 数据层 | 待验收 | ✅ 1/1 | ⏳ | [P1](Acceptance/P1_验收请求.md) |
| P2 | 运行时核心 + 相机 Kick | 待验收 | ✅ 5/5 | ⏳ | [P2](Acceptance/P2_验收请求.md) |
| P3 | 弹道 Pattern 接入 | 待验收 | ✅ 4/4 | ⏳ | [P3](Acceptance/P3_验收请求.md) |
| P4 | 恢复与姿态倍率 | 待验收 | ✅ 3/3 | ⏳ | [P4](Acceptance/P4_验收请求.md) |
| P5 | 调试与可视化工具链 | 待验收 | ✅ 6/6 | ⏳ | [P5](Acceptance/P5_验收请求.md) |
| **P8** | **相机镜头 Roll 震屏** | **待验收** | ✅ 编译 + 测试 | ⏳ | [08_CameraRollShake.md](08_CameraRollShake.md) §7 |
| **P9** | **单发插值模型（两套并存）** | **待验收** | ✅ 30/30（含 6 个 Interp） | ⏳ | [10_SingleShotInterpolation.md](10_SingleShotInterpolation.md) |
| **P10** | **回正扣减压枪量** | **待验收** | ✅ **37/37**（含 7 个 Compensation） | ⏳ | [11_RecoveryCompensation.md](11_RecoveryCompensation.md) §9 |
| ~~P6~~ | ~~联机同步~~ **已剔除** | 不做 | — | — | 2026-09-17 决定：本项目不做联机 |

**测试用例清单（37 个 = P0–P5 的 19 个 + P8 的 5 个 + P9 的 6 个 + P10 的 7 个）**

> **列契约变更（P8）**：CSV 从 6 列变 7 列（新增 `RollShake`），
> `Lyra.Recoil.Dump.HeaderSchema` 与 `Lyra.Recoil.Dump.RowCountAndValues` 已同步更新。
> 契约常量收敛在 `LyraRecoilDumpTest::ExpectedHeader` / `ExpectedColumnCount` 一处，
> 以后改 CSV 只需改那里。
>
> **P9 未动 CSV 契约**：7 列保持不变。`TimeSinceFire` / `RecoveryProgress` 语义不变，
> 插值模式下相机链改读 `CameraOffsetPitch/Yaw`，CSV 仍写 `AccumulatedPitch/Yaw`（逻辑偏移）。

| 测试名 | 阶段 |
| --- | --- |
| `Lyra.Recoil.Profile.Validation` | P1（1） |
| `Lyra.Recoil.State.{Accumulation, Clamp, RecoverySteadyState, Determinism, RefireDuringRecovery}` | P2（5） |
| `Lyra.Recoil.Pattern.{Golden, FixedRegion, RandomWalk, EnableGate}` | P3（4） |
| `Lyra.Recoil.Pose.{MultiplierRatios, RecoveryCurveShape, ResolvePriority}` | P4（3） |
| `Lyra.Recoil.Dump.{RowCountAndValues, EmptyHistory, HeaderSchema}` | P5（3） |
| `Lyra.Recoil.Console.{Registration, NullWorldSafety}` | P5（2） |
| `Lyra.Recoil.Scale.AffectsDump` | P5（1） |
| `Lyra.Recoil.RollShake.EnvelopeDecayAndZero` | **P8**（1） |
| `Lyra.Recoil.RollShake.IndependentFromPitchYaw` | **P8**（1） |
| `Lyra.Recoil.RollShake.Determinism` | **P8**（1） |
| `Lyra.Recoil.RollShake.AmplitudeRampAndCap` | **P8**（1） |
| `Lyra.Recoil.RollShake.DecayCurveOverride` | **P8**（1） |
| `Lyra.Recoil.Interp.ModeIsolation` | **P9**（1） |
| `Lyra.Recoil.Interp.LiftTiming` | **P9**（1） |
| `Lyra.Recoil.Interp.FrameRateInvariance` | **P9**（1） |
| `Lyra.Recoil.Interp.StageShape` | **P9**（1） |
| `Lyra.Recoil.Interp.LongFrameSafety` | **P9**（1） |
| `Lyra.Recoil.Interp.RefireContinuity` | **P9**（1） |
| `Lyra.Recoil.Compensation.ZeroInputMatchesBaseline` | **P10**（1） |
| `Lyra.Recoil.Compensation.RetainsPullDown` | **P10**（1） |
| `Lyra.Recoil.Compensation.OverCompensationClampsToPeak` | **P10**（1） |
| `Lyra.Recoil.Compensation.YawRetainsDrag` | **P10**（1） |
| `Lyra.Recoil.Compensation.DisabledKeepsLegacy` | **P10**（1） |
| `Lyra.Recoil.Compensation.FrozenAfterRecoveryStarts` | **P10**（1） |
| `Lyra.Recoil.Compensation.InterpolatedDropConsistency` | **P10**（1） |

### 关键数值基线（写死在这里，方便一眼看出有没有被改坏）

| 指标 | 当前值 |
| --- | --- |
| Rifle 站姿 10 发累计垂直位移 | 2.9661（`DA_Recoil_Rifle`，Golden 基准，未改动） |
| `DA_Recoil_Rifle_S` 12 发累计垂直 / 水平峰值 | 2.342° / +0.655°（收尾 -0.343°） |
| `DA_Recoil_Rifle_7` 12 发累计垂直 / 水平峰值 | 2.585° / +1.400°（收尾 -0.052°） |
| 姿态倍率实测比值（站/蹲/空中/瞄准） | 1.0000 / 0.8000 / 1.5000 / 0.7500（配置值 1.0 / 0.8 / 1.5 / 0.75，偏差 ≤ 5e-5） |
| 非线性回正半程进度（快回/线性/慢回） | 0.7667 / 0.5000 / 0.3667 |
| Golden 20 发（Rifle） | `Source/LyraGame/Tests/Data/RecoilGolden_DA_Recoil_Rifle.json`，已逐发手工验算 |
| **`DA_Recoil_Rifle_S` 的 `SingleShotMode`** | **`Interpolated`**（lift 0.045 / rebound 0.030 / ratio 0.72） |
| **`DA_Recoil_Rifle_7` 的 `SingleShotMode`** | **`InstantWrite`**（默认值，参数不激活） |
| **P9 帧率不变性（60 vs 144fps）** | 相机链 `max trajectory deviation = 0.000000`（严格为零）<br>逻辑偏移 `max accumulated deviation = 0.007500`（容差 1e-2） |
| **P9 子步常量** | `FixedSubStepSeconds = 1/60`、`MaxSubStepsPerAdvance = 8` |
| **P10 回正公式** | `终止值 = clamp(峰值 × RecoilReturnRatio + 压枪量, min(峰值, 峰值×Ratio), max(峰值, 峰值×Ratio))` |
| **P10 压枪量口径** | 以「本轮连发第一发的 ControlRotation」为基准，取差值的**负值**（往下压 / 往左拉为正） |
| **P10 冻结时机** | `InstantWrite` = Accumulating→Recovering；`Interpolated` = Settle→Drop |
| **P10 零输入回归** | 压枪量恒为 0 时逐位等于旧公式 —— 既有 30 个用例、3 份 Golden **一个都没改** |
| **两把枪的 `SingleShotMode`（2026-09-20 起）** | `DA_Recoil_Rifle_S` 与 `DA_Recoil_Rifle_7` **都是 `Interpolated`** |

---

## 4. 代码地图

```
Source/LyraGame/
├── Weapons/
│   ├── Recoil/
│   │   ├── LyraRecoilTypes.h          【P0】枚举与结构体契约（已冻结）
│   │   ├── LyraRecoilProfile.h/.cpp   【P1】【P8】【P10】手感配置资产（唯一数值来源；含 Recoil|RollShake 与 bCompensationAwareRecovery）
│   │   ├── LyraRecoilState.h/.cpp     【P2/P4】【P8】【P10】FRecoilRuntimeState —— 纯算法层，无 UWorld 依赖（含压枪量测量与回正扣减）
│   │   └── LyraRecoilDebug.h/.cpp     【P2/P5】【P8】【P10】CVar + 屏幕面板（含压枪量一行）+ Roll 波形面板 + 世界 DebugDraw + CSV 导出
│   ├── LyraRangedWeaponInstance.h/.cpp 【改】持有 RecoilProfile + RecoilState；Tick 驱动；相机修改器挂载（三轴）；【P10】SampleRecoilPlayerAim()
│   └── LyraGameplayAbility_RangedWeapon.cpp 【改】P2 加 1 行 AddRecoil()；P3 注入弹道偏移
├── Camera/
│   ├── LyraCameraModifier_WeaponRecoil.h/.cpp  【P2】【P8】只改显示层 POV（Pitch/Yaw 带 NormalizeAxis，Roll 不带）
│   ├── LyraCameraShakeTypes.h         【P8】★新建 Roll 参数/状态契约 + 镜头模式枚举
│   └── LyraCameraRollShake.h/.cpp     【P8】★新建 Roll 纯算法层（无 UWorld 依赖）
└── Tests/
    ├── LyraRecoilTest.spec.cpp         【P2】State 组
    ├── LyraRecoilPatternTest.spec.cpp  【P3】Pattern 组
    ├── LyraRecoilPoseTest.spec.cpp     【P4】Pose 组
    ├── LyraRecoilDumpTest.spec.cpp     【P5】Dump 组
    ├── LyraRecoilConsoleTest.spec.cpp  【P5】Console 组 + Scale 通路
    ├── LyraRecoilRollShakeTest.spec.cpp【P8】★新建 RollShake 组（5 个用例）
    └── Data/RecoilGolden_*.json        【P3】Golden 基准（3 份）

Source/LyraEditor/
├── Commandlets/
│   ├── LyraRecoilAssetGenCommandlet.*    【P1】-run=LyraRecoilAssetGen
│   └── LyraRecoilGoldenDumpCommandlet.*  【P3】-run=LyraRecoilGoldenDump
└── Tests/LyraRecoilProfileValidationTest.cpp 【P1】

Content/Weapons/Recoil/
├── DA_Recoil_{Rifle,Pistol,Shotgun}.uasset  【P1】原有三份（**未改动**，Rifle 是 Golden 基准）
├── DA_Recoil_Rifle_S.uasset                 【P7】S 型 · 小      ← 场上槽位 0
└── DA_Recoil_Rifle_7.uasset                 【P7】7 字型 · 稍大  ← 场上槽位 1

Plugins/GameFeatures/ShooterCore/Content/
├── Weapons/Rifle/ID_Rifle_7 / WID_Rifle_7 / B_WeaponInstance_Rifle_7  【P7】第二把枪的资产链（复制品）
├── Weapons/Rifle/B_WeaponInstance_Rifle                               【改】RecoilProfile 已接线
└── Game/B_Hero_ShooterMannequin                                       【改】InitialInventoryItems = 两把步枪

Config/DefaultEngine.ini                     【改】EditorStartupMap = /ShooterCore/Maps/L_ShooterPerf

Docs/Recoil/
├── 00_Recon.md                    【P0】勘察报告（接入点带文件:行号）
├── 04_PoseMatrix.md               【P4】姿态倍率验收数值表（实测值）
├── 07_TuningRecipe.md             【P7】两把步枪的后坐力配方 + 手算弹道表 + 怎么改
├── 08_CameraRollShake.md          【P8】Roll 震屏策划案 + 实现方案 + 实时调试说明
├── 09_SingleShotCurveGap.md       【分析】现状单发模型 vs 参考文档 §2 四段式的逐条差异（未改代码）
├── 10_SingleShotInterpolation.md  【P9】单发插值模型实现方案（InstantWrite / Interpolated 共存）
├── 11_RecoveryCompensation.md     【P10】回正扣减压枪量 —— 规则 / 采样口径 / 冻结时机 / 验收
├── 后坐力系统调试.html             【P7】给人看的操作手册：测试步骤 / 改后坐力 / 新增枪 / Debug 开关效果
├── PROGRESS.md                    ← 你正在看的这份
├── Acceptance/P{0..5}_验收请求.md
├── Charts/*.html                  【P5】CSV 曲线报告（浏览器直接打开）
└── Tools/*.ps1, plot-recoil-csv.py
```

### 关键实现事实（P0 勘察得出）

| 事实 | 位置 |
| --- | --- |
| `ULyraRangedWeaponInstance::Tick()` 由 `ULyraWeaponStateComponent::TickComponent()` 每帧驱动 | `LyraWeaponStateComponent.cpp` **L38** |
| 弹道方向基准 | `LyraGameplayAbility_RangedWeapon.cpp` **L365**（AimDir）、**L397**（扩散采样）、**L534-536**（每发递增） |
| 姿态判定现成来源 | `LyraRangedWeaponInstance.cpp` **L175–L208** |
| 状态重置点 | `LyraRangedWeaponInstance.cpp` **L48–L66** / **L68–L71** |
| CameraModifier 公共 API（本引擎版本） | `AddNewCameraModifier` / `FindCameraModifierByClass` / `RemoveCameraModifier`（**没有** `AddCameraModifier`） |
| 武器实例 Blueprint | `Plugins/GameFeatures/ShooterCore/Content/Weapons/B_WeaponInstance_*` |

---

## 5. 主线设计决策（已落地）

| 决策 | 落地方式 | 依据 |
| --- | --- | --- |
| 相机偏移绝不碰 `ControlRotation` | `ModifyCamera()` 只改 `FMinimalViewInfo`。全局搜 `AddPitchInput`/`AddYawInput` 零命中 | 计划 §3.3 决策 1 / 硬性规则 5 |
| 算法层无 UWorld 依赖 | 所有输入靠参数传入；工具 / IO / 世界查询全部落在 `ULyraRecoilDebug` | 硬性规则 4 |
| 全部手感参数来自资产 | 武器实例持有 `RecoilProfile`；源码里搜不到影响手感的字面量 | 硬性规则 6 |
| 弹道偏移叠加在扩散**之前** | `TraceBulletsInCartridge()` 先算 `AimDirWithRecoil` 再走扩散；整发（cartridge）共用同一偏移 | 计划 §P3 |
| 随机种子可控且与调用顺序无关 | 逐发哈希采样 `HashCombine(Seed, ShotIndex)` | 计划 §3.3 决策 4/5 |
| Pattern 只描述形状，趋势交给曲线 | X∈[-1,1]、Y∈[0,1] 恒定归一化；"越打越高"由 `VerticalKickCurve` 承担 | P1 §5.1 |
| 姿态/瞄准换算收在纯静态函数里 | `ResolvePoseState()` / `ComputePoseMultiplier()` | 计划 §P4 |
| CSV 列清单是冻结契约 | 由 `Lyra.Recoil.Dump.HeaderSchema` 逐列断言 | 计划 §P5 |
| 命令名 + 类型是冻结契约 | 由 `Lyra.Recoil.Console.Registration` 断言 | P5 |

---

## 6. 需要你拍板的清单（汇总）

| # | 事项 | 位置 |
| --- | --- | --- |
| 1 | 编译命令统一加 `-NoUBA`，是否写回计划正文 | P0 §5.1 |
| 2 | `FRecoilShotResult` 比计划多两个字段（`PoseState` / `PoseMultiplier`） | P0 §5.2 |
| 3 | 瞄准不进 `EPoseState`，改用混合权重相乘 | P0 §5.3 |
| 4 | `.uasset` 用 Commandlet 生成 | P0 §5.4 |
| 5 | Pattern 的"形状 vs 趋势"职责划分 | P1 §5.1 |
| 6 | 三份资产初始数值是占位值（P7 再调） | P1 §5.2 |
| 7 | **武器实例接线（`DA_Recoil_*` → `B_WeaponInstance_*`）由谁做** ← **唯一阻塞手动验收的** | P1 §5.3 / P2 §3 步骤 0 |
| 8 | `AccumulatedPitch` 的双重语义（既是累加量、又是回正量） | P2 §5.1 |
| 9 | `RecoilReturnRatio = 0` 时"准星 ≠ 弹着点"是刻意设计 | P2 §5.2 |
| 10 | 姿态倍率瞬时切换、不插值 | P2 §5.3 / P4 §5.2 |
| 11 | 后坐力偏移用 Pitch/Yaw 直接相加，而非四元数 | P3 §5.1 |
| 12 | 垂直 Pattern 尾部"饱和"而非"继续爬升" | P3 §5.4 |
| 13 | 是否新增 `PoseMultiplier_Moving`（计划提到 Speed 但参数表没这一项） | P4 §5.1 |
| 14 | 空中优先级高于蹲伏 | P4 §5.3 |
| 15 | CSV 是否要加 `PoseState` / `PoseMultiplier` 两列（现在严格 6 列） | P5 §5.1 |
| 16 | `ReloadProfile` 的真实场景（编辑器内改资产**不需要**它）是否写回计划 | P5 §5.2 |
| 17 | `ReloadProfile` 会在 PIE 里触发一次 Full GC（卡顿尖峰，非崩溃） | P5 §5.3 |
| 18 | DebugDraw 的绿线是近似（用 ControlRotation，非 GA 的 CameraTowardsFocus） | P5 §5.4 |
| 19 | "回正进度指示"做成世界内竖条而非 HUD | P5 §5.5 |
| ~~7~~ | ~~武器实例接线由谁做~~ —— **2026-09-17 已完成**（两把枪的 `RecoilProfile` 都接上了） | [07_TuningRecipe.md](07_TuningRecipe.md) |

### 单发模型差异（2026-09-17 追加 → **同日已拍板并落地，见 P9**）

| # | 事项 | 结论 | 详细 |
| --- | --- | --- | --- |
| 26 | **现状单发模型不是文档 §2 的四段式** —— 是「阶跃累加 → 冻结 → 一次回正」，缺 t1 瞬时回弹、缺阶段时长、缺上抬/下降曲线 | ✅ **已落地**：新增 `Interpolated` 模式补齐四段。原模型改名 `InstantWrite` 并保持为默认，零回归 | [10_SingleShotInterpolation.md](10_SingleShotInterpolation.md) |
| 27 | 「分段」的歧义：现状按**发序号**分段（`VerticalKickCurve`），文档按**单发内时间轴**分段 | ✅ 两者并存：发序号轴曲线照旧，另加 `LiftCurve` / `ReboundCurve`（段内时间轴） | [09](09_SingleShotCurveGap.md) §3.2 / [10](10_SingleShotInterpolation.md) §1 |
| 28 | ~~若落地四段式，代价是 3 份 Golden + 多个测试基线要重建~~ | ✅ **代价已规避**：走路 1（开关共存）。实际 Golden 与 19 个老测试**一个都没改** | [10](10_SingleShotInterpolation.md) §2 |
| 29 | 两把默认枪各用哪套模型 | ✅ **槽位 0 `Rifle_S` = `Interpolated`；槽位 1 `Rifle_7` = `InstantWrite`**（大祥老师拍板） | [10](10_SingleShotInterpolation.md) §6 |
| 30 | 帧率不稳 / 低帧率怎么保证轨迹一致 | ✅ 固定子步长 `1/60` + 尾段吸附 + **刻意不做尾料冲刷**；实测相机链偏差严格 0.000000 | [10](10_SingleShotInterpolation.md) §4 |
| 31 | `Settle` 段时长是否新开参数 | ✅ **不开**，复用 `RecoveryDelay`（大祥老师：`把稳定当成 recoverydelay 然后把这个参数干掉`） | [10](10_SingleShotInterpolation.md) §1.2 |
| 32 | 默认插值曲线取什么形状 | ✅ `LiftCurve` 取 **Ease-Out**（快起慢收，附 5 条理由），`ReboundCurve` 线性 | [10](10_SingleShotInterpolation.md) §6.2 |

### P10 新增待拍板（2026-09-20）

| # | 事项 | 我的默认选择 | 影响面 |
| --- | --- | --- | --- |
| 33 | **yaw 轴是否也扣压枪量** —— 你拍板「两轴同规则」 | 已按「一律扣」实现 | 副作用：连发中主动拉枪追目标，回正会把视角拽回开火前的位置。若不想要，把 `bCompensationAwareRecovery` 关掉即可回到旧行为 |
| 34 | **多轮连发时压枪量会一轮一轮垒进残留偏移**（顶到 `MaxVerticalKick` 后停住） | 先按现状，等你实测 | 见 [11_RecoveryCompensation.md](11_RecoveryCompensation.md) §8：三个可选缓解方向（调小 Ratio / 加零位缓慢衰减 / 关开关） |
| 35 | **压枪量的采样只认本地玩家** | 远程玩家跳过（压枪量恒 0） | 本项目不做联机，无实际影响 |
| 36 | **两把枪统一四段式后，是否要分开调时间轴** | 目前两把共用 `0.045 / 0.030 / 0.72`（射速相同，见 §8.10） | 想区分「重枪更沉」就各改各的 `LiftDuration` |

### P7 新增待拍板（两把步枪落地带来的）

| # | 事项 | 我的默认选择 | 影响面 |
| --- | --- | --- | --- |
| 20 | **"7 字型"的方向** —— 现在是「斜向右上 → 顶部向左横」；若要「竖上 → 顶部向右横」，把 Pattern 的 X 全取反 | 按数字 7 的正常笔顺 | 手感差异明显，1 分钟能改 |
| 21 | **20 只 bot 的初始武器跟着一起变了** | 没做区分 | `B_Hero_ShooterMannequin` 是玩家与 bot 共用的英雄蓝图，`InitialInventoryItems` 一改两边都变：原来手枪，现在每只 2 把步枪。要"只有玩家换"得改代码或给 bot 换蓝图 |
| 22 | 出生时第二把要不要保留手枪 | 已按需求做成「就两把步枪」 | 快捷栏第 3 格空着 |
| 23 | 原版 `DA_Recoil_Rifle` 怎么处理 | 原样留着当 Golden 基准 | 场上两把枪现在都不用它 |
| 24 | `GameDefaultMap` 是否也指到 `L_ShooterPerf` | 没改 | 改了就绕过 Lyra 前端菜单流程 |
| 25 | ~~是否修 §8.6 那个 flaky 测试~~ —— **2026-09-17 已修**（落盘前顺延编号，重复路径不可达） | 已编译并复跑全绿（现 30/30） | 无需你做事 |

---

## 7. 下一步（P7：手感调参 + 固化，可选）

> **P6「联机同步」已于 2026-09-17 剔除 —— 本项目定位为单机 TPS Demo，开发阶段到 P5 为止。**
> 代码里"状态集中成纯数值结构体"的设计保留（它对纯数值可测性有实际价值），
> 但不会再做复制 / 预测 / 服务器校验。

计划 §P7 的交付物：

| 交付项 | 现状 |
| --- | --- |
| `Docs/Recoil/07_TuningRecipe.md`：各武器最终参数配方 | **已完成首版**（两把步枪 S 型 / 7 字型，含手算逐发表） |
| 全套 CSV 曲线对比图 | **工具已就绪**：`plot-recoil-csv.py` 支持多文件叠加，需要你在 PIE 里实际连发产生 CSV |
| `Docs/Recoil/FinalAcceptanceReport.md`：完整验收报告 | 未开始 |
| 三把武器（Rifle / Pistol / Shotgun）手感差异明确并定稿 | 步枪已完成（变成两把）；Pistol / Shotgun 仍是占位数值 |

**P7 现在的实际状态（2026-09-17 更新）：**

1. ~~武器实例接线~~ → **已完成**。`B_WeaponInstance_Rifle` → `DA_Recoil_Rifle_S`，
   新增的 `B_WeaponInstance_Rifle_7` → `DA_Recoil_Rifle_7`。
2. **初始装备已完成**：`L_ShooterPerf` 一进就带两把步枪，可直接切。
3. **编辑器启动关卡已指向 `L_ShooterPerf`**（重启编辑器生效）。
4. 已验证：PIE 正常起、21 个英雄各自装备成功、日志无加载/装备报错；`Lyra.Recoil` 19/19 通过。
5. **还压着的**：§6 的 P7 新增 6 条（#20~#25），其中 #20（"7"的方向）和 #21（bot 一起变了）
   会直接影响手感与性能测试口径。

---

## 8. 调参 / 手改资产时的注意事项

1. **改完资产必须重导 Golden**，否则 `Lyra.Recoil.Pattern.Golden` 会 FAIL 并提示 stale：
   ```powershell
   powershell -ExecutionPolicy Bypass -File "D:\TPSGunsDemo\TPSGunsDemo\Docs\Recoil\Tools\gen-recoil-golden.ps1"
   ```
   需要重导的字段：`FixedRandomSeed`、`RandomSeedMode`、`PatternLength`、`PatternPoints`、
   `RecoilPerShot_Vertical`、`RecoilPerShot_Horizontal`、`VerticalKickCurve`、`HorizontalRandomRange`
2. **改姿态倍率 / 单发基准后，`Docs/Recoil/04_PoseMatrix.md` 的实测表也要重跑更新。**
3. **不要跑 `gen-recoil-assets.ps1 -Force`**，除非确实要丢弃手改数值。
4. **在编辑器里改资产不需要敲 `ReloadProfile`** —— PIE 与编辑器同进程共享 UObject，改完立刻生效。
   `ReloadProfile` 只用于"资产被编辑器之外的东西改了"。
5. 出曲线图的完整流程：
   ```
   连发 → Lyra.Recoil.Dump → python Tools/plot-recoil-csv.py Saved/RecoilDump_*.csv -o 对比.html
   ```
   `plot-recoil-csv.py` 支持一次传多个 CSV 做叠加对比，纯标准库、不需要装 matplotlib。
6. **`Lyra.Recoil.Scale.AffectsDump` 曾经偶发失败 —— 2026-09-17 已修**。
   旧现象：`The two dumps are different files (..._213043_598.csv / ..._213043_598.csv)`，
   后面 17 条 half 断言连锁失败。真因：`LyraRecoilDebug` 用**毫秒时间戳**命名 CSV，
   同一毫秒内连续 Dump 两次 → 第二次覆盖第一次 → 两个路径相同；机器快的时候才撞得上。
   **修法**：`ULyraRecoilDebug::DumpShotHistoryToCsv` 落盘前若目标文件已存在就顺延编号
   （`_2` / `_3` / …），让"每次 Dump 一个独立文件"成为结构性契约 —— 重复路径现在不可达。
   改的是 `Source/LyraGame/Weapons/Recoil/LyraRecoilDebug.cpp`，需要编译（已经编过）。
   **证据**：连跑 4 轮，第 3 轮两次 Dump 落在同一毫秒，产出
   `RecoilDump_20260917_214709_285.csv` 与 `RecoilDump_20260917_214709_285_2.csv`
   （修复前第二份会直接覆盖第一份）；四轮均无 `Result={Fail}`，每轮 19 tests performed。
7. **编辑器开着也能改资产、跑测试，不用关**：
   本机 UE 编辑器接了 MCP，可以直接
   `DataAssetTools.create` / `ObjectTools.set_properties` 改资产，
   `AssetTools.duplicate` 复制武器链，
   `AutomationTestToolset.RunTestsByFilter("StartsWith:Lyra.Recoil")` 原地跑自动化测试。
   只有**改 C++ 代码**这一步仍然必须关编辑器（或 `Ctrl+Alt+F11` 走 Live Coding）。
8. **改「两把步枪」的专属注意事项见 [07_TuningRecipe.md](07_TuningRecipe.md) §5~§7。**
9. **Roll 震屏（P8）的调参入口见 [08_CameraRollShake.md](08_CameraRollShake.md) §2.5 / §4。**
   速记：
   - 资产参数在 `DA_Recoil_*` 的 `Recoil | RollShake` 分类下，共 12 个，改完 PIE 立即生效。
   - 临时试量级用 `Lyra.Recoil.RollShake <f>`（**不改资产、不污染 CSV**），满意了再写进资产。
   - 看波形用 `Lyra.Recoil.RollDebug 1`（ASCII 时间轴曲线，能直接看出衰减与回零干不干净）。
- **`Lyra.Recoil.RollShake 0` 后 Pitch/Yaw 必须完全不变** —— 这是"Roll 是独立通道"的判据，
  若变了说明两条通道被意外耦合，属回归，立刻报。
10. **P10（回正扣减压枪量）的速记**（完整说明见 [11_RecoveryCompensation.md](11_RecoveryCompensation.md)）：
    - 总闸是每把枪资产上的 `bCompensationAwareRecovery`（`Recoil → Recovery` 分类，默认开）。
      关掉它 → 退回旧公式（`终止值 = 峰值 × Ratio`），这是最快的 A/B 手段。
    - **压枪量只在"开始回正"那一刻冻结**：`RecoveryDelay` 之内继续压的枪也算数；
      回正开始之后再动鼠标**不改变落点**（这是刻意的，别当成 bug）。
    - 看数用 `Lyra.Recoil.Debug 1` —— 面板多出一行
      `PushComp / FrozenComp / AimNow`：分别是实时压枪量、冻结快照、当前 ControlRotation。
    - **压枪量为 0 时行为与改动前逐位一致**。所以"没压枪也觉得准星变飘了"，
      那一定不是这套逻辑引起的，直接报。
    - CSV **没有新增列**（7 列契约未动），压枪量只在屏幕面板上看。
11. **2026-09-20：两把枪的单发模型统一成四段式 `Interpolated`**
    （此前槽位 1 的 `DA_Recoil_Rifle_7` 是 `InstantWrite`）。
    四段式的时间轴参数为 `LiftDuration = 0.045` / `ReboundDuration = 0.030` /
    `ReboundRatio = 0.72` / `LiftCurve = Ease-Out` / `ReboundCurve = 线性`。
    **改完 `SingleShotMode` 必须重导 Golden**（Golden 的 `singleShotMode` 是元信息字段，
    不重导会让 `Lyra.Recoil.Pattern.Golden` 报 stale）：
    ```powershell
    powershell -ExecutionPolicy Bypass -File "D:\TPSGunsDemo\TPSGunsDemo\Docs\Recoil\Tools\gen-recoil-golden.ps1"
    ```
