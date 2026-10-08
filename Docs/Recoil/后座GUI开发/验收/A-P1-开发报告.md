# A-P1 开发报告：后坐力 GUI 无 UI 数据转换与纯结构逻辑

- 任务来源：`Docs/Recoil/后座GUI开发/任务/A-P1.txt`（P1 转换与编辑合同，开发计划 §05/§06/§07/§08 与 T01–T08）。
- 开发子 agent：Harness A（DeepSeek Harness headless）。日期：2026-10-08。
- 验证状态：**代码已按验收标准编写完成；本报告不声称编译通过、不声称测试通过。**
  按任务约定不启动整项目构建，编译与 `Lyra.Recoil.Editor.Mapping.*` 运行由主 agent Codex 统一执行并逐项审查。

## 1. 产出文件（独占写入范围内）

| 文件 | 说明 |
| --- | --- |
| `Source/LyraEditor/Private/Recoil/LyraRecoilPatternAdapter.h` | 无 UI 数据模型 + 统一接口 + 辅助类型（新增） |
| `Source/LyraEditor/Private/Recoil/LyraRecoilPatternAdapter.cpp` | 全部转换/结构/剪贴板/高级命令实现（新增） |
| `Source/LyraEditor/Tests/LyraRecoilEditorMappingTest.cpp` | T01–T08 自动化测试，命名 `Lyra.Recoil.Editor.Mapping.*`（新增） |
| `Docs/Recoil/后座GUI开发/验收/A-P1-开发报告.md` | 本报告（新增） |

未创建 `LyraRecoilClipboard.h/.cpp`：剪贴板逻辑属于同一数据契约，放在 Adapter 头/实现里，
避免再拆一个只有 JSON 的文件（任务原文该项为“如需要”）。未修改 `LyraGame`、正式资产、Golden、
开发计划、`LyraEditor.Build.cs` 与其他 agent 文件；未执行 git commit/reset/checkout。

## 2. 实现清单（对应计划条款）

### 2.1 §05 参数映射与双向转换

- `Read()`：原样读取 `PatternPoints` 与 `PatternLength`，**不 sanitize、不 clamp**（打开不改 L）。
- `BuildCumulative()` / `BuildCumulativeFor()`：只构建前 L 个固定点，`FVector2D(Yaw, Pitch)`；
  `ΔYaw = H·X`、`ΔPitch = V·Y·c[i]`、`P[i] = P[i-1] + Δ[i]`、`P[-1] = (0,0)`；
  曲线倍率调用现有 `ULyraRecoilProfile::GetVerticalKickCurveScale(i)`（不自行实现空曲线语义）。
- `MoveCumulative()`：反算 `X = ΔYaw/H`、`Y = ΔPitch/(V·c[i])`，全部中间量为 `double`。
- **未变字段不往返重算**：按“目标增量 vs 原始增量”逐轴判定（容差 1e-9°），未变轴**保留原始 float 位**；
  实现“无操作零补丁”（`Equal()` 为逐字段精确比较，可支撑“无变化不建事务”）。
- 相邻影响按 §05.2 自然成立：移动 P[k] 只改变 Δ[k] 与 Δ[k+1]；多选同时平移各选中点；
  末固定点无“下一点”，随机尾段不参与反算。
- 负曲线：保留带符号分母（`c<0` 时向下 Kick 可表达；向上目标显式拒绝），不取绝对值、不改曲线。
- 零/极小分母（`|H| ≤ 1e-8` 或 `|V·c[i]| ≤ 1e-8`）：目标增量为零 → **保留原始隐藏值**（不写 0）；
  目标增量非零 → 整组拒绝。装载路径仍显示精确值（阈值只作用于反算）。
- 浮点边界：归一化容差 1e-6（仅容差内舍入归并到 `[-1,1]`/`[0,1]`，超容差拒绝、绝不钳制）；
  正向重建容差 `1e-5 + 1e-6·|目标角度|`；`FLyraRecoilConversionReport` 记录最大重建误差、
  最大归并误差与被归并发序号。
- 提交前正向重建：把候选写回 float 后按同一公式重建累计点并与目标比对，超差即整体拒绝。

### 2.2 §06 可表达范围与错误处理

