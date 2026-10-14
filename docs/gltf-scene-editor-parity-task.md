# MGIF glTF Scene Editor 完整复刻任务书

> 状态：需求已确认；先完成 P0 reference-oracle/data-safety gate，再进入 P1-P10 实现
> 编制日期：2026-08-22
> 参考仓库：`F:\GitHub\vk_gltf_renderer`
> 参考基线：`50373c7b0ff58078727af2c2aac70e29d5e74752`
> 目标仓库：`G:\MGIF`
> MGIF 基线：`6c36567625b305286755a734a66188a22df18e77`
> 最终歧义分数：`0.10`，通过规格门槛 `<= 0.20`
> 需求数量：24 条锁定需求

## 1. 文档用途

本文档是 MGIF 复刻 `vk_gltf_renderer` glTF 场景编辑器的唯一任务基线。它同时定义：

- 产品范围：要复刻什么、不复刻什么；
- 行为合同：用户执行同一操作后应看到什么、得到什么文件；
- 架构边界：怎样接入 MGIF，而不破坏当前 RHI 和渲染架构；
- 实施顺序：每个工作包的依赖、产物、文件和完成条件；
- 验收方法：自动测试、人工交互、视觉对照、平台和性能门槛；
- 失败边界：空场景、无效索引、循环层级、保存失败、异步竞态等必须怎样处理。

实现者不得把“窗口已经画出来”视为完成。只有编辑后的权威 glTF 文档、运行时渲染、撤销/重做状态以及重新加载后的保存结果四者一致，才算完成。

## 2. 已锁定决策

| 决策 | 锁定结论 |
|---|---|
| 功能范围 | 完整覆盖参考项目 `Scene Asset Editor` 的能力，而不是只做最小核心子集 |
| 平台范围 | 桌面编辑器保持 RHI 后端中立；Windows Vulkan 做完整运行验收；D3D12/Metal 做公共合同和编译验证；Android 不提供桌面编辑 UI |
| UI 复刻标准 | 布局、菜单、快捷键、上下文菜单、交互状态和操作结果与参考项目一致；颜色、字体、间距等视觉主题融入 MGIF |
| 行为基线 | 以参考提交 `50373c7` 的可观察行为、用户指南和自动测试为准 |
| 架构优先级 | 行为等价不等于逐文件照搬；MGIF 的 RHI 边界、句柄体系、延迟销毁和 GPU-driven 数据路径必须保留 |
| Editor 依赖边界 | MGIF production editor UI 只使用仓库已有的 upstream Dear ImGui API，以及 MGIF 自有的 app/RHI/scene adapter；不得把 `nvpro_core`/`nvcore`/`nvapp`/`nvgui`/`nvvk*`/`nvvkgltf` 或参考 Vulkan helper 作为 include、link、wrapper、public API、target 或传递生产依赖 |
| 保存语义 | “非破坏性”表示源文件在显式 `Save As` 成功前不被修改，未知扩展及未编辑数据不能被静默丢弃；参考提交没有单独的 `Save` 命令 |

## 3. 最终目标与完成定义

### 3.1 目标

MGIF 在桌面端加载 `.gltf` 或 `.glb` 后，用户可以通过与参考项目一致的 Scene Browser、Inspector、菜单、快捷键、上下文菜单和 RHI 变换 Gizmo，对场景层级、节点、primitive、材质、相机、`KHR_lights_punctual` 灯光、可见性和场景级变换进行非破坏性编辑，并可撤销/重做、合并、压缩和保存为语义等价且可重新加载的 glTF/GLB。

### 3.2 “完美复刻”的可判定含义

必须同时满足以下四类等价：

1. **交互等价**：窗口布局、命令入口、快捷键、禁用状态、确认框、拖放目标、焦点和选择传播与参考项目一致。
2. **文档等价**：相同初始文件与相同编辑序列产生相同的 glTF 语义；允许 JSON 字段排版、浮点文本格式和资源排列存在非语义差异。
3. **渲染等价**：变换、材质、灯光、可见性和层级变化在下一可提交帧正确反映；撤销与重做也必须恢复对应画面。
4. **生命周期等价**：加载、替换、合并、Compact、保存失败、撤销分叉和连续拖拽遵守参考项目的历史与状态规则。

逐像素复制参考项目主题不是目标；MGIF 现有暗色主题、Docking 风格和中央视口构图保持不变。

### 3.3 最终通过条件

以下条件全部通过后，任务才可标记完成：

- 24 条锁定需求全部有自动或人工验收证据；
- 参考功能矩阵没有 `Missing` 或未解释的 `Partial`；
- 所有 CPU 测试、round-trip 测试、RHI 合同测试和 Windows Vulkan 编辑器测试通过；
- Windows Vulkan 在开启 Validation Layer 的完整编辑脚本中无 validation error、assert、崩溃或资源生命周期错误；
- D3D12 与 Metal 的公共编辑器/Overlay 接口完成编译验证，不出现 Vulkan 类型泄漏；
- Android 构建不编译桌面编辑器 UI，且现有运行路径无回归；
- 保存后重新加载的场景与保存前编辑文档满足语义、层级、材质、灯光和渲染对照；
- `tools/check_rhi_boundaries.ps1` 及现有 RHI 守卫继续通过；
- fail-closed editor dependency scan 覆盖现有及未来 production editor 路径、editor public API、target include/link 声明和传递生产依赖，且没有任何未批准的 nvpro/reference-helper 命中；
- `MinimalLatestApp.h` 不再承载完整编辑器业务实现，只保留应用组装与生命周期协调。

## 4. 事实基线

### 4.1 参考实现证据

| 能力 | 参考文件 | 关键事实 |
|---|---|---|
| 权威 glTF 场景与脏标记 | `src/gltf_scene.hpp`, `src/gltf_scene.cpp` | `tinygltf::Model` 为权威状态；维护节点世界矩阵、render node 注册表、材质/灯光/节点脏集合和全量重建标志 |
| 场景编辑 API | `src/gltf_scene_editor.hpp`, `src/gltf_scene_editor.cpp` | 节点新增、深复制、递归删除、重挂、TRS、资源挂接、材质操作、可见性、结构快照与索引重映射 |
| 撤销/重做 | `src/undo_redo.hpp`, `src/undo_redo.cpp` | 线性命令栈、100 条上限、500 ms 连续编辑合并、分叉清空 redo、结构快照 |
| 统一选择与材质剪贴板 | `src/scene_selection.hpp`, `src/scene_selection.cpp` | Node/Primitive/Material/Mesh/Camera/Light 单一选择上下文及事件回调；OS clipboard 格式为 `gltf_material:<index>` |
| Scene Browser | `src/ui_scene_browser.hpp`, `src/ui_scene_browser.cpp` | Asset Info、层级、平铺列表、Debug、拖放重挂、上下文菜单、重命名、焦点同步 |
| Inspector | `src/ui_inspector.hpp`, `src/ui_inspector.cpp` | 节点/primitive/mesh/material/camera/light 上下文属性，全套材质扩展和连续编辑快照 |
| 变换 Gizmo | `src/gizmo_transform_vk.hpp`, `src/gizmo_transform_vk.cpp` | 自研 translate/rotate/scale 组合 Gizmo、CPU 射线命中、父空间变换、屏幕恒定尺寸、snap |
| Gizmo 视觉聚合 | `src/gizmo_visuals_vk.*`, `src/gizmo_grid_vk.*` | 网格、Gizmo 与场景深度组合，在 tonemap 后、UI 前绘制 |
| UI、拾取与快捷键接线 | `src/ui_renderer.cpp`, `src/ui_mouse_state.hpp`, `src/renderer.cpp` | File/Edit/View/Windows/Tools 菜单、single/double viewport ray picking、Gizmo 输入优先级、Escape 清选、历史刷新、保存/合并/Compact |
| 保存、合并、Compact、验证 | `src/gltf_scene.cpp`, `src/gltf_scene_merger.*`, `src/gltf_compact_*`, `src/gltf_scene_validator.*` | 保存前严格验证、外部图像处理、跨模型索引重映射、孤儿资源清理 |
| 回归测试 | `tests/test_roundtrip.cpp`, `test_model_primary.cpp`, `test_index_remapping_*`, `test_dirty_flags_and_render_nodes.cpp`, `test_node_hierarchy_operations.cpp`, `test_children_order_preservation.cpp`, `test_material_cache.cpp`, `test_error_paths.cpp` | 参考实现已有可移植的编辑语义和失败路径测试集合 |

参考用户可见合同以 `docs/user-guide.md` 的 `Scene Asset Editor` 章节为准。源代码与文档冲突时，先用参考可执行程序复现；可执行行为优先，冲突记录到 parity manifest，禁止凭印象选择。

### 4.2 MGIF 已有能力

| 现状 | MGIF 证据 | 可复用部分 |
|---|---|---|
| Scene Graph 树和选中节点 | `app/MinimalLatestApp.h:3118` | 已有 ImGui Docking、树递归和基本选中状态 |
| 节点 TRS 数值编辑 | `app/MinimalLatestApp.h:3252` | 已有 translation/rotation/scale 控件和世界位置展示 |
| 变换增量更新 | `app/MinimalLatestApp.h:3340`, `render/RendererFacade.cpp:117` | legacy mesh 与 experimental instance 均已有变换更新入口 |
| 渲染中立场景投影 | `scene/SceneAsset.h`, `scene/SceneAssetView.h` | `SceneAsset` 已包含层级、材质、相机、灯光及 GPU-ready payload |
| glTF 载入 | `loader/GltfLoader.cpp:486` | tinygltf、KTX2 替换、依赖跟踪和安全数量上限可复用 |
| 灯光编辑和渲染 | `app/MinimalLatestApp.h:2028`, `render/GPUDrivenRenderer.cpp:5276` | point/spot/directional 数据已进入每帧灯光路径 |
| 场景缓存与异步上传 | `scene/SceneAssetSerializer.*`, `scene/ParallelSceneLoader.*`, `render/AsyncLoadingCoordinator.*` | 可继续作为运行时缓存和上传层 |
| RHI 与 ImGui | `render/ImGuiRhiRenderer.*`, `rhi/*` | Gizmo 必须复用的后端中立渲染能力 |

### 4.3 MGIF 关键缺口

1. `GltfLoader` 在局部 `tinygltf::Model` 上读取后只保留有损的 `GltfModel`；primitive 结构、未知扩展、extras、skin、animation 等不能完整保存。
2. `GltfModel` 与 `SceneAsset` 两条加载/编辑路径并存，UI 对两套结构分别分支，没有单一编辑权威。
3. 当前节点身份是可移动 vector index；删除、Compact 和 Merge 后不足以支撑稳定选择与命令引用。
4. Scene Graph 只有浏览和 TRS，缺少新增、复制、删除、重挂、重命名、平铺列表和上下文选择。
5. 没有命令系统、事务边界、dirty revision、unsaved 状态或撤销/重做。
6. 没有保存 glTF/GLB、合并、Compact 和保存前验证闭环；`.sceneasset` 只是运行时缓存，不能代替源文档。
7. 运行时只有变换更新入口；缺少材质更新、visibility、资源重新指派和拓扑重建合同。
8. 没有 viewport picking、渲染高亮和 Transform Gizmo。
9. 灯光控件直接改 vector，未进入命令栈，也没有新增/删除灯光节点。
10. 编辑器业务集中在约 3,000 行以上的 `MinimalLatestApp.h`，继续堆功能会使状态和帧生命周期不可验证。

## 5. 范围

### 5.1 范围内

- lossless glTF/GLB 编辑文档的加载、验证、保存与 round-trip；
- Scene Browser 顶部的 Asset Info、条件显示的 Animation/Material Variants 折叠区，以及 Scene Graph、Scene List、Debug-build-only Debug 三个 tab；
- Node、Primitive、Mesh、Material、Camera、Light 的统一选择与 Inspector；
- 节点新增、深复制、递归删除、重命名、append-only 跨父/root 拖放重挂，以及参考的同父 drop no-op 历史副作用；
- 数值 TRS、场景级根变换和 RHI Transform Gizmo；
- Gizmo 网格、显示开关、snap、hover、drag、父空间和输入优先级；
- metallic-roughness、specular-glossiness、纹理槽、alpha、double-sided 和参考项目暴露的全部材质扩展；
- primitive 材质 Assign、Copy/Paste、Duplicate、Split、Merge；
- perspective/orthographic 相机属性，以及 Apply to Current View / Set from Current View 双向同步；
- `KHR_lights_punctual` 三类灯光的新增、选择、变换、编辑、删除和保存；
- `KHR_node_visibility` 的显示、继承、运行时同步和保存；
- 参考实现标记为 undoable 的命令完整撤销/重做；
- New/Open/Open Recent、拖放替换、Shift+拖放合并、每次均打开原生对话框的 Save As、Compact；
- 合并后的资源、节点、skin、animation channel、camera、light 和 extension 索引重映射；
- Compact 对不可达 mesh/material/texture/image/accessor/bufferView/buffer/sampler/skin/camera/animation/light 的清理；
- 只读展示 glTF asset metadata、texture、image、animation 等列表；
- animated scene 顶部的 animation 选择、播放/暂停、单步、Reset、speed 和 timeline scrub；编辑 animation 驱动的 transform 时必须按 5.3 的冲突规则处理；
- `KHR_materials_variants` 数据保留和列表/选择接线；运行时已有能力不足时作为单独受测增量实现；
- Settings 面板中条件显示的 `Multiple Scenes` 折叠区、scene 单选列表和 current-scene 运行时切换；
- document revision/clean revision、错误提示、busy/disabled 状态和最近文件；严格 parity 模式不增加参考中不存在的 unsaved-change 提示；
- Windows Vulkan 完整验收、D3D12/Metal 合同与编译验证、Android 编辑器隔离。

### 5.2 范围外

- 逐像素复制 NVIDIA 参考项目主题；MGIF 视觉主题是已锁定约束。
- 复刻参考项目的 path tracer、RTX、DLSS、OptiX、环境、tonemapper、memory monitor 或其他非编辑器渲染功能。
- 新增动画 authoring、关键帧编辑、曲线编辑器或骨骼蒙皮编辑；本任务只要求动画数据保留、结构操作索引修复、列表展示、参考 playback controls 以及编辑期间的安全协作。
- 修改 mesh 顶点、索引、UV、skin weights 或 morph target 几何内容；primitive 只做资源指派和复制关系编辑。
- Android 触屏场景编辑器；Android 只需不受桌面代码影响。
- 在本任务内补齐 D3D12/Metal 的完整运行时渲染后端；只验证编辑器公共合同没有绑定 Vulkan。
- 多文档同时编辑、多人协作、云保存、插件系统和脚本语言。
- 自动把 `.sceneasset` 写回 glTF；它继续是可失效、可重建的运行时缓存。

### 5.3 参考实现已知限制的处理

- 参考提交中 visibility toggle 和 scene-level transform 不在 UndoStack 内。本轮以严格行为等价为最低门槛；不得把它们错误宣称为可撤销。若实现时选择增强，必须单独记录为 MGIF extension，并增加测试，不得改变参考快捷键或其他命令语义。
- 参考提交的 root-level Add Node、mesh/material rename、camera edit、material duplicate/Assign/Copy/Paste/Split/Merge、variant/animation controls 也不进入 UndoStack；MGIF 必须按 6.5 的 exact matrix 呈现历史，不得因为内部统一使用 transaction 就把它们显示为可 Undo。
- 参考提交的 New/Open/Exit 没有 Save/Discard/Cancel 提示，Compact 也不弹确认框。为满足严格 UI parity，本任务不新增这些 modal；内部 revision 仍用于保存正确性和测试。若未来需要 unsaved guard，必须作为单独、经用户批准的 MGIF extension 变更本规格。
- 动画播放可能在下一帧覆盖刚恢复的节点变换。MGIF 至少要在编辑动画驱动属性时暂停对应播放或给出明确状态，不能静默制造“撤销失效”的假象。
- 参考 Scene Browser 某些上下文菜单仅展示入口，实际 material assign 主要由 Inspector 完成。MGIF 的验收以可工作的 Inspector 流程为准，不要求复制无效占位操作。

### 5.4 严格 parity 与 MGIF 内部安全增强的边界

严格 parity 约束所有正常成功路径，以及参考提交已经定义的失败路径：窗口布局、菜单/控件顺序、label、快捷键、上下文菜单、tooltip、focus、enablement、modal、选择、历史计数、文档结果和渲染结果都不得因“安全增强”而改变。用户可见命令只有参考中的 `Save As`；本文出现的内部 `save`/writer 名称均指该命令调用的持久化管线，不代表新增 `Save` 菜单或不经对话框覆盖当前路径。

允许且要求的增强只位于不可见实现层或参考未定义的灾难失败边界：generation ID、revision/token 校验、candidate document、原子文件发布、stale async result 丢弃、fence-safe GPU swap、typed error、debug invariant 和 telemetry。它们必须满足：

- 正常成功 action trace 与参考一致，不增加 dirty badge、Save/Discard/Cancel、Compact confirmation、额外 toolbar/menu 或快捷键。
- 失败时可以 fail closed 并保留最后有效 document/render scene，不得为了表面复刻而发布半状态、损坏旧文件或解引用 stale handle。
- malformed 外部输入可比参考更严格地拒绝，但必须是无 mutation 的 no-op/error；不得把失败伪装为成功。
- 任何新增可见安全 UI、参考已定义行为的变更或默认开启的 MGIF extension，都必须先由用户批准并修改本规格；不能在实现阶段自行扩张 parity 例外。

Material clipboard 的 malformed 数字是本条的显式实例，而不是遗漏的 parity：参考使用 `std::atoi()`，因此 `gltf_material:abc` 可能被当成 index `0`。该字符串不是参考定义的合法交换格式；MGIF 必须只接受完整的非负十进制 index，并对空数字、负号、空白、尾随字符、非数字和溢出 fail closed。对 canonical 合法字符串、合法但越界的正 index 以及跨 document 重新验证，仍须严格复刻参考状态和结果。

多 scene 的 Scene Browser 结构编辑是实现前必须关闭的参考歧义。源码在所有 scene row 上渲染相同的 root context/drop target，但 root Add/Light/drop 没有传递 row 的 `sceneID`，而是直接使用 `m_currentScene`；`m_nodeParents` 也只从 current scene 的 traversal 构建。因此不能把“点击非当前 scene row”擅自解释成编辑该 row，也不能像旧草案一样无证据地规定为 pre-command reject。P0 必须对每个 scene row 和 current/non-current node 逐项捕获 Add Node、Add Light、Duplicate、Delete、Rename、Add Child、node/root drop、Undo/Redo、scene switch 的 document/selection/history/runtime trace。若参考可执行行为会造成非当前 scene authored 数据丢失、无效层级或 Undo 无法恢复，Gate P0 必须停止并取得用户对“精确复制缺陷”或“列明后的 MGIF 数据安全 parity 例外”的明确选择；N04 禁止实现者静默选择任一语义。

## 6. 架构合同

### 6.1 单一权威模型

新增 `GltfDocument` 作为编辑器唯一可变权威：

```text
                    load / merge / save
                           |
                           v
                 GltfDocument (authoritative)
                 tinygltf::Model + stable IDs
                 path + dirty revision + history
                           |
                 project / classify changes
                           v
          SceneAsset + SceneRuntimeMapping (derived)
                           |
                 SceneChangeSet queue
                           v
        RendererFacade / GPUDrivenRenderer / RHI
```

硬性规则：

- `tinygltf::Model` 保存 glTF 语义；`SceneAsset` 只保存渲染投影。
- UI 不得直接修改 `SceneAsset`、`GltfModel` 或 renderer 容器。
- 所有可撤销修改必须通过 `SceneEditor` 和 `IEditorCommand`。
- renderer 只消费只读投影和 `SceneChangeSet`，不得反向成为编辑权威。
- `.sceneasset` 缓存命中时，编辑器仍需加载或构造可验证的 `GltfDocument`；缓存只能加速 payload 和上传。
- production editor 的 UI 直接使用本仓库已经引入的 upstream Dear ImGui API；平台、文件、场景和渲染接线只能通过 MGIF 自有 app/RHI/scene adapter，不得引入第二套 app/framework/helper abstraction。
- `scene/editor/**`、`app/editor/**`、`render/editor/**`、名称或 target/source 归属为 editor 的 public API/production target，以及其传递生产依赖，均不得 include、link、wrap、re-export、复制或要求 `nvpro_core`（含 `nvpro_core2`）、`nvcore`、`nvapp`、`nvgui`、任何 `nvvk*` family（含 `nvvk`、`nvvkhl`、`nvvkgltf`）或参考项目 Vulkan helper/target。
- 参考 executable/probe 可以在隔离的外部 evidence 环境中保留其原生 nvpro 依赖，但它们不得成为 MGIF source、target、include path、link edge 或传递生产依赖；evidence 文档中的依赖名称不是 MGIF dependency finding。
- 当前没有获批例外。任何确实无法避免的例外都必须在改动前取得用户明确批准，并同时记录最小必要范围、已评估替代方案、平台/许可证/binary-size 影响、精确 symbols/targets 以及带 owner/日期的移除计划；实现者、reviewer 或阶段 gate 都无权自行批准。
- 现有 renderer 中与 editor 无关的 `NVVK_SEMAPHORE_DEBUG` compile token 保持原状且不属于本边界；只有它进入 editor 路径、editor target 的 include/link 声明或 editor 传递生产依赖时才构成违规。

### 6.2 建议模块边界

