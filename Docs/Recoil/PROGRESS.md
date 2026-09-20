# 后坐力系统 · 当前进度指南

> 这份文档是给大祥老师用的**操作手册 + 状态看板**。
> 每次推进都会更新，看这一份就知道"现在到哪了、怎么复跑、要拍板什么"。
>
> 最后更新：2026-09-20（P0–P5 完成；P7 两把步枪落地；P8 Roll 震屏；P9 单发插值；
> **P10 连发累积失效修复**；**P12 垂直钳制实时抵扣压枪量**；
> **P13 散布并入后坐力配置表** —— 见 [12_SpreadInProfile.md](12_SpreadInProfile.md)）

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

| 槽位 | 武器 | 后坐力资产 | 形状 | 强度 | 单发模型 |
| --- | --- | --- | --- | --- | --- |
| 0（出生默认） | `ID_Rifle` | `DA_Recoil_Rifle_S` | S 型（一个弯） | 小：12 发垂直 2.34° | **`Interpolated`** |
| 1 | `ID_Rifle_7` | `DA_Recoil_Rifle_7` | 7 字型（斜线 + 顶部横杠） | 稍大：12 发垂直 2.59°，水平峰值 1.40° | `InstantWrite` |

同时 `Config/DefaultEngine.ini` 的 `EditorStartupMap` 已指向 `/ShooterCore/Maps/L_ShooterPerf`。

**P10 已修复（2026-09-20）**：`Interpolated` 单发模型有个**连发致命缺陷** —— 回弹/回正锚在"绝对峰值"上，
连发时每发把整条已累加偏移按 `ReboundRatio`(0.72) 乘回一次 → 几何衰减 → 30 发只涨到 ~1.05° 就不再动。
玩家感知就是「**连发时一压枪后坐力就像变成 0**」。修复：锚点改为「**基底 + 本发幅度**」，
连发偏移恢复单调上涨、30 发内爬到 `MaxVerticalKick=4.0°` 封顶（首越 ≈ 第 23 发，与云文档 §7.4 一致）。
**`InstantWrite` / Golden / CSV / 30 个既有用例零行为变化**（`RecoveryBase=0` 时新旧公式逐位等价；单发 diff=0.0000000）。
详见 → **[11_BurstAccumulationFix.md](11_BurstAccumulationFix.md)**。✅ 已编译通过（`Result: Succeeded`，0 error）**+ `Lyra.Recoil` 30/30 全绿（Failed: 0）**；⏳ 仅剩 PIE 手测。

**P12 已修复（2026-09-20 追加）**：P10 修复后暴露的下一个手感问题 —— **压枪的人会提前撞到垂直钳制上限**，
之后连发不再推高镜头，体感"压着枪打着打着后坐力就没了"。
根因：`MaxVerticalKick` 钳的是**裸累加偏移**，而不是**镜头相对起枪点的净抬升**。
修复：钳制边界改为 `MaxVerticalKick + min(压枪量, MaxVerticalKick)` —— 抵扣量由武器实例每帧写入
（读的是**不含后坐力偏移**的 `ControlRotation.Pitch`，基线取本梭首发俯仰）。
压枪量默认 `0` ⇒ 不压枪时新旧公式**逐位相同**，Golden / CSV / 既有全部用例零回归。
⚠️ **Live Coding 接不住本次改动**（新增了 `UPROPERTY` 成员 = 改了反射类布局），必须关编辑器整包重编。
✅ **已编译通过**（`Result: Succeeded`，0 error）**+ `Lyra.Recoil` 37/37 全绿（`Failed: 0`）**，Golden 5 份 md5 逐位未变；
⏳ 仅剩 PIE 手测（连发 + 持续下压，盯面板 `CapV` / `aimComp`）。
详见 → **[11_BurstAccumulationFix.md](11_BurstAccumulationFix.md) §12**。

**开发阶段本身到此结束** —— 2026-09-17 决定 **不做联机**，原 P6「联机同步」已从计划中剔除。
剩下是 P7（继续调参 + 固化）、P8/P9（镜头与单发模型收尾），以及一直卡着的**手动验收**（需要你人在机器前做一次 PIE 手测）。

```
P0 ✅ 勘察与接口冻结   P1 ✅ 数据层        P2 ✅ 运行时核心+相机Kick
P3 ✅ 弹道 Pattern     P4 ✅ 恢复+姿态倍率  P5 ✅ 调试与可视化工具链
P6 联机同步 —— 已剔除（本项目不做联机）      P7 ⬜ 手感调参 + 固化（可选）
P8 ✅ 相机镜头 Roll 震屏 + 镜头模式（2026-09-17 追加）
P9 ✅ 单发插值模型 · 两套并存（2026-09-17 追加）   测试 30/30 全绿
P10 ✅ 连发累积失效修复（Interpolated 锚点）       测试 30/30 全绿
P12 ✅ 垂直钳制实时抵扣压枪量                      编译通过 + 37/37 全绿
```

