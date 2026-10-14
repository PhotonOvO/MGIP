# Live replay 截图证据失效链：幻影显示器、遮挡与近黑边界帧

日期：2026-09-09　范围：`tools/editor_ui_live_replay.ps1`、G8-R1 截图/DPI/分辨率轴

## 结论摘要

`mgif-live-matrix-20260909-R1`（192 轮，bitblt 为主）与 `-R2`（192 轮，全部
desktop-copy）在运行时全部报告 `PASS_BOUNDED_LIVE_PROOF`，但**没有一张
`client.png` 是编辑器边界帧**。失效由三层叠加造成，前两层已在 harness 侧修复，
第三层是应用侧待查缺陷：

1. **幻影显示器**：窗口被居中到没有物理显示面的虚拟屏右半区 → 捕获得到确定性
   两色占位图。
2. **遮挡**：钉回主屏后，屏幕拷贝拍到的是覆盖该区域的其它进程窗口。
3. **近黑边界帧**：排除以上两者后，应用自身呈现的边界帧仍近乎全黑，缺少 3D
   场景与大部分面板内容 —— 与 20260908 的真实帧显著不同。

## 证据

### 显示器几何（本机实测）

```
SM_CXSCREEN(0)=2560  SM_CYSCREEN(1)=1440        # 主屏 2560x1440 @ (0,0)
SM_CXVIRTUALSCREEN(78)=5120  SM_CYVIRTUALSCREEN(79)=1440
SM_XVIRTUALSCREEN(76)=0      SM_YVIRTUALSCREEN(77)=0
```

虚拟屏宽 5120 而物理主屏只有 2560 → x∈[2560,5120) 无显示面。三时刻探针测得应用
窗口 `clientOrigin=(3222,440)`，恰为在第二显示区居中：`2560+(2560-1296)/2+8≈3200`。

### 两色占位图（R1 与 R2 共同表现）

- R1 `Debug/U06-…-1280x720-dpi1/client.png`（transport=`window-dc-bitblt`）与
  R2 同用例（transport=`desktop-copy`）顶色均为
  `(48,10,36)` + `(40,50,40)` + `(128,128,128)`。
- 40x40 patch 内**仅 2 种颜色**；相邻同色率 0.999 → 大块纯色矩形，非噪声、非 UI。
- 色块 bbox：`(48,10,36)` 全屏、`(40,50,40)` 为 (178,66)-(1278,324) 矩形。
- 跨用例/跨配置/跨 DPI 字节相同，例：
  `Debug/U01-…-dpi1`（applied 1.0, documentRevision 0, semanticHash 4f6c44c7…）与
  `Debug/U04-…-dpi1p5`（applied 1.5, documentRevision 4, semanticHash b0857044…）
  sha256 相同、mtime 相差 108 s。含边界 UI 的帧不可能如此。
- R2 共 19 组 59 根碰撞；R1 共 21 组 48 根。

### 排除的假设

| 假设 | 检验 | 结论 |
|---|---|---|
| 桌面合成冻结 | 无窗口/有窗口/窗口消失三次全屏截图：差 26.03% / 26.04% / 0.01% | 桌面是活的，否证 |
| 碰撞帧是壁纸裁剪 | 3 帧在桌面图上 4 px 步长全偏移模板搜索（10 点探针 + 7 px 全核） | none，否证 |
| 仅 bitblt 陈旧重定向表面 | R2 全量 desktop-copy 仍碰撞 | 非通道问题 |

`window-dc-bitblt` 在 20260908 能取到真实帧，是因为它读窗口自身重定向表面、不受
屏幕位置影响；而 R1 中同一通道取到占位图，是因为窗口位于幻影区、该表面未被维护。

### 主题基准（源码钉定）

`app/MinimalLatestApp.h:2117` `ImGui::StyleColorsDark(&uiStyle)`；
`:2146` `colors[ImGuiCol_WindowBg] = ImVec4(0.018f, 0.022f, 0.030f, 0.24f)` →
×255 = `(4.6,5.6,7.7)`，实测新帧顶色第 4 位正是 `(5,8,12)`，证明捕获对象确为本应用窗口。

### 钉入主屏后的两次捕获（通道互相印证）