| 模块 | 责任 | 禁止依赖 |
|---|---|---|
| `scene/editor/GltfDocument.*` | 权威 tinygltf model、路径、revision、dirty、当前 scene、稳定 ID | RHI、ImGui、Vulkan |
| `scene/editor/SceneEditor.*` | 所有模型 mutation、结构不变量、索引重映射、snapshot/restore | UI、GPU 原生类型 |
| `scene/editor/SceneProjection.*` | 从 document 构造/更新 `SceneAsset` 和双向 mapping | ImGui、平台窗口 |
| `scene/editor/SceneChangeSet.*` | 变更分类、合并、revision 与 frame-safe 消费 | UI 绘制代码 |
| `scene/editor/commands/*` | 命令栈、事务、具体命令、描述文本 | renderer 直接 mutation |
| `app/editor/SceneSelection.*` | 单一选择上下文和事件 | GPU 原生对象 |
| `app/editor/SceneBrowserPanel.*` | 通过仓库现有 upstream Dear ImGui API 绘制 Asset/Hierarchy/List/Debug UI | 直接修改模型、nvpro/nvapp/nvgui/nvvk helper |
| `app/editor/InspectorPanel.*` | 通过仓库现有 upstream Dear ImGui API 执行属性编辑、连续事务生命周期 | 直接上传 GPU、nvpro/nvapp/nvgui/nvvk helper |
| `app/editor/EditorController.*` | 文档、选择、命令、面板、文件操作和 renderer delta 编排 | 后端原生 API、reference app/helper target |
| `render/editor/TransformGizmo.*` | 后端无关 Gizmo 状态、CPU 命中与拖拽数学 | ImGui 业务状态、Vulkan、reference Vulkan helper |
| `render/passes/EditorOverlayPass.*` | RHI Gizmo/Grid 绘制、depth/x-ray 合成 | tinygltf mutation |
| `shaders/editor_*.slang` | Gizmo/Grid shader | 后端专用 binding 假设 |

文件名在 plan 阶段可按仓库命名规范微调，但责任边界不可合并回 `MinimalLatestApp.h`。

### 6.3 稳定身份与索引映射

- `EditorObjectId` 使用带 generation 的强类型句柄，不把 vector index 暴露为持久身份。
- Document 保存 `EditorObjectId <-> glTF index` 映射；Projection 保存 `EditorObjectId <-> SceneAsset index / instance index / handle` 映射。
- Delete/Compact 可改变 glTF vector index，但不得让旧 ID 指向另一个对象；旧 ID 必须失效。
- Undo restore 原对象时恢复同一逻辑 ID；Redo 再删除时再次失效，选择必须得到明确的 fallback/clear 事件。
- Merge 为导入对象分配全新 ID；相同名称不合并身份。
- ID 仅存在于编辑会话或显式 sidecar，不得为实现方便写入用户 glTF `extras`。

### 6.4 变更分级与运行时同步

| 变更级别 | 示例 | renderer 行为 |
|---|---|---|
| `Transform` | TRS、Gizmo、scene root transform | 更新受影响节点子树世界矩阵与现有 instance/object transform；禁止全量 scene rebuild |
| `Light` | color/intensity/range/cone/type | 更新 CPU light projection；下一帧灯光 buffer 刷新；必要时重置 DDGI history |
| `MaterialParameters` | factor、alpha cutoff、extension scalar | 更新 material record/GPU material data 和相关 draw record；不得重传 geometry |
| `MaterialBinding` | texture/primitive material assign | 更新 descriptor/material handle 与相关 draw classification；alpha mode 改变时重建对应 pass list |
| `Visibility` | node visibility | 更新受影响 subtree 的 draw/instance enable state；隐藏不删除资源 |
| `Hierarchy` | reparent、empty node add | 重建世界矩阵拓扑和必要 mapping；无 geometry 变化时复用 mesh/texture GPU 资源 |
| `Topology` | duplicate/delete mesh-bearing subtree、split/merge | 在 frame-safe 边界重建 draw/instance registry；旧资源延迟销毁 |
| `FullRebuild` | merge、Compact、无法增量映射的 geometry 变化 | 异步构建新投影并在安全帧原子切换；历史按合同清空 |

连续 Inspector/Gizmo 编辑不得调用 `waitForIdle()`、重建纹理或重新上传完整场景。离散结构操作可以触发较重的 rebuild，但必须通过现有 fence/延迟销毁模型安全切换，且 UI 不得看到半更新状态。

### 6.5 Undo/Redo 合同

| 操作 | 命令 | 快照策略 | 合并策略 |
|---|---|---|---|
| 节点 TRS / Gizmo | `SetTransformCommand` | old/new TRS | 同一节点、同类连续编辑在 500 ms 内合并；一次 Gizmo drag 永远只有一条命令 |
| 节点重命名 | `RenameNodeCommand` | old/new UTF-8 name | 不合并 |
| 在选中 node 下新增空 child | `AddNodeCommand` | parent、插入顺序、new ID | 不合并；scene root 的 Add Node 按参考为 non-history transaction |
| 新增灯光 | `AddLightCommand` | node/light collections 的受影响快照 | 不合并 |
| 深复制节点 | `DuplicateNodeCommand` | 原根 ID、复制结果快照与映射 | 不合并 |
| 删除节点 | `DeleteNodeCommand` | nodes、scene roots、skins、animations、lights 等结构快照；不复制大纹理/顶点 payload | 不合并 |
| 重挂节点 | `ReparentNodeCommand` | 只保存 old/new parent；参考不保存 sibling position | 不合并；same-parent 仍形成 no-op history entry |
| 材质编辑 | `EditMaterialCommand` | 完整 tinygltf material old/new | 同一材质连续控件编辑在 500 ms 内合并 |
| 灯光编辑 | `EditLightCommand` | 完整 tinygltf light old/new | 同一灯光连续控件编辑在 500 ms 内合并 |

UndoStack 上限固定为 100。新命令在 undo 之后执行必须清空 redo；New/Open/替换场景、成功 Merge、成功 Compact 和文档卸载必须清空历史。`Save As` 不清空历史，只更新 clean revision。

严格进入历史的操作只有：Node/Primitive 对应 node 的 Inspector/Gizmo transform、node rename、node subtree duplicate、node subtree delete、node child Add Node、root/node Add Light、drag reparent、material property/extension edit、light property edit。以下操作使用同样的 validate/transaction/delta 管线但**不压入 UndoStack**：root-level Add Node、mesh/material rename、camera edit/apply/set、visibility、scene transform、material duplicate/Assign/Copy/Paste/Split/Merge、animation/variant controls、文件操作、Merge、Compact。测试必须断言 non-history 操作前后 undo/redo count 不变；不得用统一命令架构改变参考可观察历史。

`DeleteNodeCommand` 的唯一根特例仍属于 history：参考会先清 selection，再由 `deleteNodeRecursive()` 拒绝删除，随后 `UndoStack::executeCommand()` 无条件清 redo 并压入命令。因此 MGIF 不能把它优化成完全无副作用的 validation failure；它必须遵守 R05 的 document no-op、selection/history side effect 以及 Undo/Redo 结果。该特例不代表其他 validation failure 也可以入栈。

`ReparentNodeCommand` 也有必须保留的可观察边界。drag payload 只有 node identity，没有 sibling insertion position；父级实际改变时，节点从旧 collection 删除并追加到新 parent children 或 current scene roots 的尾部。Undo 只调用 old parent，因此同样追加到旧 collection 尾部，不恢复 drag 前的 sibling slot；Redo 再追加到新 collection 尾部。同父 node drop 或 root-to-root drop 会在 domain 层因 `oldParent == newParent` 成为 document/revision/runtime no-op，但 UI 已创建命令，所以 redo 被清空、UndoStack 新增恰好一条 `Move '<name>'`；Undo/Redo 只在两栈间移动该命令，selection 与 document 始终不变。MGIF 不得把它实现成 sibling reorder，也不得因内部 preflight 优化掉该 history entry。

## 7. UI 与交互合同

### 7.1 默认布局

```text
+--------------------------------------------------------------------------------+
| File | Edit | View | Windows | Tools |                      FPS / busy state   |
+----------------------+--------------------------------------+------------------+
| Scene Browser        | Viewport                             | Inspector        |
| Info + Graph/List/Dbg|                                      | context-aware    |
|                      |  Grid + Transform Gizmo              | properties       |
|                      |                                      |                  |
|                      |                                      |                  |
+----------------------+--------------------------------------+------------------+
| existing MGIF panels remain dockable as tabs; no editor panel nests cards in cards          |
+--------------------------------------------------------------------------------+
```

- Scene Browser 默认停靠左侧；Inspector 默认停靠右侧；中央节点继续是无 tab bar 的实际渲染视口。
- 现有 Settings、Model Loader、FlaxDebugUI 和 profiler 保持可停靠，不得被删除；可作为左右 dock tab。
- 窗口最小尺寸必须约束，长名称使用 clip/ellipsis + tooltip，任何视口尺寸下文本和按钮不得重叠。
- 图标优先使用项目已有 Material Symbols/ImGui 图标体系；无图标时才使用短文本按钮。

### 7.1.1 Settings / Multiple Scenes

- 仅当 active glTF 的 `scenes.size() > 1` 时，在 Settings 面板的 renderer settings 分隔线之后显示 `Multiple Scenes` CollapsingHeader；单 scene、无 document 和 candidate loading 尚未 publish 时不显示。
- 展开后按 document scene index 顺序显示每个 scene name 的 RadioButton，且恰有 current scene 一项为 selected；重复名称依靠稳定 ImGui ID/index 区分，空名称的可见 fallback 以 P0 reference trace 为准。
- 选择另一项只切换 editor/runtime 的 current scene，不改写 glTF collections 或 `defaultScene`，不进入 UndoStack；随后按 MGIF frame-safe 合同重建/切换该 scene 的 projection、render nodes、textures、bounds 和 animation/variant runtime state。
- scene switch 后 selection、Gizmo attachment、camera、history 和 panel focus 的最终状态必须等于 P0 在参考可执行程序上的 action trace；在该 trace 固化前不得由实现者自行清空、保留或迁移 selection。无论具体 reference 结果为何，都不得出现旧 scene draw 与新 scene Inspector/Gizmo 的 silent mismatch。
- Scene Browser 同时展示多个 scene 时，scene row context/drop 与非当前 scene node 操作的 target/result 也必须纳入同一 P0 trace。源码预测 row target 未携带 `sceneID` 且结构 helper 依赖 current scene，但在 executable trace 固化和 5.4 的数据安全决策关闭前，不得据此实现、修正或假定 reject。

### 7.2 菜单、工具栏与快捷键

| 入口 | 命令 | 快捷键/规则 |
|---|---|---|
| File | New Scene | `Ctrl+N` |
| File | Open | `Ctrl+O` |
| File | Import/Merge Scene | 无强制快捷键；Shift+文件拖放等价 |
| File | Open Recent | 保留有效最近路径，失效路径可移除并报告 |
| File | Save As | `Ctrl+S`；每次都打开原生 Save As 对话框，与参考提交的可见标签和结果一致 |
| File | Exit | `Ctrl+Q`；直接请求关闭，不弹参考中不存在的 unsaved-change modal |
| Edit | Undo / Redo | `Ctrl+Z` / `Ctrl+Y`，菜单显示下一条命令描述 |
| Edit | Duplicate / Delete | `Ctrl+D` / `Delete`；文本输入时 Delete 不触发节点删除 |
| View | Fit Scene / Fit Object | `Ctrl+Shift+F` / `Ctrl+F`；无 scene 或无可聚焦选择时 disabled |
| View | Grid / Gizmo / Snap / Grid & Snap Settings | `G` / `T` / 无默认键 / 无默认键；只在 viewport 有焦点且 ImGui 未捕获文本时响应单键 |
| Windows | Scene Browser / Inspector | `F2` / `F4`，与参考窗口开关一致；MGIF 已有其他 panel 保持原快捷键 |
| Viewport toolbar | Grid / Gizmo / Snap | `G` / `T` / 无默认键；按钮 active 状态明确 |
| Tools | Compact Scene | `Ctrl+K`；valid scene 时立即执行，不弹参考中不存在的确认框；成功后清空可能持有旧索引的历史 |
| 全局 | Clear selection | `Escape`；按参考只清空 selection，不是 Gizmo rollback/cancel 快捷键，不恢复 drag 起始 TRS |

### 7.3 Scene Browser

- `Asset Info` 是 tab bar 上方始终存在的 CollapsingHeader，显示 asset version、generator、copyright、extensions、`KHR_xmp_json_ld` 等只读信息；它不是第四个 tab。
- `Animation` 和 `Material Variants` 也是 tab bar 上方的条件 CollapsingHeader：只在 scene 分别具有 animation 或 variant 时出现。
- Release 的 tab bar 只有 `Scene Graph`、`Scene List`；Debug build 追加带 bug icon 的 `Debug` tab。
- Scene Graph 显示 scene root、node、mesh、primitive、camera、light、skin；图标、缩进和数量与类型匹配。
- Node row 支持选择、展开、focus-on-selection、拖放 source/target 和右键菜单。Viewport 新选中 Node/Primitive 后展开其 parent path 并设置下次 Scene Graph render 的 scroll；参考提交不会通过 ImGui selected-tab flag 强制从 Scene List/Debug 切回 Scene Graph，MGIF 也不得额外切 tab。
- Scene root 是合法 drop target。父级实际改变时，节点追加到 current scene roots 尾部；node target 同样追加到新 parent children 尾部。参考没有 sibling 插入 payload，因此不支持同父 reorder；同父/root-to-root drop 的 document 不变但 history 按 6.5 变化。多个 scene row 的 target 归属以 5.4/P0 trace 为准。
- Scene List 按 Nodes、Meshes、Materials、Cameras、Lights、Textures、Images、Animations 分组，显示数量和索引。
- Nodes/Meshes/Materials/Cameras/Lights 可选择；Textures/Images/Animations 是 display-only；texture 可跳转关联 image。
- 空组显示计数为 0 或清晰 empty state，不创建虚假占位资源。
- Context menu 至少包含 Add Child、Add Point/Directional/Spot Light、Duplicate、Delete、Rename，以及参考中可工作的 mesh/material 操作。
- Debug tab 在 Debug build 显示 document/render node/render primitive 数量、expected-vs-actual mismatch、GPU sync validation 状态、render-node details，以及 `Rebuild All Render Nodes` repair action；Release build 与参考一样不显示该 tab。

### 7.4 Inspector

- 无选择时显示稳定空状态；选择变化不得残留上一个对象的 snapshot 或 Euler cache。
- Node：name、parent、children、resource attachments、TRS、visibility 和相关 light/camera section。
- Primitive：节点 TRS、primitive statistics、当前材质和 material assignment toolbar。
- Mesh：只读统计、引用关系及参考实现支持的操作。
- Material：核心 PBR、textures、normal scale、occlusion strength、emissive、alpha、double-sided、extensions、复制/指派操作。
- Camera：perspective/orthographic 类型对应字段，以及 Apply to Current View / Set from Current View。
- Light：type、sRGB 编辑/linear 存储 color、intensity、range、spot inner/outer cone。
- 数值拖拽在 `IsItemActivated` 捕获 before snapshot，在 `IsItemDeactivatedAfterEdit` 提交命令；逐帧变化只预览，不逐帧压入历史。

### 7.5 Gizmo

- Translate：X/Y/Z axis 和 XY/XZ/YZ plane。
- Rotate：X/Y/Z ring。
- Scale：X/Y/Z axis 和 center uniform scale。
- 所有操作可同时显示，点击命中的 component 决定当前 operation，与参考实现一致。
- 默认 snap：translation 等于 Grid Unit（默认 `1.0`）、rotation `45 deg`、scale `0.1`；仅在 Snap 开启时应用。Grid Unit、rotation、scale 的 UI 范围分别为 `[0.001, 1000]`、`[1, 180] deg`、`[0.001, 10]`，与参考提交的实际设置接线一致。
- Gizmo 以选中节点 world position 显示，方向使用父空间语义，保持屏幕像素尺寸近似恒定。
- hover 高亮、active 高亮、遮挡/x-ray 行为和 depth 合成可观察上等价。
- Gizmo 捕获鼠标时禁止 viewport picking 和 camera orbit/pan/zoom；从 viewport 内开始的 drag 在 pointer 离开 viewport 后仍持续，到左键 release 时 `endDrag` 并提交。
- `Escape` 保持参考的全局 clear-selection 语义，不是 Gizmo rollback：不得恢复 drag 起始 TRS，也不得声称产生 cancel transaction。Escape-only（左键仍按下）会在下一次 attachment sync 直接 detach，保留最后 preview TRS 且不产生 transform-end history；窗口失焦若输入层合成 mouse release，则只走正常 `endDrag`/commit，不建立独立 rollback 行为。
- `Escape` 与左键 release 落在同一 ImGui frame 是独立事件序列：参考源码会让 release 走 `endDrag`，因此最终 selection 清空、preview TRS 保留并产生恰好一条 transform history；P0 必须用可执行 trace 验证 callback/frame 顺序，若 runtime 与源码预测冲突，以 runtime trace 修订本条后才可实现。
- 输入处理与绘制分离，CPU picking math 不包含 Vulkan、D3D12 或 Metal 类型。

### 7.6 Viewport Picking

- 仅在 viewport hover、scene valid 且 renderer 非 busy 时接受新 pick；active Gizmo 的输入和跨 viewport drag 优先于 scene ray/object-ID picking。
- click classifier 必须与参考的 `UiMouseState` 可见时序一致：释放后等待 double-click 窗口；超时才产生 single-click，第二次点击在窗口内到达则产生 double-click，不能先提交一次 single-click selection 再执行 double-click。
- single-click 命中有效 render instance 时选择对应 Primitive；若它已经是唯一选中的同一 render node，则清空选择。命中无效 instance 或空白/environment 也清空选择。
- 新选择 Primitive 后同步 silhouette、Inspector、Gizmo attachment，并调用 Scene Browser focus 行为；Node 选择仍高亮其递归 render nodes，Primitive 只高亮目标 render node。
- double-click 命中时不执行 single-click selection toggle，而把 camera center 设置为 `rayOrigin + rayDirection * hitT`，并把 navigation speed 设置为 `hitT`；double-click 空白不改变 camera center/speed。
- MGIF 可以用 RHI-neutral object-ID/readback pass 代替参考的同步 Vulkan ray picker，但 action timing、single/double classification、selection/camera 结果必须等价。异步结果必须携带 document/projection revision；过期、已删除或已隐藏对象的结果按空白/no-op 合同处理，绝不能选中复用同 index 的对象。

## 8. 锁定需求

本节的 `Current`、`Target` 和 `Acceptance` 是后续 plan、实现和验收的权威输入。验收中的测试名是目标测试职责；实施时可按仓库命名规范微调文件名，但不可删除验证语义。任何需求变更都必须先修改本节，再调整实施计划。

### 8.1 R01：无损可编辑文档

- **Current**：`GltfLoader::load()` 在局部 `tinygltf::Model` 上解析后，只输出有损 `GltfModel`；`SceneAsset` 又进一步只保留渲染所需数据。多 primitive 关系、sampler、skin、animation、未知 extension、extras 和原始资源引用无法从当前运行时对象完整恢复。
- **Target**：加载 `.gltf`/`.glb` 时创建一个持有完整 `tinygltf::Model`、源路径、资源依赖、current scene 和编辑 revision 的 `GltfDocument`；`GltfModel`/`SceneAsset` 只能由它投影产生。未编辑字段及未知 extension/extras 在保存时保持语义不变。
- **Acceptance (PASS/FAIL)**：对包含外部 buffer/image、data URI、嵌入 GLB、多个 scene、skin、animation、sparse accessor、未知根/对象 extension 和嵌套 extras 的语料执行 load -> no-op save -> reload；glTF 语义比较器报告集合、引用和值等价，active/default scene 语义不漂移，且每个 scene 的投影都可以完成一次 Vulkan 渲染。任何字段静默丢失即 FAIL。

### 8.2 R02：稳定身份与全链路映射

- **Current**：节点、材质、mesh、camera 和 light 主要用 vector index 标识；删除、Merge、Compact 会整体移动索引，现有选择和运行时句柄无法证明仍指向同一对象。
- **Target**：所有可选择/可命令引用对象使用带 generation 的 `EditorObjectId`；document index、primitive 子键、`SceneAsset` index、render instance/draw handle 之间有受验证的双向映射。
- **Acceptance (PASS/FAIL)**：property test 随机执行 add/duplicate/delete/reparent/undo/redo/merge/compact 后，每一步都验证 ID 唯一性、generation 失效规则、双向映射和所有 glTF 引用范围；删除对象的旧 ID 永不别名到新对象，Undo 恢复其逻辑 ID，Merge 的同名对象获得新 ID。

### 8.3 R03：Scene Browser 顶部区域与 Tab

- **Current**：`MinimalLatestApp.h` 只有基础 Scene Graph 树，没有参考项目的 Asset Info、完整层级行、分组 Scene List 和 Debug 视图。
- **Target**：复刻 tab bar 上方的 `Asset Info`、条件 `Animation`/`Material Variants` CollapsingHeader，以及 Release 的 Scene Graph/Scene List 两个 tab 和 Debug build 追加的 Debug tab；标签、分组、顺序、数量、展开、focus 和 empty state覆盖 node、primitive、mesh、material、camera、light、texture、image、animation。
- **Acceptance (PASS/FAIL)**：对空文档、单节点、多 scene、同名对象、超长 UTF-8 名称和 65,536 节点合成文档运行 UI harness；顶部 header 的存在条件、tab 数和顺序逐项等于参考；列表计数等于 document 集合，hierarchy preorder 与 glTF child/root 顺序一致，折叠/展开和滚动不改变选择，display-only 类型不能产生伪选择。外部 focus 只展开 parent path/设置 scroll，不额外强制切 tab。

### 8.4 R04：统一选择服务

