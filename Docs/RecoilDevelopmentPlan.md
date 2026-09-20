# TPS 后坐力系统开发计划（Recoil System Development Plan）

| 项 | 值 |
| --- | --- |
| 项目 | `D:\TPSGunsDemo\TPSGunsDemo` |
| 引擎 | UE 5.8.2（源码版，`D:\UE_5.8`） |
| 代码模块 | `LyraGame` / `LyraEditor` |
| 手感取向 | **数据驱动可调框架**（不绑定具体竞品手感，参数后期在编辑器调） |
| 网络策略 | **不做联机**（2026-09-17 决定，详见 §5「P6 — 已剔除」） |
| 交付范围 | 程序核心 + 调试与可视化工具链 |
| 本文档状态 | 已冻结，作为开发与验收的唯一基准（2026-09-17 修订：剔除 P6 联机同步阶段） |
| 建立日期 | 2026-09-17 |

---

## 1. 背景与现状

项目是 Lyra 派生的第三人称射击 Demo。武器层**只有扩散（Spread）模型，没有真正的后坐力（Recoil）系统**。

### 1.1 现状盘点

> ⚠️ **2026-09-20 更新**：下表是 **P0（2026-09-17）立档时的勘察结果**，保留作为历史基线。
> 其中与**散布（Spread）**相关的四行已被 **P13** 推翻 —— 散布的配置已从武器实例搬进
> `DA_Recoil_*`，模型换成「姿态-角度直接模型」。现行口径见
> [Recoil/12_SpreadInProfile.md](Recoil/12_SpreadInProfile.md) 与
> [Recoil/后坐力系统调试.html §11](Recoil/后坐力系统调试.html#spread)。

| 能力 | 现状 | 位置 |
| --- | --- | --- |
| 武器扩散/热量模型 | ✅ 已有 <br>**[P13] ⛔ 已废弃（标 `Spread (deprecated)`，仅作回退）** | `ULyraRangedWeaponInstance`：`HeatToSpreadCurve`、`HeatToHeatPerShotCurve`、`HeatToCoolDownPerSecondCurve`、`SpreadExponent` |
| 姿态/移动精度倍率 | ✅ 已有 <br>**[P13] ⛔ 已废弃（迁为 `Recoil\|Spread` 的姿态角度 + ramp 倍率）** | 同上：`SpreadAngleMultiplier_Aiming / _StandingStill / _Crouching / _JumpingOrFalling` |
| 首枪精准 | ✅ 已有 <br>**[P13] ⛔ 已废弃（新模型刻意不提供该开关）** | `bAllowFirstShotAccuracy` |
| **散布数值进资产** | **[P13] ✅ 新增** | `ULyraRecoilProfile` → `Recoil\|Spread` 组（20 个字段）+ `FRecoilRuntimeState` 散布通道 |
| 相机 Kick（视觉后坐力） | ❌ 缺失 | 无 CameraModifier 实现 |
| 弹道 Pattern（可复现弹道） | ❌ 缺失 | 发射方向只有 `VRandCone` 随机扩散 |
| 后坐力累加 / 恢复曲线 | ❌ 缺失 | 无状态机、无恢复逻辑 |
| 后坐力调试可视化 | ❌ 缺失 | 无 CVar、无 DebugDraw、无数据导出 |

### 1.2 关键接入点（P0 阶段需逐条核实到行号）

| 接入点 | 文件 | 说明 |
| --- | --- | --- |
| 发射方向计算 | `Source/LyraGame/Weapons/LyraGameplayAbility_RangedWeapon.cpp` | 弹道 Pattern 注入位置（现为 `VRandCone` 采样处） |
| 武器运行时状态 | `Source/LyraGame/Weapons/LyraRangedWeaponInstance.h/.cpp` | 后坐力状态的宿主位置，参照现有 `CurrentHeat` / `AddSpread()` 写法 |
| 相机链 | `Source/LyraGame/Camera/LyraPlayerCameraManager.h/.cpp`、`LyraCameraComponent` | CameraModifier 挂载点 |
| 武器状态同步 | `Source/LyraGame/Weapons/LyraWeaponStateComponent.h/.cpp` | （原 P6 联机参考，联机已剔除）但该组件实际是**每帧驱动武器 `Tick()` 的关键一环**，见 §3 |
| 调试命令 | `Source/LyraGame/Player/LyraCheatManager.h/.cpp` | exec 命令扩展入口 |
| 自动化测试范式 | `Source/LyraGame/Tests/MenuStartElimination.spec.cpp` | 新增测试用例的写法参考 |

---

## 2. 目标与范围

### 2.1 目标

做一套**数据驱动、可调、可自动化验证**的 TPS 后坐力系统：武器配置资产决定全部手感，运行时纯算法计算，相机与弹道两条输出链路各自独立，且全流程可用数值和 CSV 曲线验收，而不是靠"感觉对不对"。

### 2.2 范围内

- 后坐力数据资产（曲线、Pattern、倍率、上限）
- 运行时状态机与纯算法层（可脱离引擎单测）
- 相机 Kick 输出链（CameraModifier）
- 弹道 Pattern 输出链（发射方向注入）
- 后坐力恢复（延迟 + 曲线 + 回正比例）
- 调试与可视化工具链（CVar / DebugDraw / CSV 导出 / 热调参）

### 2.3 范围外（本计划不做）

- 准星 / HUD 的 UI 联动反馈（后续独立需求）
- CameraShake 资产与音效触发挂点（预留接口，不实现）
- 武器动画 / 蒙皮后坐动画（美术职责）
- 具体数值的最终手感调优（P7 单独做，属于调参不属于开发）
- **联机同步（状态复制 / 客户端预测 / 服务器校验 / 作弊检测）** —— 本项目定位为**单机** Demo，
  2026-09-17 决定不做联机，原 P6 阶段已剔除，详见 §5「P6 — 已剔除」

---

## 3. 架构设计

### 3.1 分层

```
[输入层] Lyra Input / GA_Weapon_Fire
    │
    ▼
[状态层] FRecoilRuntimeState  ←── 读取 ──  [数据层] ULyraRecoilProfile (DataAsset)
    │  纯算法，无引擎依赖，可 Automation 单测
    │
    ├──► [相机链] UCameraModifier_WeaponRecoil ──► LyraPlayerCameraManager
    │        视觉 Kick + 回正，不改真实 ControlRotation
    │
    └──► [弹道链] LyraGameplayAbility_RangedWeapon 发射方向偏移
             可复现 Pattern（前 N 发固定 + 之后伪随机）

[工具层] CVar / DebugDraw / CSV Dump / 资产热调参
```

### 3.2 新增类清单

| 类 / 文件 | 类型 | 职责 |
| --- | --- | --- |
| `LyraRecoilTypes.h` | 头文件 | 枚举与结构体：`EPoseState`、`FRecoilPatternPoint`、`FRecoilShotResult` |
| `ULyraRecoilProfile` | `UPrimaryDataAsset` | 全部手感参数与曲线（配置层） |
| `FRecoilRuntimeState` | `USTRUCT` | 纯算法：累加、恢复、姿态倍率、Pattern 采样（**无 UWorld 依赖**） |
| `UCameraModifier_WeaponRecoil` | `UCameraModifier` | 把状态转为相机偏移并回正 |
| `ULyraRecoilDebug` | 工具 | CVar 注册、DebugDraw、CSV 导出 |

统一放置于 `Source/LyraGame/Weapons/Recoil/`，相机修改器放 `Source/LyraGame/Camera/`。

### 3.3 关键决策（提前定死，避免后期返工）

1. **相机偏移走 CameraModifier，不用 `AddPitchInput` / `AddYawInput`。**
   `AddPitchInput` 会直接改 `ControlRotation`，与玩家输入、灵敏度耦合，恢复时无法区分"玩家自己拉的"和"后坐力抬的"，回正会把玩家视角一起拽走。CameraModifier 只作用于显示层，真实瞄准方向始终可控。
   （原文此处还列了"网络复制"作为耦合来源之一 —— 本项目 2026-09-17 已决定不做联机，该条不再适用，故删去；不影响结论。）
2. **算法层独立成纯 USTRUCT，不依赖 UWorld。**
   这是"每阶段可验证"的技术保障——P2/P3 的核心逻辑可以跑纯数值 Automation 测试，不需要起 PIE、不需要人工看画面。
3. **区分"视觉回正"与"弹道回正"，用 `RecoilReturnRatio` 单参数控制。**
   语义：`0 = 相机完全回正（弹道不回正，玩家需自己压枪，竞技向）`；`1 = 相机完全不回正`。这一条决定整个系统的手感性格，是首要调参项。
4. **Pattern 采用"前 N 发固定 + 之后伪随机"。**
   前 `PatternLength` 发使用资产内配置的归一化 Pattern 数组（保证可学习、可复现）；其后走**基于随机种子的确定性随机游走**，避免纯 `VRand` 带来的每次弹道不可复现、无法测试的问题。
5. **随机种子必须可控。**
   `RandomSeedMode = Fixed | Random`。Fixed 模式下同一武器连发 N 发，弹道序列逐帧一致——这是 P3 自动化测试的前提。

---

## 4. 阶段总览

| 阶段 | 目标 | 核心交付 | 验证手段 | 依赖 | 规模 |
| --- | --- | --- | --- | --- | --- |
| **P0** | 勘察与接口冻结 | 勘察报告 + 类型头文件骨架 | 编译通过 + 接入点定位到行号 | — | S |
| **P1** | 数据层 | `ULyraRecoilProfile` + 武器资产实例 | 资产校验 Automation Test | P0 | S |
| **P2** | 运行时核心 + 相机 Kick | 状态算法 + CameraModifier | 纯数值单测 + PIE 目视 | P1 | M |
| **P3** | 弹道 Pattern 接入 | 发射方向注入 + Pattern 采样 | Golden 数据比对 + 靶场截图 | P2 | M |
| **P4** | 恢复与姿态倍率 | 恢复曲线 + 4 种姿态倍率 | 数值参数化测试 + 姿态对比表 | P3 | M |
| **P5** | 调试与可视化工具链 | CVar 集 + DebugDraw + CSV Dump | CSV 曲线图（核心验收证据） | P4 | M |
| ~~**P6**~~ | ~~联机同步（后置）~~ **已剔除** | — | — | — | — |
| **P7** | 手感调参 + 固化（可选） | 参数配方 + 验收报告 | 完整验收 checklist 全绿 | P5 | S |
| **P8** | **相机镜头系统：Roll 震屏 + 镜头模式**（2026-09-17 追加） | Roll 独立通道 + 实时调试面板 + 镜头模式契约 | 纯数值单测 + PIE 目视 + A/B 消融 | P5 | M |

依赖链为严格串行：`P0 → P1 → P2 → P3 → P4 → P5 → P7`。**不允许跳阶**，每阶段必须通过验收才进入下一阶段。

> **范围变更（2026-09-17）**：原 `P6 联机同步` 已从本计划剔除。
> 理由：本项目定位为**单机** TPS 枪械 Demo，不做联机。
> 因此 P5 是本轮开发的**最后一个必做阶段**，P7 为可选的收尾（调参 + 固化）。
> 详见 §5「P6 — 已剔除（2026-09-17）」。
>
> **范围追加（2026-09-17）**：新增 **P8「相机镜头系统」**，依据
> 《FPS 相机镜头设计与实现（脱敏版）》§1/§2/§2.1/§2.2/§7，
> 在后坐力的 Pitch/Yaw 之外独立实现 **Roll 方向震屏**，并按文档 §1 抽象镜头模式契约。
> P8 只依赖 P5（复用既有 Debug 体系与测试范式），与 P7 调参并行。
> 详见 **[Docs/Recoil/08_CameraRollShake.md](Recoil/08_CameraRollShake.md)**。

---

## 5. 阶段详情

> 每阶段的"可验证内容"分为 **自动验证**（命令 + 数值断言，AI 可自证）与 **手动验收**（需大祥老师亲自确认的现象）。

---

### P0 — 勘察与接口冻结

**目标**：把接入点钉死到行号，冻结类型定义，之后不再返工。

**交付物**
- `Docs/Recoil/00_Recon.md`：现状勘察报告
- `Source/LyraGame/Weapons/Recoil/LyraRecoilTypes.h`：枚举与结构体（仅声明）

**任务清单**
- [ ] 读取 `LyraRangedWeaponInstance` 全量代码，梳理 Heat → Spread 的完整计算链路
- [ ] 定位 `LyraGameplayAbility_RangedWeapon` 中发射方向的计算位置（文件:行号）
- [ ] 梳理 `LyraPlayerCameraManager` → `LyraCameraComponent` → `LyraCameraMode_ThirdPerson` 的相机偏移管线，确认 CameraModifier 挂载点
- [ ] 确认 `LyraWeaponInstance` 的生命周期（装备/卸下时状态如何重置）
- [ ] 输出 `LyraRecoilTypes.h` 骨架并纳入编译

**可验证内容**

*自动验证*
```bash
# 编译（命令按实际引擎路径调整）
"D:\UE_5.8\Engine\Build\BatchFiles\Build.bat" LyraEditor Win64 Development -Project="D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject" -WaitMutex
```
预期：编译 0 error，新头文件被 `LyraGame.Build.cs` 收录。

*手动验收*
- [ ] 勘察报告中每个接入点都标注到 `文件:行号` 级别，不接受"大概在这个文件里"
- [ ] 类型头文件中的枚举/结构体设计经确认可作为后续阶段的稳定契约

**DoD**：接入点清单无 TBD 项；编译零错误。

---

### P1 — 数据层：后坐力配置资产

**目标**：把全部手感参数收敛到一个数据资产，代码零硬编码。

**交付物**
- `Source/LyraGame/Weapons/Recoil/LyraRecoilProfile.h/.cpp`
- `Content/Weapons/Recoil/DA_Recoil_Rifle.uasset`（+ Pistol / Shotgun 各一份）

**参数清单**（资产字段）

| 分组 | 参数 | 说明 |
| --- | --- | --- |
| 基础 | `RecoilPerShot_Vertical` | 每发基础垂直 Kick（度） |
| 基础 | `RecoilPerShot_Horizontal` | 每发基础水平 Kick（度） |
| 曲线 | `VerticalKickCurve` | 射击序号 → 垂直 Kick 倍率（实现"渐强/渐弱"） |
| 曲线 | `RecoveryCurve` | 归一化时间 `[0,1]` → 回正进度 `[0,1]` |
| 曲线 | `RecoveryDelay` / `RecoveryTime` | 停火后开始回正的延迟 / 回正总时长 |
| Pattern | `PatternPoints[]` | 归一化 Pattern 点数组（前 N 发固定弹道） |
| Pattern | `PatternLength` | 固定 Pattern 覆盖的发数 |
| Pattern | `HorizontalRandomRange` | Pattern 之后水平随机游走范围 |
| 倍率 | `PoseMultiplier_Aiming / _Standing / _Crouching / _JumpingOrFalling` | 姿态倍率 |
| 上限 | `MaxVerticalKick` / `MaxHorizontalKick` | 累加上限（Clamp） |
| 回正 | `RecoilReturnRatio` | `0=完全回正`，`1=不回正`（核心手感开关） |
| 随机 | `RandomSeedMode` | `Fixed` / `Random`（Fixed 供自动化测试） |

**可验证内容**

*自动验证*
- 新增 Automation Test：`Lyra.Recoil.Profile.Validation`
  - 遍历 `Content` 下所有 `ULyraRecoilProfile` 资产
  - 断言：曲线非空、`RecoveryTime > 0`、`PatternLength ≤ PatternPoints.Num()`、倍率 ∈ `(0, 5]`、上限 > 0
  - 一条不合格即 FAIL 并打印资产路径

*手动验收*
- [ ] 编辑器内可右键新建该资产类型
- [ ] Details 面板分类清晰（基础 / 曲线 / Pattern / 倍率 / 上限）
- [ ] PIE 中改参数后重新运行，日志打印的新值生效

**DoD**：所有手感参数均来自资产，源码中无可调魔数。

---

### P2 — 运行时核心与相机 Kick

**目标**：连发时相机产生可见上跳并自动回正，且核心算法可被纯数值验证。

**交付物**
- `Source/LyraGame/Weapons/Recoil/LyraRecoilState.h/.cpp`：`FRecoilRuntimeState`
- `Source/LyraGame/Camera/LyraCameraModifier_WeaponRecoil.h/.cpp`
- 接入 `ULyraRangedWeaponInstance`（持有一个 `FRecoilRuntimeState` 实例）
- `Source/LyraGame/Tests/LyraRecoilTest.spec.cpp`

**状态机（`FRecoilRuntimeState`）**
```
Idle ──开火──► Accumulating ──停火 > RecoveryDelay──► Recovering ──归零──► Idle
                   ▲                                      │
                   └────────── 再次开火 ──────────────────┘
```

**可验证内容**

*自动验证*（纯数值，无需 PIE）
- `Lyra.Recoil.State.` 测试组，断言：
  1. 连发 10 发后 `AccumulatedPitch` == 逐步累加和（容许浮点误差 `1e-4`）
  2. 累加值被正确 Clamp 在 `MaxVerticalKick`
  3. 停火经过 `RecoveryDelay` + `RecoveryTime` 后，稳态偏移 == `MaxKick × RecoilReturnRatio`
  4. **确定性**：固定种子下连续两次模拟 30 发，逐发偏移序列完全一致
  5. `Recovering` 中途再次开火，状态正确回到 `Accumulating` 且不产生跳变

*手动验收*
- [ ] PIE 中长按开火：相机持续上抬，松手后平滑回正，无卡顿与跳变
- [ ] `Lyra.Recoil.Debug 1` 屏幕左上角实时显示 `ShotIndex / CurrentKick / State / RecoveryTimer`
- [ ] `stat unit` 对比基线，帧时间无明显恶化（`Game` 线程增量 < 0.1ms）

**DoD**：连续 30 发无异常；自动化测试全绿；回正目视平滑。

---

### P3 — 弹道 Pattern 接入

**目标**：子弹实际落点形成可学习、可复现的后坐力弹道。

**交付物**
- `FRecoilRuntimeState::GetShotDirectionOffset(ShotIndex)` → 返回角度偏移
- 修改 `LyraGameplayAbility_RangedWeapon.cpp`：在发射方向计算处叠加后坐力偏移（**叠加在扩散之前**，两者独立可调）
- `Source/LyraGame/Tests/Data/RecoilGolden_*.json`：固定种子下的弹道基准数据

**可验证内容**

*自动验证*
- `Lyra.Recoil.Pattern.` 测试组：
  1. 固定种子连发 20 发，生成的方向偏移序列与 Golden 数据逐发比对（`1e-4` 容差）
  2. 前 `PatternLength` 发的水平偏移严格等于 `PatternPoints` 中的配置值
  3. `PatternLength` 之后进入伪随机区间，断言其仍在 `HorizontalRandomRange` 内且**同种子可复现**
  4. 开关关闭时，弹道偏移恒为 0（不污染原有扩散逻辑）

*手动验收*
- [ ] 靶场对墙连发 30 发：弹着点呈现清晰的"先上后左右摆"的 Pattern
- [ ] 相同操作重放两次，弹着点分布一致（截图叠加对比）
- [ ] 用 `Lyra.Recoil.Enable 0` 关闭后，弹道回归 Lyra 原有纯扩散行为

**DoD**：Pattern 可复现；与 spread 扩散叠加后逻辑互不干扰；Golden 测试纳入回归。

---

### P4 — 恢复曲线与姿态/移动倍率

**目标**：站/蹲/移动/空中四种状态下的后坐力差异明确且可量化。

**交付物**
- `RecoveryCurve` 完整接入，支持非线性回正（快回—慢回 / 慢回—快回）
- 姿态倍率接入，复用 Lyra 已有的 Crouch / Speed / Jump 判定（不重复造轮子）
- `Docs/Recoil/04_PoseMatrix.md`：姿态倍率验收数值表

**可验证内容**

*自动验证*
- `Lyra.Recoil.Pose.` 参数化测试：对四种姿态各模拟 10 发，输出垂直/水平累计位移，断言：
  - 各姿态结果比值 == 配置倍率比值（`1e-3` 容差）
  - 蹲姿位移 < 站姿位移（默认配置下）

*手动验收*
- [ ] 四种姿态各录一段相同操作，横向对比反弹幅度，填入验收表
- [ ] 非线性恢复曲线生效：调成"慢—快"与"快—慢"两种，回正手感肉眼可区分

**DoD**：姿态倍率数值表实测值与配置值一致；验收表由大祥老师签字确认。

---

### P5 — 调试与可视化工具链（本轮范围的重点）

**目标**：不开编辑器重启、不重编译，就能把整套手感参数调完并拿到曲线证据。

**交付物**
- CVar 集（注册于 `ULyraRecoilDebug`）：
  | 命令 | 作用 |
  | --- | --- |
  | `Lyra.Recoil.Enable 0/1` | 总开关 |
  | `Lyra.Recoil.Scale <f>` | 全局后坐力倍率（调试用，不影响资产） |
  | `Lyra.Recoil.Debug 0/1` | 屏幕数值面板 |
  | `Lyra.Recoil.DebugDraw 0/1` | 世界内可视化 |
  | `Lyra.Recoil.Dump` | 导出上一轮连发数据到 CSV |
  | `Lyra.Recoil.ReloadProfile` | 热重载当前武器的资产 |
- DebugDraw 内容：准星当前偏移点、Pattern 点阵、回正进度指示
- CSV 导出路径：`Saved/RecoilDump_<timestamp>.csv`，列：
  `ShotIndex, VerticalKick, HorizontalKick, AccumulatedPitch, AccumulatedYaw, TimeSinceFire`

**可验证内容**

*自动验证*
- `Lyra.Recoil.Dump.` 测试：程序化发射 N 发（N=17，非整数倍用于边界）后触发 Dump
  - 断言 CSV 行数 == N
  - 断言 CSV 数值与 `P2` 自动化测试中同参数下的计算值一致

*手动验收*
- [ ] 靶场连发 → `Lyra.Recoil.Dump` → CSV 文件生成在 `Saved/`
- [ ] 用图表工具（Excel / Python）画出 `AccumulatedPitch` 曲线，形状符合预期
- [ ] 改资产参数 → `Lyra.Recoil.ReloadProfile` → 再连发 → 导出第二条 CSV，两条曲线叠加可见差异
- [ ] **不开编辑器重启、不重编译**，凭调试工具完成一次完整的参数微调

**DoD**：CSV 曲线成为后续手感调优的标准证据；调参全程无需重启。

---

### P6 — 已剔除（2026-09-17）

**状态**：**不做**。本项目定位为**单机** TPS 枪械 Demo，不做联机，故原「联机同步接入」阶段从本计划中剔除。

**决定记录**

| 项 | 内容 |
| --- | --- |
| 决定日期 | 2026-09-17 |
| 决定人 | 大祥老师 |
| 原计划内容 | DS 权威 + 客户端预测 + 服务器校验 + 容差回滚（交付物为 `FRecoilRuntimeState` 关键字段复制、`LyraTestController` 双端自动化、`Net PktLag=100` 手动验收） |
| 剔除理由 | 本项目是单机 Demo，没有联机需求 |
| 影响 | P5 成为本轮开发的**最后一个必做阶段**；依赖链变为 `P0 → P1 → P2 → P3 → P4 → P5 → P7`；附录 B 对应行标记为「已剔除」 |

**保留下来的设计（不做联机也依然有价值，故不回退）**

1. `FRecoilRuntimeState` 不持有任何 UObject 指针 → 核心算法可以被**纯数值单测**完整覆盖。
   这是 P2/P3/P4 每阶段都能给出可复制命令 + PASS/FAIL 证据的技术基础（见硬性规则 4）。
2. 相机偏移走 `CameraModifier` 而不改 `ControlRotation` → 消除的是"后坐力与玩家输入耦合"这个问题（回正时不会把玩家自己拉的视角一起拽走）。**单机下同样存在**，与联机无关。
3. 随机种子可控（`Fixed` / `Random`）→ 单机下用于**弹道可复现**与 Golden 回归锁。

**如果将来要重启联机**（保留备查，不在本轮范围）：

- 给 `FRecoilRuntimeState` 加 `UPROPERTY(Replicated)` + `GetLifetimeReplicatedProps`
- 让"这一发用哪个 ShotIndex / 哪个 Seed"由可复现的输入显式决定，使双端算出逐位一致的结果
- 服务器权威 + 客户端预测的容差回滚（算法是纯函数 + 固定种子，回滚可简化为"权威值覆盖本地"）

---

### P7 — 手感调参、参数固化与验收报告（可选）

**目标**：把框架变成"能用的武器手感"，并留档。

**交付物**
- `Docs/Recoil/07_TuningRecipe.md`：各武器最终参数配方
- 全套 CSV 曲线对比图
- `Docs/Recoil/FinalAcceptanceReport.md`：完整验收报告

**可验证内容**

*手动验收*
- [ ] 完整验收 checklist 全项通过
- [ ] 三把武器（Rifle / Pistol / Shotgun）手感差异明确，经确认定稿

---

### P8 — 相机镜头系统：Roll 震屏与镜头模式（2026-09-17 追加）

**目标**：依据《FPS 相机镜头设计与实现（脱敏版）》§1/§2/§2.1/§2.2/§7，
在既有 Pitch/Yaw 后坐力之外**独立**实现 Roll 方向阻尼震屏，并按 §1 抽象镜头模式契约，配实时调试。

**为什么独立成阶段**：Roll 与 Pitch/Yaw **没有共享逻辑** —— 前者是"累加-回正"的积分模型、
停在稳态偏移；后者是"每发重置时钟 → 衰减包络 × 周期项"的解析模型、必回零。
把它塞进 `AccumulatedPitch/Yaw` 会导致 Roll 单向漂移（看起来"镜头歪了"）并污染回正曲线。
因此它是一条**并列的第三通道**，不是 Pitch/Yaw 的扩展 —— 这是本阶段的核心设计判断。

**交付物**
- `Source/LyraGame/Camera/LyraCameraShakeTypes.h`：参数/状态契约 + 镜头模式枚举
- `Source/LyraGame/Camera/LyraCameraRollShake.h/.cpp`：Roll 纯算法层（无 UWorld 依赖）
- `ULyraRecoilProfile` 的 `Recoil|RollShake` 完整参数组
- `Docs/Recoil/08_CameraRollShake.md`：策划案 + 实现方案（含实时调试说明）

**可验证内容**

*自动验证*
- [ ] 编译 0 error（`-NoUBA`）
- [ ] `Lyra.Recoil` 全量测试通过（含新增 Roll 用例）
- [ ] Roll 单测：`Elapsed >= Duration` 输出精确为 0；`PhaseJitter = 0` 时序列完全可复现

*手动验收*
- [ ] `Lyra.Recoil.RollDebug 1` 面板出现，波形行能看到 ~4 个递减波峰、末尾贴中轴
- [ ] `Lyra.Recoil.RollShake 0` → 横滚抖动消失，**Pitch/Yaw 完全不受影响**（独立通道的判据）
- [ ] 扫满弹匣 → `StartAmp` 从 0.6 逐发涨到 1.5 封顶
- [ ] `Lyra.Recoil.DebugDraw 1` → 开火时品红圆弧往复，绿线（真实瞄准轴）不动

**已知未落地项**（见 `08_CameraRollShake.md` §6）
- 固定步长更新（文档 §2/§7 要求）仍走帧 DeltaSeconds
- FOV / Breathing 模式只有枚举、无实现（属独立需求）

---

## 6. 验收制度（强制执行）

### 6.1 Gate 规则

**每个阶段完成后，必须停下并发出验收请求，在收到"验收通过"之前不得开始下一阶段。**

这条规则不可跳过、不可批量合并、不可用"我顺手把下一阶段也做了"绕过。

### 6.2 验收请求模板（每次必须包含）

```
【验收请求 — 阶段 P<N>：<阶段名>】

1. 改动清单
   - 新增：<文件路径>
   - 修改：<文件路径>（说明改了什么）
2. 自动验证结果
   - 命令：<可直接复制的命令>
   - 结果：<PASS/FAIL + 关键数值>
3. 手动验收步骤
   - 步骤 1：<操作>  预期：<现象>
   - 步骤 2：<操作>  预期：<现象>
4. 证据
   - 截图 / CSV 路径 / 日志片段
5. 待确认项
   - <需要大祥老师拍板的点，如参数取值>
```

### 6.3 提醒机制

- **即时提醒**：AI 每完成一个任务条目或阶段，立即发出上述验收请求卡片，并停止推进。
- **状态表维护**：每阶段验收结果同步更新到本文档 [附录 B](#附录-b进度与验收状态表)。
- **定时巡检**：已配置工作日巡检，检查状态表中是否存在"待验收"项，如有则提醒。

---

## 7. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 相机偏移与玩家输入耦合 | 回正时"抢"玩家视角，手感割裂 | 决策 1 已定：走 CameraModifier，不碰 `ControlRotation` |
| 后坐力与 Lyra 原有 Spread 冲突 | 双份随机叠加，弹道失控 | P3 明确顺序：后坐力偏移叠加在扩散之前，两者独立可关 |
| 弹道每次不可复现 | 无法自动化测试，只能靠感觉 | 决策 4/5：固定 Pattern + 可控随机种子 |
| 手感调参无据可依 | 反复"凭感觉"改参数，无法收敛 | P5 的 CSV 曲线作为标准证据，调参前后可量化对比 |
| ~~P6 联机改造波及前期设计~~ | — | **已随 P6 剔除而消失（2026-09-17）**。不过"状态集中在 `FRecoilRuntimeState`"这个做法保留 —— 它的价值是实现层面的纯数值可测，与联机无关 |
| 资产过多导致手工维护成本 | 参数不一致 | P1 资产校验测试覆盖全部资产 |

---

## 附录 A：总任务初始 Prompt

> 新建对话后，把下面整段内容作为第一条消息发送。

```
【项目】UE 5.8.2 TPS 枪械 Demo（Lyra 派生）
- 工程路径：D:\TPSGunsDemo\TPSGunsDemo
- 引擎路径：D:\UE_5.8（源码版）
- 主力模块：LyraGame / LyraEditor
- 编辑器：VS Code（D:\Microsoft VS Code）

【任务】按开发计划文档，逐阶段实现「TPS 后坐力系统（数据驱动可调框架）」。

【基准文档 — 先读再动手】
D:\TPSGunsDemo\TPSGunsDemo\Docs\RecoilDevelopmentPlan.md
这份文档是唯一基准，包含：架构设计、关键决策、P0–P7 阶段划分、每阶段的可验证内容、
验收制度、进度状态表（附录 B）。开工前必须完整读一遍。

【硬性规则，不可违反】
1. 严格按 P0 → P1 → P2 → P3 → P4 → P5 顺序推进，不允许跳阶。
   （原 P6「联机同步」已由大祥老师于 2026-09-17 决定剔除 —— 本项目不做联机，详见 §5；
     P7「手感调参 + 固化」为可选的收尾阶段。）
2. 每完成一个任务条目或一个阶段，立即停下来发出「验收请求」，格式见文档第 6.2 节，
   内容必须包含：改动清单、自动验证命令与结果、手动验收步骤、证据、待确认项。
   发出后停止推进，等待我回复"验收通过"才继续。
3. 每个阶段必须做到"可验证"：
   - 自动验证：跑 UE Automation Test（范式参考 Source/LyraGame/Tests/MenuStartElimination.spec.cpp），
     给出可复制的命令行和 PASS/FAIL 结果；
   - 手动验收：给出我能照着做的操作步骤 + 预期现象。
   不接受"已实现，应该没问题"这类没有证据的结论。
4. 核心算法层（FRecoilRuntimeState）必须保持无 UWorld 依赖，保证可纯数值单测。
5. 相机偏移一律走 CameraModifier，禁止使用 AddPitchInput / AddYawInput 直接改 ControlRotation。
6. 所有手感参数必须来自 ULyraRecoilProfile 数据资产，源码里不允许出现可调魔数。
7. 每完成一个阶段，同步更新计划文档附录 B 的进度与验收状态表。
8. 编译验证命令（每次改完 C++ 必须执行）：
   "D:\UE_5.8\Engine\Build\BatchFiles\Build.bat" LyraEditor Win64 Development -Project="D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject" -WaitMutex
   编译不通过不算完成。

【允许拆分子 agent】
在以下情况下可以把子任务派给子 agent 并行执行：
- 相互独立、不修改同一文件的任务（例如：一个子 agent 写自动化测试，一个写调试工具）
- 需要大量代码勘察的任务（例如 P0 阶段检索相机管线与武器生命周期）
- 内容生成类任务（例如生成验收文档、整理参数表）
约束：
- 涉及同一文件的多项改动必须串行，禁止并发写同一文件；
- 子 agent 的产出必须由主 agent 复核（实际读文件确认改动落地），不得直接采信其总结；
- 主 agent 对最终结果负责，验收请求由主 agent 统一发出。

【执行方式】
1. 先读基准文档和项目现状，向我汇报你确认到的接入点（文件:行号），确认无误后开始 P0。
2. 之后按阶段推进，每个阶段结束时发出验收请求并停止。
3. 如果我提出的验收没通过，记录问题、修复后重新发验收请求。

现在开始：读文档 → 输出 P0 的执行计划与我确认。
```

---

## 附录 B：进度与验收状态表

> 状态取值：`未开始` / `进行中` / `待验收` / `已通过` / `已驳回（原因）`
> 每阶段完成后由 AI 更新本表。

| 阶段 | 名称 | 状态 | 自动验证 | 手动验收 | 完成日期 | 备注 |
| --- | --- | --- | --- | --- | --- | --- |
| P0 | 勘察与接口冻结 | 待验收 | ✅ 编译 0 error | ⏳ 待确认 | 2026-09-17 | 接入点全部定位到 `文件:行号`，无 TBD |
| P1 | 数据层 | 待验收 | ✅ 1/1 PASS | ⏳ 待确认 | 2026-09-17 | 3 份资产已生成；Commandlet 幂等 |
| P2 | 运行时核心 + 相机 Kick | 待验收 | ✅ 5/5 PASS | ⏳ 待确认 | 2026-09-17 | 纯数值测试；未改动 `LyraWeaponStateComponent` |
| P3 | 弹道 Pattern 接入 | 待验收 | ✅ 4/4 PASS | ⏳ 待确认 | 2026-09-17 | Golden 3 份已导出并手工验算 |
| P4 | 恢复与姿态倍率 | 待验收 | ✅ 3/3 PASS | ⏳ 待确认 | 2026-09-17 | 恢复曲线已在 P2 接入；姿态换算抽成纯静态函数；实测表见 `Docs/Recoil/04_PoseMatrix.md` |
| P5 | 调试与可视化工具链 | 待验收 | ✅ 6/6 PASS | ⏳ 待确认 | 2026-09-17 | 六个交付项全部落地；新增 CSV→HTML 曲线工具；命令注册钉成断言 |
| ~~P6~~ | ~~联机同步~~ **已剔除** | 不做 | — | — | 2026-09-17 | 决定：本项目为单机 Demo，不做联机；详见 §5「P6 — 已剔除」 |
| P7 | 手感调参 + 固化 | 未开始 | — | — | — | 可选 |
| **P8** | **相机镜头 Roll 震屏 + 镜头模式** | **待验收** | ✅ 编译 + 5 个用例 | ⏳ 待确认 | 2026-09-17 | 见 [Recoil/08_CameraRollShake.md](Recoil/08_CameraRollShake.md) |
| **P9** | **单发插值模型（InstantWrite / Interpolated 两套并存）** | **待验收** | ✅ 编译 + 6 个用例 | ⏳ 待确认 | 2026-09-17 | 见 [Recoil/10_SingleShotInterpolation.md](Recoil/10_SingleShotInterpolation.md) |
| **P10** | **连发累积失效修复（Interpolated 锚点）**（云端） | **待 PIE 手测** | ✅ 编译 + 全绿 | ⏳ 待确认 | 2026-09-20 | 见 [Recoil/11_BurstAccumulationFix.md](Recoil/11_BurstAccumulationFix.md)。**回正目标的计算已由 P11 接管**（2026-09-20 合并决定：回正相关以本次实现为主），本条保留为根因分析与连发锚点的记录 |
| **P11** | **回正扣减压枪量**（本次，**回正计算的权威实现**） | **待验收** | ✅ 7 个 `Lyra.Recoil.Compensation.*` 用例 | ⏳ 待确认 | 2026-09-20 | 见 [Recoil/11_RecoveryCompensation.md](Recoil/11_RecoveryCompensation.md)。两轴同规则 + `bCompensationAwareRecovery` 总开关；与 P10/P12/P14 同源，代码**只走这一条** |
| **P12** | **垂直钳制实时抵扣压枪量** | **待 PIE 手测** | ✅ 编译 + 37/37 全绿 | ⏳ 待确认 | 2026-09-20 | 同上 §12（编号未占 P11）；钳制口径仍成立，回正目标部分由 P11 提供 |
| **P13** | **散布并入后坐力配置表（姿态-角度直接模型）** | **待 PIE 手测** | ✅ 编译 + 37/37 全绿 | ⏳ 待确认 | 2026-09-20 | 见 [Recoil/12_SpreadInProfile.md](Recoil/12_SpreadInProfile.md)；验收请求 [Recoil/Acceptance/P13_验收请求.md](Recoil/Acceptance/P13_验收请求.md) |

**累计自动化测试：37 个用例全绿**（`Lyra.Recoil.*` = P0–P5 的 19 + P8 的 5 + P9 的 6 + P13 的 7）。
实测证据：`Saved/Logs/TPSGunsDemo.log` 里
`LogAutomationCommandLine: Display: ...Automation Test Queue Empty 37 tests performed.`
一键复跑：`Docs/Recoil/Tools/run-all-checks.ps1`

> **P13 的散布不体现在这 37 个用例的"后坐力"部分**：它是一套独立特性（7 个用例挂在
> `Lyra.Recoil.Spread.*` 下）。且 **磁盘上的 5 份 `DA_Recoil_*` 资产尚未重新生成**，
> 所以 PIE 里跑的还是旧 heat 散佈链路 —— 这是刻意的零回归默认，不是漏配。

**P4 实测姿态倍率（Rifle，10 发累计垂直位移）**：站 2.9661 / 蹲 2.3729 / 空中 4.4492 / 瞄准 2.2246，
比值与配置倍率（1.0 / 0.8 / 1.5 / 0.75）最大偏差 5e-5。
**非线性回正实测半程进度**：快回—慢回 0.7667 / 线性 0.5000 / 慢回—快回 0.3667。

**P5 实测证据**：CSV 导出 17 发、行数与数值逐项与 P2 公式吻合（1e-4）；
`Lyra.Recoil.Scale` 1.0 → 0.5 时逐发 kick 严格减半；
曲线报告产物 `Docs/Recoil/Charts/P5_ScaleOverlay.html`（两条曲线叠加可见差异）。

### 执行备注（AI 记录，待大祥老师确认后可能回写正文）

1. **本机编译命令必须追加 `-NoUBA`**。系统拦截了进程的文件删除类系统调用
   （`SetFileInformationByHandle(FileDispositionInfo)`），UBA 清理临时文件被拒会导致整轮构建
   被判 `Failed (OtherCompilationError)`，但代码本身是好的。加 `-NoUBA` 后正常。
   本文档 §5「自动验证」处给出的原始命令**未做修改**，等你拍板后再统一回写。
2. **P1 的 `.uasset` 由 `ULyraRecoilAssetGenCommandlet` 生成**（工程未启用 PythonScriptPlugin）。
   命令：`UnrealEditor-Cmd <uproject> -run=LyraRecoilAssetGen`
3. **P3 的 Golden 数据由 `ULyraRecoilGoldenDumpCommandlet` 导出**。
   改过资产的相关字段后必须重导，否则 `Lyra.Recoil.Pattern.Golden` 会报 stale。
   命令：`UnrealEditor-Cmd <uproject> -run=LyraRecoilGoldenDump`
4. **武器实例接线（`DA_Recoil_*` → `B_WeaponInstance_*`）尚未执行**，因此 P2/P3 的手动验收
   暂时无法进行。BluePrint 位置：`Plugins/GameFeatures/ShooterCore/Content/Weapons/`。
5. 详细的逐阶段改动清单、证据与待确认项见 `Docs/Recoil/Acceptance/`；
   操作手册见 `Docs/Recoil/PROGRESS.md`。
6. **`-ExecCmds` 里不能塞多个分号命令**（本机实测不会执行，且 `Quit` 失效导致编辑器挂住）。
   单命令 + `-TestExit` 正常。`Lyra.Recoil.Dump` / `ReloadProfile` 这类动作型命令的注册状态
   改由自动化测试 `Lyra.Recoil.Console.Registration` 断言，不依赖命令行调用。
7. **CSV 曲线工具**：`Docs/Recoil/Tools/plot-recoil-csv.py`（纯标准库，无需 matplotlib）
   把 `Lyra.Recoil.Dump` 导出的 CSV 画成自包含 HTML；支持多文件叠加对比，
   是 §P5 手动验收与 §P7 调参的配套证据工具。

---

*本计划由祥子依据 TPSGunsDemo 现有代码结构制定，2026-09-17。*
