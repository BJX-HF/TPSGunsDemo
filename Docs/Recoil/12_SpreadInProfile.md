# 12 · 散布并入后坐力配置表（姿态-角度直接模型）

> **代号 P13**（编号说明：11 号文档已经占用了 P10 / P12 两个编号，本文按最大号往后顺延）。
> 2026-09-20 · 状态：**代码 + 文档已完成，⏳ 待编译 + 待 PIE 手测**
>
> 前置阅读：
> - [后坐力系统调试.html §11](后坐力系统调试.html#spread) —— 操作向（怎么调、怎么看、怎么排错）
> - [PROGRESS.md](PROGRESS.md) —— 总状态看板
>
> 本文回答的是**为什么这么设计**，以及**改了什么、为什么这么改、代价是什么**。

---

## 1. 一句话

**把散布的数值配置从武器实例蓝图（`B_WeaponInstance_*`）搬进后坐力配置资产（`DA_Recoil_*`），
并把「heat → 三曲线」的间接模型换成「姿态 → 直接配角度」的直接模型。**

- 参数形状参考 **DLC36** 的 `FWeaponFireParam` 散布族（`Stand/Move/Rush/Aim` 四套 + `StopFireRecoverScatterSpeed`）。
- 单位从 DLC36 的「面积 0~9」改成 **度（全锥角）** —— 「最大散布 2.0°」就填 2.0。
- 姿态**沿用 Lyra 原有的 `EPoseState` 三态**（站定 / 蹲伏 / 空中），不新增枚举、不改 `ResolvePoseState()`。
- 移动（站定 ↔ 跑动）不占独立姿态，用一组速度 ramp **倍率**插值。
- 瞄准仍是 `[0,1]` 混合权重 **倍率**。
- Lyra 原生 heat 模型**完整保留**，由 `bEnableProfileSpread` 开关控制，默认 **false**。

---

## 2. 为什么要改

### 2.1 旧模型的三个具体痛点

| # | 痛点 | 具体表现 |
| --- | --- | --- |
| 1 | **想配"最大散布 2°"要反推曲线** | `HeatToSpreadCurve` 的 X 是 heat、Y 是角度。你只能调端点，且端点还会被 `HeatToHeatPerShotCurve` / `HeatToCoolDownPerSecondCurve` 的 X 范围共同决定（`ComputeHeatRange()` 取三条曲线的并集）。改一个数要同时想三张图 |
| 2 | **换枪后第一发的散布不是基础值** | `OnEquipped()` 把 `CurrentHeat` 初始化到 `(MinHeat + MaxHeat) / 2` —— **范围中点**。于是"刚换完弹匣随手一枪"的散布是一个和基础值无关的数。这是"开局第一枪就是散的"的根因 |
| 3 | **配置散在两个地方** | 后坐力手感在 `DA_Recoil_*`，散布手感在同一把枪的 `B_WeaponInstance_*`。调一把枪的手感要开两个资产，它们之间没有任何引用关系，容易只改一边 |

### 2.2 目标

- 一个资产装完一把枪的"打起来什么样"：**后坐力 + 散布**。
- 每个参数都是**可直接读出的物理量**（度、度/秒、倍率），不需要经过中间变量解释。
- **零回归**：不勾开关时行为逐位等于改造前。

---

## 3. 模型定义

### 3.1 计算链

```
① 开火加热（每扣一次扳机）
     CurrentSpreadAngle += SpreadAddPerShot_<姿态>
     CurrentSpreadAngle  = Clamp(CurrentSpreadAngle, Base_<姿态>, Max_<姿态>)

② 停火回落（每帧，在 RecoilState.Advance() 之后）
     if (TimeSinceLastFire <= SpreadRecoveryDelay) → 不回落（仅钳制）
     else CurrentSpreadAngle -= SpreadRecoverRate_<姿态> × DeltaSeconds
     CurrentSpreadAngle = Clamp(CurrentSpreadAngle, Base_<姿态>, Max_<姿态>)

③ 结算最终锥角（弹道侧取值时）
     EffectiveCone = CurrentSpreadAngle
                   × SpreadAimingMultiplier(AimingAlpha)      ← Lerp(1, SpreadMultiplier_Aiming, α)
                   × SpreadMovementMultiplier                  ← 速度 ramp 的 FInterpTo 当前值

④ 喂给圆锥采样
     HalfSpreadAngleInRadians = DegreesToRadians(EffectiveCone × 0.5)
     BulletDir = VRandConeNormalDistribution(AimDirWithRecoil, HalfSpreadAngleInRadians, SpreadExponent)
```

### 3.2 单位口径（必须记住的三条）

1. **全锥角（直径角），度** —— 与 Lyra 原有的 `CurrentSpreadAngle` 语义一致，
   唯一的半角换算点在 `LyraGameplayAbility_RangedWeapon.cpp` 的 `ActualSpreadAngle * 0.5f`（L419）。
2. **`Base` 是地板不是起点** —— `CurrentSpreadAngle` 在稳态下恒等于当前姿态的 `Base`。
3. **姿态是瞬时的** —— 蹲/跳立刻换一组 `Base/Max/AddPerShot/RecoverRate`，不插值。

### 3.3 参数与 DLC36 的对照

| DLC36 `FWeaponFireParam` | 本文模型 | 说明 |
| --- | --- | --- |
| `StandScatteringArea` / `StandMaxScatteringArea` / `StandShootAddScatter` | `SpreadAngle_Standing` / `MaxSpreadAngle_Standing` / `SpreadAddPerShot_Standing` | 单位由"面积 0~9"改为"度" |
| `MoveScatteringArea` … | 无独立字段 | 走 `SpreadMultiplier_StandingStill` + 速度 ramp |
| `RushScatteringArea` … | 无独立字段 | 本项目无 Sprint 机制（DLC36 也是配了但代码里没实现） |
| `AimScatteringArea` … | 无独立字段 | 走 `SpreadMultiplier_Aiming`（混合权重，而非独立姿态） |
| `StopFireRecoverScatterSpeed` | `SpreadRecoverRate_<姿态>` | DLC36 全局一个 → 这里**每姿态一个** |
| `bUseShootScatterCurve` + `ShootScatterAddCurve` / `ShootScatterRecoverCurve` | **无对应** | 第一版不做曲线，见 §7 缺口 |
| — | `SpreadExponent` | 额外：锥内聚拢度（Lyra 原生就有） |

### 3.4 与旧模型字段的迁移对照

| 旧（`B_WeaponInstance_*` → `Spread`） | 新（`DA_Recoil_*` → `Recoil \| Spread`） |
| --- | --- |
| `HeatToSpreadCurve` | `SpreadAngle_*` / `MaxSpreadAngle_*`（每姿态一组） |
| `HeatToHeatPerShotCurve` | `SpreadAddPerShot_*` |
| `HeatToCoolDownPerSecondCurve` | `SpreadRecoverRate_*` |
| `SpreadRecoveryCooldownDelay` | `SpreadRecoveryDelay` |
| `SpreadExponent` | `SpreadExponent`（Profile 侧同名） |
| `SpreadAngleMultiplier_Aiming` | `SpreadMultiplier_Aiming` |
| `SpreadAngleMultiplier_StandingStill` + `StandingStillSpeedThreshold` + `StandingStillToMovingSpeedRange` + `TransitionRate_StandingStill` | `SpreadMultiplier_StandingStill` + `SpreadStandingStillSpeedThreshold` + `SpreadStandingStillToMovingRange` + `SpreadTransitionRate_StandingStill`（4 项原样保留） |
| `SpreadAngleMultiplier_Crouching` / `TransitionRate_Crouching` | `SpreadAngle_Crouching` / `MaxSpreadAngle_Crouching`（**倍率 → 角度**） |
| `SpreadAngleMultiplier_JumpingOrFalling` / `TransitionRate_JumpingOrFalling` | `SpreadAngle_JumpingOrFalling` / `MaxSpreadAngle_JumpingOrFalling` |
| `bAllowFirstShotAccuracy` | **无对应**（刻意删掉，见 §4.3） |

**旧字段全部保留**，分类改成 `Spread (deprecated)`，每个都加了 `DeprecationMessage`。
保留而不是删除的理由：① 删 `UPROPERTY` 会让既有武器蓝图的序列化数据静默丢失，回退路径就没了；
② "关掉开关 = 逐位一致"这条承诺只能靠旧字段还在来兑现。

---

## 4. 关键设计决策

### 4.1 为什么姿态用"三态 + 移动倍率"而不是抄 DLC36 的四姿态

| 方案 | 代价 |
| --- | --- |
| 抄 DLC36：新增 `Moving` / `Rushing` 姿态 | 要动 `EPoseState` 枚举 → 动 `FRecoilRuntimeState::ResolvePoseState()` → **后坐力侧的 P4 用例（姿态倍率比值）与 3 份 Golden 全部要重导** |
| **本文：三态 + 速度 ramp 倍率** | 只加字段，`ResolvePoseState()` 一行不动。移动精度仍然是连续的（比"移动 = 一个固定姿态"更细腻） |

选后者。收益是**后坐力侧零改动**，代价是"DLC36 的 Move/Rush 分档"这件事用一条 ramp 近似掉了 ——
对一把没有 Sprint 的枪来说，这个近似没有实际损失。

### 4.2 为什么蹲伏/空中不做过渡，站定/移动要做

- **蹲伏 / 空中**：与后坐力姿态倍率的口径**保持一致**（"每发按当下的姿态取值，不做插值"）。
  P4 那套"各姿态累计位移比值 == 配置倍率比值"的验收判据依赖这条；散布跟着同一口径，
  两个系统不会出现"后坐力认为你蹲了、散布认为你还没蹲"的错拍。
- **站定 / 移动**：这一路本来就**不是姿态切换**，而是同一个姿态内的连续量（速度）。
  沿用 Lyra 原生的 `FInterpTo` 语义，手感连续、不抖。

### 4.3 为什么不提供"首发绝对精准"开关

旧模型的 `bAllowFirstShotAccuracy` 做的事是：当三个倍率都到最小、heat 也到最小时，
把 `GetCalculatedSpreadAngleMultiplier()` **直接返回 0** —— 散布瞬间归零。

它的代价是**不可归因**：准星忽然缩到 0，你从数值上看不出是因为配了基础角 0，还是因为这个开关。
新模型改成"要 0 散布就把该姿态的 Base / AddPerShot 配成 0"，结果**永远是参数算出来的**，
面板上也永远能读到 `Cone: raw=0.000`。

### 4.4 为什么换枪后是"站立基础角"而不是"中点"

旧模型 `OnEquipped()` 取 `(MinHeat + MaxHeat) / 2`，是 heat 时代的产物 ——
heat 是"没有物理意义的中间变量"，只能取中点当作"一把陌生枪的默认状态"。
新模型里 `Base` 本身就是"静止时的散布角"，直接拿它当初始值即可，语义清晰且可解释。

实现上放在 `FRecoilRuntimeState::Reset()` 里（而不是 `OnEquipped()`），
这样纯数值单测也能覆盖到（`Lyra.Recoil.Spread.AccumulateAndClamp` 的第一条断言）。

### 4.5 为什么 CSV 需要 `PendingShotSpreadAngle` 这个中转字段

Lyra 的既有顺序是「**先按当前散布打出去 → 再 `AddSpread()` 加热**」
（`TraceBulletsInCartridge()` 在 `OnTargetDataReadyCallback()` 里的 `AddSpread()` 之前）。
而写 ShotHistory 的 `ApplyShot()` 发生在 `AddSpread()` **之后** ——
那一刻 `CurrentSpreadAngle` 已经是"下一发要用的值"。

所以弹道侧必须在发弹那一刻留一份快照（`NotifyShotSpreadUsed()`）。
不这样做，CSV 第 8 列会整体错位一发。这条在调试文档 §11.7 也写了一遍，
因为它是"把 CSV 读错"的最短路径。

### 4.6 为什么调试面板独立成 `Lyra.Recoil.SpreadDebug`

与 Roll 震屏（P8）的处理口径一致：**一个面板回答一个问题**。
后坐力面板回答"镜头被推到哪了"，散布面板回答"这一发的锥角是多少、为什么这么大"。
混在一屏会导致 10 行数字反而看不清。

同时主面板末尾**也**加了一行散布摘要 —— 调后坐力时经常要顺带确认"锥角变了没有"，
来回切 CVar 太烦。两者是"摘要 vs 详情"的关系，不是重复。

---

## 5. 改动清单

### 5.1 代码

| 文件 | 改动 |
| --- | --- |
| `Weapons/Recoil/LyraRecoilTypes.h` | **新增** `FRecoilSpreadParams`（Base / Max / AddPerShot / RecoverRate）；`FRecoilShotResult` **新增** `SpreadAngle` |
| `Weapons/Recoil/LyraRecoilProfile.h/.cpp` | **新增** `Recoil\|Spread` 组 20 个字段；**新增** `GetSpreadParams()` / `GetSpreadAimingMultiplier()` / `GetSpreadMovementMultiplierTarget()`；`ValidateProfile()` 在开关打开时校验这 20 个字段 |
| `Weapons/Recoil/LyraRecoilState.h/.cpp` | **新增** 8 个散布状态字段 + `ApplySpreadShot()` / `AdvanceSpread()` / `ResetSpread()` / `GetEffectiveSpreadAngle()` / `SetSpreadPlayerMultipliers()` / `SetPendingShotSpreadAngle()`；`Reset()` 里初始化；`ApplyShot()` 写入 `Result.SpreadAngle` |
| `Weapons/LyraRangedWeaponInstance.h/.cpp` | **新增** `UsesProfileSpread()`（两链路唯一分叉点）/ `NotifyShotSpreadUsed()` / `SpreadMovementMultiplier`；**改** `GetCalculatedSpreadAngle()` / `GetCalculatedSpreadAngleMultiplier()` / `GetSpreadExponent()`（从 inline 移到 .cpp，因为需要完整 Profile 类型）；**改** `AddSpread()` / `UpdateSpread()` / `OnEquipped()` / `UpdateMultipliers()` / `UpdateRecoil()` / `UpdateDebugVisualization()` / `Tick()`；**改** 15 个旧散布字段的分类与 DeprecationMessage |
| `Weapons/LyraGameplayAbility_RangedWeapon.cpp` | `TraceBulletsInCartridge()` 里**新增**一行：发弹前 `NotifyShotSpreadUsed()` 留快照（+6 行注释解释为什么） |
| `Weapons/Recoil/LyraRecoilDebug.h/.cpp` | **新增** CVar `Lyra.Recoil.SpreadDebug` + `DrawSpreadDebugPanel()`；**改** `DrawDebugPanel()`（加一行摘要）、`DumpShotHistoryToCsv()`（第 8 列） |
| `Tests/LyraRecoilSpreadTest.spec.cpp` | **新增**，7 个用例 |
| `Tests/LyraRecoilDumpTest.spec.cpp` | **改** `ExpectedHeader`（加 `SpreadAngle`）、`ExpectedColumnCount` 7 → 8、新增一条"未启用时该列恒为 0"的断言 |
| `LyraEditor/Commandlets/LyraRecoilAssetGenCommandlet.cpp` | **新增** `FProfileSpec::FSpreadSpec` + `ApplySpec()` 逐字段写入 + 5 把枪的首版数值 |

**未改动**：`LyraReticleWidgetBase.h/.cpp`（准星侧一行都没动 —— 因为
`GetCalculatedSpreadAngle()` / `GetCalculatedSpreadAngleMultiplier()` 已经在武器实例内部做好了分叉，
准星自然跟随）。

### 5.2 文档

| 文件 | 改动 |
| --- | --- |
| `Docs/Recoil/后坐力系统调试.html` | 第 11 节**整节重写**（11.1–11.10）；TOC / 页头版本行 / §1 速查表 / §7 排错表 / §8 代码索引 / 页脚同步 |
| `Docs/Recoil/12_SpreadInProfile.md` | **本文 · 新增** |
| `Docs/Recoil/PROGRESS.md` | 新增 P13 条目、测试数 30 → 37、CVar 表加 `SpreadDebug`、代码地图更新、待拍板清单追加 |
| `Docs/RecoilDevelopmentPlan.md` | §1.1 状态表补一行 |
| `Docs/Recoil/Acceptance/P13_验收请求.md` | **新增** |

---

## 6. 验证方式

### 6.1 自动化（7 个新用例，`Lyra.Recoil.Spread.*`）

| 用例 | 钉住什么 |
| --- | --- |
| `DisabledIsNoOp` | 新建资产默认关闭；关闭时 `ApplySpreadShot` / `AdvanceSpread` 逐位不动状态；`ShotHistory.SpreadAngle` 恒为 0 |
| `AccumulateAndClamp` | 手算序列 `1.0 → 1.5 → 2.0 → 2.5 → 3.0 → 3.0`；`Reset` 后从站立基础角起步而不是 0 |
| `Recover` | 停火延迟边界（`<=` 时不回落）；越过后按 `2.0 度/秒` 回落（3.0 → 2.5）；钳在基础角 1.0 |
| `PoseSwitch` | 蹲下立刻钳到蹲伏上限 1.0；起跳把锥角**抬到**新的基础角 2.5；空中 `recover=0` 不回落；落地回到 1.0 |
| `PlayerMultipliers` | 瞄准 `Lerp(1, 0.5, α)`；移动 ramp 四个边界；**带宽为 0 的退化区间不产生 NaN**；负倍率被夹成 0 |
| `ProfileParams` | 各姿态参数解析正确；`Max < Base` 被 `ValidateProfile` 报错且错误消息点名到字段；出口兜底把 `Max` 抬到 `>= Base`；负值夹成 0 |
| `ToggleBackIsClean` | 累加过之后关闭开关再 `Reset`，三个角度字段（Current / Base / Max）**全部归零**，且后续开火不再累加 |

### 6.2 回归面

- **既有 30 个用例**：`Lyra.Recoil.Dump.*` 的列契约从 7 列改 8 列（已同步断言）。
  其余 27 个用例与改造前**逐位一致** —— 因为它们的测试 Profile 都走 `bEnableProfileSpread = false`
  的默认路径，代码路径完全没变。
- **3 份 Golden 数据**：不受影响（Golden 由 `ComputeShotKick()` 直接算，与散布无关）。
- **武器实例上的旧字段**：分类改名 + 加 meta，数值与序列化不变。

### 6.3 与调试文档的三方一致性

| 层 | 文件 |
| --- | --- |
| 怎么用 | `后坐力系统调试.html` §11 |
| 为什么这么设计 | 本文 |
| 代码事实 | `LyraRecoilProfile.cpp` / `LyraRecoilState.cpp` / `LyraRangedWeaponInstance.cpp` / `LyraRecoilDebug.cpp` |

---

## 7. 已知缺口（没做就是没做）

| # | 缺口 | 影响 | 补的代价 |
| --- | --- | --- | --- |
| 1 | **没有散布曲线**（DLC36 的 `bUseShootScatterCurve` + `ShootScatterAddCurve` / `ShootScatterRecoverCurve`） | 做不出"越打加得越快"这类非线性 ramp；当前是恒定 `addPerShot` 线性累加 | 低。加 1 个 bool + 2 条 `FRuntimeFloatCurve`，`ApplySpreadShot` / `AdvanceSpread` 各加一次 `Eval`。**但会让 CSV 的"可手算复核"性质变弱**，所以先不做 |
| 2 | **姿态切换无过渡**（蹲/跳瞬切） | 站着打到 2.2° 时按下蹲，锥角会被立刻钳到蹲伏上限 1.6° | 中。要给"姿态角"也加 `FInterpTo`，但会破坏"每发按当下姿态取值"的口径，进而影响 P4 的比值判据。**建议先实机确认观感是否真的不能接受** |
| 3 | **没有散布总开关 CVar** | 想快速 A/B"关掉散布"得去资产上取消勾选 | 低。加一个 `Lyra.Recoil.Spread 0/1`（语义同 `Lyra.Recoil.Enable`） |
| 4 | **没有散布倍率 CVar** | 调量级只能改资产数值 | 低。加 `Lyra.Recoil.SpreadScale <f>` |
| 5 | **生成资产的首版数值未经实机验证** | 见 §8 | — |

缺口 3 / 4 建议**一起补**（一次改动两个 CVar，同一个 `ULyraRecoilDebug` 里加两行）。

---

## 8. 首版数值（可作调参起点，不是结论）

`LyraRecoilAssetGenCommandlet` 生成资产时写入的散布参数。单位：度（全锥角）/ 度每秒。

| 参数 | Rifle | Rifle_S | Rifle_7 | Pistol | Shotgun |
| --- | --- | --- | --- | --- | --- |
| `bEnableProfileSpread` | true | true | true | true | true |
| `SpreadAngle_Standing` | 0.35 | 0.32 | 0.38 | 0.50 | 3.50 |
| `MaxSpreadAngle_Standing` | 2.20 | 2.10 | 2.30 | 3.00 | 5.00 |
| `SpreadAddPerShot_Standing` | 0.28 | 0.26 | 0.30 | 0.45 | 0.60 |
| `SpreadRecoverRate_Standing` | 2.00 | 2.20 | 2.60 | 3.50 | 1.20 |
| `SpreadAngle_Crouching` | 0.25 | 0.24 | 0.28 | 0.35 | 3.00 |
| `MaxSpreadAngle_Crouching` | 1.60 | 1.55 | 1.70 | 2.20 | 4.50 |
| `SpreadAddPerShot_Crouching` | 0.22 | 0.21 | 0.24 | 0.35 | 0.50 |
| `SpreadRecoverRate_Crouching` | 2.40 | 2.50 | 2.80 | 4.00 | 1.40 |
| `SpreadAngle_JumpingOrFalling` | 2.50 | 2.50 | 2.50 | 3.00 | 5.00 |
| `MaxSpreadAngle_JumpingOrFalling` | 4.00 | 4.00 | 4.00 | 5.00 | 6.50 |
| `SpreadAddPerShot_JumpingOrFalling` | 0.35 | 0.35 | 0.35 | 0.50 | 0.60 |
| `SpreadRecoverRate_JumpingOrFalling` | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| `SpreadMultiplier_Aiming` | 0.60 | 0.60 | 0.60 | 0.55 | 0.85 |
| `SpreadMultiplier_StandingStill` | 0.50 | 0.50 | 0.50 | 0.45 | 0.90 |
| `SpreadStandingStillSpeedThreshold` | 80 | 80 | 80 | 80 | 80 |
| `SpreadStandingStillToMovingRange` | 20 | 20 | 20 | 20 | 20 |
| `SpreadTransitionRate_StandingStill` | 5.0 | 5.0 | 5.0 | 5.0 | 5.0 |
| `SpreadRecoveryDelay` | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |
| `SpreadExponent` | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |

### 推导示例（步枪站定）

- **第几发到顶**：`(Max − Base) / Add = (2.20 − 0.35) / 0.28 ≈ 6.6` → **第 7 发散布到顶**
- **打完一梭要多久收回**：假设打到顶 2.20°，`(2.20 − 0.35) / 2.00 = 0.925 秒`
- **跑动时的额外惩罚**：`move` 从 0.50（站定）滑到 1.0（速度 ≥ 100cm/s），
  即"边走边打"的锥角是站定的 **2 倍**
- **开镜收益**：`aim` 从 1.0 滑到 0.60 → **减 40%**

### 更稳妥的落地路径

1. **先别勾开关**（`bEnableProfileSpread = false`），保持旧手感。
2. 进 PIE，`Lyra.Recoil.SpreadDebug 1`，连发 + 移动 + 蹲 + 跳，**记录旧模型的锥角范围**
   （面板的 `Cone: raw=` 与 `effective=` 就是读数）。
3. 照实测值把新参数填一遍。
4. 打开开关，同一套动作再做一遍，对比两条曲线。
5. 觉得 OK 就把数值固化进 `LyraRecoilAssetGenCommandlet` 的 `Make*Spec()`（否则下次
   `gen-recoil-assets.ps1 -Force` 会把它们冲回首版值）。

---

## 9. 待拍板

| # | 事项 | 我的默认选择 | 备选 / 影响面 |
| --- | --- | --- | --- |
| 1 | 参数单位用「度（全锥角）」还是「度（半角）」 | **全锥角** —— 与 Lyra 原生 `CurrentSpreadAngle` 一致，且 GA 里有一行现成的 `× 0.5` | 半角 ⇒ 更贴近"我要 2° 的离散"这种直觉，但要改 `GetCalculatedSpreadAngle()` 的语义并同步准星，属破坏性改动 |
| 2 | 移动精度：ramp 倍率（本文）还是新增 `Moving` 姿态 | **ramp 倍率** —— 后坐力侧零改动 | 新增姿态 ⇒ P4 用例 + 3 份 Golden 全部重导 |
| 3 | 是否保留旧 heat 字段 | **保留 + 标废弃**（回退路径） | 删除 ⇒ 旧武器蓝图数据静默丢失，且失去零回归的对照物 |
| 4 | 是否要"首发绝对精准"开关 | **不提供** —— 要 0 散布就配 0 | 提供 ⇒ 回到"不可归因"的老问题 |
| 5 | 5 把枪的首版散布数值是否可用 | 需 PIE 实测后定稿（见 §8） | — |
| 6 | 是否补缺口 3 / 4（两个 CVar） | **建议补**（成本很低，调参体验差别很大） | — |
| 7 | 是否补缺口 1（散布曲线） | **先不做** —— 会削弱 CSV 的可手算复核性 | — |
| 8 | 是否补缺口 2（姿态切换过渡） | **先不做** —— 会破坏"每发按当下姿态取值"的口径 | — |
