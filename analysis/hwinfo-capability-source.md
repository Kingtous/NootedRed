# 修正 Metal HwInfo / capability 源头（分支 `research/hwinfo-capability-source`）

日期：2026-10-18  
基线：`master` = `86c2f33`（条件化 Metal gate/setz 修复已合入）  
目标：不再用“打开 gate + 强制 allow”的补丁，而是从 `GFX9_HwInfoRec` 初始化源头修正 capability，让驱动自己计算 `dualSource || !allowRBPlus`。

## 1. 结论

可以。Metal 侧的源头是：

- `AMDRadeonX5000MTLDriver` 的 `gfx9_HwInfoInitData`（VA `0x7ffb087ef3dc`，15.8.1）；
- Vega10/Renoir 分支把 `HwInfo+0xa0` 写成 **`0x4d`**（bit1 = 0）；
- `amdMtl_HWL_InitDeviceSettings` 会把 `HwInfo+0xa0` bit1 复制到 `AMD_DeviceSettings` 的 **bit 34（`0x400000000`）**；
- 这个 bit 正是 render-pipeline 条件里的 `allowRBPlus`：
  ```asm
  test byte [HwInfo+0xa0], 0x2
  jnz  condition
  ...
  test [deviceSettings+...+8], 0x400000000
  setz al          ; !allowRBPlus
  or   al, cl      ; dualSource || !allowRBPlus
  ```
- 因此把 Renoir 分支的 `or ecx, 0x4d` 改成 **`or ecx, 0x4f`**（只置 bit1），
  就能让 gate 自然通过、`allowRBPlus` 自然为 true，普通 pipeline 保留 RB+，只有双源 pipeline 关闭 dual-quad。

**不需要再 patch `75 0B` / `0F 94 C0`。**

## 2. 具体位置

MTL 驱动 image base：`0x7ffb08757000`（同 15.8.1 shared cache）。

| 项目 | VA | 页 | 页内偏移 |
| --- | --- | --- | --- |
| `gfx9_HwInfoInitData` | `0x7ffb087ef3dc` | `0x7ffb087ef000` | `0x3dc` |
| Renoir 分支 `or ecx,0x4d` | `0x7ffb087ef53e` | `0x7ffb087ef000` | `0x53e` |
| `InitDeviceSettings` | `0x7ffb0886c85c` | — | — |
| pipeline gate `test byte [HwInfo+0xa0],2` | `0x7ffb087b3156` | `0x7ffb087b3000` | `0x156` |
| pipeline condition | `0x7ffb087b3171` | `0x7ffb087b3000` | `0x171` |

页 `0x7ffb087ef000` 中：

- `83 C9 4D` 出现两次：`0x53e`（Vega10/Renoir 分支）和 `0x5c6`（Vega20 分支）；
- `83 C9 4F` 出现一次：`0x589`（Vega12 分支）；
- 目标：page offset `0x53e`，`83 C9 4D` → `83 C9 4F`，skip=0、count=1。

注意：只改第 1 个（Renoir 走的分支）；不要改 Vega20 分支。

## 3. 验证

### 用户态进程内验证

由于当前 EFI 里是条件化 gate/setz 版本，测试时在 interposer 里：

1. 把 gate/setz 补丁还原：
   - `0x5c164`: `EB 0B` → `75 0B`
   - `0x5c198`: `31 C0 90` → `0F 94 C0`
2. 把 `gfx9_HwInfoInitData` 的 `0x53e` 改成 `0x4F`；
3. 创建 Metal 单源/双源 pipeline。

结果：

```text
dual=0 CB_COLOR_CONTROL=0x00cc0010 DISABLE_DUAL_QUAD=0
dual=1 CB_COLOR_CONTROL=0x00cc0011 DISABLE_DUAL_QUAD=1
```

无 GPU reset。这说明**只改 HwInfo 源头**就能得到与 gate/setz 相同的条件化行为，而且普通 pipeline 保留 RB+。

### 内核投递

尚未重启验证新页 `0x7ffb087ef000` 的 NootedRed 页补丁投递；但此前 `0x7ffb087b3000` 页的投递已经证明 `_cs_validate_range` 能改 shared-cache Metal 页，因此投递路径本身可行。

## 4. 代码状态

研究分支 `UserPatches.cpp` 已把原来的两个 Metal gate/setz `PagePatch` 替换为：

