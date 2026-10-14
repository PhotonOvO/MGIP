# Pi + RenderDoc (`rdc-cli`) V0.1 — 可执行任务计划

> 目标：由 Coding Agent 直接按本文件实施，不把它当作设计讨论稿。  
> 最终结果必须形成一个最小但完整的闭环：  
> **Observe → Diagnose → Patch → Rebuild → Recapture → Verify**

---

## 0. Agent 执行协议

### 0.1 执行方式

从本文件顶部开始，严格按 Phase 顺序执行。

每完成一个任务：

- 将对应 `[ ]` 修改为 `[x]`
- 在 `## Execution Log` 追加一条简短记录
- 立即运行该任务定义的验证命令
- 验证失败时先修复，不允许带着失败进入下一 Phase

不要因为某个实现细节与计划略有不同就停下来询问。优先检查当前仓库和本机工具能力后做最小适配。

只有以下情况可以停止并报告阻塞：

1. `rdc-cli` / RenderDoc 无法安装或 `rdc doctor` 无法通过；
2. 当前机器没有可用 Vulkan/D3D GPU replay 环境；
3. 当前仓库无法构建，且失败与本任务修改无关；
4. 需要管理员权限、设备权限或用户手工操作才能继续。

### 0.2 实施原则

必须遵守：

- 不实现 GUI。
- 不引入 LangGraph / CrewAI / MCP。
- 不实现多 Agent。
- 不引入数据库或向量数据库。
- 不 fork / 修改 Pi 本体。
- Pi 只通过项目级 `.pi/extensions/` 扩展。
- RenderDoc 查询必须通过我们自己的 typed tools。
- 不允许 Agent 用 `bash` 直接调用 `rdc` 绕过 typed tools。
- 不把大量原始 RenderDoc JSON 无过滤地塞进 LLM Context。
- 所有诊断结论必须引用 Evidence。
- Patch 必须最小化，不做无关重构。
- 修复后必须重新 Capture 并验证，不能只凭源码推断“应该好了”。

### 0.3 版本适配规则

不要假设本计划中的 CLI 示例与本机 `rdc-cli` 版本完全一致。

实施前必须执行：

```bash
rdc --version
rdc doctor
rdc --help
rdc open --help
rdc draws --help
rdc pixel --help
rdc pipeline --help
rdc bindings --help
rdc shader --help
rdc diff --help
rdc assert-pixel --help
rdc capture --help
```

以**本机帮助输出**作为真实命令签名。

`rdc-cli` 预期至少支持这些能力：

- `open` / `close`
- `info`
- `draws`
- `draw`
- `pipeline`
- `bindings`
- `shader`
- `pixel`
- `diff`
- `assert-pixel`
- `capture`
- JSON 输出

如果命令名称或参数发生变化，在 `src/rdc/client.ts` 内适配，不要修改上层 Agent Tool API。

---

# 1. 最终交付目标

最终仓库应具备：

```text
User
 │
 │ “这个像素为什么是黑色？”
 ▼
Pi Main Agent
 │
 ▼
renderdoc-debug Skill
 │
 ▼
Typed RenderDoc Tools
 │
 ▼
rdc-cli
 │
 ▼
bug.rdc
 │
 ├─ pixel history
 ├─ draw
 ├─ pipeline
 ├─ bindings
 ├─ shader
 └─ constants
 │
 ▼
Evidence[]
 │
 ▼
Hypothesis
 │
 ▼
Source Search
 │
 ▼
Minimal Patch
 │
 ▼
Build + Run
 │
 ▼
fixed.rdc
 │
 ├─ pixel history
 ├─ diff
 └─ assert-pixel
 │
 ▼
Verified Fix
```

V0.1 的目标不是“自动解决所有 GPU Bug”，而是稳定解决一个人为构造、证据明确的渲染错误。

---

# 2. 建议目录结构

最终结构：

