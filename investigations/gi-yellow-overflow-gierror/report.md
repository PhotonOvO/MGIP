# GI 黄色高亮区域“数值溢出色”调查 — gierror.rdc

## Symptom (as reported)
- Capture: `C:\Users\happyelements\Downloads\gierror.rdc` (826 MB), Vulkan
- 报告现象：黄色高亮的选择区域内，GI 颜色呈现“类似数值溢出”的颜色。
- 期望：GI 在该区域应显示正常的间接光照颜色。

## Environment repair (prerequisite, verified)
`rdc_open` 最初失败：`Unable to start rdc: spawn rdc ENOENT`。

原因：typed 工具通过 `src/rdc/process.ts` 的 `createRdcProcessRunner()` 以
`spawn("rdc", ...)` 启动 helper；本机 helper 实际是 Windows 二进制 `rdc.exe`
（PE32+），而 Node 在 WSL/linux 下不做 PATHEXT 解析，故 `spawn("rdc")` 必然
ENOENT。证据：`spawnSync("rdc")` → `ERROR ENOENT`，`spawnSync("rdc.exe")` →
`rdc, version 0.6.1`。

处置：
1. `src/rdc/process.ts` — 新增 `resolveRdcExecutable()`，对默认名 `rdc` 依次
   尝试 `rdc`/`rdc.exe`/`rdc.cmd`/`rdc.bat` 并按 PATH 解析（`tsc` 对该文件
   0 错误）。
2. `/home/photon/.local/bin/rdc` — WSL 启动垫片：`exec rdc.exe`，并用
   `wslpath -w` 把 `/mnt/...` 参数转成 Windows 路径（helper 是 Windows 进程，
   无法解析 WSL 挂载路径）。垫片只是让 typed 工具可达，不绕过它。

修复后 `rdc_open` 成功（见下）。注意：本会话已加载的扩展仍是旧代码，
`process.ts` 的改动需 pi 重载后才生效；垫片让当前会话立即可用。

## Frame facts (evidence)
`rdc_open` → API **Vulkan**, events **1472**, drawCalls **1228**
(1222 indexed / 6 non-indexed / **77 dispatches**), clears 7, copies 6。

Pass 拓扑（`rdc_frame_overview`）：

| Pass | EID | marker | target(s) |
|---|---|---|---|
| #1 Depth | 39–1616 | GPUDrivenDepthPrepass | D32 (2510) |
| compute | 1634–2142 | GPUDrivenDepthPyramid | 77 dispatches, 无 color target |
| #2 | 2144–3721 | GPUDrivenGBufferPass | 2457 RGBA8_UNORM, 2461 RGBA8_UNORM, **2465 RGBA16F** + D 2510 |
| #3 Clear | 3754–3760 | **GPUDrivenLightPass** | **2472 R16G16B16A16_FLOAT** |
| #4 | 3782–5360 | GPUDrivenEditorOccurrence | **2578 R32_UINT** + D 2581 |
| #5 Clear | 5377–5383 | GPUDrivenVelocity | 2495 R16G16_FLOAT |
| #6/#7 | 5417–5438 | GPUDrivenEditorHighlight | 2469 B8G8R8A8_UNORM |
| #8 | 5448–5526 | EditorOverlay | 2469 + D 2569 |
| #9 | 5549–5644 | GPUDrivenImgui | — |
| present | 5539 | GPUDrivenPresent | `vkCmdBlitImage` → **2432**（最终显示） |

## Evidence collected

**E1** — `rdc_pixel_history` 最终显示目标 2432 @EID 5644：
- (550,530) → `(0.5804, 0.5647, 0.5333, 1)`，来自 EID 5539 `directShaderWrite`（blit）
- (550,700) → `(0.5490, 0.5333, 0.5020, 1)`；(400,500) → `(0.5961, 0.5922, 0.5647, 1)`
- (560,150) / (620,420) / (700,450) / (700,600) / (800,100) / (874,229) / (900,900) / (505,31) → `(0,0,0,1)`

均为**正常灰色石材色或黑色**。未在任何采样点观察到黄色，也未观察到
任何通道饱和到 1.0 的“溢出色”。