- **Current**：当前只有应用内的选中节点索引；层级、Inspector、viewport、灯光列表和 GPU draw 没有统一选择合同。
- **Target**：单一 `SceneSelection` 同步 hierarchy、flat list、Inspector、viewport picking、渲染高亮、Gizmo attachment、Fit Object/focus 和 `Escape` 清除；选择 payload 使用稳定 ID 加 primitive 子键。Viewport single-click 遵守“新 Primitive 选中、唯一同一 Primitive 再点清空、空白清空”，double-click 只更新 camera center/speed。
- **Acceptance (PASS/FAIL)**：从每一种入口依次选择 Node/Primitive/Mesh/Material/Camera/Light，所有消费者在同一 UI 帧末看到相同 selection revision；pick classifier 在 double-click 窗口结束时只发 single 或 double 之一；删除/Compact/文档替换后选择按合同 fallback 或清空，绝不显示另一个同索引对象；重复 viewport click 的 toggle、空白 click、invalid/stale/hidden result、double-click hit/miss 和 `Escape` 均与 7.6 一致。

### 8.5 R05：节点与层级操作

- **Current**：MGIF 的树只读，缺少 Add Child、Add Light、Duplicate、Delete、Rename 和 drag reparent。
- **Target**：Scene root 与 node 上下文菜单、`Ctrl+D`、`Delete` 和 drag/drop 复刻参考行为；支持空节点和三类 light 子节点、递归删除、重命名，以及 append-only 的跨父/root 重挂并保持确定性 collection order。参考不支持同父 sibling reorder：同父/root-to-root drop 是 command-level no-op。Duplicate 按参考源码复制 node subtree，并修复 skin 与 animation channel 的 node 映射；mesh/material 等资源保持共享，除非用户显式 Split/Duplicate Material。
- **Acceptance (PASS/FAIL)**：层级测试覆盖 leaf/subtree/root duplicate/delete、same-parent/root-to-root no-op drop、跨父移动、drop 到 scene root、重复名称、空名称和多 scene；self-parent、descendant cycle 和无效/过期 ID 在 command 创建前拒绝，document/revision/history/selection 完全不变。父级改变时 execute 追加到目标尾部；Undo 追加回旧 parent/root 尾部而不恢复原 sibling slot；Redo 再追加到目标尾部，三步 collection 顺序均与参考 trace exact。same-parent/root-to-root drop 的 document revision/runtime digest/selection 不变，现有 redo 被清空且 UndoStack 新增恰好一条 `Move '<name>'`；Undo/Redo 只移动该 no-op command。非当前 scene row/node 的所有结构操作不得预设 reject 或 clicked-scene routing，必须等于 5.4/P0 的已批准 trace 决议；决议未关闭则 Gate P0 FAIL。Save/reload 后次序等于最终 document。唯一 current-scene root Delete 是另一严格 parity 特例：entry 保持 enabled，确认 modal 后 node/document revision/runtime digest 不变，但 selection 被清空、现有 redo 被清空且 UndoStack 新增恰好一条 `Delete '<name>'`；Undo 恢复该 selection 并把命令移到 redo，Redo 再次清 selection，三步均不得删除 node 或改写 glTF。

### 8.6 R06：数值 TRS 编辑

- **Current**：已有 translation/Euler rotation/scale 控件和两条 transform 更新路径，但直接改运行时数据、无事务、无 matrix/TRS 权威规则，且当前 scale 被额外 clamp 到 `0.001`。
- **Target**：Inspector 对节点 document TRS 编辑；rotation UI 保持连续 Euler cache，document 存 quaternion；一旦 TRS 成为权威就清除 `node.matrix`。允许 glTF 合法的零/负 scale，拒绝 NaN/Inf 以及会产生非有限 local/world matrix 的值，并原子更新后代、camera、light 和 render instance。
- **Acceptance (PASS/FAIL)**：输入正/负/零 scale、跨 `+/-180 deg` 连续旋转、有/无 parent、原 matrix 节点和边界有限值；document quaternion 归一化，world matrix 与独立计算在 `1e-5` 相对/绝对误差内一致；无效输入不改 revision；一次控件激活到释放只产生一条可逆命令。

### 8.7 R07：RHI Transform Gizmo 与 Grid

- **Current**：MGIF 没有 viewport Gizmo、Grid picking 或 editor overlay；参考实现直接依赖 Vulkan。
- **Target**：用后端中立 CPU 数学和 RHI Overlay Pass 复刻 translate X/Y/Z、XY/XZ/YZ，rotate X/Y/Z，scale X/Y/Z/uniform，hover/active、父空间、恒定屏幕尺寸、depth/x-ray、Grid 与 Snap；默认与 7.5 一致。
- **Acceptance (PASS/FAIL)**：确定性射线测试覆盖正交/透视相机、正/负 parent scale、近乎平行 ray、视口边缘和不同 DPI；拖动结果与参考捕获在约定容差内一致；一次 press-drag-release 只有一条历史；Vulkan 截图中每个 handle 非空、颜色/遮挡/hover/active 状态正确，Validation Layer 无错误。

### 8.8 R08：完整材质属性与扩展

- **Current**：MGIF 投影只覆盖基础 metallic-roughness、近似 spec-gloss、五类纹理和少量 alpha 字段，不能编辑或无损保存参考项目的完整材质数据。
- **Target**：Inspector 覆盖 metallic-roughness、`KHR_materials_pbrSpecularGlossiness`、参考暴露的纹理槽 Add/Switch/Remove、normal scale、occlusion strength、emissive、alpha、double-sided，以及 anisotropy、clearcoat、diffuse transmission、dispersion、emissive strength、IOR、iridescence、sheen、specular、transmission、unlit、volume、volume scatter；支持 Add/Remove extension。参考 UI 未暴露的 texCoord/`KHR_texture_transform` 只要求无损保留，不新增编辑控件。
- **Acceptance (PASS/FAIL)**：每个字段执行 min/mid/max、添加、删除、Undo/Redo、Save/reload；document 数值按 glTF 约束保存，未知 sibling extension/extras 不变，renderer 在下一可提交帧更新。alpha classification 变化重建正确 pass list，但 geometry buffer identity 不变。

### 8.9 R09：Primitive/Material 工作流

- **Current**：没有 primitive 级稳定选择及 Assign/Copy/Paste/Duplicate/Split/Merge；`SceneAsset` 的扁平 mesh 数据不足以充当编辑权威。
- **Target**：复刻 Inspector material assignment toolbar：Assign 现有 material、Copy/Paste material reference、Duplicate Material、Split（复制当前 node 使用的 mesh 和目标 material 以独立编辑）、Merge（仅在存在等价共享 mesh 时回并并安全删除孤儿副本）。Copy 必须向 OS clipboard 写入 exact ASCII `gltf_material:<current-document-index>`；Paste 从 OS clipboard 读取并在当前 active document 验证 index，稳定 ID 仍只在进程内使用。
- **Acceptance (PASS/FAIL)**：构造两个 node 共享 mesh/material、多 primitive 不同 material、无 material 和无等价 merge 候选的语料；每个操作后验证引用计数、primitive 顺序、accessor/geometry 共享关系、selection remap 与 reload，并断言 Undo/Redo count 不变。Clipboard partition 必须逐项 exact：无 prefix、空数字、负数、空白、非数字、尾随字符或 overflow 时 Paste disabled/no-op；完整非负十进制 index 在当前 document 范围内时 enabled 并显示 material name/index；非负但大于等于 material count 时保持参考的 enabled 状态、hover 显示 invalid clipboard、click no-op。切换 document 后同一 clipboard 文本必须按新 document 重新校验，不能映射为旧稳定 ID。malformed 数字的严格拒绝是 5.4 明示的安全例外；canonical/越界正 index 行为不是例外。无候选 Merge 必须是有提示的 no-op，不能删除资源。

### 8.10 R10：相机编辑与视口同步

- **Current**：MGIF 能加载 perspective/orthographic camera 并用于导航，但没有参考 Inspector 与 Apply to Current View / Set from Current View 的完整双向编辑闭环；参考中的 camera property edit、Apply 和 Set 本身均不进入 UndoStack。
- **Target**：相机列表与 Inspector 覆盖 perspective `yfov/aspectRatio/znear/zfar` 和 orthographic `xmag/ymag/znear/zfar`；复刻 Apply to Current View 与 Set from Current View；保留 glTF perspective `zfar` 可省略语义。
- **Acceptance (PASS/FAIL)**：验证参考 UI 数值边界和 glTF 约束：perspective `yfov > 0`、`aspectRatio > 0`（省略时随 viewport）、`znear > 0`、`zfar > znear` 或省略；orthographic `xmag/ymag != 0`、`zfar > znear`。Apply 后 view/projection 与 camera node world transform 一致；Set 后 camera/node 数据可再次 Apply 得到容差内相同视图；保存/重载不把省略字段伪造为 authored 值，且每条 property/Apply/Set 操作前后 Undo/Redo count 不变。

### 8.11 R11：KHR_lights_punctual 全生命周期

- **Current**：MGIF 可加载并直接编辑运行时 `SceneLight` vector，但缺少 document mutation、新增/删除节点、Undo/Redo 和保存。
- **Target**：point/directional/spot 可在 root、node 和 Lights group 新增，可选择、变换、切换 type、编辑、删除、Undo/Redo、渲染和保存；color 在 UI 用 sRGB、document 用 linear；directional intensity 为 lux，point/spot 为 candela；range `0` 表示未定义/无限；`0 <= inner < outer <= pi/2`。
- **Acceptance (PASS/FAIL)**：三种创建入口与三种类型逐一执行 create/edit/type-switch/delete/undo/redo/save/reload；node local `-Z` 方向映射正确，range 不受 node scale 影响，spot 边界不会产生 NaN/反向锥体，renderer light 数量和参数在下一帧一致。

### 8.12 R12：KHR_node_visibility

- **Current**：当前 `SceneNode` 没有扩展语义和 effective visibility；只能通过是否存在 instance 间接决定渲染。
- **Target**：有扩展时显示 Visible；无扩展时显示 Add Visibility；父节点隐藏递归抑制后代 draw，但不删除 document/GPU 资源；显示恢复后资源复用；保存 extension authored 状态。
- **Acceptance (PASS/FAIL)**：测试 visible/hidden parent-child 四种组合、动态 reparent、add/remove extension、隐藏选中对象、Save/reload；effective visibility 等于祖先链与自身 authored 值的合取，隐藏前后 GPU mesh/texture handle 不变。按参考最低行为，该操作不进入 UndoStack，UI 不得暗示可撤销。

### 8.13 R13：场景级根变换

- **Current**：MGIF 没有 scene-level transform；参考实现为每个 glTF scene 维护 root baseline，并在层级改变时冻结已应用结果。
- **Target**：每个 scene root 提供 Transform popup 与 Reset；用固定 baseline 计算 `sceneTRS * baselineLocal`，反复拖动不累计漂移；层级改变时按参考行为把当前结果冻结到现有 roots、重置 popup TRS，再为新 roots 建 baseline。
- **Acceptance (PASS/FAIL)**：对多 root、多 scene、matrix 节点、1000 次来回拖动、Reset、编辑中 add/delete/reparent root 执行矩阵对照；静止返回 identity 后误差不超过 `1e-5`，非可分解矩阵安全保留为 matrix。按参考行为不进入 UndoStack，并在 UI 明确其状态。

### 8.14 R14：Undo/Redo 历史

- **Current**：没有命令栈、clean revision 或统一事务；控件逐帧直接写状态。
- **Target**：线性历史最多 100 条，兼容命令在 500 ms 内合并；完成 release 的 Gizmo drag/完成 deactivation 的控件交互各一条命令；Undo 后执行新命令清空 redo；New/Open/replace/unload、成功 Merge、成功 Compact 清空历史；Save As 只更新 clean revision。进入与不进入 history 的 exact matrix 以 6.5 为准；唯一根 Delete no-op、same-parent/root-to-root Move no-op、reparent Undo append-order 和 active Gizmo drag 中的 `Escape`/release 特例分别以 R05、R16 为准。
- **Acceptance (PASS/FAIL)**：fake clock 测试 `499/500/501 ms`、第 `100/101` 条、空栈、连续 Undo 到头、Redo 到尾和分叉；所有 reference-undoable 操作做 execute -> undo -> redo 的 document/projection/render/selection 对照，但 expected state 必须取 reference trace，不能假定 reparent Undo 恢复原 sibling slot。所有 reference non-history 操作断言 undo/redo count 与 next-description 不变；另测试 R05 的唯一根 Delete 与 same-parent/root-to-root Move 两类 no-op command 都仍入栈并清 redo，各自的 Undo/Redo selection/history 结果 exact。每步比较 document hash、document/selection revision、description 和 stack sizes；命令描述与菜单标签一致，无 stale ID 解引用。

### 8.15 R15：SceneChangeSet 与 frame-safe 同步

- **Current**：只有 legacy mesh 与 experimental instance 的 transform 更新入口；材质、visibility、assignment、hierarchy 和 topology 没有统一增量合同。
- **Target**：document mutation 产生 `Transform/MaterialParameters/MaterialBinding/Light/Visibility/Hierarchy/Topology/FullRebuild` delta；delta 可合并、带 source/target revision，由 renderer 在 frame-safe 边界消费；`SceneAsset` 仍是只读派生投影。
- **Acceptance (PASS/FAIL)**：每类命令断言产生最小合法 delta；同对象连续 delta 合并到最新 revision，不同对象顺序稳定；过期/重复 delta 幂等忽略；FullRebuild 在新投影和 GPU 资源完全就绪后一次性切换，失败保持旧场景。连续 TRS/材质/灯光编辑不得调用 `waitForIdle()` 或重传完整 geometry/texture。

### 8.16 R16：视口输入优先级

- **Current**：当前没有 Gizmo/picking 仲裁；camera navigation 和 ImGui capture 由应用直接判断。
- **Target**：优先级固定为 modal/dialog -> active Gizmo -> ImGui captured widget -> viewport picking -> camera navigation；drag 从 viewport 内开始后，即使 pointer 离开 viewport 仍持续到左键 release。参考没有 Gizmo cancel/rollback API：active drag 中 Escape-only（按下 Escape 时左键仍保持 down）只清空 selection，下一次 attachment sync 直接 detach，保留最后 preview TRS 且不产生 transform-end history entry；focus loss 本身也不创建 rollback，只有输入层实际交付 release 才走正常 commit。Escape 与 release 同帧时 release 仍走 `endDrag`，产生一条 transform history，selection 最终清空。
- **Acceptance (PASS/FAIL)**：输入状态机测试覆盖同帧 press+move、跨 viewport、DPI scale、窗口失焦有/无 synthetic release、modal 打开、文本编辑、左右键并发、Escape-only while mouse-down、Escape+release 同帧和 detach 后的 late release；每个事件最多由一个消费者处理。普通及同帧 Escape+release 都精确提交一次且相机不跳变；Escape-only trace 断言 selection 清空、最后 preview TRS 保留、history count/next-description 不变、detach 后 late release 不补交命令；不得实现参考中不存在的恢复起始 TRS。P0 runtime trace 若推翻同帧源码预测，必须先更新 R16、U03 和 manifest，而不是在实现中暗改。

### 8.17 R17：原子 Save As 与 round-trip

- **Current**：MGIF 只能加载 glTF 和读写有损 `.sceneasset` 缓存，没有源 glTF/GLB 保存、验证和 clean state。
- **Target**：唯一用户命令 `Save As` 每次先打开原生对话框；选定路径后，内部 writer 先验证 document，再在目标同目录写临时文件及所需资源，flush/close 成功后原子替换；支持 `.gltf` 外部资源和 `.glb` 嵌入；只有全部成功才更新 path、recent files 与 clean revision。
- **Acceptance (PASS/FAIL)**：测试新路径、覆盖、只读目录、磁盘写失败、外部 image/buffer 缺失、同名资源、Unicode 路径、跨卷 Save As、目标已存在和进程中断注入；任何失败均不损坏旧目标、不留下被引用的半文件、不改变 dirty/path/history；成功产物通过验证器并 reload 语义等价。

### 8.18 R18：Merge 与 Compact

- **Current**：MGIF 没有 glTF merge/index remap/compact；现有资源限制只在 loader/cache 层零散存在。
- **Target**：Merge 先完整解析和验证 incoming document，检查合并后资源上限，再为其分配新 ID、重映射所有标准引用和已知 extension 索引，并包在命名 root 下；Compact 从全部 scenes/animations/skins/extensions 的可达根清除不可达资源并重映射索引。
- **Acceptance (PASS/FAIL)**：交叉语料覆盖同名资源、多个 scene、skin joints/skeleton、animation sampler/channel、camera/light、material texture chains、bufferView/accessor、variants 和未知 extension；失败保持原 document；成功后 validator、映射不变量、渲染 hash、Save/reload 均通过，且 Compact 二次执行为 no-op。

### 8.19 R19：文档生命周期与原子切换

- **Current**：加载/卸载由应用状态分支完成，replace 会先 teardown 当前 scene，recent-file 策略与异步发布分散，失败可能让 UI 看见无 scene/半生命周期状态。
- **Target**：New、Open、Open Recent、drop replace、Shift+drop Merge、reload、unload、close 和 Settings 的 Multiple Scenes current-scene switch 都经过 `EditorController`；成功路径的菜单/modal 行为与参考一致，不新增 unsaved prompt。为满足 MGIF 的资源安全，后台 load/project/upload 成功前不暴露候选 document，最新有效请求获胜或按队列合同串行；失败保留最后有效 active scene 并显示错误。Scene switch 不是 document mutation，不改 `defaultScene`、不进历史，并按 7.1.1 发布新的 runtime projection。
- **Acceptance (PASS/FAIL)**：对每个入口验证 clean/modified document 都不会弹额外 unsaved modal；New/Open/Exit 的 command dispatch 与参考一致，成功替换只发一次 selection/history reset；快速连续打开 A/B、加载中 close、无效 recent path、drop 非 glTF 文件和 candidate load/upload failure 均无半状态或 use-after-free，失败时 active hashes 不变。对 0/1/2+ scenes 验证 `Multiple Scenes` 的存在条件、radio 顺序/selected state、逐 scene render digest、document/defaultScene/UndoStack 不变，以及 selection/Gizmo/camera/focus 与 P0 trace 一致。

### 8.20 R20：UI/UX 行为复刻与 MGIF 主题

- **Current**：MGIF 有 ImGui Docking 和自身暗色主题，但编辑器菜单、面板、toolbar、context menu、dialog、enablement 和快捷键不完整。
- **Target**：布局、入口顺序、文案、快捷键、上下文菜单、tooltip、focus、disabled/busy/hover/active/selected/error 状态和操作结果与参考提交一致；只替换为 MGIF 的 palette、字体、spacing 和已有图标语言，中央 viewport 与现有 MGIF panels 保留。UI 实现使用仓库已有 upstream Dear ImGui API 和 MGIF-owned adapter，不移植参考 app/UI/helper framework。
- **Acceptance (PASS/FAIL)**：建立 reference/MGIF 同尺寸截图与交互脚本 manifest；100%、125%、150%、200% DPI 和 `1280x720/1920x1080/2560x1440` 下逐状态对照。命令可达性、快捷键、focus、dialog 返回结果必须完全匹配；文本不得重叠/截断到不可辨识，超长名称必须 ellipsis + tooltip。

### 8.21 R21：平台与 RHI 边界

- **Current**：MGIF 已有后端中立 RHI 和边界守卫，但参考 Gizmo/Grid 是 Vulkan 专用；D3D12/Metal 当前仅保证公共合同/编译，不是完整 runtime backend。
- **Target**：editor domain、selection、commands、projection、Gizmo math 和 Overlay public interface 不暴露 `Vk*`、`ID3D12*`、Metal object 或 backend include；production editor target graph 只连接现有 upstream Dear ImGui 与 MGIF-owned app/RHI/scene adapter，且没有 nvpro/reference-helper include、link、wrapper、public exposure 或传递依赖；Windows Vulkan 完整运行；D3D12/Metal 编译公共合同；Android 不编译 `app/editor` UI 与 desktop overlay orchestration。
- **Acceptance (PASS/FAIL)**：RHI boundary guard 为零新增违规；fail-closed scan 覆盖 `scene/editor`、`app/editor`、`render/editor`、editor-named public/source path、editor target include/link 声明及 production transitive closure；`DEMO_RHI_CONTRACT_ONLY`、Windows D3D12 object target、Apple Metal contract/compile target 和 Android 构建通过；Windows Vulkan 完整脚本通过。公共头出现原生 backend token、上述扫描出现 `nvpro_core`/`nvcore`/`nvapp`/`nvgui`/`nvvk*`/`nvvkgltf`/reference Vulkan helper，或 Android 链接 ImGui desktop/editor symbols 均即 FAIL。外部 reference evidence 和 unrelated renderer 的既有 `NVVK_SEMAPHORE_DEBUG` 不得被误报。

### 8.22 R22：容量、响应性与有界工作

- **Current**：loader/cache 已有 65,536 nodes/meshes、16,384 materials/images/cameras/lights 等 sanity limits，但 UI 递归、编辑操作和 rebuild 尚无统一预算。
- **Target**：空文档至现有上限均不崩溃；所有新增/merge 在 mutation 前检查溢出和资源预算；大列表 clip/virtualize；连续编辑只做受影响子树/记录更新；重拓扑和 FullRebuild 可显示 busy 并在后台准备。
- **Acceptance (PASS/FAIL)**：0、1、10k、65,535、65,536 节点基准可浏览，尝试第 65,537 个节点被原子拒绝；10k 可见列表 UI CPU p95 `<= 4 ms/frame`，单节点 Gizmo/TRS 编辑 UI+document+delta CPU p95 `<= 2 ms/frame`（验收机器固定、release+symbols、300 帧）；连续编辑无全场景 rebuild、全量 GPU upload 或 device idle。

### 8.23 R23：自动化与可审计验收