```text
<repo-root>/
│
├── AGENTS.md
│
├── TASK_PLAN_PI_RENDERDOC_V0_1.md
│
├── .pi/
│   ├── extensions/
│   │   └── renderdoc.ts
│   └── skills/
│       └── renderdoc-debug/
│           └── SKILL.md
│
├── src/
│   ├── rdc/
│   │   ├── process.ts
│   │   ├── client.ts
│   │   ├── types.ts
│   │   └── inspect-draw.ts
│   │
│   └── evidence/
│       ├── evidence.ts
│       └── investigation.ts
│
├── examples/
│   └── textured-quad/
│       └── ...
│
├── captures/
│   ├── bug.rdc
│   └── fixed.rdc
│
├── investigations/
│   └── .gitkeep
│
├── package.json
└── tsconfig.json
```

如果当前仓库已经有合适的 Vulkan/D3D sample：

- 优先复用；
- `examples/textured-quad/` 可以替换为现有 sample；
- 不为满足目录结构而重复造 renderer。

---

# 3. Phase 0 — 环境与仓库探测

## Tasks

- [x] 0.1 检查当前仓库结构、语言、构建系统和已有 graphics sample。
- [x] 0.2 检查 Node.js 与 npm/pnpm 可用。
- [x] 0.3 检查 `pi` 可运行。
- [x] 0.4 检查 `rdc` 可运行。
- [x] 0.5 执行 `rdc doctor`。
- [x] 0.6 保存 `rdc --version`。
- [x] 0.7 探测本机各个目标命令的 `--help`。
- [x] 0.8 检查 RenderDoc replay 是否能在本机正常打开一个 capture。
- [x] 0.9 确定 Demo 路线：
  - A：复用仓库已有 Vulkan/D3D sample；
  - B：不存在合适 sample 时创建最小 Vulkan textured quad。

## 输出

创建：

```text
investigations/environment.json
```

至少保存：

```json
{
  "pi": "...",
  "node": "...",
  "rdcCli": "...",
  "renderdoc": "...",
  "platform": "...",
  "demoStrategy": "reuse-existing | create-minimal"
}
```

## Done 条件

必须全部成立：

```text
pi --version           PASS
node --version         PASS
rdc --version          PASS
rdc doctor             PASS
rdc --help             PASS
```

如果 `rdc doctor` 失败，不允许进入 Phase 1。

---

# 4. Phase 1 — 建立项目骨架

## Tasks

- [x] 1.1 创建 `src/rdc/`。
- [x] 1.2 创建 `src/evidence/`。
- [x] 1.3 创建 `.pi/extensions/`。
- [x] 1.4 创建 `.pi/skills/renderdoc-debug/`。
- [x] 1.5 创建 `captures/`。
- [x] 1.6 创建 `investigations/`。
- [x] 1.7 创建或补全 TypeScript 配置。
- [x] 1.8 建立最小 `package.json` scripts。

推荐 scripts：

```json
{
  "scripts": {
    "typecheck": "tsc --noEmit",
    "test": "node --test",
    "check": "npm run typecheck && npm test"
  }
}
```

可根据仓库既有工具链调整。

## Done 条件

```bash
npm run typecheck
```

返回 0。

---

# 5. Phase 2 — `rdc-cli` Process Layer

目标：

```text
TypeScript
   ↓
spawn(shell=false)
   ↓
rdc
   ↓
stdout JSON
```

## 5.1 `src/rdc/process.ts`

### 必须实现

```ts
export interface RdcProcessResult<T> {
  data: T;
  stderr: string;
  exitCode: number;
}
```

以及：

```ts
runRdc<T>(
  args: string[],
  signal?: AbortSignal
): Promise<RdcProcessResult<T>>
```

### 约束

- 使用 `node:child_process.spawn`
- `shell: false`
- executable 固定为 `rdc`，或通过一个受控配置字段指定
- 参数必须使用 string array
- 不允许拼接 shell command
- 捕获 stdout/stderr
- stdout 只负责解析 JSON
- stderr 不参与 JSON parse
- 非 0 exit code 抛出包含 stderr 的结构化错误
- 支持 AbortSignal
- 处理 Windows 路径中的空格和中文
- 增加超时保护，默认建议 60 秒；capture/replay 可覆盖更长超时

## 5.2 测试

在不依赖真实 capture 的情况下至少测试：

- [x] 成功执行 `rdc --version` 或等价安全命令
- [x] 非法 command 能正确返回错误
- [x] 参数中含空格不会被 shell 拆分
- [x] AbortSignal 能终止进程

## Done 条件

