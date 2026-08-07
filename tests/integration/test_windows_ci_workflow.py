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
        # The libobs job is image-pinned: upstream OBS 32.1.2 needs a VS 2022
        # generator, which the windows-latest (2025, VS 18-only) image lacks.
        self.assertIn("runs-on: windows-2022", workflow)
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
            # Required targets fail the pwsh step on a non-zero exit code.
            self.assertIn(
                f"--target {target}\n          if ($LASTEXITCODE -ne 0) {{ exit $LASTEXITCODE }}",
                workflow,
            )
        # obs-ffmpeg stays optional: it is the LAST build line, with no exit-code
        # guard after it, and the step ends in exit 0 so its failure never gates.
        self.assertIn("--target obs-ffmpeg\n          exit 0", workflow)

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

    def test_libobs_job_packages_the_bare_root_runtime(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("packaging/windows/package-dist.sh", workflow)
        for argument in (
            "--host-bin", "--smoke-bin", "--obs-dll-dir", "--deps-bin-dir",
            "--graphics-module", "--obs-modules-dir", "--streammate-plugin",
            "--libobs-data-dir", "--output-dir dist",
        ):
            self.assertIn(argument, workflow)
        self.assertIn("libobs_data_dir/default.effect", workflow)
        self.assertIn("dist/StreamMateStudioHost/studio-host.exe", workflow)
        self.assertIn("sha256sum -c ../sha256-manifest.txt", workflow)

    def test_packaged_dist_runs_real_control_plugin_and_containment_e2es(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertGreaterEqual(workflow.count("STREAMMATE_EXPECT_LIBOBS=1"), 3)
        self.assertIn("test_production_control_verbs.py", workflow)
        self.assertIn("test_user_plugin_loading.py", workflow)
        self.assertIn("test_plugin_crash_containment.py", workflow)
        for plugin in (
            "streammate-test-source.dll", "streammate-test-filter.dll",
            "streammate-test-noop.dll", "streammate-test-crash.dll",
            "streammate-test-hang.dll",
        ):
            self.assertIn(plugin, workflow)
        self.assertIn("dependency-missing fixture SKIPPED", workflow)
        self.assertIn("STREAMMATE_NOEXPORTS_DYLIB", workflow)

    def test_repack_and_upload_are_pinned(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("--output-dir dist-repack", workflow)
        self.assertIn("diff dist/sha256-manifest.txt dist-repack/sha256-manifest.txt", workflow)
        self.assertIn("cmp dist/StreamMateStudioHost-windows-x64.tar.gz", workflow)
        self.assertIn("uses: actions/upload-artifact@v4", workflow)
        self.assertIn("name: streammate-studio-host-windows-x64", workflow)
        self.assertIn("dist/StreamMateStudioHost-windows-x64.tar.gz", workflow)

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
        self.assertIn("NAME windows-packaging", cmake)
        self.assertIn("test_windows_packaging.py", cmake)
        self.assertIn(
            "macOS: not registering windows-packaging (Windows-only contract)",
            cmake,
        )


if __name__ == "__main__":
    unittest.main()
