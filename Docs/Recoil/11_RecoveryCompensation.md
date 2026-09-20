# 11 · 回正扣减压枪量（Recovery Compensation）

| 项 | 值 |
| --- | --- |
| 项目 | `D:\TPSGunsDemo\TPSGunsDemo` |
| 实现主体 | `FRecoilRuntimeState`（`Source/LyraGame/Weapons/Recoil/LyraRecoilState.h/.cpp`） |
| 配置主体 | `ULyraRecoilProfile::bCompensationAwareRecovery` |
| 采样接入点 | `ULyraRangedWeaponInstance::SampleRecoilPlayerAim()` |
| 建立日期 | 2026-09-20 |
| 前置文档 | `04_PoseMatrix.md`（回正与姿态）、`10_SingleShotInterpolation.md`（单发模型） |

---

## 1. 一句话

**回正量要扣掉玩家压的枪。** 玩家往下压多少，回正就少回多少；压过头就停在最后一发的位置。

---

## 2. 问题现象（修改前的 bug）

后坐力的相机偏移只作用在**显示层 POV**（CameraModifier，见开发计划 §3.3 决策 1），
玩家压枪动的是 `ControlRotation` —— 这两笔账本来就是分开的。但回正只回偏移那一笔：

```
开枪 10 发  →  后坐力偏移顶到 +10°  → 准星比目标高 10°
玩家往下压 4°  →  ControlRotation 降 4°  → 准星被拉回来，重新压在目标上
停火回正     →  偏移从 +10° 回到 RecoilReturnRatio 决定的残留值
                 →  准星比目标低了约 4°：**玩家压的那 4° 被"还回去"了**
```

玩家必须再往上抬一次才能重新命中 —— 这就是"回正有明显 bug"的真身。
它不是算术写错了，而是**回正没有把玩家的操作算进去**。

---

## 3. 规则

### 3.1 公式

```
原本回正量 = 峰值 − 峰值 × RecoilReturnRatio
实际回正量 = clamp(原本回正量 − 压枪量, 0, 原本回正量)
回正终止值 = 峰值 − 实际回正量
```

展开后就是代码里那一行（`FRecoilRuntimeState::ComputeRecoveryTarget`）：

```
终止值 = clamp(峰值 × Ratio + 压枪量,
               min(峰值, 峰值 × Ratio),
               max(峰值, 峰值 × Ratio))
```

双端钳制各自的含义：

| 边界 | 什么时候撞到 | 含义 |
| --- | --- | --- |
| 上界 `峰值` | 压枪量 ≥ 原本回正量 | **"只回正到最后一发子弹射出的位置"**（回正量归零，偏移留在峰值） |
| 下界 `峰值 × Ratio` | 玩家顺着后坐力方向推 | 回正量最多就是原本那么多，**不会因为玩家推得更狠而回得更多** |

> 用 `min/max` 而不是写死 `0`：垂直轴峰值恒 ≥ 0，水平轴峰值可正可负，两种情形共用一条公式。

### 3.2 手算对照（垂直轴，Ratio = 0.25，峰值 5.0）

| 压枪量 | 原本回正量 | 实际回正量 | 终止值 | 说明 |
| --- | --- | --- | --- | --- |
| 0 | 3.75 | 3.75 | **1.25** | 与改动前**逐位一致**（既有行为） |
| 1.0 | 3.75 | 2.75 | **2.25** | 少回 1° |
| 3.75 | 3.75 | 0 | **5.00** | 回正量刚好归零 |
| 20.0 | 3.75 | 0 | **5.00** | 压过头 → 钳在峰值 |

### 3.3 两轴同规则

Pitch 与 Yaw **共用同一条公式**，只有"压枪量"的符号来源不同（见 §4.2）。

---

## 4. 压枪量怎么测

### 4.1 采样口径

以**本轮连发第一发的玩家瞄准**为基准，逐帧对 `ControlRotation` 做差：

```
压枪量Pitch = −NormalizeAxis(当前Pitch − 基准Pitch)
压枪量Yaw   = −NormalizeAxis(当前Yaw   − 基准Yaw)
```