- 页镜像：`AMDRadeonX5000MTLDriver` 的 `0x7ffb087ef000` 页（4 KiB 原图）；
- patch：page offset `0x53e`，`83 C9 4D` → `83 C9 4F`，skip=0/count=1。

GL 的 A1/A2 不变；release 构建通过。

## 4.1 内核投递验证（2026-10-18 23:14，已重启）

安装本分支 kext 到 EFI 并重启后，直接读取已加载的 Metal 驱动代码页：

```text
gate  +0x5c164: 75 0b        # 原始指令，未被 gate patch
setz  +0x5c198: 0f 94 c0     # 原始指令，未被 allow patch
hwInfo+0x9853e: 83 c9 4f     # 源头 patch 已生效
```

说明 NootedRed 页投递成功，且当前运行的是**源头版本**，不是 gate/setz 版本。

行为与渲染：

```text
dual=0 control=00cc0010 disable=0  pixel 96,128,160,191 PASS
dual=1 control=00cc0011 disable=1  pixel 128,160,192,223 PASS
```

无 GPU reset。source-level patch 已验证可替代 gate/setz 实现。

## 4.2 回归：源头版本会导致视觉错误（2026-10-18 23:16）

用户实际使用后报告 Chromium/Electron 界面出现黑块、缩略图缺失等渲染错误（没有卡死）。  
回滚分析：

- 源头补丁把 `HwInfo+0xa0` bit1 置 1，该 bit 不只用于 pipeline 的 `DISABLE_DUAL_QUAD` 决策；
  `amdMtl_HWL_InitDeviceSettings` 会把它写入 `AMD_DeviceSettings` bit 34（`0x400000000`），
  可能同时影响其他 RB+ 行为（tiling/DCC/UBM 等）。
- 旧的 gate/setz 条件化版本只强制 pipeline condition，**没有**置 HwInfo bit1 / DeviceSettings bit34，
  因此不会启用这些额外的 RB+ 路径；用户在使用该版本时没有报告此类视觉错误。
- 结论：source-level bit1 不是“纯 capability 开关”，在 Renoir 的其余 HwInfo 参数（SE/CU/TCC/
  GB_ADDR_CONFIG）没有完整修正前，不能直接启用。

处理：立即回滚 `master` 到 gate/setz 条件化版本；source-level 研究保留在本分支，不作为生产修复。

## 5. GL 为什么不能同样从源头修

GL 的 `_glrAMD_GFX9_UpdateHwCbDualQuadMode` 与 Metal 不同：

```asm
testb $0x4, 0x30ec(ctx)
je skip
rax = ctx->0x12d8              ; GFX9_HwInfoRec
ecx = (hwInfo->0xa0 >> 1) & 1  ; 直接作为 DISABLE_DUAL_QUAD
...
```

GL 把 `HwInfo+0xa0` bit1 **直接当 DISABLE_DUAL_QUAD**，没有 Metal 那种
`dualSource || !allowRBPlus` 条件。因此：

- 把 GL HwInfo 的 `0x4d` 改成 `0x4f` 只会让它**全局关闭** dual-quad（和现有 A2 等效）；
- 不会恢复普通 GL pipeline 的 RB+；
- GL 要条件化，必须在 `UpdateHwCbDualQuadMode` 里引入 blend state 的双源判断，是另一个补丁。

## 6. 后续

1. 安装本分支 kext、重启，确认 `0x7ffb087ef000` 页投递后单源/双源控制字仍为 0/1；
2. 真实 Metal 渲染 + Chrome/Electron 稳定性复测；
3. 可选：MTL HwInfo 的 `numShaderEngines=4` 是否也要按 Renoir 改成 1（当前未改，先观察 Metal compute/scratch 行为）；
4. 若要更通用，仍需按 GPU family 做 gating（Vega10 独显不能套用）。

## 7. 与已合入条件化补丁的关系

- `master` 当前是 gate/setz 条件化版本（行为已验证）。
- 本分支是更“源头”的等价实现：一个 3 字节 patch 替代两个 gate/setz patch。
- 两者行为目标一致：单源 `DISABLE=0`、双源 `DISABLE=1`、普通 pipeline 保留 RB+。
- 建议在重启验证通过后，用本分支替代 `master` 的 gate/setz 实现。