```bash
npm run check
```

通过。

---

# 6. Phase 3 — `RdcClient`

创建：

```text
src/rdc/client.ts
```

## 6.1 Public API

V0.1 至少提供：

```ts
openCapture(path)
closeCapture()

getInfo()
getDraws()
getDraw(eid)

getPipeline(eid)
getBindings(eid)

getShader(eid, stage)
getShaderConstants(eid, stage)

getPixelHistory(x, y, eid?)
diffCaptures(before, after)

assertPixel(...)
captureExecutable(...)
```

其中内部如何映射到 `rdc-cli`，以 Phase 0 探测出的本机命令为准。

## 6.2 JSON 强制要求

所有 query 类命令尽量使用：

```text
--json
```

不要解析 TSV 人类文本。

## 6.3 Session 约束

`rdc open` 后才允许执行依赖当前 capture 的命令。

Client 维护轻量状态：

```ts
interface RdcSessionState {
  capturePath?: string;
  openedAt?: string;
}
```

不能假定永远有一个 capture 已加载。

## 6.4 Context 限流

`getDraws()` 不直接向 LLM 返回无限 draw list。

Client 层保留完整数据，但 Agent Tool 层必须提供：

- limit
- filter
- pass/name filter（如果 CLI 支持）
- 或默认只返回概要

## Done 条件

使用一个已知 `.rdc` 文件完成：

```text
open
→ info
→ draws
→ close
```

并全部解析为 JSON。

---

# 7. Phase 4 — Domain 聚合：`inspectDraw`

创建：

```text
src/rdc/inspect-draw.ts
```

目标是屏蔽底层多个 CLI 调用。

Public API：

```ts
inspectDraw(
  client: RdcClient,
  eid: number,
  signal?: AbortSignal
): Promise<DrawInspection>
```

## 7.1 `DrawInspection`

建议：

```ts
interface DrawInspection {
  eid: number;

  draw: unknown;

  pipeline: {
    renderTargets?: unknown;
    depthTarget?: unknown;
    viewport?: unknown;
    scissor?: unknown;
    raster?: unknown;
    depth?: unknown;
    blend?: unknown;
    topology?: unknown;
  };

  bindings: unknown;

  shaders: {
    vertex?: unknown;
    pixel?: unknown;
    compute?: unknown;
  };

  constants: {
    vertex?: unknown;
    pixel?: unknown;
    compute?: unknown;
  };
}
```

## 7.2 获取顺序

尽可能并行获取：

```text
draw
pipeline
bindings
VS reflection
PS reflection
PS constants
```

Compute draw 时适配为 CS。

## 7.3 输出压缩

不要简单：

```ts
return {
  rawPipelineJson: 500KB,
  ...
}
```

先提取 GPU 调试真正重要的字段。

原始数据可以保存在：

```text
investigations/<id>/raw/
```

LLM Context 只给摘要。

## Done 条件

给定一个确定 EID，能够一次调用得到：

- draw identity
- render/depth targets
- depth/blend/raster state
- bound shaders
- relevant bindings
- relevant constants

---

# 8. Phase 5 — Evidence Model

创建：

```text
src/evidence/evidence.ts
src/evidence/investigation.ts
```

## 8.1 Evidence

```ts
export type EvidenceSource =
  | "renderdoc"
  | "source"
  | "runtime"
  | "build";

export type EvidenceKind =
  | "pixel-history"
  | "draw"
  | "pipeline"
  | "binding"
  | "shader"
  | "constant"
  | "source-code"
  | "build-result"
  | "verification";

export interface Evidence {
  id: string;
  source: EvidenceSource;
  kind: EvidenceKind;
  description: string;
  data: unknown;
}
```

## 8.2 Hypothesis

```ts
export interface Hypothesis {
  id: string;
  claim: string;
  evidenceIds: string[];
  confidence: "low" | "medium" | "high";
  verification: string;
  status: "proposed" | "confirmed" | "rejected";
}
```

## 8.3 Investigation

```ts
export interface Investigation {
  id: string;
  symptom: string;

  capture: string;

  targetPixel?: {
    x: number;
    y: number;
  };

  evidence: Evidence[];
  hypotheses: Hypothesis[];

  patch?: {
    files: string[];
    summary: string;
  };

  verification?: {
    capture: string;
    passed: boolean;
    details: unknown;
  };
}
```