| 运行 | transport | uniq | lumMean | ink(max>20) | colorful | 顶色 |
|---|---|---|---|---|---|---|
| `pin4`（pinned+topmost） | desktop-copy | 55 | 3.9 | 0.037 | 0.0126 | (0,0,0),(2,2,3),(2,3,4),(5,8,12) |
| `pin5`（强制 bitblt） | window-dc-bitblt | 85 | 3.9 | 0.032 | 0.0106 | 同上 |
| 20260908 真实帧（对照） | window-dc-bitblt | 562 | 33.4 | 1.000 | 0.1717 | (25,26,28),(29,35,54),(51,53,59) |
| `pin-720`（钉入但未 topmost） | desktop-copy | 25373 | 151.8 | 0.973 | 0.5080 | (255,255,255) 33%、(246,247,248)、(218,226,249) |

`pin-720` 的亮色帧与源码钉定的暗色主题矛盾 → 判定为**其它进程窗口**，故加入
topmost + `WindowFromPoint` 归属校验。

## 已实施的 harness 修复（`tools/editor_ui_live_replay.ps1`）

1. 新增 `GetForegroundWindow` / `WindowFromPoint` / `GetWindowThreadProcessId` P/Invoke。
2. 可达性预检改为以**主屏**（而非虚拟屏）为准，并新增 `primaryMonitor` 证据字段。
3. 捕获前把客户区**钉入主屏居中**（`SetWindowPos` HWND_TOPMOST，SWP_NOSIZE|SWP_NOACTIVATE），
   重新解析客户区，fail-closed 校验客户矩形完整落在主屏内。
4. 遮挡校验：客户区中心 `WindowFromPoint` 必须返回本窗口或**同进程**窗口，
   最多 3 次重申 topmost/foreground，否则抛错拒拍。
5. 证据字段：`screenshot.clientOrigin{x,y}`、`pinnedToPrimary`、`topmostVerified`、
   `foregroundMatches`；顶层 `primaryMonitor{width,height}`。
6. 失败诊断：审计不通过时把被拒位图存为 `client.png.rejected.png`，并在异常文本中带出
   `lastAuditError`、`clientOrigin`、`extent`，使 blank 判定可由文件系统证据复核。

现有像素闸门（`nonBlackRatio>=0.05` 等）在此修复后**正确拒绝**了近黑帧，即 harness
不再可能产出伪 PASS 截图证据。

## 仍待处理（应用侧，阻塞 G8-R1 截图轴）

1. 定位为何当前二进制的 live 边界帧近黑：3D 场景未呈现 / 面板底色缺失 /
   边界呈现帧与握手帧不同步。对照 20260908 真实帧（`ink20=1.000`, `uniq=562`）。
   - 已排除：捕获通道、遮挡、屏幕位置、主题设置、桌面冻结。
   - 相关源码：`app/MinimalLatestApp.h:3081-3089`（边界：waitForIdle →
     writeEditorLiveEvidence → `waitForAutomationCaptureHandshake(marker,"post-render")`）、
     `:6183` `onEditorLiveFrameRendered()`（经 `onAutomationFrameRendered()` 于
     `m_renderer.render()` 之后调用）、`:3049-3053` `markerFrameHold = emptyStart ? 12 : 3`、
     `:4960-4992` 停靠等待循环仅 `glfwPollEvents()`+`sleep(2ms)`，**不再渲染/呈现**。
   - 各根 `sync/*.ready.json`：`phase=post-render`，`frame` 仅 8–18（中位 14）。
2. 边界帧修复后重跑 192 轮矩阵，要求：零 `client.png` 摘要碰撞、每组 DPI 响应、
   `topmostVerified=true`、`clientOrigin` 落在主屏内、且 `lumMean`/`uniq` 落在真实帧区间。
3. `tools/verify_editor_live_matrix.py` 需增加上述新字段的 fail-closed 校验
   （clientOrigin 在主屏内、pinnedToPrimary/topmostVerified 为真、uniq 与 ink 下限）。

## 状态

