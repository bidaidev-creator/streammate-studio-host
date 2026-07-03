#!/usr/bin/env python3
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "macos-ci.yml"


class MacosCiWorkflowTest(unittest.TestCase):
    def test_obs_configure_keeps_xcode_16_deprecation_warnings_non_fatal(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        start = workflow.index("      - name: Configure upstream OBS without Qt frontend")
        end = workflow.index("      - name: Build libobs and required upstream modules")
        body = workflow[start:end]
        for flag_name in ["CMAKE_OBJC_FLAGS", "CMAKE_OBJCXX_FLAGS", "CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS"]:
            with self.subTest(flag_name=flag_name):
                self.assertIn(f"-D{flag_name}=", body)
        self.assertIn("-Wno-error=deprecated-declarations", body)


if __name__ == "__main__":
    unittest.main()
