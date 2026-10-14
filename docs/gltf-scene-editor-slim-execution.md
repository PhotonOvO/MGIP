# glTF Scene Editor 精简实施档

状态：当前开发执行入口（2026-09-09）

本文档用于把功能开发和完整验收拆开。原始任务书、历史账本、失败 Gate 和固定策略继续保留，本文档不改变它们的历史含义。

当前采用用户确认的 `P8-P10-Fast` 工程收敛模式：P8、P9、P10 都先交付可用功能，再把完整 parity 证据留作后续工作。

## 目标

先在 Windows/MSVC + Vulkan 上落地一个可用的 glTF 场景编辑器闭环：加载、编辑、渲染更新、撤销/重做、保存、重新加载。功能完成后再补完整 parity 证据矩阵。

## 必须先完成的功能

### S1：文档和生命周期

- `GltfDocument` 作为唯一可保存编辑权威。
- 普通生产 `Demo` 支持 `--editor-empty` 和 `--editor-scene=PATH` 直接进入空编辑器或指定 glTF 场景。
- New、Open、替换、无效文件失败回滚、当前文档状态和错误状态。
- `.gltf`、`.glb`、external/data URI 资源的加载与 Save As。
- 保存后重新加载并进行语义比较。

完成条件：成功路径可用；候选文件失败时旧文档、路径、revision 和选择保持不变。

### S2：场景浏览和选择

- Scene Browser 的场景/节点/primitive/material/camera/light 基本列表。
- Node、Primitive、Material、Camera、Light 选择统一传播到 Inspector 和 viewport 高亮。
- 当前 scene 切换和空文档状态。

完成条件：列表、Inspector、viewport 使用同一个 selection snapshot；删除或替换后旧选择失效并安全清空。

### S3：结构和变换编辑

- 新增 child、重命名、复制、删除、reparent。
- 数值 TRS 和 Transform Gizmo。
- Undo/Redo、连续变换合并、Escape/focus-loss 不产生错误提交。

完成条件：文档层级、projection、运行时世界矩阵和保存结果一致。

### S4：材质、相机、灯光和可见性

- primitive 材质指派和主要材质参数。
- camera apply/set 与基本 camera 属性。
- point、directional、spot light 的创建和编辑。
- node visibility 及父子继承。

完成条件：下一可提交帧反映修改；连续属性编辑不触发完整 geometry/texture 重传。

### S5：Merge、Compact、Save As

- Merge 的基础索引重映射。
- Compact 的可达资源清理和重复执行稳定性。
- Save As GLB、失败回滚、reload。
- 成功的结构性操作按合同清理或保留 history。

完成条件：merge/compact/save/reload 后语义、节点数量、材质/灯光引用和运行时 digest 一致。

### S6：应用组装和稳定性

- `MinimalLatestApp` 只负责组装和生命周期协调。
- editor controller、document、projection、renderer adapter 的边界固定。
- Windows Debug 和 Release 均可构建；Debug Vulkan smoke 无崩溃、assert 和 validation error。

完成条件：S1-S5 的最小端到端脚本连续运行；所有失败路径保持旧状态。

普通 Windows 入口：

```text
Demo.exe --editor-empty
Demo.exe --editor-scene=tests/fixtures/gltf_editor/multi_scene_oracle.gltf
```

不带这两个参数时保留现有默认场景启动行为。

## 精简验证集

每个功能包只保留能够阻止错误实现的检查：

1. 相关 CPU/editor tests。
2. Windows/MSVC Debug 构建和一次 Vulkan Validation smoke。
3. 一次 1280x720 的 U01-U08 controller-bound live replay。
4. Save As -> reload semantic compare。
5. 关键失败路径：invalid open、save failure、merge failure、NoDocument、stale selection。

在 S1-S6 完成前，不重复运行完整的三分辨率乘四 DPI 矩阵，也不把每个 Debug/Release 组合都当作开发阻塞条件。

## P8-P10-Fast 收敛规则

