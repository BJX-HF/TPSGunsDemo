# GunKick 近期 trace 修复记录（2026-09-28）

> **2026-10-01 更新**：本文的“回正目标恒为 0 / Idle 偏移归零”是历史行为，
> 已被 [14_RecoveryToBurstStart.md](14_RecoveryToBurstStart.md) 的压枪抵扣规则取代。
> 有界垂直上限、到顶逐发 Lift/Rebound 和最终相机俯仰边界继续有效。

本文记录 2026-09-28 的三次连续修复。以下规则与测试结果描述当时版本。

## 1. 当时规则

| 环节 | 当时（2026-09-28）行为 |
| --- | --- |
| 输入瞄准 | 鼠标只修改 `ControlRotation`；后坐力修改器只修改最终相机 POV。 |
| 停火回正 | `ComputeRecoveryTarget()` 返回 `0°`；到达 Idle 后 `CameraOffsetPitch/Yaw` 归零，玩家压枪造成的 `ControlRotation` 变化仍由玩家控制。 |
| 垂直累计上限 | `BurstStartPitchOffset + MaxVerticalKick + clamp(AimCompensationPitch, 0, MaxVerticalKick)`。压枪最多增加一个 `MaxVerticalKick` 的额度。 |
| 插值连发到顶 | 本发峰值先受累计上限约束；从这个受限峰值回落本发的回弹量，下一发仍有 Lift/Rebound 行程。 |
| 最终相机俯仰 | 修改器把 `BasePitch + AppliedPitchDegrees` 限制在 `PlayerCameraManager.ViewPitchMin/Max`；没有 CameraOwner 的测试对象使用 `-89°～89°`。 |

这里有两种边界：`MaxVerticalKick` 约束后坐力状态的累计偏移，
`ViewPitchMin/Max` 约束最终显示角度。相机边界不会改写 `ControlRotation`，
也不会替代后坐力状态中的累计上限。

## 2. Trace 所见、原因与修改

### 停火后俯仰角被改

旧回正目标把压枪量留在显示层。进入 Idle 后，鼠标控制角已包含玩家的下拉，
最终 POV 又叠加了非零后坐力偏移；因此停火后仍存在持续的视角差。
现在显示偏移在回正结束时归零，不把该偏移写进 `ControlRotation`。

### 一直下压会碰到视角最底部

修复前的垂直上限是 `BurstStartPitchOffset + MaxVerticalKick + AimCompensationPitch`。
trace 中玩家从约 `+89°` 一直压到约 `-89°`，`pushComp/cover` 增至约 `178°`，
后坐力 `push` 曾升至约 `35.19°`。每多压 `1°` 又增加 `1°` 后坐力额度，
形成追逐反馈。现在压枪抵扣最多为一个 `MaxVerticalKick`；相机修改器在加 Pitch 时
直接按可视范围钳制，防止跨过 `±90°` 的旋转极点。

### 连发一段时间后垂直后坐力消失

首次修复把压枪抵扣完全移除后，后续 trace 中 `Interpolated` 仍持续在
`Lift/Rebound/Settle` 阶段运行，但 `acc/push` 长时间固定为 `7.5000°`。
旧做法对本发未经约束的 Peak 和 ReboundEnd 分别钳制；两者都超过硬上限时，
都会变成同一个 `7.5000°`，逐发垂直脉冲因此消失。

当前 `ComputeBoundedShotAnchors()` 先计算受限 Peak，再从该 Peak 减去
`本发幅度 × (1 - ReboundRatio)` 得到回弹终点。到顶后的回弹会为下一发腾出空间；
同时恢复有界压枪抵扣，避免正常压枪时过早到顶。

## 3. 验证与实机复验

2026-09-28 最后一次修改后，`LyraEditor Win64 Development` 编译成功；
`Lyra.Recoil` 自动化测试 **55/55 通过**，包括：

- `Lyra.Recoil.CameraModifier.ClampsPitchBeforeRotatorPole`：上边界附近不会越过旋转极点；
- `Lyra.Recoil.Compensation.EffectiveLimitUsesBurstBaseline`：压枪额度最多增加一个 `MaxVerticalKick`；
- `Lyra.Recoil.Interp.SaturatedBurstKeepsPerShotPulse`：累计值到顶后，后续单发仍有可见的峰值到回弹变化。

这些是构建和数值测试结果；修改后的完整 PIE 连发手感、第三人称轨道在视角边界处的观感，
仍需实机确认。轨道代码读取修改器请求施加的 Pitch，而最终 POV 会再按视角边界钳制；
两者在边界附近可能不完全一致，这也是第 4 步需要观察的原因。复验步骤：

1. 在 PIE 控制台输入 `Lyra.Recoil.Trace 1`，对着固定参照物连续开火至接近空弹匣，期间持续下压；再停火等待回正。
2. 检查同一轮连发中 `pushComp` 增长后，`push/acc` 不会超出有界上限；达到平台后，后续每发仍有 Lift 峰值和 Rebound 回落，而不是一条平线。
3. 检查最终 POV Pitch 不跨过视角上下限；回到 `state=0 stage=0` 后，`push` 应为零。没有其他相机效果时，POV Pitch 应与控制角一致（允许日志采样的一帧延迟）。
4. 检查 `CamRel` 和画面朝向在视角边界附近是否协调。完成后输入 `Lyra.Recoil.Trace 0` 关闭逐帧日志。

`Lyra.Recoil.Trace` 默认是 `0`（关），`1` 为开；日志写在 `Saved/Logs/TPSGunsDemo.log`。
若要记录逐帧 GunKick CSV，另用 `Lyra.Recoil.VisualTrace 1/0` 与 `Lyra.Recoil.VisualDump`；
它和上面的日志开关是两套命令。

## 4. 实现位置

- `Source/LyraGame/Weapons/Recoil/LyraRecoilState.cpp`：`ComputeRecoveryTarget()`、`GetEffectiveVerticalKickLimit()`、`ComputeBoundedShotAnchors()`。
- `Source/LyraGame/Camera/LyraCameraModifier_WeaponRecoil.cpp`：最终 POV Pitch 边界。
- `Source/LyraGame/Tests/LyraRecoilTest.spec.cpp`、`LyraRecoilRollShakeTest.spec.cpp`：本次回归测试。