---

## 1. 必须知道的坑（本机环境）

### 坑 1：编译失败的真正原因 —— UBA 写不进 `C:\ProgramData`（2026-09-20 更正）

> **旧版这一节写的是"加 `-NoUBA` 就行"，那是错的。** `-NoUBA` 只解决了一半，另一半是路径与调用侧。

**必须同时满足两条：**

1. **加 `-UBARootDir="E:\TPSGunsDemo\Saved\UBACache"`** —— 把 UBA 存储根目录从
   `C:\ProgramData\Epic\UnrealBuildAccelerator` 搬走。
2. **从 Bash 工具侧发起构建**（不要走 PowerShell 工具）—— 宿主沙箱对该路径的写入会拦；
   Bash 工具在沙箱拒绝后会被宿主**自动放行重跑**，PowerShell 工具没有这个行为。

`build.ps1` 已带 `-UBARootDir`（默认 `E:\TPSGunsDemo\Saved\UBACache`，可用同名参数覆盖）；
Bash 侧的调用模板放在 `%TEMP%\tps_recoil\run_build.bat`，形如：

```
cmd //c "C:\Users\yeyuxiang\AppData\Local\Temp\tps_recoil\run_build.bat >> <log> 2>&1"
```

**漏掉第 1 条的报错**（看着像代码坏了，其实代码是好的）：

```
UbaSessionServer - ERROR opening file C:\ProgramData\Epic\UnrealBuildAccelerator\memgroups
                    for write after retrying for 20 seconds (Access is denied.)
Result: Failed (OtherCompilationError)        ← 但 0 error / 0 warning
```

**为什么 `-NoUBA` 挡不住：** UE 5.8 的 `ExecutorFactory.GetUBAExecutor()` **无论如何都构造 `UBAExecutor`**
（`Engine/Source/Programs/UnrealBuildTool/Executors/ExecutorFactory.cs` L46–53）：

> We always use the UBA executor, but we disable detouring to mirror legacy behaviour if the config disables it.

`-NoUBA` 只是 `Config.bAllowDetour = false`（`BuildConfiguration.cs` L54）—— local executor 照样启动
session server 并写 `memgroups`。UBA 存储路径的选取见 `UBAExecutor.cs` L257–284
（`UBAConfig.RootDir` → 环境变量 `UBA_ROOT` / `BOX_ROOT` → `%ProgramData%\Epic\UnrealBuildAccelerator`）。

**噪声 vs 致命 —— 只认 `Result:` 行：**

```
UbaSessionServer - SetFileInformationByHandle (FileDispositionInfo) failed on ... \memgroups (Access is denied.)
Result: Succeeded          ← 判据在这里
```

`memgroups` 的 **open-for-write** 成功即可构建；结尾那条 `FileDispositionInfo` 是
**关闭时的删除清理**被拦，**无害噪音**（`Trace.uba` 同理）。

> 2026-09-20 实测：`-UBARootDir` + Bash 侧发起 → **`Result: Succeeded`，25 actions，76.98s**。
> 同一命令走 PowerShell 工具 → 仍然是 `memgroups ... Access is denied`。

### 坑 2：`-ExecCmds` 里塞多个分号命令会挂住编辑器

```powershell
# ❌ 实测不会执行，编辑器一直挂着直到被杀（Quit 也不生效）
... -ExecCmds="Lyra.Recoil.Dump; Lyra.Recoil.ReloadProfile; Quit"

# ✅ 单命令 + 内建退出条件（自动化测试就是这么跑的）
... -ExecCmds="Automation RunTests Lyra.Recoil" -TestExit="Automation Test Queue Empty"
```

要跑多个命令就**拆成多次调用**。

### 坑 3：Bash 工具需要先 `export PATH`

`ls` / `dirname` / `cd` 会报 `command not found`，**每条命令前加前缀即可**：

```bash
export PATH="/c/Users/yeyuxiang/.workbuddy/binaries/PortableGit/versions/1.2.0/usr/bin:/c/Windows/System32:/c/Windows"
```

绝对路径 Python（venv 已装 python-docx / lxml / bs4）：
`C:\Users\yeyuxiang\.workbuddy\binaries\python\versions\3.13.12\python.exe -c "..."`
（注意：**PowerShell 工具的 stdout 不回传**，要拿输出就把命令结果重定向到文件再读。）

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

