# 连发后坐力累积失效修复记录（Burst Accumulation Fix）

| 项 | 值 |
| --- | --- |
| 项目 | `E:\TPSGunsDemo` |
| 建立日期 | 2026-09-20 |
| 文档性质 | **缺陷修复记录 + 根因分析**。不是调参文档，不是设计方案。 |
| 缺陷等级 | 阻断级（后坐力系统在连发 + 压枪工况下整体失效） |
| 涉及文件 | `Source/LyraGame/Weapons/Recoil/LyraRecoilState.h`<br>`Source/LyraGame/Weapons/Recoil/LyraRecoilState.cpp` |
| 影响模式 | **仅 `Interpolated`**（场上 = 槽位 0 `ID_Rifle` / `DA_Recoil_Rifle_S`） |
| 未受影响 | `InstantWrite`（槽位 1 `ID_Rifle_7` / `DA_Recoil_Rifle_7`）、Golden 数据、CSV 契约、既有 30 个自动化用例 |
| 前置文档 | [10_SingleShotInterpolation.md](10_SingleShotInterpolation.md)（模型设计）<br>[07_TuningRecipe.md](07_TuningRecipe.md)（数值配方） |
| 云端参照 | `TPS_Recoil_Impl_v2.1` §6.4 / §7.1 / §7.4 / §7.5（tdrive `后坐力开发/`） |
| 编译状态 | ✅ **编译通过**（LyraEditor Win64 Development，`Result: Succeeded`，0 error / 0 warning，47.68s） |
| 测试状态 | ✅ **`Lyra.Recoil` 30/30 全绿**（`Failed: 0`，2026-09-20） |

---

## 0. 一句话说清

`Interpolated` 单发模型把「回弹终点」和「回正目标」都锚在了**绝对峰值**上。
连发时每一发都会把**整条已经累加好的偏移**按回弹比（0.72）往回乘一次，
于是后坐力变成几何衰减：`Base(n) = 0.72 × Peak(n−1)`，几发之后收敛到 ~1.05° 不再上涨。

**玩家看到的就是「连续开火时后坐力越打越小 / 一动鼠标压枪就像没有后坐力」。**

修复方式：把回弹 / 回正锚点从「绝对峰值」改成「**本发幅度 + 进入本发时的累加基底**」，
让"这一发的贡献"和"之前连发攒下的偏移"彻底解耦。
连发偏移因此恢复成**单调上涨**，30 发弹匣内爬升到 `MaxVerticalKick = 4.0°` 封顶 ——
与云端 `TPS_Recoil_Impl_v2.1` §7.4 的设计意图（默认 `pitch_max = 4.0°`、弹匣内封顶）一致。

---

## 1. 现象与复现

### 1.1 玩家侧描述（原始报告）

> 「武器连续开火时，如果我移动鼠标（开始压枪操作），后续的后坐力都会失效。就如果我一动鼠标后坐力就会变成 0。」

### 1.2 精确化之后的现象

| # | 观察 | 判定 |
| --- | --- | --- |
| A | 按住左键连发时，镜头抬升量**几发之后就不再增长** | 真实缺陷 |
| B | 单发射击的顿挫感看起来正常 | 正常 |
| C | 「一动鼠标」这个条件**本身不是触发条件**，只是放大感知 | 见 §2 |
| D | 切到槽位 1（`InstantWrite` 的 `DA_Recoil_Rifle_7`）后连发正常累加 | 定位到模式 |

### 1.3 复现步骤（PIE）

1. 进 `L_ShooterPerf`，用**槽位 0**（`ID_Rifle`，`Interpolated`）。
2. 对着墙长按左键打空一个弹匣，**完全不碰鼠标** → 镜头抬升到 ~1.05° 后就停住（异常）。
3. 换成**槽位 1**（`ID_Rifle_7`，`InstantWrite`）重复 → 镜头持续抬升、能到上限（正常）。
4. 用 `Lyra.Recoil.Dump` 导出两轮 CSV，对比 `AccumulatedPitch` 列的形状即可量化。

> 第 2 步**不需要动鼠标**就能看到后坐力不涨 —— 这一点很关键，它说明鼠标不是原因。

---

## 2. 为什么「鼠标」是红鲱鱼

报告里说「一动鼠标后坐力就变 0」，很容易误判成"鼠标输入污染了后坐力数值"。
**不是。** 完整走查后坐力全链路后确认：后坐力数值链与鼠标输入**零耦合**。

### 2.1 两条链路各自独立

| 链路 | 谁写入 | 谁读取 | 是否碰鼠标 |
| --- | --- | --- | --- |
| **玩家瞄准** | `AddControllerPitchInput` / `AddControllerYawInput`（`LyraHeroComponent::Input_LookMouse`）→ `ControlRotation` | 相机 pivot / 弹道基准 | 是（这条就是鼠标） |
| **后坐力** | `FRecoilRuntimeState::ApplyShot` / `Advance` → `AccumulatedPitch` / `CameraOffsetPitch` | `UCameraModifier_WeaponRecoil::ModifyCamera()`（**只改 `FMinimalViewInfo`**） | **否** |

`UCameraModifier_WeaponRecoil::ModifyCamera()` 的实现是对 `InOutPOV.Rotation` 做**显示层叠加**，
**绝不调用 `SetControlRotation` / `AddPitchInput`**。全局搜 `AddPitchInput` / `AddYawInput` 在后坐力模块内**零命中**。

这正是云端 `TPS_Recoil_Impl_v2.1` **§6.4 / 红线 R2** 要求的形状：

> 「后坐力偏移不应该被写进 `ControlRotation`」—— 因为一旦写回，下一帧玩家的鼠标输入就会从"已被后坐力推偏的视角"开始累积，**无法区分"玩家往下压了 3°"和"后坐力把视角推高了 3°"**。

本项目的实现遵守了这条红线，所以：

### 2.2 真实机理：不是"鼠标把它清零"，而是"它本来就快没了，一压枪就被盖住"

- 连发时后坐力几何衰减到 ~1.05°，本身已经很弱；
- 玩家一压枪，鼠标把视角往下拉了远大于 1° 的量；
- 视觉上「玩家自己的下拉」彻底盖住那点残留后坐力 → 主观感受就是"后坐力变成了 0"。

**结论：鼠标是放大器，不是原因。真正的缺陷是连发累积被几何衰减吃掉了。** 修复后第 3 节的现象 A 消失，压枪时仍能感到持续的抬升对抗。

---

## 3. 根因：几何衰减

### 3.1 模型里的三个锚点（修复前）

`Interpolated` 单发四段式：`Lift → Rebound → Settle → Drop`。定义：

```
Base      = InterpBasePitch        本发开始那一刻的逻辑偏移
Peak      = Base + 本发幅度        上抬段终点
```

修复前的**终点口径**是：

| 段 | 终点（修复前） |
| --- | --- |
| Lift 上抬 | `Peak` |
| **Rebound 回弹** | **`Peak × ReboundRatio`** ← 锚在绝对峰值 |
| Settle 稳定 | `Peak × ReboundRatio`（冻结） |
| **Drop 下降** | **`Peak × RecoilReturnRatio`** ← 锚在绝对峰值 |
| 回正 `ApplyRecoveryStep` | **`RecoveryPeak × RecoilReturnRatio`** ← 锚在绝对峰值 |

**单发场景（Base = 0）下，`Peak = 幅度`，所以"绝对峰值"和"本发幅度"是同一个数 —— 一切正常。**

### 3.2 连发场景下它是怎么烂掉的

连发时 `Base ≠ 0`。以场上 `DA_Recoil_Rifle_S` 为例：射速间隔 `0.12s`，
`Lift(0.045) + Rebound(0.030) = 0.075 < 0.12`，所以在两发之间单发时间轴**能走到 Settle**；
而 `Settle` 的时长复用 `RecoveryDelay = 0.12`，恰好被下一发打断（`Drop` 在连发期根本走不到）。

于是每一发的净累积变成：

```
Base(n) = ReboundEnd(n−1) = Peak(n−1) × ReboundRatio = 0.72 × Peak(n−1)
```

这是一个 **0.72 的等比递推**，收敛到：

```
Base(∞) = 0.72 × 幅度 / (1 − 0.72) ≈ 2.571 × 幅度
```

再叠上 `VerticalKickCurve` 的尾段放大，实测 30 发封顶在 **≈ 1.05°** 就再也不动。