- **Current**：MGIF 有 RHI/renderer 模型测试，但没有 editor domain、round-trip、interaction 或 Gizmo visual test；参考项目的相关测试尚未移植。
- **Target**：建立 CPU unit、property、round-trip、index remap、history、renderer integration、RHI contract、UI interaction、Vulkan screenshot/validation 和 performance suite；每项功能矩阵映射至少一个测试或人工证据。
- **Acceptance (PASS/FAIL)**：CI/验收报告列出 24 个 Requirement ID、对应 test/evidence、结果、平台、commit 和 artifact；不存在 `Not Run`、无理由 skip、flaky retry 后掩盖失败或只有截图没有状态断言的条目。

### 8.24 R24：许可证与来源隔离

- **Current**：参考仓库源码带 NVIDIA copyright、Apache-2.0 和 SPDX notice；MGIF 已建立 provenance ledger，但仓库根仍无 owner 选择的 `LICENSE`/`COPYING`/`NOTICE`，因此对外分发按未授权、rights-reserved 处理。
- **Development policy**：P1 engineering readiness 与 release-license readiness 分离。新实现默认是基于可观察行为的 MGIF original clean-room work，除非 provenance ledger 明确逐文件记录为 copied/adapted；不复制 upstream Apache 源码；既有第三方 notice 必须保留；外部 reference executable/probe 的原生依赖只作为 evidence provenance，不进入 MGIF build graph。
- **Acceptance (PASS/FAIL)**：新增文件逐一做 provenance review；copied/adapted 文件含正确 SPDX/版权并在 manifest 记录 upstream commit 与原文件；dependency/boundary scan 不出现 `nvpro_core`、`nvcore`、`nvapp`、`nvgui`、任何 `nvvk*`/`nvvkgltf`、参考 Vulkan helper 或非目标 renderer 模块泄漏。例外记录缺少事前用户批准、最小性、替代方案、平台/许可证/binary-size 影响、精确 symbols/targets 或移除计划时同样 FAIL；当前批准例外数必须为 `0`。在 owner 明确选择根许可证之前，F42 与 R24 必须保持 `BLOCKED`，任何 release/distribution gate 都不得声称 PASS；该阻断不否定 P1 engineering readiness。

## 9. 功能与 UI 对照矩阵

### 9.1 功能 parity manifest

状态含义：`Existing` 表示 MGIF 已有可复用实现，`Partial` 表示只有运行时投影或局部 UI，`Missing` 表示尚无目标能力。最终验收时本表所有 `Partial/Missing` 必须转为 `Pass` 并链接实际测试/截图 artifact；不得仅修改表格状态。

| ID | 参考可观察行为 | MGIF 基线 | 目标证据 |
|---|---|---|---|
| F01 | `.gltf/.glb` 完整模型加载并直接编辑内存 glTF | `Partial`：只留下有损 `GltfModel/SceneAsset` | `T-DOC-01` no-op round-trip；R01 |
| F02 | 多 scene、scene root/current scene 保留；Settings 条件 `Multiple Scenes` radio 切换；跨 scene row/node 编辑目标 | `Partial`：rootNodes 投影，无权威多 scene 编辑/切换 UI | `T-DOC-02`、`T-EDIT-01`、`T-UI-15`；R01/R05/R19/R20 |
| F03 | Asset Info、extensions、XMP metadata | `Missing` | `T-UI-01` screenshot/state；R03 |
| F04 | Animation 选择、播放/暂停、单步、Reset、speed、timeline | `Missing` | `T-ANIM-01`、`T-UI-02`；R03/R15 |
| F05 | `KHR_materials_variants` 列表与全局 variant 应用 | `Missing` | `T-MAT-06`；R03/R08/R15 |
| F06 | Scene Graph 完整层级、类型图标、展开和 focus | `Partial`：基础 node 树 | `T-UI-03`；R03/R04 |
| F07 | Scene List 按 8 类资源分组、计数和选择规则 | `Missing` | `T-UI-04`；R03/R04 |
| F08 | Debug tab 显示 document/projection/draw 数量与映射诊断 | `Missing` | `T-UI-05`；R03/R15 |
| F09 | Node/Primitive/Mesh/Material/Camera/Light 单一选择 | `Partial`：node index | `T-SEL-01`；R02/R04 |
| F10 | Viewport delayed single/double picking、same-Primitive toggle、Scene Browser focus、silhouette/highlight、double-click camera center/speed、Fit Object | `Missing` | `T-SEL-02`、`V05`；R04/R16 |
| F11 | Scene root 新增 node/三类 light | `Missing` | `T-EDIT-02`；R05/R11 |
| F12 | Node Add Child 空节点/三类 light | `Missing` | `T-EDIT-03`；R05/R11 |
| F13 | Duplicate subtree 并修复 skin/animation node 映射 | `Missing` | `T-EDIT-04`、`T-ID-01`；R02/R05 |
| F14 | Delete subtree、确认框，以及 last-root document no-op + selection/history side effect | `Missing` | `T-EDIT-05`、`T-UI-06`；R05/R14 |
| F15 | Node/Mesh/Material Rename | `Missing` | `T-EDIT-06`、`T-UI-07`；R05/R20 |
| F16 | Append-only drag reparent、cycle reject、drop to root、same-parent no-op history 与 reference Undo order | `Missing` | `T-EDIT-07`、`T-ID-01`；R05/R14 |
| F17 | Inspector 数值 TRS 与 quaternion-backed Euler cache | `Partial`：直接编辑运行时 TRS | `T-TRS-01`、`T-CMD-02`；R06 |
| F18 | Scene Transform popup、baseline、Reset/freeze | `Missing` | `T-TRS-02`、`T-UI-08`；R13 |
| F19 | Translate/Rotate/Scale Gizmo、hover/active/picking | `Missing` | `T-GIZMO-01..04`、`V02`；R07/R16 |
| F20 | Infinite Grid、Grid Unit、Snap 设置与 toolbar 状态 | `Missing` | `T-GIZMO-05`、`T-UI-09`；R07/R20 |
| F21 | Core metallic-roughness 与纹理编辑 | `Partial`：可加载，不能权威编辑/保存 | `T-MAT-01`；R08 |
| F22 | specular-glossiness 与参考 13 类 material extension | `Partial`：只做近似投影 | `T-MAT-02..04`；R08 |
| F23 | Material Add/Remove extension、即时渲染更新 | `Missing` | `T-MAT-04`、`T-DELTA-02`；R08/R15 |
| F24 | Primitive Assign、OS clipboard `gltf_material:<index>` Copy/Paste 与 invalid/no-prefix 状态 | `Missing` | `T-MAT-05`；R09 |
| F25 | Duplicate Material、Split、Merge equivalent mesh | `Missing` | `T-MAT-07`；R09 |
| F26 | Perspective/orthographic camera Inspector | `Partial`：只加载/导航 | `T-CAM-01`；R10 |
| F27 | Apply to Current View / Set from Current View | `Partial`：应用路径局部存在 | `T-CAM-02`、`T-UI-10`；R10 |
| F28 | 三类 punctual light 新增、变换、属性编辑、删除 | `Partial`：运行时 vector 直接编辑 | `T-LIGHT-01..03`；R11 |
| F29 | `KHR_node_visibility` authored/effective visibility | `Missing` | `T-VIS-01`、`T-DELTA-04`；R12 |
| F30 | Linear Undo/Redo、描述、100 limit、500 ms merge | `Missing` | `T-CMD-01..06`；R14 |
| F31 | Transform/material/light 增量 dirty update | `Partial`：只有 transform | `T-DELTA-01..04`；R15 |
| F32 | Hierarchy/topology frame-safe rebuild 与延迟销毁 | `Missing` | `T-DELTA-05`、`V06`；R15 |
| F33 | New/Open/Open Recent 与拖放 replace | `Partial`：基础加载入口 | `T-LIFE-01`、`T-UI-11`；R19 |
| F34 | Shift+drop 与 File Import/Merge | `Missing` | `T-MERGE-01`、`T-UI-12`；R18/R19 |
| F35 | Save As、外部资源复制、GLB 嵌入、reload | `Missing` | `T-IO-01..04`；R17 |
| F36 | Compact orphan resources 与全引用重映射 | `Missing` | `T-COMPACT-01..03`；R18 |
| F37 | Busy/disabled/error、Delete/Rename/Assign modal 状态；无额外 unsaved/Compact modal | `Missing` | `T-LIFE-02..05`、`T-UI-13`；R19/R20 |
| F38 | 参考菜单、快捷键、context menu 和 toolbar | `Missing/Partial` | `T-UI-14` command manifest；R20 |
| F39 | Windows Vulkan validation + screenshot parity | `Existing foundation`，无 editor scene | `V01..V07`；R21/R23 |
| F40 | D3D12/Metal contract compile、Android editor 隔离 | `Existing foundation`，需纳入新接口 | `T-RHI-01..03`；R21 |
| F41 | 大 scene clip/virtualize 与增量编辑预算 | `Missing` | `T-PERF-01..03`；R22 |
| F42 | Apache-2.0 notice 与 upstream provenance | `Missing manifest` | `T-LIC-01`；R24 |

### 9.2 菜单和快捷键权威表

下表只复制参考项目中属于场景编辑器或直接服务于场景编辑工作流的入口。Load HDR、Save Image/Screen、RTX/path tracing、重新生成 tangents、reference Debug GPU toggles 等非编辑器功能按 5.2 不移植；MGIF 已有 renderer 菜单继续保留，但不得抢占下列快捷键或改变下列相对顺序。

| Menu | 顺序 | Label | Shortcut | Enable/Result |
|---|---:|---|---|---|
| File | 1 | New Scene | `Ctrl+N` | 始终可触发；直接卸载当前 scene，不弹 unsaved modal |
| File | 2 | Open | `Ctrl+O` | 始终可触发；选择 `.gltf/.glb`，成功后原子替换 |
| File | 3 | Import/Merge Scene... | - | 仅 valid document enabled；选择 `.gltf/.glb` 后事务 merge |
| File | 4 | Open Recent | - | 按 MRU 顺序；不存在路径显示错误并可移除，不替换当前文档 |
| File | 5 | Save As | `Ctrl+S` | 仅 valid document enabled；打开 Save As dialog，后缀决定 glTF/GLB |
| File | 6 | Exit | `Ctrl+Q` | 直接请求关闭，不弹 unsaved modal |
| Edit | 1 | Undo `<description>` | `Ctrl+Z` | 无历史或无文档 disabled；执行后刷新 selection/projection/render |
| Edit | 2 | Redo `<description>` | `Ctrl+Y` | 无 redo disabled；描述与下一条 command 一致 |
| Edit | 3 | Duplicate | `Ctrl+D` | 仅 Node 或 Primitive 对应的 node selected 时 enabled |
| Edit | 4 | Delete | `Delete` | 同上；文本输入时快捷键被抑制；先打开确认框 |
| View | 1 | Fit Scene | `Ctrl+Shift+F` | 仅 valid document enabled |
| View | 2 | Fit Object | `Ctrl+F` | 仅有可渲染 selection enabled |
| View | 3 | Grid | `G` | check/active 状态同步 toolbar；只在 viewport context 响应单键 |
| View | 4 | Gizmo | `T` | check/active 状态同步 toolbar；无 transformable selection 时可开但不显示 attachment |
| View | 5 | Snap | - | check/active 状态同步 toolbar |
| View | 6 | Grid & Snap Settings... | - | 打开或 focus 同一设置窗口，不生成重复 window state |
| Windows | 1 | Scene Browser | `F2` | toggle/focus 同一 dock panel |
| Windows | 2 | Inspector | `F4` | toggle/focus 同一 dock panel |
| Windows | 3 | Grid & Snap | - | 与 View 设置入口指向同一窗口 |
| Tools | 1 | Compact Scene | `Ctrl+K` | valid document enabled；立即事务执行，无确认 modal；成功后历史清空 |

快捷键解析硬规则：

- `Ctrl` 组合优先于 viewport 单键；`Ctrl+Shift+F` 不得同时触发 `Ctrl+F`。
- `io.WantTextInput` 时抑制 `Delete`、`G`、`T` 等会破坏文本编辑的命令；菜单点击仍按 enablement 执行。
- modal/文件对话框存在时仅处理其已有 Cancel/关闭及明确标记可重入的窗口操作；busy transaction 禁止冲突命令，但不得因此新增参考没有的 visible Cancel。
- 同一物理按键事件只 dispatch 一次；menu、toolbar 和 shortcut 必须调用同一个 command/controller API。

### 9.3 Toolbar 权威表

| Group | 控件 | 可见状态与结果 |
|---|---|---|
| Window | Scene Browser、Inspector icon button | 与 F2/F4 和 Windows menu 同一 bool；active 使用 MGIF `ButtonActive` 颜色，tooltip 显示名称和快捷键 |
| View | Grid、Gizmo、Snap icon button | 固定方形尺寸，开关不能引发布局跳动；active、hover、disabled 与 menu 同步 |
| Status | busy、error/progress | busy 指向当前事务，完成/失败后必须清除；错误可展开且不会自动提交部分状态；`revision != cleanRevision` 仅是内部状态，不新增参考中没有的 visible dirty badge |

MGIF 可在 toolbar 中保留已有 renderer 控件，但 editor controls 的分组、顺序和 tooltip 必须与参考一致；不得用带文字的圆角按钮替代已有熟悉图标。

### 9.4 上下文菜单权威表

| Target | 顺序 | Entries | 关键状态/结果 |
|---|---:|---|---|
| Scene root | 1 | Add Node | 新增 root node，选中新对象；按参考不增加 history entry |
| Scene root | 2 | separator；Add Point Light；Add Directional Light；Add Spot Light | 新增 light definition + root node，进入一条命令 |
| Node | 1 | Duplicate；Delete | Delete 打开 modal；唯一 current-scene root 时 entry 仍 enabled，确认后 dialog 关闭且无额外拒绝 UI，node 保留、selection 清空、redo 清空并新增 `Delete '<name>'` history entry；Undo 重选该 root，Redo 再清选，document 始终不变 |
| Node | 2 | separator；Add Child > Empty Node / Point / Directional / Spot | 新对象成为直接 child，顺序追加且选中 |
| Node | 3 | separator；Rename | modal 自动 focus 输入；Enter 提交，Escape/Cancel 不改 revision |
| Mesh | 1 | Rename | 只改 mesh name，不复制资源，不增加 history entry |
| Primitive | 1 | Assign Material... | 参考 Scene Browser 此项是占位；MGIF 必须至少路由/focus 到可工作的 Inspector Assign，不允许伪成功 |
| Material | 1 | Rename | 只改 material name，不增加 history entry |
| Material | 2 | separator；Copy Material ID | 向 OS clipboard 写 exact `gltf_material:<当前 document material index>`；这是与进程内稳定 ID 分离的 parity 例外 |

Inspector Primitive toolbar 的 Paste 状态同属本表合同：无 `gltf_material:` prefix 或 suffix 不是完整非负十进制 index 时 disabled 并显示 `no material copied` tooltip；canonical prefix + index 可解析但不在当前 document 范围时保持参考的可点击状态，tooltip 为 invalid，点击 no-op；合法 index tooltip 显示当前 material 名和 index。Copy/Paste 均不增加 history entry。对 malformed suffix 的 fail-closed 行为是 5.4 明示的外部输入安全例外。

Drag/drop 是 context menu 的同级合同：drag source payload 只能携带稳定 Node ID，不携带 sibling slot；hover target 必须显示参考的合法/非法状态；drop 只提交一次。self、descendant cycle 和无效/过期 ID 在命令创建前拒绝；父级改变时追加到目标尾部。same-parent/root-to-root 不能作为 reorder，仍按 6.5 创建 `Move` no-op command。多 scene target 与可能导致 current scene 空 root 的路径必须先由 5.4/P0 trace 定案，不得把“更合理”的预拒绝或 clicked-row routing 伪装成参考行为。

### 9.5 Inspector 精确属性表

| Selection | 编辑/显示内容 | Mutation/Undo 规则 |
|---|---|---|
| None | 稳定 `No selection` 空状态 | 清除 pending snapshot、Euler cache、popup target |
| Node | name、parent/children、mesh/camera/skin/light attachment、TRS、visibility | node name/TRS 经命令；visibility 按参考为 non-history；attachment 只读或通过明确 picker 改动 |
| Primitive | node TRS、mesh/primitive index、vertex/index/material stats、material toolbar、material properties | Assign/Copy/Paste/Split/Merge 走 non-history transaction；连续 material property edit 一条命令 |
| Mesh | name、primitive 数量/列表、引用 node、XMP | Rename 可编辑但 non-history，其余统计只读 |
| Material | name、core PBR、textures、alpha、double-sided、13 类 extension、Duplicate/Copy | property 控件 activation 捕获完整 material before 并在 deactivate 后提交 old/new；name/Duplicate/Copy 为 non-history |
| Camera | type 对应字段、Apply to Current View、Set from Current View | 不显示另一 type 的无效控件；所有值先验证后以 non-history transaction 提交 |
| Light | type、sRGB color、intensity、range、inner/outer cone、所属 node TRS | light property 连续编辑一条命令；node TRS 使用 transform command |

Reference material extension 精确集合为：`KHR_materials_anisotropy`、`KHR_materials_clearcoat`、`KHR_materials_diffuse_transmission`、`KHR_materials_dispersion`、`KHR_materials_emissive_strength`、`KHR_materials_ior`、`KHR_materials_iridescence`、`KHR_materials_sheen`、`KHR_materials_specular`、`KHR_materials_transmission`、`KHR_materials_unlit`、`KHR_materials_volume`、`KHR_materials_volume_scatter`，另含 `KHR_materials_pbrSpecularGlossiness` workflow。扩展名称、Add/Remove 状态、tooltip 和数值范围以参考提交与 glTF extension 规范的交集为准；规范不允许的值必须在 mutation 前拒绝。

### 9.6 UI 状态机

| State | Scene Browser | Inspector | Viewport/Gizmo | Menus/shortcuts |
|---|---|---|---|---|
| `NoDocument` | 显示 `No scene loaded` | 空状态 | 无 selection/Gizmo | New/Open/Recent/Exit 可用，其余 document 命令 disabled |
| `LoadingCandidate` | 旧文档仍可见但 mutation disabled，显示 busy | 旧属性只读 | 旧场景继续渲染或显示明确 busy overlay | 禁止第二个 destructive transaction；不新增 reference 没有的 visible Cancel，内部 token 可取消过期 job |
| `ReadyClean` | 完整交互 | 完整交互 | picking/Gizmo 可用 | Save As enabled；Undo 取决于 history |
| `ReadyModified` | 与 `ReadyClean` 外观一致 | 同上 | 同上 | New/Open/Exit 不弹 unsaved modal；Save As 成功更新 clean revision |
| `GizmoHover` | selection 不变 | 不变 | handle hover 高亮 | camera/picking 尚可，press 命中后转 Dragging |
| `GizmoDragging` | selection 通常不变；Escape 后清空 | 当前 TRS 实时预览 | capture pointer，active 高亮，camera/picking suppressed，离开 viewport 后继续 | 普通 release 提交一次；Escape-only while mouse-down 清 selection、保留 preview、detach 且无 history；Escape+release 同帧清 selection、保留 preview 并提交一次；均不 rollback。其他快捷键以 P0 reference trace 为 exact oracle，不自行增加 suppression |
| `Modal` | 背景不可 mutation | 背景不可 mutation | 不接收输入 | 只处理 modal 内 command 和系统关闭规则 |
| `Saving/Merging/Compacting` | 显示具体 operation 和 progress | mutation disabled | 已提交旧状态持续渲染，直到原子切换 | 冲突命令 disabled；失败回到原状态并显示错误 |
| `Error` | 保留最后有效 document | 保留有效 selection/属性 | 保留最后有效 render scene | 错误可 dismiss；不得伪造 clean/success |

## 10. 文件级改动清单

以下是计划期默认文件布局。文件名允许因 MGIF 命名规范微调，`Owner/Responsibility` 和依赖方向不允许改变。每个新 public header 都必须通过 RHI boundary 与 backend token scan。

### 10.1 新建：无 UI 的 editor domain

| 文件/目录 | Owner/Responsibility |
|---|---|
| `scene/editor/EditorObjectId.h` | 强类型 object kind + slot + generation；hash/compare/invalid；禁止存 glTF index 或写入 extras |
| `scene/editor/EditorLimits.h` | 统一 node/mesh/material/image/camera/light/accessor/payload 上限和 checked-add helpers，替代各处漂移的 magic limit |
| `scene/editor/GltfDocument.h/.cpp` | 权威 `tinygltf::Model`、source/path/dependencies、active scene、revision/clean revision、ID registry、read-only query |
| `scene/editor/GltfDocumentIO.h/.cpp` | `.gltf/.glb` load、external URI policy、validate-before-commit、atomic save/Save As、临时文件清理和错误对象 |
| `scene/editor/GltfSemanticCompare.h/.cpp` | 测试/验收用语义 digest 与差异报告；忽略 JSON 排版但不忽略 collection/reference/value |
| `scene/editor/GltfValidator.h/.cpp` | index、hierarchy、TRS/matrix、camera/light/material range、resource reachability、limit 与 extension-aware validation |
| `scene/editor/SceneEditor.h/.cpp` | 唯一 mutation API；add/duplicate/delete/reparent/rename/TRS/visibility/attachment/material/camera/light/variant；preflight 后原子提交 |
| `scene/editor/SceneGraphSnapshot.h/.cpp` | 结构命令所需最小快照、ID/revision 恢复和引用重映射；不复制不变的大型 vertex/image payload |
| `scene/editor/SceneProjection.h/.cpp` | document -> `SceneAsset`/runtime payload；增量重投影；输出 mapping 与 change set，不反向修改 document |
| `scene/editor/SceneRuntimeMapping.h/.cpp` | `EditorObjectId <-> glTF index <-> SceneAsset/runtime handle` 双向表与 invariant checker |
| `scene/editor/SceneChangeSet.h/.cpp` | delta 类型、受影响 ID、revision、合并/去重/ordering、FullRebuild transaction token |
| `scene/editor/SceneMerger.h/.cpp` | incoming document preflight、资源/extension 索引 remap、wrapper root、全有或全无 commit |
| `scene/editor/SceneCompactor.h/.cpp` | 全图可达性、资源清理、稳定 ID/引用 remap、idempotent compact |
| `scene/editor/commands/EditorCommand.h` | `execute/undo/redo/description/canMerge/merge` 合同；命令持稳定 ID，不持 vector reference |
| `scene/editor/commands/EditorHistory.h/.cpp` | 100-entry linear history、500 ms merge、fake clock injection、clean revision 协作 |
| `scene/editor/commands/TransformCommands.*` | node TRS 与 Gizmo transaction |
| `scene/editor/commands/HierarchyCommands.*` | add node/light、duplicate、delete、reparent、rename |
| `scene/editor/commands/PropertyCommands.*` | 只包含 reference-undoable 的 material/light property 命令；camera/binding/split/merge 使用 non-history transaction |
| `scene/editor/NonHistoryTransactions.h/.cpp` | root Add Node、mesh/material rename、camera、visibility、scene transform、material duplicate/Assign/Split/Merge 等 reference non-history mutation 的统一 preflight/commit；不提供 undo entry |
| `scene/editor/AnimationPlayback.h/.cpp` | 参考 playback state、sampling 与 editor conflict policy；只消费 document animation，不提供 authoring |

