# P0 勘察报告 — TPS 后坐力系统接入点冻结

| 项 | 值 |
| --- | --- |
| 阶段 | P0 勘察与接口冻结 |
| 基准文档 | `Docs/RecoilDevelopmentPlan.md` |
| 工程 | `D:\TPSGunsDemo\TPSGunsDemo`（UE 5.8.2 源码版 `D:\UE_5.8`） |
| 勘察日期 | 2026-09-17 |
| 结论 | 接入点全部定位到 `文件:行号`，**无 TBD 项** |

> 行号对应勘察当日的工作区快照。若后续阶段改动了这些文件，以本报告记录的行号作为"改动前基线"。

---

## 1. 发射方向计算链路（P3 注入点）

**文件**：`Source/LyraGame/Weapons/LyraGameplayAbility_RangedWeapon.cpp`

| 位置 | 内容 |
| --- | --- |
| L45–L70 | `VRandConeNormalDistribution()` —— 现有唯一的方向扰动函数，唯一随机源是 `FMath::FRand()`（L53、L55） |
| L352–L380 | `PerformLocalTargeting()` —— 本地瞄准方向入口 |
| **L365** | `InputData.AimDir = TargetTransform.GetUnitAxis(EAxis::X);` —— **基准瞄准方向** |
| L366 | `InputData.StartTrace = TargetTransform.GetTranslation();` |
| L368 | `InputData.EndAim = StartTrace + AimDir * MaxDamageRange` |
| L378 | `TraceBulletsInCartridge(InputData, OutHits)` —— 进入逐发循环 |
| L382–L438 | `TraceBulletsInCartridge()` —— 逐发构造弹道 |
| **L391–L393** | `BaseSpreadAngle` / `SpreadAngleMultiplier` / `ActualSpreadAngle` 计算 |
| **L397** | `const FVector BulletDir = VRandConeNormalDistribution(InputData.AimDir, HalfSpreadAngleInRadians, WeaponData->GetSpreadExponent());` —— **弹道 Pattern 注入点** |
| L399 | `EndTrace = StartTrace + BulletDir * MaxDamageRange` |
| L552–L595 | `StartRangedWeaponTargeting()`，L567 调 `PerformLocalTargeting()` |
| **L534–L536** | `OnTargetDataReadyCallback()` 中 `WeaponData->AddSpread();` —— **真正消耗弹药成功之后的唯一递增点** |

### 冻结的注入方案（P3 执行）

1. **偏移叠加位置：L397 之前，作用在 `InputData.AimDir` 上。**
   顺序为 `AimDir → [叠加后坐力偏移] → [走原有 spread 扩散]`。符合计划 §P3「叠加在扩散之前，两者独立可调」。
2. **递增触发点：L536 `AddSpread()` 旁边**，即开火提交成功之后。保证"没扣成弹不涨后坐力"。
3. **不改 `VRandConeNormalDistribution()` 本身**——它是原始扩散逻辑，`Lyra.Recoil.Enable 0` 时必须保持原样可用。

### 已确认的冲突风险
- `CartridgeID = FMath::Rand()`（L575）—— 这是命中标记用的随机数，**不是弹道随机**，与后坐力随机源互相独立，无需处理。
- `GetTargetingTransform()` L209–L290 返回的 `CamRot` 来自 `PC->GetPlayerViewPoint()`（L239）。因为相机偏移走 CameraModifier（只改显示层），`GetPlayerViewPoint` 拿到的仍是**真实**瞄准方向 —— 后坐力偏移必须显式叠加，不会被相机自动带偏。**这正是决策 1 的必要性所在。**

---

## 2. 武器运行时状态宿主（P2 主战场）

**文件**：`Source/LyraGame/Weapons/LyraRangedWeaponInstance.h` / `.cpp`

