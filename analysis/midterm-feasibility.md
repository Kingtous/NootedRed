# 中期计划可行性研究（分支 `research/midterm-feasibility`）

日期：2026-10-18  
基线：`master` = `09a4026`（A1+A2+A3 修复已提交）  
本文件只做静态/离线研究，没有修改生产代码，也没有再做 GPU 压力测试。

目标：评估“性能更好、通用性更强”的根因修复路线是否可行，而不是继续叠加全局强制补丁。

---

## 1. 先纠正一个之前的关键假设

之前 `kernel-leverage` 报告提到“Linux Renoir 使用 `0xF3E777FF | 0x22010042`，NootedRed 用的是 Raven 的 `0x24000042`”。  
实际核对 Linux `gfx_v9_0.c` 后：

| 表 | GB_ADDR_CONFIG | mask |
| --- | --- | --- |
| `golden_settings_gc_9_0_vg20`（Vega20） | `0x22014042` | `0xF3E777FF` |
| `golden_settings_gc_9_1_rn`（Renoir） | **`0x24000042`** | **`0xF3E777FF`** |
| NootedRed `gcGoldenSettingsRenoir` | **`0x24000042`** | `0xFFFF77FF` |

结论：

- **Renoir 的值本来就是 `0x24000042`，NootedRed 的值没有错**；`0x22010042/0x22014042` 是 Vega20 的值。
- 唯一差异是 mask：
  - NootedRed `0xFFFF77FF` 会把 `NUM_SHADER_ENGINES`（bits 19–20）和 `NUM_RB_PER_SE`（bits 26–27）按 Raven 值显式写入；
  - Linux Renoir 的 `0xF3E777FF` 会**保留这两个字段的硬件值**。
- 这两个字段与 `SHADER_ENGINE_TILE_SIZE`/dual-source blend 没有直接关系。
- 因此“把 `GB_ADDR_CONFIG` 改成 `0x22010042` 来修 #158” **依据不成立**，风险高、收益低，暂不采用。

**可行性评级：低。** 如果还要碰，只能是“mask 审计实验”，不是 #158 的主修复。

---

## 2. Renoir 的 RB+ 其实是可用的

Mesa `src/amd/common/ac_gpu_info.c`：

```c
info->has_rbplus = info->family == CHIP_STONEY || info->gfx_level >= GFX9;
info->rbplus_allowed =
   info->has_rbplus &&
   (info->family == CHIP_STONEY || info->family == CHIP_VEGA12 ||
    info->family == CHIP_RAVEN || info->family == CHIP_RAVEN2 ||
    info->family == CHIP_RENOIR || info->gfx_level >= GFX10_3);
```

即 **Renoir 的 RB+ 是 allowed**。这解释了为什么当前 A3 的“全局 `DISABLE_DUAL_QUAD=1`”是**过度保守**的：

- 正确逻辑应是 `disable_dual_quad = dual_source || !allowRBPlus`；
- Renoir `allowRBPlus=true`，所以普通混合可以保留 RB+，只有双源混合关闭；
- 当前补丁把所有 pipeline 的 RB+ 都关了，性能上确实有优化空间。

---

## 3. Metal 条件化：有明确切入点，但还缺一个 capability 输入

反汇编 `AMDRadeonX5000MTLDriver` 的
`-[GFX9_MtlRenderPipelineState initWithDevice:pipelineStateDescriptor:vertexVariant:fragmentVariant:]`
（15.8.1 VA `0x7ffb087b1fa0`，文件相对 cache 偏移 `0x5afa0`）可以看到：

```asm
; VA 0x7ffb087b3156
mov  rax, [rbp-0x1d8]          ; HwInfo / settings 指针
test byte [rax+0xa0], 0x2      ; allowRBPlus / capability gate
jnz  0x7ffb087b3171            ; 只有 bit1=1 才计算 condition
xor  eax, eax
jmp  0x7ffb087b319d

; VA 0x7ffb087b3171：condition 计算
cmp  dword [rbp-0x190], 0
setnz cl                        ; 某个 dual-source / 状态标志
mov  rax, [rbp-0x1c0]
mov  rdx, 0x400000000
test [r11+rax+8], rdx           ; 另一个 capability / descriptor 标志
setz al
or   al, cl                     ; 目标 = flagA || !flagB
...
; VA 0x7ffb087b31bc
mov  ecx, [r14+0x28]            ; CB_COLOR_CONTROL
and  ecx, ~1
or   ecx, eax                   ; DISABLE_DUAL_QUAD
mov  [r14+0x28], ecx
```

当前 Renoir 的 `HwInfo+0xa0` 是 `0x4d`，bit1=0，所以直接跳过 condition，`eax=0` → 不关闭 dual-quad → 双源混合卡死。  
这正好对应社区 fork 所说的“缺少 capability report 时跳过计算”。

### 可行性

- **切入点明确**：`0x7ffb087b3164` 的 `75 0B`（`jnz`）可以改成 `EB 0B`（`jmp`），强制计算 condition。
- 但只改这一个字节还不够：在 A3 全局补丁已存在的进程里做对照实验时，单源/双源都仍然是 `DISABLE=1`。原因是 condition 里的第二项 `[r11+rax+8] & 0x400000000` 在 Renoir 上可能表示 `!allowRBPlus`，如果它也是 false，则所有 pipeline 仍然关闭。
- 因此需要先找到并正确设置这个 capability 输入（很可能是 Metal 的 `GFX9_HwInfoRec`/device settings 中的 RB+ 字段），再让 gate 正常计算；不能只把 gate 强制跳过去。
- 可以继续用“进程内 patch + 读 `m_members+0x28`”的方式做低成本 A/B，不需要改 NootedRed 或重启。

