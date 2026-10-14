from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest


PACKAGE_SRC = Path(__file__).resolve().parents[1] / "src"
sys.path.insert(0, str(PACKAGE_SRC))

from mgif_beeai_dx12.state_store import StateStore  # noqa: E402


class StateStoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory(prefix="beeai-state-")
        self.addCleanup(self.temp_dir.cleanup)
        self.database = Path(self.temp_dir.name) / "state.sqlite3"
        self.store = StateStore(self.database)

    def test_run_steps_and_approval_survive_reopen(self) -> None:
        run_id = self.store.create_run("dx12", "G:\\MGIF")
        attempt = self.store.start_step(run_id, "preflight", {"input": True})
        self.store.finish_step(run_id, "preflight", attempt, {"passed": True})
        self.store.approve(run_id, "dx12-source", "user")
        self.store.update_run(run_id, "ready", baseline_commit="abc")

        reopened = StateStore(self.database)
        self.assertEqual(reopened.get_run(run_id)["status"], "ready")
        self.assertTrue(reopened.has_approval(run_id, "dx12-source"))
        self.assertEqual(reopened.list_steps(run_id)[0]["status"], "completed")

    def test_step_attempts_are_monotonic(self) -> None:
        run_id = self.store.create_run("dx12", "G:\\MGIF")
        first = self.store.start_step(run_id, "build")
        self.store.fail_step(run_id, "build", first, "failed")
        second = self.store.start_step(run_id, "build")
        self.assertEqual((first, second), (1, 2))


if __name__ == "__main__":
    unittest.main()