| 位置 | 内容 | 用途 |
| --- | --- | --- |
| `.h` L196–L215 | 现有运行时状态字段：`CurrentHeat`(L197)、`CurrentSpreadAngle`(L200)、`bHasFirstShotAccuracy`(L203)、`CurrentSpreadAngleMultiplier`(L206)、`StandingStillMultiplier`(L209)、`JumpFallMultiplier`(L212)、`CrouchingMultiplier`(L215) | 后坐力状态字段照此风格加入同一 `private` 区 |
| `.h` L218 / `.cpp` L73–L86 | `Tick(float DeltaSeconds)` | **后坐力每帧推进挂这里** |
| `.h` L225 / `.cpp` L111–L123 | `AddSpread()` | 现有的"每发"钩子，后坐力递增与之并列 |
| `.cpp` L48–L66 | `OnEquipped()` | **状态重置点**：现有代码在此重置 heat 与各 multiplier，后坐力重置加在此处（L65 之后） |
| `.cpp` L68–L71 | `OnUnequipped()` | 目前**只调 Super，不清理任何状态** → 必须补 `ResetRecoilState()`，否则换枪残留偏移 |
| `.cpp` L166–L217 | `UpdateMultipliers()` | **P4 姿态倍率的现成判定源**，见下节 |
| `.h` L130–L167 | 玩家参数 UPROPERTY 组：`SpreadAngleMultiplier_Aiming`(L132)、`_StandingStill`(L137)、`_Crouching`(L154)、`_JumpingOrFalling`(L163) | 后坐力姿态参数的字段命名与分类照此风格 |
| `.h` L100–L190 | `Spread|Fire Params` / `Weapon Config` 资产字段 | P1 的 `ULyraRecoilProfile` 分类命名参考 |

### P4 直接复用的既有判定（不重复造轮子）

| 姿态 | 现成判定位置 | 表达式 |
| --- | --- | --- |
| 站立/移动 | `.cpp` L175–L181 | `Pawn->GetVelocity().Size()` 对 `StandingStillSpeedThreshold` 做映射 + `FInterpTo` |
| 蹲伏 | `.cpp` **L184** | `CharMovementComp->IsCrouching()` |
| 空中 | `.cpp` **L190** | `CharMovementComp->IsFalling()` |
| 瞄准 | `.cpp` **L196–L208** | `ULyraCameraComponent::GetBlendInfo()` + `TAG_Lyra_Weapon_SteadyAimingCamera`（tag 定义于 `.cpp` L13） |

> P4 的 `EPoseState` 推导直接从这三个 `bIsCrouching` / `bIsJumpingOrFalling` / 速度判定取值。为避免与 spread 的平滑插值耦合，后坐力侧另存一套自身插值状态，但**判定源共用**。

---

## 3. Tick 驱动源（P2 关键前提 — 本阶段最重要的发现）

**`ULyraRangedWeaponInstance::Tick()` 不是引擎自动调用的。**
`ULyraEquipmentInstance` 继承自 `UObject`（`Source/LyraGame/Equipment/LyraEquipmentInstance.h` L20），没有 `TickComponent`，也不在 `UObject` 的 tick 路径上。

实际驱动链在 **`Source/LyraGame/Weapons/LyraWeaponStateComponent.cpp`**：

| 行号 | 内容 |
| --- | --- |
| L19–L26 | 构造函数中开启 `PrimaryComponentTick.bCanEverTick = true`（L25） |
| L28–L42 | `TickComponent()` |
| L32 | `APawn* Pawn = GetPawn<APawn>()` |
| L34 | `Pawn->FindComponentByClass<ULyraEquipmentManagerComponent>()` |
| L36 | `EquipmentManager->GetFirstInstanceOfType(ULyraRangedWeaponInstance::StaticClass())` |
| **L38** | **`CurrentWeapon->Tick(DeltaTime);`** ← 唯一的驱动调用 |

**结论**：
- `ULyraRangedWeaponInstance::Tick()` 每帧被调用一次，**后坐力的每帧推进直接写在里面即可，无需新增 Tick 源、无需改 `LyraWeaponStateComponent`**。
- `ULyraWeaponStateComponent` 挂在 PlayerController 上（`UControllerComponent`，`.h` L41），因此 **AI 控制的武器也在 tick**（`GetPawn<APawn>()` 对 bot 同样成立）。
- 副作用提醒：`Tick()` 里现有 `check(Pawn != nullptr)`（`.cpp` L76）。后坐力推进代码必须放在这个 check 之后，避免额外空指针风险。

---

## 4. 相机链与 CameraModifier 挂载点（P2 交付）

### 4.1 现状

| 文件 | 位置 | 内容 |
| --- | --- | --- |
| `Camera/LyraPlayerCameraManager.h` | L25–L46 | `ALyraPlayerCameraManager : public APlayerCameraManager`（`MinimalAPI`） |
| `Camera/LyraPlayerCameraManager.cpp` | L34–L45 | `UpdateViewTarget()` 覆写 |
| `.cpp` | L37–L42 | UI Camera 优先分支：`Super::UpdateViewTarget` + `UICamera->UpdateViewTarget` |
| `.cpp` | L44 | 常规分支：`Super::UpdateViewTarget(OutVT, DeltaTime)` |
| `Camera/LyraCameraComponent.h` | L28–L70 | `ULyraCameraComponent : UCameraComponent`，L57 `GetCameraView()` 是 POV 来源 |
| `Camera/LyraCameraMode_ThirdPerson.*` | 全部 | PIE 默认第三人称相机模式 |
| `Camera/LyraUICameraManagerComponent.*` | 全部 | ⚠️ 是 `UCameraManagerComponent`，**不是 CameraModifier**，机制不同，仅作代码风格参考 |

