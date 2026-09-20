# TPS 相机（腰射 / ADS）调试手册

| 项 | 值 |
| --- | --- |
| 项目 | `D:\TPSGunsDemo\TPSGunsDemo` |
| 引擎 | UE 5.8.2（源码版，`D:\UE_5.8`） |
| 相机结构 | Lyra 原生 `ULyraCameraModeStack`，**未派生自定义相机类** |
| 覆盖范围 | 腰射 / 常态相机 + 瞄准（ADS）相机的参数定位与调试方法 |
| 建立日期 | 2026-09-20 |
| 状态 | 现状已核实到资产级；本文为调参操作手册 |

---

## 1. 结论速查

| | 腰射 / 常态 | 瞄准 / ADS |
| --- | --- | --- |
| 相机模式类（父类均为 `ULyraCameraMode_ThirdPerson`） | `/Game/Characters/Cameras/CM_ThirdPerson` | `/ShooterCore/Camera/CM_ThirdPersonADS` |
| 偏移曲线 | `/Game/Characters/Cameras/ThirdPersonOffsetCurve` | `/ShooterCore/Camera/ThirdPersonADSOffsetCurve` |
| 谁把它挂上去 | PawnData 的 `DefaultCameraMode`（`HeroData_ShooterGame`、`SimplePawnData`） | `/ShooterCore/Input/Abilities/GA_ADS` |
| 触发方式 | 出生即生效，常驻栈底 | 按住输入 `InputTag.Weapon.ADS` → 压栈；松手 → 出栈 |
| CameraTypeTag | 未覆盖（空） | `Lyra.Weapon.SteadyAimingCamera` |

另有死亡相机 `/Game/Characters/Cameras/CM_ThirdPerson_Death`（曲线 `ThirdPersonDeathOffsetCurve`），
由 `/Game/Characters/Heroes/Abilities/GA_Hero_Death` 挂载——**调腰射/瞄准时不用管它，但它会影响死亡瞬间的观感**。

---

## 2. 完整链路

```
腰射：PawnData.DefaultCameraMode ─┐
                                  ├─→ ULyraHeroComponent::DetermineCameraMode()
瞄准：GA_ADS → SetCameraMode ─────┘        ↓
                                   ULyraCameraComponent::UpdateCameraModes()
                                            ↓
                                   ULyraCameraModeStack::PushCameraMode(模式类)
                                            ↓
                         按 BlendWeight 混合 → FLyraCameraModeView
                                            ↓
                     DesiredView (Location / Rotation / FOV)
                                            ↓
                     ALyraPlayerCameraManager::UpdateViewTarget()
                                            ↓
                     CameraModifier（WeaponRecoil / RollShake 在此叠加）
                                            ↓
                                          屏幕
```

对应代码位置：

| 环节 | 文件 | 行 |
| --- | --- | --- |
| 模式选择（能力优先于 PawnData） | `Source/LyraGame/Character/LyraHeroComponent.cpp` | L471-493 |
| 能力设置 / 清除相机模式 | 同上 | L495-511 |
| 蓝图可调度的 `SetCameraMode` / `ClearCameraMode` | `Source/LyraGame/AbilitySystem/Abilities/LyraGameplayAbility.cpp` | L520-544 |
| 取模式并压栈 | `Source/LyraGame/Camera/LyraCameraComponent.cpp` | L85-99 |
| 产出最终 View | 同上 | L32-83 |
| 栈内混合 | `Source/LyraGame/Camera/LyraCameraMode.cpp` | L330-341 / L407-428 |
| 模式实例缓存（决定"改了要不要重启"） | 同上 | L343-363 |
| 第三人称 offset 应用 | `Source/LyraGame/Camera/LyraCameraMode_ThirdPerson.cpp` | L35-72 |
| 调试打印 | `Source/LyraGame/Camera/LyraPlayerCameraManager.cpp` | L47-65 |

---

## 3. 参数清单

### 3.1 相机模式标量属性（在 CM_ThirdPerson / CM_ThirdPersonADS 的 Class Defaults 里）

