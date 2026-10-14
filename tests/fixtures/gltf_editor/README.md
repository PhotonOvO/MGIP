# glTF Editor Test Corpus

These fixtures are deliberately small, hand-authored inputs for deterministic editor tests. The only external binary payload is the original nine-byte `external_buffer.bin` sentinel; the corpus contains no third-party asset bytes.

| File | Purpose | Expected load result |
|---|---|---|
| `minimal_scene.gltf` | Smallest editable one-root scene | valid |
| `multi_scene_oracle.gltf` | D20 current/non-current scene matrix, per-scene children, and a root shared by both scenes | valid |
| `unknown_extension_roundtrip.gltf` | Unknown document/scene/node extensions and extras preservation | valid |
| `invalid_child_index.gltf` | Out-of-range hierarchy reference and atomic load rejection | invalid |
| `invalid_cycle.gltf` | Bounded iterative cycle detection | invalid |
| `invalid_multiparent.gltf` | Duplicate node-parent rejection | invalid |
| `external_buffer.gltf` + `external_buffer.bin` | Contained external dependency loading and accounting | valid |
| `external_image.gltf` + `external_image.payload` | Original external image sentinel, dependency accounting, and staged Save As | valid |
| `feature_preservation.gltf` | Data URI and bufferView resources, sparse/skin/animation/morph/material/camera/light/unknown-value preservation | valid |
| `unicode_names.gltf` | CJK, emoji, and combining-sequence name preservation | valid |

Every D20 action must start from a newly loaded copy of `multi_scene_oracle.gltf`. An earlier mutation must never become the initial state of a later oracle trace. `corpus.json` binds every committed fixture to its SHA-256 digest and provenance.
