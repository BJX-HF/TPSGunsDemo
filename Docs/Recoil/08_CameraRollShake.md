# 相机镜头系统 · Roll 震屏与镜头模式（策划案 + 实现方案）

| 项 | 值 |
| --- | --- |
| 项目 | `D:\TPSGunsDemo\TPSGunsDemo` |
| 依据文档 | 《FPS 相机镜头设计与实现（脱敏版）》§1 / §2 / §2.1 / §2.2 / §7 |
| 代码模块 | `LyraGame`（`Source/LyraGame/Camera/`、`Source/LyraGame/Weapons/Recoil/`） |
| 建立日期 | 2026-09-17 |
| 状态 | 已实现，待手动验收 |

---

## 0. 一句话

在原有后坐力系统的 **Pitch / Yaw 累加-回正** 通道之外，**新增一条完全独立的 Roll 震屏通道**，
并按文档 §1 把镜头效果抽象成**可并行激活的「镜头模式」**；同时提供**实时调试面板**（波形 + 参数 + 实时值）。

---

## 1. 为什么 Roll 必须独立（关键设计判断）

这是本次改动**最重要的一条认知**，写在这里是因为它决定整个架构，不是实现细节。

| | Pitch / Yaw 后坐力 | Roll 震屏 |
| --- | --- | --- |
| 数学模型 | **积分模型**：每发累加增量 | **解析模型**：按当前时刻直接求解 |
| 是否累加 | 累加（`AccumulatedPitch += Kick`） | **不累加** |
| 停火后 | 走回正曲线，**停在稳态偏移**（`RecoilReturnRatio` 决定残留） | **衰减到零**，围绕零点往复震颤后归零 |
| 长连发表现 | 弹道持续上抬，越打越偏 | 每发重新起震，幅度可能随连射频率叠加但**始终围绕零点** |
| 时钟 | 全局 `RecoveryElapsed` | **每发重置**的独立时钟 |
| 状态归属 | `AccumulatedPitch` / `AccumulatedYaw` | `RollShake.CurrentRoll` |

**结论：两者除了"都作用在相机 POV 上"以外，没有任何共享逻辑。**

曾经尝试过的错误做法（已废弃）：把 Roll 值当成第三个累加通道塞进 `AccumulatedPitch/Yaw` 旁边。
后果是 Roll 会像 Pitch 一样单向漂移、永远回不到零点，看起来就是"镜头歪了"，而不是"抖了一下"。
更严重的是它会污染回正逻辑 —— 回正曲线是给"弹道上抬"设计的，套在往复震动上毫无意义。

**因此 Roll 是独立通道，不进 `AccumulatedPitch/Yaw`，不参与回正，不共享任何状态。**

---

## 2. 策划案：Roll 震屏要什么手感

### 2.1 视觉目标

开火瞬间，画面上除了"枪口上抬"（Pitch）与"枪身左右摆"（Yaw）之外，还有一层**更细、更快、更短促**的
镜头横滚抖动 —— 就是"枪的后座力顶到肩膀、镜头跟着歪一下再弹回来"的那一下。

三条手感要求：

1. **短**：一次震动总时长约 0.2 秒量级，比回正（0.3~0.4 秒）还短。它是"开火瞬间的爆点"，不是持续状态。
2. **快**：一个震动周期约 0.05 秒量级 —— 也就是一次震动里要抖 3~4 个来回，才显得"脆"。周期太长会变成"慢慢歪一下"，像喝醉了。
3. **必回零**：震动结束时 Roll 必须精确归零。它是往复震颤，不是偏移，不该留下任何残留。

### 2.2 幅度

单发 Roll 振幅**明显小于** Pitch 振幅（量级是 Pitch 的 1/3 ~ 1/2）。理由：