- 失败一律**原子拒绝**：所有输出参数先等于源数据，只有全部校验通过才写入候选；不存在半写入。
- 非有限数：`BuildCumulative`/`MoveCumulative`/`Insert`/结构命令/剪贴板全部拒绝 NaN/Inf；
  `Validate()` 额外覆盖原始字段、曲线键、曲线采样、坐标变换结果，避免 NaN 进入 Slate/资产提交。
- `Validate()` = 现有 `ULyraRecoilProfile::ValidateProfile()` + 编辑器附加有限数检查
  （Base/SingleShot/Recovery/Clamp/Pattern/Multipliers/RollShake/Spread/WeaponVisual 全部数值、
  Vector/Rotator、7 条曲线键、`VerticalKickCurve` 在 `[0, max(N,L)]` 的采样）。
  `ValidateProfile` 的范围比较对 NaN 恒为 false，这一点在测试 `ProfileValidation` 中显式断言。
- `Validate()` 只做 const 读取，不调用 `Modify()`、不发属性通知、不标脏（T08 用 `UPackage::IsDirty()` 断言）。
- 高级命令 `BuildStrengthAdjustmentCandidate()`：
  `H' = max(H, max|ΔYaw|)`、`V' = max(V, max(ΔPitch/c[i]))`，只纳入符号相容且可逆的目标发；
  零曲线且目标垂直增量非零、方向不相容、float 溢出时命令禁用并给出 `BlockingReasons`；
  可行时按新分母重新归一化、列出逐字段差异、标注“H 改变影响随机尾段 / V 改变影响数组外与曲线外推”，
  并显式列出“原分母为零、新分母可逆、为保留零角度被迫写 0”的隐藏值（`HiddenValuesForcedToZero`）。
  不修改 `HorizontalRandomRange`、姿态、上限，不把曲线重置为 1，不触碰源资产。

### 2.3 §07 增删、重排与 PatternLength

- `Insert(Source, Index, NewPoints, bFixedAtBoundary)`：`0 ≤ Index ≤ N`；
  `Index < L → L' = L+m`；`Index == L` 默认尾段（L 不变），显式固定段选项 `→ L' = L+m`；
  `Index > L → L` 不变；`N' = N+m`。既有节点归一化 X/Y 原样保留（曲线在新序号处求值）。
- `Delete(Source, Indices)`：按操作前索引一次性计算 `N' = N−|D|`、`L' = L−|{i∈D 且 i<L}|`；
  重复索引去重；删光后 `N = L = 0`，不补默认点。
- `Reorder(Source, Order)`：`Order` 必须是 `[0,N-1]` 的排列；N、L 数值不变，固定段仍是新顺序前 L 个节点。
- `SetPatternLength(Source, NewLength)`：显式设置 0…N；数组与原始点不变；越界拒绝。
- `MakeStablePitchOrder()`：一次性“按累计 Pitch 排序”的稳定排序，返回可直接交给 `Reorder` 的排列；
  同值保持原发序号顺序；不自动触发、不绑定拖动。

### 2.4 §07 剪贴板 / §08 一致性

- `Copy()`：严格版本 JSON（`format = "LyraRecoilClipboard"`、`version = 1`），
  按原发序号**升序去重**记录 `indices`、原始 `x/y`、可选源逐发角度 `yawDegrees/pitchDegrees/verticalScale`
  与源 `horizontal/vertical`。索引非法、非有限、空选择或超出保护值时返回空串。
- `ParseClipboardEx()` / `ParseClipboard()`：严格校验标识、版本、`points` 存在与非空、
  数量 ≤ 4096、文本 ≤ 1 MiB（UTF-8 字节）、每个点必须是 JSON 对象且 `x/y` 必须是 JSON 数字、
  有限性、可选 `indices` 与点数一致且为非负整数、角度字段要么都有要么都没有。
  未知版本/畸形 JSON/类型不符一律整体拒绝，`OutPoints` 清空（拒绝无副作用）。
- `ConvertClipboardAngles()`：“按角度粘贴”，在目标资产 H/V/曲线下逐发反算，`FirstTargetIndex`
  起按剪贴板顺序；不可逆/超界/方向不相容/非有限一律整体拒绝；成功后做正向重建比对。
  不复制源曲线引用（粘贴只产生归一化候选）。

## 3. 统一接口

必需接口全部按任务原文实现（签名逐字一致）：