### P8-Fast：可用 UI 和编辑行为

- Scene Browser、Inspector、统一 selection、菜单命令和 Gizmo controller action 可执行。
- U01、U02、U03、U04、U05、U08 的 direct controller replay 通过一次 1280x720 Debug 运行。
- 关键 UI/editor CPU 测试返回 0。
- full OS click/dialog、完整视觉像素对照和全 DPI 组合转入 backlog。

### P9-Fast：生命周期和持久化

- U06 覆盖 Merge、Compact、Save As、reload，semantic digest 一致。
- U07 覆盖 invalid open/save/merge、旧状态保留、NoDocument 和 Exit。
- 普通 Demo 支持拖放 `.gltf`/`.glb` 执行 Open，按住 Shift 拖放执行 Import/Merge。
- 只保留一轮关键失败路径；不执行完整 fault-injection 矩阵和 cross-volume/平台 I/O 组合。

### P10-Fast：构建和交付收敛

- 当前源码通过 Windows/MSVC Debug 和 Release 构建及 editor dependency preflight。
- 普通生产入口支持空文档启动和指定场景启动；不依赖 live replay harness 才能进入编辑器。
- 关键 renderer integration、transaction、mutation、merge、compact、round-trip、selection/picking 测试返回 0。
- 本地 progress ledger validator 通过。
- 生成一份绑定 executable/source identity 的执行记录。

P8-Fast、P9-Fast、P10-Fast 全部满足后，停止日常测试扩展并进入交付整理。以下项目不阻塞这三个 Fast 阶段：完整 OS click/dialog、全分辨率/DPI 组合、30 分钟 soak、65,536 节点容量、Metal/D3D12/Android、RenderDoc 全帧审计、110 条 edge obligation、license/distribution Gate。

### 日常构建（2026-09-10）

- CMake 的 `BUILD_TESTING` 默认关闭；Visual Studio 的 `x64-debug`、`x64-release` 两种入口（Presets / CMakeSettings）均显式设置为 `OFF`，重新配置即可移除日常工程中的测试、legacy test core 和 live/OS replay 目标。
- 测试源码保留供后续回归使用；显式选择 `x64-debug-tests` 或 `x64-release-tests` preset 才启用测试，并使用独立的仓库外构建目录。
- 普通构建默认 `DEMO_SKIP_EDITOR_DEPENDENCY_PREFLIGHT=ON`，不创建 `scene_editor_dependency_preflight` 项目。可直接使用仓库内的 `build/` 和 Visual Studio/MSBuild 生成器；RHI boundary guard 仍执行。
- 配置图验收只在显式设置 `DEMO_SKIP_EDITOR_DEPENDENCY_PREFLIGHT=OFF` 时启用，仍要求仓库外 Ninja 构建树。独立的 `*-tests` presets 显式启用它；日常构建成功不代表这项验收已执行。
- `scene_*_runtime`、`scene_editor_document`、`scene_editor_core` 等名称是编辑器生产实现库，属于 `Demo.exe` 的链接闭包，不是单元测试；VS 中统一放在 `Runtime/Scene Editor` 文件夹，启用后的 preflight 放在 `Validation` 文件夹。

已有的 Visual Studio 解决方案若缓存了旧的预检开关，可在仓库根目录运行一次：

```bat
cmake -S . -B build -DBUILD_TESTING=OFF -DDEMO_SKIP_EDITOR_DEPENDENCY_PREFLIGHT=ON
cmake --build build --config Debug --target Demo
```

该命令沿用 `build` 中的生成器，无需删除构建目录或切换到 Ninja。

Visual Studio/MSBuild 的 Demo 调试工作目录跟随可执行文件目录（Debug 为 `build\Debug`，Release 为 `build\Release`），与 `copy_demo_resources` 的输出位置一致。默认 `resources/GLTF_Sponza/sponza.gltf` 从该目录解析；命令行手动启动时也应先进入可执行文件目录。

