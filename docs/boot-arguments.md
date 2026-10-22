# 修复功能启动参数

本版只使用以下三个入口，不兼容此前的修复启动参数。

| 参数 | 功能 | 省略时 |
| --- | --- | --- |
| `-NRedDisplayFix` | 花屏／卡顿修复，包含 SubmitGuard；按系统选择渲染补丁 | 显示修复全部关闭 |
| `-NRedVideoFix` | VCN 硬件视频加速，包含现有解码和编码路径 | VCN 桥接关闭 |
| `NRedECFix=1` | 使用固件报告的 EC 背光来源 | EC 服务关闭 |

三项均启用的启动参数：

```text
-NRedDisplayFix -NRedVideoFix NRedECFix=1
```

只有需要主动切换固件控制来源时，才把 `NRedECFix=1` 改为 `NRedECFix=2`。
`=2` 沿用强制模式，仍须核验固件调用结果和报告来源；本机固件的来源读取固定返回 2，
不能仅凭该值证明控制权切换。`=0`、其他无效值或省略参数均不启动 EC 服务。

显示开关在 macOS 14/15 启用 LegacyBlend 和 SubmitGuard，在 26 启用
ImmediateSync、SrdShared、LegacyBlend、ComputeScratch 和 SubmitGuard。
**本版不含 HDRIntegerFilter 候选的运行代码与补丁数据。**
设备、缓存、完整代码和 ABI 核验仍然生效，具体版本边界见
[多系统适配](macos-multi-os-compat.md)。

显示、视频和 EC 三项可独立启用，关闭某一项只需移除对应参数并重启。
插件全局关闭 `-NRedOff`、调试参数及上游的 `AMDBacklight` 设置不在本次更名范围内；
`AMDBacklight=0` 仍会阻止 EC 服务启动。

以下旧修复参数应从配置中移除，本版不会读取：

```text
-NRedImmediateSync -NRedSrdShared -NRedLegacyBlend -NRedComputeScratch
-NRedTransfer1x1 -NRedVCN -NRedNoSubmitGuard -NRedHDRIntegerFilter
NRedECBacklight=1 NRedECBacklight=2
```

诊断属性仍沿用模块名，例如 `NRedVCN*`、`NRedComputeScratch*`、`NRedECBacklight` 服务。
这些名称是运行状态，不是旧参数兼容入口。
`Enabled` 仅表示已注册；确认实际补丁写入还需检查 `Applied`／`WindowServerApplied`。
