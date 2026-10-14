# 像素审计缺陷与 live 闸门重标定（2026-09-09）

## 1. 结论摘要

1. `uniqueColors` 度量长期错误：Windows PowerShell 5.1 中 `[byte] -shl 16`
   **保持 Byte 类型**，移位后截断为 0，`-bor` 结果实际只剩 **B 通道**。
   实测：`[byte]200 -shl 16 => value=0 type=Byte`；`[int]200 -shl 16 => 13107200 type=Int32`。
2. 因此 `tools/editor_ui_live_replay.ps1` 与 `tools/editor_ui_os_replay.ps1`
   记录的每个 `uniqueColors`（含分区 `zoneColors`）**都是"不同蓝色值个数"，不是颜色数**。
3. live 亮度闸门（`nonBlackRatio>=0.05`、`colorfulRatio>=0.02`、`edgeRatio>=0.03`）
   是按 20260908 那一帧"面板叠在受光场景上"的亮帧标定的，
   **会拒绝当前二进制的真实暗帧**，却放行两色幻影占位与外来亮窗。
4. 修正后，live 实时捕获立即恢复 `PASS_BOUNDED_LIVE_PROOF`。
   此前"live replay 视口/场景渲染为黑"的**应用侧缺陷结论被撤回**：应用无缺陷。

## 2. 度量修正前后对照（同一批文件，[int] 形式与独立 PIL 计数完全一致）

| 帧 | byte 形式（旧） | [int] 形式（新）= PIL |
|---|---|---|
| `mgif-u08-live-debug-20260908-norm/client.png` | 141 | **562** |
| R2 `U06-...-1280x720-dpi1/client.png`（两色占位） | 86 | **114** |
| `parkrender/client.png.rejected.png`（真实暗 live） | 46 | **51** |
| `vis-smoke-1/V01-grid--perspective-unit-0p001` | 171 | **444** |
| `vis-probe-1440-undec/V01-grid--perspective-horizon` | 41 | **49** |

## 3. 闸门重标定依据（采样步长 4，比率制）

| 样本类别 | uniq | nonBlackRatio | colorfulRatio | edgeRatio | coherentRunRatio | structuredRowRatio |
|---|---|---|---|---|---|---|
| 真实暗 live 边界帧 | 51 | 0.0329 | 0.0088 | 0.0293 | 0.2833 | 0.2667 |
| 真实亮 live 参考 20260908 | 562 | 1.000 | 0.1717 | 0.0715 | 1.000 | 1.000 |
| 真实暗视觉帧（视觉 harness 已接受） | 48 | 0.0235 | 0.0030 | 0.0246 | 0.2852 | 0.3296 |
| 真实暗视觉帧 1440p undecorated | 49 | 0.0140 | 0.0019 | 0.0120 | 0.2417 | 0.2417 |
| 真实亮视觉帧 | 444 | 0.2247 | 0.0037 | 0.2273 | 9.9556 | 1.000 |
| 白 blank（本次捕获期，6/6 次） | 2 | 1.000 | 0 | 0 | 1.000 | 1.000 |
| 两色幻影占位（垃圾） | 114 | 1.000 | 0.6349 | 0.1095 | 1.000 | 1.000 |
| 合成黑 blank | 1 | 0 | 0 | 0 | 0 | 0 |
| 退化样本（历史记录） | — | — | — | — | 0.1444 | 0.1778 |

采用的 fail-closed 闸门（结构决定，亮度只用于剔除均质面）：

```
uniqueColors        >= 32      # 真实最小 48
coherentRunRatio    >= 0.19    # 真实最小 0.2417；退化 0.1444
structuredRowRatio  >= 0.21    # 真实最小 0.2417；退化 0.1778
nonBlackRatio       >= 0.01    # 真实最小 0.0140；blank 0
colorfulRatio       >= 0.001   # 真实最小 0.0019；blank 0
edgeRatio           >= 0.008   # 真实最小 0.0120；blank 0
uniformSurface      == false   # nonBlackRatio>=0.90 且 uniqueColors<200
```

`uniformSurface` 专治两色占位（uniq 114）与白 blank（uniq 1-2），
而真实亮帧（444-562）不受影响。`luminanceMean/StdDev` 仍只记录不设闸，
以免将来浅色主题被主题相关数字拒绝。

## 4. 离线一致性证明（直接抽取 harness 内真实 `Get-PixelAudit` 执行）

9/9 判定正确：5 个真实样本全部 ACCEPT，4 个垃圾/blank 全部 REJECT。
证明脚本：`G:\Temp\gate_conformance.ps1`（诊断用，不入库）。

## 5. 对历史证据的影响

- 历史 live/OS-replay 根中 `audit.uniqueColors`、`zones[].uniqueColors`、
  `viewport.uniqueColors` 的**数值语义作废**（应读作蓝色通道不同值个数）。
- `client.png` 字节与 `sha256`、`sampled/nonBlack/colorful/edgeTransitions`
  及各比率字段**不受影响**，仍然有效。
- 任何仅依据 `uniqueColors` 数值得出的历史结论需按本节重读；
  本文档不追溯改写既有证据文件（append-only 证据原则）。

## 6. 代码变更

- `tools/editor_ui_live_replay.ps1`
  - `Get-PixelAudit`：颜色打包改为 `[int]` 转换后移位；
  - 新增 `coherentRunRatio`、`structuredRowRatio`、`uniformSurface` 计算与记录；
  - 闸门按第 3 节重标定。
- `tools/editor_ui_os_replay.ps1`
  - `$packed` 改为 `[int]` 形式，与同函数内既有 `$nativePacked` 一致
    （此前两种打包并存、按构造互相矛盾）。
  - 该文件所有 blank 闸门均为**下界**形式，计数变大只会更严格于信息量、
    不会改变既有通过性；`:3259-3263` 的同轮持久化一致性比较两侧同时修正。
- `tools/verify_editor_live_matrix.py`
  - `AUDIT_FLOORS` 同步为新阈值并纳入结构指标；
  - `UNIQUE_COLOR_FLOOR = 32`；
  - 新增均质面剔除（`UNIFORM_SURFACE_NON_BLACK_RATIO=0.90`、
    `UNIFORM_SURFACE_MIN_COLORS=200`）。

## 7. 验证

- `ctest -C Debug -R os_replay`：**4/4 通过**（含锁定 ps1 内容的
  `scene_editor.ui_os_replay_contract`，33.43 s）。
- 无测试锁定 `tools/editor_ui_live_replay.ps1`（`grep -rl` 于 `tests/` 为空）。
- 首个重标定后 live PASS 根：`/mnt/g/Temp/gatefix-u02-1`
  （`PASS_BOUNDED_LIVE_PROOF`，uniq=54，coh=0.2889，srow=0.2722，desktop-copy）。
- R3 矩阵首批根（Debug U01 1280x720 dpi 1/1.25/1.5/2）：
  全部 `PASS_BOUNDED_LIVE_PROOF`、精确 1280x720、`pinnedToPrimary=true`、
  `topmostVerified=true`、`uiDpi.requested==applied`、sha256 互异、
  DPI 像素响应 0.0440-0.0703（校验器下限 0.01）。
