from __future__ import annotations

from pathlib import Path
import unittest
from unittest.mock import patch

from mgif_beeai_dx12.cli import main


class CliExitCodeTests(unittest.TestCase):
    def _argv(self) -> list[str]:
        return [
            "action",
            "--workspace",
            str(Path.cwd()),
            "--run-id",
            "test-run",
            "--name",
            "git-status",
        ]

    def test_action_failure_is_propagated(self) -> None:
        with patch("mgif_beeai_dx12.cli._execute", return_value={"exit_code": 7}):
            self.assertEqual(main(self._argv()), 7)

    def test_action_success_returns_zero(self) -> None:
        with patch("mgif_beeai_dx12.cli._execute", return_value={"exit_code": 0}):
            self.assertEqual(main(self._argv()), 0)


if __name__ == "__main__":
    unittest.main()
