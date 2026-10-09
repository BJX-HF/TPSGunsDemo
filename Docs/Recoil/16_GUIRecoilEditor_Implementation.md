# 任务15 GUI 后坐力编辑器实施说明

开发依据：[完整开发计划](15_GUIRecoilEditor_DevelopmentPlan.html)。实际进度、委派单、开发报告和主 agent 验收统一保存到 [后座GUI开发](后座GUI开发/进度.md)。本说明随实施更新，不把尚未验证的阶段标成完成。

## 数据与模块边界

编辑器位于现有 `LyraEditor`，唯一持久配置仍是 `ULyraRecoilProfile`。固定段画布使用累计角度，逐发表格使用原始归一化参数；转换通过 Adapter。随机尾段来自现有运行时，仅作只读数值显示。会话 ID、选择、视图和预览快照不进入运行时 Profile。

运行时沿用任务14回正到本轮起枪角的实现。正式 Profile、现有 Golden、LyraGame 算法不因本任务自动调整。

## 来源与许可

- 上游固定版本：[CrystalRecoil d977456a8468ab15c7ca05ede8cddf72dd5e5bc5](https://github.com/CrystalVapor/UE5-CrystalRecoil/tree/d977456a8468ab15c7ca05ede8cddf72dd5e5bc5)。
- [MIT 许可全文](ThirdParty/CrystalRecoil-LICENSE.txt)，Copyright (c) 2024 CrystalVapor。
- 画布坐标、网格、命中、多选、框选、缩放和命令组织的参考文件：`CRRecoilUnitGraphEditor.cpp`、`CRRecoilUnitGraphBackgroundWidget.cpp`、`CRRecoilUnitGraphWidgetDragOperations.cpp`、`CRRecoilPatternEditorCommands.cpp`。
- 专用 Toolkit 与资产动作的参考文件：`CRRecoilPatternEditor.cpp`、`CRAssetTypeActions_RecoilPattern.cpp`；实际接入使用本机 UE 5.8 原生 AssetDefinition，旧 AssetTypeActions 保留兼容调用。
- 本项目的归一化桥接、候选校验、事务、稳定 ID、随机尾段、共享曲线与运行时预览重新实现；未安装上游 Runtime 插件。
- 本轮按上述上游行为参考独立实现，没有引入上游 Runtime 模块或实质复制上游源文件。完整许可单独归档，以下为实际代码清单。

## 实际代码清单

源码均在 `Source/LyraEditor`。以下同名文件包含 `.h/.cpp`：

| 文件 | 职责与来源 |
| --- | --- |
| `Private/AssetDefinition_LyraRecoilProfile` | UE 原生资产定义注册表中的默认 GUI 入口；旧 AssetTypeActions 仅保留兼容调用，不再注册 |
| `Private/Recoil/LyraRecoilEditorRouting` | GUI/原始 Details 双向顶部入口、资产右键入口与窗口复用 |
| `Private/Recoil/LyraRecoilProfileEditor` | Toolkit、布局、命令路由、Undo 与资产生命周期；参考上游编辑器组织，独立实现 |
| `Private/Recoil/SLyraRecoilPatternGraph` | 累计画布、命中、框选、拖动、缩放；参考上游交互行为，独立 Slate 实现 |
| `Private/Recoil/LyraRecoilEditorCommands` | 命令与快捷键；参考上游命令组织，独立实现 |
| `Private/Recoil/LyraRecoilPatternAdapter` | 本项目固定/随机段映射、反算、结构编辑与剪贴板合同 |
| `Private/Recoil/LyraRecoilEditorSession` | 临时稳定节点 ID、选择、修订及事务恢复 |
| `Private/Recoil/LyraRecoilEditOperations` | 候选校验、单次事务与属性变更通知 |
| `Private/Recoil/LyraRecoilProfileDetails`、`SLyraRecoilShotTable` | 本项目局部属性、共享曲线操作和原始逐发表格 |
| `Private/Recoil/LyraRecoilPreviewController`、`SLyraRecoilPreview` | 深快照及既有运行时驱动、分帧预算、曲线与逐发表 |
| `Tests/LyraRecoilEditor*Test.cpp` | 映射、预览、事务、实际 Toolkit 通知及保存卸载重载回归 |
| `Tests/LyraRecoilEditorUIFixtures.cpp`、`LyraRecoilEditorPIEFixtures.cpp` | 仅 Editor 的显式人工验收夹具，临时资产位于 Saved |

`LyraEditor.Build.cs` 与模块启动/关闭注册同步更新；所有新增依赖及测试夹具只在 Editor 模块。没有修改 `Source/LyraGame`、正式 `Content` 资产或现有 Golden。

## 操作说明

在 Content Browser 双击 `ULyraRecoilProfile` 打开专用编辑器。顶部为原始逐发表格、累计画布、局部 Details，底部为数值预览。N 是原始数组长度，L 是固定段长度；随机尾段在图中以只读虚线显示，表中保留存储的原始参数。

2026-10-09 默认入口改为原生 `AssetDefinition`。GUI 顶部“原始参数（Details）”打开引擎完整参数页，原始页顶部“后坐力 GUI”返回专用编辑器；资产右键也可显式选择任一入口。另一台电脑需同步源码并完整编译 LyraEditor，旧 DLL 不包含这些入口。详见 [本轮验收](后座GUI开发/验收/20261009-默认入口与页面切换.md)。

画布编辑固定段累计角度；表格编辑 X/Y 归一化值。候选超出 X[-1,1] 或 Y[0,1] 时整次拒绝并显示诊断。拖动在释放时形成一次事务，取消、无变化或拒绝不写入资产。高级强度调整会先展示对其他发及随机尾段的影响，确认后原子提交。

| 输入 | 操作 |
| --- | --- |
| 左键、Ctrl+左键、Shift拖动空白 | 选择、多选、框选 |
| 中键拖动、滚轮 | 平移、围绕鼠标缩放 |
| Home、F、G | 适配全部、适配选择、切换吸附 |
| 方向键、Shift+方向键 | 微调、十倍步长；按键释放提交一次事务 |
| Esc | 取消当前草稿手势 |
| Ctrl+Z、Ctrl+Shift+Z | 撤销、重做 |
| Ctrl+C、Ctrl+V、Ctrl+Shift+V | 复制、按归一化参数粘贴、按源角度反算粘贴 |
| Insert、Delete、Alt+上下 | 插入、删除、重排选中发 |
| Ctrl+S | 校验并保存当前 Profile 包 |

快捷键需对应编辑面板获得焦点。重排/排序先显示 L/N、顺序与曲线索引的影响。固定段边界处默认插入随机尾段，明确选择固定段插入时才扩展 L。另存使用引擎标准 Save As。

外部共享曲线默认只读；可显式复制到内联曲线、复制新资产或切换引用。保存 Profile 不自动保存共享曲线包。局部 Details 中的“原始 Details（高级）”打开通用资产编辑器；外部结构修改无法证明节点对应关系时重建会话身份并取消草稿。

预览支持射速、发数、两轮间隔、起枪角、姿态、ADS、倍率、种子、运行模式和压枪输入。控制器复制 Profile 及外部曲线，用既有运行时函数推进；预览参数与结果不持久化到 Profile。修改资产后刷新快照。理论累计、Camera Pitch/Yaw、Visible Pitch、Control Pitch 曲线与逐发表来自同一模拟结果，播放倍率只改变回放位置。

## 当前验证

2026-10-09 `LyraEditor Win64 Development` 第23轮完整构建成功。其后的 `Lyra.Recoil` 83/83通过，前轮82项及原57项完整保留，0 failed、notRun、inProcess、warnings，运行器及引擎退出码0；报告为 `Saved/Automation/RecoilGUI-RoutingRefresh83-Final-20261009-172931/index.json`。顶部按钮刷新修复及正常 UI 复验见 [默认入口验收](后座GUI开发/验收/20261009-默认入口与页面切换.md)。此前非 Editor `LyraGame` 构建及实际 Cook/Stage/Package/Archive 成功；失败轮次及运行器归档编码问题也保留在进度中。

正常 GUI 已验证四面板、图表同步、拒绝越界候选、拖动、选择保留、Undo/Redo 与临时资产保存。打包游戏实际载入旧 `DA_Recoil_Rifle_S` 并开火，IoStore/Pak 清单未包含 Editor 资产或模块。

2026-10-09 正常 UI 已确认 Rifle_7 双击默认进入 GUI，GUI/原始 Details 两个顶部按钮及两个右键入口实际往返成功、窗口复用，All Saved，正式资产文件哈希不变；此次使用-nosound，不扩展为带音频退出验收。

任务15尚未全量验收：真实 PIE 第三轮完成16场景32轮，有3项失败，见 `后座GUI开发/验收/PIE-Verify3-Failed.txt`；真实100%/150%/200% DPI已验证并修复显示与点击问题，原125%已恢复；1000节点性能和完整人工交互矩阵仍待完成；打包游戏 Alt+F4 出现引擎断言，带音频编辑器退出也有失败记录。用户再次请求验证后已恢复 UI 验证，详见 [续验记录](后座GUI开发/验收/20261008-续验.md) 和 [此前正常 GUI/PIE/打包记录](后座GUI开发/验收/P3-P5-GUI手测记录.md)。

## 后续验收入口

以下控制台命令仅用于 Editor 的开发验收，需显式调用，不属于打包游戏功能：

- `Lyra.Recoil.GUIQA.Open`：复制正式资产至 `Saved/RecoilGUI/Interactive` 并打开临时 Toolkit。
- `Lyra.Recoil.GUIQA.Report`：记录当前字段、选择、修订、Undo 状态及窗口 DPI。
- `Lyra.Recoil.GUIQA.Thousand`：创建1000节点临时样例供正常 GUI 性能验收。
- `Lyra.Recoil.GUIQA.LoadSaved QA_11E4A8434EA6886AC60540A3DBD2BE19`：重载本轮已保存的临时样例。
- `Lyra.Recoil.GUIQA.PIEArm`：等待真实 PIE 世界就绪，验收保存样例及压枪矩阵；报告目标为 `Saved/RecoilGUI/PIE/MatrixReport.txt`。当前尚无成功矩阵报告。

自动化入口脚本为 `后座GUI开发/任务/Run-Automation.ps1`。继续验收时以新报告、实际正常 UI 与真实武器运行结果为依据，不能用已通过的 NullRHI 自动化替代尚未完成的人工项目。
