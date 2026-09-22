# 11 · 回正扣减压枪量（Recovery Compensation）

| 项 | 值 |
| --- | --- |
| 项目 | `E:\TPSGunsDemo` |
| 实现主体 | `FRecoilRuntimeState`（`Source/LyraGame/Weapons/Recoil/LyraRecoilState.h/.cpp`） |
| 配置主体 | `ULyraRecoilProfile::bCompensationAwareRecovery` |
| 采样接入点 | `ULyraRangedWeaponInstance::SampleRecoilPlayerAim()` |
| 建立日期 | 2026-09-20 |
| **最近修订** | **2026-09-22（第三次定型）—— 公式改为 `终止值 = min(累计压枪量, 本轮峰值)`；压过头时保留超压角度** |
| 前置文档 | `04_PoseMatrix.md`（回正与姿态）、`10_SingleShotInterpolation.md`（单发模型）、`11_BurstAccumulationFix.md §13`（回正抵扣终稿） |
| 验证状态 | **构建 `Result: Succeeded`；`Lyra.Recoil` 45/45 全绿；5 份 Golden md5 逐位未变** |

---

> ## ★ 现行口径（2026-09-22 第三次拍板，以此为唯一权威）
>
> ```
> 回正终止值 = min(本梭累计压枪量, 本轮后坐力峰值)
> ```
>
> 峰值（Peak）只作为压枪抵扣的上限。不压枪 ⇒ 终止值 = 0；未压住 ⇒ 终止值 = P；压过头 ⇒ 终止值 = K。
>
> ### 为什么是这个式子：屏幕视角的账
>
> ```
> 屏幕 POV = ControlRotation（含玩家压枪） + 后坐力偏移
> ```
>
> 相机修改器 `ULyraCameraModifier_WeaponRecoil::ModifyCamera` 只做一件事：
> `InOutPOV.Rotation.Pitch += AppliedPitchDegrees;`（`AppliedPitchDegrees ← AccumulatedPitch`）。
> 玩家往下压 P 度 ⇒ `Ctrl = −P`；屏幕 = `偏移 − P`。
> 当 `P ≤ K`，把偏移收敛到 P ⇒ 屏幕 = 0 ⇒ **精确回到开枪前**。
> 当 `P > K`，偏移最多保留 K ⇒ 屏幕 = `K − P` ⇒ **保留玩家压过头的角度**。
>
> ### 场景对照（K = 峰值/枪抬多少，P = 玩家压多少）
>
> | 场景 | K | P | Ctrl 下沉 | 偏移终止值 | 屏幕 POV | 回正量 = K − P |
> | --- | --- | --- | --- | --- | --- | --- |
> | 完全没压枪 | 10° | 0° | 0° | **0°** | **0°（回零）** | 10°（回满） |
> | 压了一半 | 10° | 5° | 5° | **5°** | **0°** | **5°** ← 大祥老师原例 |
> | 压满 | 10° | 10° | 10° | **10°** | **0°** | 0° |
> | 压过头 | 10° | 11° | 11° | **10°** | **−1°** | 0°（不反向补偿，见 §10） |
>
> 两条不变量（自动化用例逐条锁死）：
> 1. `偏移终止值 == min(P, K)`
> 2. `屏幕终点 == min(P, K) − P`（未压住回零；压过头保留负角度）
>
> 验收用例：`Lyra.Recoil.Compensation.UserContractScenarios`。
>
> **⚠️ 已废弃的两版口径**（请勿再引用）：
> - `终止值 = 峰值 × RecoilReturnRatio` —— 残留比例缩放，字段已删除
> - `终止值 = 峰值 − 压枪量` —— 方向错误，实机表现为**看地板**，见 §8.2
>
> **已删除字段**（大祥老师 2026-09-21：「以后如果我没要求别做这种自以为是的设计」）：
> - `RecoilReturnRatio` —— 残留比例缩放
> - `RecoilCompensationMinResidualRatio` —— 残留地板
>
> **本文件 §3.2 与 §11 是 P11 时代的推导，仅作设计史保留，请勿当作现行行为。**

---

## 0. 修订记录

