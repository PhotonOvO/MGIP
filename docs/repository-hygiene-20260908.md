# Repository Hygiene Pass — 2026-09-08

## Measurement Before The Pass

- `git diff --numstat`: 414 modified tracked files, `+162023 / -141681`
- `git diff --ignore-cr-at-eol --numstat`: 72 files, `+23436 / -3094`
- Therefore **342 of the 414 modified files differed from `HEAD` by line endings
  alone**.
- `core.autocrlf` and `core.eol` are both unset, so git performed no conversion;
  the churn came from Windows-side writers (PowerShell `Set-Content`/`Out-File`,
  patch utilities) rewriting whole files with CRLF. Two extreme examples were
  pure flips with zero content change: `tools/rdc_csm_motion_compare.py`
  (15187/15187 lines) and `tools/run_csm_shadow_motion_smoke.py` (10375/10375
  lines).
- Every `HEAD` blob for the 72 content-modified files is LF-only (zero CR), so
  LF is the committed convention.
- Untracked: 752 entries, 28 MB, including roughly 15 MB of stray build output
  at the repository root.
- `git status` emitted `warning: could not open directory 'mgif build entrypoints
  kftx8g0b/': Permission denied` on every invocation.

## Actions Taken

1. **Safety snapshot first.** The complete pre-change diff and the revert list
   were written to
   `G:\Temp\mgif-hygiene-quarantine-20260908\pre-revert-full-414-files.diff`
   (12286493 bytes) and `eol-only-files.txt` (342 paths).
2. **Reverted the 342 line-ending-only files** with
   `git checkout --pathspec-from-file=<nul list> --pathspec-file-nul`.
   Spot-checked files were proven content-identical beforehand with
   `diff --strip-trailing-cr` against `git show HEAD:<path>`: `.gitmodules`,
   `.github/workflows/rhi-contract.yml`, `tools/rdc_csm_motion_compare.py`,
   `tools/run_csm_shadow_motion_smoke.py`, `.planning/STATE.md`.
3. **Normalized CRLF to LF in the remaining 72 content-modified files.** Each
   file was verified byte-wise during the rewrite
   (`original.replace(b"\r\n", b"\n") == normalized`), so no content changed.
   `resources` is a submodule and was skipped.