### 10.2 新建：桌面 editor shell

| 文件/目录 | Owner/Responsibility |
|---|---|
| `app/editor/EditorController.h/.cpp` | 组装 document/history/selection/projection/renderer delta；New/Open/Save/Merge/Compact、revision/clean state；异步 transaction 状态机 |
| `app/editor/SceneSelection.h/.cpp` | Node/Primitive/Mesh/Material/Camera/Light 单一选择、OS material clipboard adapter（canonical `gltf_material:<index>`）、focus/apply-camera events；稳定 ID 不写入 clipboard |
| `app/editor/EditorInputRouter.h/.cpp` | modal -> Gizmo -> ImGui -> picking -> camera 优先级，viewport/DPI 坐标、delayed single/double classifier、pointer capture 和 reference Escape/no-rollback 语义 |
| `app/editor/EditorMenuBar.h/.cpp` | 9.2 菜单、快捷键和 command enablement；不实现 domain mutation |
| `app/editor/EditorToolbar.h/.cpp` | 9.3 window/gizmo controls 与 status；固定尺寸、tooltip、theme token |
| `app/editor/SceneBrowserPanel.h/.cpp` | Asset Info、Animation、Variants、Scene Graph/List/Debug、drag/drop、context menu |
| `app/editor/InspectorPanel.h/.cpp` | selection-aware properties、Euler cache、widget transaction、assignment toolbar |
| `app/editor/EditorDialogs.h/.cpp` | Rename/Delete/Assign Material modal、错误展示和原生 Open/Save file dialog adapter；不得增加 unsaved/Compact modal |
| `app/editor/EditorTheme.h/.cpp` | 仅定义 MGIF palette/icon/spacing 到 editor 控件的映射；不改变交互结构 |
| `app/editor/EditorTestDriver.h/.cpp` | 仅在 test/acceptance build 开启的确定性命令、UI state dump、screenshot marker；production 默认关闭 |

### 10.3 新建：RHI editor rendering

| 文件/目录 | Owner/Responsibility |
|---|---|
| `render/editor/TransformGizmo.h/.cpp` | backend-neutral Gizmo state、attachment、ray geometry hit test、drag/snap math；不创建 GPU resource |
| `render/editor/GizmoGeometry.h/.cpp` | CPU vertex/index generation、component bounds/color/visibility；稳定、可单测 |
| `render/editor/ScenePicking.h/.cpp` | backend-neutral pick request/result、stable selection mapping、hit world position/distance；无 backend handle |
| `render/passes/EditorPickingPass.h/.cpp` | 在 MGIF GPU-driven path 输出/读取 object ID 或等价 pick result；异步结果带 scene revision，过期结果丢弃 |
| `render/passes/EditorOverlayPass.h/.cpp` | RHI buffer/pipeline/argument table、Grid/Gizmo draw、scene depth/x-ray 合成、延迟销毁 |
| `shaders/editor_overlay.slang` | Grid/Gizmo vertex/fragment，使用 MGIF clip-space/ShaderInterop 合同 |
| `shaders/editor_picking.slang` | 仅在现有 pass 无法复用 object ID 时新增；输出 runtime instance ID，不编码 `EditorObjectId` |

### 10.4 修改：现有加载、场景和 renderer

| 文件 | 必要改动 |
|---|---|
| `loader/GltfLoader.h/.cpp` | 将“解析文件”和“从已解析 model 解码 runtime payload”拆开；兼容入口委托给 `GltfDocumentIO + SceneProjection`，不再把局部 model 当唯一来源 |
| `loader/TinyGltfImplementation.cpp`（新） | 从 `GltfLoader.cpp` 移出唯一 `TINYGLTF_IMPLEMENTATION/STB_*_IMPLEMENTATION` translation unit，供 document IO 与 runtime decoder 共享，避免重复定义 |
| `scene/SceneAsset.h` | 补齐 runtime 所需 primitive identity、effective visibility、material extension projection、variant/skin/animation runtime data；仍不得包含可保存 document authority |
| `scene/SceneAssetBuilder.*` | 新增从 document/projection intermediate 构建；保留兼容 wrapper，最终移除 UI 对 `GltfModel` 分支 |
| `scene/SceneAssetSerializer.*` | 若缓存结构改变则 bump format version、拒绝旧/新不兼容数据并自动 fallback rebuild；缓存不保存 editor ID/history |
| `scene/SceneAssetView.h` | 提供 renderer 只读 view 所需的新 spans/mapping metadata，避免 renderer 持 editor model |
| `scene/TransformSystem.*` | subtree dirty propagation、effective visibility、cycle-safe world update 和 scene-root transform support |
| `scene/ParallelSceneLoader.*` | candidate document/projection job 与取消 token；完成前不 publish active document |
| `scene/SceneUploadPlan*` | Topology/FullRebuild 的 frame-safe 新旧资源切换计划和 revision 标记 |
| `render/RenderTypes.h` | backend-neutral editor frame params、overlay/pick inputs；禁止 native handle |
| `render/RendererFacade.*` | `applySceneChanges`、pick request/result、overlay state、projection swap；UI 不再逐 API 修改 renderer containers |
| `render/GPUDrivenRenderer.*` | 消费 change set；material/light/visibility/assignment/hierarchy/topology 增量更新；接入 picking/overlay pass |
| `render/GPUSceneRegistry.*` | stable runtime instance/draw mapping、enable/disable、frame-safe add/remove/update 与 debug invariants |
| `render/MeshPool.*` | topology rebuild 时复用不变 geometry，新增资源按 fence 延迟 retire |
| `render/LightResources.*`、`render/GPUDrivenLightResources.*` | document-projected light update、数量变化、type-specific buffer refresh |
| `render/ImGuiRhiRenderer.*` | 仅在 editor texture/viewport integration 需要时扩展；不得承担 Gizmo domain math |
| `render/PassExecutor.*`、`render/FrameScheduler.*` | overlay/picking pass 的显式依赖、scene depth transition 和 safe publish point |
| `app/MinimalLatestApp.h/.cpp` | 删除两套 node Inspector/selection/灯光直接 mutation；只保留 app lifecycle、renderer frame 和 `EditorController` 组装 |
| `CMakeLists.txt` | 新增 `scene_editor_core`、`scene_editor_projection`、desktop-only editor UI/overlay sources、shader 编译、test targets；Android 条件排除 UI，contract-only target 编译 public headers |
| `.rhi-boundary-allow` / baseline | 正常情况下不增加 allowlist；若工具误报，只能附证据修规则，不能用新增 baseline 掩盖 native leak |

### 10.5 新建：测试、语料与验收工具

| 文件/目录 | 责任 |
|---|---|
| `tests/editor/gltf_document_tests.cpp` | R01/R17 load/save/no-op round-trip、unknown data、failure injection |
| `tests/editor/editor_identity_tests.cpp` | R02 stable ID/generation/property sequences |
| `tests/editor/scene_editor_hierarchy_tests.cpp` | R05 hierarchy、order、cycle、duplicate/delete/remap |
| `tests/editor/editor_history_tests.cpp` | R14 limit/timing/branch/description/restore |
| `tests/editor/material_editor_tests.cpp` | R08/R09 core/extensions/assignment/split/merge、OS clipboard canonical/invalid/cross-document cases |
| `tests/editor/camera_light_visibility_tests.cpp` | R10-R12 bounds、projection、inheritance、round-trip |
| `tests/editor/scene_projection_delta_tests.cpp` | R15 minimal delta、coalescing、mapping、atomic swap |
| `tests/editor/scene_merge_compact_tests.cpp` | R18 全资源引用 remap/reachability/idempotency |
| `tests/editor/scene_selection_tests.cpp` | R04 stable selection/fallback/events、delayed single/double picking、same-Primitive/blank toggle、camera center/speed |
| `tests/editor/transform_gizmo_math_tests.cpp` | R06/R07/R13/R16 ray/drag/snap/matrix/property、outside-viewport release 与 Escape clear/no-rollback tests |
| `tests/editor/editor_ui_state_tests.cpp` | R03/R19/R20 command manifest、enablement、focus、modal/busy state |
| `tests/editor/editor_rhi_contract_tests.cpp` | R21 public include compile/token/static assertions |
| `tests/editor/editor_renderer_integration_tests.cpp` | R15 CPU/model-level renderer registry and frame-safe state transitions |
| `tests/fixtures/gltf_editor/` | 最小手写 glTF、reference-derived allowed fixtures、unknown extensions、bad indices、limits、Unicode、merge/compact corpus |
| `tools/run_editor_acceptance.ps1` | 构建、CTest、Vulkan Validation、scripted UI、截图/像素检查、performance、artifact index；失败返回非零 |
| `tools/compare_gltf_semantics.ps1` | 包装 semantic comparator 生成 machine-readable + human-readable diff |
| `docs/editor-parity/manifest.json` | reference action/state/screenshot -> MGIF evidence 映射，不存临时运行输出 |
| `docs/editor-parity/LICENSE-SOURCES.md` | copied/adapted/concept-only provenance、upstream path/commit/license |

### 10.6 构建与依赖方向

```text
scene_editor_core
  -> tinygltf implementation + glm + standard library
  -X SceneAsset / ImGui / RHI / RendererFacade / backend headers

scene_editor_projection
  -> scene_editor_core + SceneAsset data types + RHI public value enums
  -X ImGui / RendererFacade / backend headers

desktop_editor_ui (NOT ANDROID)
  -> scene_editor_core + scene_editor_projection + repository upstream Dear ImGui APIs
  -> MGIF-owned app file-dialog/input/window adapters
  -> backend-neutral RendererFacade editor API
  -X Vulkan / D3D12 / Metal headers
  -X nvpro_core / nvcore / nvapp / nvgui / nvvk* / nvvkgltf / reference Vulkan helpers

demo_core
  -> scene_editor_projection + RendererFacade + editor RHI passes
  -> backend implementation only behind existing RHI

Demo desktop executable
  -> MinimalLatestApp + desktop_editor_ui + demo_core

DemoAndroid
  -> demo_core runtime path only; no app/editor panels or desktop dialogs
```

`scene_editor_core` 必须能被 CPU tests 独立链接；否则每个 glTF/command test 都会被 GPU 初始化绑架，无法覆盖错误注入和 property sequence。`tinygltf` implementation macro 在整个目标图中只允许一个 translation unit 定义。

依赖审计以最终 production target graph 为准，而不只检查 direct include。`scene_editor_core`、`scene_editor_projection`、`desktop_editor_ui`、editor overlay/picking target，以及任何包含/链接它们的 production executable，均必须递归检查 target source、include/link 声明与内部 target closure。用中性名称 wrapper 间接链接 forbidden target 仍然违规；reference executable/probe 只有在外部 evidence graph 中隔离、且没有任何 MGIF production target 指向它时才允许保留原生 nvpro 依赖。

## 11. 分阶段实施计划

总顺序是 `P0 -> P1 -> P2 -> P3 -> P4 -> P5 -> (P6 || P7 CPU 部分) -> P7 RHI -> P8 -> P9 -> P10`。P6 与 P7 的 CPU math 可并行规划，但共享 `SceneSelection/RendererFacade` 接口必须先在 P5 冻结。每一阶段 gate 未通过不得在后续阶段用 UI mock 隐藏缺口。P0-P10 的每个阶段 gate 都必须重跑 fail-closed editor dependency scan；新路径、新 public API、新 target 或新传递 edge 一旦命中 N11 就停止，不能延后到 P10，也不能添加本地 allowlist。

### P0：基线、parity manifest 与测试骨架

**依赖**：无。

**需求**：R05、R14、R16、R19-R21、R23、R24 的 reference/baseline 部分。

- `P0-T01`：固定参考 commit `50373c7` 和 MGIF commit `6c36567`；记录构建器、Windows SDK、Vulkan SDK、GPU/driver、DPI 和测试 viewport。
- `P0-T02`：从参考可执行程序捕获 7.1.1、7.6、9.2-9.6 的 clean/hover/active/disabled/modal/busy 状态截图及 action trace；必须包含 `Multiple Scenes` 的存在/切换结果、每个 current/non-current scene row/node 上的全部结构操作、append-only reparent、same-parent/root-to-root `Move` no-op 及 Undo/Redo order、delayed single/double pick、same-Primitive toggle、double-click camera center/speed、Scene Browser 不强制切 tab、唯一根 Delete、active-drag Escape-only、Escape+release 同帧、focus loss 有/无 synthetic release 和 OS clipboard；建立 `manifest.json`，截图必须标注 current scene、clicked row、source/target stable ID、viewport、DPI、操作序列。O09 已取得 298/298 fresh-process real GLFW/Win32 + Dear ImGui + Vulkan action-path 与 numeric-exit capture，但只允许称为 `native_partial`：290 cells 有完整 visible history、7 cells/20 checkpoints 有 serializer topology、298/294 cells 的 current scene/selection 仅为 visible observation、private readback 为 0，不得把 298-cell domain matrix 改称 298 次完整 UI state replay。
- `P0-T03`：引入合法可再分发的最小 glTF corpus；对不能复制的第三方 asset 记录获取方法/hash，不提交受限内容。多 scene oracle fixture 至少包含 scene 0 roots `[A0, A1]`、scene 1 roots `[B0, B1]`、各自 child、一个两 scene 共享的 root/node 和不重复名称/ID；current scene 分别为 0/1 的 case 必须从同一 pristine fixture 单独重载，不能让前一个操作污染后一个 oracle。
- `P0-T04`：建立 `scene_editor_core` 空 target、test naming、failure-injection/test-clock utilities 和 acceptance report schema。
- `P0-T05`：跑现有 CTest、RHI boundary、Windows Vulkan smoke，保存 baseline；任何已有失败单独记录，不允许归因给 editor 后继续。
- `P0-T06`：D20 已由 MGIF project owner（user）以原文 `参考你的建议` 明确选择本文推荐的 `mgif_safety_exception`。原 user message timestamp 不可取得且不得伪造；`2026-08-22T16:55:24Z` 仅是 coordinator 第一次 durable recording/attestation 时间。合同固定为 clicked scene ID + clicked occurrence routing、全 `all_scenes` snapshot/restore、shared-node occurrence consistency；cycle/multiparent/stale occurrence 必须在 command/history entry 前原子拒绝；遍历必须 bounded iterative、不得递归爆栈；UI entry/label/layout/shortcut/modal/focus/enablement/history description 继续 reference parity。将这些字段和 O09 evidence hash 绑定到 D20、R05、manifest 与后续 acceptance oracle。
- `P0-T07`：锁定 Dear ImGui/MGIF-owned adapter dependency boundary；validator fail closed 扫描现有及未来 production editor path、editor public/source path、target include/link 声明和内部传递 target closure。负例必须证明 direct token、include/link、variable-indirection 和中性 wrapper/transitive edge 被拒绝，同时 reference evidence 文档与 editor scope 外既有 `NVVK_SEMAPHORE_DEBUG` 保持允许。

**Gate P0 / P1 engineering readiness**：42 项 manifest 均有 reference evidence 或明确“源码行为”证据；same-parent/root-to-root history、reparent Undo order、Escape 同帧顺序和多 scene 结构编辑有 bounded executable evidence，不能只靠源码预测。D20 已记录 owner 批准且不存在未决数据安全语义；O04/O06 conflict 有明确 disposition，O05/O07/O08/O09/O10 `native_partial` debt 绑定 later acceptance IDs；现有 MGIF build/tests/smoke 与 per-build Dear ImGui identity 可复现；development provenance policy 已确定；P0-T07 正/负 validator tests 通过，production editor dependency exception registry 为空。根许可证仍未选择时可以进入 P1 engineering，但 F42/R24 与 release/distribution gate 保持 FAIL/BLOCKED。

### P1：权威 GltfDocument、稳定 ID 与 I/O 基础

**依赖**：P0。

**需求**：R01、R02、R17、R22、R24。

- `P1-T01`：抽出 TinyGLTF implementation TU；实现 `GltfDocument`、revision/clean/path/dependency 与 limits。
- `P1-T02`：解析 `.gltf/.glb` 到 candidate document；加载成功后分配稳定 IDs，加载失败不 publish。
- `P1-T03`：实现 validator、semantic comparator、unknown extension/extras preserving policy。
- `P1-T04`：实现 no-op atomic writer、external URI staging、GLB packing 和 fault injection；暂不接 UI。
- `P1-T05`：导入/改写参考的 round-trip、model-primary、features-preserved、error-path 测试，并新增 Unicode/unknown extension/atomic failure cases。

**Gate P1**：`T-DOC-01/02`、`T-IO-01..04` 全通过；no-op save 的 semantic diff 为空；任何写入失败保持旧目标与 document state；核心库不依赖 ImGui/RHI，且 target/source/transitive scan 证明没有 nvpro/reference-helper dependency。

### P2：无损文档到 SceneAsset 的统一投影

**依赖**：P1。

**需求**：R01、R02、R15、R21。

- `P2-T01`：实现 `SceneProjection` 与 `SceneRuntimeMapping`，每个 node+primitive 获得可追踪 runtime identity。
- `P2-T02`：重构 `GltfLoader` 为从已解析 model 解码 payload；禁止第二次解析同一文件并产生不同 authority。
- `P2-T03`：扩展 `SceneAsset/SceneAssetView/Serializer` 所需字段与 cache version；old cache 自动失效重建。
- `P2-T04`：legacy/experimental 载入入口都通过 document -> projection；移除 UI 对两套 mutable model 的分支，但暂保留内部兼容 wrapper。
- `P2-T05`：mapping invariant、multi-primitive、camera/light/material/visibility/variant/animation projection 测试。

**Gate P2**：所有 P1 corpus 均得到可渲染投影；document semantic hash 在 projection 前后不变；两个旧入口产生相同 projection digest；Windows Vulkan 现有 scene smoke 无回归。

### P3：SceneEditor 领域操作与索引重映射

**依赖**：P2。

**需求**：R02、R05、R06、R08-R13、R18 的 mutation primitives。

- `P3-T01`：实现 query/preflight/transaction guard 和 add/rename/TRS/attachment。
- `P3-T02`：实现 duplicate/delete/reparent、目标尾部 append order、same-parent/root-to-root document no-op、reference Undo append order、cycle rules、skin/animation node remap，以及唯一根 Delete 的 document no-op result；多 scene mutation 必须遵守 D20，selection/history side effect 在 P4 接线。
- `P3-T03`：实现 material core/extensions、primitive assign/duplicate/split/merge。
- `P3-T04`：实现 camera/light/visibility/scene-root transform 与 variants；animation playback sampling 保持 document authority。
- `P3-T05`：从参考测试移植 hierarchy、index-remapping、children-order、material-cache/error paths，并加 randomized operation sequence。

**Gate P3**：除 R05 已锁定的唯一根 Delete 与 same-parent/root-to-root Move command-envelope 特例外，每个 rejected mutation 都满足“preflight 失败零改变”，每个 semantic mutation revision +1、validator 通过且 projection 可更新；两类 domain no-op 返回可识别结果且 document revision 不变。多 scene 操作满足 D20，并在每步验证所有 scene roots/children。property tests 至少 10 个固定 seed x 1,000 operations，无 dangling index/ID。

### P4：Command/Undo/Redo 与连续编辑事务

**依赖**：P3。

**需求**：R05-R11、R14。

- `P4-T01`：实现 `EditorCommand/EditorHistory`、100 limit、500 ms merge、fake clock 和描述。
- `P4-T02`：只封装 reference-undoable 的 transform、node rename/duplicate/delete/child-add/reparent、material property、light add/property 命令；Delete command 必须复刻唯一根 document no-op 仍清 selection/redo 并入栈的特例，Reparent command 必须复刻 same-parent/root-to-root document no-op 仍清 redo 并入栈以及 Undo append-order。其他 mutation 走 non-history transaction并断言 history count 不变。
- `P4-T03`：实现 widget begin-preview-commit 和 Gizmo begin-preview-release transaction；preview 不逐帧压历史。Gizmo 不提供 rollback/cancel：Escape-only 按 R16 清 selection、保留 preview 且不压 transform-end history，Escape+release 同帧按正常 release 精确压一条。
- `P4-T04`：定义 selection fallback、stable ID restore 和 history lifecycle；visibility/scene transform 保持参考 non-undo 标记。
- `P4-T05`：对所有命令执行 hash-based execute/undo/redo 和第 100/101、499/500/501 ms 边界测试。