**E2** — 高亮 shader 在所有采样点均 `discard`：
`rdc_pixel_history` @EID 5438 / 5526（target 2469）在 (550,530)、(300,700)、
(450,400)、(650,650) 全部返回 `passed:false, flags:["shaderDiscarded"]`，
`post_mod` 保持清屏黑。即这些像素**没有**被黄色描边覆盖。

**E3** — GI/光照 HDR 目标在采样点为 0：
target **2472**（`GPUDrivenLightPass`，R16G16B16A16_FLOAT）@EID 3760 在
(550,530)、(400,300)、(560,150) 的两次修改（3754 clear、3760 draw）
`shader_out` 与 `post_mod` 的 rgb 全为 0（alpha=1）。
GBuffer **2465**（RGBA16F）@EID 3721 target=2 在 (550,530)、(560,150) 同样全 0。

**E4** — Pass #4 的 target 是 **R32_UINT** occurrence token，不是颜色：
pixel history 报出的 `2.6624670822171524e-44` / `5.4650640108667866e-43` /
`9.528829557408756e-44` 是整数 token 被按 float 解释的结果，换算后分别为
**19 / 390 / 68 / 392**。**这不是数值溢出**，是工具的格式误读，必须排除。

**E5** — 黄色来源在源码中确定：
`render/RenderTypes.h:384`
```cpp
struct EditorHighlightShaderPushConstants {
  std::array<uint32_t,4> viewport{};
  std::array<uint32_t,4> counts{};
  std::array<float,4> color{1.0f, 0.72f, 0.08f, 1.0f};   // 琥珀/黄
};
```
`shaders/editor_highlight.slang` 的 `editorHighlightFragmentMain` 末尾
`return editorHighlight.color;`，即用户看到的“黄色高亮”就是该描边色，
与 GI 数值无关。

**E6** — GI debug overlay 在本次 capture 中**未启用**：
overlay 区域按 `render/RenderDevice.cpp` 的 tile 布局计算为
x∈[504,1095], y∈[8,301]（tile=144, 4列2行）。E1 中 (560,150)、(800,100)、
(874,229)、(505,31) 落在该区域内且最终为黑，符合
`drawFlaxDebugViewportOverlay()` 在 `!flaxGIDebugOverlayEnabled` 时提前返回。
故本 capture 内**没有 GI 调试图块可供观察溢出色**。

## Hard tool limitations (blocked deeper analysis)

**L1 — `shaders.compute` 是陈旧/粘滞值，不可用于识别 GI dispatch。**
同一 `computePipeline: 683` / `shader 681` / `transparentVisibilityPatchMain`
在 1634、2142、2144、3754、3782、5377、5438、5526、5539 **全部**出现；
其中 3754/5377 是纯 clear、5539 是 `vkCmdBlitImage`，这些事件不可能有
compute shader。只有 EID 1634（真 dispatch）报出不同的 `depthPyramid`(566)。
结论：除 1634 外，任何 `shaders.compute` 字段都不能作为证据。

**L2 — shader 源码与常量完全不可读。**
所有 `rdc_inspect_draw` 的 `warnings` 中，
`shader.{vs,ps,cs}.{source,constants}` 一律返回
`not found: \\wsl.localhost\Ubuntu-24.04\draws\<eid>\shader\<stage>\{source,constants}`，
`constants` 字段始终为 `{}`。因此无法从 capture 读取 GI push constants、
`lighting` cbuffer(480)、`clusteredLight`(482) 或任何 GI 着色器源码/常量。

**L3 — 77 个 dispatch 无法枚举。**
`rdc_frame_overview` 的 `draws` 只返回 DrawIndexed 类事件，`filter="dispatch"`
/`"vkCmdDispatch"` 均返回 `returnedDraws: 0`；逐个探测 1634–2142 区间的 EID
（1640/1646/1660/1700/1800）返回自相矛盾的
`out of range (max: 5653)`（1634 与 2142 却可查）。故 GI 各 dispatch 的
EID、entry name、绑定与线程组维度均无法获得。

## Hypotheses