> ⚠️ **但注意**：如果本次改动**动了反射类的布局**（新增/删除 `UPROPERTY` / `UFUNCTION` 成员），
> Live Coding **接不住**（按 `Ctrl+Alt+F11` 也会提示需要重启）。这类改动必须关编辑器整包重编。

### 坑 6：UHT 会「静默不重跑」，留下陈旧的 `.generated.h` → 报一堆莫名其妙的错

**症状**（2026-09-20 实测）：

```
LyraRecoilDebug.h(43,14): error C2143: 语法错误: 缺少";"(在"<class-head>"的前面)
LyraRecoilDebug.h(42,1):  error C4430: 缺少类型说明符 - 假定为 int        ← UCLASS()
LyraRecoilDebug.h(45,2):  error C4430: 缺少类型说明符 - 假定为 int        ← GENERATED_BODY()
LyraRecoilDebug.gen.cpp(NN,1): error C2039: "execGetGlobalScale": 不是 "ULyraRecoilDebug" 的成员
                        （exec* 函数成串报「不是成员」）
```

**机理** —— 别被「找不到符号」骗了，根本不是代码写错：

1. UE 的 `UCLASS()` / `GENERATED_BODY()` 展开成的是**带行号拼出来的宏**：
   `UCLASS()` → `BODY_MACRO_COMBINE(CURRENT_FILE_ID,_,__LINE__,_PROLOG)` → 形如
   `LYRAGAME_LyraRecoilDebug_h_42_PROLOG`。
2. 这些带行号的宏定义在 **`<Class>.generated.h`** 里，是 **UHT** 生成的。
3. 一旦在 `UCLASS()` **之前**插入/删除任何一行（哪怕只是加一行 `*` 文档注释），
   `UCLASS()` 的行号就变了；若 UHT 没重跑，`.generated.h` 里只有旧行号的定义
   → `UCLASS()` 展开成一个**未定义标识符** → `C4430 假定为 int` → 后面全崩。

**为什么以前没暴露**：改在 `USTRUCT` / `GENERATED_BODY` **之后**（例如只增删结构体成员）
**不会**移动这些行号，陈旧生成头恰好仍对得上 —— 编译能过，只是**反射是旧的**（更难发现）。

**诊断一条命令**（比对时间戳，`.generated.h` 比 `.h` 旧就是它）：

```bash
stat -c '%y %n' Source/LyraGame/Weapons/Recoil/LyraRecoilDebug.h \
                Intermediate/Build/Win64/UnrealEditor/Inc/LyraGame/UHT/LyraRecoilDebug.generated.h
```

**处理**（删除生成代码，强制 UHT 重跑；LyraGame 会整模块重编）：

```bash
rm -rf Intermediate/Build/Win64/UnrealEditor/Inc/LyraGame
# 必要时连目标文件一起清，保证依赖图重建：
rm -rf Intermediate/Build/Win64/x64/UnrealEditor/Development/LyraGame
```

**正常日志里应该能看到 UHT 步骤**（`Parsing headers for LyraGame` / UHT 动作）。
如果一个只有 11 个 action、全是 `Compile`/`Link`、**看不到任何 UHT 行**的构建突然报这类错，
先怀疑这条，别去改代码。

### 另外两条

- **USTRUCT 内不能有 `UFUNCTION`** —— UHT 直接报错。
- **`UE_LOG` / `AddError` / `AddInfo` 一律写英文** —— 控制台按 ANSI 输出，中文全乱码，日志就没法当证据。C++ 注释与文档可以中文。

---

## 2. 标准命令（直接复制就能跑）

> **⚠️ 2026-09-20 路径修正**：工程与引擎已从 `D:\` 迁到 **`E:\`**
> （`E:\TPSGunsDemo\TPSGunsDemo.uproject`、`E:\UE_5.8`）。本文命令已按 `E:\` 更新；
> `Tools\*.ps1` 的默认参数也已同步（`-NoUBA` 仍必需）。历史文档（`00_Recon.md` / `04` / `08` / `09` / `10` /
> `Acceptance\*`）里的 `D:\TPSGunsDemo\TPSGunsDemo\...` 是迁移前的记录，**按其照抄会失败**，请以本节为准。

脚本目录：`E:\TPSGunsDemo\Docs\Recoil\Tools\`

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
powershell -ExecutionPolicy Bypass -File "E:\TPSGunsDemo\Docs\Recoil\Tools\run-all-checks.ps1"

# 也可显式传引擎 / 工程（脚本默认值即为下列 E: 路径，一般不用传）
powershell -ExecutionPolicy Bypass -File "E:\TPSGunsDemo\Docs\Recoil\Tools\run-recoil-tests.ps1" `
    -EngineRoot "E:\UE_5.8" -Project "E:\TPSGunsDemo\TPSGunsDemo.uproject"
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
| `Lyra.Recoil.SpreadDebug 0/1` | **【P13】** 散布（锥角）实时面板：走哪条链路 / 裸锥角 → 最终锥角 / 瞄准与移动倍率 / 当前姿态四参数 / 换算好的半角。与 `Debug`、`RollDebug` 三者独立可同开 |
| `Lyra.Recoil.Dump` | 导出本轮连发到 `Saved/RecoilDump_<时间戳>.csv`（表头 8 列，含 `RollShake` 与 **`SpreadAngle`**） |
| `Lyra.Recoil.ReloadProfile` | 从磁盘强制重载资产（**编辑器里改资产不需要它**，见 §8.4） |