- Roll 是"歪头"方向，人眼对这个方向的偏移比上下偏移**更敏感、更不适**。同样的角度，Roll 看起来"歪得更厉害"。
- 连发时 Roll 会叠加感知（每发一次），幅度大了会晕。
- 参考基线：`0.6°` 起始，随连射最多涨到 `1.5°`。

### 2.3 与连射的关系

**每发都重置时钟**，但**振幅随连射序号增长**，且分两段：

- 第 0 ~ 3 发：振幅 = 基准值（开局稳，让前几发可控 —— 这是"前几发准"的常见设计）
- 第 4 发起：每发 `+0.05°`，直到 `+0.9°` 封顶

也就是说：单点射几乎感觉不到 Roll；持续扫射时 Roll 会明显起来。这正好符合"后坐力是持续射击的代价"这个设计意图。

### 2.4 随机性

每次震动的**初始相位**带一个小随机扰动（`±0.35 rad`）。

- 为什么需要：如果每发相位完全相同，连发时震动会**完全同步叠加**，看起来像机械振动，很假。
- 为什么只是"小"扰动：扰动太大就变成"每发都不一样"，失去规律感。0.35 rad ≈ 20°，足够打破同步，又不至于乱。
- **可复现**：相位由 `(种子, 发序号)` 哈希决定，不是真随机。同一轮连发、同一个种子，震动序列逐帧一致（自动化测试依赖这一点）。

### 2.5 参数表（对应文档 §2.2）

| 参数 | 默认值 | 单位 | 说明 |
| --- | --- | --- | --- |
| `bEnableRollShake` | `true` | — | 本资产是否启用 Roll |
| `RollShake_Amplitude` | `0.6` | 度 | 起始振幅（基准） |
| `RollShake_Duration` | `0.22` | 秒 | 单次震动总时长 |
| `RollShake_Period` | `0.055` | 秒 | 震动周期（一个来回） |
| `RollShake_PhaseJitter` | `0.35` | 弧度 | 相位扰动范围 |
| `RollShake_AmplitudeCurve` | 空 | — | 衰减曲线（X 归一化时间、Y 振幅保留比例）。空 = 线性 |
| `RollShake_EndAmplitudeRatio` | `0.0` | — | 衰减终值比例。0 = 震到零 |
| `RollShake_AmplitudePerShot` | `0.05` | 度/发 | 连射增量 |
| `RollShake_RampStartShot` | `4` | 发 | 从第几发开始加增量 |
| `RollShake_MaxAmplitudeBonus` | `0.9` | 度 | 连射增量上限 |
| `RollShake_SegmentScaleCurve` | 空 | — | 分段系数（按发序号缩放振幅） |
| `RollShake_PeriodScaleCurve` | 空 | — | 分段周期系数 |

---

## 3. 实现方案

### 3.1 分层

```
[数据层] ULyraRecoilProfile
    │    Category = "Recoil|RollShake" 的完整参数组
    │    BuildRollShakeParams() ──► 装配「本次实际生效」的参数快照
    ▼
[参数契约] FCameraRollShakeParams     （Camera/LyraCameraShakeTypes.h）
    │    振幅 / 周期 / 时长 / 相位扰动 / 衰减曲线指针 / 终值比例
    ▼
[算法层] ULyraCameraRollShake          （Camera/LyraCameraRollShake.h/.cpp）
    │    全部静态纯函数，无 UObject、无 UWorld、不读 CVar
    │    EvaluateRollShake() / EvaluateEnvelope() / Trigger() / Advance()
    ▼
[状态层] FCameraRollShakeState         （Camera/LyraCameraShakeTypes.h）
    │    挂在 FRecoilRuntimeState 上：RollShake 字段
    │    输出 GetCameraRollOffset()
    ▼
[输出层] UCameraModifier_WeaponRecoil
          InOutPOV.Rotation.Roll += RollOffsetDegrees
```

### 3.2 新增 / 改动文件清单