### 3.3 数值仿真佐证

用 1:1 移植的 Python 模型（`LyraRecoilState.cpp` 逐步对照，`DA_Recoil_Rifle_S` 真实数值，
60fps、站姿、间隔 0.12s、30 发）：

| 指标 | 修复前（CURRENT） | 修复后（FIXED） |
| --- | --- | --- |
| 30 发内最大逻辑偏移 `maxAccP` | **1.0496°** | **4.0446°**（触上限 4.0°） |
| 30 发内最大相机补间 `maxCamP` | 1.1693° | 4.0446° |
| 逻辑 vs 补间最大分叉 | **0.1507** | 0.0312 |
| 12 发（发前基底） | 0.857（窗口峰值） | **1.6715** |
| 单发等价性（1 发逐点对比） | — | **diff = 0.0000000（逐位相同）** |
| 4.0° 上限首越弹序 | 永不可达 | **第 23 发** |

> 修复前 30 发只有 1.05°，连文档 [07_TuningRecipe §3.1](07_TuningRecipe.md) 的 12 发数值（2.342°）都到不了 —— 这就是"失效"。

---

## 4. 修复方案（三处协同，各管一件事）

修复原则：**把"本发贡献"与"已累加基底"解耦**。三处必须同时改，缺一处会引入新的跳变或分叉。

### 4.1 A · 阶段终点锚定「本发幅度」

`LyraRecoilState.cpp` → `ComputeStageTarget()`

```cpp
// 修复前（锚绝对峰值）
const float ReboundEndPitch = PeakPitch * Profile.ReboundRatio;
const float SteadyEndPitch  = PeakPitch * Profile.RecoilReturnRatio;

// 修复后（锚「基底 + 本发幅度」）
const float ReboundEndPitch = BasePitch + State.InterpShotAmplitudePitch * Profile.ReboundRatio;
const float SteadyEndPitch  = BasePitch + State.InterpShotAmplitudePitch * Profile.RecoilReturnRatio;
```

- 单发时 `Base = 0` → **与旧公式逐位等价**，手感/曲线/单发用例一字不变。
- 连发时回弹量恒为 `幅度 × (1 − ReboundRatio)`，**与打了几发无关** → 偏移单调上涨。

### 4.2 B · 子步内「先夹一次、两处同写」

`LyraRecoilState.cpp` → `AdvanceInterpolatedSubStepBy()`

```cpp
// 修复前：逻辑偏移夹了，补间输出却拿未夹的目标做差分 → 到上限后两者分叉
State.AccumulatedPitch = FMath::Clamp(TargetPitch, -Max, Max);   // 只夹逻辑
State.CameraOffsetPitch += (TargetPitch - State.LastTargetPitch); // 未夹

// 修复后：先夹一次，再同时写进「补间输出」与「逻辑偏移」
const float ClampedPitch = FMath::Clamp(TargetPitch, -Profile.MaxVerticalKick, Profile.MaxVerticalKick);
State.CameraOffsetPitch += (ClampedPitch - State.LastTargetPitch);
State.LastTargetPitch = ClampedPitch;
State.AccumulatedPitch = ClampedPitch;   // 两个输出恒等
```

这一条对应云端 `TPS_Recoil_Impl_v2.1` **§7.1 / §7.4** 的口径：

> 「为什么 clamp 要在累加之后、而不是'每加一条就 clamp'……**逐条 clamp，后面脉冲的贡献会被前面已饱和的值吞掉，导致"到了上限之后新发的后坐力完全消失"**。在**总和**上 clamp 更符合"到顶后不再累加"的语义。」

修复前正是"逐条夹"的变体：逻辑偏移夹住了、补间输出没夹，到上限后两者分叉，
实测最大分叉 0.15°（修复后降到 0.031°，只剩子步相位的正常残差）。

### 4.3 C · 回正锚定「累加基底」

新增两个状态字段 `RecoveryBasePitch` / `RecoveryBaseYaw`（`LyraRecoilState.h`），
语义 = **「进入本发那一刻已经累加好的偏移」**。回正目标改为：

```cpp
// 修复前
Target = RecoveryPeak × RecoilReturnRatio;
// 修复后
Target = RecoveryBase + (RecoveryPeak − RecoveryBase) × RecoilReturnRatio;
```

即「**只衰减本发贡献，不衰减之前连发累加的偏移**」。
在 6 处写入/清零 `RecoveryBase`：`Reset`、`ApplyShot`（两分支）、`Advance` 的 Accumulating 分支、
子步 Drop 收尾、长帧保护收尾。**`InstantWrite` 分支恒置 0 → 旧公式 = 新公式，零行为变化。**

> 若不改这一处（只改 A/B）：连发中途一旦 `RecoveryDelay` 撞上射速间隔误触发一次回正，
> 仍会把整条已累加偏移按 0.15 吃掉 —— 等于换个地方重现同一个 bug。

---

## 5. 代码改动清单

### 5.1 `LyraRecoilState.h`

| 位置 | 改动 |
| --- | --- |
| 结构体字段区（`RecoveryPeakYaw` 之后，≈ L196–L216） | **新增** `float RecoveryBasePitch / RecoveryBaseYaw`（`UPROPERTY(BlueprintReadOnly, Category="Recoil\|Internal")`），含语义注释与 `InstantWrite` 退化说明 |

### 5.2 `LyraRecoilState.cpp`

| 函数 | 位置（≈） | 改动 |
| --- | --- | --- |
| `ApplyRecoveryStep` | L35–L43 | 回正目标改 `Base + (Peak − Base) × RecoilReturnRatio` |
| `ComputeStageTarget` | L146–L166 | `ReboundEnd` / `SteadyEnd` 改锚「Base + 幅度 × 比例」 |
| `AdvanceInterpolatedSubStepBy` | L280–L308 | 上限**先夹一次**，`CameraOffset` 与 `Accumulated` **同写** |
| `AdvanceInterpolatedSubStepBy`（Drop 收尾） | L333–L335 | 转常规回正前带 `RecoveryBase = InterpBase` |
| `Advance`（长帧保护） | L718–L747 | 稳态残留改 `InterpBase + 幅度 × 回正比`，并带 `RecoveryBase = InterpBase` |
| `Advance`（Accumulating 分支） | L770–L782 | 按模式设基底：`Interpolated` 取 `InterpBase`，`InstantWrite` 取 0 |
| `Reset` | L414–L415 | 清零 `RecoveryBasePitch/Yaw` |
| `ApplyShot`（Interpolated 分支） | L564–L567 | 设 `RecoveryBase = InterpBase` |
| `ApplyShot`（InstantWrite 分支） | L585–L588 | 显式置 `RecoveryBase = 0` |

**刻意不动**：`InstantWrite` 的行为、`FRecoilShotResult` 字段（CSV 7 列契约）、
`ShotHistory`、Golden 3 份 JSON、既有 30 个测试断言、`LyraRecoilProfile` 的任何参数默认值。

---

## 6. 回归影响分析

| 对象 | 结论 | 依据 |
| --- | --- | --- |
| `InstantWrite` 模式（槽位 1、Golden、CSV、19 个老用例） | **零行为变化** | `RecoveryBase` 恒为 0 → 新公式逐位退化成旧公式；`ApplyShot` 的 InstantWrite 分支未改 |
| 单发（1 发，`Interpolated`） | **逐位等价** | `Base = 0` → 新公式 = 旧公式；仿真 `diff = 0.0000000` |
| `Lyra.Recoil.Interp.*` 6 个用例 | **不受影响** | 全部是**单发**或「整数倍子步时长 + 单发」的帧率不变性用例，`Base = 0` |
| `Lyra.Recoil.Interp.FrameRateInvariance` | **仍成立** | 单发 20/60/144fps 逐点一致（修复不改变固定子步长机制） |
| Golden `RecoilGolden_DA_Recoil_Rifle.json` | **无需重导** | Golden 基准枪是 `DA_Recoil_Rifle`（`InstantWrite`） |
| `DA_Recoil_Rifle_S` 的**连发**手感 | **改变**（预期内） | 累积从「≈1.05° 封顶」变回「单调爬升、4.0° 封顶」 |

> ⚠️ **唯一需要拍板的**：`Interpolated` 的连发累积基线被"修回正常"了 —— 这不是调参，是把设计本意（[10_SingleShotInterpolation §7](10_SingleShotInterpolation.md)「逻辑偏移照常累加」）恢复回来。