4. **Quarantined 15 MB of root junk** to
   `G:\Temp\mgif-hygiene-quarantine-20260908\` (moved, not deleted): 12 stray
   `.obj` compiler outputs dated 2026-08-23 to 2026-09-05, `vc140.pdb`, and
   `_tmp_frame_compare`, `_tmp_frame_compare2`, `_tmp_frame_compare3`
   (5.3 MB `api.out`). No repository document references those temp
   directories; the only grep hits were their own `diagnostic.json` files.
5. **Added `.gitattributes`.** `* text=auto eol=lf` pins the committed LF
   convention at add/checkout time. The hash-bound ledger subtree is excluded
   from conversion (`docs/editor-parity/** -text`, plus the three parity
   progress/task documents), because validators recompute SHA-256 over those
   exact bytes and two of them carry CRLF today: `docs/editor-parity/manifest.json`
   is bound as `a5182ddaeb88368b293ee55931e6dd7deec989eb62cda57d5c9c09c65b38b053`
   by the frozen legacy state, and
   `docs/editor-parity/schemas/progress-ledger.schema.json` also contains CRLF.
   Converting either would invalidate a frozen binding on a fresh checkout.
   Binary payload extensions are marked `binary`; `*.gltf` stays text because it
   is JSON.
6. **Extended `.gitignore`** with `*.obj`, `*.pdb`, `_tmp_frame_compare*/`, and
   the stray directory path.

## Verification After The Pass

- `git diff --numstat`: 72 files, `+23443 / -3101` (from `+162023 / -141681`).
- Residual line-ending noise is zero except seven files that add a POSIX trailing
  newline absent in `HEAD`: `render/FrameScheduler.h`, `render/FrameScheduler.cpp`,
  `rhi/vulkan/VulkanCommandAllocator.h`, `rhi/vulkan/VulkanCommandAllocator.cpp`,
  `rhi/vulkan/VulkanCommandBuffer.h`, `rhi/vulkan/VulkanQueue.cpp`,
  `tests/rhi_public_contract_tests.cpp`. That is a real, benign one-line
  difference per file, not EOL churn.
- `git status --porcelain`: 127 entries and no permission warning.
- Untracked work is now 679 files / 13 MB, all legitimate: `docs/editor-parity`
  299, `tests/editor` 81, `tests/fixtures` 66, `scene/editor` 65, `app/editor`
  35, `tools/beeai_dx12` 26, `render/editor` 23, `tests/pi-renderdoc` 10,
  `src/rdc` 5, `render/passes` 4, and a few single files.
- Attribute probe: `git check-attr text eol` reports `text: unset` for
  `docs/editor-parity/**` and the parity documents, and `text: auto`, `eol: lf`
  elsewhere. A throwaway CRLF file added under a normal path stored LF in the
  index while the worktree kept CRLF; the probe was unstaged and deleted.
- Rebuild after normalization: Debug 173 ninja steps, exit 0, including
  `Demo.exe`; Release 169 ninja steps, exit 0
  (`G:\Temp\build-after-eol-normalization-debug.log`,
  `G:\Temp\build-after-eol-normalization-release.log`). The configured
  production-editor dependency preflight passed in both.
- Live replay matrix rerun in both configurations on the normalized tree: 7/8
  `PASS_BOUNDED_LIVE_PROOF`, `U01-empty-new-open` failing identically at frame 0,
  new shared scoped source identity
  `3f1c0752f4a63ef9cb7478ad67ec44a61053584f703a1bec6d3dcfad62fcce19`. See
  `docs/editor-parity/u01-u08-live-replay-matrix.md`.
- The rebuild also unmasked one pre-existing defect: Debug
  `editor_selection_picking_integration_tests.exe` now aborts with exit code 3
  because `/RTC1` detected a stack guard overrun around a local
  `ui::EditorController`. The previously recorded `rc=0` came from stale Debug
  objects, and Release passes, so this is not a hygiene regression. Evidence and
  next step: `docs/editor-selection-picking-debug-rtc-stack-corruption.md`.
- Ledger untouched: `python3 tools/validate_editor_local_progress.py` still
  reports `head=1d54cec00c96d704229448564c3b762e869159eb333e8bb2f4e68051b294f00b
  ready=0` with exit 0, and the publisher `--check` still reports
  `bundle=404366e527af67ea2d0bb57ba751be2aa0d2c7812d233ae869c1833a5d5120b7`.
  The 258 files under `docs/editor-parity/progress-ledger` contain no CR byte and
  every content-addressed name still recomputes to its own SHA-256.

## Left In Place, With Reasons

- **Stray directory `mgif build entrypoints kftx8g0b`** (created 2026-08-06
  20:24). Its ACL denies listing even to the owning user; `icacls /grant` and
  `takeown /f` both returned access denied, so it cannot be inspected or removed
  from a non-elevated shell. It is now gitignored, which stops the traversal
  warning. Removal requires an elevated shell:

  ```text
  takeown /f "G:\MGIF\mgif build entrypoints kftx8g0b" /r /d y
  icacls  "G:\MGIF\mgif build entrypoints kftx8g0b" /reset /t /c
  rmdir /s /q "G:\MGIF\mgif build entrypoints kftx8g0b"
  ```

- **`tools/run_csm_shadow_motion_smoke.py.orig`** (3423 lines versus 10375 in the
  live file) and **`tools/test_csm_final_path_contract.py.orig`** (798 versus
  1159, with renamed test methods) are older superseded revisions, not redundant
  copies, so they were kept. The existing `*.orig` ignore rule already keeps them
  out of git.
- **No `.orig`, quarantine, or evidence file was deleted.** Everything moved is
  recoverable from `G:\Temp\mgif-hygiene-quarantine-20260908\`.

## Not Done, And Why

No commit was created. The pending content diff is now reviewable
(`+23436 / -3094` across 72 files), and a single snapshot commit would mix three
unrelated bodies of work: the RHI/render follow-ups, the editor parity
implementation, and the hash-bound ledger evidence store. A scoped sequence is
recommended instead, with the owner choosing messages and boundaries:

```text
git add .gitattributes .gitignore
git commit -m "chore(repo): pin LF normalization and ignore stray build artifacts"

git add app/editor scene/editor render/editor tests/editor tests/fixtures \
        tools/editor_ui_live_replay.ps1 tools/editor_ui_os_replay.ps1
git commit -m "feat(editor): scene editor parity implementation and tests"

git add docs/editor-parity docs/gltf-scene-editor-parity-task.md \
        docs/gltf-scene-editor-parity-progress.md \
        docs/gltf-scene-editor-parity-local-progress.md
git commit -m "docs(editor-parity): ledger, evidence store, development records"

git add -A
git commit -m "feat(rhi/render): remaining tracked changes"
```

`.gitattributes` already guarantees that the ledger subtree is committed
byte-exact, which the frozen hash bindings require.

Risk note: 13 MB of untracked work plus 72 modified files is one
`git clean -xdf` away from being unrecoverable, so committing soon is advisable.