**H1 — 用户看到的黄色是编辑器选择描边色，而非 GI 数值。**
- Claim: 黄色来自 `EditorHighlightShaderPushConstants::color = {1.0,0.72,0.08}`
  经 `editorHighlightFragmentMain` 直接输出。
- Evidence: E5, E2, E4
- Expected: 黄色像素只出现在 selected/unselected token 的 1–4px 边界带内，
  且其值恒为 (1.0,0.72,0.08)，与 GI 辐照度无关。
- Verification: 在描边带上取 `rdc_pixel_history` @5438，应 `passed:true`
  且 `shader_out == (1.0,0.72,0.08,1)`。
- Confidence: medium  Status: **proposed（未在本次 capture 命中描边像素）**

**H2 — GI 辐照度被 RGBA8_SNORM 截断导致溢出色。**
- Claim: `probesIrradianceOut[texel] = float4(irradiance, attention)` 写入
  SNORM 图像，超出 [-1,1] 被钳制/回绕产生异常色。
- Evidence: 反证 —— `render/FlaxDDGIResources.cpp:44-45` 显示 SNORM
  (`probeDataFormat`) **仅**用于 `FlaxDDGI.ProbesData`；
  `ProbesIrradiance`/`ProbesIrradianceHistory` 为 `rgba16Sfloat`，
  `ProbesTrace` 为 `rgba16Sfloat`，`ProbesDistance` 为 `rg16Sfloat`。
  `shaders/ddgi_flax_update_irradiance.slang:77-79` 写入的是 HDR 目标。
- Confidence: high  Status: **rejected**

**H3 — 本 capture 未复现所报症状。**
- Claim: gierror.rdc 中既无黄色高亮像素，也无饱和/溢出色；GI HDR 目标与
  GI debug overlay 在采样点均为 0/未启用。
- Evidence: E1, E2, E3, E6
- Confidence: medium  Status: **proposed**
- 说明：采样为逐点探测（约 12 个点），描边是 1–4px 细带，随机采样极易漏掉；
  且受 L2/L3 限制无法读取 GI 内部数值。因此这是“未找到证据”，
  不等于“证据表明不存在”。

## Source-side observations (not proof of runtime state)
- `shaders/ddgi_flax_trace_rays.slang:295-296` 写
  `probesTrace = float4(radiance, storedDistance)`，radiance 无上限钳制；
  环境光路径 `:204-207` 用 `environmentParams.y` 缩放，命中路径 `:288`
  `radiance = albedoSample * directRadiance ...`，并叠加
  `bouncedIrradiance * kFlaxDDGIMultiBounceFeedback`(`:287`)。
  多次弹射反馈若无能量上限，理论上可产生远超 1.0 的 HDR 值 —— 这是
  “溢出色”最可能的物理来源，但**本次 capture 无法验证**（L2/L3）。
- `shaders/ddgi_flax_debug_views.slang:35-59` 的 `toneMapDebug()`
  (`value/(value+1)`) 与 `boostDebugChroma()`（`chromaBoost` 钳到 [1,2]，
  并按 luminance 限制 boost）本身是**收敛**的，不会单独制造溢出色；
  但该 atlas 在本次 capture 未启用（E6）。

## Result
**未能确认根因。** 本次 capture 中没有采到黄色高亮像素，也没有采到任何
饱和/溢出色；GI HDR 目标与 GI debug overlay 在采样点均为 0/未启用（E1–E3, E6）。
同时 L1（compute shader 字段陈旧）、L2（shader 源码/常量全部 not found）、
L3（77 个 dispatch 不可枚举）使得无法读取任何 GI dispatch 的着色器、常量或
中间数值。

按 skill 规则：未获得支持性证据，不提出确定性根因；H2 已被反证拒绝，
H1/H3 仍为 proposed。

## 需要的后续输入（任一即可推进）
1. **黄色区域的一个确切像素坐标**（在 RenderDoc 里对着黄色描边取 x,y），
   以便 `rdc_pixel_history` 精确命中并读取 `shader_out`。