---

## 7. 验证

### 7.1 纯数值仿真（已做）

脚本：`sim4.py` / `sim5.py`（1:1 移植 `LyraRecoilState.cpp` + `DA_Recoil_Rifle_S` 真实数值）。

| 用例 | 结果 |
| --- | --- |
| 30 发连发（60fps） | CURRENT 1.0496° → FIXED 4.0446°（触上限，**不再衰减**） |
| 单发等价性 | 1 发逐点 `max\|diff\| = 0.0000000` → **IDENTICAL** |
| 射速敏感性 | 射速越高累积越多（0.06s → 3.48°；0.18s → 3.36°），方向正确 |
| 上限可达 | FIXED 首次触 4.0° = **第 23 发**，与云文档 §7.4「默认 4.0°、弹匣内封顶」一致 |

### 7.2 自动化测试（✅ 已跑，30/30 全绿）

```powershell
powershell -ExecutionPolicy Bypass -File "E:\TPSGunsDemo\Docs\Recoil\Tools\run-recoil-tests.ps1" `
    -EngineRoot "E:\UE_5.8" -Project "E:\TPSGunsDemo\TPSGunsDemo.uproject"
```

**结果（2026-09-20）**：`Automation Test Queue Empty 30 tests performed`，`Succeeded: 30`，`Failed: 0`。

关键回归项（此前最担心的几条）全部通过：

| 用例 | 关心什么 |
| --- | --- |
| `Lyra.Recoil.State.Accumulation` | 累加语义（InstantWrite 基线）未变 |
| `Lyra.Recoil.State.Clamp` | 上限钳制未变 |
| `Lyra.Recoil.State.RecoverySteadyState` | 稳态残留值未变 |
| `Lyra.Recoil.State.RefireDuringRecovery` | 回正中途再次开火（正是本次缺陷的同源场景） |
| `Lyra.Recoil.Interp.FrameRateInvariance` | 单发 20/60/144fps 逐点一致 |
| `Lyra.Recoil.Interp.ModeIsolation` | `InstantWrite` 下 `CameraOffset == Accumulated` |
| `Lyra.Recoil.Pattern.Golden` | Golden 3 份 JSON 比对通过（无需重导） |

### 7.3 手动 PIE 验收（待做，需大祥老师本人在机器前）

| # | 步骤 | 期望 |
| --- | --- | --- |
| 1 | 槽位 0（`Interpolated`）长按打空弹匣，**不碰鼠标** | 镜头**持续抬升**、到 ~4.0° 封顶（修复前：1.05° 就不动） |
| 2 | 同一步骤，**同时缓慢下拉鼠标压枪** | 仍能感到持续上抬对抗；松开鼠标后镜头**平滑回到**接近原朝向上方（不是掉到地板） |
| 3 | `Lyra.Recoil.Dump` 导出 CSV，看 `AccumulatedPitch` 列 | 单调上升后触顶，而不是早期饱和 |
| 4 | 切槽位 1（`InstantWrite`）重复步骤 1 | 与修复前一致（零回归） |
| 5 | 单发射击 | 顿挫感与修复前一致 |

---

## 8. 与云端设计文档的一致性

| 云端条款（`TPS_Recoil_Impl_v2.1`） | 本次修复的对应 |
| --- | --- |
| **§6.4 / 红线 R2**：不把后坐力偏移写回 `ControlRotation` | 未改动。修复只动算法层数值，相机链继续只改 `FMinimalViewInfo`（§2 已证） |
| **§7.1 / §7.4**：上限 clamp 作用在**总和**上，不能逐条夹（否则"到顶后新发的后坐力消失"） | 修复 B：子步内**先夹一次、两处同写**，消除逐条夹导致的分叉（0.15 → 0.031） |
| **§7.5 / R7**：`DelayWait` / `Recovering` 期间再次开火 → 立即回 `Firing`，**不重置 Offset（后坐力继续累积）** | 修复 A/C：连发期偏移单调累积，不再被每发的回弹/回正按比例吃回 |
| **§7.4**：默认 `pitch_max = 4.0°`、弹匣内封顶（首越约第 22 发） | 修复后 4.0° 封顶、首越第 23 发（数值量级一致） |

> **一句话**：修复前是"逐条夹 / 每发重置"的负向变体（偏移被几何衰减吞掉），
> 修复后回到云端文档明确要求的"总和累积"语义。

---

## 9. 残留差异与后续可选项（需大祥老师拍板）

### 9.1 12 发累计值的口径差异（诚实说明）

[07_TuningRecipe §3.1](07_TuningRecipe.md) 的 S 型表列出「12 发垂直累计 = **2.342°**」。
该表的计算口径是**裸和**（逐发增量直接求和，见 §5.2「累计 = 逐发求和」），
等价于 `InstantWrite` 的连发语义（射速间隔 ≤ `RecoveryDelay` 时回正不触发 → 100% 累积）。

但 `Interpolated` 模型有显式的**回弹段**：连发期每发在 `Settle` 被下一发打断，
净累积 = `本发幅度 × ReboundRatio = 0.72 × 幅度`。所以：

| 口径 | 12 发垂直累计 |
| --- | --- |
| 07_TuningRecipe §3.1（裸和） | 2.342° |
| `InstantWrite` 实跑 | ≈ 2.342°（一致） |
| **`Interpolated` 修复后实跑** | **≈ 1.67°（≈ 0.72 × 2.342°）** |

**这不是 bug**，是"回弹比 0.72"这一设计在连发下的自然结果；但它和 §3.1 表的数字不一致。

**可选后续（本次未做，属设计变更）**：若希望 `Interpolated` 连发累积严格等于裸和，
需要把「视觉回弹」与「累积基底」**解耦** —— 即新增一个独立的"累积峰值"场，
让下一发的 `Base` 取"累加 100% 的峰值"，`Settle` 只冻结视觉值不动基底。
这会改变单发之外的全部连发手感，**需要你明确要求再动**。

### 9.2 手感确认项

- 4.0° 封顶是否合适？（`MaxVerticalKick` = 4.0，云端 §7.4 默认值也是 4.0）
- 首越弹序第 23 发是否符合"一个弹匣打满"的节奏预期？
- 压枪手感：修复后需要玩家真正"压"住 4°，比修复前的 1° 明显更费力 —— 这是设计本意（压枪向），但请实机确认强度是否可接受。

---

## 10. 待办

| # | 事项 | 状态 |
| --- | --- | --- |
| 1 | 编译（关编辑器或 Live Coding） | ✅ **通过**（`Result: Succeeded`，0 error） |
| 2 | 跑 `Lyra.Recoil` 30/30 | ✅ **30/30 全绿，Failed: 0** |
| 3 | PIE 手动验收（§7.3） | ⏳ **待做**（需大祥老师本人在机器前） |
| 4 | 若确认"连发累积 = 裸和"，落地 §9.1 的解耦方案 | 待拍板 |
| 5 | 更新 [PROGRESS.md](PROGRESS.md)（已随本文档同步） | ✅ |

---

## 11. 附：关于「钳制上限是否应该减掉压枪量」的结论（2026-09-20 讨论）

> ⚠️ **本节结论（"不做"）已于同日被实机复现推翻 —— 见 [§12](#12-追加修复2026-09-20垂直钳制实时抵扣玩家压枪量)。
> 保留本节是为了记录推翻过程：§11.1 的论证前提在 §4 落地后失效，§11.3 的三条顾虑在 §12.2 逐条处理。**
> 原始 §11.4 第三行「钳制净位移 `ControlRotation.Pitch + offset`，并给裸偏移留一个宽松的硬上限做安全阀」
> 就是最终采用的做法 —— 当时列了它、却因为"要承认两处代价"而没有选它。

> 提问：`MaxVerticalKick / MaxHorizontalKick` 是「累加偏移的钳制上限」，
> 但它只看裸的累加偏移，**没有考虑玩家压枪** —— 是否应该在计算时实时减去玩家的压枪角度？

### 11.1 先纠正一个前提：这不是本次缺陷的原因

| 事实 | 数值 |
| --- | --- |
| 修复**前** 30 发连发的最大逻辑偏移 | **1.0496°** |
| `DA_Recoil_Rifle_S` 的 `MaxVerticalKick` | **4.0°** |
| 结论 | 上限**从未被触及**，所以它不可能造成「后坐力变成 0」 |

本次缺陷的真因是 §3 的几何衰减（`Base(n) = 0.72 × Peak(n−1)`），与钳制无关。
**钳制要不要考虑压枪，是一个独立的设计问题，不是这个 bug 的成因。**

### 11.2 代码事实：钳制点与压枪量之间没有数据通路

全项目只有 **3 处** 对后坐力偏移的钳制，全部只吃偏移本身：

| 位置 | 代码 |
| --- | --- |
| `LyraRecoilState.cpp` L292–293 | `Clamp(TargetPitch, ±MaxVerticalKick)`（Interp 子步） |
| `LyraRecoilState.cpp` L577–578 | `Clamp(AccumulatedPitch + Kick, ±MaxVerticalKick)`（InstantWrite） |
| `LyraRecoilState.cpp` L724–725 | 长帧保护落稳态时的同一钳制 |

而玩家压枪完全不在这里：

```
鼠标 → LyraHeroComponent::Input_LookMouse → AddControllerPitchInput
     → ControlRotation
     → LyraCameraMode_ThirdPerson::UpdateView() 夹 ±89°（LYRA_CAMERA_DEFAULT_PITCH_*）
     → LyraCameraComponent::GetCameraView() 写回 PC->SetControlRotation()

