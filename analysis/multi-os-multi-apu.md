# 多 macOS 版本 / 多 APU 支持设计

日期：2026-10-19  
分支：`feature/multi-os-multi-apu`  
基线：`master`（条件化 gate/setz 修复）

## 目标

把当前只针对 macOS 15.8.1 / Renoir 的 page-image 修补，扩展为可维护的多版本、多 APU 框架：

- 每个 macOS 构建一份 `PatchSet`；
- 每个 `PatchSet` 可限定 APU family；
- 运行时按 Darwin 版本和 APU attributes 选择唯一匹配的 set；
- 仍以完整 4 KiB 页镜像作为最后防线，未匹配就 no-op。

## 架构

### `PatchSet`

```cpp
struct PatchSet {
    const char* name;
    KernelVersion minVersion;
    KernelVersion maxVersion;
    ApuMask apus;
    const PagePatch* patches;
    size_t count;
};
```

- 版本精确到 Darwin major.minor（例如 15.8.1 = Darwin 24.6.0 → `(24, 6)`）；
- 不跨版本复用页镜像；新增版本必须新增 set，不能把 15.8.1 的页镜像标成 15.x 全匹配；
- `apus` 支持 Raven/Raven2/Picasso/Renoir/RenoirE/GreenSardine 位掩码；
- 首个匹配的 set 被选中；没有匹配则完全不打补丁并打印日志。

### 运行时选择

`UserPatches::selectPatchSet()` 在 patcher 初始化时运行：

```text
version = currentKernelVersion()
apus    = attributes -> ApuMask
for set in sets:
    if version in [min, max] and set.apus & apus:
        select set
        return
no patch
```

`NRed::init` 中 `UserPatches::init()` 被移到 `NRed` 的 patcher-load 回调注册之后，确保 APU attributes 已经由 `NRed::processPatcher` 填好。

### 多 APU

目前所有 gfx9 APU 共用同一份 15.8.1 页镜像：

- GL A1/A2：同一 `AMDRadeonX5000GLDriver` 页；
- Metal gate/setz：同一 `AMDRadeonX5000MTLDriver` shared-cache 页；
- Renoir/Raven/Picasso/Raven2 都走同一个 Vega10 HwInfo 分支（A1 只改第 1 个分支），所以当前 `kApuAll` 是合理的；
- 如果未来某颗 APU 走不同 HwInfo 分支，新增一个 `PatchSet` 并设置对应 `apus` 即可，不需要改后端。

**限制**：当前还没有 Raven/Picasso/Cezanne 的实机验证；`kApuAll` 是代码层面的兼容范围，不是已验证声明。

## 新增一个 macOS 版本

1. 在目标 macOS 的**原始/未修补**驱动上运行：

```sh
python3 tools/gen_patch_data.py tools/patch-specs/<version>.json --out /tmp/new-set.cpp
```

2. 把生成的 `k...Page`、`kGeneratedPatches` 和 `PatchSet` 加入 `UserPatches.cpp`；
3. 修改 `darwin` 为目标的 Darwin major.minor；
4. 在 `UserPatches` 的 `kGLDriverPatchSets` 里增加对应条目；
5. 重新构建并验证：
   - 启动日志出现 `Selected patch set "<name>"`；
   - GL 双源混合、Metal 单源/双源 control 正确；
   - 无 GPU reset。

## 页镜像生成工具

`tools/gen_patch_data.py` 接受 JSON spec，校验每个 find 在页内存在、skip/count 正确后才输出 C++：

- `gl_pages`：从 GL driver bundle 的指定文件偏移读取页；
- `cache_pages`：从 dyld shared cache 子文件按 VA 读取页；
- 输出页数组、find/replace 数组、`PagePatch` 表和 `PatchSet`。

工具不会猜测偏移；必须由人提供目标版本的页偏移/VA 和指令字节。

## 安全边界

- 每个补丁仍然要求完整页镜像匹配；
- 版本不匹配 → no-op，不会改错页；
- 新增版本的 set 必须逐版本实测，不能仅凭“页镜像类似”认定可用；
- 完整 RB+ source-level 路线已证明不可行（见 `analysis/hwinfo-capability-source.md`），本框架只承载条件化修复。

## 当前状态

- 代码：`PatchSet`、APU mask、版本选择、生成工具已实现；
- 数据：仅 15.8.1（Darwin 24.6.0）一份；
- 构建：release 通过；
- 测试：待安装/重启验证 selected set 日志和条件化行为。