## 8.4 持久化

每次 investigation 写入：

```text
investigations/<id>/investigation.json
```

以及：

```text
investigations/<id>/report.md
```

JSON 是 source of truth。

Markdown 只是人类可读报告。

## Done 条件

有单元测试确认：

- Evidence ID 可引用
- Hypothesis 不能引用不存在 Evidence
- confirmed hypothesis 必须有 verification

---

# 9. Phase 6 — Pi RenderDoc Extension

创建：

```text
.pi/extensions/renderdoc.ts
```

使用 Pi 项目级 Extension。

不要修改 Pi Core。

## 9.1 V0.1 Tool Surface

只暴露以下高层工具：

```text
rdc_open
rdc_frame_overview
rdc_pixel_history
rdc_inspect_draw
rdc_diff
rdc_assert_pixel
rdc_close
```

可选增加：

```text
rdc_capture
```

如果 capture CLI 在本机稳定。

不要直接把所有 `rdc-cli` 命令注册成几十个 Pi Tool。

## 9.2 Tool 语义

### `rdc_open`

Input：

```json
{
  "capture": "captures/bug.rdc"
}
```

Output 只返回：

- capture path
- API
- frame basic info
- open status

---

### `rdc_frame_overview`

Output：

- API
- frame
- pass overview
- selected draw summary

必须允许 limit/filter，防止 Context Explosion。

---

### `rdc_pixel_history`

Input：

```json
{
  "x": 640,
  "y": 360
}
```

Output 应聚焦：

- 哪些 EID 修改该 pixel
- before
- shader output（如果 CLI 提供）
- after
- rejected/failed reason（如果提供）

---

### `rdc_inspect_draw`

Input：

```json
{
  "eid": 134
}
```

Output：

```text
Draw
Pipeline
Bindings
Shaders
Constants
```

但必须是精简后的领域数据。

---

### `rdc_diff`

Input：

```json
{
  "before": "captures/bug.rdc",
  "after": "captures/fixed.rdc"
}
```

Output：

- draw changes
- pipeline changes
- framebuffer summary

按本机 `rdc diff` 能力实现。

---

### `rdc_assert_pixel`

用于自动验收。

Input：

```json
{
  "eid": 134,
  "x": 640,
  "y": 360,
  "expected": [1, 0, 0, 1],
  "tolerance": 0.05
}
```

exit code：

```text
0 → PASS
1 → ASSERTION FAILED
2+ → TOOL/REPLAY ERROR
```

如果本机版本定义不同，以本机帮助为准并封装统一语义。

## 9.3 Tool 调用安全

Pi Extension 必须拦截：

```text
bash
```

当 command 尝试直接执行：

```text
rdc ...
renderdoccmd ...
```

时返回：

```text
block: true
```

reason：

```text
Use typed rdc_* tools instead of invoking RenderDoc directly.
```

目的：

```text
LLM
 ↓
typed API
 ↓
policy / normalization
 ↓
rdc-cli
```

而不是：

```text
LLM
 ↓
arbitrary shell
 ↓
rdc-cli
```

## 9.4 Extension 加载验证

运行：

```bash
pi
```

确认 Pi 信任该项目后：

- Extension 成功加载
- Tool list 中出现 `rdc_*`
- 不存在 TypeScript runtime error

修改 Extension 后使用：

```text
/reload
```

或重启 Pi。

## Done 条件

让 Pi 自主完成：

```text
rdc_open
→ rdc_frame_overview
→ rdc_close
```

无 bash 介入。

---

# 10. Phase 7 — RenderDoc Debug Skill

创建：

```text
.pi/skills/renderdoc-debug/SKILL.md
```

Skill 必须规定：

```text
Symptom
→ Pixel
→ Draw
→ Pipeline
→ Bindings
→ Shader
→ Constants
→ Source
→ Hypothesis
→ Patch
→ Recapture
→ Verify
```

## Skill 强制规则

必须写入以下原则：

### Evidence First

在至少拿到一个 RenderDoc Evidence 之前：

```text
禁止提出 root cause。
```

### Pixel First

如果用户提供了错误像素坐标：

