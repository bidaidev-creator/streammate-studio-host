#!/usr/bin/env python3
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "windows-ci.yml"


class WindowsCiWorkflowTest(unittest.TestCase):
    def test_workflow_has_parallel_bounded_scaffold_and_libobs_jobs(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("  windows-scaffold:", workflow)
        self.assertIn("  windows-libobs:", workflow)
        self.assertEqual(workflow.count("    runs-on:"), 2)
        self.assertIn("runs-on: windows-latest", workflow)
        self.assertIn("timeout-minutes: 40", workflow)
        self.assertIn("timeout-minutes: 100", workflow)
        self.assertNotIn("    needs:", workflow)

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

    def test_libobs_build_is_pinned_and_browser_free(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("origin tag 32.1.2", workflow)
        self.assertIn('-G "Visual Studio 17 2022" -A x64', workflow)
        self.assertIn("VIRTUALCAM_GUID=A3FCE0F5-3493-419F-958A-ABA1250EC20B", workflow)
        for flag in (
            "ENABLE_UI",
            "ENABLE_SERVICE_UPDATES",
            "ENABLE_SCRIPTING",
            "ENABLE_WEBRTC",
            "ENABLE_VST",
            "ENABLE_AJA",
            "ENABLE_DECKLINK",
            "ENABLE_BROWSER",
        ):
            self.assertIn(f"-D{flag}=OFF", workflow)
        for target in (
            "libobs",
            "libobs-d3d11",
            "libobs-opengl",
            "obs-outputs",
            "obs-x264",
            "rtmp-services",
            "win-capture",
            "win-wasapi",
        ):
            self.assertIn(f"--target {target}", workflow)
            self.assertNotIn(f"--target {target} || true", workflow)
        self.assertIn("--target obs-ffmpeg || true", workflow)

    def test_libobs_artifacts_feed_the_host_smoke(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("-iname 'obs.lib'", workflow)
        self.assertIn("-iname 'obs.dll'", workflow)
        self.assertIn("-name 'obsconfig.h'", workflow)
        self.assertIn("streammate-plugin-stage", workflow)
        self.assertIn("obs-deps-*-x64", workflow)
        self.assertIn("-DSTREAMMATE_REQUIRE_LIBOBS=ON", workflow)
        self.assertIn("steps.libobs.outputs.obs_dll_dir", workflow)
        self.assertIn("steps.libobs.outputs.deps_bin", workflow)
        self.assertIn("streammate-native-overlay native-overlay-module-smoke", workflow)
        self.assertIn("./build/host/studio-host-smoke.exe", workflow)
        self.assertIn("deferred to the Windows packaging leg", workflow)

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
