"""Regression tests for the publication gate; no build toolchain is required."""

import contextlib
import importlib.util
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/check-repository.py"
SPEC = importlib.util.spec_from_file_location("repository_checks", SCRIPT)
CHECKS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKS)


class PublicationChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pause-publication-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        subprocess.run(["git", "init", "--quiet", str(self.root)], check=True)
        (self.root / ".gitignore").write_text("build/\nCMakeUserPresets.json\n", encoding="utf-8")

    def check(self):
        output = io.StringIO()
        with patch.object(CHECKS, "ROOT", self.root), patch.object(CHECKS, "REQUIRED", ()), \
             patch.object(sys, "argv", [str(SCRIPT)]), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = CHECKS.main()
        return result, output.getvalue()

    def test_source_links_and_directories_are_published(self):
        (self.root / "docs").mkdir()
        (self.root / "docs/guide.md").write_text("# Guide\n", encoding="utf-8")
        (self.root / "README.md").write_text("[guide](docs/guide.md) [index](docs/)\n", encoding="utf-8")
        self.assertEqual(self.check()[0], 0)

    def test_local_ignored_document_is_not_a_published_link(self):
        (self.root / "CMakeUserPresets.json").write_text("{}\n", encoding="utf-8")
        (self.root / "README.md").write_text("[local](CMakeUserPresets.json)\n", encoding="utf-8")
        result, diagnostic = self.check()
        self.assertEqual(result, 1)
        self.assertIn("local link is not published", diagnostic)

    def test_missing_heading_is_rejected(self):
        (self.root / "README.md").write_text("# Title\n[missing](#missing)\n", encoding="utf-8")
        result, diagnostic = self.check()
        self.assertEqual(result, 1)
        self.assertIn("missing document anchor", diagnostic)

    def test_unicode_and_duplicate_headings_are_valid(self):
        (self.root / "README.md").write_text(
            "# Title\n## 用法\n## Title\n[one](#title) [two](#title-1) [unicode](#用法)\n",
            encoding="utf-8",
        )
        self.assertEqual(self.check()[0], 0)

    def test_tracked_ignored_file_is_rejected(self):
        (self.root / "CMakeUserPresets.json").write_text("{}\n", encoding="utf-8")
        subprocess.run(["git", "add", "-f", "CMakeUserPresets.json"], cwd=self.root, check=True)
        result, diagnostic = self.check()
        self.assertEqual(result, 1)
        self.assertIn("ignored/local file is tracked", diagnostic)

    def test_build_evidence_is_not_a_published_link(self):
        (self.root / "docs").mkdir()
        (self.root / "docs/report.md").write_text("[log](../build/old.log)\n", encoding="utf-8")
        self.assertEqual(self.check()[0], 1)
        (self.root / "build").mkdir()
        (self.root / "build/old.log").write_text("local evidence\n", encoding="utf-8")
        self.assertEqual(self.check()[0], 1)

    def test_dangling_symlink_is_rejected(self):
        try:
            (self.root / "dangling.md").symlink_to("missing.md")
        except OSError:
            self.skipTest("creating symbolic links is not permitted on this host")
        result, diagnostic = self.check()
        self.assertEqual(result, 1)
        self.assertIn("source symlink", diagnostic)

    def test_staged_file_deleted_from_worktree_is_not_a_valid_link(self):
        (self.root / "gone.md").write_text("# Gone\n", encoding="utf-8")
        subprocess.run(["git", "add", "gone.md"], cwd=self.root, check=True)
        (self.root / "gone.md").unlink()
        (self.root / "README.md").write_text("[gone](gone.md)\n", encoding="utf-8")
        self.assertEqual(self.check()[0], 1)


if __name__ == "__main__":
    unittest.main()