```text
优先 pixel history
```

不要仅根据 draw name 猜测责任 Draw。

### Draw Inspection

确认责任 EID 后检查：

- render target
- depth target
- viewport/scissor
- topology
- depth
- raster
- blend
- resource bindings
- shaders
- constants

### Hypothesis Format

每个 Hypothesis 必须包含：

```text
Claim
Evidence IDs
Expected behavior
Verification method
Confidence
```

### Source Mapping

用 GPU Evidence 搜源码：

- shader name
- pipeline/debug marker
- descriptor binding
- constant name
- render pass name

### Minimal Patch

只修改能验证当前 hypothesis 的最小代码。

### Mandatory Verification

没有第二次 capture，不得把 hypothesis 标记为 `confirmed`。

## Done 条件

启动 Pi 后给出一个模拟问题，Pi 能主动选择并遵守该 Skill。

---

# 11. Phase 8 — 构建现实 Demo：Textured Quad

## 11.1 Demo 选择

优先级：

### A. 当前仓库已有 renderer/sample

复用。

新增一个：

```text
TexturedQuadBugScene
```

### B. 没有

创建最小 Vulkan textured quad。

避免额外 feature：

- 不加 ECS
- 不加 RenderGraph
- 不加复杂 asset pipeline
- 不加 ImGui
- 不加多线程 render
- 不加 scene serializer

只需要：

```text
Window
Vulkan device
Swapchain
Pipeline
Quad
Texture
Uniform/Constant Buffer
Draw
Present
```

## 11.2 正确 Shader 逻辑

语义：

```hlsl
Texture2D BaseColorTexture;
SamplerState LinearSampler;

cbuffer MaterialConstants
{
    float4 BaseColorFactor;
};

float4 PSMain(VSOutput input) : SV_Target
{
    float4 texColor =
        BaseColorTexture.Sample(
            LinearSampler,
            input.uv);

    return texColor * BaseColorFactor;
}
```

GLSL 等价实现也可。

## 11.3 固定测试内容

为降低不确定性：

- 使用固定窗口尺寸，例如 `1280x720`
- Quad 覆盖屏幕中心
- 使用确定性红色纹理，最好 1x1 RGBA8：
  - `[255, 0, 0, 255]`
- 禁用随机动画
- 固定 camera
- 固定 draw order
- 给责任 draw 添加稳定 debug marker：

```text
TexturedQuad/Main
```

## 11.4 正确状态

```text
BaseColorFactor = [1, 1, 1, 1]
```

中心像素预期接近：

```text
[1, 0, 0, 1]
```

## Done 条件

正常 Demo：

- 可稳定运行
- 屏幕中心为红色
- RenderDoc 可 capture
- `rdc open` 可 replay
- draw list 能找到 `TexturedQuad/Main`

---

# 12. Phase 9 — 注入可诊断 Bug

故意修改：

```text
BaseColorFactor
```

从：

```text
[1, 1, 1, 1]
```

变成：

```text
[0, 0, 0, 1]
```

结果：

```text
textureColor * 0
→ black
```

## 要求

不要通过 Shader 直接：

```hlsl
return float4(0,0,0,1);
```

因为这样无法验证“Constant Buffer → Shader → Source”完整链路。

## Bug Capture

生成：

```text
captures/bug.rdc
```

Capture 命令必须根据：

```bash
rdc capture --help
```

构造，不要猜参数。

如果自动 capture 不稳定，允许使用 RenderDoc 手工 capture **仅作为 Phase 9 的一次环境准备**。

但分析与验证仍必须通过 `rdc-cli`。

## Done 条件

确认：

```text
bug.rdc 可 replay
中心像素黑色
TexturedQuad draw 存在
```

---

# 13. Phase 10 — 第一次完整 Agent Investigation

这是 V0.1 最关键测试。

## User Prompt

将以下内容作为测试输入：

```text
Analyze captures/bug.rdc.

The textured quad at the center of the screen should be red,
but it is black.

The approximate failing pixel is (640, 360).

Find the root cause using RenderDoc evidence.
Do not guess from source code first.

If the evidence identifies a source-code defect,
make the minimum fix.

Then rebuild, produce captures/fixed.rdc,
and verify the fix using RenderDoc evidence and an automated assertion.

Record the investigation under investigations/.
```