2. 复现时**开启 FlaxGI Debug Overlay**（`Overlay On Main Viewport`，
   见 `app/MinimalLatestApp.h:8118`），并指定看哪一个 view
   （`probeIrradiance` / `traceRadiance` / `surfaceAtlasLighting`），
   重新抓一帧 —— 这样 GI 数值才会出现在可采样的 atlas 里（当前 E6 为关）。
3. 若需要 GI dispatch 级证据，需要一个**带 RenderDoc 可解析 shader 调试信息**
   的构建（当前 capture 的 `shader/*/source` 与 `shader/*/constants` 全部
   `not found`，见 L2）。

## Session state
已按 skill 规则调用 `rdc_close`（返回 `{"status":"closed"}`），GPU 资源已释放。
拿到上述任一后续输入后需重新 `rdc_open` 同一 capture 继续探测。

---

# 补充（第二轮）：typed 工具集扩展后取得的决定性证据

## 工具集扩展（用户批准后实施）
按 `TASK_PLAN_PI_RENDERDOC_V0_1.md:86` 的方向，在 `src/rdc/` 内适配 CLI，
未改动 shell 拦截规则（`tool-policy` 测试 3/3 仍全绿）。新增 6 个 typed 工具：

| 工具 | CLI 映射 | 解决的旧限制 |
|---|---|---|
| `rdc_events` | `events --type/--filter/--range/--limit` | L3：可枚举 77 个 dispatch |
| `rdc_shader` | `shader <eid> <stage> --constants/--source/--target` | L1/L2 |
| `rdc_constants` | `cbuffer <eid> --stage/--set/--binding` | L2 |
| `rdc_texture_stats` | `tex-stats <id> [eid] --histogram` | 定量溢出判定 |
| `rdc_pick_pixel` | `pick-pixel x y [eid] --target` | 读光栅目标终值 |
| `rdc_export_texture` | `texture <id> -o <png>` | 读 compute 输出像素 |

**L2 根因修复**：`client.getShaderConstants/getShaderSource` 原先走 VFS
`cat /draws/<eid>/shader/<stage>/constants`，该路径在 rdc-cli 0.6.1 不存在，
故一律 not found。改为 `shader --constants/--source`。
`tests/pi-renderdoc/client.test.ts` 中锁定旧路径的断言已同步更新。

验证：`tsc --noEmit` 0 错误（含修复既有的 `renderdoc.ts` expected 元组类型错误）；
`node --test` 21 项中 19 过，2 项为**既有** Linux 环境 bug
（`client.test.ts:220/270` 硬编码 `\\` 分隔符致 `mkdtemp('/tmp\\...')` EACCES，
与本次改动无关，只触及未改的 `captureExecutable`）。
新工具经真实 capture 逐一验证（见下）。
注：扩展需 pi 重启才会注册新工具；本轮验证直接调用同一 typed client 库
（`src/rdc/client.ts`，即工具的实现路径），未绕过 shell 拦截规则。

## 新证据

**E7 — dispatch 全表（`rdc_events --type Dispatch --range 1600:5653`）**：77 个
dispatch 全部定位，GI 相关：
`DDGI.Classify 1957,1963 / DDGI.Compact 1971,1976 / DDGI.InitArgs 1983,1988 /
DDGI.UpdateInactive 1994 / DDGI.TraceRays 2001,2023 / DDGI.UpdateDistance 2009,2031 /
DDGI.UpdateIrradiance 2016,2038 / DDGI.DebugViews 2045`。
（L3 解除。）

**E8 — 每个 dispatch 的真实 compute entry（`rdc_shader <eid> cs`）**：
2001/2023=`CS_TraceRays`，2016/2038=`CS_UpdateIrradiance`，
2045=`CS_FlaxGIDebugViews`。（L1 解除：不再是粘滞的
`transparentVisibilityPatchMain`。）

**E9 — GI 数据定量（`rdc_texture_stats`）**：
- `probesTrace` 2225 (R16G16B16A16_FLOAT) @2023：max = **12.71/13.36/14.34, a=38.97**
  （fp16 上限 65504，未溢出，但 radiance 偏高）