> **注意**：散布的**数值配置**从 P13 起已经并入 `DA_Recoil_*` 的 `Recoil | Spread` 分类（不在
> `B_WeaponInstance_*` 上了）；但**它没有自己的总开关 CVar** —— 开关是资产上的
> `bEnableProfileSpread`，且 **`Lyra.Recoil.Enable 0` 不会关掉散布**（两个系统刻意解耦，
> 便于"只调散布、摘掉后坐力"的 A/B 对比）。

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
| **P10** | **连发累积失效修复（Interpolated）** | **待 PIE 手测** | ✅ **编译通过 + 30/30 全绿** | ⏳ | [11_BurstAccumulationFix.md](11_BurstAccumulationFix.md) §7 |
| **P12** | **垂直钳制实时抵扣压枪量**（编号对齐文档 §12，未占 P11） | **待 PIE 手测** | ✅ **编译通过 + 37/37 全绿 + Golden md5 未变** | ⏳ | [11_BurstAccumulationFix.md](11_BurstAccumulationFix.md) §12 |
| **P13** | **散布并入后坐力配置表（姿态-角度直接模型）** | **待 PIE 手测** | ✅ **编译通过 + 37/37 全绿**（其中 7 个是新加的） | ⏳ | [12_SpreadInProfile.md](12_SpreadInProfile.md) §9 |
| **P14** | **回正目标减去本梭累计压枪量**（编号对齐文档 §13，未占 P11） | **待 PIE 手测** | ✅ **编译通过 + 37/37 全绿 + Golden md5 未变**（用例喂 `cover=0`，证的是零回归；数值行为目前**只有仿真证据**） | ⏳ | [11_BurstAccumulationFix.md](11_BurstAccumulationFix.md) §13 |
| ~~P6~~ | ~~联机同步~~ **已剔除** | 不做 | — | — | 2026-09-17 决定：本项目不做联机 |

**测试用例清单（37 个 = P0–P5 的 19 个 + P8 的 5 个 + P9 的 6 个 + P13 散布的 7 个）**

> ℹ️ **2026-09-20 实际跑出 37/37 全绿**（`LogAutomationCommandLine: ...Automation Test Queue Empty 37 tests performed.`）。
> 从 30 变 37 是因为新增了 **「资产散布（Spread）」** 特性（`LyraRecoilSpreadTest.spec.cpp`，7 个用例）——
> 它与 P12 的压枪抵扣无关，但同一次跑完。该特性的独立文档见 → **[12_SpreadInProfile.md](12_SpreadInProfile.md)**。
> ⚠️ **它不会自动生效**：5 份 `DA_Recoil_*` 资产**尚未重新生成**（磁盘上仍是旧的），
> 新模型要跑 `gen-recoil-assets.ps1 -Force` 或手工打开 `bEnableProfileSpread` 之后才会被用到；
> 在此之前运行时走的是旧 heat 链路（这是刻意的零回归默认）。

