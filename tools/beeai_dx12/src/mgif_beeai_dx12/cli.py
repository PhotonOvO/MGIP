from __future__ import annotations

import argparse
import asyncio
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import sqlite3
import sys
from typing import Any, Sequence

from beeai_framework.backend import UserMessage
from beeai_framework.errors import FrameworkError

from .actions import NamedActionRunner, default_action_specs
from .agent_models import Dx12Task
from .agent_workflow import AgentLoop
from .coding_agent import BeeAICodingAgent
from .gateway import GatewayConfig
from .security import SecurityViolation
from .state_store import StateStore
from .workflow import Dx12Orchestrator, Dx12WorkflowState, SOURCE_APPROVAL_SCOPE


APPROVAL_SCOPES = (SOURCE_APPROVAL_SCOPE, "build", "smoke")


def _default_state_root(workspace: Path) -> Path:
    return workspace / "out" / "beeai_dx12"


def _add_state_root(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--state-root", type=Path)


def _add_gateway_source(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--codex-config", type=Path, help="Optional Codex config.toml gateway source")


def _add_task(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--objective", required=True)
    parser.add_argument("--constraint", action="append", default=[])


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Restricted BeeAI orchestration for MGIF DX12 work")
    subcommands = parser.add_subparsers(dest="command", required=True)

    preflight = subcommands.add_parser("preflight", help="Run the read-only BeeAI foundation workflow")
    preflight.add_argument("--workspace", type=Path, required=True)
    preflight.add_argument("--beeai-root", type=Path, required=True)
    preflight.add_argument("--run-id")
    _add_state_root(preflight)

    approve = subcommands.add_parser("approve", help="Record an explicit approval scope")
    approve.add_argument("--workspace", type=Path, required=True)
    approve.add_argument("--run-id", required=True)
    approve.add_argument("--scope", default=SOURCE_APPROVAL_SCOPE, choices=APPROVAL_SCOPES)
    approve.add_argument("--actor", required=True)
    approve.add_argument("--note")
    _add_state_root(approve)

    action = subcommands.add_parser("action", help="Run one fixed, policy-controlled action")
    action.add_argument("--workspace", type=Path, required=True)
    action.add_argument("--run-id", required=True)
    action.add_argument("--name", required=True, choices=sorted(default_action_specs()))
    _add_state_root(action)

    status = subcommands.add_parser("status", help="Display durable state for one run")
    status.add_argument("--workspace", type=Path, required=True)
    status.add_argument("--run-id", required=True)
    _add_state_root(status)

    gateway = subcommands.add_parser("gateway-check", help="Validate and optionally call the custom gateway")
    gateway.add_argument("--no-live", action="store_true", help="Only validate local configuration")
    _add_gateway_source(gateway)

    agent_plan = subcommands.add_parser("agent-plan", help="Run BeeAI source analysis and store a patch proposal")
    agent_plan.add_argument("--workspace", type=Path, required=True)
    agent_plan.add_argument("--run-id")
    _add_state_root(agent_plan)
    _add_gateway_source(agent_plan)
    _add_task(agent_plan)

    agent_run = subcommands.add_parser(
        "agent-run", help="Apply an approved proposal and iterate on build feedback"
    )
    agent_run.add_argument("--workspace", type=Path, required=True)
    agent_run.add_argument("--run-id", required=True)
    agent_run.add_argument("--proposal-iteration", type=int, default=1)
    agent_run.add_argument("--max-corrections", type=int, default=3, choices=range(1, 6))
    _add_state_root(agent_run)
    _add_gateway_source(agent_run)
    _add_task(agent_run)

    return parser


async def _preflight(args: argparse.Namespace) -> dict[str, object]:
    workspace = args.workspace.resolve(strict=True)
    state_root = (args.state_root or _default_state_root(workspace)).resolve()
    store = StateStore(state_root / "state.sqlite3")
    if args.run_id:
        existing = store.get_run(args.run_id)
        if not existing:
            raise KeyError(f"unknown run: {args.run_id}")
        run_id = args.run_id
    else:
        run_id = store.create_run("mgif-dx12-foundation", str(workspace))
    state = Dx12WorkflowState(
        run_id=run_id,
        workspace=str(workspace),
        beeai_root=str(args.beeai_root.resolve(strict=True)),
        evidence_root=str(state_root / "evidence"),
    )
    result = await Dx12Orchestrator(store).run(state)
    return result.model_dump()


async def _gateway_check(args: argparse.Namespace) -> dict[str, object]:
    config = GatewayConfig.from_sources(codex_config=args.codex_config)
    diagnostic = config.diagnostic()
    diagnostic["protocol"] = "openai-chat-completions"
    diagnostic["live"] = not args.no_live
    if args.no_live:
        return diagnostic
    model = config.create_chat_model()
    response = await model.run(
        [UserMessage("Reply with exactly BEEAI_GATEWAY_OK. Do not call tools or add other text.")],
        max_retries=1,
    )
    text = response.get_text_content().strip()
    diagnostic["response_received"] = bool(text)
    diagnostic["sentinel_matched"] = text == "BEEAI_GATEWAY_OK"
    return diagnostic


def _record_gateway_evidence(
    state_root: Path, store: StateStore, run_id: str, config: GatewayConfig
) -> Path:
    path = state_root / "evidence" / run_id / "agent" / "gateway.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = config.diagnostic() | {"protocol": "openai-chat-completions"}
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)
    store.record_artifact(run_id, "agent:gateway", path, hashlib.sha256(path.read_bytes()).hexdigest())
    return path


def _agent_components(args: argparse.Namespace):
    workspace = args.workspace.resolve(strict=True)
    state_root = (args.state_root or _default_state_root(workspace)).resolve()
    store = StateStore(state_root / "state.sqlite3")
    config = GatewayConfig.from_sources(codex_config=args.codex_config)
    driver = BeeAICodingAgent(config, str(workspace))
    loop = AgentLoop(
        workspace,
        store,
        state_root / "evidence",
        driver,
        max_correction_iterations=getattr(args, "max_corrections", 3),
    )
    return workspace, state_root, store, config, loop


async def _agent_plan(args: argparse.Namespace) -> dict[str, object]:
    workspace, state_root, store, config, loop = _agent_components(args)
    run_id = args.run_id or store.create_run("mgif-dx12-agent", str(workspace))
    if args.run_id and not store.get_run(run_id):
        raise KeyError(f"unknown run: {run_id}")
    gateway_path = _record_gateway_evidence(state_root, store, run_id, config)
    task = Dx12Task(objective=args.objective, constraints=args.constraint)
    proposal = await loop.plan(run_id, task)
    return {
        "run_id": run_id,
        "status": "agent_waiting_for_approval",
        "gateway_evidence": str(gateway_path),
        "proposal": {
            "summary": proposal.summary,
            "paths": [change.path for change in proposal.changes],
            "verification_actions": proposal.verification_actions,
            "risks": proposal.risks,
        },
    }


async def _agent_run(args: argparse.Namespace) -> dict[str, object]:
    _, state_root, store, config, loop = _agent_components(args)
    if not store.get_run(args.run_id):
        raise KeyError(f"unknown run: {args.run_id}")
    _record_gateway_evidence(state_root, store, args.run_id, config)
    task = Dx12Task(objective=args.objective, constraints=args.constraint)
    proposal = loop.load_proposal(args.run_id, args.proposal_iteration)
    return await loop.execute(args.run_id, task, proposal)


def _list_artifacts(store: StateStore, run_id: str) -> list[dict[str, Any]]:
    connection = sqlite3.connect(store.path)
    connection.row_factory = sqlite3.Row
    try:
        rows = connection.execute(
            "SELECT * FROM artifacts WHERE run_id = ? ORDER BY created_at, kind, path", (run_id,)
        ).fetchall()
        return [dict(row) for row in rows]
    finally:
        connection.close()


def _execute(args: argparse.Namespace) -> dict[str, object]:
    if args.command == "gateway-check":
        return asyncio.run(_gateway_check(args))

    workspace = args.workspace.resolve(strict=True)
    state_root = (args.state_root or _default_state_root(workspace)).resolve()

    if args.command == "preflight":
        return asyncio.run(_preflight(args))
    if args.command == "agent-plan":
        return asyncio.run(_agent_plan(args))
    if args.command == "agent-run":
        return asyncio.run(_agent_run(args))

    store = StateStore(state_root / "state.sqlite3")
    if args.command == "approve":
        store.approve(args.run_id, args.scope, args.actor, args.note)
        return {"run_id": args.run_id, "scope": args.scope, "approved": True}
    if args.command == "action":
        result = NamedActionRunner(workspace, store, state_root / "evidence").run(args.run_id, args.name)
        return asdict(result)

    run = store.get_run(args.run_id)
    if not run:
        raise KeyError(f"unknown run: {args.run_id}")
    return {
        "run": run,
        "steps": store.list_steps(args.run_id),
        "artifacts": _list_artifacts(store, args.run_id),
    }


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        payload = _execute(args)
    except FrameworkError as exc:
        print(
            json.dumps({"error": type(exc).__name__, "message": "BeeAI model invocation failed"}),
            file=sys.stderr,
        )
        return 3
    except (KeyError, RuntimeError, SecurityViolation, ValueError) as exc:
        print(json.dumps({"error": type(exc).__name__, "message": str(exc)}, sort_keys=True), file=sys.stderr)
        return 2
    print(json.dumps(payload, indent=2, sort_keys=True))
    if args.command == "action":
        return int(payload["exit_code"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