- `NormalizeAxis` 处理绕圈（缺了它，玩家转半圈会被算成"压枪 358°"）。
- **取负**：往下压（Pitch 减小）→ 压枪量为正；往左拉（Yaw 减小）→ 压枪量为正。
  也就是"**玩家把准星朝后坐力的反方向拉了多少**"。

### 4.2 基准什么时候换

| 时机 | 动作 |
| --- | --- |
| `ApplyShot` 且状态为 `Idle`（= 新一轮连发第一发） | 基准 := 当前采样值；压枪量清零 |
| `ApplyShot` 且状态非 `Idle`（连发中 / 回正中被再次开火） | 基准**不变**（还是同一轮连发） |
| `Reset`（换枪 / 卸枪） | 基准、采样值、快照全部清零 |

### 4.3 冻结时机（重要）

**压枪量在"开始回正"那一刻冻结成快照**，回正全程只用快照：

| 模式 | 冻结点 |
| --- | --- |
| `InstantWrite` | `Accumulating → Recovering`（即停火超过 `RecoveryDelay` 的那一帧） |
| `Interpolated` | `Settle → Drop`（Drop 段就是回正段） |
| 长帧保护 | 时间被丢弃、直接跳到稳态残留之前 |

理由：回正目标是 `f(峰值, 压枪量)`。若回正途中还读实时值，玩家手指再动一下目标就会改向 ——
表现为回正在半路突然拐弯。冻结之后回正是一条确定曲线，可以被自动化测试逐点断言。

> 注意"冻结"发生在**停火延迟之后**：`RecoveryDelay` 之内玩家继续压的枪仍然算数。

### 4.4 谁把瞄准喂进来

`ULyraRangedWeaponInstance::SampleRecoilPlayerAim()` —— 每帧（`UpdateRecoil`）
以及每次开火前（`AddRecoil`）各调用一次。

**这是唯一一处让算法层知道"玩家往哪压了"的地方**，`FRecoilRuntimeState` 依旧不碰 UWorld、
只接受数值入参 —— 纯数值单测的隔离性没有被破坏。

- 非本地控制（远程玩家）直接跳过 → 压枪量恒为 0 = "没人压枪"的既有语义。
- 纯数值单测**不调用** `SamplePlayerAim` → 压枪量恒为 0 → **既有 30 个用例、3 份 Golden、
  CSV 契约一个都不用改**。

---

## 5. 配置项

| 参数 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `bCompensationAwareRecovery` | bool | `true` | 回正是否扣压枪量。分类：`Recoil → Recovery` |

- 关掉它 = 退回旧公式（`终止值 = 峰值 × Ratio`），用于 A/B 对照。
- **打开它不影响任何既有验收** —— 压枪量为 0 时两条路径逐位相同。
- 压枪量本身在开关关闭时**照常被记录**（调试面板依然能看数），只是不参与回正。

---

## 6. 代码地图

| 文件 | 改动 |
| --- | --- |
| `LyraRecoilState.h` | 新增 8 个运行时字段（`Player/RecoveryCompensationPitch/Yaw`、`AimPitch/YawAtBurstStart`、`SampledAimPitch/Yaw`）+ `SamplePlayerAim()` + `ComputeRecoveryTarget()` |
| `LyraRecoilState.cpp` | `FreezeCompensationForRecovery()`；`ApplyRecoveryStep` / `ComputeStageTarget(Drop)` / 长帧保护三处改用 `ComputeRecoveryTarget`；`ApplyShot` 换基准；`Reset` 清字段 |
| `LyraRecoilProfile.h` | 新增 `bCompensationAwareRecovery` |
| `LyraRangedWeaponInstance.h/.cpp` | 新增 `SampleRecoilPlayerAim()`，在 `UpdateRecoil` / `AddRecoil` 里调用 |
| `LyraRecoilDebug.cpp` | 屏幕面板新增一行：实时压枪量 / 冻结快照 / 当前瞄准 |
| `Tests/LyraRecoilTest.spec.cpp` | 新增 6 个 `Lyra.Recoil.Compensation.*` 用例 |