### 4.2 挂载点结论

引擎 `APlayerCameraManager::UpdateViewTarget()` 在算出 ViewTarget 之后，会遍历 `CameraModifiers` 数组依次调用 `ModifyCamera(ThisCameraModifier, DeltaTime, OutVT.POV)`。

因此：

- **挂载目标 = `ALyraPlayerCameraManager`**，用继承自 `APlayerCameraManager` 的 `AddCameraModifier(UCameraModifier*)` 挂载，无需覆写 `UpdateViewTarget`。
- **获取方式**：`APlayerController::PlayerCameraManager` → `Cast<ALyraPlayerCameraManager>`。注意 `ALyraPlayerCameraManager` 是 `MinimalAPI`，跨模块需要 `LYRAGAME_API` 导出标注（新增的 CameraModifier 类本身在 LyraGame 内，同模块访问无此问题）。
- **全工程 grep 结果：`AddCameraModifier` / `CameraModifier` / `RemoveCameraModifier` 零命中** —— 这是第一条 CameraModifier 链路，不与任何现有代码冲突，但也没有现成范式可抄，需自行保证生命周期正确（必须在 Destructor / EndPlay 里 `RemoveCameraModifier`）。

### 4.3 为什么必须走 CameraModifier（复述决策 1，作为代码注释依据）

`PC->GetPlayerViewPoint()`（`LyraGameplayAbility_RangedWeapon.cpp` L239）返回的是 `ControlRotation` 派生的真实视角。若用 `AddPitchInput` 抬视角：
1. 恢复期无法区分"玩家自己拉的"与"后坐力抬的"，回正会连玩家输入一起拽走；
2. 它会被网络复制（若将来要支持联机，会变成双份抬枪 —— 本项目 2026-09-17 已决定不做联机，此条仅作为"为什么不用 AddPitchInput"的技术论据保留）；
3. 弹道方向跟着变，与"弹道独立于相机"的设计目标冲突。

CameraModifier 只改 `FMinimalViewInfo`（显示层），`ControlRotation` 恒定不变 —— 上面三个问题同时消失。

---

## 5. 武器生命周期（P2 状态重置）

| 位置 | 内容 |
| --- | --- |
| `Equipment/LyraEquipmentInstance.h` L19–L72 | `ULyraEquipmentInstance : UObject`，`IsSupportedForNetworking()=true`(L28)，`GetWorld()` override **final**(L29) |
| `.h` L49–L50 | `virtual void OnEquipped(); virtual void OnUnequipped();` |
| `Weapons/LyraWeaponInstance.cpp` L36–L45 | `ULyraWeaponInstance::OnEquipped()` —— 记录 `TimeLastEquipped`（L42）+ `ApplyDeviceProperties()` |
| `Weapons/LyraWeaponInstance.cpp` L47–L52 | `ULyraWeaponInstance::OnUnequipped()` —— 仅 `RemoveDeviceProperties()` |
| `Weapons/LyraRangedWeaponInstance.cpp` L48–L66 | **`ULyraRangedWeaponInstance::OnEquipped()`** ← 状态重置写这里（L65 之后） |
| `Weapons/LyraRangedWeaponInstance.cpp` L68–L71 | `OnUnequipped()` 目前空实现 → **必须补清理** |
| `Equipment/LyraEquipmentManagerComponent.h` | `GetFirstInstanceOfType()` —— 运行时取当前武器的唯一入口 |
| `Weapons/LyraWeaponInstance.h` L88–L89 | `TimeLastEquipped` / `TimeLastFired`（double），后坐力侧时间戳命名照此风格 |

**生命周期结论**：
- 装备/卸下是唯一的状态重置时机。`FRecoilRuntimeState` 的 `Reset()` 在 `OnEquipped()` 与 `OnUnequipped()` 两处都要调。
- `ULyraEquipmentInstance` 是 `UObject`（本身可被网络复制，L28）。**本项目 2026-09-17 已决定不做联机**，所以这里不再考虑复制边界；但把状态集中成一个"不持有任何 UObject 指针的纯数值结构体"（`FRecoilRuntimeState`）这个设计依然保留 —— 它的价值在于让 P2/P3/P4 的核心算法能被**纯数值单测**完整覆盖（开发计划 §硬性规则 4）。