- `probesIrradiance` 2228 @2038：max = 1.52/1.48/1.52；直方图 bucket0 占
  1,398,056 / ~2.2M ≈ **63% texel 辐照度为 0**
- `probesData` 2227 (R8G8B8A8_SNORM) @2038：rgb ∈ [-0.449, 0.449]，a ∈ [-1,1]
  → **在 SNORM 范围内，无钳位**（再次反证 H2）

**E10 — GI debug atlas 导出（`rdc_export_texture 2263`）+ 最终画面导出（2432）**：
`investigations/gi-yellow-overflow-gierror/ddgi-debug-atlas-2263.png`、
`final-display-2432.png`、`editor-overlay-2469.png`。

最终画面显示：
1. **琥珀色描边**（= `RenderTypes.h:384` 的 `{1.0,0.72,0.08}`）勾勒左侧/底部
   被选中的墙体 primitive —— 即用户所说"黄色高亮选择区域"。**H1 确认**：
   黄色是编辑器选择描边色，不是 GI 数值。
2. 描边区域之外的 GI 光照呈**饱和单通道色斑**：中部暗绿噪斑、右侧暗红/橄榄
   （红+绿）斑 —— 即"类似数值溢出"的观感来源。
3. atlas tile 4 `Trace Hit Distance` = **纯绿 (0,1,0.05)** → 所有光线
   `normalizedDistance≈0`（存储命中距离≈0）；
   tile 5 `Probe Distance Moments` = **纯品红 (1,0,1)** → mean≈1、variance≈0、
   second≈1（距离矩饱和在上限）。**两者互相矛盾 → 距离数据退化/不自洽。**
4. atlas tile 7 `Surface Atlas Lighting` = **紫/黑斜纹** = shader 中
   `debugPush.surfaceAtlasAvailable == 0` 的 fallback 分支
   （`ddgi_flax_debug_views.slang`：`stripe ? (0.45,0,0.45) : (0.08,0,0.08)`）
   → 本帧 surface atlas lighting 对 debug view 不可用。
5. atlas tile 6 `Probe Irradiance` 几乎全黑，与 E9 的 63% 零辐照度一致。

**E11 — debug push constants（`pipeline 2045 push-constants`）**：
`sdfSlice=0.5, exposure=1, chromaBoost=1` → debug tile 的饱和**不是**可视化
增益（exposure/chromaBoost 均为 1）造成的，是原始数据本身。

## 结论修正
- 旧 E6（"overlay 未启用"）**作废**：overlay 实际启用（E10 可见 8 个 tile）。
  旧像素采样落在 ImGui 合成前的坐标/目标上，故漏掉 atlas。
- **H1 确认**（黄色 = 选择描边色）。
- **H2 维持 rejected**（E9：SNORM 数据在范围内）。
- 新结论（proposed，高置信方向）：画面中"溢出色"观感来自 **GI probe 数据退化**
  —— trace 命中距离≈0 与距离矩≈上限互相矛盾（E10-3/4）、辐照度 63% 为 0（E9/E10-5）、
  surface atlas lighting 不可用（E10-4）；光照 pass 消费这些退化数据后输出
  饱和单通道色。非 tonemap、非格式钳位、非 debug 可视化增益（E11）。
- 尚未定位：光照 shader 中把退化距离/辐照度变成绿/红单通道色的具体采样路径。
  下一步可用 `rdc_shader <light-pass-eid> ps --constants/--source` 或重建带
  调试信息的构建读取 forward/light shader 的采样与解码逻辑。

---

# 追加调查：色斑的根因（per-channel 1/32 量化）与黑斑（量化硬截断）

本次使用**新捕获**（`gierror.rdc` 2026-09-23 12:12，833 MB；此前同路径文件已被覆盖）
及后续验证捕获。所有结论均标注证据 ID 与工具来源。

## E12 根因：`fragmentHdrMain` 的逐通道 1/32 量化（色斑）

`shaders/shader.light.slang` 光照 pass 末尾（原实现）：

```hlsl
const float hdrStabilityLevels = 32.0;
const vec3 stableHdrColor =
    floor(hdrColor * hdrStabilityLevels + 0.375) / hdrStabilityLevels;
```