默认 Sponza 已在该 Debug 工作目录实际完成加载和 5 帧 Vulkan automation（关闭 UI、post 和 DDGI），包含 `scene-ready`、首帧、`complete` 和关闭日志。此机器本次首次出帧约 85 秒；初次 45 秒观察窗口不足，扩大窗口后完成。日志：`out/codex-patch-tmp/scene-startup-20260910-r2.stdout.log`。本次修复针对启动工作目录，未做加载性能优化。

2026-09-10 已在原 `G:\MGIF\build`（Visual Studio 17 2022）重新生成并完成 Debug 全部生成，退出码为 `0`，产物为 `build\Debug\Demo.exe`；解决方案及 Demo 依赖中已无预检、测试和回放项目。构建日志：`out/codex-patch-tmp/msbuild-daily-debug-20260910.log`。独立外部 Ninja 预检仍通过；验收静态模型已改为读取实际构建缓存中的测试/预检开关，避免把开启测试的配置误判为默认关闭测试的配置，对应回归检查通过。

### Inspector 连续编辑修复（2026-09-10）

数值框只点击、不修改再释放时，现在也会结束连续编辑；字段切换、选择变化或关闭 Inspector 时会先结束原字段的编辑。重叠 begin 或无效 preview 被拒绝后，仍在进行的编辑保留初始投影，避免后续预览修改文档却无法发布，进而导致拾取版本不一致。

Windows/MSVC 的 `editor_controller_transaction_tests` 已通过，新增用例覆盖真实 ImGui 鼠标点击/拖动、无变化操作不入历史、拒绝后继续预览、单次 Undo，以及新拾取成功/旧拾取仍被拒绝。原 `G:\MGIF\build` 的 Debug Demo 已重新编译链接成功，日志为 `out/codex-patch-tmp/inspector-recovery-production-build.log`。

### BaseColor 色盘卡顿修复（2026-09-10）

在真实 ImGui 色盘中复现了 Sponza 的同步阻塞：两次拖动更新分别耗时 46,489 ms 和 45,071 ms。BaseColor 专用投影现在复用已解码的几何和纹理，并验证变更证据、当前材质值及版本；其他变更仍回到完整投影。文档编码复用有容量上限的只读缓冲区/图像 URI 缓存（最多 512 MiB、128 项），Windows 使用系统 SHA-256/Base64，保留可移植回退。增量发布也省去了不需要的完整快照拷贝。

原始规范化字节格式及摘要语义保持不变。验证覆盖 SHA-256/Base64 标准向量、分块输入、用户文本与临时编码标记冲突、glTF/GLB 保存重载、快速/完整投影逐字节一致，以及色盘拖动和单次 Undo。独立 Windows/MSVC Debug 的 controller transaction、scene change set、document roundtrip、renderer integration 四项均返回 `0`。

同一 Sponza 色盘复测的控制器更新为 1,597 ms 和 1,497 ms（不包含实际 GPU 提交耗时），打开文档为 4,579 ms。Debug 大场景仍有延迟，不能将此结果称为流畅帧率。前后记录分别为 `out/codex-patch-tmp/base-color-sponza-profile.log` 与 `out/codex-patch-tmp/base-color-sponza-final.log`。

原 MSBuild Debug 源码已编译完成，但 `build\Debug\Demo.exe` 被占用，覆盖链接返回 `LNK1168`。已将同一批生产编译产物链接到 `G:\MGIF\build\Debug\Demo.basecolor-fixed.exe`，链接及 `--help` 启动检查均返回 `0`；调试符号为同目录的 `Demo.basecolor-fixed.pdb`。关闭旧程序后可启动修复版，或通过正常构建重新生成原 `Demo.exe`。

## 暂缓项目

以下项目保留在最终验收 backlog，不阻塞功能实现：

- 完整 OS click/dialog replay 和 native dialog 证明。
- 三种分辨率乘四种 DPI 的全组合截图。
- 30 分钟随机 soak、65,536 节点性能容量门槛。
- Metal、D3D12、Android 的完整平台证明。
- 完整 RenderDoc 逐帧审计、110 条 edge obligation 全量封存。
- P8/P9/P10 Gate 账本晋级和 release/distribution 结论。