- R1、R2 及其 `verification.json`：诊断历史，**不得作为证据发布**。
- G8-R1 截图/DPI/分辨率轴：**OPEN**。
- 2560x1440 装饰窗口客户区本机不可达（上限 2560x1421）；仅 undecorated 可达，
  且需在第 1 项修复后才具证据价值。

## 2026-09-09 追加：收敛探针（hold 实验）

在窗口已钉入主屏并 topmost 归属校验通过的前提下，改 `markerFrameHold` 做对照：

| hold | ready frame | 捕获结果 | 判定 |
|---|---|---|---|
| 12 / 3（原值） | 8–18 | 近黑：uniq 55/85、lumMean 3.9、顶色含 (5,8,12)=WindowBg×255 | 应用自身呈现内容，未收敛 |
| 120 / 90（探针） | 101 | 纯白 ghost：uniq 7、lumMean 255.0、230393/230400 为 (255,255,255) | DWM 幽灵面，被闸门正确拒绝 |

结论：**加长 hold 不是解**。`waitForAutomationCaptureHandshake`
（`app/MinimalLatestApp.h:4960-4992`）在停靠期间只 `glfwPollEvents()` + `sleep(2ms)`，
**不渲染也不呈现**；被拍摄到的永远是“最后一次呈现的图像”，而更长的边界前渲染反而让
DWM 为停靠窗口换上幽灵面。已把 hold 回退为 12u/3u，并在源码注释中保留上述两组实测。

### 待实施的最小应用侧修复（G8-R1 截图轴的真正阻塞）
在停靠等待循环内**继续渲染并呈现同一边界帧**（不推进自动化状态、不消费输入），
使握手期间 swapchain 始终持有收敛后的边界图像：
- 位置：`app/MinimalLatestApp.h:4960-4992` 的 `while(true)` 停靠循环；
- 需要复用主循环的“begin ImGui frame → editor UI → `m_renderer.render()` → present”路径，
  而不是仅 `glfwPollEvents()`；
- 必须保持 `phase=post-render` 语义与 `no_post/no_ddgi/taa` 选项不变，
  且不得引入新的自动化状态推进（否则会破坏 `stale editor command sequence` 约束）；
- 验收：钉入主屏 + topmost 校验通过时，desktop-copy 与 bitblt 应同时得到
  收敛帧（对照 20260908：uniq≈562、lumMean≈33.4、ink20≈1.000），
  且 12/3 hold 下不再出现近黑或纯白帧。

## 2026-09-09 追加 2：为何"停靠期渲染"不是局部补丁，以及精化后的机理

### 帧路径事实（`app/MinimalLatestApp.h`）
`run()`（:745）把整帧内联，顺序为：
`glfwPollEvents()` → 配置/发布 → `waitForAutomationCaptureHandshake()`（默认相位）
→ `m_renderer.render(frameParams)`（:1687，录制+提交+呈现的唯一入口）
→ `onAutomationFrameRendered()`（:1700，live 边界在其内部调用
`waitForAutomationCaptureHandshake(marker,"post-render")`）。

关键约束：`frameParams` 是 `run()` 循环体内的局部量，
`waitForAutomationCaptureHandshake()` 内**无法取得**，故"停靠循环里继续渲染并呈现"
必须重构帧路径（把 per-frame 渲染体提取成可重入的成员函数，或把 frameParams 提升为成员），
不是可以就地插入几行的补丁。本轮不做该重构，以免在预算内破坏整机帧路径。

### 精化机理：flip-model 重定向表面
三次实测可由同一机理解释——DWM 对 flip-model 交换链**不用应用呈现更新重定向表面**，
一旦需要重新合成（窗口被移动/改 topmost/被遮挡/位于无显示面区域），
合成取到的是**空白或占位的重定向表面**：

| 观测 | 机理 |
|---|---|
| 幻影区两色占位 (48,10,36)/(40,50,40) | 无显示面区域无有效合成内容 |
| 钉入主屏+topmost 后近黑帧（含 WindowBg 色） | 读到应用早期未收敛呈现 |
| hold 120/90 后纯白 ghost（uniq 7、lumMean 255.0） | 移动/topmost 触发重合成，重定向表面为空白 |
| 20260908 真实帧（uniq 562、lumMean 33.4） | 未移动窗口、DWM 合成态仍持有内容 |

