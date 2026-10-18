# Chromium / Electron 花屏卡死修复 —— 新用户使用指南

> **适用范围（很重要）**：本补丁的页镜像与代码偏移来自如下环境，其他 macOS/驱动版本**不会匹配**，
> 补丁会安全地不生效（不会乱改），但问题也不会被修复：
>
> - macOS 15.8.1 (24H32)
> - `AMDRadeonX5000GLDriver` 7.0.25
> - `AMDRadeonX5000MTLDriver` / dyld shared cache（同一 15.8.1 系统）
> - AMD Renoir 核显（本机为 Ryzen 7 5800U，device `0x1638`），NootedRed 0.9.0
>
> 其他系统/GPU 请不要直接套用；需要重新生成页镜像与补丁偏移。

## 这是什么

NootedRed 内核 kext 在用户态驱动页面被加载时，在内存中打三个补丁：

| 目标 | 修改 | 作用 |
| --- | --- | --- |
| GL 驱动 `_gfx9_HwInfoInitData` | `numShaderEngines` 4 → 1 | 把 Renoir 的 CU 数从错误的 32 修正为 8 |
| GL 驱动 `UpdateHwCbDualQuadMode` | 强制 `CB_COLOR_CONTROL.DISABLE_DUAL_QUAD=1` | 修复 OpenGL 双源混合卡死/花屏 |
| Metal 驱动 render pipeline 设置 | 强制 `CB_COLOR_CONTROL.DISABLE_DUAL_QUAD=1` | 修复 ANGLE Metal（Chrome/Electron 实际路径） |

补丁只在进程页校验通过后写入**内存中的页面**，不修改磁盘上的系统缓存或 kext；也不会对不匹配的页面生效。

## 方式 A：使用已构建的 kext（同一 15.8.1 环境）

1. 备份 EFI 分区（至少备份 `EFI/OC/config.plist` 和 `EFI/OC/Kexts`）。
2. 挂载 EFI：
   ```sh
   diskutil list                      # 找到 EFI 分区，例如 disk0s1
   sudo diskutil mount disk0s1
   ```
3. 用构建好的 `NootedRed.kext` 替换 `EFI/OC/Kexts/NootedRed.kext`。
   - 构建产物：`build/macosx/x86_64/release/NootedRed.kext`
   - 压缩包：`build/macosx/x86_64/release/NootedRed-0.9.0-RELEASE.zip`
4. 在 `EFI/OC/config.plist` 的 `Kernel -> Add` 中确认 NootedRed 条目（放在 Lilu 之后）：
   ```xml
   <dict>
     <key>Arch</key><string>Any</string>
     <key>BundlePath</key><string>NootedRed.kext</string>
     <key>Comment</key><string>NootedRed</string>
     <key>Enabled</key><true/>
     <key>ExecutablePath</key><string>Contents/MacOS/NootedRed</string>
     <key>MaxKernel</key><string></string>
     <key>MinKernel</key><string></string>
     <key>PlistPath</key><string>Contents/Info.plist</string>
   </dict>
   ```
5. 在 `NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82 -> boot-args` 末尾追加：
   ```
   -NRedGLFix
   ```
   例如：
   ```
   keepsyms=1 debug=0x144 itlwm_he=1 -NRedGLFix
   ```
   （也可以保留现有参数，只在末尾加 `-NRedGLFix`。）
6. 保存 `config.plist`，重启。

## 方式 B：从源码构建

1. 准备 [AppleForge](https://git.chefkiss.dev/AppleHax/AppleForge)（与本仓库同级，或设置 `AppleForgePath`）和 xmake。
2. 应用补丁：
   ```sh
   git apply /path/to/chromium-gl-metal-fix.patch
   ```
   补丁文件在本仓库：`build/chromium-gl-metal-fix.patch`（包含 `NRed.cpp` 修改和新增的
   `UserPatches.hpp/.cpp`）。
3. 构建：
   ```sh
   xmake f -m release -y
   xmake -y
   ```
4. 之后按方式 A 的步骤 1–6 安装 kext 并加 `-NRedGLFix`。

## 如何确认生效

- 查看 kext 是否加载：
  ```sh
  kextstat | grep -i NootedRed
  ```
- 查看补丁日志（需要 root）：
  ```sh
  sudo log show --last boot --info --debug --predicate 'eventMessage CONTAINS "UserPatch"'
  ```
  正常应看到 `Enabling user-space GL driver patches` 和 `Patched ... site(s)`。
- 直接测试：
  - 打开之前会花屏/卡死的 Chromium/Electron 应用，观察几分钟。
  - 或运行 `tools/gl_blend_probe --inspect` / `--render --yes`（GL 测试；`--render` 有风险，需先确认桌面稳定）。

## 关闭 / 回滚

- **只关用户态补丁**：从 boot-args 删除 `-NRedGLFix`（或加 `-NRedGLFixOff`），重启。
  NootedRed 的其他内核补丁仍正常。
- **恢复旧 kext**：用你自己的 EFI 备份覆盖 `EFI/OC/Kexts/NootedRed.kext`。
- 当前修复的备份（本机）：
  - `build/efi-backup/NootedRed-A1only.kext`（只有 GL A1）
  - `build/efi-backup/NootedRed-A1A2.kext`（GL A1+A2，无 Metal 补丁）
  - 原始 kext 请以用户自己的备份为准。

## 注意

- 这是**实验性**用户态补丁，默认关闭，必须加 `-NRedGLFix` 才启用。
- 不写系统磁盘缓存；只改进程加载/校验时的内存页。
- 与 BFixup 等其它用户态 OpenGL 补丁**不要同时使用**。
- 其他 macOS 版本、其他 AMD GPU、Intel/Apple Silicon 不适用。
- 本项目 License 为 ChefKiss Open Knowledge License v1；分发/修改请遵守该 License。
