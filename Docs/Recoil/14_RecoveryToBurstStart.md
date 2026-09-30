# 回正到本轮起枪角，并保留超压角度（2026-10-01）

## 需求与可见结果

设第一发开火前的可见角度为 A，本轮累计上跳为 K，玩家下压为 P。
玩家停止输入后，回正终点为 `A - max(P - K, 0)`。

| 起枪角 A | 累计上跳 K | 下压 P | 回正后可见角度 |
| --- | --- | --- | --- |
| 0° | 10° | 0° | 0° |
| 0° | 10° | 4° | 0° |
| 0° | 10° | 10° | 0° |
| 0° | 10° | 11° | -1° |
| 30° | 10° | 4° | 30° |
| 30° | 10° | 11° | 29° |

## 原因与实现

9 月 28 日把 `ComputeRecoveryTarget()` 改成恒返回 0。玩家的控制角已经下压 P，
显示偏移再全部清零，最终可见角度就成了 `A - P`，与本次需求不符。

相机仍使用 `POV = ControlRotation + CameraOffset`。设本轮起始偏移为 B：

```text
K = max(RecoveryPeak - B, 0)
回正偏移目标 = B + clamp(冻结的压枪量, 0, K)
```

因此 10° / 4° 的控制角为 -4°，保留 4° 偏移后画面为 0°；
10° / 11° 的控制角为 -11°，偏移最多保留 10°，画面为 -1°。
Idle 中这部分偏移用于抵消已发生的压枪输入；下一轮以当前可见位置重新建立起点。
回正中再次开火仍中断旧回正，以重开火瞬间的可见角度作为新一轮起点。

`bCompensationAwareRecovery` 默认开启；关闭时仍使用偏移归零的 A/B 行为。
Yaw 默认不抵扣。压枪采样、开始回正时冻结快照、回正时长和曲线沿用原有机制。
本次的 K 使用本轮实际受限的后坐力峰值，而不是把受上限截断的理论 Kick 也算进去。
插值模式有逐发回弹，不能直接用枪械配置中的“每发 Kick × 发数”代替运行时累计峰值。

长帧保护可能跳过正常的 Settle → Drop。该路径也先求本发受限峰值，
然后通过同一个 `ApplyRecoveryStep()` 收尾，避免使用旧峰值或未钳制峰值。
若已进入 Drop，则保持原先冻结的峰值与压枪快照，卡顿期间的新鼠标输入不会改写回正目标。

9 月 28 日的有界抵扣上限、饱和后的逐发脉冲、相机最终角度边界继续有效。

## 验证

`LyraEditor Win64 Development` 构建成功（`Result: Succeeded`）。
完整 `Lyra.Recoil` 自动化测试 **57/57 通过**，0 failed、0 notRun，进程退出码 0。
`git diff --check` 通过。PIE 手感尚未人工验收。

验证文件（工程根目录下）：

- 构建日志：`Saved/Logs/RecoilRecoveryBuild-20261001.log`
- 测试日志：`Saved/Logs/RecoilRecoveryTests-20261001-Final.log`
- 测试报告：`Saved/Automation/RecoilRecovery-20261001-Final/index.json`

新增 `Lyra.Recoil.Compensation.TenShotVisibleAngles`：每发 1°，逐发推进并输入压枪，
覆盖两种单发模式、0°/30°起枪角、0°/4°/10°/11°压枪、连续两轮共 32 组验收。
新增 `Lyra.Recoil.Compensation.LongFrameUsesBoundedPeak`：卡顿收尾时检查部分压枪、超压终点和 Drop 中快照冻结。
原有回正、Drop 重开火、Idle 重开火、压枪冻结等测试同步改为本次需求。

实机复验：开启 `Lyra.Recoil.Trace 1`，对固定参照物连发后松开鼠标等待回正。
未压住应回起枪角，超压应保留超出的下压角度；随后重复开火检查新起点。
验收以最终可见视角为准，不能再把 Idle 的 `push == 0` 当作通过条件。
完成后用 `Lyra.Recoil.Trace 0` 关闭日志。
