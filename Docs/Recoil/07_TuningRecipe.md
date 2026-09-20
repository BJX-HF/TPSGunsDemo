# P7 · 两把步枪的后坐力配方（S 型 / 7 字型）

> 2026-09-17 落定。这份文档回答三件事：**现在场上是哪两把枪、数值是多少、想改怎么改**。
> 所有数值都能在 Content Browser 里直接看到并手改，改完立刻生效（PIE 与编辑器同进程共享 UObject）。

---

## 1. 一句话

Lyra 原有那把自动步枪被复制成 **两把**，共用同一套模型 / 动画 / 开火能力，
**只有后坐力手感资产不同**。PIE 进入 `L_ShooterPerf` 时两把都在快捷栏里，可以直接切。

| 槽位 | 武器 | 后坐力资产 | 形状 | 整体强度 |
| --- | --- | --- | --- | --- |
| 0（出生默认拿这把） | `ID_Rifle` | `DA_Recoil_Rifle_S` | **S 型**（先右后左一个弯） | 小（10 发垂直 ≈ 2.34°） |
| 1 | `ID_Rifle_7` | `DA_Recoil_Rifle_7` | **7 字型**（斜向右上 → 顶部向左一横） | 稍大（12 发垂直 ≈ 2.59°，水平峰值 1.40°） |

---

## 2. 资产地图（全部已接线，改哪个都行）

```
后坐力配置（本项目后坐力系统的唯一数值来源）
  Content/Weapons/Recoil/DA_Recoil_Rifle_S.uasset   ← S 型 · 小   ★新增
  Content/Weapons/Recoil/DA_Recoil_Rifle_7.uasset   ← 7 字型 · 稍大 ★新增
  Content/Weapons/Recoil/DA_Recoil_{Rifle,Pistol,Shotgun}.uasset  ← 原有三份，未改动

武器链（在 ShooterCore 插件里，与 Lyra 原版同构）
  槽位 0：ID_Rifle ──► WID_Rifle ──► B_WeaponInstance_Rifle ──► DA_Recoil_Rifle_S   ★接线
  槽位 1：ID_Rifle_7 ─► WID_Rifle_7 ─► B_WeaponInstance_Rifle_7 ─► DA_Recoil_Rifle_7 ★全套新增
          （ID_Rifle_7 / WID_Rifle_7 / B_WeaponInstance_Rifle_7 均为 ID_Rifle 那条链的复制品）

两把枪共用的部分（没有复制，所以手感之外完全一致）
  B_Rifle（枪模型+动画）· AbilitySet_ShooterRifle（GA_Weapon_Fire_Rifle_Auto / Reload）
  W_Reticle_Rifle · W_AmmoCounter_Rifle · SK_Rifle · GE_Damage_RifleAuto

出生装备来源
  ShooterCore/Game/B_Hero_ShooterMannequin  →  InitialInventoryItems = [ID_Rifle, ID_Rifle_7]
  （该变量被 AddInitialInventory 遍历，逐个 AddItemToSlot，最后 SetActiveSlotIndex(槽位0)）
```

`Content/Weapons/Recoil/DA_Recoil_Rifle`（原版那份）**没有被改动**，它仍是
`Lyra.Recoil.Pattern.Golden` 锁定的基准资产，同时是 pistol/shotgun 之外的"参考配方"。

---

## 3. 数值表

### 3.1 S 型（`DA_Recoil_Rifle_S`）—— 整体小 · **`Interpolated`**