**Gate P4**：reference-undoable feature matrix 的 execute/undo/redo 状态 100% 等于 reference oracle；不得用“理想可逆”覆盖 reparent Undo 不恢复 sibling slot 的参考结果。历史分叉、no-op command、清空、描述和 clean revision 全通过；无命令持有可失效 vector reference/pointer。

### P5：增量 SceneChangeSet 与 renderer 同步

**依赖**：P2-P4。

**需求**：R04、R08-R12、R15、R21、R22。

- `P5-T01`：冻结 `SceneChangeSet/RendererFacade` 公共合同、revision ordering、coalescing 和 error return。
- `P5-T02`：接 Transform/Light/MaterialParameters/Visibility 快路径；验证不重传 geometry/texture。
- `P5-T03`：接 MaterialBinding/Hierarchy/Topology；更新 draw classification、registry 与 bounds。
- `P5-T04`：实现 FullRebuild candidate upload、fence-safe atomic swap、failure rollback、deferred destruction。
- `P5-T05`：Debug counters 记录 delta count、rebuild、uploaded bytes、waitForIdle callsite；用于功能和性能 gate。

**Gate P5**：每类 mutation 的下一可提交帧 document/projection/renderer digest 一致；连续编辑 counters 为 0 full rebuild/0 geometry upload/0 device idle；Topology/FullRebuild 的 Validation Layer 无 lifetime/sync 错误；`SceneChangeSet/RendererFacade` editor public API 及其 target closure 通过 N11 dependency scan。

### P6：统一选择、Viewport Picking 与高亮

**依赖**：P5。

**需求**：R02、R04、R16。

- `P6-T01`：实现 `SceneSelection`、OS material clipboard canonical codec/current-document validation、selection revision/event、focus/apply-camera bridge。
- `P6-T02`：实现 backend-neutral pick request/result、delayed single/double classifier 和 `EditorPickingPass`；结果携带 scene revision、world hit/distance，过期/隐藏/删除 instance 不可选。
- `P6-T03`：renderer highlight/silhouette 或 MGIF 等价 outline；Node 选择高亮子树 draw，Primitive 只高亮目标 draw。
- `P6-T04`：实现 `EditorInputRouter` 与 viewport/DPI coordinate conversion；接 camera navigation suppression。

**Gate P6**：所有入口选择同步；跨帧 pick、single timeout、same-Primitive toggle、空白点击、double-click camera center/speed、focus without forced tab switch、Escape、隐藏对象和被删对象测试通过；clipboard canonical/invalid/cross-document 状态通过；截图证明高亮对象与 Inspector 一致。

### P7：RHI Grid/Gizmo Overlay

**依赖**：P5；P6 提供 attachment/picking arbitration。

**需求**：R06、R07、R13、R16、R21。

- `P7-T01`：从参考行为重写/合规改编 Gizmo CPU geometry、ray tests、parent-space drag、snap；纯 CPU tests 先绿。
- `P7-T02`：新增 Slang shader 与 RHI Overlay Pass，使用现有 buffer/pipeline/argument table/encoder，不开 native escape hatch。
- `P7-T03`：接 scene depth、x-ray、hover/active color、恒定 pixel scale、Grid settings 和 high-DPI viewport。
- `P7-T04`：接 command transaction、pointer capture、outside-viewport release/commit、Escape-only detach/no-history 与 Escape+release same-frame commit；不得新增 Gizmo cancel API 的可见行为。
- `P7-T05`：Vulkan Validation + screenshot/canvas-pixel 检查 desktop/mobile-sized viewport；D3D12/Metal public compile contract。

**Gate P7**：CPU math 全通过；所有 handle 在截图中可见且非空，reference state diff 仅允许 MGIF theme token 差异；不同 viewport/DPI 无重叠、跳动、错误拾取或 Validation error；Gizmo/overlay/picking source、public API、include/link 与 transitive closure 只使用 MGIF RHI，不含 reference Vulkan helper。

### P8：Scene Browser、Inspector、菜单和完整 UI 状态

**依赖**：P3-P7。

**需求**：R03-R14、R16、R19-R20。

- `P8-T01`：实现 Editor shell/default dock、Settings 中 7.1.1 `Multiple Scenes` selector、9.2 menu、9.3 toolbar 和快捷键 command registry。
- `P8-T02`：实现 Scene Browser Asset/Animation/Variants/Graph/List/Debug、clipper/cache、context menus、drag/drop。
- `P8-T03`：实现 Inspector 选择上下文、全部属性、material toolbar、widget transactions 和 camera sync。
- `P8-T04`：实现 reference dialogs、focus restoration、busy/error/disabled states、internal clean revision 和 MGIF theme mapping；不得增加 unsaved/Compact modal 或 visible dirty badge。
- `P8-T05`：将 `MinimalLatestApp` 旧 node/light/UI mutation 移除并改为 controller 组装；旧功能没有第二入口。
- `P8-T06`：运行 reference action trace，对每个 manifest state 生成 MGIF screenshot + state JSON。

**Gate P8**：菜单/快捷键/context/toolbar manifest 100% 通过；三种分辨率 x 四种 DPI 无重叠；同一 action 的 document/result 与 controller API 测试一致；跨 scene row/node 的 entry state 与 D20 oracle 一致；`MinimalLatestApp.h` 不含 editor mutation 实现；desktop UI target 使用仓库现有 upstream Dear ImGui API + MGIF-owned adapter，N11 direct/transitive scan 为零 finding。

### P9：完整生命周期、Merge、Compact 与持久化闭环

**依赖**：P1-P8。

**需求**：R17-R20、R22。

- `P9-T01`：接 New/Open/Recent/drop replace/Shift+drop Merge/reload/unload/close；成功路径严格不增加 Save/Discard/Cancel，candidate 失败保留旧 active state。
- `P9-T02`：接 Save As dialog、external resource staging、overwrite/cross-volume fallback 与 recent list。
- `P9-T03`：实现 `SceneMerger` 全集合/known-extension remap、limit preflight、wrapper root 和 atomic publish。
- `P9-T04`：实现 `SceneCompactor` reachability/remap/idempotency 和成功后的 history clear；UI 按参考立即执行，不增加 confirmation。
- `P9-T05`：对 load/save/merge/compact 注入 parse/validation/allocation/upload/write/rename 失败，验证旧状态完整。

**Gate P9**：端到端脚本完成 Open -> edit hierarchy/TRS/material/light -> Undo/Redo -> Merge -> Compact -> Save As -> reload，semantic/render/selection counts 对照通过；所有失败矩阵无 partial document/file/GPU publish。

### P10：全矩阵收敛、性能与发布门槛

**依赖**：P0-P9。

**需求**：R01-R24。

- `P10-T01`：运行全部 CPU/round-trip/property/integration/RHI/UI/Vulkan/performance tests，生成 requirement traceability report。
- `P10-T02`：在 Windows Vulkan Validation 下执行至少 30 分钟混合编辑 soak；监控 assert、GPU error、resource count、history/memory growth。
- `P10-T03`：跑 0/1/10k/65,536 node 性能与容量测试，修复 rebuild/upload/alloc hotspots。
- `P10-T04`：D3D12/Metal contract compile、Android build、RHI boundary、license/provenance scan。
- `P10-T05`：逐项关闭 parity manifest；任何差异必须修复或由用户书面变更本任务书，不允许实现者自批 waiver。
- `P10-T06`：删除过渡 wrapper/死 UI/双 authority 路径，更新开发文档与 acceptance artifacts。

**Gate P10**：满足 3.3 和第 17 节 Definition of Done；24/24 requirements、42/42 feature rows、110/110 edge obligations 有 PASS 证据，零未解释 skip/waiver；最终 production editor executable 的完整 include/link/transitive graph 通过 N11，且 approved dependency exception count 为 `0`。

## 12. 测试与验收策略

### 12.1 测试层与职责

| Layer | Target/工具 | 必须证明的内容 | 失败含义 |
|---|---|---|---|
| L0 Static | compiler warnings、include/token scan、license scan | public API backend-neutral、无 ODR 宏冲突、provenance 完整 | 架构/许可 gate 失败 |
| L1 CPU Unit | `scene_editor_core` tests | ID、validator、mutation、history、TRS/Gizmo math、merge/compact 的局部规则 | 领域合同失败，禁止进入 UI 验收 |
| L2 Property/Model | deterministic seeds + shrinking | 随机操作序列后所有 graph/index/revision 不变量 | 存在组合边界缺口 |
| L3 Round-trip/I/O | temp directory + fault injection | glTF/GLB 语义、未知数据、原子写、失败回滚 | 有数据损坏风险，一票否决 |
| L4 Projection/Renderer model | fake renderer registry + real `SceneAsset` | 最小 delta、mapping、frame-safe publish、resource reuse | document 与画面可能分叉 |
| L5 UI Semantic | test-only pinned ImGui Test Engine 或等价 in-process driver | menu/context/shortcut/focus/enablement/modal/busy 状态与 command result | 交互不等价 |
| L6 Vulkan Runtime | real `Demo.exe`、Validation + sync validation | overlay/picking/render update、GPU lifecycle、无崩溃/validation | Windows 主验收失败 |
| L7 Visual | deterministic camera/viewport/DPI + PNG/state JSON | panel geometry、control order、Gizmo nonblank/position/state、无 overlap | UI/Gizmo parity 失败 |
| L8 Platform contract | D3D12 object compile、Metal contract compile、Android build | public contract 无 Vulkan 假设，desktop UI 隔离 | RHI/platform gate 失败 |
| L9 Performance/Soak | release+symbols counters、30-min soak | p95 budget、无全量热路径、无资源/历史无界增长 | 不满足非破坏性实时编辑质量 |

UI test dependency只能进入 test target，必须 pin commit、记录 license，不能成为 production runtime 必需项。即使使用 semantic UI driver，Vulkan visual test 仍必须真实启动应用，因为 docking、DPI、clip、depth/x-ray 和 native dialog 生命周期无法由纯 unit test 证明。

### 12.2 比较规则与容差

| Domain | Comparator |
|---|---|
| glTF collection/reference | 类型、数量、顺序、整数索引、字符串、bool、enum 必须 exact；无语义“排序后相等”捷径 |
| 未编辑 binary/resource | buffer/image payload SHA-256 exact；允许 `.gltf` Save As 复制到新相对路径，但解析后 bytes 必须相同 |
| 未编辑 numeric | save/reload 后 IEEE value exact；writer 必须使用足够 round-trip 的 precision；若 third-party writer 无法保证，最大 `1e-12` abs/rel 且必须记录字段差异 |
| 已编辑 document numeric | UI 目标值与 reload 值 `abs <= 1e-9` 或 `rel <= 1e-9`；颜色/extension float32 来源允许 `1e-6` |
| TRS/world/runtime matrix | 每元素 `abs <= 1e-5` 或 `rel <= 1e-5`；quaternion 用 `abs(dot(q1,q2)) >= 1 - 1e-5` 处理符号等价 |
| Gizmo result | 最终 TRS 满足上述容差；handle projected center 与 reference action baseline 在 `2` logical pixels 内，命中区域差异不超过 `3` logical pixels |
| UI layout | panel/dock/control bounding rect 在同 DPI 下位置/尺寸差 `<= 2` logical pixels；MGIF 字体 metrics 导致的文本宽差可放宽到 `4`，但顺序、对齐、可见/clip 状态必须一致 |
| Theme pixels | 不与 NVIDIA palette 做逐像素相等；颜色必须来自 MGIF theme token，状态对比度满足现有 MGIF 标准；轴色语义 X/Y/Z 不可改 |
| Scene render | 不要求不同 renderer 对参考程序逐像素相等；要求同一 MGIF build 的编辑前/后 state oracle、document/projection digest、可见对象/material/light 参数和 picking ID 一致 |
| Performance | 固定验收机器、release+symbols、warm-up 120 帧、采样 300 帧以上，报告 median/p95/max、scene hash、build SHA 和 GPU/driver；只看平均值不算通过 |

任何容差不得用于放过错误的 collection、index、selection、command count、menu state 或文件生命周期；这些均为 exact contract。

### 12.3 CPU/领域测试矩阵

| Test ID | 覆盖 | 核心 cases |
|---|---|---|
| `T-DOC-01` | R01 | glTF/GLB、external/data URI/embedded、unknown extension/extras、skins/animations、多 scenes no-op round-trip |
| `T-DOC-02` | R01/R02 | ID registry、active scene、root/child/resource ordering 和 semantic digest |
| `T-IO-01..04` | R17 | glTF external staging、GLB pack、overwrite/cross-volume publish、write/flush/rename fault rollback |
| `T-ID-01` | R02 | generation reuse、stale ID、Undo restore、Merge same-name、Compact reindex |
| `T-EDIT-01..07` | R05 | add/duplicate/delete/rename/reparent、cycle/invalid ID、target-tail order、Undo append order、same-parent/root-to-root Move no-op、D20 多 scene row/node matrix；唯一根 Delete 的 document no-op + selection/history/redo/Undo/Redo exact trace |
| `T-TRS-01` | R06 | matrix->TRS、Euler continuity、zero/negative scale、non-finite reject、descendant propagation |
| `T-TRS-02` | R13 | scene baseline、Reset、1000 cycles、hierarchy freeze、多 scene |
| `T-MAT-01..07` | R08/R09 | core/所有 extension/texture/alpha、OS clipboard exact format/no-prefix/out-of-range/malformed/cross-document、assign/duplicate/split/merge/shared refs |
| `T-CAM-01..02` | R10 | perspective/ortho bounds、omitted zfar、view round-trip、node transform |
| `T-LIGHT-01..03` | R11 | type/color/intensity/range/cone/create/delete/node `-Z`/round-trip |
| `T-VIS-01` | R12 | authored/effective visibility、inheritance、reparent、resource identity |
| `T-ANIM-01` | R03/R15 | clip select、play/pause、single-step、Reset、speed/timeline、variant apply、edit conflict policy |
| `T-CMD-01..06` | R14 | execute/undo/redo、100/101、499/500/501 ms、branch、clear/clean、description、唯一根 Delete 与 same-parent/root-to-root Move no-op command、reparent Undo order |
| `T-DELTA-01..05` | R15 | delta classification/coalesce/order/stale/duplicate/full rebuild rollback |
| `T-SEL-01` | R04 | six selection types、idempotent events、delete/compact/replace fallback |
| `T-SEL-02` | R04/R16 | viewport coordinate/revision、delayed single vs double、same-Primitive toggle、blank/invalid/hidden/stale result、focus without forced tab switch、Node-vs-Primitive highlight、double-click camera center/speed、Fit Object |
| `T-GIZMO-01..05` | R07/R16 | component hits、drag planes/rings/axes、snap、parent space、outside-viewport capture/release、Escape-only detach/no-history、Escape+release same-frame commit、focus-loss synthetic-release partitions |
| `T-MERGE-01..03` | R18 | every standard index chain、known extension、limit/invalid incoming rollback |
| `T-COMPACT-01..03` | R18 | reachability、remap、semantic/render preserve、second compact no-op |
| `T-LIFE-01..05` | R17/R19 | modified-state no-modal parity、candidate races、atomic I/O faults、recent/drop/reload/close |
| `T-LIC-01` | R21/R24 | SPDX/copyright、upstream file+commit manifest、asset licenses；production editor path/public API/target include+link/transitive closure 的 nvpro/reference-helper negative scan；external reference evidence 与 unrelated renderer `NVVK_SEMAPHORE_DEBUG` allow-case |

Property tests 固定至少 10 个 CI seeds 和 1 个每日扩展 seed；失败必须打印可重放 seed 和最小化 operation trace。核心 invariant 每次 mutation 后检查，而不是只在序列末尾检查。

### 12.4 Renderer/RHI 集成断言

每类 edit 在提交前后采集 `EditorRuntimeSnapshot`：document revision、projection revision、mapping counts、GPU registry counts、resource handles、uploaded byte counters、full rebuild count、device idle count。必须满足：

- Transform：只改变受影响 subtree matrices/bounds；mesh/texture/material resource handle identity 保持。
- MaterialParameters：只更新目标 material record 和必要 draw metadata；vertex/index/texture upload bytes 为 `0`。
- MaterialBinding/alpha mode：目标 draw 的 material/classification 更新；无关 draw 和 geometry handle 保持。
- Light：light CPU/GPU record 在下一帧一致；类型/数量变化不留下旧 slot；需要时 DDGI history 只重置一次。
- Visibility：只改变 effective draw enable/registry flags；恢复后复用原资源。
- Hierarchy：world topology/mapping 更新，未改变的 mesh/texture GPU handle 保持。
- Topology：新/删 draw registry 在 frame boundary 一次发布；旧资源直到 fence 完成才 retire。
- FullRebuild：active scene 在 candidate 完成前保持旧 revision；成功只切换一次，失败切换 `0` 次。

`waitForIdle` counter 在 load initialisation、shutdown 或测试明确许可的离线边界外必须为 `0`。任何 Inspector/Gizmo 连续输入期间非零即一票否决。

### 12.5 UI 交互脚本

| Script | 操作序列 | Exact oracle |
|---|---|---|
| `U01-empty-new-open` | 无文档 -> menu enablement -> New -> Open invalid -> Open valid multi-scene -> Settings 逐 scene radio 切换 -> 在 current/non-current scene row/node 逐项执行结构操作 | no-document/clean/busy/error 状态、Multiple Scenes 条件/顺序、每 scene render、selection/history、一次 publish，以及 D20 的 target/result exact |
| `U02-hierarchy` | root context add -> child add -> rename -> duplicate -> cross-parent drop -> Undo/Redo -> same-parent drop -> root-to-root drop -> delete modal -> confirm 唯一根 Delete -> Undo -> Redo | 菜单顺序、focus、target-tail/Undo/Redo order、两类 Move no-op 的 document/selection/redo/history、普通 delete result，以及唯一根 Delete exact trace |
| `U03-transform-gizmo` | viewport single hit/toggle/blank -> Inspector TRS -> G/T/Snap -> hover/drag outside/release -> Undo/Redo -> drag+Escape-only -> drag+Escape+release 同帧 -> focus loss 有/无 release | click classification、handle/input owner、普通 release command、各 Escape/focus-loss 分支的 selection/TRS/history exact |
| `U04-material` | select primitive -> Assign -> OS Copy/Paste canonical/invalid/cross-document -> Split -> edit extensions -> Merge | toolbar enablement/tooltip、clipboard text、popup focus、shared refs、renderer delta、history unchanged、reload |
| `U05-camera-light-vis` | viewport double-click hit/miss -> apply/set camera -> add/edit three lights -> visibility parent/child | camera center/speed、numeric bounds、view state、light buffer、effective visibility |
| `U06-merge-compact-save` | Shift-drop merge -> Compact -> Save As GLB -> reload | 无 Compact modal、busy/disabled、history clear、counts、semantic/render digest |
| `U07-modified-failures` | modified -> New/Open/Exit（无额外 modal）；Save failure；Merge/load candidate failure | dispatch parity；failure 时 old document remains、revision/path/history exact、error shown once |
| `U08-large-unicode-dpi` | 10k list scroll、超长 CJK/emoji/combining name、4 DPI | clipper rows、ellipsis/tooltip、focus/selection、无 overlap |

每个脚本同时输出 action log、UI state JSON、document/runtime digest 和关键帧 PNG；仅有 PNG 或仅有状态 JSON 都不足以签收。

UI semantic target 的 ID 分配固定如下，避免 manifest 引用没有定义的测试：

| Test ID | Oracle |
|---|---|
| `T-UI-01` | Asset Info metadata/extensions/XMP 的结构、空值与长文本状态 |
| `T-UI-02` | Animation/Material Variants header 的显示条件、controls、选择与 playback 状态 |
| `T-UI-03` | Scene Graph preorder、type rows、expand/focus/drag source-target、append-only/同父 no-op 状态；external focus 不强制从 List/Debug 切 tab |
| `T-UI-04` | Scene List 八组数量、顺序、selectable/display-only 规则 |
| `T-UI-05` | Debug-build-only tab、counts/mismatch/details/repair action；Release 隐藏 |
| `T-UI-06` | Delete modal 文案、default focus、Confirm/Cancel/close result；唯一根 entry 不禁用、不显示额外拒绝 UI，Confirm 后 node 保留但 selection/history 按 R05 变化 |
| `T-UI-07` | Rename modal node/mesh/material、Enter/OK/Cancel、empty-name no-op、focus restore |
| `T-UI-08` | Scene Transform popup、Reset、hierarchy-freeze observable state |
| `T-UI-09` | Grid/Gizmo/Snap toolbar/menu/settings 的双向状态与 range |
| `T-UI-10` | Camera Apply/Set controls、enablement、view result |
| `T-UI-11` | New/Open/Recent/drop replace、modified-state no-extra-modal parity |
| `T-UI-12` | Import/Merge menu 与 Shift-drop modifier/result/busy state |
| `T-UI-13` | NoDocument/Loading/Ready/Dragging/Modal/Busy/Error 全状态；禁止额外 unsaved/Compact/visible-cancel UI；Dragging 的 Escape-only no-history 与 Escape+release same-frame commit 均为 clear/no-rollback |
| `T-UI-14` | 9.2-9.4 menu/shortcut/toolbar/context command manifest 的顺序、label、tooltip、enablement 与 dispatch |
| `T-UI-15` | Settings `Multiple Scenes` 的 `scenes.size() > 1` 条件、header 位置、radio name/index 顺序、current selection，切换后的 projection/render/selection/Gizmo/camera/history/focus，以及 current/non-current scene row/node 结构操作的 D20 exact trace |

### 12.6 Vulkan visual 与 validation 矩阵