后坐力 → FRecoilRuntimeState → CameraOffsetPitch → UCameraModifier_WeaponRecoil::ModifyCamera()
                                                  → InOutPOV.Rotation += offset（仅显示层）
```

两条链**直到 `ModifyCamera` 的最后一加才相遇**。这正是云端 `TPS_Recoil_Impl_v2.1`
**§6.4 / 红线 R2** 要求的形状：偏移绝不写回 `ControlRotation`，让"玩家补偿量"与"后坐力偏移"各自独立可测。
（`LyraRecoilDebug` 的绿线画的是 `ControlRotation`，它"永远不随后坐力动"，就是这条红线的可视证据。）

### 11.3 为什么「上限 ± 压枪量」不建议做

| # | 问题 | 说明 |
| --- | --- | --- |
| 1 | **正反馈回路** | `压枪 ↑ → 上限 ↑ → 偏移更多 → 镜头推更高 → 需要压更多`。后坐力系统通常刻意做成开环，就是为了避免这种自激；叠加固定子步累加器后容易发涩/抖动 |
| 2 | **上限不再确定** | `Lyra.Recoil.State.Clamp` 直接断言上限，云端 §7.4 的「上限可达性 / 首越弹序」分析也把 4.0° 当作常量。上限一旦跟玩家状态走，Golden / CSV / 回归判据全部失去确定的比较基准 |
| 3 | **破坏纯算法层**（开发计划硬性规则 4） | `FRecoilRuntimeState` 目前无 UWorld 依赖、全部输入靠参数传入。要读压枪量就得每帧喂 `ControlRotation` + 一个基线，纯数值可测性下降 |
| 4 | **"压枪量"需要基线才有定义** | 只能像云端 §6.4 那样，在连发首发前记录 `PreFirePlayerAim`，然后 `补偿量 = ControlRotation.Pitch − PreFirePlayerAim.Pitch`。没有基线，这个量是未定义的 |
| 5 | **符号歧义且有一半更糟** | `上限 − 压枪` → 上限更小，"到顶后新发后坐力消失"**更严重**（正是云端 §7.1 明确警告的失效模式）；`上限 + 压枪` 才等于"净位移到 4°"，但那就让上限超过文档写死的 `MaxVerticalKick` |

### 11.4 如果真正想要的是这三种效果，正确的做法

| 想要的效果 | 正确做法 | 代价 |
| --- | --- | --- |
| **到顶后不要"啪"一下死掉**，想让后坐力持续有存在感 | 把硬钳制换成**软饱和**（如 `offset = Max × tanh(raw / Max)`）：永不硬停、永不发散，也不需要读压枪量 | 改 `LyraRecoilState.cpp` 一条公式；会改动非单发基线，需重导相关数值表 |
| **上限来得太早**（第 23 发就封顶） | **纯调参**：调大 `MaxVerticalKick`（资产改动，零代码） | 无代码风险；只影响该枪手感 |
| **压枪的人不该"浪费"后坐力预算**（净位移也要能到 4°） | 钳制**净位移** `ControlRotation.Pitch + offset`，并给裸偏移留一个宽松的硬上限做安全阀 | 需要一个帧级基线 + 喂入 `ControlRotation`（破坏纯算法层），且带 11.3 #1 的回路风险 —— **要做就得明确承认这两点** |

### 11.5 一个顺带发现（与本缺陷无关，但值得记）

UE 的相机修改器在 `APlayerCameraManager` 里**晚于**相机模式求值，所以后坐力偏移是在
`ViewPitchMin/Max`（当前 = ±89°）钳制**之后**加上去的 —— 也就是说**显示层总俯仰角本身没有任何上限**。
4° 量级下无害，但如果将来把 `MaxVerticalKick` 调到很大（或做极端压枪测试），
可能出现"画面越过 ±89° 而瞄准方向没越"的观感。届时才需要考虑钳制净位移。

## 12. 追加修复（2026-09-20）：垂直钳制实时抵扣玩家压枪量

> 大祥老师实机复现后推翻了 [§11](#11-附关于钳制上限是否应该减掉压枪量的结论2026-09-20-讨论) 的"不做"结论。
> 本节记录：为什么推翻、改成什么、§11.3 的三条顾虑各自怎么处理、以及验证证据。

### 12.0 一句话

`MaxVerticalKick` 的语义从「**裸累加偏移**的上限」改成「**镜头相对起枪点的净抬升**的上限」：

```
旧：clamp(offset, ±MaxVerticalKick)
新：clamp(offset, ±(MaxVerticalKick + min(aimComp, MaxVerticalKick)))
       其中 aimComp = 本梭玩家压枪量（度，向下压为正，恒 ≥ 0）
