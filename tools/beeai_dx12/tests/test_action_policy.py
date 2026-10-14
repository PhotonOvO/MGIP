from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest


PACKAGE_SRC = Path(__file__).resolve().parents[1] / "src"
sys.path.insert(0, str(PACKAGE_SRC))

from mgif_beeai_dx12.actions import NamedActionRunner  # noqa: E402
from mgif_beeai_dx12.security import SecurityViolation  # noqa: E402
from mgif_beeai_dx12.state_store import StateStore  # noqa: E402


class ActionEvidencePolicyTests(unittest.TestCase):
    def test_evidence_root_must_remain_in_workspace(self) -> None:
        with tempfile.TemporaryDirectory(prefix="beeai-evidence-") as directory:
            root = Path(directory).resolve()
            store = StateStore(root / "state.sqlite3")
            outside = root.parent / "outside-evidence"
            with self.assertRaises(SecurityViolation):
                NamedActionRunner(root, store, outside)


if __name__ == "__main__":
    unittest.main()