| 参数 | 值 | 说明 |
| --- | --- | --- |
| **`SingleShotMode`** | **`Interpolated`** | **单发模型（2026-09-17 落地）** |
| **`LiftDuration`** | **0.045** s | 上升斜坡耗时 |
| **`ReboundDuration`** | **0.030** s | 回弹段时长 |
| **`ReboundRatio`** | **0.72** | 回弹到峰值的比例（"一顿"的深度） |
| **`LiftCurve` / `ReboundCurve`** | Ease-Out / 线性 | 段内形状 |
| `RecoilPerShot_Vertical` | 0.22 | 单发垂直基准（原 Rifle 是 0.35 → 明显更小） |
| `RecoilPerShot_Horizontal` | 0.52 | 单发水平基准（Pattern X 有正负，大部分互相抵消） |
| `PatternLength` | 12 | 12 发固定 Pattern，之后进随机游走 |
| `MaxVerticalKick` / `MaxHorizontalKick` | 4.0 / 2.0 | 上限压低 |
| `RecoveryDelay` / `RecoveryTime` | 0.12 / 0.30 | 回正快 |
| `RecoilReturnRatio` | 0.15 | 相机基本回正（压枪向） |
| `HorizontalRandomRange` | 0.45 | 尾部摆动收敛 |
| `PoseMultiplier_*` | 瞄准 0.75 / 站 1.0 / 蹲 0.8 / 空中 1.5 | 与项目基线一致 |
| `VerticalKickCurve` | (0, 0.70) (4, 1.00) (12, 1.35) | 沿用构造默认曲线 |
| `RandomSeedMode` / `FixedRandomSeed` | Fixed / 20260917 | 可复现 |

Pattern（X = 每发水平增量，右为正；Y = 每发垂直倍率）：

```
 i   0     1     2     3     4     5     6      7      8      9     10     11
 X  0.22  0.26  0.26  0.24  0.21  0.07 -0.19  -0.26  -0.33  -0.37  -0.38  -0.39
 Y  0.55  0.62  0.68  0.74  0.79  0.84  0.88   0.92   0.95   0.97   0.99   1.00
```

逐发累计（度，1 x Scale=1.0、站姿）：

```
 i | 垂直增量 | 垂直累计 | 水平增量 | 水平累计
 0 |  0.0847 |  0.085  | +0.1144 | +0.114
 1 |  0.1057 |  0.190  | +0.1352 | +0.250
 2 |  0.1272 |  0.318  | +0.1352 | +0.385
 3 |  0.1506 |  0.468  | +0.1248 | +0.510
 4 |  0.1738 |  0.642  | +0.1092 | +0.619
 5 |  0.1929 |  0.835  | +0.0364 | +0.655   ← 水平峰值（向右）
 6 |  0.2105 |  1.045  | -0.0988 | +0.556
 7 |  0.2290 |  1.274  | -0.1352 | +0.421
 8 |  0.2456 |  1.520  | -0.1716 | +0.250
 9 |  0.2601 |  1.780  | -0.1924 | +0.057
10 |  0.2750 |  2.055  | -0.1976 | -0.140
11 |  0.2874 |  2.342  | -0.2028 | -0.343   ← 收在中心左侧
```

**总计：垂直 2.342°，水平 峰值 +0.655° → 收尾 -0.343°。**
水平/垂直峰值比 ≈ 0.28 —— 一条**细长的 S**：先向右摆出去，再向左摆回来。

### 3.2 7 字型（`DA_Recoil_Rifle_7`）—— 明显更大 · **`Interpolated`**（2026-09-20 起）

| 参数 | 值 | 说明 |
| --- | --- | --- |
| **`SingleShotMode`** | **`Interpolated`** | 单发模型（2026-09-20 由 `InstantWrite` 切过来） |
| **`LiftDuration`** | **0.045** s | 与 S 型一致（两把枪射速相同，时间轴暂不分开） |
| **`ReboundDuration`** | **0.030** s | |
| **`ReboundRatio`** | **0.72** | |
| **`LiftCurve` / `ReboundCurve`** | Ease-Out / 线性 | |
| `RecoilPerShot_Vertical` | 0.42 | 单发垂直基准（比 S 型大 91%） |
| `RecoilPerShot_Horizontal` | 0.35 | 单发水平基准 |
| `PatternLength` | 12 | |
| `MaxVerticalKick` / `MaxHorizontalKick` | 9.0 / 4.0 | 上限抬高（够画完那根横杠） |
| `RecoveryDelay` / `RecoveryTime` | 0.18 / 0.42 | 回正更慢 |
| `RecoilReturnRatio` | 0.30 | 残留偏移更多（更"抓不住"） |
| `HorizontalRandomRange` | 0.70 | 尾部摆动更大 |
| `PoseMultiplier_*` | 瞄准 0.80 / 站 1.0 / 蹲 0.75 / 空中 1.65 | 姿态差异比 S 型更敏感 |
| `VerticalKickCurve` | (0, 0.80) (3, 1.00) (12, 1.25) | 前段起得更猛 |
| `RandomSeedMode` / `FixedRandomSeed` | Fixed / 20260917 | 可复现 |