```

`aimComp` 由武器实例每帧写入 —— 只有它拿得到 Pawn / Controller；算法层**只消费、不推导**，
"纯数值层无 UWorld 依赖"（开发计划硬性规则 4）因此保持不变。

### 12.1 玩家报告 —— 以及 §11 为什么看错了

> 「现在是我哪怕压枪了，如果连续开火时达到了钳制的值以后，武器就约等于没有后坐力了，
>  因为不会再触发镜头的上抬。此时我不管怎么移动鼠标，都不会触发后座了。
>  钳制需要实时减去玩家的压枪值的，他的作用是**当玩家压不住枪的时候，不让镜头飞的太高**。」

这段话里有两个 §11 没抓住的事实：

| # | §11 的说法 | 实际情况 |
| --- | --- | --- |
| 1 | §11.1 用「修复前峰值 **1.0496°** < `MaxVerticalKick` **4.0°**」论证"上限从未被触及" | 那是**修复前**的几何衰减把峰值压住了。§4 的修复把峰值推到 **4.0446°** 之后，**上限在第 23 发就被触及**（见 PROGRESS §3 基线表）。§11.1 的前提在 §4 落地后**已失效** |
| 2 | §11.3 #5 认为"上限 ± 压枪量"有两个符号分支，`+` 分支"只是让上限超过文档写死的值" | 关键不在符号，而在**钳制对象**。钳制的目的是"玩家压不住枪时不让镜头飞太高" ⇒ 被钳的应该是**镜头实际抬升量（offset − aimComp）**。钳裸 offset ⇒ 压枪的人拿到 `MaxVerticalKick − aimComp` 的净抬升就**提前封顶** ⇒ 体感"打着打着后坐力没了" |

### 12.2 §11.3 三条顾虑的逐条处理

| §11.3 顾虑 | 处理 | 残留 |
| --- | --- | --- |
| **#1 正反馈回路**（压枪↑ → 上限↑ → 偏移↑ → 要压更多） | **承认它存在，但把它限死**：抵扣量夹在 `min(aimComp, MaxVerticalKick)`，所以裸偏移硬顶 = `2 × MaxVerticalKick`。回路有界、不发散 | **大祥老师需要实机确认"压满时会不会发涩/抖动"。这是本节最大的待确认项** |
| **#2 上限不再是常量 → Golden / CSV / 断言失去确定基准** | **不成立**。`aimComp` 默认 `0`，且算法层从不自己推导它 —— 自动化测试与 Golden 走的是"无人喂入"路径，上限仍是**常量 4.0°**，既有 30 个用例**逐位不变** | 无 |
| **#3 破坏纯算法层**（硬性规则 4） | **不成立**。算法层仍无 UWorld 依赖，只是多了一个**入参** `SetAimCompensationPitch()`；读 `ControlRotation` 的动作全部留在武器实例里 | 无 |

### 12.3 为什么代码里是"抬高 clamp 边界"而不是"先减后夹"

一个容易写错的地方：`clamp(offset − aimComp)` 是**错的** —— 它让上限**变小**
（`4 − 2 = 2`），"到顶后新发后坐力消失"反而更严重。正确的等价改写是**把抵扣加到 clamp 的边界上**：

```
offset − aimComp ≤ MaxV      ⟺      offset ≤ MaxV + aimComp
```

所以代码里**没有**"先减后夹"这一步，而是把 `MaxV + aimComp` 当成新的夹持边界。
好处：既有 **3 处** clamp 的结构一字不改，只换边界来源。

### 12.4 代码改动清单

| 文件 | 位置 | 改动 |
| --- | --- | --- |
| `LyraRecoilState.h` | L241–242 | 新增 `UPROPERTY float AimCompensationPitch = 0.0f;`（**默认 0 ⇒ 旧行为**） |
| `LyraRecoilState.h` | L368–371 | 新增 `SetAimCompensationPitch()`（内部 `FMath::Max(0, x)`） |
| `LyraRecoilState.h` | L382 | 声明 `GetEffectiveVerticalKickLimit(const ULyraRecoilProfile&)` |
| `LyraRecoilState.cpp` | L411–418 | **新增** `GetEffectiveVerticalKickLimit()`：`return Profile.MaxVerticalKick + FMath::Min(AimCompensationPitch, Profile.MaxVerticalKick);` |
| `LyraRecoilState.cpp` | L297 | Interp 子步 clamp：`VerticalLimit` 改取 `GetEffectiveVerticalKickLimit(Profile)` |
| `LyraRecoilState.cpp` | L597 | InstantWrite 累加 clamp：同上 |
| `LyraRecoilState.cpp` | L745 | 长帧保护落稳态 clamp：同上 |
| `LyraRecoilState.cpp` | L432 | `Reset()` 里清零 `AimCompensationPitch` |
| `LyraRangedWeaponInstance.h` | L256 | 新增私有字段 `float BurstStartAimPitch`（本梭"起枪点"俯仰） |
| `LyraRangedWeaponInstance.h` | L343 / L351 | 声明 `TryGetAimPitch()` / `ComputeAimCompensationPitch()` |
| `LyraRangedWeaponInstance.cpp` | L283–287 | `AddRecoil()`：`State == Idle`（本梭第一发、在 `ApplyShot` 之前）时记录 `BurstStartAimPitch` |
| `LyraRangedWeaponInstance.cpp` | L317 | `UpdateRecoil()`：在 `Advance()` **之前**喂 `SetAimCompensationPitch(ComputeAimCompensationPitch())` |
| `LyraRangedWeaponInstance.cpp` | L362–380 | `TryGetAimPitch()`：读 `Controller->GetControlRotation().Pitch`（**不含**后坐力偏移，正是压枪量的正确来源） |
| `LyraRangedWeaponInstance.cpp` | L383–400 | `ComputeAimCompensationPitch()`：`Idle` 时返回 0；否则 `FMath::Max(0, BurstStartAimPitch − AimPitch)` |
| `LyraRangedWeaponInstance.cpp` | L408 | `ResetRecoilState()` 同步清零基线 |
| `LyraRecoilDebug.cpp` | L212–219 | 面板 Accum 行新增 `CapV=` / `aimComp +` 两列 |

**为什么基线（`BurstStartAimPitch`）必须在 `AddRecoil` 里取：**
`ApplyShot` 之后 `State` 已变成 `Accumulating`，而"本梭第一个"的判据正是 `State == Idle`
（`ApplyShot` 内部也是靠它把 `ShotIndex` 归零）。两处判据**必须一致**，否则基线会取到第二发的俯仰。
此刻读到的俯仰就是"既没被自己压枪、也没被后坐力推高"的基准。

**为什么 `TryGetAimPitch` 读 `ControlRotation` 而不是显示层 POV：**
云端 `TPS_Recoil_Impl_v2.1` §6.4 / 红线 R2 —— 后坐力偏移**只**作用于显示层 POV
（`UCameraModifier_WeaponRecoil::ModifyCamera`），绝不写回 `ControlRotation`。
所以 `ControlRotation.Pitch` 就是"玩家自己瞄到哪"，是压枪量的**唯一正确来源**。
（`LyraRecoilDebug` 的绿线画的也是 `ControlRotation` —— 它"永远不随后坐力动"，正是这条红线的可视证据。）

### 12.5 纯数值仿真证据（A/B，已做）

脚本 `sim8.py`，状态机与 `LyraRecoilState.cpp` 的 `Interpolated` 分支 1:1 对应，
只改钳制边界来源。场景：`DA_Recoil_Rifle_S`，`MaxVerticalKick = 4.0`，30 发 @0.12s 间隔，60fps。

```
  cover |  OLD raw  OLD net |  NEW raw  NEW net |  OLD maxStep  NEW maxStep
  ------+-------------------+-------------------+---------------------------
    0.0 |  4.0000  4.0000 |   4.0000  4.0000 |     0.18922     0.18922
    1.0 |  4.0000  3.0000 |   5.0000  4.0000 |     0.18922     0.18922
    2.0 |  4.0000  2.0000 |   5.1506  3.1506 |     0.18922     0.19144
    3.0 |  4.0000  1.0000 |   5.1506  2.1506 |     0.18922     0.19144
    4.0 |  4.0000  0.0000 |   5.1506  1.1506 |     0.18922     0.19144
    6.0 |  4.0000  0.0000 |   5.1506  0.0000 |     0.18922     0.19144