| 日期 | 改了什么 | 为什么 |
| --- | --- | --- |
| 2026-09-20 | 首版（P11）：Pitch / Yaw **两轴同规则**，回正量一律**减去**压枪量 | 修"玩家压的枪被还回去" |
| 2026-09-21 | **Yaw 摘出抵扣**：新增 `bCompensationAwareRecoveryYaw`（默认 `false`），水平轴退化为不抵扣 | 水平方向没有"压枪"，玩家水平鼠标是**转身追目标**；`MaxHorizontalKick` 只有 2.0°，门槛仅 **1.7°**，转身随时跨过 ⇒ 水平回正**长期恒为 0** |
| 2026-09-21 | 方向翻转 P11 → P14：`+ 压枪量` 改成 `− 累计压枪量` | 两者方向相反、同接会互相抵消。代码里 P11 的加法路径**已删除** |
| 2026-09-21 | 删 `RecoilReturnRatio` / `RecoilCompensationMinResidualRatio`，口径改为 `终止值 = 峰值 − 压枪量` | 去掉残留比例缩放，改成字面减法 |
| **2026-09-21（本次）** | **口径二次修正：`终止值 = 峰值 − 压枪量` → `终止值 = 本梭累计压枪量`（形参 `Peak` 移除）** | 上一版方向错误：屏幕 = `峰值 − 2 × 压枪量` ⇒ 压枪越认真越"看地板"。实机 trace 坐实，见 §8 |
| **2026-09-21（本次）** | **修 Bug A：`ComputeStageTarget` 的 Drop 段改读已冻结的 `RecoveryCompensationPitch`** | 上一版读 `bRecoveryCoverApplied ? 0 : RecoveryCoverPitch`，而该标志在 `Settle→Drop` 处已置 `true` ⇒ **整个 Drop 段**目标恒为 0，偏移冻结在钳制上限，直到收官帧一帧跳过去 |
| **2026-09-22（本次）** | **压过头规则：`终止值 = min(累计压枪量, 本轮峰值)`，形参 `Peak` 恢复** | `P≤K` 仍回到起枪角；`P>K` 不再向上补偿，保留 `K−P` 的超压角度（10° / 11° ⇒ −1°） |

> ## ⚠️ P11 已废弃（2026-09-21）
>
> **本文件 §3.2 与 §11 描述的公式是 P11（`终止值 = 峰值 × Ratio **+** 压枪量`），已被取代。**
>
> | | P11（历史） | P14（历史） | **现行** |
> | --- | --- | --- | --- |
> | 公式 | `峰值 × Ratio **+** 压枪量` | `峰值 × Ratio **−** 累计抵扣` | **`min(累计压枪量, 本轮峰值)`** |
> | 峰值参与 | 是 | 是 | **仅作为上限** |
> | 压枪量来源 | 实时值 `AimCompensationPitch` | 累计量 | **冻结的累计量 `RecoveryCompensationPitch`** |
> | 收敛 | 无 | `bRecoveryCoverApplied` | `bRecoveryCoverApplied` + Drop 段同源读取 |

> ✅ **Pitch 轴"认真压枪反而看地板"已于 2026-09-21 修复。**
> 两处根因（方向错 + Drop 段冻结）与实机 trace 证据见 §8。

---

## 1. 一句话

**未压住时，回正把后坐力偏移收敛到玩家压枪量，使屏幕回到开枪前；压过头时不反向补偿，保留超压角度。**
不压枪 ⇒ 偏移回满；`P≤K` ⇒ 偏移留在 P；`P>K` ⇒ 偏移最多留在 K。
（2026-09-21 起：这条**只作用于垂直轴**，水平轴默认不扣。）

---

## 2. 问题现象（最初要修的 bug）

后坐力的相机偏移只作用在**显示层 POV**（CameraModifier，见开发计划 §3.3 决策 1），
玩家压枪动的是 `ControlRotation` —— 这两笔账本来就是分开的。但回正只回偏移那一笔：

```
开枪 10 发   →  后坐力偏移顶到 +10°   →  准星比目标高 10°
玩家往下压 4° →  ControlRotation 降 4° →  准星被拉回来，重新压在目标上
停火回正      →  若不认这笔操作，偏移直接归 0 ⇒ 准星又低回 4°：玩家压的枪被"还回去"了
```