| 文件 | 状态 | 内容 |
| --- | --- | --- |
| `Source/LyraGame/Camera/LyraCameraShakeTypes.h` | **新建** | `FCameraRollShakeParams` / `FCameraRollShakeState` / `ECameraShakeModeType` / `FCameraShakeOutput` |
| `Source/LyraGame/Camera/LyraCameraRollShake.h/.cpp` | **新建** | Roll 震动的纯算法层 |
| `Source/LyraGame/Camera/LyraCameraModifier_WeaponRecoil.h/.cpp` | 改 | 加 Roll 通道（`SetRollOffset` / `RollOffsetDegrees` / `InOutPOV.Rotation.Roll +=`） |
| `Source/LyraGame/Weapons/Recoil/LyraRecoilProfile.h/.cpp` | 改 | 加 `Recoil\|RollShake` 参数组 + `BuildRollShakeParams()` |
| `Source/LyraGame/Weapons/Recoil/LyraRecoilState.h/.cpp` | 改 | 加 `RollShake` 状态字段；`ApplyShot` 触发 / `Advance` 推进 |
| `Source/LyraGame/Weapons/LyraRangedWeaponInstance.cpp` | 改 | 相机推送加第三轴；Debug 面板挂载点 |
| `Source/LyraGame/Weapons/Recoil/LyraRecoilDebug.h/.cpp` | 改 | `Lyra.Recoil.RollShake` / `RollDebug` CVar；Roll 面板（含 ASCII 波形）；DebugDraw 品红弧；CSV 加 `RollShake` 列 |

### 3.3 核心算法（文档 §2.1 直译）

```
衰减包络 = 按衰减曲线插值(初始振幅, 目标振幅 = 初始振幅 × EndAmplitudeRatio, t / Duration)
相位扰动 = 在 ±PhaseJitter 内取一个小随机值（每发一次，震动期间不变）
周期项   = cos(2π / Period × t + 相位扰动)
Roll     = 衰减包络 × 周期项
```

实现要点（三条硬约束）：

1. **不累加**：每帧输出的是"当前应当施加的绝对 Roll 偏移量"，不是增量。因为 `ModifyCamera` 是逐帧重算 POV，累加会导致重复叠加。
2. **到点归零**：`Elapsed >= Duration` 时返回 `0`，而不是停留在末值。这是"往复震颤"与"单向偏移"的分界。
3. **参数快照**：`Trigger()` 时把装配好的参数缓存进 `State.ActiveParams`，震动全程沿用。避免"边打边跳"时姿态倍率变化导致同一次震动的节奏中途突变。

### 3.4 确定性随机的实现

```cpp
// 逐发新建随机流，而不是复用有状态流
const uint32 Combined = HashCombine(static_cast<uint32>(Seed), static_cast<uint32>(Index));
FRandomStream Stream(static_cast<int32>(Combined));
return Stream.FRandRange(-1.0f, 1.0f);   // [-1, 1]
```

与既有弹道链（`LyraRecoilState.cpp` 的 `DeterministicSignedRandom`）**同一范式**，
保证"第 N 步只取决于 (Seed, N)"，不受调用顺序影响 —— 这是 Golden 数据比对能成立的前提。

### 3.5 推进顺序

`FRecoilRuntimeState::Advance()` 内部顺序：

```
TimeSinceLastFire += DeltaSeconds;
ULyraCameraRollShake::Advance(RollShake, DeltaSeconds);   // ← 先推 Roll
switch (State) { ... }                                    // ← 再走回正状态机
```

**为什么先推 Roll**：保证同一帧内两者的时间基准一致。若把 Roll 放在状态机后面，
回正走到 `Idle` 时会顺带把 `TimeSinceLastFire` 归零，Roll 的推进就少了一帧的量。

### 3.6 与调试倍率的关系（两套倍率，别搞混）