```

口径：`cover` = 本梭玩家压枪量（度）；`raw` = 裸累加偏移峰值；`net` = 镜头相对起枪点的净抬升 = `raw − cover`；
`maxStep` = 单个子步对裸偏移的最大增量（衡量"这一发还顶不顶得动"）。

| 读法 | 结论 |
| --- | --- |
| `cover = 0`（不压枪） | 新旧**逐位相同**：`raw` 都是 4.0000、`net` 都是 4.0000、`maxStep` 都是 0.18922 ⇒ **零回归** |
| `cover = 4.0`（压枪量 = `MaxV`） | 旧口径净抬升 **0.0000** —— 这就是大祥老师说的"约等于没有后坐力"；新口径 **1.1506** |
| 新口径 `cover ≥ 2` | 封顶的不再是钳制，而是连发**自然峰值 5.1506°**（钳制边界 6.0 / 7.0 / 8.0 都没参与）⇒ "抵扣"只在真的顶到上限时才起作用 |
| `cover = 6.0` | 净抬升 ≤ 0（玩家压过头了）—— 钳制全程未参与 |

精确定式（可直接手算复核）：
- **旧**：`net_max = min(MaxV, 自然峰值) − cover`
- **新**：`net_max = min(MaxV + min(cover, MaxV), 自然峰值) − cover`

### 12.6 编译与回归（✅ 已执行，全绿）

| 项 | 结果 |
| --- | --- |
| 编译 `LyraEditor Win64 Development`（`-NoUBA`） | ✅ **`Result: Succeeded`，0 error / 0 warning**，33 actions，56.22s（含 UHT 20.6s / 466 个生成文件） |
| `Lyra.Recoil` 自动化测试 | ✅ **37/37 全绿（`Result={Fail}` = 0）**，含 `State.Clamp`、`State.Accumulation`、`Interp.FrameRateInvariance`、`Pattern.Golden`、`Profile.Validation` |
| Golden 基准（5 份 JSON） | ✅ **md5 逐位未变** —— 跑测试前后哈希完全一致，证明默认 `aimComp = 0` 下**零回归** |
| CSV 契约 | ✅ 未改动 |

> 用例数从 30 变 **37** 的原因：这中间还落地过一个「资产散布（Spread）」特性，新增了 7 个
> `Lyra.Recoil.Spread.*` 用例（`Source/LyraGame/Tests/LyraRecoilSpreadTest.spec.cpp`）。
> 与本节改动无关，但一起跑了，全绿。

#### ⚠️ 踩坑记录：首次编译失败，**不是代码问题**

第一次编译报了一串看着很吓人的错，且**指向一个本节根本没改过的文件**：

```
LyraRecoilDebug.h(43,14): error C2143: 语法错误: 缺少";"(在"<class-head>"的前面)
LyraRecoilDebug.h(42,1):  error C4430: 缺少类型说明符 - 假定为 int      ← UCLASS()
LyraRecoilDebug.h(45,2):  error C4430: 缺少类型说明符 - 假定为 int      ← GENERATED_BODY()
LyraRecoilDebug.gen.cpp(...): error C2039: "execGetGlobalScale": 不是 "ULyraRecoilDebug" 的成员
```

**真因：UHT 静默没重跑 → `.generated.h` 陈旧 → `UCLASS()` 展开出的带行号宏对不上。**

- `LyraRecoilDebug.generated.h` 停留在 **9-18 17:12**，而 `LyraRecoilDebug.h` 是 **9-20 16:27**；
  本次构建日志里**一条 UHT 行都没有**（只有一个 11 action、全是 `Compile`/`Link` 的图）。
- UE 的 `UCLASS()` 会展开成**按行号拼出来的宏**（`BODY_MACRO_COMBINE(CURRENT_FILE_ID,_,__LINE__,_PROLOG)`
  → 形如 `LYRAGAME_LyraRecoilDebug_h_42_PROLOG`），这些定义在 `.generated.h` 里。
  新增的那行文档注释（`Lyra.Recoil.SpreadDebug`）把 `UCLASS()` 从第 41 行顶到了 **42** 行，
  陈旧的生成头里只有 `_41_PROLOG` → `UCLASS()` 变成一个未定义标识符 → 级联全崩。

**修法**（不要改代码，改的是构建缓存）：

```bash
rm -rf Intermediate/Build/Win64/UnrealEditor/Inc/LyraGame
```

之后日志里出现 `UHT processed LyraEditor in 20.6 seconds (466 generated files written)`，编译即通过。
完整机理与诊断方法已写进 [PROGRESS.md §1 坑 6](PROGRESS.md)。

> **另一个注意点：这次改动 Live Coding 接不住。** `FRecoilRuntimeState` 是带 `UPROPERTY` 的反射类型，
> 新增 `AimCompensationPitch` 成员**改变了类布局**，Live Coding 无法热重载（即便按 `Ctrl+Alt+F11` 也会要求重启）。
> 必须**关掉编辑器**做整包编译。这次也是先被
> `Unable to build while Live Coding is active` 挡住，关掉编辑器（PID 28372）才编成。

### 12.7 怎么在 PIE 里验证

1. 控制台 `Lyra.Recoil.Debug 1` 打开面板。
2. 关注 `Accum` 行的两个新字段：
   - `aimComp +X.XXX` —— 本梭压枪量，**鼠标向下拖时应该实时变大；停手就冻结**。
   - `CapV=X.XXX` —— 垂直钳制实际生效的上限，应该**随 `aimComp` 一起从 4.000 涨上去（硬顶 8.000）**。
3. 验收动作：连发并持续向下压枪 ——
   - ✅ 期望：`Accum Pitch` **能超过 4.000**（旧版永远卡在 4.000），镜头持续有上抬感。
   - ✅ 期望：`CapV` 稳态约 = `4.000 + aimComp`；松手不压枪时 `aimComp` 归零、`CapV` 回 4.000。
   - ❌ 若发现"压满时发涩/抖动"或"压枪后镜头飞得离谱" ⇒ 属于 [§12.2](#122-113-三条顾虑的逐条处理) 的回路残留，按 §12.8 调抵扣夹持量。

### 12.8 新增待拍板项

> **权威清单在 [PROGRESS.md §6「P12 带来的新待拍板」](PROGRESS.md)** —— 那里带「备选 / 影响面」两列，
> 是给大祥老师过目用的正式表格。本节只同步结论，不重复展开。

| # | 事项 | 我的默认选择 | 备选 |
| --- | --- | --- | --- |
| 36 | **抵扣量是否夹在 `1 × MaxVerticalKick`** | 夹（裸偏移硬顶 = `2 × MaxV` = 8.0°），给回正留安全阀 | 不夹（裸偏移无硬顶，极端压枪下切枪回正会甩镜头） |
| 37 | **抵扣基线取"本梭首发俯仰"** | 是 | 取"上一次休火后的稳定俯仰"，但需要额外的休火期状态 |
| 38 | **水平方向（Yaw）是否同样抵扣** | 不做（水平没有"飞太高"的问题） | 做，需要另加一个基线字段 |
| 39 | **实机手感确认：压满时会不会发涩/抖动**（§12.2 回路残留） | 需大祥老师本人在 PIE 里连发 + 持续下压确认，面板盯 `CapV` / `aimComp` | 若发涩 ⇒ 把抵扣量夹到 `0.5 × MaxV` |

> #36–#38 是**设计取舍**（要我改代码就动它们）；#39 不是取舍，是**必须实机确认**的验证项 ——
> 它是 §12.2 里唯一没能靠代码消除的残留（正反馈回路被 `1×MaxV` 夹住了，但"夹在哪里最舒服"只能靠手试）。


---

## 13. 追加修复（2026-09-20）：回正目标也减去本梭累计压枪量（P14）

### 13.0 一句话

回正目标从

```
T = RecoveryBase + (RecoveryPeak − RecoveryBase) × RecoilReturnRatio
```

改成

```
T = RecoveryBase + (RecoveryPeak − RecoveryBase) × RecoilReturnRatio − RecoveryCoverPitch
```

新增字段 `RecoveryCoverPitch`（本梭累计压枪量）。**默认 0 ⇒ 回正目标退化成旧公式，既有行为逐位不变。**

### 13.1 需求与两个已拍板的决定

原话：

> 后坐力回正的规则也需要修改一下 后坐力回正也需要减去玩家压枪的角度

我追问"减多少、减到负数怎么办"，得到的口径是：

> 你累计一下玩家的压枪量然后减去就好

据此定下两条（**都是我的问题、你的回答，不是我的选择**）：

| # | 待定项 | 你的口径 | 被否决的选项 |
| --- | --- | --- | --- |
| A | 抵扣量怎么取 | **累计**玩家的压枪量（不是"当前瞬间值"） | 读实时值 ⇒ 停火后松手会归零，见 §13.2 |
| B | 残留可以为负吗 | **可以（字面减法）** —— 玩家压过头，镜头最终就低于起枪点 | 夹到 0 / 按比例抵扣 |

> **与 §12 的分工**：§12（P12）改的是**钳制边界**（"上限"），本节（P14）改的是**回正目标**（"最终停在哪"）。
> 两者都从 `AimCompensationPitch` 取输入，但服务于两个不同时间尺度 —— 这正是 §13.2 要新增字段的原因。

### 13.2 为什么必须新增一个字段，不能复用 `AimCompensationPitch`

两个消费者的**时间尺度不同**，复用一个字段必然出错：

| | `AimCompensationPitch`（P12 用） | `RecoveryCoverPitch`（P14 用） |
| --- | --- | --- |
| 服务对象 | **钳制边界** | **回正目标** |
| 语义 | 玩家"**正在**压多少" | 这一梭"**总共**压了多少" |
| 更新方式 | 每帧覆盖写（`SetAimCompensationPitch`） | 逐帧 `max()` 累积（单调不减） |
| 松手之后 | **立刻缩回 0**（`ControlRotation` 回升） | **冻结**在最大值 |
| 为什么必须这样 | 否则"压一下再松手"能永久骗到更高的硬顶 | 否则回正目标会在回正途中跳回旧值 ⇒ 非单调甩镜 |

**关键点**：回正发生在**停火之后**，而停火之后玩家**必然松手**。
若回正直接读 `AimCompensationPitch`，它会在回正进行到一半时一路缩回 0，
回正目标随之向上跳 —— 玩家看到的是镜头先压下去、再自己弹回来，**这次改动等于白做**。

因此 `RecoveryCoverPitch` 必须是**累积量**：

- `Advance()` 末尾：`RecoveryCoverPitch = max(RecoveryCoverPitch, AimCompensationPitch)` —— 单调不减；
- 停火后 `AimCompensationPitch → 0`，而 `RecoveryCoverPitch` 保持不变（"自然冻结"）；
- 新一梭（`ApplyShot` 里 `State == Idle` 分支）/ `Reset()` 清零。

### 13.3 代码改动清单

| 文件 | 位置 | 改动 |
| --- | --- | --- |
| `LyraRecoilState.h` | L298–299 | **新增** `UPROPERTY float RecoveryCoverPitch = 0.0f;`（**默认 0 ⇒ 旧行为**） |
| `LyraRecoilState.cpp` | L40–42 | `ApplyRecoveryStep`：`TargetPitch` 末尾 `− RecoilState.RecoveryCoverPitch` |
| `LyraRecoilState.cpp` | L166–167 | `ComputeStageTarget`：`SteadyEndPitch` 末尾 `− State.RecoveryCoverPitch` |
| `LyraRecoilState.cpp` | L845–846 | 长帧保护落稳态：`SteadyPitch` 末尾 `− RecoveryCoverPitch` |
| `LyraRecoilState.cpp` | L433 | `Reset()`：清零 `RecoveryCoverPitch` |
| `LyraRecoilState.cpp` | L639–640 | `ApplyShot()` 新一梭分支：清零 `RecoveryCoverPitch` |
| `LyraRecoilState.cpp` | L886–890 | `Advance()` 末尾 `switch (State)` **之前**：`RecoveryCoverPitch = max(RecoveryCoverPitch, AimCompensationPitch)` |
| `LyraRecoilDebug.cpp` | L238 | 面板 Accum 行再加一列 `covSum`（累计抵扣量），便于和 `aimComp` 对照 |

**水平方向（Yaw）不抵扣** —— 与 §12.3 同一条理由：水平没有"飞太高"的问题，
`RecoveryCoverPitch` 只有俯仰一个分量。

**累积点为什么放在 `Advance()` 里而不是 `ApplyShot()` 里：**
玩家压枪是**连续**动作，发生在两发之间。若只在开火瞬间采样，30 发里只会取到 30 个点，
且每发之间镜头已经被后坐力推走 —— 采到的不是"玩家的压枪量"。
`Advance()` 每帧跑一次，才是真实节奏；放在 `switch` **之前**是为了保证
"本帧进入回正"时用的就是本帧最新采到的累计值。

### 13.4 ⚠️ 一个必须记下来的坑：减法被分号截断，**能编译、静默失效**

首版落地时，三处改动是这种形态（`p14_edit*.py` 的产物）：

```cpp
const float TargetPitch = RecoilState.RecoveryBasePitch
    + (RecoilState.RecoveryPeakPitch - RecoilState.RecoveryBasePitch) * Profile.RecoilReturnRatio;  // ← 分号在这里
    - RecoilState.RecoveryCoverPitch;                                                                // ← 变成独立语句