玩家必须再往上抬一次才能重新命中 —— 这就是"回正有明显 bug"的真身。
它不是算术写错了，而是**回正没有把玩家的操作算进去**。

---

## 3. 规则

### 3.1 公式（现行，2026-09-22 第三次拍板）

```cpp
// FRecoilRuntimeState::ComputeRecoveryTarget(const ULyraRecoilProfile& Profile,
//                                            float Peak, float Cover,
//                                            bool bApplyCover = true)
return (Profile.bCompensationAwareRecovery && bApplyCover)
    ? FMath::Sign(Peak) * FMath::Min(FMath::Max(Cover, 0.0f), FMath::Abs(Peak))
    : 0.0f;
```

本式**无 Ratio、无地板**；峰值仅用于限制抵扣量，避免压过头时反向补偿
（代码：`Source/LyraGame/Weapons/Recoil/LyraRecoilState.cpp` 的 `ComputeRecoveryTarget`）。

- 垂直轴恒传 `bApplyCover = true`（默认实参）。
- 水平轴传 `Profile.bCompensationAwareRecoveryYaw`（**默认 `false`**）⇒ 返回 0 ⇒ 偏移回满。
- 两把闸门**串联**：`bCompensationAwareRecovery && bApplyCover` 都为真才扣。

**稳态值也必须共用同一份实现。** 三处消费点（`ApplyRecoveryStep`、`ComputeStageTarget` 的
Drop 段、长帧保护）全部调用 `ComputeRecoveryTarget`，且**读同一个字段** `RecoveryCompensationPitch`
（`RecoveryCompensationYaw`）—— 这是 Bug A 的修复要点，见 §8.3。

---

### 3.2 【历史】P11 的公式推导（已废弃，仅作设计史）

> 以下 P11 推导**请勿当作现行行为**。

```
原本回正量 = 峰值 − 峰值 × RecoilReturnRatio
实际回正量 = clamp(原本回正量 − 压枪量, 0, 原本回正量)
回正终止值 = 峰值 − 实际回正量
```

展开后是 P11 时代代码里的那一行：

```
终止值 = clamp(峰值 × Ratio + 压枪量,
               min(峰值, 峰值 × Ratio),
               max(峰值, 峰值 × Ratio))
```

| 边界 | 什么时候撞到 | 含义 |
| --- | --- | --- |
| 上界 `峰值` | 压枪量 ≥ 原本回正量 | "只回正到最后一发子弹射出的位置" |
| 下界 `峰值 × Ratio` | 玩家顺着后坐力方向推 | 回正量最多就是原本那么多 |

> ⚠️ 方向说明：在 `clamp(峰值 × Ratio **+** 压枪量, ...)` 里，压枪量是**加到终止值上**的
> ⇒ 压枪的人镜头**停得更高**。这正是后来被推翻的原因之一。

### 3.3 两轴**不同**规则（2026-09-21 修订）

Pitch 与 Yaw **共用同一条公式**，但**只有 Pitch 默认参与抵扣**：

| 轴 | 抵扣 | 理由 |
| --- | --- | --- |
| Pitch | ✅ **默认开** | 垂直方向存在真实的"压枪"动作（对抗枪口上跳） |
| Yaw | ❌ **默认关**（`bCompensationAwareRecoveryYaw = false`） | 水平方向**没有**"压枪" —— 玩家的水平鼠标是**转身追目标** |

**为什么 Yaw 必须摘掉。** 采样口径对水平位移一视同仁，但它的物理含义完全不同：
垂直位移是"玩家主动往下拉"，水平位移是"玩家把枪口转向别处"。
而 `MaxHorizontalKick` 只有 2.0°（S 型），旧口径门槛 1.7° —— **"转身超过 1.7°"在实机里随时发生**，
一旦跨过水平回正量就是 0，且**长期如此**（不是偶发）。

打开 `bCompensationAwareRecoveryYaw` 可让水平轴也收敛到位移量（A/B 对照能力保留）。
压枪量本身**两轴照常记录**（调试面板仍能看数），开关只决定它是否参与回正。

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
- **口径边界（大祥老师 2026-09-21 确认）**：压枪**只计开火后的动作**。
  基准就是**开火那一帧**的瞄准角，不额外前移、也不做"开火前预压"的认定。

