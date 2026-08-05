#!/usr/bin/env python3
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "windows-ci.yml"


class WindowsCiWorkflowTest(unittest.TestCase):
    def test_workflow_is_one_bounded_windows_scaffold_job(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("  windows-scaffold:", workflow)
        self.assertEqual(workflow.count("    runs-on:"), 1)
        self.assertIn("runs-on: windows-latest", workflow)
        self.assertIn("timeout-minutes: 40", workflow)

    def test_checkout_and_pinned_build_tools_are_wired(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("uses: actions/checkout@v4", workflow)
        self.assertIn("submodules: recursive", workflow)
        self.assertIn("cmake==3.28.4", workflow)
        self.assertIn("ninja", workflow)
        self.assertIn("uses: ilammy/msvc-dev-cmd@v1", workflow)
        self.assertIn("arch: x64", workflow)
        self.assertIn("-G Ninja", workflow)

    def test_scaffold_ladder_and_pin_guards_are_enforced(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("bash scripts/fork-guard.sh", workflow)
        self.assertIn("-DSTREAMMATE_REQUIRE_LIBOBS=OFF", workflow)
        self.assertIn("studio-host-smoke.exe", workflow)
        self.assertIn("ctest --test-dir build/scaffold --output-on-failure", workflow)
        self.assertIn("STREAMMATE_REQUIRE_OBS_TREE=1", workflow)

    def test_repository_pins_lf_for_windows_checkout(self) -> None:
        attributes = (REPO_ROOT / ".gitattributes").read_text(encoding="utf-8")
        self.assertIn("* text=auto eol=lf", attributes)
        self.assertIn("*.sh text eol=lf", attributes)

    def test_macos_only_ctest_disposition_is_loud_on_windows(self) -> None:
        cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("if(APPLE)", cmake)
        self.assertIn("elseif(WIN32)", cmake)
        self.assertIn(
            "Windows scaffold: not registering macos-packaging or macos-ci-workflow",
            cmake,
        )


if __name__ == "__main__":
    unittest.main()