---

## 6. 调试命令入口（P5）

| 方案 | 位置 | 评价 |
| --- | --- | --- |
| `UFUNCTION(Exec)` in `ULyraCheatManager` | `Player/LyraCheatManager.h` L38–L94（范式），`CheatOutputText()` L35 | 需要 PlayerController + cheat 上下文；`USING_CHEAT_MANAGER`（`.h` L11–L13）在 Shipping 下禁用 |
| **`FAutoConsoleVariableRef`**（推荐） | 范式现成：`Weapons/LyraGameplayAbility_RangedWeapon.cpp` **L16–L38** `namespace LyraConsoleVariables` | 无上下文依赖、任意 build 都能用、可在 `UnrealEditor-Cmd` 里靠 `-ExecCmds` 驱动 → **P5 自动化测试可直接调 CVar** |

**P5 方案**：CVar 全部用 `FAutoConsoleVariableRef`，注册在 `ULyraRecoilDebug` 的静态初始化中。
命名前缀严格用 `Lyra.Recoil.`（计划 §P5 表格），与现有 `lyra.Weapon.DrawBulletTraceDuration`（L20）大小写风格并存但互不冲突。
`Lyra.Recoil.Dump` / `Lyra.Recoil.ReloadProfile` 这类"动作型"命令用 `FAutoConsoleCommand`。

---

## 7. 自动化测试范式（每阶段验收的技术基础）

### 7.1 现有唯一 spec

**文件**：`Source/LyraGame/Tests/MenuStartElimination.spec.cpp`

| 行号 | 内容 |
| --- | --- |
| L3 | `#if WITH_DEV_AUTOMATION_TESTS && WITH_AUTOMATION_DRIVER` |
| L12–L17 | `BEGIN_DEFINE_SPEC(FMenuStartEliminationSpec, "Lyra.MenuStartEliminationSpec", EAutomationTestFlags::ClientContext \| EAutomationTestFlags::ProductFilter)` |
| L26–L58 | `Define()` —— `BeforeEach` / `Describe` / `It` 结构 |

### 7.2 P0/P2/P3/P4/P5 采用的写法（与现有 spec 的差异及理由）

| 项 | 选择 | 理由 |
| --- | --- | --- |
| 门槛宏 | `#if WITH_DEV_AUTOMATION_TESTS` | **不需要 `WITH_AUTOMATION_DRIVER`**。Driver 只用于模拟 UI 点击（现有 spec 需要点菜单），我们的测试是纯数值，用 Driver 是浪费 |
| 测试宏 | `IMPLEMENT_SIMPLE_AUTOMATION_TEST(TClass, PrettyName, TFlags)` | 已确认存在于 `D:\UE_5.8\Engine\Source\Runtime\Core\Public\Misc\AutomationTest.h` **L4367–L4368** |
| TFlags | `EAutomationTestFlags::EditorContext \| EAutomationTestFlags::EngineFilter` | `EAutomationTestFlags` 在本引擎版本已是 **enum class**（`AutomationTest.h` L88，`EditorContext` L93，`EngineFilter` L131）。用 `EditorContext` 是为了能跑资产相关断言（P1 需要 `AssetRegistry`） |
| 命名 | `Lyra.Recoil.<组>.<用例>` | 与现有 `Lyra.MenuStartEliminationSpec` 前缀一致，便于 `-ExecCmds="Automation RunTests Lyra.Recoil"` 一次跑全组 |
| 位置 | `Source/LyraGame/Tests/` | 与现有 spec 同级 |

### 7.3 已确认的可执行命令

```
"D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject" \
  -ExecCmds="Automation RunTests Lyra.Recoil; Quit" \
  -unattended -nopause -nullrhi -nosplash -log
```

