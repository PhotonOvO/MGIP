from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime, timezone
import json
from pathlib import Path
import sqlite3
from typing import Any, Iterator
import uuid


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


class StateStore:
    """Durable workflow, step, approval, and artifact state."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path).resolve()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._initialize()

    @contextmanager
    def _connection(self) -> Iterator[sqlite3.Connection]:
        connection = sqlite3.connect(self.path)
        connection.row_factory = sqlite3.Row
        try:
            with connection:
                yield connection
        finally:
            connection.close()

    def _initialize(self) -> None:
        with self._connection() as connection:
            connection.executescript(
                """
                PRAGMA journal_mode=WAL;
                CREATE TABLE IF NOT EXISTS runs (
                    id TEXT PRIMARY KEY,
                    workflow TEXT NOT NULL,
                    workspace TEXT NOT NULL,
                    status TEXT NOT NULL,
                    baseline_commit TEXT,
                    beeai_commit TEXT,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS steps (
                    run_id TEXT NOT NULL,
                    name TEXT NOT NULL,
                    attempt INTEGER NOT NULL,
                    status TEXT NOT NULL,
                    input_json TEXT,
                    output_json TEXT,
                    error TEXT,
                    started_at TEXT NOT NULL,
                    finished_at TEXT,
                    PRIMARY KEY (run_id, name, attempt),
                    FOREIGN KEY (run_id) REFERENCES runs(id)
                );
                CREATE TABLE IF NOT EXISTS approvals (
                    run_id TEXT NOT NULL,
                    scope TEXT NOT NULL,
                    actor TEXT NOT NULL,
                    note TEXT,
                    approved_at TEXT NOT NULL,
                    PRIMARY KEY (run_id, scope),
                    FOREIGN KEY (run_id) REFERENCES runs(id)
                );
                CREATE TABLE IF NOT EXISTS artifacts (
                    run_id TEXT NOT NULL,
                    kind TEXT NOT NULL,
                    path TEXT NOT NULL,
                    sha256 TEXT,
                    created_at TEXT NOT NULL,
                    PRIMARY KEY (run_id, kind, path),
                    FOREIGN KEY (run_id) REFERENCES runs(id)
                );
                """
            )

    def create_run(self, workflow: str, workspace: str, run_id: str | None = None) -> str:
        identifier = run_id or uuid.uuid4().hex
        timestamp = _now()
        with self._connection() as connection:
            connection.execute(
                "INSERT INTO runs(id, workflow, workspace, status, created_at, updated_at) VALUES(?, ?, ?, ?, ?, ?)",
                (identifier, workflow, workspace, "created", timestamp, timestamp),
            )
        return identifier

    def get_run(self, run_id: str) -> dict[str, Any] | None:
        with self._connection() as connection:
            row = connection.execute("SELECT * FROM runs WHERE id = ?", (run_id,)).fetchone()
        return dict(row) if row else None

    def update_run(self, run_id: str, status: str, **fields: str | None) -> None:
        allowed = {"baseline_commit", "beeai_commit"}
        unknown = set(fields) - allowed
        if unknown:
            raise ValueError(f"unsupported run fields: {sorted(unknown)}")
        assignments = ["status = ?", "updated_at = ?"]
        values: list[str | None] = [status, _now()]
        for name, value in fields.items():
            assignments.append(f"{name} = ?")
            values.append(value)
        values.append(run_id)
        with self._connection() as connection:
            cursor = connection.execute(f"UPDATE runs SET {', '.join(assignments)} WHERE id = ?", values)
            if cursor.rowcount != 1:
                raise KeyError(f"unknown run: {run_id}")

    def start_step(self, run_id: str, name: str, input_data: Any = None) -> int:
        with self._connection() as connection:
            row = connection.execute(
                "SELECT COALESCE(MAX(attempt), 0) AS attempt FROM steps WHERE run_id = ? AND name = ?",
                (run_id, name),
            ).fetchone()
            attempt = int(row["attempt"]) + 1
            connection.execute(
                "INSERT INTO steps(run_id, name, attempt, status, input_json, started_at) VALUES(?, ?, ?, ?, ?, ?)",
                (run_id, name, attempt, "running", json.dumps(input_data, sort_keys=True), _now()),
            )
        return attempt

    def finish_step(self, run_id: str, name: str, attempt: int, output: Any = None) -> None:
        self._finish_step(run_id, name, attempt, "completed", output, None)

    def fail_step(self, run_id: str, name: str, attempt: int, error: str) -> None:
        self._finish_step(run_id, name, attempt, "failed", None, error)

    def _finish_step(
        self, run_id: str, name: str, attempt: int, status: str, output: Any, error: str | None
    ) -> None:
        with self._connection() as connection:
            cursor = connection.execute(
                "UPDATE steps SET status = ?, output_json = ?, error = ?, finished_at = ? "
                "WHERE run_id = ? AND name = ? AND attempt = ?",
                (status, json.dumps(output, sort_keys=True), error, _now(), run_id, name, attempt),
            )
            if cursor.rowcount != 1:
                raise KeyError(f"unknown step attempt: {run_id}/{name}/{attempt}")

    def approve(self, run_id: str, scope: str, actor: str, note: str | None = None) -> None:
        if not self.get_run(run_id):
            raise KeyError(f"unknown run: {run_id}")
        with self._connection() as connection:
            connection.execute(
                "INSERT OR REPLACE INTO approvals(run_id, scope, actor, note, approved_at) VALUES(?, ?, ?, ?, ?)",
                (run_id, scope, actor, note, _now()),
            )

    def has_approval(self, run_id: str, scope: str) -> bool:
        with self._connection() as connection:
            row = connection.execute(
                "SELECT 1 FROM approvals WHERE run_id = ? AND scope = ?", (run_id, scope)
            ).fetchone()
        return row is not None

    def record_artifact(self, run_id: str, kind: str, path: str | Path, sha256: str | None = None) -> None:
        if not self.get_run(run_id):
            raise KeyError(f"unknown run: {run_id}")
        with self._connection() as connection:
            connection.execute(
                "INSERT OR REPLACE INTO artifacts(run_id, kind, path, sha256, created_at) VALUES(?, ?, ?, ?, ?)",
                (run_id, kind, str(Path(path)), sha256, _now()),
            )

    def list_steps(self, run_id: str) -> list[dict[str, Any]]:
        with self._connection() as connection:
            rows = connection.execute(
                "SELECT * FROM steps WHERE run_id = ? ORDER BY started_at, name, attempt", (run_id,)
            ).fetchall()
        return [dict(row) for row in rows]
