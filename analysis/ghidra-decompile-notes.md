# MTL 驱动反编译记录（Ghidra）

日期：2026-10-18  
分支：`research/hwinfo-capability-source`  
对象：`AMDRadeonX5000MTLDriver`（15.8.1，shared cache image base `0x7ffb08757000`）

## 工具

- Ghidra 12.1.4（本机构建了 macOS x86_64 native decompiler）；
- 从 `dyld_shared_cache_x86_64h.03` 提取 `0x7ffb08757000` 起 1.4 MB 作为 raw binary；
- 反编译函数：
  - `gfx9_HwInfoInitData`（`0x7ffb087ef3dc`）
  - `amdMtl_HWL_InitDeviceSettings`（`0x7ffb0886c85c`）
  - `MtlRenderPipelineState` 初始化（`0x7ffb087b1fa0`）
  - UBM/AddrLib 相关函数（`0x7ffb087f68ee`、`0x7ffb088b6620`、`0x7ffb08813ff0`）

## 1. `gfx9_HwInfoInitData`

反编译确认 Renoir（`uVar1 < 0x14`）走 Vega10 分支：

```c
if ((param_2 & 0x70000) != 0) {
    if (uVar1 < 0x14) {                 // Renoir
        rec->family          = 0x8d;
        rec->numShaderEngines = 4;
        rec->numShaderArrays  = 1;      // from constant
        rec->numTCC           = 0x10;
        rec->flags           |= 0x4d;   // bit1=0
        rec->chipFlags       |= 0xc1;
    } else if (uVar1 < 0x28) {          // Vega12
        rec->numShaderEngines = 4;
        rec->numTCC           = 8;
        rec->flags           |= 0x4f;   // bit1=1
        rec->chipFlags       |= 0x1c2;
    } else if (uVar1 <= 0x31) {         // Vega20
        rec->numShaderEngines = 4;
        rec->numTCC           = 0x10;
        rec->flags           |= 0x4d;
        rec->chipFlags       |= 0x1c4;
    }
}
rec->totalCU = rec->numCU * rec->numShaderEngines * rec->numShaderArrays;
```

即 Apple 没有 Renoir 专用 HwInfo，只有 Vega10/Vega12/Vega20 三个分支。

## 2. `amdMtl_HWL_InitDeviceSettings`

```c
settings->bit0  = !((h->flags >> 5) & 1);
settings->bit17 = (h->numShaderEngines >= 3);   // 4 -> 1, 1 -> 0
settings->bit34 = (h->flags >> 1) & 1;          // allowRBPlus
// ...
```

- 只改 `HwInfo+0xa0` bit1 → `settings` bit34 被置 1；
- 只改 SE → `settings` bit17 被清 0；
- 其他字段仍全部来自 Vega10 模型。

## 3. Pipeline 初始化里的 bit34

反编译 `0x7ffb087b1fa0` 找到两处关键逻辑：

```c
// 1) dual-quad 决策
if ((h->flags & 2) == 0) {
    disableDualQuad = false;          // Renoir 原始 HwInfo bit1=0
} else {
    disableDualQuad =
        (settings->bit34 == 0) ||     // !allowRBPlus
        (dualSource != 0);
}
cbColorControl->bit0 = disableDualQuad;

// 2) RB+ 专项寄存器编程
if ((settings->bit34 != 0) && (dualSource == 0) && (someSize > 0xffffffff)) {
    // 根据格式/tiling 表写 CB_COLOR_CONTROL 之外的 RB+ 寄存器
    // （puVar24[0x40..0x43]）
}
```

结论：

- `bit34` **不只是** `DISABLE_DUAL_QUAD` 的开关；
- 它同时打开一整段 RB+ 专项寄存器编程，依赖 texture format/tiling/UBM 表；
- 这些表仍按 Vega10 的 SE/TCC/GB_ADDR_CONFIG 模型生成，Renoir 没有对应模型；
- 因此只修 `allowRBPlus` / SE / `PA_SC_ENHANCE` 无法让那段代码正确，反而会让它用错误参数启用 RB+，产生黑块/缩略图缺失。

## 4. 为什么条件化版本是正确解

gate/setz 版本：

- 保持 `HwInfo+0xa0` bit1=0，`settings->bit34=0`；
- 只强制 pipeline 的 `disableDualQuad` 条件；
- **不会进入** `settings->bit34 != 0` 的 RB+ 专项编程块；
- 因此不会出现黑块，同时 dual-source pipeline 正确关闭 dual-quad。

## 5. 最终结论

反编译证实：source-level 完整 RB+ 不是“少一个字段”，而是 Apple 驱动缺少 Renoir 的整套
HwInfo/UBM/AddrLib/RB+ 模型。要继续启用完整 RB+，需要重建：

- `GFX9_HwInfoRec` 的 Renoir 拓扑（SE/SH/CU/TCC/RB）；
- UBM/AddrLib 的 tiling/DCC 参数；
- pipeline RB+ 专项寄存器表和格式表。

这是大工程，且没有 Apple 源码，风险高。生产版本应保留条件化 gate/setz 实现。