| Case | Scene/Camera | Required frames/states |
|---|---|---|
| `V01-grid` | empty ground + perspective/ortho | grid off/on、unit 0.001/1/1000、near horizon、viewport resize |
| `V02-gizmo-components` | identity node | every translate/rotate/scale component normal/hover/active |
| `V03-parent-space` | rotated parent + negative/non-uniform scale | attachment orientation、axis drag、no NaN/inversion crash |
| `V04-depth-xray` | gizmo partly behind opaque mesh | visible/x-ray/depth portions and selected outline |
| `V05-delta-edit` | material + three lights + hierarchy | before/edit/undo/redo/settled frames, correct scene response |
| `V06-topology-swap` | duplicate/delete/merge/compact | pre-publish old scene、single publish、post-fence retirement |
| `V07-dpi-resize` | all supported viewport sizes | nonblank overlay、stable screen size、correct mouse mapping |

沿用 `tools/VULKAN_SMOKE_RUNBOOK.md` 的可执行 SHA、独立证据目录、timeout、exit code、Validation/sync-validation 失败正则和禁止终止外部 RenderDoc 进程规则。Editor automation 必须增加 `config/scene-ready/action-start/action-commit/settled/complete` 单次 marker；缺 marker 与 validation error 同样判 FAIL。

Canvas/pixel checks 至少断言：viewport 非全黑/非单色、Grid/Gizmo mask 像素数处于 baseline 区间、每个 axis/component 的屏幕 bounding box 非空、active/hover token 出现、UI rect 互不覆盖、截图分辨率与 viewport manifest 相符。

### 12.7 平台与构建 gate

目标验收命令（P0/P1 建立 aggregate targets 后由 `tools/run_editor_acceptance.ps1` 统一执行）：

```powershell
cmake -S . -B out/build/x64-debug -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DUSE_SLANG=ON
cmd /c build_debug_with_vsdevcmd.cmd
cmake --build out/build/x64-debug --target scene_editor_tests Demo --parallel 2
ctest --test-dir out/build/x64-debug --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass `
  -File tools/check_rhi_boundaries.ps1 -RepoRoot . -Enforce