**可行性评级：中高**（方向明确，需要补足 capability 字段映射和双源/单源验证）。

---

## 4. GL 条件化：比 Metal 难

`_glrAMD_GFX9_UpdateHwCbDualQuadMode`（GL 驱动 0x8134）实际逻辑是：

```asm
testb $0x4, 0x30ec(ctx)
je    skip
rax = ctx->0x12d8                  ; GFX9_HwInfoRec
ecx = (hwInfo->0xa0 >> 1) & 1      ; 直接作为 DISABLE_DUAL_QUAD
...
or    eax, ecx
store CB_COLOR_CONTROL
```

也就是说 GL 驱动没有像 Metal 那样的 `dualSource || !allowRBPlus` condition；`rec+0xa0` bit1 本身就直接决定是否全局关闭。  
要让它“只在双源时关闭”，需要在 `UpdateHwCbDualQuadMode`/blend state 路径里引入双源判断，属于额外 RE/代码 patch，不是简单改一个 flag。

**可行性评级：中低。** 而且 Chrome/Electron 实际走 Metal，GL 条件化的优先级低于 Metal。

---

## 5. 内核侧根因修复：仍然不可行

- `gfx9_HwInfoInitData` 的 family 0x8D、SE/CU preset、`rec+0xa0` flag 都是**用户态字面量**。
- 内核只能通过 `AMDAccelDevice::getHardwareInfo` 提供 `arg2`（`AMDHWInfo` 0x204 字节）；无法改变用户态硬编码的 `rec+0xa0`/family。
- 因此“仅靠内核补丁让驱动自己走正确路径”不成立；根修复必须在用户态代码页。

**可行性评级：低（作为 #158 的完整修复）；仅可作为辅助的 HWInfo 输入修正。**

---

## 6. 通用性：当前页镜像方案是版本锁定的

- 当前 `UserPatches` 用完整 4 KiB 页镜像匹配，只对 15.8.1/GL 7.0.25/当前 shared cache 有效。
- 更通用的方案：
  - 用模块路径 + 函数符号（或 ObjC 方法）+ 带 mask 的指令模式定位；
  - 对 shared cache 使用 COW 页 patch（进程私有，不写磁盘）；
  - 每个 macOS 版本维护一个 resolver/偏移表，并校验完整函数哈希/引用；
  - 对 GL/Metal 分别适配。
- 这能把“换 macOS 小版本就失效”变成“按版本适配”，但仍不能一套字节通吃所有版本；实现量明显大于当前方案。

**可行性评级：中（工程量大，但方向成熟）；建议作为后续独立分支。**

---

## 7. 推荐的中期方案（按优先级）

1. **Metal 条件化（优先）**
   - 找到 Metal `GFX9_HwInfoRec`/device settings 中对应 `allowRBPlus` 的字段，并在用户态把它设为 true；
   - 把 0x7b3164 的 `75 0B`→`EB 0B` 作为“gate 打开”实验；
   - 读单源/双源 `m_members+0x28`，目标：单源 `DISABLE=0`，双源 `DISABLE=1`；
   - 用真实 Metal 渲染 + CPU 对比验证，再做性能 A/B。
2. **性能 A/B 基线**
   - 当前全局 A3 vs 条件化 A3'，用 Metal 渲染/合成 benchmark 与浏览器合成负载对比；在没有数据前不要假设条件化一定更快（Renoir 的 RB+ 可能仍有其他限制）。
3. **GL 条件化（次要）**
   - 如果确认 Chrome 在某些机器上仍走 GL，再研究 `UpdateHwCbDualQuadMode` 的双源判断；否则不做。
4. **GB_ADDR_CONFIG 仅审计**
   - 不修改值；最多实验 Linux Renoir 的 mask `0xF3E777FF`，确认硬件字段是否被错误覆盖；与 #158 关系弱，风险高。
5. **通用 resolver（后续）**
   - 把页镜像匹配升级为符号/模式 resolver，支持多 macOS 版本。

---

## 8. 本轮已确认/未确认

已确认：

- Renoir 的 GB_ADDR_CONFIG 值 `0x24000042` 与 Linux Renoir 一致；之前“应改 0x22010042”的说法错误。
- Renoir 的 RB+ 在 Mesa 中是 allowed；当前全局关闭是保守策略，理论上可条件化。
- Metal gate 在 0x7ffb087b3164（`75 0B`），condition 在 0x7ffb087b3171–0x7ffb087b31c5。
- GL 的 `rec+0xa0` bit1 直接作为 DISABLE_DUAL_QUAD，不存在现成的“只在双源关闭”逻辑。

未确认/下一步：

- Metal condition 里 `[r11+rax+8] & 0x400000000` 的具体含义和来源；
- 如何把 `allowRBPlus` 正确设为 true 而不破坏其他 pipeline 状态；
- 条件化后的真实性能收益；
- 跨版本 resolver 的工程量与误匹配风险。

---

## 9. 结论

“性能更好、通用性更强”的方向是存在的，但**不是简单再改几字节**：

- **性能最好**：Metal 条件化（修 capability 输入 + 打开 gate），保留 Renoir RB+，只对双源关闭；
- **通用性最好**：符号/模式 resolver + 版本表，而不是整页镜像；
- **GB_ADDR_CONFIG 根因修复**：依据不足，暂不采用；
- **内核单独修复**：不可行，必须用户态页补丁。

本分支目前只做可行性研究，没有把任何实验性改动合入生产修复。
