# P4 姿态倍率验收数值表

> 数据来源：`Lyra.Recoil.Pose.MultiplierRatios` 测试运行时用 `AddInfo` 打到日志里的**实测值**，
> 不是手填的期望值。复跑命令见文末。

| 项 | 值 |
| --- | --- |
| 源资产 | `/Game/Weapons/Recoil/DA_Recoil_Rifle` |
| 每姿态发数 | 10 发（不推进时间，纯累积） |
| 峰值上限 | `MaxVerticalKick = 8.0`（实测位移 2.97 ~ 4.45，**未触发 Clamp**，所以比值是精确线性的） |
| 容差 | 1e-3 |
| 采集时间 | 2026-09-17 |

---

## 1. 姿态倍率实测表

四种"姿态"的定义：站/蹲/空中是三个互斥姿态；**瞄准与姿态正交**，作为第四种组合单独列（`AimingAlpha = 1`，即完全进入稳定瞄准镜头）。

| # | 姿态 | 资产配置倍率 | 期望比值<br>(相对站姿) | 实测累计垂直位移<br>AccumulatedPitch | 实测累计水平位移<br>AccumulatedYaw | 实测垂直比值 | 实测水平比值 | 判定 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | **Standing**（基准） | `PoseMultiplier_Standing` = **1.0000** | 1.0000 | **2.9661** | **0.2181** | 1.0000 | 1.0000 | ✅ |
| 2 | **Crouching** | `PoseMultiplier_Crouching` = **0.8000** | 0.8000 | **2.3729** | **0.1745** | 0.8000 | 0.8000 | ✅ |
| 3 | **JumpingOrFalling** | `PoseMultiplier_JumpingOrFalling` = **1.5000** | 1.5000 | **4.4492** | **0.3272** | 1.5000 | 1.5000 | ✅ |
| 4 | **Standing + Aiming(α=1)** | `PoseMultiplier_Aiming` = **0.7500** | 0.7500 | **2.2246** | **0.1636** | 0.7500 | 0.7500 | ✅ |

### 手工复核（差值应为 0）

| 校验 | 算式 | 结果 |
| --- | --- | --- |
| 蹲伏 | `2.9661 × 0.8 = 2.37288` vs 实测 `2.3729` | Δ = 2e-5 ✅ |
| 空中 | `2.9661 × 1.5 = 4.44915` vs 实测 `4.4492` | Δ = 5e-5 ✅ |
| 瞄准 | `2.9661 × 0.75 = 2.224575` vs 实测 `2.2246` | Δ = 2.5e-5 ✅ |
| 水平同比例 | `0.2181 × 0.8 = 0.17448` vs 实测 `0.1745` | Δ = 2e-5 ✅ |

### 计划要求的定性断言

| 断言（计划 §P4 原文） | 实测 | 判定 |
| --- | --- | --- |
| 蹲姿位移 < 站姿位移（默认配置下） | 2.3729 < 2.9661 | ✅ |
| 各姿态结果比值 == 配置倍率比值（1e-3 容差） | 见上表，最大偏差 5e-5 | ✅ |

> 水平方向上 Rifle 的前 8 发走固定 Pattern、第 9–10 发进入伪随机游走
> （从 Golden 数据看，第 8、9 发的水平分量 0.033915 / 0.081601 明显大于前几发），
> 所以站姿基线是 `0.2181` 而不是"前 8 发的量级"。这不影响比值结论 —— 倍率是整体线性缩放。

---

## 2. 非线性恢复曲线实测

用三份只有 `RecoveryCurve` 不同的临时 Profile（其余参数完全一致，`RecoilReturnRatio = 0` 便于观察回正进程本身），
推进到 `RecoveryDelay + RecoveryTime × 0.5` 时读取 `RecoveryProgress`：

| 曲线形状 | `RecoveryCurve` 关键帧 | 曲线求值 @t=0.5 | **运行时** `RecoveryProgress` @半程 |
| --- | --- | --- | --- |
| **快回—慢回** | (0, 0) · (0.25, **0.65**) · (1, 1) | 0.7667 | **0.7667** |
| 线性（基准） | (0, 0) · (1, 1) | 0.5000 | **0.5000** |
| **慢回—快回** | (0, 0) · (0.25, **0.05**) · (1, 1) | 0.3667 | **0.3667** |