```cpp
struct FLyraRecoilPatternData { TArray<FRecoilPatternPoint> Points; int32 PatternLength = 0; };

class FLyraRecoilPatternAdapter
{
public:
    static FLyraRecoilPatternData Read(const ULyraRecoilProfile& Profile);
    static bool Validate(const ULyraRecoilProfile& Profile, TArray<FString>& Errors);
    static bool BuildCumulative(const ULyraRecoilProfile& Profile, TArray<FVector2D>& OutPoints, TArray<FString>& Errors);
    static bool MoveCumulative(const ULyraRecoilProfile& Profile, const TArray<FVector2D>& Targets, FLyraRecoilPatternData& OutData, TArray<FString>& Errors);
    static bool Insert(const FLyraRecoilPatternData& Source, int32 Index, const TArray<FRecoilPatternPoint>& NewPoints, bool bFixedAtBoundary, FLyraRecoilPatternData& OutData, TArray<FString>& Errors);
    static bool Delete(const FLyraRecoilPatternData& Source, const TArray<int32>& Indices, FLyraRecoilPatternData& OutData, TArray<FString>& Errors);
    static bool Reorder(const FLyraRecoilPatternData& Source, const TArray<int32>& Order, FLyraRecoilPatternData& OutData, TArray<FString>& Errors);
    static bool Equal(const FLyraRecoilPatternData& A, const FLyraRecoilPatternData& B);
    static FString Copy(const ULyraRecoilProfile& Profile, const TArray<int32>& Indices);
    static bool ParseClipboard(const FString& Text, TArray<FRecoilPatternPoint>& OutPoints, TArray<FString>& Errors);
};
```

辅助 API / 类型（只增不改）：

| 名称 | 用途 |
| --- | --- |
| `FLyraRecoilConversionReport` | 最大重建误差、最大边界归并误差、被归并发序号 |
| `FLyraRecoilClipboardData` | 剪贴板全量载荷（源序号、原始 X/Y、源角度三元组、源 H/V） |
| `FLyraRecoilStrengthAdjustment` | H'/V' 候选、逐字段差异、被迫置零的隐藏值、禁用原因、目标累计点 |
| `MoveCumulative(..., FLyraRecoilConversionReport&, ...)` | 带诊断的重载；必需签名转发到它 |
| `BuildCumulativeFor(Profile, Data, ...)` | 用显式候选数据（而非资产现值）构建累计点，供结构/候选预览与测试重建校验 |
| `SetPatternLength(...)` | 显式改 L（§07） |
| `ParseClipboardEx(...)` / `ConvertClipboardAngles(...)` | 角度粘贴模式 |
| `MakeStablePitchOrder(...)` | 一次性稳定排序排列 |
| `BuildStrengthAdjustmentCandidate(...)` | 高级“调整基础强度以容纳候选”，只生成候选与差异 |
| `GetAngleTolerance()` / `GetClipboardFormatName()` / `GetClipboardFormatVersion()` | 阈值与版本常量出口 |
| `DenominatorEpsilon / NormalizedTolerance / AngleToleranceAbsolute / AngleToleranceRelative / MaxClipboardPoints / MaxClipboardBytes` | §06/§07 的编辑器保护常量 |

行为约定：所有函数只生成候选；`const ULyraRecoilProfile&` 输入不被写入；失败时输出参数保持源数据。

## 4. 自动化测试清单

`Source/LyraEditor/Tests/LyraRecoilEditorMappingTest.cpp`，`WITH_DEV_AUTOMATION_TESTS`，
`EditorContext | EngineFilter`，全部使用 transient Profile（T08 额外用 `/Temp/` 测试包）：

