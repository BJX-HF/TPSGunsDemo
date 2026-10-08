# C-P4 主 agent 验收

2026-10-08，验收人 Codex。**初次交付退回返修**，不标记 P4 完成。

实际检查 `LyraRecoilPreviewController.cpp`，发现原子步未在输入脚本时间拆分，缺少 Profile/输入/输出有限数阻断，覆盖模式回退存在残留风险，新轮次理论 Kick 的索引需要区分弹道发前值和实际开火结果。完整修复条件见 `../任务/C-P4-返修1.txt`，已经委派同一个 Harness session。

开发报告说明子 agent 的 shell 受限；主 agent 的构建/自动化环境可用。因此不将 shell 限制视为代码无问题的证据，也不重复要求用户修机器。

下一轮验收：检查修复差异，编译，运行全部 Preview 测试及既有57项回归，核对独立非帧边界输入场景；真实 UI/PIE 另记。

## 主 agent 返修后的验收

2026-10-08 16:53：**P4 数值合同通过，PIE 验收仍未完成**。Harness 返修因账号余额不足中断，实际修复由主 agent 执行。

重写独立离线时间表参考驱动，修正逐事件/逐帧样本混用、浮点事件边界和测试夹具时长；运行时算法保持原样。控制器在玩家输入和发射时间处拆分外层推进，分别记录发前方向索引与实际 ApplyShot 发序号；不手工重置连发。加入非有限输入/输出阻断、来源模式恢复、可取消预算推进和完整外部曲线快照隔离。

本轮 Preview 共11组通过：原9组加 `SubframeInputBoundary`、`RejectNonfiniteAndRestoreSourceMode`。覆盖两模式、0°/30°起枪与0/4/10/11°下压可见终点、两轮/Drop重开火、饱和、长帧、固定种子、姿态/倍率、30/60/120Hz同输入逐样本对照、缓存与回放。

正常 UI 显示独立理论/Camera Pitch/Camera Yaw/可见 Pitch/Control Pitch 曲线；绘图区使用自己的 Geometry 和 Clip，预览通过 ActiveTimer 预算推进。真实 PIE 的保存字段和游戏相机矩阵尚未产生成功报告，不能用自动化替代。