**结论**：
1. 两条非线性曲线的半程进度差 **0.4000**（0.7667 − 0.3667），远超 0.25 的判定阈值 → 手感差异在数值上明确可区分。
2. 运行时读到的进度与曲线求值**完全一致**（Δ = 0.0000），证明 `RecoveryCurve` 是真的接进了回正插值，而不是只在资产里躺着。
3. 三条曲线最终都能在 `RecoveryDelay + RecoveryTime` 内把偏移归零（`RecoilReturnRatio = 0` 时精确为 0）。

### 曲线求值的手工复核

- 快回—慢回 @0.5：落在 (0.25, 0.65) 与 (1, 1) 之间 → `0.65 + (0.5−0.25)/(1−0.25) × (1−0.65) = 0.65 + 0.3333×0.35 = 0.7667` ✅
- 慢回—快回 @0.5：`0.05 + 0.3333×(1−0.05) = 0.05 + 0.3167 = 0.3667` ✅

---

## 3. 复跑方式

```powershell
# 只跑姿态组
powershell -ExecutionPolicy Bypass -File "D:\TPSGunsDemo\TPSGunsDemo\Docs\Recoil\Tools\run-recoil-tests.ps1" -TestFilter "Lyra.Recoil.Pose"

# 从日志里捞实测值（本表就是这些行抄出来的）
Select-String -Path "D:\TPSGunsDemo\TPSGunsDemo\Saved\Logs\TPSGunsDemo.log" -Pattern "MEASURED|RecoveryProgress at" | ForEach-Object { $_.Line }
```

预期输出（当前基线）：

```
MEASURED Standing(aimAlpha=0.00) mult=1.0000 pitch=2.9661 yaw=0.2181
MEASURED Crouching(aimAlpha=0.00) mult=0.8000 pitch=2.3729 yaw=0.1745
MEASURED JumpingOrFalling(aimAlpha=0.00) mult=1.5000 pitch=4.4492 yaw=0.3272
MEASURED Standing(aimAlpha=1.00) mult=0.7500 pitch=2.2246 yaw=0.1636
RecoveryProgress at normalized t=0.5: fast=0.7667 linear=0.5000 slow=0.3667
Runtime RecoveryProgress at half time: fast=0.7667 linear=0.5000 slow=0.3667
```

> ⚠️ 改了 `DA_Recoil_Rifle` 的 `PoseMultiplier_*` 或 `RecoilPerShot_*` / `PatternPoints` 之后，
> 上表的实测值会变，需要重新跑一次并更新本表。

---

## 4. 待大祥老师确认

| # | 事项 |
| --- | --- |
| 1 | 计划 §P4 正文提到"复用 Lyra 已有的 Crouch / **Speed** / Jump 判定"，但 §P1 冻结的参数表里只有 `_Standing / _Crouching / _JumpingOrFalling` 三个（**没有** `_Moving`）。本表按三个姿态 + 瞄准组合实现，即"移动中"归入 Standing。**是否需要新增 `PoseMultiplier_Moving`？**（这属于改动 P1 冻结契约，需要你点头） |
| 2 | 姿态倍率是**瞬时切换**（不做 `FInterpTo` 平滑）。理由：带插值的话过渡期比值会被拉偏，1e-3 的比值断言不成立。代价是蹲下/起跳瞬间下一发手感会立刻变化。**是否接受？** 若要平滑需新增 `PoseTransitionRate` 参数。 |
| 3 | 空中优先级高于蹲伏（蹲着跳 → 用空中倍率 1.5 而不是蹲伏 0.8）。已在 `Lyra.Recoil.Pose.ResolvePriority` 里固化为断言。**是否符合预期？** |
| 4 | 本表由自动化测试输出，但计划 §P4 的手动验收要求"四种姿态各录一段相同操作，横向对比反弹幅度"。这一段仍需你在 PIE 里目视确认（前置：完成武器接线）。 |