推论：只有**在停靠期间持续呈现**，DWM 合成才始终持有收敛后的边界帧；
这也解释了为何"移动窗口到主屏"这一必要修复反而把内容变成了 ghost。

### 建议实施顺序（下一轮）
1. 把 `run()` 的 per-frame 渲染体提取为成员函数（例如 `renderPresentFrame(FrameParams&)`），
   保持调用顺序与 `framePhase` 语义不变；`frameParams` 构建留在 `run()`。
2. 让 live 边界把"当前帧参数"暂存为成员（仅在 `phase=post-render` 停靠期间使用），
   停靠循环改为：`glfwPollEvents()` → 重渲染并呈现同一边界帧 → `sleep(~8ms)`，
   **不推进** `m_automationFrame`、不写证据、不消费输入、不触发新的 marker。
3. 保留 harness 的钉入主屏 + topmost + `WindowFromPoint` 归属校验（它们使捕获对象确定）。
4. 验收：desktop-copy 与 bitblt 同时得到收敛帧（对照 20260908：uniq≈562、
   lumMean≈33.4、ink20≈1.000），且 12/3 hold 下不再出现近黑或纯白帧；
   随后重跑 192 轮矩阵（零摘要碰撞 + 每组 DPI 响应 + topmostVerified + clientOrigin 在主屏内）。

## 2026-09-09 追加 3：停靠期重呈现假设已**证伪**

实施（最小实现，编译链接 `EXITCODE=0`）：新增成员 `demo::RenderParams m_automationPresentParams{}`
+ `m_automationPresentParamsValid`；在 `:1687` `m_renderer.render(frameParams)` 前缓存边界帧参数
（其指针成员均为本次循环迭代局部量，停靠期全程存活）；停靠循环改为
`glfwPollEvents()` → `m_renderer.render(m_automationPresentParams)` → `sleep(2ms)`，
不推进 `m_automationFrame`、不调用 `onAutomationFrameRendered()`。

实测（U02-hierarchy 1280x720 dpi1，Debug，钉入主屏+topmost 校验通过）：

| 变体 | uniq | lumMean | ink20 | 顶色 |
|---|---|---|---|---|
| 改动前 | 55 / 85 | 3.9 | 0.037 / 0.032 | (0,0,0),(2,2,3),(2,3,4),(5,8,12) |
| 停靠期重呈现 | 79 | 3.8 | 0.029 | (0,0,0),(2,2,3),(2,3,4) |
| 20260908 参考 | 562 | 33.4 | 1.000 | (25,26,28),(29,35,54),(51,53,59) |

内容**没有变化** → "被拍摄的是陈旧的、未收敛的最后呈现帧"这一假设不成立。
补丁已回退（源码恢复原状，Debug `editor_ui_live_replay` 已按回退后源码重建）。

### 精化后的结论
捕获对象确为本应用窗口（顶色含 `(5,8,12)` = `WindowBg(0.018,0.022,0.030)×255`），
且早期运行的焦点审计通过（要求 `ImGui::FindWindowByName("Scene Browser")` 处于 active），
说明 **ImGui 面板确实已绘制**。当前帧 55% 为纯 `(0,0,0)`、33% 为 `(2,2,3)`，
与 20260908 参考帧 97% 为 `(25,26,28)` 的差异可由同一解释覆盖：
`WindowBg` alpha 仅 0.24，面板是**半透明**的 ——
参考帧的面板叠在**已点亮的 3D 场景**上（≈(25,26,28)），当前帧的面板叠在**全黑 viewport** 上（≈(2,2,3)/(5,8,12)）。

→ 待查缺陷从"呈现/握手时序"转为 **live replay 下 3D viewport 内容为黑**（场景未渲染/未点亮/未投影）。

### 下一步（按此顺序，最小代价优先）
1. 读当前根 `app/state.json` 的 `renderer.*`（`runtime_content_digest`、`settled`、`gpu_idle_waited`）
   与场景/投影字段，确认 `m_projectedSceneAsset` 是否非空、`resolveSceneLightSettings()` 是否给出光源；
   相关源码锚点：`:1615` `frameParams.gltfModel = nullptr;`、`:1624` `lightSettings`、
   `:1627-1629` `sceneLights`/`sceneLightSceneNodes`（以 `m_projectedSceneAsset` 为条件）。