### 4.2 基准什么时候换

| 时机 | 动作 |
| --- | --- |
| `ApplyShot` 且状态为 `Idle`（= 新一轮连发第一发） | 基准 := 当前采样值；压枪量清零 |
| `ApplyShot` 且状态非 `Idle`（连发中 / 回正中被再次开火） | 基准**不变**（还是同一轮连发） |
| `Reset`（换枪 / 卸枪） | 基准、采样值、快照全部清零 |

### 4.3 冻结时机（重要）

**压枪量在"开始回正"那一刻冻结成快照**，回正全程只用快照：

| 模式 | 冻结点 | 冻结函数 |
| --- | --- | --- |
| `InstantWrite` | `Accumulating → Recovering`（停火超过 `RecoveryDelay` 的那一帧） | `FreezeCompensationForRecovery()` |
| `Interpolated` | `Settle → Drop`（Drop 段就是回正段） | 同上 |
| 长帧保护 | 时间被丢弃、直接跳到稳态残留之前 | 同上 |

冻结函数体：

```cpp
if (RecoilState.bRecoveryCoverApplied)      // 额度已用尽
{
    RecoilState.RecoveryCompensationPitch = 0.0f;   // 本次（中途）回正不再重复抵扣
    RecoilState.RecoveryCompensationYaw   = 0.0f;
    return;
}
RecoilState.RecoveryCompensationPitch = RecoilState.RecoveryCoverPitch;  // 累计量，非实时量
RecoilState.RecoveryCompensationYaw   = RecoilState.RecoveryCoverYaw;
RecoilState.bRecoveryCoverApplied     = true;
```

理由：
1. 回正目标是 `f(压枪量)`。若回正途中还读**实时**值，玩家手指再动一下目标就会改向 ——
   表现为回正在半路突然拐弯。冻结之后回正是一条确定曲线，可以被自动化测试逐点断言。
2. 用**累计量**而非实时量：停火后玩家必然松手，实时值会缩回 0。
3. `bRecoveryCoverApplied` 是**一梭一次的收敛**（大祥老师 2026-09-21 明确保留）：
   连发途中的中途回正抵扣过一遍后，后续（中途）回正不再重复扣，
   否则同一梭的压枪量会被反复消费，偏移被越扣越负。

> 注意"冻结"发生在**停火延迟之后**：`RecoveryDelay` 之内玩家继续压的枪仍然算数。

### 4.4 谁把瞄准喂进来

`ULyraRangedWeaponInstance::SampleRecoilPlayerAim()` —— 每帧（`UpdateRecoil`）
以及每次开火前（`AddRecoil`）各调用一次。

**这是唯一一处让算法层知道"玩家往哪压了"的地方**，`FRecoilRuntimeState` 依旧不碰 UWorld、
只接受数值入参 —— 纯数值单测的隔离性没有被破坏。

- 非本地控制（远程玩家）直接跳过 → 压枪量恒为 0 = "没人压枪"的既有语义。
- 纯数值单测**不调用** `SamplePlayerAim` → 压枪量恒为 0 → Golden / CSV 契约不受影响
  （本次改动后 5 份 Golden md5 逐位未变，见文首「验证状态」）。

---

## 5. 配置项

| 参数 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `bCompensationAwareRecovery` | bool | `true` | **总开关**：回正是否扣压枪量（两轴共用）。分类：`Recoil → Recovery` |
| `bCompensationAwareRecoveryYaw` | bool | **`false`** | **本轴开关**：水平轴是否**也**扣。默认关，理由见 §3.3 |

两个开关是**串联**的：只有 `bCompensationAwareRecovery && 本轴开关` 都为真，该轴才扣压枪量。

- **关掉总开关 = 偏移完全回满到 0**（终止值恒为 0）。注意这**不等于**任何历史公式 ——
  旧文档写的"退回 `终止值 = 峰值 × Ratio`"已随 `RecoilReturnRatio` 的删除而失效。