> `UnrealEditor-Cmd.exe` 已确认存在于 `D:\UE_5.8\Engine\Binaries\Win64\`（mtime 2026-09-17 18:44）。
> `-nullrhi` 让纯数值测试无需 GPU；P1 的资产断言也不需要渲染。

---

## 8. 构建

| 项 | 值 |
| --- | --- |
| 构建命令 | `"D:\UE_5.8\Engine\Build\BatchFiles\Build.bat" LyraEditor Win64 Development -Project="D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject" -WaitMutex` |
| 引擎 | 已编译（`UnrealEditor.exe` / `UnrealEditor-Cmd.exe` mtime 2026-09-17 18:45） |
| 工程二进制 | 已有（`Binaries/Win64/UnrealEditor-LyraGame.dll`），可增量编译 |
| 收录方式 | `LyraGame.Build.cs` 的 `PublicIncludePaths.Add("LyraGame")`（L11–L15）→ 新目录用 `#include "Weapons/Recoil/XXX.h"` 即可，**无需改 Build.cs** |
| 已验证依赖 | `Core`、`CoreUObject`、`Engine`、`GameplayTags`、`GameplayAbilities`、`Json`（P5 CSV 用 `FFileHelper`，属 Core） |

---

## 9. 类型契约冻结（本阶段交付）

**新增**：`Source/LyraGame/Weapons/Recoil/LyraRecoilTypes.h`

| 类型 | 用途 | 稳定性 |
| --- | --- | --- |
| `EPoseState { Standing, Crouching, JumpingOrFalling, Count }` | 姿态枚举（计划 §3.2 指定名） | **冻结** |
| `ERecoilState { Idle, Accumulating, Recovering }` | 状态机状态 | **冻结** |
| `ERecoilRandomSeedMode { Fixed, Random }` | 随机种子模式（计划 §3.3 决策 5） | **冻结** |
| `FRecoilPatternPoint { X, Y }` | 归一化 Pattern 点 | **冻结** |
| `FRecoilShotResult { ShotIndex, VerticalKick, HorizontalKick, AccumulatedPitch, AccumulatedYaw, TimeSinceFire, PoseState, PoseMultiplier }` | 单发结果记录，P2 断言 / P3 返回值 / P5 CSV 行 | **冻结** |

### 命名冲突核查
已在 `D:\UE_5.8\Engine\Source` 全量检索 `EPoseState` / `ERecoilState` / `FRecoilPatternPoint` / `FRecoilShotResult` / `ERecoilRandomSeedMode` —— **零命中**，可采用计划文档中的原始命名，无需加 `Lyra` 前缀规避冲突。

### 设计决策记录（冻结）
1. **瞄准（Aiming）不进 `EPoseState`**：它与蹲/站/空中正交。若做成枚举成员，会出现"蹲着瞄准"等 8 种组合。改为 `PoseMultiplier = 姿态倍率 × Lerp(1, PoseMultiplier_Aiming, AimingAlpha)`。
2. **`FRecoilPatternPoint` 只描述形状**：X∈[-1,1]、Y∈[0,1] 归一化，实际幅度由 `RecoilPerShot_Vertical / _Horizontal` 缩放。好处是调威力不破坏 Pattern 形状。
3. **`FRecoilShotResult` 字段顺序 = P5 CSV 列顺序的前缀**（CSV 列：`ShotIndex, VerticalKick, HorizontalKick, AccumulatedPitch, AccumulatedYaw, TimeSinceFire`），保证导出时零映射代码。
4. **单位统一为"度"**：Pitch 上正、Yaw 右正。避免与 UE 的 `FRotator` 符号约定混用出错。

---

## 10. 风险登记（勘察阶段新发现）

| 风险 | 位置 | 对策 |
| --- | --- | --- |
| `ULyraRangedWeaponInstance::Tick()` 只在"当前装备的武器"上被调用（`GetFirstInstanceOfType`） | `LyraWeaponStateComponent.cpp` L36 | 只影响当前持枪，符合预期；但**不能**把后坐力状态放在未装备的武器上 |
| `Tick()` 内已有 `check(Pawn != nullptr)` | `LyraRangedWeaponInstance.cpp` L76 | 后坐力推进代码置于该 check 之后 |
| 工程内零 CameraModifier 先例 | 全工程 grep | P2 必须自行处理 `RemoveCameraModifier` 生命周期，并加入 `EndPlay`/析构清理 |
| `ALyraPlayerCameraManager` 标了 `MinimalAPI` | `LyraPlayerCameraManager.h` L25 | 跨模块访问其成员需导出；CameraModifier 类同属 LyraGame，不受影响 |
| `TAG_Lyra_Weapon_SteadyAimingCamera` 定义在 .cpp 静态 | `LyraRangedWeaponInstance.cpp` L13 | P4 若要在其他文件判断瞄准状态，需把该 tag 提为共享定义，或走 `GetBlendInfo()` 返回值 |

---

*勘察人：祥子，2026-09-17。本报告为 P2/P3/P4/P5 的接入点唯一依据。*