2. 对照 Demo/视觉路径（已知能产出真实内容）：同一 renderer 下 viewport 是否亮 →
   可区分"renderer 回归"与"live replay 场景投影缺失"。
3. 若为场景投影缺失：定位 live replay 的投影/绑定路径（U01 空启动改造曾调整
   `initializeEditorLiveReplay()` 对 `session()`/`projection()` 的依赖），确认预加载用例的
   `m_projectedSceneAsset` 是否仍被填充。
4. 修复后再走原验收：desktop-copy 与 bitblt 同时得到 uniq≈500+/lumMean≈30–40/ink20≈1.0 的收敛帧，
   然后重跑 192 轮矩阵。

## 2026-09-09 追加 4：结论再次精化 —— 可能是 live 亮度闸门**标定错误**，而非应用缺陷

`app/state.json` 的 `renderer` 块只有 `settled` / `gpu_idle_waited` / `runtime_content_digest`
（当前根 `bdaf1cc4…`，20260908 根 `828cf097…`，用例不同故不可比），**不含**场景/光照/相机字段，
因此无法据此判定"viewport 未点亮"。

改用跨 harness 标定对比（同一像素度量口径，stride=4）：

| 帧 | ink(max>20) | coherentRunRatio | structuredRowRatio | colorful | uniq |
|---|---|---|---|---|---|
| 当前 live（钉入主屏+topmost，U02 720p dpi1） | 0.037 | **0.289** | **0.272** | 0.0126 | 55–85 |
| 视觉 harness **已认定真实**的暗帧最小值 | 0.0235 | 0.2778–0.3000 | 0.3167–0.3296 | 0.0029 | — |
| 视觉 harness 退化样本 | — | 0.1444 | 0.1778 | — | — |
| 合成黑 blank | 0.000 | 0.000 | 0.000 | 0.0000 | 1 |
| 合成白 blank | 1.000 | 1.000 | 1.000 | 0.0000 | 1 |
| R1/R2 两色占位 | 1.000 | 1.000 | 1.000 | 0.6349 | 114 |
| 20260908 live 参考帧 | 1.000 | 1.000 | 1.000 | 0.1717 | 562 |

要点：
1. 当前 live 帧的结构度量（0.289 / 0.272）**落在视觉 harness 真实帧区间内**，
   且远高于退化样本（0.1444 / 0.1778）与黑 blank（0 / 0）；
   它只卡在 live 侧的亮度闸门 `nonBlackRatio >= 0.05`（实测 0.037）。
2. 视觉 harness 早已因"真实帧可以极暗"而放弃亮度主导、改用结构闸门
   （`coherentRunRatio>=0.19`、`structuredRowRatio>=0.21`），
   live 侧却仍用亮度/色度比值闸门 —— **两侧标定不一致**。
3. 20260908 参考帧 ink=1.000 说明当时整帧每像素 max>20（面板叠在已点亮场景上）；
   当前帧暗得多。二者都可能"真实"，差异来自渲染/主题演进，**不能仅凭亮度断定谁真**。

### 因此下一步改为（先标定，再决定是否算应用缺陷）
1. 用**结构 + 纹理**度量区分"真实暗 UI"与"垃圾帧"，并实测 edgeRatio（相邻采样点亮度差>24 的比例）
   与 distinct-2x2-pattern 数：预期真实暗 UI 因抗锯齿文本而有明显细纹理，
   两色占位只有 1 条边界（可分离），白/黑 blank 与 uniq=1 可分离。
2. 据此把 live 闸门改为与视觉侧一致的结构闸门（保留 `uniqueColors` 下限，
   亮度/色度比值降级为记录而非判定），并在 `tools/verify_editor_live_matrix.py` 同步。
3. 只有当结构度量也无法把当前帧与垃圾帧分开时，才回到"应用侧 viewport 全黑"缺陷路线
   （届时用 RenderDoc 抓 live replay 一帧，直接看 viewport pass 的输入/输出与光源常量）。
4. 无论走哪条路，harness 的钉入主屏 + topmost 归属校验都必须保留（它们已排除幻影区与遮挡）。