- **打开它不影响任何既有验收** —— 压枪量为 0 时两条路径逐位相同。
- 压枪量本身在开关关闭时**照常被记录**（调试面板依然能看数），只是不参与回正。
- 两个开关都是**资产上的 `EditAnywhere`**，改完 PIE 立即生效，**不需要重新编译**。

---

## 6. 代码地图

| 文件 | 改动 |
| --- | --- |
| `LyraRecoilState.h` | 新增运行时字段（`RecoveryCompensationPitch/Yaw`、`RecoveryCoverPitch/Yaw`、`AimPitch/YawAtBurstStart`、`SampledAimPitch/Yaw`、`bRecoveryCoverApplied`）+ `SamplePlayerAim()` + `ComputeRecoveryTarget()` |
| `LyraRecoilState.h` | `ComputeRecoveryTarget()` 保留 `bool bApplyCover = true` 本轴开关（当前为第 4 个参数） |
| `LyraRecoilState.h/.cpp` | **2026-09-22**：恢复形参 `Peak` 作为压枪量上限 —— `ComputeRecoveryTarget(Profile, Peak, Cover, bApplyCover)` |
| `LyraRecoilState.cpp` | `FreezeCompensationForRecovery()`；`ApplyRecoveryStep` / `ComputeStageTarget(Drop)` / 长帧保护三处改用 `ComputeRecoveryTarget`；`ApplyShot` 换基准；`Reset` 清字段 |
| `LyraRecoilState.cpp` | 三处调用点的 **Yaw 分支**传 `Profile.bCompensationAwareRecoveryYaw`；函数内改成"总开关 && 本轴开关"串联判定 |
| `LyraRecoilState.cpp` | **2026-09-21 本次（Bug A）**：`ComputeStageTarget` 的 Drop 段读数源由 `bRecoveryCoverApplied ? 0 : RecoveryCoverPitch` 改为 `State.RecoveryCompensationPitch` |
| `LyraRecoilProfile.h` | 新增 `bCompensationAwareRecovery`、`bCompensationAwareRecoveryYaw = false` |
| `LyraRangedWeaponInstance.h/.cpp` | 新增 `SampleRecoilPlayerAim()`，在 `UpdateRecoil` / `AddRecoil` 里调用 |
| `LyraRecoilDebug.cpp` | 屏幕面板新增一行：实时压枪量 / 冻结快照 / 当前瞄准 |
| `Tests/LyraRecoilTest.spec.cpp` | `Lyra.Recoil.Compensation.*` 共 8 个用例；新增压过头 10° / 11° ⇒ −1° 验收 |
| `Tests/LyraRecoilPoseTest.spec.cpp` | `Lyra.Recoil.Pose.RecoveryCurveShape` 稳态期望由 `5.0`（峰值）改为 `0.0`（回满） |

**刻意不动**：`FRecoilShotResult` 字段（CSV 7 列契约）、`ShotHistory`、5 份 Golden 数据。

---

## 7. 自动化测试（`Lyra.Recoil.Compensation.*`，8 个）

| 用例 | 断言 |
| --- | --- |
| `ZeroInputMatchesBaseline` | 零输入时 `AccumulatedPitch` 严格为 0，且回正量 = 峰值 |
| `RetainsPullDown` | 压 1° → 偏移终止值 = 1.0 |
| `OverCompensationPreservesOvershoot` | 峰值 10°、压 11° → 终止值 = 10°，屏幕停在 −1°，回正量为 0 |
| `YawRetainsDrag` | 默认不抵扣（水平终止值 = 0）；显式打开 `bCompensationAwareRecoveryYaw` 后终止值 = 位移量 |
| `DisabledKeepsLegacy` | 关掉总开关 → 终止值 = 0（偏移回满） |
| `FrozenAfterRecoveryStarts` | 进入回正后冻结；途中再压 30° 不影响落点 |
| `InterpolatedDropConsistency` | 插值模式：Drop 段终点与收官值**一致**（无跳变） |
| `UserContractScenarios` | 五场景参数化：① 终止值 == `min(P,K)` ② 屏幕 == `min(P,K)−P` ③ 回正量 == `max(K−P,0)` |

