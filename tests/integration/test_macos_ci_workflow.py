#!/usr/bin/env python3
import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "macos-ci.yml"


class MacosCiWorkflowTest(unittest.TestCase):
    def test_dependency_pin_guard_and_payload_revision_are_wired(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("./scripts/verify-deps-pin.sh", workflow)
        self.assertEqual(workflow.count('--source-revision "$GITHUB_SHA"'), 2)
        self.assertIn(
            "StreamMateStudioHost.app sha256-manifest.txt THIRD-PARTY-NOTICES.md PAYLOAD-PROVENANCE.json",
            workflow,
        )

    def test_obs_configure_keeps_xcode_16_deprecation_warnings_non_fatal(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        start = workflow.index("      - name: Configure upstream OBS without Qt frontend")
        end = workflow.index("      - name: Build libobs and required upstream modules")
        body = workflow[start:end]
        for flag_name in ["CMAKE_OBJC_FLAGS", "CMAKE_OBJCXX_FLAGS", "CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS"]:
            with self.subTest(flag_name=flag_name):
                self.assertIn(f"-D{flag_name}=", body)
        self.assertIn("-Wno-error=deprecated-declarations", body)
        self.assertIn("-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0", body)

    def test_ci_stages_all_required_plugin_bundles_for_packaging(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("streammate-plugin-stage", workflow)
        self.assertIn('-path "*/$module.plugin"', workflow)
        for module in ["mac-avcapture", "mac-capture", "obs-outputs", "obs-x264"]:
            with self.subTest(module=module):
                self.assertIn(module, workflow)

    def test_ci_strict_verifies_signature_before_upload(self) -> None:
        # The inside-out signature must be verified on the real bundle before
        # actions/upload-artifact runs (upload strips framework symlinks, so a
        # post-upload strict verify would be meaningless).
        workflow = WORKFLOW.read_text(encoding="utf-8")
        verify_index = workflow.index("codesign --verify --strict --deep")
        upload_index = workflow.index("- name: Upload artifact")
        self.assertLess(
            verify_index,
            upload_index,
            "strict codesign verification must run before the artifact upload",
        )
        # The outer identifier is asserted stable in CI too.
        self.assertIn("com.streammate.studio-host", workflow[verify_index:upload_index])

    def test_ci_checks_repack_determinism(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertRegex(workflow, r"(?i)repack")
        upload_index = workflow.index("- name: Upload artifact")
        self.assertLess(
            workflow.lower().index("repack"),
            upload_index,
            "repack-determinism check must run before the artifact upload",
        )

    def test_actions_are_pinned_by_commit_and_no_exit_code_is_masked(self) -> None:
        # The v2 candidate runner's workflow guard (streammate-pivot#118) fails
        # a mutable action tag and an exit code a `|| true` swallows.
        code = [line for line in WORKFLOW.read_text(encoding="utf-8").splitlines() if not line.lstrip().startswith("#")]
        uses = [line.strip() for line in code if line.strip().startswith(("uses:", "- uses:"))]
        self.assertGreaterEqual(len(uses), 2)
        for line in uses:
            with self.subTest(uses=line):
                self.assertRegex(line, r"@[0-9a-f]{40}( |$)")
        self.assertEqual([line for line in code if "|| true" in line], [])
        for module in ["coreaudio-encoder", "mac-videotoolbox", "obs-ffmpeg"]:
            with self.subTest(module=module):
                self.assertTrue(any(re.search(rf"--target {module}$", line) for line in code))


if __name__ == "__main__":
    unittest.main()
