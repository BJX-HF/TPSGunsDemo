# B-P2 主 agent 验收

2026-10-08，验收人 Codex。结论：**部分初稿，未完成，暂未通过**。

Harness 已创建 Session、统一编辑服务、命令、工具包及逐发表格。完成 Details、资产动作注册、模块依赖和事务/持久化测试之前返回 `ACCOUNT_QUOTA: Insufficient Balance`。目前工具包仍引用缺失的 Details 文件，不能作为可用 GUI 交付。

主 agent 预审指出外部结构身份判定、属性前后通知及结构差异预览需补齐。全部现有文件保留，后续补完并进行完整编译、事务/Undo、保存释放重载、共享曲线和GC测试。此状态不等同于P2完成。

## 主 agent 补齐后的验收

2026-10-08 16:53：**P2 核心事务/持久化通过；完整生命周期手测继续保留为待验收**。执行者为主 agent。

已补齐局部 Details、资产打开注册、模块依赖、原始 Details 高级入口、GC 强引用和关闭解绑；修复修订单调性、重排选择、重复曲线回调、外部未知结构失效，以及 Undo 中间通知误清节点身份的问题。

本轮通过 `AtomicUndoAndIdentity`、`SharedCurveOwnershipAndGC`、`ToolkitUndoNotifications`、`SaveUnloadReloadAndFailure`。保存用例实际保存专用临时包、释放卸载、GC、从磁盘重载，核对原始数组和 RichCurve；另存保持源资产，保存失败仍 Dirty。发现并修复测试副本包的 GC 引用问题，没有掩盖失败记录。

正常 D3D 编辑器中：临时资产第8发 X 从−0.1拖至0.444572，Y保持0.97；一次 Ctrl+Z 回到−0.1，Ctrl+Shift+Z 恢复0.444572，选择保持第8发；Ctrl+S 后星号消失且磁盘文件时间更新。前一轮错误的外部结构诊断在返修后消失。具体手测见 `P3-P5-GUI手测记录.md`。

完整外部对象替换、拖动中重载、多资产关闭重开、UI 另存/共享曲线编辑等人工矩阵尚未全部执行，不据此宣称 T10–T12 的全部 UI 项通过。