```

**`- x;` 是一条合法的表达式语句**（取负、丢弃结果），所以：

- 编译 **`Result: Succeeded`，0 error / 0 warning**；
- 减法 **完全没有生效**；
- 只有跑仿真/实机才会发现"怎么没变"。

受影响的正是 `ApplyRecoveryStep`（L41）、`ComputeStageTarget`（L166）、长帧保护（L845）三处，
在本次修复的第二轮里才改对（把分号挪到下一行末尾）。

**自查命令**（改完这类多行表达式后一定跑一次）：

```bash
grep -n "RecoilReturnRatio;$" Source/LyraGame/Weapons/Recoil/LyraRecoilState.cpp
# 期望：只剩 TargetYaw / SteadyEndYaw 那两行；不该出现 *Pitch 的行
```

### 13.5 纯数值仿真证据

脚本 `sim11.py` / `sim12.py` / `sim15.py`，状态机与 `LyraRecoilState.cpp` 逐行对应
（含 `InterpBasePitch = AccumulatedPitch`、`RecoveryBasePitch = InterpBasePitch`、
`SubStepAccumulator` / `MaxSubStepsPerAdvance`、以及 `RecoveryDelay` 触发的**中途中止回正**）。
口径：`DA_Recoil_Rifle_S` 参数（`MaxVerticalKick = 4.0`、`RecoilReturnRatio = 0.15`、
`ReboundRatio = 0.72`、`RecoveryDelay = 0.12`、`RecoveryTime = 0.30`），
30 发 @0.12s（≈ `07_TuningRecipe.md` §5.4 的射速档）、60fps、再跑 2.5s 让回正走完。

#### 13.5.1 `Interpolated`（`DA_Recoil_Rifle_S`，槽位 0 —— 你实际拿的那把）

| cover | P12 峰值 | P12 `T_raw` | P12 `T_net` | P14 峰值 | P14 `T_raw` | P14 `T_net` | Δ`T_raw` | 期望 `−cover` | 偏差 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0.0 | 4.0446 | 4.045 | 4.045 | 4.0446 | 4.045 | 4.045 | 0.000 | −0.000 | **0** |
| 0.5 | 4.5446 | 4.545 | 4.045 | 4.5000 | 4.045 | 3.545 | −0.500 | −0.500 | **0** |
| 1.0 | 5.0446 | 5.045 | 4.045 | 5.0000 | 4.045 | 3.045 | −1.000 | −1.000 | **0** |
| 2.0 | 5.5795 | 5.327 | 3.327 | 5.0462 | 2.794 | 0.794 | −2.533 | −2.000 | −0.533 |
| 3.0 | 5.5795 | 5.327 | 2.327 | 4.7795 | 1.527 | −1.473 | −3.800 | −3.000 | −0.800 |
| 4.0 | 5.5795 | 5.327 | 1.327 | 4.5128 | 0.260 | −3.740 | −5.067 | −4.000 | −1.067 |
| 6.0 | 5.5795 | 5.327 | −0.673 | 3.9795 | −2.273 | −8.273 | −7.600 | −6.000 | −1.600 |

- `peak` = 裸累加偏移峰值；`T_raw` = 回正结束后的裸残留；`T_net = T_raw − cover` = 镜头相对起枪点的净残留。
- **`cover = 0` 逐位相同** ⇒ 零回归（并被 37 个用例 + Golden md5 独立坐实，见 §13.6）。
- `T_net < 0` 是**设计上允许**的（§13.1 决定 B）：玩家压过头，镜头最终低于起枪点。

#### 13.5.2 `InstantWrite`（弹道参数借用 `Rifle_S`，只换回正口径，便于同参对照）

`InstantWrite` 下 `RecoveryBase ≡ 0`，回正目标退化成 `Peak × RecoilReturnRatio − cover`：

| cover | P12 峰值 | P12 `T_raw` | P12 `T_net` | P14 峰值 | P14 `T_raw` | P14 `T_net` | Δ`T_raw` | 期望 `−cover` | 偏差 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0.0 | 4.0000 | 0.6000 | 0.6000 | 4.0000 | 0.6000 | 0.6000 | 0.0000 | −0.000 | **0** |
| 0.5 | 4.5000 | 0.6750 | 0.1750 | 4.5000 | 0.1750 | −0.3250 | −0.5000 | −0.500 | **0** |
| 1.0 | 5.0000 | 0.7500 | −0.2500 | 5.0000 | −0.2500 | −1.2500 | −1.0000 | −1.000 | **0** |
| 2.0 | 6.0000 | 0.9000 | −1.1000 | 6.0000 | −1.1000 | −3.1000 | −2.0000 | −2.000 | **0** |
| 3.0 | 7.0000 | 1.0500 | −1.9500 | 6.3100 | −2.0535 | −5.0535 | −3.1035 | −3.000 | −0.1035 |
| 4.0 | 7.0382 | 1.0557 | −2.9443 | 6.0673 | −3.0899 | −7.0899 | −4.1456 | −4.000 | −0.1456 |
| 6.0 | 7.0382 | 1.0557 | −4.9443 | 5.5819 | −5.1627 | −11.1627 | −6.2184 | −6.000 | −0.2184 |

`cover ≥ 3` 后出现 −0.10 ~ −0.22 的小偏差：此时 P14 的累加器整体被压低，
**P12 的钳制边界（`MaxV + cover`）反倒不再参与**（`6.31 < 7.0`），
两个口径的钳制事件数不同 ⇒ 峰值不同。属可接受的二阶效应。

#### 13.5.3 偏差溯源：`Interpolated` 的抵扣为什么会「叠加」

`Interpolated` 表的偏差（−0.533 / −0.800 / −1.067 / −1.600）不是 bug，来源已经定位：

1. `Rifle_S` 的射速间隔 **0.12s** 与 `RecoveryDelay` **0.12s 相等**
   （`07_TuningRecipe.md` §5.4 / `09_SingleShotCurveGap.md`）。
2. 于是 30 发里触发了 **7 次「中途中止回正」**（`Advance()` 里
   `State == Accumulating && TimeSinceLastFire > RecoveryDelay`）——
   这不是边角情况，对这把枪是**常态**。
3. 每一次这样的回正都会扣掉一遍 `cover`；而
   `ApplyShot` 里 `InterpBasePitch = AccumulatedPitch` ⇒ **被压低的值成为下一发的基底** ⇒ 逐次放大。
4. 验证：把"每发 `Drop` 阶段也减"这条关掉，结果**完全相同**（`sim12.py` 的 `FULL` vs `REC` 两列逐位一致）
   —— 说明叠加**只**来自这条基底反馈链，与 `ComputeStageTarget` 那一处无关
   （快速连发时 `Lift 0.045 + Rebound 0.030 + Settle 0.12 = 0.195s > 0.12s`，`Drop` 阶段根本走不到）。

> **要不要收敛它，是个设计问题，不是 bug。** 见 §13.8 的 #48：
> 若你希望抵扣严格线性（`Δ = −cover`），做法是让**中途中止回正不抵扣**，
> 只在停火后的最终回正抵扣一次。届时 `T_raw = P12 的 T_raw − cover`，
> 即 `cover = 4` 时是 `5.327 − 4 = 1.327`（现在是 `0.260`）。要改我再出表。

#### 13.5.4 顺带校准一处陈旧数字（§12.5 的 `5.1506`）

§12.5 写「连发自然峰值 **5.1506°**」—— 那是用 `sim8.py` 算的，
而 `sim8` 的 `rec()` 用的是 `峰值 × RecoilReturnRatio`（`base` 从未赋值），
即 **P10 之前**的回正公式。P10 把回正锚到基底之后，自然峰值变高：

| 口径 | 无钳制自然峰值 |
| --- | --- |
| `sim8`（pre-P10 公式） | 5.1506° |
| **`sim14`（P10 基底锚定 + P12 钳制，= 当前代码）** | **5.5795°** |

结论方向不变（`cover ≥ 2` 时 `MaxV + cover ≥ 6.0 > 5.5795`，钳制确实不参与），
但**数字应以 5.5795 为准**。§12.5 的表格保留原样作为当时的记录，此处不覆盖。

### 13.6 编译与回归（✅ 已执行，全绿）

| 项 | 结果 |
| --- | --- |
| 编译 `LyraEditor Win64 Development` | ✅ **`Result: Succeeded`，0 error / 0 warning**，25 actions，76.98s |
| `Lyra.Recoil` 自动化测试 | ✅ **37/37 全绿**（`LogAutomationCommandLine: ...Automation Test Queue Empty 37 tests performed.`，`Result={Fail}` = 0） |
| 关键用例 | ✅ `State.RecoverySteadyState`（P10 基底口径）、`State.Clamp`（P12 抵扣）都在，且全绿 |
| Golden 基准（5 份 JSON） | ✅ **md5 逐位未变**（跑测试前后一致） |
| CSV 契约 | ✅ 未改动（P14 不动列） |

**为什么 37 个用例能全绿：** 现有用例喂的 `cover` 恒为 0（夹具不设 `AimCompensationPitch`），
`RecoveryCoverPitch` 因此恒为 0，公式逐位退化成旧式。
**这不是"测试覆盖了 P14"，而是"测试证明了 P14 的零回归"** —— P14 的数值行为目前**只有仿真证据**，
实机确认要靠 §13.7。

> **构建踩坑（本次花了很久，已修）**：构建一直报
> `UbaSessionServer - ERROR opening file C:\ProgramData\Epic\UnrealBuildAccelerator\memgroups for write ... (Access is denied.)`
> → `Result: Failed (OtherCompilationError)`，而**实际 0 编译错误**。
> `-NoUBA` 挡不住（UE 5.8 的 `ExecutorFactory` 无论如何都构造 `UBAExecutor`，
> `-NoUBA` 只是 `Config.bAllowDetour = false`）。
> 解法两条：**① `-UBARootDir="E:\TPSGunsDemo\Saved\UBACache"` 把 UBA 存储搬离 `C:\ProgramData`；
> ② 构建要从 Bash 侧发起（宿主沙箱对该路径放行）。** 已写进 `PROGRESS.md` 坑 1 与
> `Tools/build.ps1`（新增 `-UBARootDir` 参数）。

### 13.7 怎么在 PIE 里验证

1. 控制台 `Lyra.Recoil.Debug 1` 打开面板，看 `Accum` 行现在有三列：
   `aimComp +X.XXX`（实时压枪量）、`covSum X.XXX`（本梭累计抵扣量）、`CapV=X.XXX`。
2. **验收动作 A（单梭）**：拿 `DA_Recoil_Rifle_S`（槽位 0，`Interpolated`），
   连发并**持续向下压枪**，松手等回正走完。
   - ✅ 期望：镜头最终停在比"没压枪时"更低的位置（`covSum` 越大，停得越低）。
   - ✅ 期望：`covSum` 在压枪期间单调不减，**松手后不再下降**（这就是 §13.2 的"冻结"）。
   - ❌ 若 `covSum` 松手后跟着 `aimComp` 一起掉回 0 ⇒ 累积点接错了。
3. **验收动作 B（负残留）**：故意压过头（`covSum` 明显大于 `Accum Pitch` 峰值）。
   - ✅ 期望：回正结束后镜头**低于起枪点** —— 这是 §13.1 决定 B 的字面减法，属预期。
   - ❌ 若觉得"沉过头"，那就是 §13.8 的 #49 要讨论的。
4. **验收动作 C（零回归）**：换 `DA_Recoil_Rifle_7`（槽位 1，`InstantWrite`）**完全不压枪**。
   - ✅ 期望：手感与 P12 完全一致（`covSum` 恒 0）。
5. 打 `Lyra.Recoil.Dump 1` 导 CSV：`AccumulatedPitch` 列就是 `T_raw` 口径，
   可与 §13.5 的表逐发对（注意 CSV 只记逻辑偏移，不含显示层补间）。

### 13.8 新增待拍板项

> 权威清单在 [PROGRESS.md §6](PROGRESS.md)（那里带「备选 / 影响面」两列）。

| # | 事项 | 我的默认选择 | 备选 / 影响面 |
| --- | --- | --- | --- |
| 48 | **中途中止回正造成的抵扣叠加要不要收敛**（§13.5.3） | **先保留现状**（字面、口径最直白） | 收敛 ⇒ "中途回正不抵扣、只最终回正抵扣一次"，`Δ` 严格 = `−cover`；`cover=4` 时残留从 `0.260` 变 `1.327`。需要新增一个"本梭已抵扣"标志位 |
| 49 | **压过头（`T_net < 0`）的手感底线** | 不设下限（§13.1 决定 B：允许负残留） | 设下限如 `−0.5 × MaxV` ⇒ 需再引入一个夹持常量；实机若"沉得慌"就回到这条 |

> #48 只影响**中途中止回正频繁**的枪（`RecoveryDelay ≤ 射速间隔`，典型就是 `Rifle_S`）。
> `InstantWrite` 的枪偏差 ≤ 0.22°（§13.5.2），可以不折腾。
> （编号接在 P13 的 #47 之后，避免与 #40–#47 撞号。）

---

_本文档由祥子整理，2026-09-20。修复范围：`LyraRecoilState.h/.cpp`（`Interpolated` 连发累积）+ §12 压枪抵扣（钳制）+ §13 回正抵扣。_
_根因一句话：回弹/回正锚在绝对峰值 → 连发几何衰减；修复：锚在「基底 + 本发幅度」，两模式在 `InstantWrite` 下逐位等价。_
_P14 一句话：回正目标再减掉「本梭累计压枪量」（`RecoveryCoverPitch`，单调累积、停火冻结），默认 0 ⇒ 零回归。_
_相关：[10_SingleShotInterpolation.md](10_SingleShotInterpolation.md)（模型）、[07_TuningRecipe.md](07_TuningRecipe.md)（数值）、云端 `TPS_Recoil_Impl_v2.1` §6.4/§7.1/§7.5。_