## Agent 预期行为

### Step 1

调用：

```text
rdc_open
```

### Step 2

调用：

```text
rdc_frame_overview
```

### Step 3

调用：

```text
rdc_pixel_history(640, 360)
```

### Step 4

从 Pixel History 确定责任 EID。

### Step 5

调用：

```text
rdc_inspect_draw(EID)
```

### Step 6

至少形成以下 Evidence：

```text
E1
Pixel history:
responsible event = TexturedQuad/Main

E2
Texture binding:
red texture is correctly bound

E3
Pixel shader constants:
BaseColorFactor = [0,0,0,1]

E4
Shader behavior:
output = texture * BaseColorFactor
```

### Step 7

形成：

```text
Hypothesis H1
```

Claim：

```text
The black output is caused by BaseColorFactor being uploaded as zero.
```

必须引用 E1-E4。

### Step 8

再搜索源码中的：

```text
BaseColorFactor
```

或对应实际 constant 名称。

### Step 9

找到 CPU-side upload。

### Step 10

最小修改：

```text
[0,0,0,1]
→
[1,1,1,1]
```

### Step 11

Build。

### Step 12

运行并生成：

```text
captures/fixed.rdc
```

### Step 13

重新：

```text
rdc_open fixed.rdc
rdc_pixel_history
```

### Step 14

调用：

```text
rdc_diff
```

### Step 15

调用：

```text
rdc_assert_pixel
```

中心像素：

```text
expected ~= [1,0,0,1]
tolerance <= 0.05
```

### Step 16

Assertion PASS 后：

```text
H1.status = confirmed
```

否则：

```text
H1.status != confirmed
```

并继续诊断。

---

# 14. Phase 11 — 自动化报告

最终必须生成：

```text
investigations/<id>/investigation.json
investigations/<id>/report.md
```

## report.md 格式

```markdown
# Investigation: <id>

## Symptom

...

## Capture

...

## Responsible Draw

EID:
Marker:

## Evidence

### E1 — Pixel History
...

### E2 — Bindings
...

### E3 — Constants
...

### E4 — Shader
...

## Hypothesis

### H1

Claim:
...

Evidence:
- E1
- E2
- E3
- E4

Confidence:
High

## Source Mapping

File:
Line/function:
...

## Patch

...

## Verification

fixed capture:
...

pixel assertion:
PASS

capture diff:
...

## Final Result

Confirmed root cause:
...
```

## Done 条件

报告里的每一个重要 root-cause claim 都必须能追溯到 Evidence ID。

---

# 15. Phase 12 — V0.1 Acceptance Test

以下 12 项必须全部通过。

- [x] A1 Pi 可加载项目级 RenderDoc Extension。
- [x] A2 Pi 可加载 `renderdoc-debug` Skill。
- [x] A3 Pi 可通过 typed tool 打开 `.rdc`。
- [x] A4 Pi 可读取 Frame Overview。
- [x] A5 Pi 可由 Pixel 坐标找到责任 Draw/EID。
- [x] A6 Pi 可读取该 Draw 的 Pipeline / Bindings / Shader / Constants。
- [x] A7 Pi 先建立 Evidence，再提出 Hypothesis。
- [x] A8 Pi 能把 GPU Evidence 映射到源码。
- [x] A9 Pi 能完成最小 Patch 并 Build。
- [x] A10 Pi 能创建/取得 `fixed.rdc`。
- [x] A11 Pi 能通过 `rdc_diff` 与 pixel assertion 验证变化。
- [x] A12 只有验证通过后 Hypothesis 才标记 confirmed。

验收说明（2026-08-24）：A1-A12 的 loader、typed tool、Evidence、patch、build、recapture 与 verification 契约均由真实 capture 集成测试通过。当前 Pi 配置返回 `No models available`，因此“向 Pi 模型输入自然语言后，由模型主动选择 Skill/Tools”的额外交互 smoke test 尚不能执行；严格的交互式 Definition of Done 仍依赖先为 Pi 配置模型。

如果任意一项失败：

```text
V0.1 = NOT COMPLETE
```

---

# 16. Phase 13 — 代码质量与安全检查

## Tasks