- 该式对 **R/G/B 各自独立** 做 `floor(x*32+0.375)/32`。
- 暗弱 GI（各通道 0.01–0.04）落进各自的第一步：`(0.032,0.011,0.010) -> (1/32,0,0)` 纯红；
  `(0.010,0.032,0.009) -> (0,1/32,0)` 纯绿；两通道同时过阈值 -> 橄榄色。相邻像素逐通道
  跨阈值 -> 抖动状色斑。**与溢出无关**（fp16 上限 65504，实测 max 4.69/9.88）。
- 量化证据：旧捕获 2472 直方图中所有非零桶**精确对齐 k/32**
  （13/32→bucket22、16/32→bucket27、21/32→bucket35 … 无例外），bucket1 `r=182139, g=0, b=0`
  —— 严格单通道特征。
- 工具：`rdc_texture_stats`(eid=3760, resourceId=2472, histogram)，`rdc_pick_pixel`。

## E13 第一版修复（亮度量化）引入了"黑斑"

补丁：改为对亮度量化 `stableLum = floor(lum*32+0.375)/32; color *= stableLum/lum`。

- 消除了逐通道饱和（12:12 捕获 bucket1 变为 `r=305775, g=52725, b=41899`，g/b 不再恒 0）。
- 但引入**硬截断**：`lum < 1/32=0.0156` 的像素被整体归零 → 用户报告的"黑斑"。
- 该像素 (1065,548) 在 2472 上为 `(0,0,0,1)`，即 tonemap 之前就是纯黑。

## E14 黑斑像素的完整证据链（用户新捕获 gierror.rdc）

坐标：窗口 (1065,548) → 视口局部 (682,529)（视口 1103×1061 @ 偏移 (383,19)）。

| 环节 | EID / 资源 | 值 |
|---|---|---|
| 最终交换链 | 2434 (B8G8R8A8) `rdc_pick_pixel` | (0,0,0,1) |
| ImGui 全屏绘制 | EID 5557 `uiTexture`=2469 | 写入黑；shader_out (0,0,0,1) |
| 视口合成目标 | 2469 局部 (682,529) | (0,0,0,1) |
| 最终调色 | EID 5423 `fragmentFinalColorMain` 读 2472 | — |
| **HDR 场景色** | EID 3760 → 2472 | **(0,0,0,1)** |

GBuffer（EID 3721，`rdc_pick_pixel` target 0/1 + `rdc_pixel_history`）：

- basecolor `(0.369,0.314,0.220, a=0.655)` → 反照率/粗糙度正常（非黑斑来源）
- packed normal `(0.486,0.165,0.498, ao=1)` → `decodeNormalOct` → `N≈(-0.04,-0.91,0.41)`（朝下拱顶）
- metallic 0.498、ao 1.0、depth 0.018（有效，约 5.6 m）→ **不是背景分支，几何/深度正常**

光照常量（EID 3760，`rdc_constants` set=1 binding=2）：

- `lightColorAndNormalBias=(50,50,50,0.0025)`；`lightDirectionAndShadowStrength=(0,0.985,-0.174,1)`
  → `N·L ≈ -0.97 < 0`，`evaluateDirectBRDF` 末尾 `* NdotL` → 直射光为 0
- `ambientColorAndTexelSize=(0,0,0)` → 环境项为 0
- `iblParams=(0,1,7,1)` → `useIBL=false`（x=0），IBL 分支整体跳过；
  且 `evaluateEnvironmentBackground` 在 `!useIBL` 时**直接返回 (0,0,0)**
- `ddgiFlaxFallbackIrradiance=(0,0,0)`、`ddgiFlaxRadianceSourceParams.x=0`（无自发光源）

结论：该像素的总辐射 = 直射 0 + 环境 0 + IBL 关闭 + GI≈0 → 恰好为 0。

## E15 整帧亮度与 GI 覆盖

- 2472 直方图（用户帧）：`r` 桶0 = 770813/1170283 (65.9%)、`g` 91.8%、`b` 93.2% 为 0
  → 全帧基本无间接光，仅少量太阳直照区（max 9.88）。
