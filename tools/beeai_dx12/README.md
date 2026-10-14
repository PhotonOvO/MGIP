# MGIF BeeAI DX12 Orchestrator

This package is the safety and persistence layer between BeeAI Framework and
the MGIF repository. It intentionally does not expose BeeAI's unrestricted
local file or shell backends.

## Bootstrap

```powershell
.\tools\beeai_dx12\bootstrap.ps1
```

The script verifies the local BeeAI checkout and commit, creates a Python 3.13
environment under `out/beeai_dx12/venv`, and installs both packages editable.

## Preflight

```powershell
out\beeai_dx12\venv\Scripts\mgif-beeai-dx12.exe preflight `
  --workspace G:\MGIF `
  --beeai-root F:\GitHub\beeai-framework
```

Preflight is read-only. It records the git baseline and environment evidence,
then stops at `waiting_for_approval`. Source writes require an explicit
`dx12-source` approval for that run ID.

## Custom OpenAI-compatible gateway

The Agent commands can load an OpenAI Chat Completions gateway from the Codex
configuration without copying credentials into this repository or the evidence
directory. Only the gateway host, model ID, configuration source, and whether
authentication is present are recorded.

```powershell
out\beeai_dx12\venv\Scripts\mgif-beeai-dx12.exe gateway-check `
  --codex-config C:\Users\happyelements\.codex\config.toml
```

## BeeAI Agent loop

Create a proposal first. This step may send the bounded DX12/RHI source context
and proposal content to the configured gateway, but it cannot write source or
run a build.

```powershell
out\beeai_dx12\venv\Scripts\mgif-beeai-dx12.exe agent-plan `
  --workspace G:\MGIF `
  --codex-config C:\Users\happyelements\.codex\config.toml `
  --objective "Implement the next buildable DX12 resource slice" `
  --constraint "Preserve Vulkan behavior and remain inside the DX12 allowlist"
```

Review the returned proposal and record both required approvals for its run ID:

```powershell
out\beeai_dx12\venv\Scripts\mgif-beeai-dx12.exe approve `
  --workspace G:\MGIF --run-id <run-id> --scope dx12-source --actor user
out\beeai_dx12\venv\Scripts\mgif-beeai-dx12.exe approve `
  --workspace G:\MGIF --run-id <run-id> --scope build --actor user
```

Apply the approved proposal, run its fixed verification actions, and let BeeAI
review redacted, tail-truncated feedback. `--max-corrections 3` caps the full
apply, verify, and revise loop at three iterations.

```powershell
out\beeai_dx12\venv\Scripts\mgif-beeai-dx12.exe agent-run `
  --workspace G:\MGIF `
  --run-id <run-id> `
  --proposal-iteration 1 `
  --max-corrections 3 `
  --codex-config C:\Users\happyelements\.codex\config.toml `
  --objective "Implement the next buildable DX12 resource slice" `
  --constraint "Preserve Vulkan behavior and remain inside the DX12 allowlist"
```

## Security model

- File access is confined to one resolved workspace root.
- Reparse points, symlinks, `.git`, build output, and secret-like files are
  rejected by the source file backend.
- The generic BeeAI shell backend only accepts a small read-only command set.
- Configure, build, test, and smoke operations use named command actions.
- Gateway feedback is redacted before persistence and limited to bounded output
  tails before it is returned to BeeAI for review.
- Workflow and approval state is stored in SQLite; evidence is emitted as JSON.
- Approval never implies commit, push, branch deletion, or worktree deletion.
