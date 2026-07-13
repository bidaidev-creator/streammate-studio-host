#!/usr/bin/env python3
"""NIF-H2 (host): real user-plugin loading + boot-frozen plugins.report.

User modules named by a `--user-plugins-manifest` are loaded PER-MODULE via
obs_open_module/obs_init_module between the bundled obs_load_all_modules()
and obs_post_load_modules() (HAS_LIBOBS lane only). `plugins.report` serves a
boot-frozen, deterministic per-module outcome record:

  - plan outcomes (both lanes, static facts only): selection gating
    (`not-selected`), exclusion, duplicate_in_roots, and a static
    architecture_mismatch state read from the Mach-O header;
  - load outcomes (HAS_LIBOBS lane only): lifecycle loaded/load_failed,
    per-module registered-type DELTAS (all six kinds), and sanitized failure
    classes mapped from the module-open result (never raw dlerror text).

Scaffold-lane honesty: the scaffold NEVER claims loading. Would-be load
candidates stay `lifecycle: "discovered"` with `reasonDetail:
"scaffold-not-loaded"`; no record ever carries lifecycle "loaded",
"load_failed", or a registeredTypes field, and the whole payload is labeled
`mode: "scaffold"`.

The negative assertion binding NIF-H2 (spec 39): a module that merely loaded
and registered types reports `state: null` — never `compatible`. The derived
`compatible` verdict requires the separate instance+render probe smoke
(user-plugin-module-smoke, HAS_LIBOBS CI).

The HAS_LIBOBS end-to-end class below is env-gated (STREAMMATE_EXPECT_LIBOBS=1)
and runs in CI against the packaged app with the real in-tree test plugins.
"""
from __future__ import annotations

import hashlib
import json
import struct
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import test_host_lifecycle as host
from test_plugin_discovery import (
    CPU_TYPE_ARM64,
    CPU_TYPE_X86_64,
    build_bundle,
    build_legacy,
    recv_raw_text,
    rpc_raw,
    thin_macho64,
    tree_digest,
)

HOST_BIN = host.HOST_BIN
IS_MACOS = sys.platform == "darwin"
EXPECT_LIBOBS = os.environ.get("STREAMMATE_EXPECT_LIBOBS", "") == "1"

REPO_ROOT = Path(__file__).resolve().parents[2]

HOST_CPU = CPU_TYPE_ARM64 if platform.machine() in ("arm64", "aarch64") else CPU_TYPE_X86_64
OTHER_CPU = CPU_TYPE_X86_64 if HOST_CPU == CPU_TYPE_ARM64 else CPU_TYPE_ARM64

ALL_TYPE_KINDS = ("sources", "filters", "transitions", "outputs", "encoders", "services")


def host_arch_macho() -> bytes:
    """A binary whose Mach-O header matches the host CPU (the real host binary
    on macOS, a fabricated thin header elsewhere)."""
    return HOST_BIN.read_bytes() if IS_MACOS else thin_macho64(HOST_CPU)


def write_manifest(path: Path, roots: list[dict], selected, exclude) -> None:
    path.write_text(
        json.dumps({"version": 1, "roots": roots, "selected": selected, "exclude": exclude})
    )


def boot_and_report_raw(manifest: Path, cleanup) -> str:
    """Boot a fresh host with the manifest and return the RAW plugins.report
    response frame text (for byte-level determinism assertions)."""
    process, port, _ = host.start_host("--user-plugins-manifest", str(manifest))
    try:
        sock = host.websocket_connect(port)
        try:
            return rpc_raw(sock, 11, "plugins.report", {})
        finally:
            sock.close()
    finally:
        host.stop_process(process)