这些项目只能在 S1-S6 通过后集中处理；它们不再参与日常功能开发循环。

## 当前执行顺序

1. [x] 修复并完成 U01 的无文档启动、New、invalid Open、valid multi-scene 和 scene switch。
2. [x] 修复并完成 U03 的 fixture/binding、Gizmo 与 history 边界。
3. [x] 重新绑定最新 executable/source identity，重跑 U04-U08 的最小 1280x720 Debug 矩阵。
4. [x] 对 S1-S5 做一次 Save As/reload 和失败回滚串联测试。
5. [x] P8-Fast、P9-Fast、P10-Fast 最小收敛完成；完整 OS/DPI/soak/platform 证据转入后续 backlog。

## 记录规则

- 本档是开发执行计划，不把 direct-typed replay 称为完整 OS/UI parity。
- 不修改失败的 P8-90、G8 或其历史 receipt。
- 不修改 F42、R24、Metal、release/distribution 固定策略。
- 新证据必须绑定同一 source identity 和 executable identity；旧证据只能作为诊断参考。

## 最新执行记录（2026-09-09）

精简验证集已完成一轮 Windows/MSVC Debug 执行（scene-switch 修复前的基线轮次）：

- U01-U08 在 `1280x720` 下全部返回 `PASS_BOUNDED_LIVE_PROOF`。
- 八个 replay 使用同一 Debug executable SHA-256：`8f155bcadb97b2df85da174e7545e7e7e449312f50ea0c0ee978ee9f10766f4a`。
- 八个 replay 使用同一 source identity：`c8b2e227ad8f40b3d28c2b6ff1dccadc8cda424b87a947a9bb0a05d6e10c02d6`。
- 证据目录：`G:\Temp\mgif-slim-u01-20260909` 至 `G:\Temp\mgif-slim-u08-20260909`。
- U06 已生成 `saved.glb`，Save As 后重新加载的 semantic hash 和 document digest 保持一致。
- U07 已覆盖 invalid merge、invalid save、invalid open、NoDocument 和 Exit；失败动作保持旧状态，最终脚本完成。
- 关键 CPU/editor 测试通过：`editor_ui_replay_tests`、`editor_controller_transaction_tests`、`scene_editor_mutation_tests`、`scene_merger_tests`、`scene_compactor_tests`、`gltf_document_roundtrip_tests`、`editor_selection_picking_integration_tests`、`editor_renderer_integration_tests`。
- Debug 和 Release 的 `editor_ui_live_replay_acceptance` MSVC 增量构建均通过；Release executable SHA-256 为 `3ae7a4208ce811df0d7706fa5af58e6438d1190062d9b3c13bcd0f1534dee6f8`。

随后补齐了 U01 的多场景切换和 renderer authoritative rebuild：

- U01 最新 controller replay 为 7 步，包含 `activate-scene-one` 与 `activate-scene-zero`，全部 accepted，最终状态为 `ReadyClean`；逻辑证据目录为 `G:\Temp\mgif-slim-u01-scene-switch-20260909-r4`。
- scene switch 使用 `multi_scene_oracle.gltf`，并将 `SceneActivation` publication 送入 renderer full authoritative rebuild，避免把 active-scene mapping 当作普通增量 delta。
- 最新 Debug executable SHA-256：`c5aba5aa5fc87195940b816824a5ebfcfa329d46e3f157a3dac41e26d324edae`；当前有效 Release executable 为 `G:\Temp\mgif-p6-90-msvc-20260905T232000Z-Release\editor_ui_live_replay.exe`，SHA-256：`528c3e1af27ad32183c199d6f377ac92713f3851239844d8f39ecf06f3a0bd74`。
- 当前 U02-U08 source identity aggregate：`ae548d1738fe53704bd9d2e3749c50db0c427e38e5992d89487ea0e3f0625103`；对应证据目录为 `G:\Temp\mgif-slim-current-u02-20260909` 至 `G:\Temp\mgif-slim-current-u08-20260909`。
- 这 7 个最新目录的 manifests 全部为 `PASS_BOUNDED_LIVE_PROOF`，action counts 分别为 U02=8、U03=13、U04=9、U05=11、U06=6、U07=10、U08=6。
- U01 的最终 PNG 在本机窗口捕获层被白色/低信息表面审计拒绝，但应用已完成 boundary handshake，`app/result.json` 的 7 个动作全部通过；这属于截图传输问题，不能覆盖 controller 结果。
- scene-switch 修复后的 `editor_ui_replay_tests`、`editor_renderer_integration_tests`、`editor_controller_transaction_tests` 和 `scene_editor_mutation_tests` 均返回 0；Debug/Release MSVC 增量构建和依赖预检均通过。