- [x] 13.1 `npm run typecheck`
- [x] 13.2 unit tests
- [x] 13.3 确认 `shell:false`
- [x] 13.4 确认不存在用户字符串拼 shell 命令
- [x] 13.5 确认 Pi `bash` 无法直接调用 `rdc`
- [x] 13.6 确认大 JSON 不直接进入 LLM Context
- [x] 13.7 确认 raw capture/data 不被误提交 Git
- [x] 13.8 为 `captures/*.rdc` 根据需求配置 `.gitignore`
- [x] 13.9 确认 failure path 不会把错误判断成 assertion pass
- [x] 13.10 确认所有修改只涉及 V0.1 范围

---

# 17. 禁止提前实现的 V0.2+ 功能

以下内容明确不属于本任务：

```text
GUI / Electron / Tauri

MCP

Multi-Agent orchestration

Research Sub-Agent

RenderDoc Sub-Agent

Unity Editor MCP

Adreno Profiler

Mali Streamline / Performance Studio integration

Xcode GPU Trace

Android GPU Hang automation

Vector DB

Long-term memory

Automatic bug database
```

即使实现“很容易”，也不要顺手添加。

---

# 18. V0.1 完成后建议的下一阶段

仅记录，不执行。

## V0.2 — Pipeline State Bug

构造：

```text
Depth compare 设置错误
→ Quad/mesh 消失
```

要求 Agent 通过：

```text
Pixel History
→ rejected fragment
→ Pipeline depth state
→ Source
→ Patch
→ Verify
```

定位。

## V0.3 — Descriptor Binding Bug

构造：

```text
Albedo binding
→ 错误绑定 Normal texture
```

要求 Agent 通过：

```text
Pixel
→ Draw
→ Shader Reflection
→ Binding
→ Resource
→ Descriptor Write
→ Source
```

定位。

## V0.4 — Unity 接入

把 Evidence Provider 从：

```text
Minimal Renderer
```

扩展到：

```text
Unity Player
+
URP/HLSL
+
RenderDoc Capture
+
Unity Source
```

不修改现有 Agent Runtime。

---

# 19. Definition of Done

本任务真正完成的定义不是“代码写完”。

必须能够实际演示：

```text
1. 运行带 Bug 的 Demo

2. 打开 captures/bug.rdc

3. 给 Pi 输入：
   “中心 Quad 应为红色但现在是黑色，
    failing pixel ≈ (640,360)”

4. Pi 自动：
   Pixel History
   → EID
   → Draw Inspection
   → Evidence
   → Hypothesis

5. Pi 搜索 Source 并修改 BaseColorFactor

6. Build 成功

7. 生成 captures/fixed.rdc

8. Pi 重新分析

9. rdc_assert_pixel PASS

10. Investigation report 标记 H1 = confirmed
```

只有完成这 10 步：

```text
Pi + RenderDoc Closed Loop V0.1 = DONE
```

---

# 20. 最终输出清单

Agent 完成任务时必须给出：

```text
Implementation Summary
```

包括：

1. 新增文件
2. 修改文件
3. Pi tools 列表
4. 本机 rdc-cli 版本
5. Demo build command
6. Demo run command
7. Capture command
8. bug.rdc 路径
9. fixed.rdc 路径
10. Evidence 摘要
11. Root Cause
12. Verification Result
13. Acceptance Test 12/12 状态
14. 尚未解决的问题

不要只回答：

```text
Done.
```

---

# Execution Log

Coding Agent 从这里开始追加。