| 属性 | 含义 | 位置 |
| --- | --- | --- |
| `FieldOfView` | 该模式的视场角（度） | `LyraCameraMode.h` L121 |
| `ViewPitchMin` / `ViewPitchMax` | 俯仰限位 | `LyraCameraMode.h` L125-130 |
| `BlendTime` | 进入该模式的混合时长 | `LyraCameraMode.h` L133 |
| `BlendFunction` | Linear / EaseIn / EaseOut / EaseInOut | `LyraCameraMode.h` L137 |
| `BlendExponent` | 缓动曲线指数 | `LyraCameraMode.h` L141 |
| `CameraTypeTag` | 供 gameplay 查询"当前是不是在瞄准" | `LyraCameraMode.h` L114 |
| `TargetOffsetCurve` | 偏移曲线资产引用 | `LyraCameraMode_ThirdPerson.h` L40 |
| `bUseRuntimeFloatCurves` + `TargetOffsetX/Y/Z` | 内联曲线（**调参期间不要用**，见 §4） | `LyraCameraMode_ThirdPerson.h` L45-55 |
| `CrouchOffsetBlendMultiplier` | 蹲下时相机高度过渡速度 | `LyraCameraMode_ThirdPerson.h` L58 |
| `PenetrationBlendInTime` / `OutTime` | 相机被墙挤压时的推进/恢复速度 | `LyraCameraMode_ThirdPerson.h` L63-67 |
| `bPreventPenetration` | 是否做相机防穿墙 | 同上 L70 |
| `bDoPredictiveAvoidance` | 是否做预测性避让 | 同上 L73 |
| `CollisionPushOutDistance` | 穿墙时的推出距离 | 同上 L77 |
| `PenetrationAvoidanceFeelers` | 避让射线组（构造里默认 7 条） | `LyraCameraMode_ThirdPerson.cpp` L26-32 |

要点：**CM_ThirdPerson 未覆盖 `FieldOfView`、`BlendTime`、`CameraTypeTag`，走的是 C++ 默认值（80 / 0.5 / 空）**；
CM_ThirdPersonADS 覆盖了 `FieldOfView`、`BlendTime`、`CameraTypeTag`。想知道 ADS 具体是多少，直接打开该资产看 Class Defaults。

### 3.2 偏移曲线（CurveVector）

- 横轴是 **Pitch（角色俯仰，约 -89 ~ 89）**，不是时间。
- X / Y / Z = 在角色局部空间的前后 / 左右 / 上下偏移（cm），代码里用 `PivotRotation.RotateVector(TargetOffset)` 应用。
- 曲线为零/不存在时，相机就落在 Pivot（眼睛高度）上，退化为越肩位置都没有的第一人称式视角。
- 这就是"腰射手感（越肩位置、抬头/低头时相机怎么走）"和"ADS 收拢程度"的**唯一**决定因素。

### 3.3 C++ 默认值（改这些要重编译）

| 项 | 值 | 位置 |
| --- | --- | --- |
| 默认 FOV | `LYRA_CAMERA_DEFAULT_FOV = 80.0f` | `LyraPlayerCameraManager.h` L14 |
| 默认俯仰限位 | `-89.0f / +89.0f` | `LyraPlayerCameraManager.h` L15-16 |
| 默认混合 | `BlendTime 0.5` / `EaseOut` / `BlendExponent 4` | `LyraCameraMode.cpp` L52-63 |

---

## 4. 生效规则（决定你该怎么改）

| 改什么 | 生效条件 | 依据 |
| --- | --- | --- |
| 偏移曲线资产 | **PIE 中实时生效**，边跑边拖点 | `UpdateView` 每帧从资产求值（`LyraCameraMode_ThirdPerson.cpp` L55） |
| 相机模式标量属性 | 改蓝图 defaults 后**必须重启 PIE** | 模式实例由 `CameraModeInstances` 缓存，创建时从 CDO 拷贝一次（`LyraCameraMode.cpp` L343-363） |
| `bUseRuntimeFloatCurves` | 内联曲线在 PIE 中**无法实时编辑** | 源码注释 UE-103986（`LyraCameraMode_ThirdPerson.h` L43） |
| C++ 构造函数默认值 | 重编译 + 重启编辑器 | `LyraCameraMode.cpp` L52-63 |

> 所以调参期一律走**曲线资产**，不要动 `bUseRuntimeFloatCurves`。

---

## 5. 调试手段

### 5.1 看运行时状态：`showdebug camera`

PIE 控制台执行 `showdebug camera`（再执行一次关闭）。它走 `ALyraPlayerCameraManager::DisplayDebug`，
输出 `ULyraCameraComponent::DrawDebug` + `ULyraCameraModeStack::DrawDebug` 的内容：

- 相机 Location / Rotation / FOV
- 模式栈每层**名字 + BlendWeight**
- `LyraCameraMode_ThirdPerson::DrawDebug` 额外输出本帧被相机防穿墙射线命中的 Actor 列表

**用途**：确认"按下 ADS 后 `CM_ThirdPersonADS` 是否真的压上了栈""权重有没有到 1""FOV 当前实际是多少"。
调参时这个窗口常开。

### 5.2 调偏移曲线（主战场）

1. 起 PIE，站定，调好一个固定 Pitch（例如 0°，再试 ±30°）。
2. 打开 `ThirdPersonOffsetCurve` 或 `ThirdPersonADSOffsetCurve`，直接拖关键点。
3. 画面即时变化，满意后保存资产。

### 5.3 隔离 ADS 调参：PawnData 替换法

不想每次都按住 ADS 键触发（还会带入移动速度、动画、UI 等变量）时：