| 倍率 | 来源 | 作用范围 | 是否写回状态 |
| --- | --- | --- | --- |
| `Lyra.Recoil.Scale` | CVar | Pitch / Yaw / Roll **三轴都乘** | 是（进 `GlobalScale`，参与 `BuildRollShakeParams`） |
| `Lyra.Recoil.RollShake` | CVar | **只乘 Roll 的显示层输出** | **否**（纯显示层缩放） |

第二条的关键：`Lyra.Recoil.RollShake 0` 可以在**不改资产、不污染 CSV / Golden 数据**的前提下临时关掉 Roll，
用于 A/B 对比"到底有没有 Roll 的差别"。这是调参期最有用的一个开关。

---

## 4. 实时调试

### 4.1 开关

| CVar | 默认 | 作用 |
| --- | --- | --- |
| `Lyra.Recoil.RollShake <f>` | `1.0` | Roll 实时振幅倍率。`0` = 临时关掉 Roll；`2` = 放大两倍便于观察 |
| `Lyra.Recoil.RollDebug 0/1` | `0` | Roll 实时面板（实时值 + 生效参数 + ASCII 波形） |

### 4.2 面板内容

Roll 面板独立于主面板（`Lyra.Recoil.Debug`），可以同时开、互不覆盖。四段：

```
[RollShake] ON  Scale=x1.00  Profile=DA_Recoil_Rifle_S  AssetRoll=on
  Active=yes  Roll=+0.4123 deg  Envelope=0.6871  t=0.0689/0.2200 (left 69%)
  Params: Amp=0.600 deg  Period=0.0550 s  Jitter=0.350 rad  EndRatio=0.00  Curve=linear  Shot=5
  StartAmp=0.600 deg  PhaseOffset=+0.1821 rad (10.4 deg)  Cycles≈4.00
  Waveform t=0..0.220s  peak=0.600 deg  (row density = amplitude, '|' = now)
        ......::
  :::..........
  ..............
  --------------  0
  ..............
  ..............
        ::::....
```

第三段（波形）是这一版新增的核心手段：把整段震动从 `t=0` 到 `t=Duration` 采样 48 点画成 ASCII 折线。

**为什么值得做**：Roll 的核心争议是"波形看起来对不对" —— 衰减够不够快、抖了几个来回、回零干净不干净。
盯着一个会跳的数字看不出这些，把整条曲线摊开在屏幕上就直接能判。

- 行密度 = 振幅强弱（`:` → `.` → 空格）
- `|` = 当前时刻的游标
- 中轴线标 `0`，上下对称

### 4.3 世界内可视化

`Lyra.Recoil.DebugDraw 1` 时新增**品红色圆弧**：

Roll 是**绕视线轴的旋转**，在 3D 里没法用"方向线"表达（它不改朝向，只改倾斜）。
所以改用**绕瞄准轴的一段圆弧**来示意 —— 弧从世界的"上"方向扫到"倾斜 Roll 度后的上"方向，
弧的倾角就等于当前 Roll。同时画一条白色细线做水平参照（相机右轴）。

文字标签也加了 Roll：

```
Recoil Accumulating  Shot=5  Pitch=0.83 Yaw=0.65  Roll=+0.412  Prog=0.00  Pose=Standing
```

### 4.4 CSV 导出

`Lyra.Recoil.Dump` 的表头**新增第 7 列** `RollShake`：

```
ShotIndex,VerticalKick,HorizontalKick,AccumulatedPitch,AccumulatedYaw,TimeSinceFire,RollShake
```

**列语义说明（重要）**：`RollShake` = **开火瞬间** Roll 震动值（解析解在 `t=0` 的采样），
不是"本发累计"。它与 `AccumulatedPitch/Yaw` 的语义不同 —— 后者是累加量，前者是解析解的瞬时值。
把两者并列在同一行只是为了"一次导出行内看到三轴的起点"，不表示它们同源。

---

## 5. 镜头模式系统（文档 §1）