## 2026-09-09 追加 5：**撤回**追加 4 的"闸门标定错误"判断

追加 4 曾据 `coherentRunRatio/structuredRowRatio` 落在视觉真实帧区间而怀疑 live 亮度闸门标定错误。
补测纹理丰富度后该判断不成立：

| 帧 | edgeRatio | distinct 2×2 图案(32 级量化) | uniq |
|---|---|---|---|
| 当前近黑 live（parkrender） | 0.0291 | 178 | 51 |
| 当前近黑 live（pin4） | 0.0304 | 198 | 55 |
| R2 两色占位（已知垃圾） | 0.1100 | 879 | 114 |
| 20260908 真实 live 参考 | 0.0723 | 3142 | 562 |
| 外来亮窗（已知垃圾） | 0.1244 | 14003 | 25373 |
| 合成黑 / 白 blank | 0.0000 | 1 | 1 |

判读：
1. 近黑帧的结构丰富度是**所有非 blank 样本中最低**的，甚至低于已确证为垃圾的两色占位
   （uniq 51 vs 114、pat 178 vs 879、edge 0.029 vs 0.110），比真实参考帧低 6–18 倍。
2. 带文本的真实编辑器 UI 不可能只有 51 种颜色 / 178 种 2×2 图案（参考帧 562 / 3142）。
3. 故近黑帧**不是编辑器 UI**，而是近乎平坦的暗面；`coherentRunRatio` 之所以不低，
   是因为该度量在 ink 仅 3.3% 时由稀疏噪点簇即可抬高，**不足以单独作为真实性判据**。
4. 结论：**live 现有闸门拒绝该帧是正确的**；追加 4 提出的"改用结构闸门"不得实施，
   除非同时加入纹理丰富度下限（例如 `uniqueColors >= 256` 且 `distinct2x2 >= 500`，
   可分离：真实 562/3142、外来窗 25373/14003 vs 近黑 51/178、占位 114/879、blank 1/1）。

### 缺陷归属重新确定：应用侧
在窗口已钉入主屏、topmost 归属校验通过、两种通道（desktop-copy 与 window-dc-bitblt）
一致、且停靠期持续重呈现也**不改变内容**的前提下，呈现出的仍是近平坦暗面 ——
即 live replay 边界处**编辑器 UI 未真正进入被呈现的交换链图像**，
尽管 `phase=post-render`、焦点审计（要求 Scene Browser active）通过、`renderer.settled=true`。

### 下一步（按 AGENTS.md 的 RenderDoc 规程）
1. 用 `renderdoc-debug` skill 与 `rdc_*` 工具对 `editor_ui_live_replay.exe`（Debug，U02 1280x720 dpi1）抓帧；
2. 以 frame overview 定位 present 之前的最后一个 pass，核对：ImGui 绘制 pass 是否存在、
   其 target 是否即 swapchain backbuffer、viewport pass 输出是否被合成进 present 图像；
3. 对 present 图像取像素历史（选取应为面板/文本的坐标），确认是"未绘制"还是"被后续 clear/覆盖"；
4. 每个假设与结论都必须引用 EID；先取证再提最小源码补丁；
   修复需经重建 → 重抓 → RenderDoc 复核才算确认。

## 2026-09-09 追加 6：RenderDoc 取证的前置障碍与可行方案

### 障碍
live replay 在每个边界停靠等待 harness 写入 continue marker（`:4960-4992`），
超时 `captureSyncTimeoutMilliseconds`（默认 30000，`:132`）后是 **throw**，不是跳过；
`app/editor/EditorUiLiveReplayMain.cpp:106` 解析该超时、`:156` 拒绝 0 值（fail-closed）。
应用选项全集（无"免握手"开关）：
`--editor-live-case`、`--editor-live-evidence-dir`、`--editor-live-sync-dir`、
`--editor-live-executable-sha256`、`--editor-live-source-sha256`、`--editor-ui-dpi-scale`、
`--editor-window-undecorated`、`--editor-visual-*`（case/component/dpi/state/sync-dir/variant/
executable-sha256/source-sha256）。
故直接用 `rdc_capture` 启动 `editor_ui_live_replay.exe` 会在第一个边界挂死至超时抛错。