Pattern：

```
 i   0     1     2     3     4     5     6      7      8      9     10     11
 X  0.55  0.60  0.62  0.60  0.58  0.55  0.50  -0.70  -0.80  -0.85  -0.90  -0.90
 Y  0.60  0.72  0.82  0.90  0.95  0.98  1.00   0.05   0.05   0.05   0.05   0.05
```

> 第 7~11 发 `Y = 0.05`（几乎不抬枪）是**刻意的** —— 数字「7」顶部那根横杠要平，
> 垂直不再涨、水平往左走，形状才出得来。这也是为什么这把枪的 `PatternPoints`
> 前 7 发全是正 X、后 5 发全是负 X：单方向累积大，所以"整体后坐力稍大"。

逐发累计：

```
 i | 垂直增量 | 垂直累计 | 水平增量 | 水平累计
 0 |  0.2016 |  0.202  | +0.1925 | +0.193
 1 |  0.2621 |  0.464  | +0.2100 | +0.402
 2 |  0.3214 |  0.785  | +0.2170 | +0.619
 3 |  0.3780 |  1.163  | +0.2100 | +0.829
 4 |  0.4101 |  1.573  | +0.2030 | +1.032
 5 |  0.4345 |  2.008  | +0.1925 | +1.225
 6 |  0.4550 |  2.463  | +0.1750 | +1.400   ← 拐点：斜线走完，到最高点
 7 |  0.0233 |  2.486  | -0.2450 | +1.155   ┐
 8 |  0.0239 |  2.510  | -0.2800 | +0.875   │ 横杠段：垂直几乎不动
 9 |  0.0245 |  2.534  | -0.2975 | +0.578   │ 水平一路向左
10 |  0.0251 |  2.560  | -0.3150 | +0.263   │
11 |  0.0257 |  2.585  | -0.3150 | -0.052   ┘
```

**总计：垂直 2.585°，水平 峰值 +1.400° → 收尾 -0.052°。**
斜线段 `(0,0) → (1.40, 2.46)`，横杠段 `(1.40, 2.46) → (-0.05, 2.59)` —— 横杠末端越过了斜线起点，
所以笔迹确实是一个「7」。

### 3.3 两把枪的差异一览

| 指标 | S 型 | 7 字型 | 差异 |
| --- | --- | --- | --- |
| 12 发垂直累计 | 2.342° | 2.585° | +10% |
| 水平峰值 | +0.655° | +1.400° | +114% |
| 水平最大单发 | 0.135° | 0.313° | +132% |
| 水平收尾 | -0.343° | -0.052° | 7 字型几乎回到中线 |
| 拐点 | 6 发后换向 | 7 发后换向 + 垂直转平 | — |

---

## 4. 关卡默认加载

`Config/DefaultEngine.ini`：

```ini
[/Script/EngineSettings.GameMapsSettings]
GlobalDefaultGameMode=/Game/B_LyraGameMode.B_LyraGameMode_C
GameInstanceClass=/Game/B_LyraGameInstance.B_LyraGameInstance_C
GameDefaultMap=/Game/System/FrontEnd/Maps/L_LyraFrontEnd.L_LyraFrontEnd        ; 未改，仍是前端流程
EditorStartupMap=/ShooterCore/Maps/L_ShooterPerf.L_ShooterPerf                  ; ★已改
```

- **打开工程（编辑器）就落在 L_ShooterPerf。** 需要重启编辑器才生效。
- `GameDefaultMap`（打包/独立运行时的入口）**故意没动** —— 改成 ShooterPerf 会绕过
  Lyra 的前端菜单与 Experience 流程，属于另一件事，没你的话不改。

---

## 5. 想改怎么改（三条路）

### 5.1 直接改资产（推荐，最快）

Content Browser → `Content/Weapons/Recoil/DA_Recoil_Rifle_S` 或 `DA_Recoil_Rifle_7`
→ 打开，改 `Pattern` / `RecoilPerShot_*` / `VerticalKickCurve` → PIE 里立刻生效。