一键复跑：`Docs/Recoil/Tools/run-recoil-tests.ps1`（或编辑器内
`AutomationTestToolset.RunTestsByFilter("StartsWith:Lyra.Recoil")`）。

**2026-09-22 验证结果**：专项 `8/8`；完整 `Lyra.Recoil` 为 `45/45`，`Result={Fail}` = 0；`LyraEditor` 构建成功。

---

## 8. 两处根因与实机 trace 证据（2026-09-21）

### 8.1 抓到 trace 的方法

```
Lyra.Recoil.Trace 1        # CVar，定义在 LyraRangedWeaponInstance.cpp，日志类别 LogLyraRecoilWeapon
```

每帧打印：`push / Ctrl / POV / delta / aimBase / aimNow / pushComp / cover / coverUsed / applied / peak / acc`。

实机场景：`DA_Recoil_Rifle_S` 连发，`mode=Interpolated`，全量 1451 行。

### 8.2 Bug B —— 终止值方向错误（屏幕"看地板"）

上一版公式 `终止值 = 峰值 − 压枪量`，代入屏幕口径：

```
屏幕 = Ctrl + 偏移 = (−压枪量) + (峰值 − 压枪量) = 峰值 − 2 × 压枪量
```

**压枪越认真，屏幕越低** —— 在真实弹道上就是"看地板"。

| 量 | 实机数值 |
| --- | --- |
| 峰值 | 17.600° |
| 累计压枪量 | 13.650° |
| 玩家 Ctrl 下沉 | 13.650° |
| 旧公式终止值 = 17.600 − 13.650 | 3.950° |
| 旧公式屏幕 = −13.650 + 3.950 | **−9.700°（低于开枪前 9.7°）** |
| 新公式终止值 = 13.650 | 13.650° |
| 新公式屏幕 = −13.650 + 13.650 | **0.000°（精确回到开枪前）** ✅ |

### 8.3 Bug A —— Drop 段读数源用了被标志置零的字段

`ComputeStageTarget()`（决定插值链各阶段的目标值）里，Drop 段原本写的是：

```cpp
const float StageCoverPitch = State.bRecoveryCoverApplied ? 0.0f : State.RecoveryCoverPitch;
```

本意是"本函数可能在冻结之前被调用，那时读快照会拿到 0"。但实机时序恰好相反：
`FreezeCompensationForRecovery()` 在 `Settle→Drop` 切换处就把标志置成了 `true`，
于是**整个 Drop 段**每帧都推导出 `StageCoverPitch = 0` ⇒ 目标 = 不抵扣 ⇒
逻辑偏移冻结在钳制上限纹丝不动，直到收官那一帧 `ApplyRecoveryStep` 用快照算对，**一帧跳过去**。

| 观测 | 数值 |
| --- | --- |
| Drop 段持续帧数 | 23 帧，`acc` 恒为 **15.000** |
| 15.000 的来源 | `GetEffectiveVerticalKickLimit()` = `MaxVerticalKick(7.5) + min(AimCompensationPitch 13.65, 7.5)` = **15.0** |
| Drop 期间 `applied` | 恒为 1 |
| Drop 期间 `peak` | 显示 0.000 |
| 收官帧 `acc` | 3.950（旧公式值） |
| 屏幕跳变 | **+2.75 → −8.30** |

**修法**：Drop 段直接读 `State.RecoveryCompensationPitch`
（= `Freeze` 时锁定的本梭累计压枪量），与 `ApplyRecoveryStep` 完全同源
⇒ Drop 段平滑收敛，终点与收官值一致、不再跳。

### 8.4 顺带确认：压枪量链路是健康的

大祥老师当时怀疑"压枪量被第一发吃掉"。trace 显示这一梭里
`cover = 13.650` 与 `pushComp = 13.650` **完全一致且正确累计** —— 
本轮射击中**没有复现**该现象。真正的问题只在 §8.2 / §8.3 两处。

---

## 9. 手动验收步骤