### 可行方案（按代价排序）
1. **并发 marker 服务 + rdc_capture**：先用 bash 起一个后台循环（nohup 持久），
   轮询 `--editor-live-sync-dir` 下出现的 `*.ready.json`，为每个 ready 写入对应 continue marker
   （与 `tools/editor_ui_live_replay.ps1` 同一命名/内容契约，可从该脚本抄）；
   再 `rdc_capture(executable=editor_ui_live_replay.exe, args=[…live 选项…], frame=<目标 Present 序号>)`。
   注意 `--editor-live-evidence-dir`/`--editor-live-sync-dir` 必须**全新且不存在**；
   `--editor-live-executable-sha256`/`--editor-live-source-sha256` 会被校验，
   取值方式照抄 harness（`source-identity.json` 的 `aggregateSha256` 与 exe 摘要）。
2. **先抓视觉路径作对照基线**：`--editor-visual-case=V01-*`（视觉 harness 已知能产出真实内容），
   用同法解决其 `--editor-visual-sync-dir` 握手；对比 present 前最后一个 pass 与 ImGui pass 的 target，
   可区分"live 模式特有缺陷"与"renderer/present 通用缺陷"。
3. 取证要点（每个结论引用 EID）：
   - present 之前是否存在 ImGui 绘制 pass；其 color target 是否即 swapchain backbuffer
   - 若 ImGui pass 存在但 target 是离屏纹理：核对合成/copy pass 是否执行、是否被后续 clear 覆盖
   - 对 present 图像取像素历史（选应为面板/文本的坐标，例如客户区中心与左栏文本行），
     判定"从未绘制"还是"绘制后被覆盖"
   - 核对 viewport pass 输出（3D 场景）是否为黑，与 `frameParams.gltfModel=nullptr`（`:1615`）、
     `lightSettings`（`:1624`）、`sceneLights`/`sceneLightSceneNodes`（`:1627-1629`，
     条件依赖 `m_projectedSceneAsset`）的关系
4. 修复确认标准：重建 → 重抓 → RenderDoc 复核 → 再用 harness 单例验收
   （uniq≈500+、distinct2x2≈3000+、lumMean≈30–40、ink20≈1.0），最后才跑 192 轮矩阵。

## 2026-09-09 追加 7：撤回追加 5/6 的"应用侧缺陷"结论

追加 5 判定"近黑帧不是真实编辑器 UI、缺陷在应用侧"，**该结论错误，现予撤回**。
真因有两处，均在 harness：

1. **`uniqueColors` 度量缺陷**：PowerShell 5.1 中 `[byte] -shl 16` 保持 Byte 类型并截断为 0，
   打包值退化为仅 B 通道 → 追加 5 用来"证伪真实性"的 51/55 色其实是蓝色通道计数；
   同一帧真实颜色数为 562（与 PIL 独立计数一致）。
2. **闸门亮度标定错误**：`nonBlackRatio>=0.05 / colorfulRatio>=0.02 / edgeRatio>=0.03`
   按 20260908 亮帧标定，拒绝当前二进制的真实暗帧（视觉 harness 已接受的同类暗帧
   实测 nonBlackRatio 0.014-0.024），却放行两色占位与外来亮窗。

修正与重标定见 `docs/editor-parity/pixel-audit-uniquecolors-byte-shift-defect.md`。
离线一致性证明 9/9 正确；`ctest -R os_replay` 4/4 通过；
修正后 live 捕获立即恢复 `PASS_BOUNDED_LIVE_PROOF`（根 `/mnt/g/Temp/gatefix-u02-1`），
R3 矩阵首批 4 根全部通过且 DPI 响应真实（0.044-0.070，sha256 互异）。

因此追加 6 的 RenderDoc 取证**不再需要**（其前置障碍记录保留为环境事实：
`rdc` 后端在本环境不可用，`spawn rdc ENOENT`）。
白 blank 捕获期仍然存在（本次 6/6 次整例重跑得到 `lumMean=255.0, uniq=2`），
新闸门对其正确 fail-closed；矩阵驱动以 20 次预检 + 每轮 5 次有界重试跨越该故障。