- **Pattern 只描述形状**（X ∈ [-1,1] 右为正、Y ∈ [0,1]），"渐强"交给 `VerticalKickCurve`。
- 改完 `PatternPoints` 的元素个数后，`PatternLength` 会自动被夹回范围内（`PostEditChangeProperty`）。
- 形状想镜像（"7"的横杠朝另一侧）：**把 Pattern 里所有 X 取反**即可。
- 形变幅度想整体放大 / 缩小：只动 `RecoilPerShot_Horizontal`（不影响垂直节奏）。

### 5.2 手算复核

第 3 节的表就是手算出来的，公式只有三条：

```
垂直增量 = RecoilPerShot_Vertical   × PatternY(i) × VerticalKickCurve(i) × 姿态倍率
水平增量 = RecoilPerShot_Horizontal × PatternX(i)                        × 姿态倍率
累计     = 逐发求和（相机链，见 LyraRecoilState.h 的两条输出链注释）
```
曲线是**线性插值**不是阶跃，取值别取错。

### 5.3 跑自动化测试

在这台机器上**不需要关编辑器、不需要编译** —— UE 编辑器里的 MCP 工具可以直接跑：

```
AutomationTestToolset.DiscoverTests()
AutomationTestToolset.RunTestsByFilter("StartsWith:Lyra.Recoil")
```

当前结果：**19/19 通过**。命令行老路子（`Docs/Recoil/Tools/run-recoil-tests.ps1`）仍然可用。

### 5.4 射速 / 弹容 / 备弹（2026-09-17 补充）

后坐力之外，这三个数值也常一起调。**它们不在同一层，这是最容易找错的地方。**

| 想改的 | 改哪 | 当前值 |
| --- | --- | --- |
| 射速（连发间隔） | `ShooterCore/Weapons/Rifle/GA_Weapon_Fire_Rifle_Auto` 的 `Fire Delay Time Secs` | `0.12`（≈ 8.33 发/秒） |
| 是否全自动 | 同上，`Auto Rate` | `1`（1 = 全自动） |
| 弹匣上限 | `ID_Rifle` / `ID_Rifle_7` 的 `InventoryFragment_SetStats` → `Lyra.ShooterGame.Weapon.MagazineSize` | `30` |
| 出生时弹匣内 | 同上 → `MagazineAmmo` | `30` |
| 备弹总数 | 同上 → `SpareAmmo` | `60` |
| 重装速度 | `GA_Weapon_Reload_Rifle` 的 `Play Rate` | `1` |

三条硬约束：

1. **射速在能力层，不在武器实例层。** Lyra 原版 `ULyraRangedWeaponInstance` 里**没有**
   `RoundsPerSecond` 这类字段，别在 `B_WeaponInstance_Rifle` 里翻。
   公式：`射速(RPS) = 1 / FireDelayTimeSecs`。
2. **两把枪共用同一个 `AbilitySet_ShooterRifle`**，所以直接改 `GA_Weapon_Fire_Rifle_Auto`
   会**两把一起变**。要分开必须复制 GA + AbilitySet，再在对应 `WID_*` 的
   `AbilitySetsToGrant` 里替换。
3. **弹容/备弹每把枪各自独立**（各在自己 `ID_*` 的 SetStats 里），改一把不影响另一把。
   但 `InitialItemStats` 只在 `OnInstanceCreated` 写入一次 —— **改完必须重新 PIE**，
   已拿在手上的实例不会吃新值。

各武器当前弹药基线：Rifle `30/30/60`、Rifle 7 `30/30/60`、Pistol `12/12/48`、Shotgun `8/8/16`。

操作手册见 `Docs/Recoil/后坐力系统调试.html` 第 6 节。

### 5.5 Roll 震屏（2026-09-17 新增）

两把枪的 Pitch/Yaw 之外，还各自带一组**独立的 Roll 震屏参数**（`Recoil | RollShake` 分类）。
完整说明看 → **[08_CameraRollShake.md](08_CameraRollShake.md)**。速查：