1. 打开 `HeroData_ShooterGame`（或 `SimplePawnData`）。
2. 把 `DefaultCameraMode` 临时换成 `CM_ThirdPersonADS`。
3. 重启 PIE，就是纯 ADS 视角，可以安安静静调 `ThirdPersonADSOffsetCurve` 和 ADS 的 FOV。
4. 调完换回 `CM_ThirdPerson`。

### 5.4 先排除干扰层（必做）

`UCameraModifier_WeaponRecoil` 和 `LyraCameraRollShake` 是在相机模式**之后**叠加到 POV 上的，
由 `ULyraRangedWeaponInstance` 驱动（`LyraRangedWeaponInstance.cpp` L10），跟相机模式无关。
开着它们调曲线，你分不清画面变化是自己拖出来的还是后坐力推的。

调相机前先执行：

```
Lyra.Recoil.Enable 0
Lyra.Recoil.RollShake 0
```

### 5.5 旁观相机实际位置：`ToggleDebugCamera`

PIE 里切 UE 原生自由相机，可以从侧面看角色相机到底停在哪。注意切成自由相机后就不再走角色相机模式，
它只用于"确认相机位置"，不用于调参。

---

## 6. 症状 → 归因对照

| 症状 | 大概率原因 |
| --- | --- |
| 改了资产没反应 | 改的是标量属性且没重启 PIE；或改的是 `bUseRuntimeFloatCurves` 的内联曲线 |
| 按住 ADS 画面完全不变 | `CM_ThirdPersonADS` 没压上栈 —— 用 `showdebug camera` 确认；再查 `GA_ADS` 是否被触发（`InputTag.Weapon.ADS`） |
| ADS 过渡"黏" | `CM_ThirdPersonADS.BlendTime` 偏大；或两层 FOV 差太大 |
| ADS 收镜后画面还在飘 | 是后坐力层，先按 §5.4 关掉再判断 |
| 相机贴墙时抖动/穿墙 | `Penetration*` 参数与 `PenetrationAvoidanceFeelers` |
| 蹲下时相机高度跳变 | `CrouchOffsetBlendMultiplier` |
| 抬头/低头时相机贴脸或穿角色 | 偏移曲线在高 |Pitch| 段没给够 Z / X |

---

## 7. 当前缺口与建议补齐

现状：**只有曲线能热调**。FOV、混合时长这类"到底是 65 还是 72"的问题，每轮都要改资产 + 重启 PIE。

建议照项目既有的 `LyraRecoilDebug`（`Lyra.Recoil.*`）范式，补一套 `Lyra.Camera.*`：

| CVar | 作用 |
| --- | --- |
| `Lyra.Camera.Debug` | 屏幕面板：当前模式栈、各层权重、FOV、Pivot、Offset、被挡百分比 |
| `Lyra.Camera.FOV` | 覆盖当前 FOV（走现成的 `AddFieldOfViewOffset`） |
| `Lyra.Camera.OffsetScale` | 整体缩放曲线输出，用于快速判断"偏移量级对不对" |
| `Lyra.Camera.ADSBlendTime` | 覆盖 ADS 混合时长 |
| `Lyra.Camera.ForceADS` | 强制切 ADS 模式，免按键 |

实现约束（沿用项目已定的规矩）：

- 用**文件内局部** `FAutoConsoleVariableRef` / `FAutoConsoleVariable` 注册，**不要挂在 UCLASS 上**，避免 UHT 重跑。
- 放到 `Source/LyraGame/Camera/` 下新增 .cpp，编译走既有的 `pipe2.py` 流程（只重编 TU + 重链，10s 级；必须 `-NoUBA`）。
- 自动化测试可以直接 `ExecCmds` 驱动 CVar（参考 `LyraRecoilConsoleTest.spec.cpp` 的写法）。

---

## 8. 一次典型调参流程

1. [ ] 起 PIE，控制台：`showdebug camera`、`Lyra.Recoil.Enable 0`、`Lyra.Recoil.RollShake 0`
2. [ ] 持枪站定，录一条基准（腰射，Pitch 0 / +30 / -30 各停一下）
3. [ ] 拖 `ThirdPersonOffsetCurve`，实时看画面；确认 X/Y/Z 三路方向没搞反
4. [ ] 按住 ADS，`showdebug camera` 确认栈里出现 `CM_ThirdPersonADS`、权重最终到 1
5. [ ] 拖 `ThirdPersonADSOffsetCurve`，把瞄准视角收到目标位置
6. [ ] 打开 `CM_ThirdPersonADS`，调 `FieldOfView`（ADS 视场）与 `BlendTime`（收镜速度）→ **重启 PIE** 验证
7. [ ] 若腰射基准 FOV 也要改，同样改 `CM_ThirdPerson` 的 defaults → 重启 PIE
8. [ ] 恢复 `Lyra.Recoil.Enable 1`、`Lyra.Recoil.RollShake 1`，整体复看：射击时的相机叠加是否仍然可接受
9. [ ] 保存全部涉及的资产