> **列契约变更（P8）**：CSV 从 6 列变 7 列（新增 `RollShake`），
> `Lyra.Recoil.Dump.HeaderSchema` 与 `Lyra.Recoil.Dump.RowCountAndValues` 已同步更新。
> 契约常量收敛在 `LyraRecoilDumpTest::ExpectedHeader` / `ExpectedColumnCount` 一处，
> 以后改 CSV 只需改那里。
>
> **列契约变更（P13）**：CSV 从 7 列变 **8 列**（新增 **`SpreadAngle`** = 本发实际使用的散布锥角，
> 度·全锥角，含姿态/瞄准/移动倍率；**未启用资产散布时恒为 0**，有一条专门的断言钉住）。
> `HeaderSchema` / `RowCountAndValues` 已同步更新并全绿（见 `LyraRecoilDebug.h` 的列顺序注释）。
> 实测落盘表头：
> `ShotIndex,VerticalKick,HorizontalKick,AccumulatedPitch,AccumulatedYaw,TimeSinceFire,RollShake,SpreadAngle`
>
> **P9 / P12 未动 CSV 契约**：P9 的 `TimeSinceFire` / `RecoveryProgress` 语义不变，
> 插值模式下相机链改读 `CameraOffsetPitch/Yaw`，CSV 仍写 `AccumulatedPitch/Yaw`（逻辑偏移）；
> P12 的抵扣只改钳制边界，不改任何列的含义。

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
| `Lyra.Recoil.Spread.{AccumulateAndClamp, DisabledIsNoOp, PlayerMultipliers, PoseSwitch, ProfileParams, Recover, ToggleBackIsClean}` | **散布**（7） |

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
| **P10 `DA_Recoil_Rifle_S` 30 发连发峰值** | 修复前 **1.0496°**（几何衰减封顶）→ 修复后 **4.0446°**（触 `MaxVerticalKick` 上限） |
| **P10 上限首越弹序** | 第 **23** 发（修复前永不可达） |
| **P10 单发等价性** | 1 发逐点 `max\|diff\| = 0.0000000`（`InstantWrite` 与单发逐位不变） |
| **P10 `Interpolated` 12 发垂直累计** | ≈ **1.67°**（= 0.72 × 裸和 2.342°；口径差异见 [11](11_BurstAccumulationFix.md) §9.1） |
| **P12 压枪抵扣硬顶** | `2 × MaxVerticalKick` = **8.0°**（`DA_Recoil_Rifle_S`；抵扣量自身夹在 `1 × MaxV`） |
| **P12 `cover = 0` 等价性** | 裸偏移峰值 4.0000 / 净抬升峰值 4.0000 / 单子步最大增量 0.18922 —— 与旧口径**逐位相同** |
| **P12 `cover = 4.0°` 净抬升峰值** | 旧口径 **0.0000**（完全封死，"后坐力没了"）→ 新口径 **1.1506** |
| **P12 连发自然峰值**（钳制边界 ≥ 6.0° 时不参与） | **5.1506°**（`DA_Recoil_Rifle_S` 30 发 @0.12s，60fps） |
| **P12 净抬升判据式** | 旧 `net = min(MaxV, 自然峰值) − cover`；新 `net = min(MaxV + min(cover, MaxV), 自然峰值) − cover` |
| **P13 散布单位口径** | **全锥角（直径角），度**。唯一半角换算点：`LyraGameplayAbility_RangedWeapon.cpp` 的 `ActualSpreadAngle * 0.5f` |
| **P13 散布参数字段数** | **20**（1 总开关 + 3 姿态 × 4 + 玩家侧 7）；分布在 4 个分类：`Recoil\|Spread` / `\|Standing` / `\|Crouching` / `\|JumpingOrFalling` / `\|Player` |
| **P13 新模型默认开关** | `bEnableProfileSpread = false` ⇒ **零回归**（`DisabledIsNoOp` / `ToggleBackIsClean` 两个用例钉住） |
| **P13 步枪首版"到顶弹序"** | `(2.20 − 0.35) / 0.28 ≈ 6.6` → **第 7 发到顶**（`DA_Recoil_Rifle`；仅脚本默认值，未实机验证） |
| **P13 步枪首版"打满收回时长"** | `(2.20 − 0.35) / 2.00 = 0.925 秒` |
| **P13 移动惩罚** | `move` 从 0.50（速度 ≤ 80cm/s）线性升到 1.00（≥ 100cm/s）⇒ 跑动时锥角是站定的 **2 倍** |
| **P13 开镜收益** | `aim` 从 1.00 滑到 0.60 ⇒ **−40%**（步枪） |
| **P13 CSV 第 8 列** | `SpreadAngle`，实测落盘表头与数值已验证（未启用资产散布时恒为 `0.000000`） |
| **P13 资产是否已重生成** | **❌ 尚未**（`Content/Weapons/Recoil/*.uasset` 仍是 09-18 时间戳）⇒ PIE 里跑的还是旧 heat 模型 |
| **P14 新增字段** | `FRecoilRuntimeState::RecoveryCoverPitch`（本梭累计压枪量，度；**默认 `0.0f` ⇒ 零回归**） |
| **P14 累积规则** | `Advance()` 末尾 `RecoveryCoverPitch = max(RecoveryCoverPitch, AimCompensationPitch)` —— 单调不减、停火后冻结；新一梭 / `Reset()` 清零 |
| **P14 回正目标式** | `T = RecoveryBase + (RecoveryPeak − RecoveryBase) × RecoilReturnRatio − RecoveryCoverPitch`（水平 Yaw **不**抵扣） |
| **P14 `cover = 0` 等价性** | `Interpolated`：峰值 `4.0446` / 残留 `4.045`，与 P12 口径**逐位相同**；`InstantWrite`：峰值 `4.0000` / 残留 `0.6000`，同样逐位相同 |
| **P14 `cover = 4.0°` 残留**（`Rifle_S` 30 发 @0.12s，60fps） | P12 裸残留 `5.327` / 净 `1.327` → P14 裸残留 **`0.260`** / 净 **`−3.740`**（允许负残留） |
| **P14 抵扣是否严格线性** | **`InstantWrite` 基本是**（`Δ = −cover`，`cover ≥ 3` 后附加 −0.10~−0.22）；**`Interpolated` 不是**（`cover=4` 时 `Δ = −5.067`）—— 原因：`Rifle_S` 射速 0.12s = `RecoveryDelay`，30 发里触发 **7 次中途中止回正**，每次都扣一遍并通过 `InterpBasePitch = AccumulatedPitch` 反馈进下一发基底 |
| **P14 无钳制自然峰值（校准 §12.5）** | P10 口径下是 **`5.5795`**；§12.5 写的 `5.1506` 是 pre-P10 公式（`sim8` 的 `base` 未赋值）算出来的，**数字以 5.5795 为准** |
| **P14 仿真脚本** | `%TEMP%\tps_recoil\sim11.py`（Interpolated）/ `sim12.py`（溯源 FULL vs REC）/ `sim14.py`（自然峰值）/ `sim15.py`（InstantWrite） |