| 想改的 | 参数 | S 型 / 7 字型当前 |
| --- | --- | --- |
| 有没有横滚抖动 | `bEnableRollShake` | 都 `true` |
| 抖多大 | `RollShake_Amplitude` | 都 `0.6` 度 |
| 抖多久 | `RollShake_Duration` | 都 `0.22` 秒 |
| 抖多快（一个来回） | `RollShake_Period` | 都 `0.055` 秒（≈ 4 个来回） |
| 抖动乱不乱 | `RollShake_PhaseJitter` | 都 `0.35` 弧度 |
| 连射越抖越狠 | `RollShake_AmplitudePerShot` / `_RampStartShot` / `_MaxAmplitudeBonus` | `0.05` / 第 4 发起 / 封顶 `+0.9` |

**三个关键区别（别和后坐力混）：**

1. **Roll 不累加、不休正。** 它是"每发重置时钟 → 衰减包络 × 周期项"的解析解，震动结束**精确归零**。
   Pitch/Yaw 是累加的、停火后停在稳态偏移。两者没有共享逻辑。
2. **Roll 不写进 CSV 的累加列。** `Lyra.Recoil.Dump` 的 CSV 里 `RollShake` 是**开火瞬间**的值，
   不是"本发累计" —— 列语义不同，别拿来和 `AccumulatedPitch` 做同样的运算。
3. **`Lyra.Recoil.RollShake <f>` 是纯显示层倍率。** 它只乘 Roll 施加到相机的那个值，
   **不改资产、不写回状态、不污染 CSV 与 Golden**。所以 `Lyra.Recoil.RollShake 0` 是最快的
   "关掉 Roll 做 A/B 对比"手段，比改资产再重载快得多。

### 5.6 单发 Pitch/Yaw 用的是哪套模型？（2026-09-17 补充 → 同日已落地两套）