**刻意不动**：`FRecoilShotResult` 字段（CSV 7 列契约）、`ShotHistory`、3 份 Golden 数据。

---

## 7. 自动化测试（6 个新用例）

| 用例 | 断言 |
| --- | --- |
| `Lyra.Recoil.Compensation.ZeroInputMatchesBaseline` | 零输入时严格等于旧公式；开关默认为 true |
| `Lyra.Recoil.Compensation.RetainsPullDown` | 压 1° → 终止值 = 原本目标 + 1.0 |
| `Lyra.Recoil.Compensation.OverCompensationClampsToPeak` | 压 20° → 终止值钳在峰值，回正量恰好为 0 |
| `Lyra.Recoil.Compensation.YawRetainsDrag` | 水平轴同规则：往左压 0.5° → 水平终止值保留 0.5° |
| `Lyra.Recoil.Compensation.DisabledKeepsLegacy` | 关掉开关 → 退回旧公式（但压枪量照常记录） |
| `Lyra.Recoil.Compensation.FrozenAfterRecoveryStarts` | 进入回正后冻结；途中再压 30° 不影响落点 |
| `Lyra.Recoil.Compensation.InterpolatedDropConsistency` | 插值模式：Drop 段终点与收官值一致（无跳变） |

一键复跑：`Docs/Recoil/Tools/run-recoil-tests.ps1`（或编辑器内
`AutomationTestToolset.RunTestsByFilter("StartsWith:Lyra.Recoil")`）。

---

## 8. 已知副作用（需要观察）

**多轮连发时的累积。** 压枪量按"每轮连发"重新起算，但终止值是**累加**在偏移上的：

```
第 1 轮：峰值 1.4°，压枪量 1.4° → 终止值 1.4°（偏移停在 1.4°）
第 2 轮：从 1.4° 起跳，峰值 2.8°，压枪量 1.4° → 终止值 ≈ 1.4~2.8°
```

偏移会一轮一轮往上垒，直到撞到 `MaxVerticalKick` / `MaxHorizontalKick` 上限后停住（不再增长）。
观感上不会有"镜头越飘越远"—— 因为准星 = 显示层，玩家本来就是照着显示层瞄的，
偏移变化对玩家是隐形的。但 `ControlRotation` 会随之下沉（玩家需要一轮一轮多压一点）。

**如果实测下来觉得这么垒不舒服**，三个可选方向（都不需要推翻本方案）：

1. 把 `RecoilReturnRatio` 调小 —— 累加速度随之变慢；
2. 给偏移加一条缓慢的"零位衰减"（独立于回正，走时间常数）；
3. 关掉 `bCompensationAwareRecovery` 做对照，确认这个累加到底是不是可感知的问题。

---

## 9. 手动验收步骤

| 步骤 | 操作 | 预期 |
| --- | --- | --- |
| 1 | PIE，控制台 `Lyra.Recoil.Debug 1` | 面板多出 `PushComp / FrozenComp / AimNow` 一行 |
| 2 | 连发 12 发，**完全不压枪** | `PushComp` 恒为 ±0.000；松手后准星回到原来的位置（与改动前一样） |
| 3 | 连发 12 发，中途往下压一段 | `PushComp P` 随压枪增长为正；松手回正后**准星停在刚才贴着目标的位置**，不再往下滑 |
| 4 | 连发中持续猛压（压过头） | 回正量归零：准星停在最后一发的位置，不再继续回 |
| 5 | 连发中往左/右拉枪 | `PushComp Y` 随之变化；松手后准星落点包含这段位移 |
| 6 | 把 `bCompensationAwareRecovery` 关掉，重复步骤 3 | 准星重新出现"往下滑"的旧现象 → 证明这个开关确实是这条修复的总闸 |
| 7 | 回正途中继续压枪 | 落点**不变**（压枪量已冻结） |

---

_本文档由祥子整理，2026-09-20。实现见 §6，验收见 §7 / §9。_
