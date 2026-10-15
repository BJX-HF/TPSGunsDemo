# 武器配置与准星换算（2026-10-15）

默认出生快捷栏为：第 1 格 Rifle、第 2 格 ak47、第 3 格 Pistol；默认手持第 1 格。

散布、弹容、初始备弹和射速现在都由对应的后坐力 DA 提供。打开 DA 后，顶部「武器配置（索引 / 弹药 / 射速）」可以编辑后三项；散布在 `Recoil | Spread` 各姿态分组。

| 武器 | 配置资产 | 弹容 | 备弹 | 射速 RPM |
| --- | --- | --- | --- | --- |
| Rifle | `/Game/Weapons/Recoil/DA_Recoil_Rifle_S` | 30 | 60 | 500 |
| ak47 | `/Game/Weapons/Recoil/DA_Recoil_ak47` | 30 | 60 | 500 |
| Pistol | `/Game/Weapons/Recoil/DA_Recoil_Pistol` | 12 | 48 | 500 |
| Shotgun | `/Game/Weapons/Recoil/DA_Recoil_Shotgun` | 8 | 16 | 120 |

`DA_Recoil_Rifle` 是保留预设，已配好散布和 30 / 60 / 500 的武器参数；默认携带步枪实际引用的是 `Rifle_S`。五份 DA 的散布值来自项目现有生成规格，并已开启 `bEnableProfileSpread`。本次使用仅迁移散布的模式，未运行覆盖后坐力的 `-force`。

## 配置如何生效

- `WeaponItemDefinition` 是对应 ID 资产的浏览索引。运行时按 ID → 装备定义 → 武器实例 → RecoilProfile 解析，索引本身不会重新绑定武器。
- `MagazineSize` 决定弹容，新建物品自动满弹匣；`SpareAmmo` 不包含弹匣内子弹。修改后重新创建物品或重新开始 PIE。切枪保留已消耗弹药，拾取点查询默认弹药也读取同一 DA。
- `FireRateRPM` 的单位是发/分钟，开火间隔为 `60 / RPM` 秒。每次开火计时读取当前装备的 DA，因此共用 GA 的步枪也能独立调速。正常蒙太奇播放完成不会提前结束开火间隔；取消和中断仍能结束能力。
- 未启用 `bEnableWeaponConfig` 的旧武器仍使用原物品和能力默认值。`AutoRate` 是蒙太奇播放倍率，不是全自动开关。

## 准星

弹丸与准星共用 `GetEffectiveSpreadAngle()`，返回含姿态、瞄准、移动倍率的全锥角。发射采样与准星投影都使用该角的一半。

```text
焦距像素 = max(视口宽 × Projection[0][0], 视口高 × Projection[1][1]) / 2
像素半径 = tan(有效全锥角 / 2) × 焦距像素
UMG 半径 = 像素半径 / 视口 DPI 缩放
外圈宽高 = 2 × UMG 半径
```

实际相机投影矩阵包含 ADS FOV、宽高比约束及玩家视口。UI 不再重复乘 ApplicationScale；霰弹枪 96 半径上限、外圈固定尺寸上下限及宽度压缩被绕过。标记的外侧对齐选项已正确传给 Slate，非正方形标记旋转后的尺寸也参与内边缘对齐。

准星表示当前下一发的随机散布锥角。`SpreadExponent` 改变锥内概率密度，不改变边界。后坐力造成的整体方向偏移、近距离相机与发射起点视差、命中 sweep 半径不是随机散布角；准星不是任意距离上所有弹着点的绝对包围圈。开火后散布增加，对应下一发的范围。

## 验证与工具

- LyraEditor Development 完整编译通过。
- `Lyra.Recoil` 全部 93 项自动化测试通过。
- 补齐拾取默认弹药读取后重新编译，`Lyra.Recoil.WeaponConfig` 两项复测通过。
- `ProjectionAndDPI` 使用引擎 `ProjectWorldToScreen` 校验真实圆锥边缘方向，覆盖 4 种视口、3 种 FOV、4 种散布角、4 种 UI 缩放；误差小于 0.002 像素。
- 已验证保存后的开火与三个准星蓝图接线。未进行人工 PIE 画面验收。

日志位于 `Saved/WeaponConfigBuild.log`、`Saved/WeaponConfigRegression.log` 和 `Saved/WeaponConfigFinalTests.log`。编辑器启动另有既存烟雾纹理加载错误：`Content/Effects/Textures/Flipbooks/SmokeSwirl_3_Flipbook_CHANNELPACK.uasset` 当前是 134 字节 Git LFS 指针。该错误导致迁移命令行的引擎汇总退出码为非零，但所有目标资产均已保存，随后独立加载与自动化验证通过。

可重用迁移命令（配合 UnrealEditor-Cmd 与本项目路径）：

```text
-run=LyraRecoilAssetGen -spreadonly
-run=LyraWeaponConfig -migrate
-run=LyraWeaponConfig -reticles
```

已启用的 DA 数值不会被重新初始化。`-run=LyraWeaponConfig` 不带迁移参数时只读取配置与蓝图图表，输出 `Saved/WeaponConfigInspection.txt`。