class UserPluginLoadingPlanTest(unittest.TestCase):
    """Scaffold-lane plan honesty (runs everywhere, including HAS_LIBOBS CI
    where the same assertions hold for plan-only records)."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.root0 = self.base / "root0"
        self.root1 = self.base / "root1"
        self.root0.mkdir()
        self.root1.mkdir()

        # root0: a selected host-arch candidate, an unselected host-arch
        # candidate, an excluded candidate, and a selected wrong-arch candidate.
        build_bundle(self.root0, "alpha", host_arch_macho())
        build_bundle(self.root0, "beta", thin_macho64(HOST_CPU))
        build_bundle(self.root0, "excluded-mod", thin_macho64(HOST_CPU))
        build_bundle(self.root0, "otherarch", thin_macho64(OTHER_CPU))
        # root1: a duplicate of alpha (first-root-wins).
        build_bundle(self.root1, "alpha", thin_macho64(HOST_CPU))

        self.manifest = self.base / "manifest.json"
        write_manifest(
            self.manifest,
            [{"binaryDir": str(self.root0)}, {"binaryDir": str(self.root1)}],
            ["module:alpha", "module:otherarch"],
            ["module:excluded-mod"],
        )

    def _report(self, manifest: Path) -> dict:
        process, port, _ = host.start_host("--user-plugins-manifest", str(manifest))
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)
        raw = rpc_raw(sock, 3, "plugins.report", {})
        return json.loads(raw)["result"]

    def test_report_carries_plan_outcomes(self) -> None:
        result = self._report(self.manifest)
        self.assertIs(result["ok"], True)
        self.assertIn(result["mode"], ("scaffold", "libobs"))
        by_key = {(m["moduleRef"], m["rootRef"]): m for m in result["modules"]}

        alpha = by_key[("module:alpha", "root:0")]
        if result["mode"] == "scaffold":
            self.assertEqual(alpha["lifecycle"], "discovered")
            self.assertIsNone(alpha["state"])
            self.assertEqual(alpha["reasonDetail"], "scaffold-not-loaded")
            self.assertNotIn("registeredTypes", alpha)
        else:
            # alpha is the host EXECUTABLE staged as a bundle: the libobs lane
            # ATTEMPTS it (it passed the static plan) and it must fail honestly
            # as a load attempt, never silently vanish or claim types.
            self.assertEqual(alpha["lifecycle"], "load_failed")
            self.assertNotIn("registeredTypes", alpha)

        beta = by_key[("module:beta", "root:0")]
        self.assertEqual(beta["lifecycle"], "discovered")
        self.assertEqual(beta["reasonDetail"], "not-selected")
        self.assertIsNone(beta["state"])
        self.assertNotIn("registeredTypes", beta)

        excluded = by_key[("module:excluded-mod", "root:0")]
        self.assertEqual(excluded["lifecycle"], "excluded")
        self.assertIsNone(excluded["reasonDetail"])

        otherarch = by_key[("module:otherarch", "root:0")]
        self.assertEqual(otherarch["lifecycle"], "discovered")
        self.assertEqual(otherarch["state"], "architecture_mismatch")
        self.assertIs(otherarch["observed"]["arch"], True)
        self.assertNotIn("registeredTypes", otherarch)

        duplicate = by_key[("module:alpha", "root:1")]
        self.assertEqual(duplicate["lifecycle"], "duplicate_in_roots")
        self.assertNotIn("registeredTypes", duplicate)

    def test_selected_null_makes_all_eligible_and_exclude_still_wins(self) -> None:
        manifest = self.base / "manifest-all.json"
        write_manifest(
            manifest,
            [{"binaryDir": str(self.root0)}],
            None,
            ["module:excluded-mod"],
        )
        result = self._report(manifest)
        by_ref = {m["moduleRef"]: m for m in result["modules"]}
        self.assertEqual(by_ref["module:excluded-mod"]["lifecycle"], "excluded")
        if result["mode"] == "scaffold":
            self.assertEqual(by_ref["module:alpha"]["reasonDetail"], "scaffold-not-loaded")
            self.assertEqual(by_ref["module:beta"]["reasonDetail"], "scaffold-not-loaded")
        # A wrong-arch candidate is never a load candidate, selected or not.
        self.assertEqual(by_ref["module:otherarch"]["state"], "architecture_mismatch")

    def test_scaffold_never_claims_loading(self) -> None:
        if EXPECT_LIBOBS:
            self.skipTest("libobs lane: loading assertions live in the end-to-end class")
        result = self._report(self.manifest)
        self.assertEqual(result["mode"], "scaffold")
        for module in result["modules"]:
            self.assertNotIn(module["lifecycle"], ("loaded", "load_failed"))
            self.assertNotIn("registeredTypes", module)
            self.assertIs(module["observed"]["dependencies"], False)

    def test_report_is_boot_frozen_and_deterministic_across_boots(self) -> None:
        first = boot_and_report_raw(self.manifest, self.addCleanup)
        second = boot_and_report_raw(self.manifest, self.addCleanup)
        self.assertEqual(first, second)

    def test_boot_load_path_is_read_only(self) -> None:
        before0 = tree_digest(self.root0)
        before1 = tree_digest(self.root1)
        boot_and_report_raw(self.manifest, self.addCleanup)
        self.assertEqual(tree_digest(self.root0), before0)
        self.assertEqual(tree_digest(self.root1), before1)

    def test_report_without_manifest_stays_empty(self) -> None:
        process, port, _ = host.start_host()
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)
        result = json.loads(rpc_raw(sock, 5, "plugins.report", {}))["result"]
        self.assertEqual(result["modules"], [])

    def test_nonportable_names_are_clamped_to_refused_records(self) -> None:
        # F2 (opus review): names the host accepts (is_safe_protocol_id) but
        # the mono validator refuses (leading ._-/_ or >120 chars) must never
        # produce a non-conforming moduleRef. They surface as REFUSED records
        # with a synthesized conforming ref + reasonDetail name-not-portable —
        # never silently dropped, never inventory-poisoning, never loaded.
        root = self.base / "clamp-root"
        root.mkdir()
        build_bundle(root, "_hidden", thin_macho64(HOST_CPU))
        long_name = "a" * 130
        build_bundle(root, long_name, thin_macho64(HOST_CPU))
        build_bundle(root, "portable-mod", thin_macho64(HOST_CPU))
        manifest = self.base / "clamp-manifest.json"
        write_manifest(manifest, [{"binaryDir": str(root)}], None, [])

        result = self._report(manifest)
        refs = [m["moduleRef"] for m in result["modules"]]
        # No mono-refused ref shapes may ever appear on the wire.
        for ref in refs:
            self.assertRegex(ref, r"^module:[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
            self.assertLessEqual(len(ref), 135)
        self.assertIn("module:portable-mod", refs)

        clamped = [m for m in result["modules"] if m["reasonDetail"] == "name-not-portable"]
        self.assertEqual(len(clamped), 2)
        for record in clamped:
            self.assertRegex(record["moduleRef"], r"^module:unportable-\d+-\d+$")
            self.assertEqual(record["lifecycle"], "discovered")
            self.assertIsNone(record["sha256"])
            self.assertEqual(record["arch"], [])
            self.assertNotIn("registeredTypes", record)
            self.assertNotIn("_hidden", json.dumps(record))
            self.assertNotIn(long_name, json.dumps(record))

    def test_discover_filenames_are_bare_names(self) -> None:
        # Mono contract (StudioPluginModuleRecord.fileName): bare bundle or
        # library file name, never a relative path with separators.
        build_legacy(self.root1, "legacy-mod", thin_macho64(HOST_CPU), subdir="nested")
        process, port, _ = host.start_host("--user-plugins-manifest", str(self.manifest))
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)
        result = json.loads(rpc_raw(sock, 7, "plugins.discover", {}))["result"]
        names = {m["moduleRef"]: m["fileName"] for m in result["modules"]}
        self.assertEqual(names["module:alpha"], "alpha.plugin")
        self.assertEqual(names["module:legacy-mod"], "legacy-mod.so")
        for file_name in names.values():
            self.assertNotIn("/", file_name)


class UserPluginStaticContractTest(unittest.TestCase):
    """Static contracts: the in-tree test plugins, the instance+render probe
    smoke, and the CI wiring exist exactly as the workpack pins them."""

    def _cmake(self) -> str:
        return (REPO_ROOT / "CMakeLists.txt").read_text()

    def _workflow(self) -> str:
        return (REPO_ROOT / ".github" / "workflows" / "macos-ci.yml").read_text()

    def test_cmake_declares_test_plugin_module_targets(self) -> None:
        cmake = self._cmake()
        # The three test modules are declared through one foreach over MODULE
        # bundles; each name must be in the list and the loop must build a
        # BUNDLE_EXTENSION plugin MODULE library.
        for target in ("streammate-test-source", "streammate-test-filter", "streammate-test-noop"):
            self.assertIn(target, cmake)
        self.assertRegex(cmake, r"add_library\(\$\{test_module\}\s+MODULE")
        self.assertIn("user-plugin-module-smoke", cmake)

    def test_module_sources_register_pinned_type_ids(self) -> None:
        source = (REPO_ROOT / "src" / "obs_module" / "streammate_test_source_module.cpp").read_text()
        self.assertIn('"streammate_test_source"', source)
        self.assertIn("OBS_SOURCE_TYPE_INPUT", source)
        filt = (REPO_ROOT / "src" / "obs_module" / "streammate_test_filter_module.cpp").read_text()
        self.assertIn('"streammate_test_filter"', filt)
        self.assertIn("OBS_SOURCE_TYPE_FILTER", filt)
        noop = (REPO_ROOT / "src" / "obs_module" / "streammate_test_noop_module.cpp").read_text()
        self.assertNotIn("obs_register_source", noop)

    def test_ci_wires_user_plugin_loading_lane(self) -> None:
        workflow = self._workflow()
        # Real-loading proof: the packaged host + the python end-to-end lane.
        self.assertIn("STREAMMATE_EXPECT_LIBOBS", workflow)
        self.assertIn("test_user_plugin_loading.py", workflow)
        # Instance+render probe smoke (the `compatible` matrix row for NIF-H2).
        self.assertIn("user-plugin-module-smoke", workflow)
        # The artifact ships the test plugins for local slices (NIF-V1).
        self.assertIn("dist/test-plugins", workflow)


@unittest.skipUnless(EXPECT_LIBOBS, "HAS_LIBOBS lane only (STREAMMATE_EXPECT_LIBOBS=1)")
class UserPluginLoadingLibobsTest(unittest.TestCase):
    """End-to-end real loading against the packaged host (CI HAS_LIBOBS lane).

    Env contract (set by the CI step):
      STREAMMATE_TEST_SOURCE_PLUGIN / STREAMMATE_TEST_FILTER_PLUGIN /
      STREAMMATE_TEST_NOOP_PLUGIN   — built .plugin bundle directories
      STREAMMATE_DEP_MISSING_PLUGIN — test-source copy whose libobs load
                                      command was rewritten to a nonexistent
                                      dylib (then re-signed)
    """

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.root = self.base / "user-plugins"
        self.root.mkdir()

        for env_key, name in (
            ("STREAMMATE_TEST_SOURCE_PLUGIN", "streammate-test-source"),
            ("STREAMMATE_TEST_FILTER_PLUGIN", "streammate-test-filter"),
            ("STREAMMATE_TEST_NOOP_PLUGIN", "streammate-test-noop"),
            ("STREAMMATE_DEP_MISSING_PLUGIN", "depmiss"),
        ):
            bundle = os.environ.get(env_key, "")
            self.assertTrue(bundle and Path(bundle).is_dir(), f"{env_key} must name a bundle dir")
            shutil.copytree(bundle, self.root / f"{name}.plugin", symlinks=False)
        # depmiss bundle binary must carry the module name inside MacOS/.
        depmiss_macos = self.root / "depmiss.plugin" / "Contents" / "MacOS"
        binaries = sorted(depmiss_macos.iterdir())
        self.assertTrue(binaries)
        if binaries[0].name != "depmiss":
            binaries[0].rename(depmiss_macos / "depmiss")

        # A user module colliding with a bundled upstream module id: bundled
        # wins, never loaded (uses the test-source binary under the taken name).
        mac_capture = self.root / "mac-capture.plugin" / "Contents" / "MacOS"
        mac_capture.mkdir(parents=True)
        shutil.copy2(
            self.root / "streammate-test-source.plugin" / "Contents" / "MacOS" / "streammate-test-source",
            mac_capture / "mac-capture",
        )
        # Broken module: a VALID host-arch Mach-O header (so the static plan
        # admits it) followed by nothing loadable — the dlopen itself fails.
        broken = self.root / "broken.plugin" / "Contents" / "MacOS"
        broken.mkdir(parents=True)
        (broken / "broken").write_bytes(thin_macho64(HOST_CPU) + b"\x00" * 4096)

        # Missing-exports: a real loadable dylib that is NOT an OBS module
        # (supplied by CI; the packaged app's libobs-opengl.dylib).
        noexports_dylib = os.environ.get("STREAMMATE_NOEXPORTS_DYLIB", "")
        self.assertTrue(noexports_dylib and Path(noexports_dylib).is_file(),
                        "STREAMMATE_NOEXPORTS_DYLIB must name a loadable dylib")
        noexports = self.root / "noexports.plugin" / "Contents" / "MacOS"
        noexports.mkdir(parents=True)
        shutil.copy2(noexports_dylib, noexports / "noexports")
        # Wrong-arch candidate: never attempted.
        build_bundle(self.root, "otherarch", thin_macho64(OTHER_CPU))

        self.manifest = self.base / "manifest.json"
        write_manifest(self.manifest, [{"binaryDir": str(self.root)}], None, [])

    def test_vertical_slice_verbs_end_to_end(self) -> None:
        """NIF-V1: plugin-kind instantiation, real frame capture, generic
        production actions, and mapped_plugin import classification against
        the packaged HAS_LIBOBS host."""
        process, port, _ = host.start_host("--user-plugins-manifest", str(self.manifest))
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)

        loaded = host.rpc(sock, 9401, "scene.load", {"sceneId": "slice", "width": 128, "height": 72})
        self.assertTrue(loaded["result"]["ok"])
        set_program = host.rpc(sock, 9402, "scene.setProgram", {"sceneId": "slice"})
        self.assertTrue(set_program["result"]["ok"])

        # Instantiate the plugin source WITH settings and the plugin filter
        # attached — no placeholder path exists for plugin kinds.
        color = 0xFFFF8020
        created = host.rpc(
            sock,
            9403,
            "source.create",
            {
                "sceneId": "slice",
                "sourceId": "slice-src",
                "kind": "streammate_test_source",
                "settings": {"color": color},
                "pluginFilterKind": "streammate_test_filter",
                "filterId": "slice-filter",
            },
        )
        self.assertTrue(created["result"]["ok"])
        self.assertEqual(created["result"]["kind"], "streammate_test_source")

        # Real async frame: deterministic pattern derived from the settings.
        cap1 = host.rpc(sock, 9404, "source.captureFrame", {"sourceId": "slice-src"})
        self.assertTrue(cap1["result"]["ok"])
        self.assertEqual(cap1["result"]["renderer"], "libobs-async-frame")
        self.assertEqual(cap1["result"]["format"], "bgra")
        self.assertEqual(cap1["result"]["width"], 8)
        self.assertEqual(cap1["result"]["height"], 8)
        self.assertIs(cap1["result"]["nonZeroBytes"], True)
        expected = hashlib.sha256(struct.pack("<I", color) * 64).hexdigest()
        self.assertEqual(cap1["result"]["frameSha256"], expected)

        cap2 = host.rpc(sock, 9405, "source.captureFrame", {"sourceId": "slice-src"})
        self.assertEqual(cap2["result"]["frameSha256"], cap1["result"]["frameSha256"])

        # The attached plugin filter is listed and toggles (generic production
        # action on the plugin-backed filter).
        filters = host.rpc(sock, 9406, "filter.list", {"sourceId": "slice-src"})
        filter_ids = [f["filterId"] for f in filters["result"]["filters"]]
        self.assertIn("slice-filter", filter_ids)
        toggled = host.rpc(sock, 9407, "filter.setEnabled",
                           {"sourceId": "slice-src", "filterId": "slice-filter", "enabled": False})
        self.assertTrue(toggled["result"]["ok"])
        retoggled = host.rpc(sock, 9408, "filter.setEnabled",
                             {"sourceId": "slice-src", "filterId": "slice-filter", "enabled": True})
        self.assertTrue(retoggled["result"]["ok"])

        # Generic production action on the plugin-backed source.
        hidden = host.rpc(sock, 9409, "sceneItem.setVisible",
                          {"sceneId": "slice", "itemId": "slice-src", "visible": False})
        self.assertTrue(hidden["result"]["ok"])
        shown = host.rpc(sock, 9410, "sceneItem.setVisible",
                         {"sceneId": "slice", "itemId": "slice-src", "visible": True})
        self.assertTrue(shown["result"]["ok"])

        # Import classification: a collection whose source type is registered
        # by a LOADED user plugin maps as mapped_plugin (no placeholder).
        obs_dir = host.write_obs_fixture(self.base)
        scenes_path = obs_dir / "basic" / "scenes" / "fixture-main.json"
        scenes = json.loads(scenes_path.read_text(encoding="utf-8"))
        scenes["sources"].append({"name": "Slice Plugin Source", "id": "streammate_test_source",
                                  "settings": {"color": color}})
        scenes["sources"].append({"name": "Slice Plugin Filter", "id": "streammate_test_filter"})
        scenes_path.write_text(json.dumps(scenes, indent=2) + "\n", encoding="utf-8")

        scan = host.rpc(sock, 9411, "import.scan", {"configDir": str(obs_dir)})
        collection_id = scan["result"]["collections"][0]["collectionId"]
        report = host.rpc(sock, 9412, "import.load", {"collectionId": collection_id})["result"]["report"]
        mapped_by_module = {item.get("moduleName"): item for item in report["mapped"]}
        self.assertIn("streammate_test_source", mapped_by_module)
        self.assertEqual(mapped_by_module["streammate_test_source"]["reason"], "mapped_plugin")
        self.assertIn("streammate_test_filter", mapped_by_module)
        self.assertEqual(mapped_by_module["streammate_test_filter"]["reason"], "mapped_plugin")
        for bucket in ("degraded", "unresolved"):
            for item in report[bucket]:
                self.assertNotIn(item.get("moduleName"), ("streammate_test_source", "streammate_test_filter"))

    def test_real_loading_end_to_end(self) -> None:
        before = tree_digest(self.root)
        raw_first = boot_and_report_raw(self.manifest, self.addCleanup)
        result = json.loads(raw_first)["result"]
        self.assertEqual(result["mode"], "libobs")
        by_ref = {m["moduleRef"]: m for m in result["modules"]}

        source = by_ref["module:streammate-test-source"]
        self.assertEqual(source["lifecycle"], "loaded")
        # NEGATIVE ASSERTION (spec 39): load + registration alone is never
        # `compatible` — the derived verdict needs the instance+render smoke.
        self.assertIsNone(source["state"])
        self.assertIs(source["observed"]["dependencies"], True)
        self.assertEqual(sorted(source["registeredTypes"].keys()), sorted(ALL_TYPE_KINDS))
        self.assertEqual(source["registeredTypes"]["sources"], ["streammate_test_source"])
        self.assertEqual(source["registeredTypes"]["filters"], [])

        filt = by_ref["module:streammate-test-filter"]
        self.assertEqual(filt["lifecycle"], "loaded")
        self.assertIsNone(filt["state"])
        # F1 (opus review): the module ALSO registers a non-portable type id
        # (leading underscore, >64 chars); the emitted delta must carry only
        # ids the mono STUDIO_PLUGIN_TYPE_ID_PATTERN accepts, while the module
        # still reports loaded honestly.
        self.assertEqual(filt["registeredTypes"]["filters"], ["streammate_test_filter"])
        self.assertEqual(filt["registeredTypes"]["sources"], [])
        for kind_ids in filt["registeredTypes"].values():
            for type_id in kind_ids:
                self.assertRegex(type_id, r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")

        noop = by_ref["module:streammate-test-noop"]
        self.assertEqual(noop["lifecycle"], "loaded")
        self.assertEqual(noop["state"], "type_not_registered")
        self.assertTrue(all(noop["registeredTypes"][kind] == [] for kind in ALL_TYPE_KINDS))

        broken = by_ref["module:broken"]
        self.assertEqual(broken["lifecycle"], "load_failed")
        self.assertEqual(broken["state"], "module_load_failed")

        noexports = by_ref["module:noexports"]
        self.assertEqual(noexports["lifecycle"], "load_failed")
        self.assertEqual(noexports["state"], "module_load_failed")
        self.assertEqual(noexports["reasonDetail"], "missing-exports")

        depmiss = by_ref["module:depmiss"]
        self.assertEqual(depmiss["lifecycle"], "load_failed")
        self.assertEqual(depmiss["state"], "dependency_missing")
        self.assertIs(depmiss["observed"]["dependencies"], True)

        collided = by_ref["module:mac-capture"]
        self.assertEqual(collided["lifecycle"], "duplicate_of_bundled")
        self.assertNotIn("registeredTypes", collided)

        otherarch = by_ref["module:otherarch"]
        self.assertEqual(otherarch["lifecycle"], "discovered")
        self.assertEqual(otherarch["state"], "architecture_mismatch")

        # Sanitized failure detail only: no raw path or dlerror text anywhere.
        self.assertNotIn(str(self.root), raw_first)

        # Restart-stable: a second boot with the same manifest is byte-identical.
        raw_second = boot_and_report_raw(self.manifest, self.addCleanup)
        self.assertEqual(raw_first, raw_second)

        # Loading never mutates the user's plugin directories.
        self.assertEqual(tree_digest(self.root), before)


if __name__ == "__main__":
    sys.argv = sys.argv[:1] + sys.argv[2:]
    unittest.main(verbosity=2)
