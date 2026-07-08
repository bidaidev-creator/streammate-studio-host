#!/usr/bin/env python3
"""Spec 34 Capability 3 / chunk 34.H3 — in-tree OBS plugin module for the native
renderer + CI compile/smoke proof under HAS_LIBOBS (Q-121).

The libobs compile + smoke run only on the CI proof lane (macos-15, full Xcode);
the local box has CommandLineTools only. These assertions are therefore static
contract checks over the module/smoke sources, the CMake MODULE target, the
package-app.sh staging, and the macos-ci.yml smoke wiring — the same shape the
existing test_macos_ci_workflow.py uses for the libobs lane. Together they pin:

  * a real in-tree OBS plugin module (CMake MODULE, obs_module_load/unload,
    obs_register_source) built under STREAMMATE_HAS_LIBOBS with no `|| true`;
  * bundle staging under Contents/PlugIns/obs-plugins/ loaded by the host's
    normal module load path;
  * a HAS_LIBOBS smoke (module-registered source id, apply fixture toast,
    render cycle, frame delivered, clean exit);
  * shared golden hashes across scaffold and HAS_LIBOBS (the rasterizer core
    stays a plain shared library outside the module);
  * a submodule diff guard (no external/obs-studio file modified).
"""
import re
import subprocess
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SRC = REPO_ROOT / "src"
MODULE_SRC = SRC / "obs_module" / "native_overlay_obs_module.cpp"
SMOKE_SRC = SRC / "obs_module" / "native_overlay_module_smoke.cpp"
CMAKE = REPO_ROOT / "CMakeLists.txt"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "macos-ci.yml"
GOLDEN = REPO_ROOT / "tests" / "unit" / "native_overlay_golden.txt"
STUDIO_HOST = SRC / "studio_host.cpp"

# The registered source id the module exports and the smoke/driver create by.
REGISTERED_SOURCE_ID = "streammate_native_overlay"
# The plugin bundle name staged under Contents/PlugIns/obs-plugins/.
PLUGIN_BUNDLE = "streammate-native-overlay"


class NativeOverlayModuleSourceTest(unittest.TestCase):
    def test_module_source_lives_in_host_tree_not_the_submodule(self) -> None:
        # ADR-0005 Decision 1: OBS plugin modules live in our own source tree,
        # never as submodule patches. The module + smoke are under src/.
        self.assertTrue(MODULE_SRC.is_file(), f"missing module source {MODULE_SRC}")
        self.assertTrue(SMOKE_SRC.is_file(), f"missing smoke source {SMOKE_SRC}")
        self.assertFalse(
            (REPO_ROOT / "external" / "obs-studio" / "plugins" / "streammate-native-overlay").exists(),
            "the Stream Mate module must not live inside the obs-studio submodule",
        )

    def test_module_exports_obs_module_entrypoints_and_registers_source(self) -> None:
        text = MODULE_SRC.read_text(encoding="utf-8")
        self.assertIn("OBS_DECLARE_MODULE", text)
        self.assertRegex(text, r"bool\s+obs_module_load\s*\(")
        self.assertRegex(text, r"void\s+obs_module_unload\s*\(")
        self.assertIn("obs_register_source", text)
        self.assertIn("obs_source_info", text)
        # Registered by its stable id, as an async video source that uploads
        # BGRA frames via obs_source_output_video.
        self.assertIn(REGISTERED_SOURCE_ID, text)
        self.assertIn("OBS_SOURCE_ASYNC_VIDEO", text)
        self.assertIn("obs_source_output_video", text)
        self.assertIn("VIDEO_FORMAT_BGRA", text)

    def test_module_shares_the_plain_rasterizer_core_library(self) -> None:
        # The 34.H2 rasterizer core stays a plain host-repo library shared by
        # scaffold mode and the module; the module includes it rather than
        # forking a second rasterizer.
        text = MODULE_SRC.read_text(encoding="utf-8")
        self.assertIn("native_overlay_renderer.h", text)
        self.assertIn("NativeOverlayRenderer", text)

    def test_module_carries_no_product_logic(self) -> None:
        # ADR-0005 Decision 2: the host/module carry no policy/playbook/journal
        # product logic. Guard against leakage in *code* (comments may name the
        # boundary they preserve), so strip // line comments before scanning.
        code = "\n".join(
            line.split("//", 1)[0] for line in MODULE_SRC.read_text(encoding="utf-8").splitlines()
        ).lower()
        for banned in ("playbook", "journal", "approval", "policy-gate"):
            self.assertNotIn(banned, code, f"product-logic token {banned!r} leaked into the module code")