```

额外 gate：

- `T-RHI-01` Windows：另建 `DEMO_RHI_CONTRACT_ONLY=ON` tree，编译 `rhi_public_contract_tests` 与 `rhi_d3d12_contract`，并让 `editor_rhi_contract_tests` 包含所有新增 public headers。
- `T-RHI-02` macOS CI：`DEMO_RHI_CONTRACT_ONLY=ON` 编译/run `metal4_contract_tests`，再编译 `scene_editor_core` 和 editor overlay public contract；没有 macOS artifact 不得声称 Metal 已验证。
- `T-RHI-03` Android：运行项目既有 Gradle/NDK build；构建图和最终 symbols 中不得出现 `app/editor/*Panel`、desktop file dialog 或 EditorOverlay orchestration。
- Boundary guard：扩展 `tools/check_rhi_boundary.py` 与 `tools/check_rhi_boundaries.ps1` 的 scan roots，纳入 `scene/editor` 和 `render/editor`；新增 finding 不得写入 baseline/allowlist 来转绿。
- Dependency guard：每次 scaffold/phase/acceptance 运行 `python tools/validate_editor_p0_artifacts.py --repo-root . --mode scaffold`，扫描 `scene/editor`、`app/editor`、`render/editor`、editor-named source/public API、editor target 的 include/link 声明及 production target transitive closure；reference evidence 文档不进入扫描输入，unrelated renderer compile definition 不因 token 相似被误判为 editor dependency。

### 12.8 性能、内存和稳定性

| Metric | Workload | Pass threshold |
|---|---|---|
| UI large list | 10k visible-resource rows、持续 scroll 300 帧 | editor UI CPU p95 `<= 4 ms`，无每帧全量 label/cache 重建 |
| Transform edit | 65,536-node doc 中编辑 leaf，300 帧 | UI+document+delta CPU p95 `<= 2 ms`；只更新必要 subtree |
| Gizmo render | 1080p，Grid+Gizmo+outline | editor overlay GPU p95 `<= 0.5 ms`；无 per-frame pipeline/resource creation |
| Material edit | 连续 slider 300 帧 | full rebuild `0`、geometry/texture upload `0`、device idle `0` |
| Idle editor | loaded medium scene，5 min | history/revision 不变；editor-owned live allocation 增长 `<= 1%` 且最终稳定 |
| Soak | U02-U07 随机循环 30 min | 0 crash/assert/validation；live GPU/editor resources 返回稳定区间；history `<= 100` |
| Capacity reject | 65,536 nodes 后 add/merge | reject latency有界，document/projection/render/history hash 全部不变 |

数值预算是本任务的最低门槛；若现有 renderer baseline 已慢于某项，只能把 editor **增量**与总 baseline 分开报告，不能删除 editor 自身预算。

Canonical performance targets：`T-PERF-01` = UI large list + Transform edit，`T-PERF-02` = Gizmo/Material hot path + counters，`T-PERF-03` = idle/30-min soak/capacity reject。三者都必须输出固定环境、raw samples 和 p95，不能只写人工结论。

## 13. Edge Coverage

GSD canonical edge-probe 对带 authored shapes 的 24 条原始需求输入计算出 `110` 个适用边界；原始引擎输出按设计是 `0 resolved / 110 unresolved`，因为输入只负责召回类别，不伪装尚未执行的 resolution。随后本文的规格 authoring pass 在 13.1-13.2 将 `110/110` 全部分配到显式验收或 held-out/property test，即 `110/110 assigned`、`0 unassigned`。`assigned` 不等于实现当前通过；实施完成时报告必须另行显示 `110/110 executed + passed`。

### 13.1 分类覆盖

| Category | 数量 | 具体边界义务 | Verification |
|---|---:|---|---|
| adjacency | 16 | 同一/相邻 index、相同 parent/root、equal timestamp、共享 resource、恰好触及 limit、等价 mesh 候选 | exact unit + reference remap tests；`T-ID/T-EDIT/T-MAT/T-CMD/T-MERGE/T-COMPACT` |
| boundary | 8 | numeric min/max 的 below/at/above、0/负 scale、camera planes、light cone、history 100、node 65,536 | parameterized boundary tests；R06-R14/R22 acceptance |
| concurrency | 21 | 同帧 input、多 background candidate、stale pick/delta、load/close、edit/animation、frame publish/fence | deterministic scheduler/fake clock + Vulkan frame tests |
| empty | 17 | 无 document、0 scene/root/child/material/texture/animation、空名称、空 clipboard、空 history/list | explicit empty fixtures + UI empty-state scripts |
| encoding | 3 | UTF-8/CJK/emoji/combining object name、Unicode/long path、extension/extras text | round-trip bytes/value + UI clip/tooltip tests |
| idempotency | 21 | 重复 select/clear/delta/release/save/compact/Escape-detach、同 revision publish、二次 action | state digest before/after second invocation |
| ordering | 16 | scene/root/child/primitive/resource list、command/delta、merge append、recent MRU、async completion | exact sequence assertions，禁止 unordered compare |
| precision | 8 | quaternion/Euler、matrix decomposition、ray-plane/ring、Snap、camera/light float、scene baseline drift | double oracle + documented tolerances + 1000-cycle tests |

### 13.2 Requirement-to-edge 分配

`E` 表示 acceptance 中已有显式 example/boundary test；`P` 表示由 deterministic property/held-out sequence backstop。每个单元列出的所有 category 都必须执行，不能只取其一。

| Requirement | Applicable categories | Resolution/Test allocation |
|---|---|---|
| R01 | adjacency, concurrency, empty, idempotency, ordering | E: no-op I/O/empty/multi-scene；P: resource/reference combinations |
| R02 | adjacency, concurrency, empty, idempotency, ordering | E: stale/generation；P: randomized operation mapping |
| R03 | adjacency, concurrency, empty, encoding, idempotency, ordering | E: empty/Unicode/order、multi-scene selector；P: large list/state-switch sequences |
| R04 | concurrency, idempotency | E: delayed single/double exclusivity、same-Primitive toggle、blank/repeat selection |
| R05 | adjacency, concurrency, empty, encoding, idempotency, ordering | E: cycle/root/name、target-tail/Undo order、same-parent/root-to-root Move no-op、唯一根 Delete no-op、多 scene D20 matrix；P: random hierarchy sequences |
| R06 | boundary, concurrency, idempotency, precision | E: min/non-finite/activation；P: matrix/quaternion values |
| R07 | boundary, concurrency, idempotency, precision | E: viewport/input/snap；P: generated rays/cameras |
| R08 | adjacency, boundary, concurrency, empty, idempotency, ordering, precision | E: every extension min/mid/max/add/remove；P: extension combinations |
| R09 | adjacency, concurrency, empty, idempotency, ordering | E: shared/no material/no candidate、clipboard canonical/no-prefix/malformed/range/document switch；P: ref graph combinations |
| R10 | adjacency, boundary, concurrency, empty, idempotency, ordering, precision | E: type/plane/zfar boundaries；P: camera/view values |
| R11 | adjacency, boundary, concurrency, empty, idempotency, ordering, precision | E: types/range/cones；P: node/light operation sequences |
| R12 | adjacency, concurrency, empty, idempotency, ordering | E: ancestor truth table/reparent/repeat |
| R13 | adjacency, boundary, concurrency, empty, idempotency, ordering, precision | E: roots/hierarchy/Reset；P: 1000 transforms |
| R14 | adjacency, boundary, concurrency, empty, idempotency, ordering, precision | E: 100/101、499/500/501、唯一根 Delete 与 same-parent/root-to-root Move no-op/redo clear、reparent Undo order；P: mixed command sequences |
| R15 | adjacency, concurrency, empty, idempotency, ordering | E: stale/duplicate/empty delta/order；P: frame schedule permutations |
| R16 | concurrency, idempotency | E: simultaneous input、outside release、Escape-only detach、Escape+release same-frame commit、focus-loss synthetic release、repeated late release |
| R17 | adjacency, concurrency, empty, idempotency, ordering | E: filesystem fault matrix；P: staged resource sets |
| R18 | adjacency, concurrency, empty, idempotency, ordering | E: every remapped chain/second Compact；P: resource graphs |
| R19 | adjacency, concurrency, empty, idempotency, ordering | E: modified no-modal parity/A-B load/failure rollback/0-1-2+ scene switch、D20 current/non-current action matrix；P: lifecycle schedules |
| R20 | concurrency, empty, encoding, idempotency | E: no-document/modal/Unicode/repeated toggle |
| R21 | none proposed by taxonomy | Explicit platform/RHI compile and boundary gates; no fabricated edge |
| R22 | adjacency, boundary, concurrency, empty, idempotency, ordering, precision | E: 0/1/10k/65535/65536/65537；P: capacity operations |
| R23 | none proposed by taxonomy | Explicit evidence completeness meta-gate; no fabricated edge |
| R24 | none proposed by taxonomy | Explicit provenance/license scan; no fabricated edge |

### 13.3 通用失败边界

- 所有 public operation 返回 typed result/error；失败不得依赖日志字符串供调用者判断。
- 所有 mutation 遵守 `validate/preflight -> prepare -> commit -> publish`；任何阶段失败只清理 candidate，不碰 active state。
- 上一条不覆盖参考已定义的两类 UI command envelope：唯一根 Delete 的 domain mutation 是 document no-op，但必须清 selection/redo 并入栈；same-parent/root-to-root Move 的 domain mutation 也是 no-op，但必须清 redo 并入栈且 selection 不变。除此以外不得借此放宽 failure atomicity；多 scene 行为只以 D20 批准结果为准。
- 所有异步 result 带 document/revision/token；与 active 不匹配就丢弃且记录 counter，不能“尽量套用”。
- 所有 index 从文件或 extension 进入前检查负值、上界和 checked arithmetic；任何乘加 overflow 在 allocation 前拒绝。
- 遍历 scene graph 必须显式检测 cycle/duplicate parent/过深路径，不能让递归栈成为验证机制；UI 大树使用 iterative traversal/clip。
- 所有 UTF-8 名称/路径在 Windows API 边界做正确宽字符转换；内部不因截断 UI label 修改 document string。

## 14. 工程红线与禁止项

这些是 implementation-time must-NOT，违反任一项即使 UI 看起来正确也不能合并。

规格阶段的 must-NOT 两阶段 probe（宽召回 -> 删除普通正确性/通用安全项）保留了 3 条本产品特有的数据安全禁止项，均为 `resolved/test`，`0 unresolved`：不得静默丢失用户 authored 数据（N04）、不得把失败事务或半文件发布给用户（N05）、不得把编辑器内部身份/历史元数据注入用户资产（N08）。它们的机械检查已分配给 no-op round-trip、fault injection 和 reserved-key negative tests；实现前暂无可执行 test binary，因此这里只能声称 `3/3 assigned`，不能声称已经通过。路径遍历、权限、注入和资源耗尽等 canon 安全项转交常规 secure review，不在此重复伪造 bespoke prohibition。

| ID | 禁止项 | 机械验证 |
|---|---|---|
| N01 | 禁止从 `GltfModel`、`SceneAsset`、GPU registry 或任何有损投影反向生成可保存 glTF | Save API 只接受 `GltfDocument`；compile-time signature + architecture test |
| N02 | 禁止 UI/panel 直接写 `tinygltf::Model`、`SceneAsset` 或 renderer vector/container | mutation API visibility + source scan + command result tests |
| N03 | 禁止 editor/app/render public interface 泄漏 Vulkan/D3D12/Metal/VMA 类型、include 或 native getter | expanded RHI boundary guard，0 new findings |
| N04 | 禁止静默丢弃未知 extensions/extras、未编辑 collections、外部 bytes 或 sparse/skin/animation 数据 | unknown corpus no-op semantic/byte round-trip |
| N05 | 禁止失败的 load/save/merge/compact publish/提交部分 document、文件、mapping 或 GPU scene | fault injection snapshots exact before/after |
| N06 | 禁止连续 Inspector/Gizmo/material/light 编辑调用 `waitForIdle()`、FullRebuild、完整 geometry/texture upload | runtime counters + performance test，一次命中即 FAIL |
| N07 | 禁止命令/选择长期持有 vector index、element pointer/reference 或把 generation-less handle 当稳定身份 | command type/static review + stale-ID property test |
| N08 | 禁止把 `EditorObjectId`、history、selection 或内部 cache metadata 写进用户 glTF `extras/extensions` | saved semantic scan + reserved-key negative test |
| N09 | 禁止在每帧/每个 slider tick 深拷贝完整 document、vertex/image payload，或无界缓存 label/snapshot | allocation counters + soak growth gate |
| N10 | 禁止用新增 `.rhi-boundary` baseline/allowlist、无理由 skip、golden 更新或 retry 掩盖测试失败 | CI diff policy；baseline/golden 更新需附 reference evidence 和 review |
| N11 | production editor UI 只可使用仓库现有 upstream Dear ImGui API 与 MGIF-owned app/RHI/scene adapter；禁止在 `scene/editor`、`app/editor`、`render/editor`、editor public API/target 或 production transitive dependency 中 include、link、wrap、re-export、复制或要求 `nvpro_core`/`nvcore`/`nvapp`/`nvgui`/任何 `nvvk*`（含 `nvvkhl`/`nvvkgltf`）及参考 Vulkan helper。外部 reference executable/probe 只能作为隔离 evidence；当前无例外，未来例外须事前用户明确批准并记录最小性、替代方案、平台/许可证/binary-size、精确 symbols/targets 与移除计划。editor scope 外既有 `NVVK_SEMAPHORE_DEBUG` 不得借题改动，也不得被误报 | fail-closed production-path token scan + editor target include/link/transitive graph scan + forbidden/allowed negative tests；approved exception count `0` |
| N12 | 禁止复制/实质改编参考源码后删除 Apache-2.0 notice、上游版权或 provenance | SPDX/license/source manifest scan |
| N13 | 禁止把 reference 中无效占位菜单伪装为成功；入口必须路由到有效实现或显示明确 disabled/说明 | UI action result assertion |
| N14 | 禁止运行时状态与 document 发生 silent divergence；检测到 revision/mapping mismatch 必须 fail closed、保留最后有效 scene 并报告 | debug invariant + mismatch injection test |

路径遍历、符号链接、临时文件权限、资源耗尽等通用文件安全问题由常规 secure review 负责；本任务不得因未在表中逐条列出而豁免它们。

## 15. 风险、迁移与回滚

### 15.1 风险登记

| Risk | 概率/影响 | 早期信号 | Mitigation | Fallback/Stop rule |
|---|---|---|---|---|
| K01 有损模型继续成为隐性 authority | 高/致命 | UI 或 Save API 仍接收 `GltfModel/SceneAsset&` | P1/P2 先统一 document，compile-time 封闭 mutation | Gate P2 不通过，禁止开始 UI 编辑功能 |
| K02 tinygltf writer 对未知/外部数据保留不完整 | 中/致命 | no-op semantic diff、binary hash 改变 | P1 建 corpus/comparator；必要时为未知 JSON/value 与 URI staging 增加 preservation layer | 任一静默丢失即停止 Save 上线；保持 viewer-only |
| K03 `SceneAsset`/shader 不支持全部材质扩展 | 高/高 | document 可编辑但 renderer 无变化 | 区分 document-complete 与 runtime projection；按扩展逐项加 material record/shader path | 不允许“能保存但无即时反馈”签收；单项功能保持 disabled 且不计 parity |
| K04 topology/FullRebuild 破坏 GPU lifetime | 中/致命 | Validation VUID、device lost、旧 handle 被复用 | candidate upload + fence-safe swap + deferred retirement；P5 fault tests | 保留旧 active scene，candidate 全清理；任何 validation error 阻断后续 |
| K05 async load/pick/delta 返回顺序竞态 | 高/高 | 快速 A/B Open 后显示 A、旧 pick 选中新对象 | token + document/revision matching、deterministic scheduler tests | fail closed 丢弃 stale result，不尝试 index 迁移 |
| K06 Gizmo 在负/非均匀 parent scale 下数学不稳定 | 中/高 | handle 翻转、NaN、drag 跳变 | double-precision ray math、matrix decomposition tests、reference capture | 对不可可靠分解的 attachment 明确 disabled 并报告；不得写坏 TRS |
| K07 UI parity 测试对字体/DPI 过脆 | 中/中 | golden 频繁因像素字体变化失败 | semantic state + rect tolerance + theme mask，像素只验证结构/非空 | 不放宽 command/focus/enablement exact oracle |
| K08 animation 覆盖编辑/Undo | 高/高 | Undo 后下一帧 transform 又变化 | 编辑 animation-driven channel 时暂停该 playback 或显式锁定；记录状态 | 若无法可靠隔离，禁用该属性编辑并显示原因，不能 silent overwrite |
| K09 macOS/Metal 没有真实 CI 证据 | 中/高 | 只有 Windows 编译推断 | 从 P0 建 macOS contract job、artifact identity | 没有远端 artifact 时状态只能是 `Not Verified`，不能完成 R21 |
| K10 当前 RHI Phase 9 与 editor 同改公共接口 | 高/高 | `RendererFacade/RenderTypes/CMake` 持续冲突 | 先完成/冻结现有 native-leak gate；P5 公共合同单独 review | rebase 后重跑 P0 boundary/build baseline；禁止复制旧 native escape hatch |
| K11 参考文档与源码行为冲突 | 中/中 | Duplicate 等描述不一致 | 可执行行为 > 源码/tests > guide；manifest 记录 evidence | 无法运行参考时以源码+tests 为 provisional，P10 前必须补 runtime evidence |
| K12 参考源码/asset 许可证处理遗漏 | 低/高 | 新文件大段相似却无 notice | 每阶段 provenance review、Apache notice、test asset license/hash | 有疑问的代码重写为 clean-room behavior implementation；不提交受限 asset |
| K13 65,536 节点 UI/递归爆栈或卡死 | 中/高 | 大树展开超时、栈溢出、alloc spike | iterative traversal、ImGui clipper、lazy row cache、preflight limits | 大操作进入 background candidate；无法满足预算则阻断 R22 |
| K14 过度快照导致 history 内存无界 | 中/高 | 100 次 delete 后内存按 asset payload 倍增 | 结构差量/共享不可变 payload/stable IDs，记录 snapshot bytes | snapshot 超预算的命令在提交前拒绝并提示，不能 OOM 后半提交 |
| K15 多 scene row 与 current scene 语义错位 | 高/致命 | 在非当前 row Add/Delete/Drop 后修改了另一 scene，或 Undo 未恢复所有 roots | P0 current/non-current 全矩阵 trace；所有 scene roots/children digest；D20 明确选择 | D20 未批准或 N04 数据完整性无法满足时 Gate P0 停止，禁止实现层级 mutation |

### 15.2 迁移顺序

迁移期间允许新旧**读取路径**并行用于 digest 对照，不允许两套可变 authority 同时存在。

1. `M0 Baseline`：冻结 P0 证据、现有 cache version、加载/渲染 hash 与 RHI boundary report。
2. `M1 Shadow document`：加载时同时创建 read-only `GltfDocument` 和旧运行时结果，比较投影 digest；UI 仍不可编辑 document。
3. `M2 Projection cutover`：所有 load 经 document -> projection；旧 `GltfLoader` API 仅为兼容 wrapper，禁止保存/编辑。
4. `M3 Mutation cutover`：TRS/灯光等已有 UI 先改走 `SceneEditor + command + change set`，删除直接 vector mutation。
5. `M4 UI cutover`：启用新 Scene Browser/Inspector/menu/Gizmo；旧 scene graph/灯光面板不再显示，避免双入口。
6. `M5 Persistence cutover`：在 P1-P9 fault/round-trip gate 全绿后才启用 Save/Merge/Compact；此前 UI 可存在但必须 disabled 并标注未通过 gate。
7. `M6 Cleanup`：移除 `GltfModel` UI ownership、旧 selection index、legacy mutation 和过渡 digest code；保留只读 compatibility loader 仅在确有调用方时。

建议新增 compile option `DEMO_ENABLE_SCENE_EDITOR`：desktop 默认 `ON`，Android 固定 `OFF`。它只能切换“带编辑 UI的 document viewer”和“无编辑 UI的 document viewer”，不能切回旧 mutable `GltfModel` authority。P10 后不得以此 flag 规避桌面 editor CI。

### 15.3 Cache 与格式迁移

- `SceneAssetSerializer` 结构一旦改变必须 bump explicit format version；旧 cache 只做 clean miss 并从 source glTF 重建，不做猜测式字段读取。
- cache key 必须包含 source dependencies、projection/schema version 和所有影响 runtime payload 的 build options；不包含 session ID/history/selection。
- source 缺失但只有旧 `.sceneasset` 时可继续按现有 viewer policy 报错/只读加载，但不得提供 Save glTF，因为不存在无损 authority。
- 新 editor 不原地升级用户 glTF；只有显式 `Save As` 成功才创建/替换文件。

### 15.4 实施回滚

- 每个 P 阶段使用独立、可构建、测试全绿的提交集合；不要把 document、renderer、UI 三层塞进一个不可拆提交。
- P1-P2 回滚：关闭 editor UI，旧 viewer 可暂用兼容 wrapper；任何已生成的新 cache 视为可删除产物并自动重建。
- P3-P5 回滚：保留 `GltfDocument` read-only load/projection，关闭 mutation command registration；不得恢复直接 mutation。
- P6-P8 回滚：关闭 picking/overlay/panels，仍可通过 CPU/controller tests验证文档；不回退 RHI public contract。
- P9 Save/Merge/Compact 出现回归：立即在 command registry disabled 三个入口，保留 Open/read-only editor；先修数据安全再恢复。
- 已显式保存的用户文件只通过原子写与可选外部版本控制恢复；实现回滚不得删除或覆盖用户产物。

### 15.5 角色与 Review Gate

| Role | Ownership | Required approval |
|---|---|---|
| Document/Editor Core | P1-P4、P9 的 model、I/O、commands、remap | data-loss/round-trip/index invariant review |
| Renderer/RHI | P2、P5-P7 的 projection、delta、picking、overlay、lifetime | RHI boundary + Vulkan validation review |
| Editor UX | P0、P8 的 manifest、panels、menus、focus/theme | reference action trace + DPI/interaction review |
| Verification | P0、P10、fault/property/perf/CI artifacts | 独立确认 tests 真正覆盖 requirement，不接受实现者自签 waiver |

同一人可以承担多个 role，但 data-loss、RHI boundary 和最终 parity 证据至少需要一次独立 review。

## 16. 追踪、决策与证据

### 16.1 Requirement Traceability Matrix

| Req | Primary phases | Minimum automated evidence | Runtime/visual evidence |
|---|---|---|---|
| R01 | P1-P2 | `T-DOC-01/02` | Open/no-op Save/reload Vulkan scene |
| R02 | P1-P3 | `T-ID-01` + property | stale selection/pick after delete/compact |
| R03 | P8 | `T-UI-01..05`, `T-UI-15` | U01/U08 screenshots/state |
| R04 | P6/P8 | `T-SEL-01/02` | U03 single/toggle/blank + U05 double-click + tree/list/Inspector/highlight trace |
| R05 | P3-P4/P8 | `T-EDIT-01..07` | U02 |
| R06 | P3-P4/P7-P8 | `T-TRS-01`, `T-CMD-02` | U03/V03 |
| R07 | P7 | `T-GIZMO-01..05` | V01-V04/V07 |
| R08 | P3-P5/P8 | `T-MAT-01..04` | U04/V05 |
| R09 | P3-P5/P8 | `T-MAT-05..07` | U04 OS clipboard/assignment + save-reload |
| R10 | P3/P8 | `T-CAM-01/02` | U05 |
| R11 | P3-P5/P8 | `T-LIGHT-01..03` | U05/V05 |
| R12 | P3/P5/P8 | `T-VIS-01` | U05/V05 |
| R13 | P3/P7-P8 | `T-TRS-02` | U03 scene-root capture |
| R14 | P4 | `T-CMD-01..06` | menu descriptions + U02-U05 |
| R15 | P2/P5 | `T-DELTA-01..05` | V05/V06 counters/validation |
| R16 | P6-P7 | `T-SEL-02`, `T-GIZMO-01..05` input state tests | U03 Escape-only/same-frame release/outside-release/focus-loss + V07 |
| R17 | P1/P9 | `T-LIFE/IO fault` | U06/U07 output artifacts |
| R18 | P3/P9 | `T-MERGE/COMPACT` | U06/V06 |
| R19 | P8-P9 | `T-LIFE-01..05`, `T-UI-15` | U01 scene switch/U06/U07 |
| R20 | P0/P8 | command/UI state manifest, `T-UI-01..15` | U01-U08, all DPI screenshots |
| R21 | P0/P5-P8/P10 | `T-RHI-01..03`, `T-LIC-01`, RHI + dependency boundary | Vulkan report + D3D12/Metal/Android artifacts + production editor target/include/link/transitive scan |
| R22 | P1-P10 | limit/property/perf | performance JSON + soak |
| R23 | P0/P10 | report schema completeness test | signed artifact index |
| R24 | P0/all/P10 | `T-LIC-01`, provenance + dependency scan | `LICENSE-SOURCES.md` review + external-evidence isolation + zero approved production exceptions |

### 16.2 决策日志

| Decision | 结论 | 原因/后果 |
|---|---|---|
| D01 Scope | 完整覆盖参考 Scene Asset Editor，不做 MVP 裁剪 | 用户明确要求；实施按阶段交付但最终无 Missing/Partial |
| D02 Platform | RHI-neutral desktop；Windows Vulkan runtime；D3D12/Metal contract compile；Android 无 editor UI | 与 MGIF 现有 RHI milestone 一致，避免把 editor 绑回 Vulkan |
| D03 UI | layout/menu/shortcut/context/state/result parity；MGIF visual theme | 锁定行为又保留产品身份，不做 NVIDIA palette clone |
| D04 Authority | `GltfDocument(tinygltf::Model)` 唯一可保存可变 authority | 当前 `GltfModel/SceneAsset` 有损，无法满足 non-destructive save |
| D05 Runtime | `SceneAsset` 是 renderer-facing derived projection | 保留 MGIF GPU-ready 架构与 cache，不让 renderer 反向拥有文档 |
| D06 Identity | generation-based `EditorObjectId`，不以 vector index 持久标识 | delete/merge/compact/undo 后避免 alias/stale reference |
| D07 Mutation | UI -> command/SceneEditor -> change set -> renderer | 统一 undo、validation、atomicity 和增量更新 |
| D08 History | reference 100/500 ms/linear/clear lifecycle；visibility/scene transform 非 undo | 严格遵循参考 observable behavior，不虚构增强 |
| D09 Duplicate | 以源码为准复制 node subtree，mesh/material 共享；skin/animation node map 修复 | 用户指南“复制 geometry/material”与实际实现冲突，runtime evidence/source 优先 |
| D10 Save | validate + same-directory staging + atomic publish；成功后才 clean | 数据安全是一票否决条件 |
| D11 Reference precedence | executable behavior > source/tests > user guide | 处理文档/源码漂移，所有冲突进入 manifest |
| D12 Gizmo | behavior port + clean RHI decomposition，不逐文件照搬 Vulkan code | 复刻交互同时守住 MGIF RHI 边界 |
| D13 Evidence | semantic state + runtime counters + screenshot，三者缺一不可 | 避免“只有 UI”或“只有 unit tests”的假完成 |
| D14 License | copied/adapted code保留 Apache notice 和 upstream manifest | 满足 Apache-2.0 义务并隔离非目标依赖 |
| D15 Picking | delayed single 选择/toggle；double 只改 camera center+speed；外部 focus 不强制切 tab | 以 `UiMouseState` 和 `mouseClickedInViewport()` 的可观察行为为准，不能改成即时 single 或双击前先选中 |
| D16 Clipboard | OS text `gltf_material:<document index>` 是 parity 例外；稳定 ID 只留进程内；malformed suffix fail closed | 参考通过 ImGui system clipboard 跨控件传递 index；Paste 每次按 active document 验证且不进 history。参考未定义的 malformed `atoi` 输入按 5.4 安全例外拒绝 |
| D17 Safety boundary | 成功路径 exact parity；candidate/atomic/revision/fence 等只做内部失败安全 | 防止“安全增强”被用来新增 modal/dirty badge，也防止为了表面复刻而接受数据损坏 |
| D18 Observable quirks | 唯一根 Delete 仍清 selection/redo 并入 history；same-parent/root-to-root Move 仍清 redo 并入 history；reparent Undo 追加回尾部；Escape-only 与 Escape+release 同帧分开验收 | strict result parity 优先于“更合理”的事务优化；以参考 runtime > source/tests 的证据顺序定案 |
| D19 Multiple scenes | selector 固定在 Settings，条件为 `scenes.size() > 1`，current-scene switch 为 non-history runtime operation | 防止误放 Scene Browser、误写 `defaultScene`，并用 P0 trace 锁定 selection/Gizmo/camera/focus |
| D20 Multi-scene structural edit | **RESOLVED: `mgif_safety_exception`**。Owner approval identity `MGIF project owner (user)`，approval text `参考你的建议`；原消息 timestamp 不可取得，`2026-08-22T16:55:24Z` 仅为 durable coordinator attestation。clicked scene ID/occurrence routing；`all_scenes` snapshot/restore；shared occurrence consistency；cycle/multiparent/stale occurrence 在 command/history entry 前原子拒绝；bounded iterative traversal；UI entry/label/layout/shortcut/modal/focus/enablement/history description 保持 reference parity | 参考的 current-scene routing、multi-parent/cycle 和 crash 路径已由 domain 与 real-UI evidence 绑定；选择只授权所列 result safety differences，不得扩大为一般 parity waiver |
| D21 Editor dependency boundary | **LOCKED，无批准例外**：production editor UI 使用仓库现有 upstream Dear ImGui API + MGIF-owned app/RHI/scene adapter；禁止 nvpro/nvcore/nvapp/nvgui/nvvk/reference Vulkan helper 的 direct、wrapper、public、target 与 transitive production dependency。reference native dependencies 只留在隔离 external evidence；unrelated renderer `NVVK_SEMAPHORE_DEBUG` 保留 | 防止以 UI parity 为名把参考 framework/renderer 带入 MGIF、破坏 RHI/平台/许可/体积边界；任何未来例外必须先由用户按 N11 的完整影响与移除字段明确批准 |

### 16.3 歧义报告

| Dimension | Score | Minimum | Status | 主要结论与证据门 |
|---|---:|---:|---|---|
| Goal Clarity | `0.96` | `0.75` | PASS | 完整能力、四类等价和最终 artifact 均已定义 |
| Boundary Clarity | `0.94` | `0.70` | PASS | editor 与 renderer 非目标功能、动画 authoring、Android/D3D12/Metal 边界明确 |
| Constraint Clarity | `0.88` | `0.65` | PASS | 单 authority、RHI、limits、atomicity、性能、license 已锁定 |
| Acceptance Criteria | `0.78` | `0.70` | PASS | 24 条逐项验收、42 项矩阵、110 edges、test/evidence 规则已定义；D20 fail-closed，不允许 planner 假设 |
| **Weighted ambiguity** | **`0.10`** | **`<= 0.20`** | **PASS** | `1 - (0.35*0.96 + 0.25*0.94 + 0.20*0.88 + 0.20*0.78) = 0.097 ~= 0.10` |

### 16.4 需求确认记录

| Round/Perspective | 问题 | 锁定回答 |
|---|---|---|
| Researcher | 参考能力与 MGIF 当前差距是什么 | 参考具有完整 glTF document editor；MGIF 只有有损 load/projection 和基础 node TRS UI |
| Scope/Boundary | 只做核心功能还是完整覆盖 | 完整覆盖参考项目 Scene Asset Editor 能力 |
| Architecture | 是否接受 RHI-neutral 方案 | 采用本文建议：Windows Vulkan 完整验收，D3D12/Metal 合同编译，Android 不提供 editor UI |
| UI | “复刻”的界限是什么 | 布局、菜单、快捷键、上下文菜单、交互状态和结果一致；视觉主题使用 MGIF |
| Failure analysis | 什么会让结果被拒绝 | 有损保存、双 authority、stale index、partial publish、Vulkan leak、热路径 full rebuild、无证据 parity |
| Reference edge audit | 多 scene row 的 target 与 current scene 源码错位如何处理 | P0 executable trace 后由用户在 exact defect parity 与逐项列明的 safety exception 中选择；未决即停止，不允许实现者代选 |
| Editor dependency boundary | UI 复刻是否复用 reference nvpro/nvapp/nvgui/nvvk framework/helper | 不复用；锁定为 repository upstream Dear ImGui + MGIF-owned adapters。reference executable/probe 原生依赖仅作为 external evidence，当前没有 production exception |

### 16.5 证据目录合同

每次 P10 候选运行创建不可混用的独立目录：

```text
out/validation/gltf-editor-parity/<UTC timestamp>-<Demo SHA12>/
  identity.json
  requirement-results.json
  feature-manifest-results.json
  edge-coverage-results.json
  ctest.xml
  rhi-boundary-report.json
  platform/
  ui/<script>/<viewport>-<dpi>/state.json
  ui/<script>/<viewport>-<dpi>/*.png
  vulkan/<case>/{stdout.log,stderr.log,result.json,*.png}
  roundtrip/<case>/{semantic-diff.json,output.gltf|output.glb}
  performance/{machine.json,metrics.json}
  license/{sources.json,scan.json}
  summary.json
```

`identity.json` 至少绑定 git HEAD、dirty diff hash、Demo 路径/SHA-256、CMake generator/config/options、compiler、Vulkan SDK、GPU/driver、OS、DPI 和 reference commit。不同 executable SHA、不同 git diff 或不同机器的数据不得合并成一次“通过”。

### 16.6 变更控制

- 发现 reference 新行为时，先在 parity manifest 添加 failing row/evidence，再修改本任务书需求和计划；不得直接在代码里悄悄选择语义。
- 任何范围缩减、平台降级、验收 waiver、容差放宽或 non-undo 行为变化需要用户明确确认并更新“已锁定决策”。
- 实现细节可在不改变 Current/Target/Acceptance、依赖方向和红线的前提下调整；文件名微调不需要变更需求。
- reference baseline 变更到其他 commit 视为重新基线，必须重跑 P0，不可沿用旧截图/manifest。
- D21/N11 的任何放宽都不是实现细节：必须在 dependency edge 落地前取得用户明确批准，并把最小性、替代方案、平台/许可证/binary-size 影响、精确 symbols/targets、owner/date removal plan 写入规格、manifest 和 acceptance oracle；没有这些字段就保持 fail closed。

## 17. Definition of Ready 与 Definition of Done

### 17.1 Definition of Ready（开始 P1 前）

- [ ] 用户确认本文档为本任务唯一规格基线，或所有批注已经合并。
- [ ] 参考 commit `50373c7` 可构建/运行，或已记录无法运行的原因与源码/tests provisional evidence。
- [ ] MGIF 当前 RHI Phase 9 公共合同状态已冻结，重叠文件 owner/merge 顺序明确。
- [x] P0 reference action/state/screenshot manifest、test corpus 与 conservative rights-reserved development policy 已建立；release root-license selection 仍未完成。
- [x] same-parent/root-to-root Move、reparent Undo order、Escape+release 同帧和 multi-scene structural matrix 已取得 bounded executable evidence；D20 已选择 `mgif_safety_exception`。
- [ ] 现有 MGIF Debug build、CTest、RHI boundary 和 Vulkan smoke baseline 可复现。
- [x] P0 dependency validator 正/负例通过；Dear ImGui/MGIF-owned adapter boundary、per-build resolved identity 与 no-override contract 已锁定，production dependency exception registry 为空。
- [ ] Windows Vulkan 验收机与 macOS/Android CI 执行环境已指定。

### 17.2 Definition of Done：功能与数据

- [ ] R01-R24 每条在 `requirement-results.json` 中为 `PASS`，包含实际 test/artifact 路径。
- [ ] F01-F42 每项从 `Missing/Partial` 变为 `PASS`，无空白、`Not Run` 或无用户批准的 waiver。
- [ ] `.gltf`、`.glb`、external/data URI/embedded、unknown extension/extras、skin/animation、多 scene no-op round-trip 通过。
- [ ] Node/Primitive/Mesh/Material/Camera/Light selection 与所有 hierarchy/material/light/camera/visibility 操作通过。
- [ ] Undo/Redo 100-entry、500 ms、branch/clear/clean revision、reference non-undo exclusions、唯一根 Delete no-op、same-parent/root-to-root Move no-op 和 reparent Undo append-order 的 exact 结果通过。
- [ ] current/non-current scene row/node 的 Add/Light/Duplicate/Delete/Rename/Add Child/drop/Undo/Redo 全矩阵符合已批准 D20，且所有 scene roots/children、selection、history 与 runtime digest 一致。
- [ ] Merge/Compact 所有标准/已知 extension 引用重映射、idempotency 与失败回滚通过。
- [ ] 唯一用户入口 `Save As` 的每次对话框、validation、atomic write、external resources、failure matrix 与 reload 通过；没有额外 `Save` 命令。

### 17.3 Definition of Done：UI 与交互

- [ ] 9.2 菜单/快捷键、9.3 toolbar、9.4 context menu、9.5 Inspector 和 9.6 状态机逐项 exact oracle 通过。
- [ ] Scene Browser 顶部 Asset/条件 Animation/Variants、Release Graph/List tabs、Debug-build-only Debug tab、Settings 条件 `Multiple Scenes` selector 及 Inspector 六种选择上下文通过；scene switch 逐 scene render 与状态 trace 通过，外部 focus 不强制切 tab。
- [ ] Viewport delayed single/double、same-Primitive toggle、blank clear、double-click camera center/speed，以及 OS clipboard canonical/invalid/cross-document 状态通过。
- [ ] Gizmo 全部 component、Grid、Snap、hover/active/depth/x-ray/parent-space/input capture 通过；outside release 提交，Escape-only、Escape+release 同帧、focus loss 有/无 synthetic release 的 selection/TRS/history 与参考一致且均不 rollback。
- [ ] U01-U08 在 `1280x720`、`1920x1080`、`2560x1440` 和 100/125/150/200% DPI 的适用组合通过。
- [ ] MGIF theme token 生效，轴色语义正确；无文本/控件重叠、不可辨识截断、空白 viewport 或动态布局跳动。
- [ ] 参考 action trace 与 MGIF action trace 的 command、focus、enablement、selection、document result 一致；若 D20 选择 safety exception，仅允许该决策逐项列出的 result 差异，UI layout/entry/label 仍须一致。

### 17.4 Definition of Done：架构、平台与质量

- [ ] `GltfDocument` 是唯一可保存 mutable authority；source scan/compile contract 未发现第二 authority。
- [ ] UI 无 direct document/SceneAsset/renderer mutation；所有可撤销 edit 经 command/SceneEditor。
- [ ] `MinimalLatestApp.h/.cpp` 只做组装与生命周期，不含完整 editor panels/domain mutation。
- [ ] 连续 edit 的 full rebuild、geometry/texture upload、device idle counters 全为 `0`。
- [ ] Windows Vulkan build、Validation+sync validation、V01-V07、30-min soak 均为 PASS，零 VUID/SYNC-HAZARD/assert/crash/device lost。
- [ ] D3D12 与 Metal 的 public contract/compile artifact 为 PASS；无证据的平台未被标为通过。
- [ ] Android build 为 PASS，且不包含 desktop editor UI/overlay orchestration symbols。
- [ ] 扩展后的 RHI boundary guard 为 PASS，新增 public/editor 文件零 backend native leak、零新增 baseline waiver。
- [ ] production editor source/public API/target include+link/transitive closure 只依赖仓库现有 upstream Dear ImGui 与 MGIF-owned adapters；N11 forbidden finding 为 `0`，approved exception count 为 `0`；external reference evidence 和 unrelated renderer token 的 allow-case 仍通过。
- [ ] 12.8 所有性能/容量/内存阈值通过，并绑定验收机与 executable SHA。
- [ ] Edge obligations `110/110 executed + passed`，没有 unresolved edge。
- [ ] N01-N14 机械检查/判断 review 全通过。
- [ ] Apache-2.0 notice、SPDX、`LICENSE-SOURCES.md` 和 test asset license/provenance 全通过。

### 17.5 最终签收

- [ ] `tools/run_editor_acceptance.ps1` 在全新或已验证 canonical build tree 中返回 `0`。
- [ ] `summary.json` 汇总 24/24 requirements、42/42 features、110/110 edges、所有 platform/test/performance gate 为 PASS。
- [ ] 证据目录满足 16.5，所有文件可从 identity 中重放，且没有混用不同 SHA/dirty diff 的 artifact。
- [ ] reviewer 独立抽查至少一个 round-trip failure case、一个 randomized remap seed、一个 Gizmo visual case和一个 FullRebuild fence case。
- [ ] 文档、用户指南、开发架构说明与实际菜单/快捷键/限制一致；不再声称 reference parity 以外的未实现增强。
- [ ] 过渡 flag/wrapper/旧 direct-mutation UI 已按 M6 清理，或每一项保留都有明确调用方、owner 和后续移除条件。

全部 checkbox 只能由实际证据从空白改为完成。阶段实现结束、界面看起来相似、单个平台能运行或手工演示成功，均不等于本任务完成。

---

**任务状态**：P0 evidence/integration 已关闭到 P1 engineering-ready 边界；D20 已解析为 `mgif_safety_exception`。O04/O06 保持 conflict，O05/O07/O08/O09/O10 保持 bounded `native_partial`；F42/R24 因根许可证未选择而继续 BLOCKED，最终产品/发布尚未完成。

**下一执行入口**：可按 P1 从 `GltfDocument` 开始 engineering implementation；不得把 engineering readiness 解释为 F42/R24 或 release PASS，也不得从 Scene Browser UI 或 reference dependency integration 直接开工。