**P14 首版交付踩的坑（已修）：** 三处多行减法写成 `... * Ratio;\n − cover;` ——
**分号提前结束语句，`− cover;` 变成一条丢弃结果的表达式语句**。
`- x;` 是合法 C++ ⇒ **编译 `Result: Succeeded`、0 warning，减法静默失效**。
自查：`grep -n "RecoilReturnRatio;$" LyraRecoilState.cpp` 不该出现 `*Pitch` 的行。

---

## 4. 代码地图

```
Source/LyraGame/
├── Weapons/
│   ├── Recoil/
│   │   ├── LyraRecoilTypes.h          【P0】枚举与结构体契约（已冻结；【P13】加 FRecoilSpreadParams）
│   │   ├── LyraRecoilProfile.h/.cpp   【P1】【P8】手感配置资产（唯一数值来源；含 Recoil|RollShake、Recoil|Spread 参数组）
│   │   ├── LyraRecoilState.h/.cpp     【P2/P4】【P8】FRecoilRuntimeState —— 纯算法层，无 UWorld 依赖（【P13】含散布通道）
│   │   └── LyraRecoilDebug.h/.cpp     【P2/P5】【P8】CVar + 屏幕面板 + Roll 波形面板 + **散布面板** + 世界 DebugDraw + CSV 导出
│   ├── LyraRangedWeaponInstance.h/.cpp 【改】持有 RecoilProfile + RecoilState；Tick 驱动；相机修改器挂载（三轴）
│   │                                   【P13】★两条散布链路的分叉点 UsesProfileSpread()；旧 heat 字段移入 "Spread (deprecated)"
│   └── LyraGameplayAbility_RangedWeapon.cpp 【改】P2 加 1 行 AddRecoil()；P3 注入弹道偏移；【P13】发弹前留散布快照
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
    ├── LyraRecoilSpreadTest.spec.cpp  【P13】★新建 散布（Spread）组（7 个用例）→ [12_SpreadInProfile.md](12_SpreadInProfile.md)
    └── Data/RecoilGolden_*.json        【P3】Golden 基准（5 份：Rifle / Pistol / Shotgun / Rifle_S / Rifle_7）

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
├── 10_SingleShotInterpolation.md  【P9】单发插值模型（两套并存）实现方案 + 帧率不变性
├── 11_BurstAccumulationFix.md     【P10/P12】★连发累积失效修复 + 压枪抵扣（根因 / 三处改动 / 回归分析 / 仿真验证 / §12 压枪抵扣 / UHT 踩坑）
├── 12_SpreadInProfile.md          【P13】★散布并入后坐力配置表（姿态-角度直接模型）：动机 / 计算链 / 8 条设计决策 / 改动清单 / 缺口 / 待拍板
├── 后坐力系统调试.html             【P7】给人看的操作手册：测试步骤 / 改后坐力 / 新增枪 / Debug 开关效果 / **§11 散布（P13 整节重写）**
├── PROGRESS.md                    ← 你正在看的这份
├── Acceptance/P{0..5}_验收请求.md
├── Acceptance/P13_验收请求.md      【P13】★散布并入后座配置表的验收请求
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
| **散布也收进资产（唯一数值来源）** | 【P13】`Recoil\|Spread` 20 个字段进 `DA_Recoil_*`；移动/瞄准仍用倍率，姿态沿用 `EPoseState` 三态 | 12 号文档 §1 |
| **散布与后坐力解耦（可单独开关）** | 【P13】`bEnableProfileSpread` 只管散布，`Lyra.Recoil.Enable` 只管后坐力；互不影响 ⇒ 能只调一边做 A/B | 12 号文档 §3 / §4 |
| **换枪后锥角 = 站立基础角** | 【P13】在 `FRecoilRuntimeState::Reset()` 里给初值，不用旧模型"heat 取范围中点" | 12 号文档 §4.4 |
| **改散布数值不放宽"零回归"承诺** | 【P13】开关关闭时 `AddSpread/UpdateSpread/UpdateMultipliers` 走原代码路径，散布状态字段一个不读 | 12 号文档 §6.2 |

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

### P10 连发累积修复带来的待拍板（2026-09-20）

| # | 事项 | 我的默认选择 | 影响面 |
| --- | --- | --- | --- |
| 33 | **`Interpolated` 连发累积口径** —— 修复后 12 发 ≈ 1.67°（= 0.72 × 裸和）；[07 §3.1](07_TuningRecipe.md) 的 2.342° 是裸和口径 | 先按修复现状（保留回弹比 0.72 的自然累积），实机确认手感 | 若要与裸和严格一致，需把"视觉回弹"与"累积基底"解耦（新增累积场），属设计变更 |
| 34 | **4.0° 封顶 / 首越第 23 发**是否符合一个弹匣的节奏 | 保持 `MaxVerticalKick = 4.0`（与云文档 §7.4 默认值一致） | 影响连发强度上限与"打满一梭"的主观感受 |
| ~~35~~ | ~~**钳制上限要不要"减掉压枪量"**~~ —— **2026-09-20 实机复现后推翻**：压枪的人会提前撞上限，之后连发不再推高镜头。已按"钳制**净抬升**"落地，见 **P12** | ✅ **已改为「做」**（钳制对象从"裸偏移"改成"净抬升"；正反馈回路用"抵扣量夹 `1×MaxV`"限死） | 详见 [11_BurstAccumulationFix.md](11_BurstAccumulationFix.md) §12（§11 保留为推翻过程记录） |

### P12 带来的新待拍板（2026-09-20）

| # | 事项 | 我的默认选择 | 备选 / 影响面 |
| --- | --- | --- | --- |
| 36 | **抵扣量是否夹在 `1 × MaxVerticalKick`** | 夹（裸偏移硬顶 = `2 × MaxV` = 8.0°），给切枪回正留安全阀 | 不夹 ⇒ 裸偏移无硬顶，极端压枪下切枪回正会甩镜头 |
| 37 | **抵扣基线取"本梭首发俯仰"** | 是（判据与 `ApplyShot` 的"新连发"完全一致） | 取"上一次休火后的稳定俯仰" ⇒ 需要额外的休火期状态 |
| 38 | **水平方向（Yaw）是否同样抵扣** | 不做（水平没有"飞太高"的问题） | 做 ⇒ 需要另加一个基线字段 |
| 39 | **实机手感确认：压满时会不会发涩/抖动**（回路残留） | 需你本人在 PIE 里连发 + 持续下压确认；面板看 `CapV` / `aimComp` | 若发涩 ⇒ 把抵扣量夹到 `0.5 × MaxV` |

### P13 散布并入后坐力配置表带来的待拍板（2026-09-20）

> 完整版见 [12_SpreadInProfile.md §9](12_SpreadInProfile.md)。这里只列**需要你决定**的。

| # | 事项 | 我的默认选择 | 备选 / 影响面 |
| --- | --- | --- | --- |
| 40 | **是否把 5 份资产重新生成**（跑 `gen-recoil-assets.ps1 -Force`） | **建议先别跑** —— 先手工在资产上勾 `bEnableProfileSpread` 试一把，满意了再固化进脚本 | 直接跑 ⇒ 5 份资产的后坐力数值也会一起被脚本默认值覆盖（Rifle_S / Rifle_7 是手调过的，会丢） |
| 41 | **5 把枪的首版散布数值是否可用** | 我按武器类型的常规量级填的（步枪第 7 发到顶 / 手枪回正 3.5°/s / 霰弹基础 3.5°），**没做实机验证** | 需你在 PIE 里打几轮定稿。更稳的路径见 12 号文档 §8 末尾 |
| 42 | 参数单位用「全锥角」还是「半角」 | **全锥角**（与 Lyra 原生 `CurrentSpreadAngle` 一致，GA 里有现成的 `× 0.5`） | 改半角 ⇒ 要动 `GetCalculatedSpreadAngle()` 语义 + 准星，属破坏性改动 |
| 43 | 移动精度：速度 ramp 倍率 vs 新增 `Moving` 姿态 | **ramp 倍率**（后坐力侧零改动） | 新增姿态 ⇒ P4 用例 + 5 份 Golden 全部重导 |
| 44 | 是否补两个 CVar（`Lyra.Recoil.Spread 0/1` 总开关 + `Lyra.Recoil.SpreadScale <f>` 倍率） | **建议补**（成本极低，但调参体验差别很大 —— 现在 A/B 只能改资产） | 不补 ⇒ 每次对比都要去动资产 |
| 45 | 是否补"散布曲线"（DLC36 的 `bUseShootScatterCurve` + 两条曲线） | **先不做** —— 会削弱 CSV 的"可手算复核"性质 | 做 ⇒ 能做"越打加得越快"，但验收只能靠观感 |
| 46 | 是否补"姿态切换过渡"（蹲/跳瞬切会让锥角跳一下） | **先不做** —— 会破坏"每发按当下姿态取值"的口径（P4 判据依赖它） | 做 ⇒ 需要给姿态角单独加插值状态 |
| 47 | **`bAllowFirstShotAccuracy` 永久删掉还是留着** | **留着标废弃**（回退路径需要它） | 删 ⇒ 旧武器蓝图数据静默丢失，失去零回归对照物 |

### P14 回正抵扣带来的待拍板（2026-09-20）

> 完整版见 [11_BurstAccumulationFix.md §13.8](11_BurstAccumulationFix.md)。这里只列**需要你决定**的。

| # | 事项 | 我的默认选择 | 备选 / 影响面 |
| --- | --- | --- | --- |
| 48 | **中途中止回正造成的抵扣叠加要不要收敛**（`Interpolated`；`cover=4` 时 `Δ = −5.067` 而非 `−4`） | **先保留现状**（口径最直白：回正目标一律减累计压枪量） | 收敛 ⇒ "中途回正不抵扣、只最终回正抵扣一次"，`Δ` 严格 = `−cover`；`cover=4` 时残留 `0.260 → 1.327`。需新增"本梭已抵扣"标志位 |
| 49 | **压过头（`T_net < 0`）的手感底线** | 不设下限（字面减法，允许镜头最终低于起枪点） | 设下限如 `−0.5 × MaxV` ⇒ 再引入一个夹持常量；实机若"沉得慌"就回到这条 |

> 这两条**只有实机手感能定**，仿真给不出答案。`InstantWrite` 的枪（`Rifle_7`）偏差 ≤ 0.22°，可以不折腾。

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
10. **散布（P13）的调参入口见 [后坐力系统调试.html §11](后坐力系统调试.html#spread)。**
    速记：
    - 资产参数在 `DA_Recoil_*` 的 `Recoil | Spread` 分类下，共 **20 个**（1 开关 + 站定/蹲伏/空中各 4 + 玩家侧 7）。
    - **总开关是 `bEnableProfileSpread`，默认 `false`** —— 不勾就走旧的 Lyra heat 链路（零回归）。
      想看新模型必须先把它勾上；磁盘上的 5 份资产**当前还没勾**。
    - 看数值用 `Lyra.Recoil.SpreadDebug 1`（面板第二行就是 `raw -> effective` 两个锥角）。
    - `Lyra.Recoil.Enable 0` **不会**关掉散布。想单独摘掉散布，就在资产上取消勾选开关，
      或把对应姿态的 `Base` / `AddPerShot` 配成 0。
    - 单位是**全锥角（直径角）**。面板最后一行会把换算好的半角（度与弧度）都打出来，
      跟代码里的 `HalfSpreadAngleInRadians` 对得上就说明链路没被改坏。
    - 改散布数值**不会**影响 Golden（Golden 走 `ComputeShotKick()`，与散布无关），
      但会改 CSV 第 8 列 `SpreadAngle` 的内容 —— 如果做了逐发基线表，记得一起更新。
    - ⚠️ **Live Coding 接不住 P13 的改动**（新增了 `USTRUCT` 字段与 `UPROPERTY` 成员 = 改了反射类布局）。
      本次已经关编辑器整包重编过一次（`Result: Succeeded` + 37/37），后续再动这些结构体仍需同样处理。
