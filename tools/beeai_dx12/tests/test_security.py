from __future__ import annotations

import asyncio
from pathlib import Path
import sys
import tempfile
import unittest


PACKAGE_SRC = Path(__file__).resolve().parents[1] / "src"
sys.path.insert(0, str(PACKAGE_SRC))

from mgif_beeai_dx12.security import (  # noqa: E402
    RestrictedShellBackend,
    SecurityViolation,
    WorkspaceFileBackend,
)


class WorkspaceFileBackendTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory(prefix="beeai-security-")
        self.addCleanup(self.temp_dir.cleanup)
        self.root = Path(self.temp_dir.name).resolve()
        (self.root / "rhi").mkdir()
        (self.root / "rhi" / "sample.cpp").write_text("old\n", encoding="utf-8")

    def test_read_and_approved_atomic_write_stay_in_workspace(self) -> None:
        backend = WorkspaceFileBackend(self.root, writable=True)
        path = self.root / "rhi" / "sample.cpp"
        self.assertEqual(asyncio.run(backend.read_text(str(path))), "old\n")
        asyncio.run(backend.write_text(str(path), "new\n"))
        self.assertEqual(path.read_text(encoding="utf-8"), "new\n")

    def test_default_backend_is_read_only(self) -> None:
        backend = WorkspaceFileBackend(self.root)
        with self.assertRaises(SecurityViolation):
            asyncio.run(backend.write_text(str(self.root / "rhi" / "new.cpp"), "content"))

    def test_outside_and_protected_paths_are_rejected(self) -> None:
        backend = WorkspaceFileBackend(self.root, writable=True)
        with self.assertRaises(SecurityViolation):
            asyncio.run(backend.read_text(str(self.root.parent / "outside.txt")))
        with self.assertRaises(SecurityViolation):
            asyncio.run(backend.write_text(str(self.root / ".git" / "config"), "bad"))
        with self.assertRaises(SecurityViolation):
            asyncio.run(backend.write_text(str(self.root / "out" / "payload.cpp"), "bad"))

    def test_reparse_point_is_rejected_when_supported(self) -> None:
        outside = self.root.parent / f"{self.root.name}-outside"
        outside.mkdir()
        self.addCleanup(lambda: outside.rmdir() if outside.exists() else None)
        link = self.root / "linked"
        try:
            link.symlink_to(outside, target_is_directory=True)
        except OSError:
            self.skipTest("symlink creation is not available")
        backend = WorkspaceFileBackend(self.root)
        with self.assertRaises(SecurityViolation):
            asyncio.run(backend.read_text(str(link / "secret.txt")))


class RestrictedShellBackendTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory(prefix="beeai-shell-")
        self.addCleanup(self.temp_dir.cleanup)
        self.root = Path(self.temp_dir.name).resolve()
        self.backend = RestrictedShellBackend(self.root)

    def test_read_only_git_status_is_allowed(self) -> None:
        resolved = self.backend.validate(["git", "status", "--short"], str(self.root), None)
        self.assertEqual(resolved, self.root)

    def test_mutating_or_redirected_commands_are_rejected(self) -> None:
        rejected = [
            ["git", "commit", "-m", "bad"],
            ["git", "status", "--git-dir=C:\\outside"],
            ["powershell", "-Command", "Write-Host bad"],
            ["cmake", "--build", "out"],
        ]
        for command in rejected:
            with self.subTest(command=command), self.assertRaises(SecurityViolation):
                self.backend.validate(command, str(self.root), None)


if __name__ == "__main__":
    unittest.main()