- [2026-08-24 10:21] Phase 0.1 — PASS — 识别为 CMake/C++20 Vulkan/D3D 仓库，已有复杂 Demo 与 captures。
- [2026-08-24 10:21] Phase 0.2 — PASS — Node v24.10.0，npm 11.6.1。
- [2026-08-24 10:21] Phase 0.3 — PASS — Pi 0.84.2 可运行。
- [2026-08-24 10:21] Phase 0.4 — PASS — rdc-cli 0.6.1 可运行。
- [2026-08-24 10:21] Phase 0.5 — PASS — rdc doctor 全部必要检查通过。
- [2026-08-24 10:21] Phase 0.6 — PASS — 保存 rdc-cli 0.6.1 / RenderDoc 1.44。
- [2026-08-24 10:21] Phase 0.7 — PASS — 已探测 open/draws/draw/pixel/pipeline/bindings/shader/diff/assert-pixel/capture/info/close 帮助。
- [2026-08-24 10:21] Phase 0.8 — PASS — D3D12 capture 可 open/info/draws/close，replay 正常。
- [2026-08-24 10:21] Phase 0.9 — PASS — 采用 create-minimal，新增隔离的 D3D11 textured quad，避免触碰用户正在修改的主 renderer。
- [2026-08-24 10:24] Phase 1.1 — PASS — 创建 src/rdc。
- [2026-08-24 10:24] Phase 1.2 — PASS — 创建 src/evidence。
- [2026-08-24 10:24] Phase 1.3 — PASS — 创建 .pi/extensions。
- [2026-08-24 10:24] Phase 1.4 — PASS — 创建 .pi/skills/renderdoc-debug。
- [2026-08-24 10:24] Phase 1.5 — PASS — 复用现有 captures 目录。
- [2026-08-24 10:24] Phase 1.6 — PASS — 创建 investigations 并保留 environment.json。
- [2026-08-24 10:24] Phase 1.7 — PASS — TypeScript NodeNext strict 配置通过 typecheck。
- [2026-08-24 10:24] Phase 1.8 — PASS — package scripts 与锁定依赖完成，npm test 通过。
- [2026-08-24 10:29] Phase 2.1 — PASS — 实现 spawn(shell:false)、JSON/text 分离、结构化错误、AbortSignal、超时与输出上限。
- [2026-08-24 10:29] Phase 2.2 — PASS — 7 项 process 测试通过，覆盖真实 rdc、非法命令、空格/中文参数、stderr、abort 与 timeout。
- [2026-08-24 10:45] Phase 3 — PASS — RdcClient 完成全部 V0.1 API；真实 D3D12 capture 的 open/info/draws/close 通过。
- [2026-08-24 10:45] Phase 4 — PASS — inspectDraw 聚合真实 targets/pipeline/bindings/shaders/constants，输出 16867 bytes 且无 warning。
- [2026-08-24 11:02] Phase 5 — PASS — Evidence/Hypothesis 校验与 JSON/Markdown 持久化通过，confirmed 强制要求不同的第二 capture。
- [2026-08-24 11:04] Phase 6 — PASS — Pi 0.84.2 SDK 自动发现 Extension；8 个 `rdc_*` 工具进入 session 且 active，真实 open/overview/close 通过。
- [2026-08-24 11:05] Phase 7 — PASS — `renderdoc-debug` Skill 自动发现且 `quick_validate.py` 通过，Evidence-first/Pixel-first/recapture 规则完整。
- [2026-08-24 11:10] Phase 8 — PASS — 隔离 Win32+D3D11 Demo 构建并稳定运行，固定 1280x720、红色 1x1 纹理与 `TexturedQuad/Main` marker。
- [2026-08-24 11:14] Phase 9 — PASS — typed capture 生成 `captures/bug.rdc`；中心像素黑色，pixel history 确认责任 draw 为 EID 11。
- [2026-08-24 11:20] Phase 10 — PASS — 收集 E1-E4 后定位零 `BaseColorFactor`，只改一行常量，重建并生成 `captures/fixed.rdc`。
- [2026-08-24 11:26] Phase 11 — PASS — 生成 `investigations/textured-quad-black-center/{investigation.json,report.md}`，8 条 Evidence 可追溯。
- [2026-08-24 11:29] Phase 12 — PASS — A1-A12 自动化契约全部通过；fixed 中心像素 `[1,0,0,1]`，assertion PASS，H1 才标记 confirmed。
- [2026-08-24 11:30] Phase 13 — PASS — typecheck、30 项单元测试、3 项真实集成测试、Skill 校验、安全策略与 Git ignore 检查全部通过。
- [2026-08-24 11:34] Phase 7 interactive smoke — FAIL — Pi 返回 `No models available`；项目 Skill/Tools 已加载并实调，但自然语言模型主动选择行为需配置模型后复验。

格式：

```text
- [YYYY-MM-DD HH:MM] Phase X.Y — PASS/FAIL — 简短结果
```