文档 §1 要求镜头效果按「模式」组织，且**允许多个模式同时激活**。本次按此抽象出契约：

```cpp
UENUM(BlueprintType)
enum class ECameraShakeModeType : uint8
{
    RecoilPitchYaw,   // 后坐力抬枪（累加-回正）
    RollShake,        // Roll 阻尼震动（解析-归零）
    FOV,              // 开镜 FOV（中途反向保持连续）
    Breathing,        // 呼吸晃动
};

struct FCameraShakeOutput
{
    float Pitch = 0.0f;   // 相对量（度）
    float Yaw   = 0.0f;
    float Roll  = 0.0f;
    float FOV   = 0.0f;
    void Reset();
    bool IsNearlyZero() const;
    void Accumulate(const FCameraShakeOutput& Other);
};
```

**关键说明：`ECameraShakeModeType` 目前只用于调试分类，不驱动任何互斥逻辑。**
各模式天然并行：`ULyraRangedWeaponInstance::UpdateRecoilCameraModifier()` 每帧把三轴一起推给
相机修改器，修改器统一施加到 `FMinimalViewInfo`。没有"切换模式"这一步 —— 因为并行是要求，不是选项。

FOV / Breathing 两个枚举值是**为后续接入预留的分类位**，本次不实现具体算法（文档 §3 / §4 属于另一项需求）。

### 5.1 固定步长更新

文档 §2 / §7 要求射击相关逻辑走**固定步长**（如稳定 60Hz）以减少帧率对手感的影响。

当前实现：`ULyraRangedWeaponInstance::Tick()` 由 `ULyraWeaponStateComponent::TickComponent()` 每帧驱动，
传入的是**帧 DeltaSeconds**。这在 60fps 附近没问题，但高刷屏（144Hz）下 Roll 的采样率会变，
"抖几个来回"的观感会有细微差异。

**这是当前已知的一处未落地项**，见 §6 待办。

---

## 6. 待办与已知限制

| # | 事项 | 影响 | 建议 |
| --- | --- | --- | --- |
| 1 | **固定步长更新未落地**（文档 §2/§7 要求） | 高刷屏下 Roll 观感略有差异 | 在武器实例里加一个固定步长累加器（如 1/60s），把 `Advance()` 拆成 N 次调用 |
| 2 | FOV / Breathing 模式**只有枚举，无实现** | 文档 §3 / §4 的验收项无法覆盖 | 属独立需求，需另行排期 |
| 3 | Roll 目前**只作用在相机 POV**，不含骨骼跟随 | 文档 §5 的"骨骼旋转跟随"未做 | 若要做，需在角色动画层加局部坐标增量（文档 §5 的方案） |
| 4 | `RollShake_SegmentScaleCurve` / `PeriodScaleCurve` 默认空 | 空 = 系数恒 1，行为与不配一致 | 需要"分段不同节奏"时再配曲线 |
| 5 | CSV 的 `RollShake` 列取 `t=0` 采样 | 曲线只反映"起点强度"，不反映整段波形 | 要完整波形请用 `RollDebug` 面板或另做逐帧录制 |

### 6.1 已知的契约变更（P8 引入）

`Lyra.Recoil.Dump` 的 CSV 从 **6 列变 7 列**（末尾追加 `RollShake`）。
两个既有测试已同步更新：

- `Lyra.Recoil.Dump.HeaderSchema` —— 表头列数与逐列名
- `Lyra.Recoil.Dump.RowCountAndValues` —— 每行列数 + 新列的有限性断言

契约常量收敛在 `LyraRecoilDumpTest::ExpectedHeader` 与 `ExpectedColumnCount` **一处**，
以后改 CSV 只需要改那里，测试会立刻拦住不一致。

**任何解析既有 CSV 的外部脚本（例如 `Tools/plot-recoil-csv.py`）需要确认能否容忍多出的第 7 列。**

---

## 7. 验收清单

### 7.1 自动

