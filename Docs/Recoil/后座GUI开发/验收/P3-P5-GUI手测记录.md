# 正常 GUI / PIE / 打包验收记录

主 agent，2026-10-08。正常编辑器使用 D3D 渲染；自动化另使用 NullRHI。两类证据不互相替代。

## 已执行

1. 新入口成功打开 `ULyraRecoilProfile` 专用 Toolkit，正常显示逐发表格、累计画布、局部 Details、时间预览四个面板。样例固定 L=8、数组 N=12，表格尾段保留原始 X/Y，画布尾段为只读虚线。
2. 所有可写手测使用 `Saved/RecoilGUI/Interactive` 临时复制资产，正式 Content 资产未写入。
3. 吸附开启时横移第8点导致 Y 反算1.02473571，候选整次被拒绝，显示具体发序号、字段、值与允许范围，原始值/Dirty不变。
4. 关闭吸附后成功横移第8点，X变为0.444572，Y仍0.97，其余原值保持。图与表同步，星号/Dirty出现。
5. 初次手测发现 Undo 虽恢复原值却误清选择身份，验收退回。主 agent 修复 `GIsTransacting` 通知时机后，复验 Ctrl+Z / Ctrl+Shift+Z 均恢复相应字段及第8点选择，没有错误的外部结构诊断。新增实际绑定 Toolkit 的回归用例也通过。
6. Ctrl+S 保存临时 Profile，星号消失，文件 `QA_11E4A8434EA6886AC60540A3DBD2BE19.uasset` 更新时间16:40:13；后续新进程启动命令使用 `GUIQA.LoadSaved` 重载这一磁盘资产。
7. 预览绘图区未覆盖参数控件；正常显示时间曲线和逐发方向/相机数据。补齐 Control Pitch 紫色通道及图例。
8. 正常编辑器点击 Play，进入 `/ShooterCore/Maps/UEDPIE_0_L_ShooterPerf`，实际 Pawn、武器、HUD 显示。首次 PIE 验收驱动未成功找到 firing ability，**未通过**；主 agent 将筛选修正为真实 RangedWeapon ability，等待重新执行。

UI日志：`Saved/Logs/RecoilGUI-Interactive.log`、`Interactive2.log`、`Interactive3.log`、`Interactive4.log`（完整前缀均为 `RecoilGUI-`）。GUI 与 PIE 验收夹具只属于 LyraEditor，需明确控制台调用才执行。

## 打包

- 非 Editor `LyraGame Win64 Development` 构建成功。
- 实际 Cook/Stage/Package/Archive 成功，UAT exit0，43分34秒，日志 `../日志/P5-CookPackage.log`。
- 打包归档：`Saved/RecoilGUI/Packaged/Windows`。启动实际 exe 后进入 FiringRange，鼠标实际开火，弹匣30→29。
- `Lyra.Recoil.Dump` 实际导出1发，日志明确 `profile=DA_Recoil_Rifle_S shots=1`；原生导出命令忽略额外路径参数，主 agent 从实际生成路径归档到 `P5-PackagedWeapon.csv`。
- 三个 IoStore 容器清单与 Pak0 清单已归档。包含旧 `DA_Recoil_Rifle_S/7`，未发现 LyraEditor、RecoilGUIQA、RecoilPIEQA、CrystalRecoil 或 Session 资产。归档没有 LyraEditor 二进制。
- 启动用了 `-nosound`，既有武器音效蓝图出现 TapIds/CountdownAudio 警告；未修改这部分代码或配置。
- Alt+F4 关闭游戏时出现引擎 `SceneViewport.cpp:196 IsInGameThread()` 断言。加载/开火已有证据，但**退出验收未通过**，不能称游戏全流程通过。没有修改引擎或 LyraGame 规避这一问题。

## 保留为待验收

- 用户按物理 Escape 停止 Computer Use 后，主 agent 停止本轮全部后续电脑控制。
- 系统“设置”应用访问授权请求超时；真实100%/150%/200% Windows DPI尚未完成，也未用 Slate ApplicationScale 冒充真实系统DPI。
- 1000节点临时夹具命令已实现，但正常UI拖动/帧耗时未测量。
- 重新执行修正后的真实PIE驱动：GUI保存字段的8发核对、16场景32轮压枪、两模式/0°与30°起枪；尚无通过报告。
- 补齐框选/多选/重叠/取消/快捷键焦点、外部修改/对象替换/共享曲线/另存等完整人工矩阵。
- 复验打包游戏正常退出，保留首次Alt+F4失败证据。

这些待验收项使任务15尚不满足全量 Definition of Done。