| 测试名 | 覆盖（T01–T08） | 主要断言 |
| --- | --- | --- |
| `Lyra.Recoil.Editor.Mapping.Conversion` | T01 | §5.2 往返示例 `(0.5,0.75),(-0.5,1.0) → P=(0.1,0.3),(0.0,0.5)`，改点后 `Pattern'=(0.75,0.875),(-0.75,0.75)`；逐发与 `FRecoilRuntimeState::ComputeShotKick`（倍率 1）比对；无操作 bitwise 零补丁；全零/负水平/可复现随机合法数组 |
| `Lyra.Recoil.Editor.Mapping.Denominators` | T02 | `H=0`、`V=0`、`c=0` 隐藏值 bitwise 保留；不可逆非零目标拒绝且原数据不变；负曲线向下可表达、向上拒绝；`H=1e-12` 锁定与精确装载；`H=1e-7` 超界拒绝（不 clamp）；NaN/Inf 全链路拒绝且不静默清零；负强度拒绝并报错 |
| `Lyra.Recoil.Editor.Mapping.Adjacent` | T03 | 首/中/末固定点与相邻发规则（Δ[k]+=d、Δ[k+1]-=d）；非连续多选；末固定点与尾段隔离；下一发超限整组回退（错误含 `Shot 1`）；容差内边界归并（记录误差）与超容差拒绝 |
| `Lyra.Recoil.Editor.Mapping.Tail` | T04 | `N=0` 不自动补点 + 插入边界默认尾段/显式固定段；`L=0`；`L<N` 尾段 X/Y 原样保留；`L=N`；`GetPatternPoint(N-1/N/N+1)`；N 之后曲线继续按发序号求值 |
| `Lyra.Recoil.Editor.Mapping.Structural` | T05 | 插入 `k<L / k=L / k>L / k=N / 越界`；跨边界删除、删光、非法索引；显式重排、非置换拒绝、恒等零补丁；`SetPatternLength` 激活尾段 X（`P[2].Yaw=0.38`）；稳定排序 `[1,0,2,3]` 与“归一化保留 + 曲线按新索引重采样” |
| `Lyra.Recoil.Editor.Mapping.OutOfRangeSolutions` | T06 | 默认拒绝不钳制；`H'/V'` 候选（`V=0.4→0.6`）与重新归一化、候选强度正向重建命中；H/V 差异标注随机尾段且 `HorizontalRandomRange` 不被改；零曲线禁用并给原因；隐藏值被迫置零显式列出 |
| `Lyra.Recoil.Editor.Mapping.Clipboard` | T07 | 非连续选择按序号升序复制/解析；源序号与源强度；角度粘贴在不同强度下反算 `(0.2,0.5)`；未知版本、未知格式、畸形 JSON、字符串坐标、空数组整体拒绝且无副作用；1 MiB 与 4096 点保护；`Copy` 非法输入返回空 |
| `Lyra.Recoil.Editor.Mapping.Preservation` | T08 | 含尾部 X、零分母非零值、复杂曲线与 Roll/Spread/WeaponVisual 的资产：只读/候选/结构/高级/剪贴板调用后源资产逐字段不变、尾部 X/Y bitwise 保留、隐藏 Y bitwise 保留、`UPackage::IsDirty()` 保持 false |
| `Lyra.Recoil.Editor.Mapping.ProfileValidation` | T02/T08 附加 | 合法复杂 Profile 通过 `Validate`；`ValidateProfile` 单独漏掉 NaN（显式断言该缺口）；NaN 点与非有限曲线键被 `Validate` 捕获 |

## 5. 未运行项 / 验证状态（不虚报）

- **未编译**：本 agent 按任务约定未启动 `LyraEditor Win64 Development` 构建，也未做任何局部编译。
  因此不能保证一次通过 UHT/编译；接口与字段名已逐条对照 `LyraRecoilProfile.h`、`LyraRecoilTypes.h`、
  `LyraRecoilState.h` 与本机 UE 5.8 头文件（`JsonObject.h`/`JsonValue.h`/`RichCurve.h`/`AutomationTest.h`）核对。
- **未运行**：9 个 `Lyra.Recoil.Editor.Mapping.*` 用例均未执行；`failed/notRun/inProcess` 无本轮报告。
- **本会话 `pwsh` 完全不可用**：DSH 沙箱在给 `D:\TPSGunsDemo\TPSGunsDemo` 授予写权限时被拒绝
  （`SetNamedSecurityInfoW failed (Win32 5): grantWrite(D:\TPSGunsDemo\TPSGunsDemo)`），任何命令都未执行。
  已按 `diagnose-windows-sandbox-acl` 技能发起一次带审批的修复调用，但本机无审批通道（fails closed），
  故该路径**未诊断、未修复**。因此本报告连“本地语法/括号自检”这类轻量校验也未运行；
  文件写入走的是 DSH 文件工具（正常），源码内容完整。
