from __future__ import annotations

from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest


PACKAGE_SRC = Path(__file__).resolve().parents[1] / "src"
sys.path.insert(0, str(PACKAGE_SRC))

from mgif_beeai_dx12.actions import ActionSpec, NamedActionRunner  # noqa: E402
from mgif_beeai_dx12.security import SecurityViolation  # noqa: E402
from mgif_beeai_dx12.state_store import StateStore  # noqa: E402


class NamedActionRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory(prefix="beeai-action-")
        self.addCleanup(self.temp_dir.cleanup)
        self.root = Path(self.temp_dir.name).resolve()
        self.store = StateStore(self.root / "state.sqlite3")
        self.run_id = self.store.create_run("dx12", str(self.root))
        specs = {
            "version": ActionSpec("version", None, lambda root: [sys.executable, "--version"], 10.0),
            "approved": ActionSpec("approved", "build", lambda root: [sys.executable, "--version"], 10.0),
        }
        self.runner = NamedActionRunner(self.root, self.store, self.root / "evidence", specs)

    def test_read_only_action_records_hashed_evidence(self) -> None:
        result = self.runner.run(self.run_id, "version")
        self.assertEqual(result.exit_code, 0)
        connection = sqlite3.connect(self.store.path)
        try:
            artifact = connection.execute(
                "SELECT kind, path, sha256 FROM artifacts WHERE run_id = ?", (self.run_id,)
            ).fetchone()
        finally:
            connection.close()
        self.assertEqual(artifact[0], "action:version")
        self.assertTrue(Path(artifact[1]).is_file())
        self.assertEqual(len(artifact[2]), 64)

    def test_approval_scope_is_mandatory(self) -> None:
        with self.assertRaises(SecurityViolation):
            self.runner.run(self.run_id, "approved")
        self.store.approve(self.run_id, "build", "user")
        self.assertEqual(self.runner.run(self.run_id, "approved").exit_code, 0)

    def test_unknown_action_is_rejected(self) -> None:
        with self.assertRaises(SecurityViolation):
            self.runner.run(self.run_id, "arbitrary-command")


if __name__ == "__main__":
    unittest.main()