P8-Fast、P9-Fast、P10-Fast 的条件已经满足，工程版本可以停止扩展验证并进入交付整理。完整 OS 点击、全 DPI 组合、长时间 soak、跨平台验证和 Gate 账本仍按本文档的 backlog 处理，不改变 P8/G8 历史状态。

固定交付记录见：[gltf-scene-editor-p8-p10-fast-completion.md](gltf-scene-editor-p8-p10-fast-completion.md)。

本轮追加的生产入口补丁已通过 `minimal_app_editor_authority_contract_tests.py`（6/6）。Windows 构建树随后被外部清理；下一次保留有效 CMake 树时只需重建 `Demo` 单目标确认入口编译。

最终 MSVC 收敛记录（2026-09-09）：

- 新建并使用真正的 MSVC 19.38 构建树：`G:\Temp\mgif-p8-p10-fast-20260909-Debug` 和 `G:\Temp\mgif-p8-p10-fast-20260909-Release`。
- Debug/Release 的 Demo、editor live replay 和关键 editor tests 均成功链接；RHI boundary guard 与 editor dependency preflight 通过。
- Vulkan-only Fast 构建关闭 `DEMO_ENABLE_D3D12`；CMake 的 Windows DXC final-color shader 条件已同步限定为 D3D12 开启时，避免 Vulkan-only 构建触发无关 DXC 失败。
- Release U02 live controller replay 的 8 个动作全部通过；PNG 仍受本机窗口捕获层白色占位影响，但 `app/result.json`、state 和 boundary handshake 完整。
- 最终 binaries：Debug `Demo.exe=75ea84ddd763489625d7cab1b05b43f012fb39646fc9a7df5a6499eae5f5b3c0`、`editor_ui_live_replay.exe=16cb4ffd350e37a3f91b4b76c789f4ba38d683df9922142e0e46193a7d2d7f38`；Release `Demo.exe=37616f705d5c0964e3ea87428750a53827b51f4fa489aa74f3e22a3019c630df`、`editor_ui_live_replay.exe=5a6a0f0b1dae5df48e808b832d87b8258adc2aaa850d7c312b75395e04943e2d`。
- Debug shader modules 通过 `spirv-val --target-env vulkan1.4`；MDI 路径使用独立的 `DrawUniformsStorage` 类型，修复 ConstantBuffer Block 嵌套进 StructuredBuffer runtime array 的 validation 错误。
- Debug/Release 强制重链后的 U02 controller replay 均完成 8/8 actions，stdout 已无该 VUID；剩余失败仅是本机窗口捕获层白色占位审计。
- Release `Demo --editor-scene=... --automation=... --auto-exit --no-ui` 已返回 0，日志包含 `scene-ready`、`start`、`settled`、`complete` 和正常 shutdown；生产入口 smoke 已闭合。
- 最终 Release 关键 editor 测试四项均返回 0：`editor_ui_replay_tests`、`editor_renderer_integration_tests`、`editor_controller_transaction_tests`、`scene_editor_mutation_tests`。