> **✅ 2026-09-17 更新：已支持四段式，但它是可选模式。**
> 每把枪的资产上现在有 `SingleShotMode` 开关：
> `InstantWrite`（**结构默认值**，即本文 5.6 原描述的老模型）/ `Interpolated`（新增四段式）。
> **两把枪的实际分配（2026-09-20 起）**：`DA_Recoil_Rifle_S` 与 `DA_Recoil_Rifle_7` **都是 `Interpolated`**。
> 完整方案 → **[10_SingleShotInterpolation.md](10_SingleShotInterpolation.md)**；操作向 → **[后坐力系统调试.html §10](后坐力系统调试.html#model)**。

下表**对 `InstantWrite` 模式（默认）依然完全成立**：

| 你可能想调 | `InstantWrite` 该动哪个参数 | 注意 |
| --- | --- | --- |
| "打起来越来越跳" | `VerticalKickCurve`（横轴 = **发序号**）+ `RecoilPerShot_Vertical` | 这是**发序号轴**曲线，不是单发内的时间轴 |
| "松手后多久开始回" | `RecoveryDelay` | 这段期间偏移**冻结不动**，相当于文档的 t2 稳定段 |
| "回得是快是慢 / 曲线形状" | `RecoveryTime` + `RecoveryCurve` | `RecoveryCurve` 横轴 = 归一化回正时间 |
| "回不到零，留一点" | `RecoilReturnRatio` | 刻意设计，不是 bug |
| "抬起来要有一顿 / 先回弹一下" | **`InstantWrite` 做不到** → 把该枪改成 `SingleShotMode = Interpolated` | 改完**必须重导 Golden** |

**切到 `Interpolated` 后多出来的 6 个参数**（`Recoil | SingleShot` 分组，仅该模式下激活）：

| 参数 | 默认值 | 调它管什么 |
| --- | --- | --- |
| `SingleShotMode` | `InstantWrite` | 模型总开关 |
| `LiftDuration` | `0.045` s | 上升斜坡耗时（**0 = 退化回瞬时**） |
| `ReboundDuration` | `0.030` s | 回弹段时长 |
| `ReboundRatio` | `0.72` | **"一顿"的深度**，回弹到峰值的比例；越接近 1 越像老模型 |
| `LiftCurve` | Ease-Out | Lift 段形状（快起慢收），横轴 = 段内归一化时间 |
| `ReboundCurve` | 线性 | Rebound 段形状 |

**两套模型下都不变的三条**（所以老 Golden 没被改坏）：

- `Settle` 段**复用** `RecoveryDelay`，没有新增"稳定段时长"参数；
- `Drop` 段**复用** `RecoveryTime` + `RecoveryCurve`；
- CSV / Golden / 测试读的仍是**逻辑偏移** `AccumulatedPitch/Yaw`，插值只多了一条给相机用的补间输出。

**最容易混的一点**：`VerticalKickCurve` 的分段是**按第几发**分的，
`LiftCurve` / `ReboundCurve` 的分段是**按这一发内的第几毫秒**分的。**两者不是一回事，两套模型下都成立。**

---

## 6. 已知问题

### 6.1 `Lyra.Recoil.Scale.AffectsDump` 偶发失败 —— 2026-09-17 已修

- 旧现象：`The two dumps are different files (..._213043_598.csv / ..._213043_598.csv)`，
  随后 17 条"half"断言连锁失败。
- 真因：`LyraRecoilDebug` 用**毫秒级时间戳**命名 CSV。同一毫秒内连续 Dump 两次 →
  第二次覆盖第一次 → 两个路径相同。机器快的时候（这套测试本地跑完只要 0.15 秒）就撞得上。
- **修法**：`ULyraRecoilDebug::DumpShotHistoryToCsv` 在落盘前检查目标文件是否已存在，
  存在就顺延编号（`_2` / `_3` / …）。这样"每次 Dump 产出独立文件"变成结构性契约，
  重复路径不可达 —— 不再是"概率修好了"，而是"撞不上"。
- 改动文件：`Source/LyraGame/Weapons/Recoil/LyraRecoilDebug.cpp`。已编译，复跑 `Lyra.Recoil` 19/19 通过。
- **直接证据**：连跑 4 轮，第 3 轮两次 Dump 落在同一毫秒 285，产出
  `RecoilDump_20260917_214709_285.csv` 与 `RecoilDump_20260917_214709_285_2.csv`
  —— 修复前第二份会直接覆盖第一份，正是老 bug 的成因。四轮全部 0 fail。

---

## 7. 需要你拍板

> 2026-09-17 已拍板：A 按现在这样（斜向右上 → 顶部向左横）；B bot 一起变无所谓；
> C 不保留手枪；D 原版留着；E `GameDefaultMap` 不改；F flaky 测试已修。
> 下表保留原问题，便于回溯。

| # | 事项 | 我的默认选择 | 影响面 |
| --- | --- | --- | --- |
| A | **"7 字型"的方向**：现在是「斜向右上 → 顶部向左横」；如果你要的是「竖着上去 → 顶部向右横」，把两份 Pattern 的 X 整体取反 / 交换符号即可 | 按数字 7 的正常笔顺 | 手感差异明显，改起来 1 分钟 |
| B | **初始装备里有 20 个 bot 也一起变了** | 没做区分 | `B_Hero_ShooterMannequin` 是玩家和 bot 共用的英雄蓝图，`InitialInventoryItems` 一改两边都变。原来 bot 拿手枪，现在拿步枪（每只 2 把）。想要"只有玩家换步枪、bot 保持手枪"就得改代码或给 bot 换英雄蓝图 |
| C | **出生手上的第二把要不要保留手枪** | 已按你说的做成「就两把步枪」 | 快捷栏 `NumSlots = 3`，第 3 格空着 |
| D | **原版 `DA_Recoil_Rifle` 怎么处理** | 原样留着（Golden 基准） | 现在场上的两把枪都不用它。要不要删 / 要不要让它当第三把"中等"步枪 |
| E | `GameDefaultMap` 是否也要指到 `L_ShooterPerf` | 没改 | 改了就绕过 Lyra 前端菜单流程 |
| F | 是否要我修 6.1 那个 flaky 测试 | 没改（要编译） | 需要你关掉编辑器让我编一次 |

---

## 8. 验收清单（手动那部分交给你）

1. 重启编辑器 → 应该直接落在 `L_ShooterPerf`。
2. PIE → 手上已经有枪（槽位 0 = S 型那把）。
3. 切到槽位 1 → 应该明显感觉：抬枪更猛、向右甩得更开、最后顶部往左一横。
4. 控制台 `Lyra.Recoil.DebugDraw 1` 看弹道形状；`Lyra.Recoil.Dump` 导 CSV，
   用 `Docs/Recoil/Tools/plot-recoil-csv.py` 出图对比（支持多份 CSV 叠加）。