class NativeOverlayModuleSmokeSourceTest(unittest.TestCase):
    def test_smoke_creates_registered_source_applies_toast_asserts_delivery(self) -> None:
        text = SMOKE_SRC.read_text(encoding="utf-8")
        # Loads the module through libobs, then creates the source by its
        # registered id and drives one fixture toast render cycle.
        self.assertIn("obs_startup", text)
        self.assertRegex(text, r"obs_open_module|obs_load_all_modules|obs_add_module_path")
        self.assertIn(REGISTERED_SOURCE_ID, text)
        self.assertIn("obs_source_create", text)
        self.assertIn("toast", text)
        # Asserts a frame was delivered and exits cleanly.
        self.assertRegex(text, r"deliver", "smoke must assert frame delivery")
        self.assertIn("obs_shutdown", text)

    def test_smoke_asserts_shared_golden_hash_with_scaffold(self) -> None:
        # The HAS_LIBOBS smoke proves the module frame uses the same plain
        # rasterizer core as scaffold mode by asserting the toast raster hash
        # equals the shared golden captured in scaffold mode.
        text = SMOKE_SRC.read_text(encoding="utf-8")
        self.assertIn("native_overlay_renderer.h", text)
        self.assertRegex(text, r"golden|raster_hash|fnv1a64", "smoke must tie back to the shared golden hash")


class NativeOverlayModuleCMakeTest(unittest.TestCase):
    def test_cmake_defines_bundled_module_target_under_has_libobs(self) -> None:
        text = CMAKE.read_text(encoding="utf-8")
        self.assertIn(PLUGIN_BUNDLE, text)
        # A CMake MODULE library producing a .plugin bundle loaded by libobs.
        self.assertRegex(text, r"add_library\(\s*streammate-native-overlay\s+MODULE")
        self.assertIn("BUNDLE_EXTENSION plugin", text)
        # Links the shared rasterizer core (not a forked copy) and libobs.
        self.assertRegex(text, r"target_link_libraries\(\s*streammate-native-overlay[^\)]*native-overlay-renderer")

    def test_cmake_defines_has_libobs_module_smoke_target(self) -> None:
        text = CMAKE.read_text(encoding="utf-8")
        self.assertIn("native-overlay-module-smoke", text)
        self.assertRegex(text, r"target_link_libraries\(\s*native-overlay-module-smoke[^\)]*native-overlay-renderer")


class NativeOverlayModuleCiTest(unittest.TestCase):
    def _libobs_lane(self, workflow: str) -> str:
        start = workflow.index("- name: Configure obs_startup smoke against built libobs")
        end = workflow.index("- name: Upload artifact")
        return workflow[start:end]

    def test_ci_compiles_module_target_without_or_true(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        lane = self._libobs_lane(workflow)
        self.assertIn(PLUGIN_BUNDLE, lane, "CI must build the module target on the HAS_LIBOBS lane")
        # No `|| true` on any line that builds our module target (compile proof).
        for line in lane.splitlines():
            if PLUGIN_BUNDLE in line and "cmake --build" in line:
                self.assertNotIn("|| true", line, f"module compile must not be soft-failed: {line!r}")

    def test_ci_builds_and_runs_the_module_smoke(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        lane = self._libobs_lane(workflow)
        self.assertIn("native-overlay-module-smoke", lane)
        # There is an explicit run step for the module smoke.
        self.assertRegex(lane, r"(?m)^\s*(?:\./)?build/host/native-overlay-module-smoke")
        for line in lane.splitlines():
            if "native-overlay-module-smoke" in line and "cmake --build" in line:
                self.assertNotIn("|| true", line, f"module smoke compile must not be soft-failed: {line!r}")

    def test_ci_stages_the_module_into_the_bundle(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("--streammate-plugin", workflow)


class NativeOverlayNormalLoadPathTest(unittest.TestCase):
    def test_host_enumerates_bundle_obs_plugins_via_normal_module_load_path(self) -> None:
        # The staged module is loaded by the host's normal module load path:
        # add_bundle_module_path points obs at Contents/PlugIns/obs-plugins with
        # the %module%.plugin/Contents/MacOS pattern, then obs_load_all_modules
        # enumerates it. Staging location must match that pattern exactly.
        text = STUDIO_HOST.read_text(encoding="utf-8")
        self.assertIn('"PlugIns" / "obs-plugins"', text)
        self.assertIn("%module%.plugin", text)
        self.assertIn("obs_load_all_modules", text)


class NativeOverlaySubmoduleDiffGuardTest(unittest.TestCase):
    def test_obs_studio_submodule_has_no_working_tree_modifications(self) -> None:
        # Diff guard: chunk 34.H3 adds no file under external/obs-studio and
        # modifies none. (fork-guard.sh enforces the pin in CI; this asserts the
        # working tree here.)
        result = subprocess.run(
            ["git", "status", "--porcelain", "--", "external/obs-studio"],
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout.strip(), "", f"obs-studio submodule dirty: {result.stdout!r}")

    def test_ci_runs_the_fork_guard(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertRegex(workflow, r"fork-guard\.sh")


if __name__ == "__main__":
    unittest.main()