- 探针辐照度 2228：max 1.5186（gamma=5 编码），**59.7% 恰为 0**；"Probe Irradiance" 调试图谱
  几乎全黑（`rdc_export_texture` 2263）。
- 探针追踪 2225：**98.6% 辐射为 0**（多数光线命中无光表面，miss 回退 `fallbackIrradiance/PI=0`）。
- 代码依据：`shaders/ddgi_flax_trace_rays.slang:204` miss 分支；
  `render/passes/FlaxDDGIPass.cpp` 每帧经 `buildFlaxDDGIData` 上传。
- app 侧：`app/MinimalLatestApp.h:262` 启动时**硬编码 `m_debugOptions.enableIBL = false`**，
  `lightSettings.ambient` 也为 0 → 本 build 默认只有"太阳直射 + GI"，间接光一旦缺失即为纯黑。

## E16 修复：log2 相对量化（已验证）

`shaders/shader.light.slang` `fragmentHdrMain` 末尾改为：

```hlsl
const float hdrStabilityLevels = 32.0;
const vec3 quantizableHdrColor = max(hdrColor, vec3(5.9604645e-8)); // 2^-24
const vec3 stableHdrColor = exp2(
    floor(log2(quantizableHdrColor) * hdrStabilityLevels + 0.375) /
    hdrStabilityLevels);
```

- 步长变为**相对**（2^(1/32)≈2.2%）：暗弱 GI 保留能量与色相，不再被归零或饱和成单通道；
  仍保留"量化以求时间稳定"的原始意图。
- 编译验证：`slangc shaders/shader.light.slang -target spirv -entry fragmentHdrMain` 退出码 0。

### 验证捕获（工具 `rdc_capture`，frame 150，`capture-log2-f150_frame150.rdc`）

| 指标（2472 @ light pass EID 3758） | 用户帧 12:12（亮度量化） | 验证帧（log2 量化） |
|---|---|---|
| min | 0 | **5.96e-8 (2^-24)**，无精确 0 |
| bucket0 `r/g/b` | 770813 / 1074567 / 1090798（65.9/91.8/93.2%） | 555030 / 582073 / 694698（47.4/49.7/59.4%） |
| 分布形态 | 集中于前几桶 | 1–63 桶连续平滑，**无 k/32 梳状对齐** |
| max | 9.8828125 | 9.5078125 |

抽样 texel (250,200) = `(0.0405, 0.0306, 0.0182)`：暗弱但有值、色相连续（R>G>B），
不再是 `(1/32,0,0)` 也不再是 `(0,0,0)`。导出画面 `fix2-final-f150.png` 可见
Sponza 柱廊/地面/地毯正常受光，**无色斑、无硬边黑斑**。

### 复核（已回退的假设）

曾尝试让 FlaxGI 探针的环境（天空）辐射不再受光照 pass 的 IBL 开关约束
（`render/GPUDrivenRenderer.cpp` `flaxIBLEnvironmentEnabled` 解耦）。验证捕获
`capture-fix3-f150_frame150.rdc` 显示：追踪缓冲低值桶增多（bucket1 12599→67450），
但**光照输出未改变**（2472 直方图仅差 ~0.1%，抽样 texel (250,200) 逐位相同）。
无可见收益且改变了调试开关语义，故**已回退**，Demo.exe 已按回退后源码重建（13:35:58）。

## 剩余问题（非量化问题，属光照配置）

黑斑区域在物理上确实"无光"。若要让拱顶/室内也被照亮，需改变光照配置而非量化：

1. `app/MinimalLatestApp.h:262` 的 `enableIBL = false` 使场景只剩太阳直射；
   环境贴图其实已加载（`iblParams.w=1`，`environmentTex` 516 max 6992，`luminance` 正常）。
2. `lightSettings.ambient` 为 0，且 IBL 关闭时 `evaluateEnvironmentBackground` 返回黑色。
3. FlaxGI 的 miss 辐射被同一个 IBL 开关关闭（`environmentParams.x=0`），
   室内探针因此拿不到天空能量。