| 步骤 | 操作 | 预期 |
| --- | --- | --- |
| 1 | PIE，控制台 `Lyra.Recoil.Debug 1` | 面板多出 `PushComp / FrozenComp / AimNow` 一行 |
| 2 | 连发 12 发，**完全不压枪** | `PushComp` 恒为 ±0.000；松手回正后**屏幕精确回到开枪前**（准星归位） |
| 3 | 连发 12 发，中途往下压一段 | `PushComp P` 随压枪增长为正；松手回正后**屏幕同样回到开枪前**，不再往下滑 |
| 4 | 连发中持续猛压（压过头） | 偏移终止值不超过峰值；屏幕保留超压量（峰值 10°、压 11° ⇒ −1°） |
| 5 | 连发中往左/右拉枪 | `PushComp Y` 照常变化（**仅记录**），松手后水平偏移回满到 0，**不含**这段位移 |
| 5b | 把 `bCompensationAwareRecoveryYaw` 勾上，重复步骤 5 | 水平终止值变成"含这段位移"→ 证明本轴开关确实有效 |
| 6 | 把 `bCompensationAwareRecovery` 关掉，重复步骤 3 | 偏移完全回满 ⇒ 玩家压的枪被"还回去"，旧现象重现 → 证明这是总闸 |
| 7 | 回正途中继续压枪 | 落点**不变**（压枪量已冻结） |
| 8 | 开 `Lyra.Recoil.Trace 1`，连发一次并压枪 | 尾段 `acc` **单调平滑收敛**到 `cover`，无"冻结若干帧后一帧跳变" |

---

## 10. 压过头规则（2026-09-22 已定型）

当 `P > K`（玩家压得比枪抬得还多）时，终止值夹在 K：

- 后坐力偏移不会超过本轮实际峰值，也不会在 Drop 段反向上升；
- 屏幕终点为 `Ctrl(−P) + 偏移(K) = K−P`；
- 示例：累计后坐力 10°、玩家压 11° ⇒ 最终屏幕为 **−1°**。

自动化用例 `Lyra.Recoil.Compensation.OverCompensationPreservesOvershoot` 与
`Lyra.Recoil.Compensation.UserContractScenarios` 已锁定该行为。

---

## 11. 【已结案】Pitch 轴的"归零"问题（2026-09-21 第一版修法，已被 §8 的二次修正取代）

### 11.1 当时的症状

旧公式 `终止值 = 峰值 × RecoilReturnRatio − 压枪量` 里，钳制上界是 `峰值 × Ratio`，
于是「压枪量 ≥ 峰值 × (1 − Ratio)」时终止值被压到 `峰值 × Ratio` 附近 ——
表现为**认真压枪后回正量显得严重缩水**，落点与玩家预期差一大截。
而"压枪量 ≈ 峰值"正是压枪动作的定义 ⇒ 认真压枪时必然归零。

### 11.2 实测数值（纯数值复现）

脚本 `recovery_diag.py`（严格照抄 `Interpolated` 路径），输出 `recovery_diag_out.txt`：

| 枪 | 峰值 | 旧 Ratio | 旧门槛 | 完美压枪时的压枪量 | 旧落点 |
| --- | --- | --- | --- | --- | --- |
| `DA_Recoil_Rifle_S` | 4.0° | 0.15 | 3.4° | 4.0° | 1.0°（≈ 峰值×0.25） |
| `DA_Recoil_Rifle_7` | 9.0° | 0.30 | 6.7° | 9.0° | 2.7°（≈ 峰值×0.30） |

### 11.3 修法演进

1. 第一版：加 `bCompensationAwareRecovery` 开关 + 删 `RecoilReturnRatio`，改为 `终止值 = 峰值 − 压枪量`
   （**后经实机验证方向错误，见 §8.2**）。
2. 第二版（历史）：`终止值 = 本梭累计压枪量`，并修掉 Drop 段冻结（§8.3）。
3. 第三版（现行）：`终止值 = min(累计压枪量, 本轮峰值)`，压过头时保留超压角度。

> 历史备选方案（**未采用**）：曾计划新增 `RecoveryCompensationMaxShare`（0~1）
> 给抵扣设"硬上限比例"，以保留一部分回正量。该方案与"压多少认多少"的期望冲突，已废弃。

---

_本文档由祥子整理，2026-09-20；2026-09-22 第三次定型（压过头时以峰值封顶）。
实现见 §6，验收见 §7 / §9，根因证据见 §8。_