```
AutomationTestToolset.RunTestsByFilter("StartsWith:Lyra.Recoil")
```

新增 Roll 相关测试（见 `Docs/Recoil/PROGRESS.md` 测试清单）。

### 7.2 手动（需要人在机器前）

1. PIE 进 `L_ShooterPerf`，控制台 `Lyra.Recoil.RollDebug 1`。
2. 单点一枪 → 面板应显示 `Active=yes`，`Roll` 值在 `±0.6°` 内快速往复，`t` 从 0 涨到 0.22 后停。
3. 观察波形行 → 应该能看到 4 个左右的波峰，且**越靠右越矮**（衰减生效），末尾贴近中轴（归零干净）。
4. `Lyra.Recoil.RollShake 0` → 画面上的横滚抖动应该**立刻消失**，但 Pitch/Yaw 后坐力**完全不受影响**。
   这一条同时验证了"Roll 是独立通道"。
5. `Lyra.Recoil.RollShake 3` → 横滚抖动明显放大，便于确认它作用在哪。
6. 长按左键扫完一个弹匣 → `StartAmp` 应该从 0.6 逐发涨到 1.5 封顶。
7. `Lyra.Recoil.DebugDraw 1` → 开火时出现品红圆弧，倾角随 Roll 往复；绿线（真实瞄准轴）**纹丝不动**。

**关键判据：第 4 条。** 如果 `RollShake 0` 之后 Pitch/Yaw 也变了，说明两条通道被意外耦合了，属回归。

---

## 8. 附：与 Pitch/Yaw 的代码归属对照

| 关注点 | Pitch / Yaw | Roll |
| --- | --- | --- |
| 参数组 | `Recoil` / `Recoil\|Pattern` | `Recoil\|RollShake` |
| 参数装配 | `ComputeShotKick()` | `BuildRollShakeParams()` |
| 状态字段 | `AccumulatedPitch/Yaw` | `RollShake.CurrentRoll` |
| 触发点 | `ApplyShot()` 内累加 | `ApplyShot()` 内 `Trigger()` |
| 推进点 | `Advance()` 的 `switch (State)` 分支 | `Advance()` 内独立一行 |
| 输出查询 | `GetCameraPitchOffset()` / `GetCameraYawOffset()` | `GetCameraRollOffset()` |
| 相机施加 | `InOutPOV.Rotation.Pitch +=` / `.Yaw +=`（带 `NormalizeAxis`） | `InOutPOV.Rotation.Roll +=`（**不带** `NormalizeAxis`） |
| 调试面板 | `Lyra.Recoil.Debug` | `Lyra.Recoil.RollDebug` |
| CSV 列 | `AccumulatedPitch` / `AccumulatedYaw` | `RollShake`（语义不同，见 §4.4） |
| 单发模型 | **阶跃累加 + 停火回正**（非文档 §2 的四段式，见 [09_SingleShotCurveGap.md](09_SingleShotCurveGap.md)） | **衰减包络 × 周期震动**（与文档 §2.1 一致） |

**为什么 Roll 不用 `NormalizeAxis`**：Pitch/Yaw 的累加量可能被玩家输入叠到 ±180 之外，需要归一化；
Roll 是"一瞬间往复震动、幅度 < 3°"的值，归一化是多余开销，且会引入不必要的分支。

> **注（2026-09-17）**：Pitch/Yaw 的单发模型与参考文档 §2 的四段式存在实质差距
> （缺 t1 瞬时回弹、缺阶段时长、缺上抬/下降曲线）。**Roll 本身没有这个差距** ——
> 它已经完全按文档 §2.1 实现。差异分析与迁移代价见
> → **[09_SingleShotCurveGap.md](09_SingleShotCurveGap.md)**。

---

_本文档由祥子整理，2026-09-17。依据《FPS 相机镜头设计与实现（脱敏版）》§1/§2/§2.1/§2.2/§7。_