- **未做**：Slate/画布/事务/对象生命周期（P2 及以后）；`LyraEditor.Build.cs` 未改（Json 已是私有依赖，
  UBT 自动收集 `Private/Recoil/*.cpp` 与 `Tests/*.cpp`，无需登记）。
- **未做**：真实资产、Golden、`LyraGame` 运行时、PIE 手感验收。

## 6. 限制与已知边界

1. **只到候选层**：Adapter 不执行事务、不发属性通知、不写 UObject；把候选提交成
   `FScopedTransaction` + 最小字段差异是 P2 的职责（计划 §08.1、§11）。
2. **稳定 ID 不在本层**：T07 的“新节点新 ID”属于 P2 Session（`FGuid` 映射），Adapter 只产生候选点。
3. **“有损投影到合法范围”未实现**：计划 §06 允许的显式投影命令（展示逐发与累计误差后应用）本轮
   只做了“拒绝 + 具体范围诊断”，投影命令留待 P3 与诊断面板一起做（本报告明确列为未完成项）。
4. **高级命令的尾段影响只做文字标注**：随机尾段与 N 之外曲线外推的逐发前后比较依赖 P4 预览，
   Adapter 只输出差异说明，不承诺“全运行时无损”。
5. **稳定排序键覆盖全数组**：累计 Pitch 对所有 N 个点用同一 ΔPitch 公式计算（尾段没有
   §5.1 定义的累计语义）；排序结果必须交给 `Reorder` 并按新索引重采样曲线，这一点已由测试覆盖。
6. **`Validate()` 的有限数检查是“附加”而非常规资产校验**：它会检查关闭开关下的字段与全部曲线键，
   比 `ValidateProfile` 更严格；若主 agent 认为某些关闭字段不应阻断保存，可在 P2 收敛为警告级。
7. **剪贴板保护值**（1 MiB / 4096 点）是编辑器限制，不是运行时资产上限；`Copy` 同样施加该限制。
8. **角度粘贴的 API 形状**：`ConvertClipboardAngles` 需要显式 `FirstTargetIndex`，调用方
   （P3 命令层）需在插入位置确定后再调用；未提供“解析即按角度转换”的隐式路径。

## 7. 关键设计决策（供审查）

- **“未变”判定用增量而不是累计点**：`|目标Δ − 原始Δ| ≤ 1e-9°` 才保留原值；这样拖动整段同向平移时
  内部增量不变、只有边界发改变，与 §05.2 的多选语义一致，并且 `MoveCumulative(P, BuildCumulative(P))`
  是严格的 bitwise 零补丁。
- **隐藏值保留以“轴”为单位**：只有该轴增量未变（或分母不可逆且目标增量为零）才保留原 float；
  另一轴变化不影响本轴原值。
- **方向不相容单独报错**（负曲线 + 向上目标），比笼统的“超界”更可诊断，测试用例锁定该行为。
- **重建校验使用候选 float 值**，因此 float 降精度导致的偏差也会被容差捕获，符合“提交时转 float 再检查”。
- **`FLyraRecoilPatternData` / 报告结构是纯 C++ 结构**（非 `USTRUCT`），不进入反射与序列化，
  不会被误存进运行时资产。

## 8. 待主 agent 执行的验证

```powershell
# 1) 构建（不在本 agent 执行范围内）
powershell -ExecutionPolicy Bypass -File .\Docs\Recoil\Tools\build.ps1 -EngineRoot 'D:\UE_5.8' -Project 'D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject' -Target LyraEditor -Platform Win64 -Config Development -UBARootDir 'D:\TPSGunsDemo\TPSGunsDemo\Saved\UBACache'

# 2) 仅跑本层用例
& 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject' '-ExecCmds=Automation RunTests Lyra.Recoil.Editor.Mapping' '-TestExit=Automation Test Queue Empty' "-ReportExportPath=<本轮新目录>" '-unattended -nopause -nullrhi -nosplash'
```

预期用例名（9 个，全部 `Lyra.Recoil.Editor.Mapping.*`）：
`Conversion`、`Denominators`、`Adjacent`、`Tail`、`Structural`、`OutOfRangeSolutions`、`Clipboard`、
`Preservation`、`ProfileValidation`。请以本轮 `index.json` 的 `failed/notRun/inProcess` 与完整用例名核对，
不要以退出码或本报告代替本轮证据。
