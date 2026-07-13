#!/usr/bin/env python3
"""NIF-H3 (host): bounded plugin failure containment.

Crash attribution rides a durable load-phase sentinel file
(`--plugin-load-sentinel <absolute path>`, schema plugin-load-sentinel.v1):
the host records the module ref it is about to load; a crash or watchdog
deadline mid-load leaves that ref in the file, and every later boot refuses
the suspect (lifecycle "excluded", state "runtime_crash", reasonDetail
"crash-suspected" / "hang-suspected") until the manifest explicitly retries
(`retry: [...]`, one fresh attempt) or excludes it. Suspicion is durable
across clean boots (a suspect skipped this boot stays a suspect), so a
crash-looping module can never restart-loop the host: each module crashes at
most once per explicit retry. Unrelated modules are never disabled.

The hang watchdog (`--plugin-load-deadline-ms`, libobs lane) converts a
hanging obs_module_load into a bounded exit (code 65) after writing a
deadline-exceeded sentinel, so attribution survives hangs too.

Scaffold lane proves the lane-agnostic pieces (sentinel grammar, suspicion,
retry, durability, determinism, refusals); the env-gated HAS_LIBOBS class
proves real crash/hang containment with seeded in-tree modules.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import test_host_lifecycle as host
from test_plugin_discovery import (
    HOST_BIN,
    USAGE_EXIT,
    build_bundle,
    recv_raw_text,
    rpc_raw,
    thin_macho64,
    tree_digest,
)
from test_user_plugin_loading import HOST_CPU, host_arch_macho

IS_MACOS = sys.platform == "darwin"
WATCHDOG_EXIT = 65
SENTINEL_SCHEMA = "plugin-load-sentinel.v1"


def write_manifest(path: Path, roots, selected, exclude, retry=None) -> None:
    body = {"version": 1, "roots": roots, "selected": selected, "exclude": exclude}
    if retry is not None:
        body["retry"] = retry
    path.write_text(json.dumps(body))


def write_sentinel(path: Path, phase: str, module_ref=None, suspects=None, consumed=None) -> None:
    body = {"schema": SENTINEL_SCHEMA, "phase": phase, "suspects": suspects or {}}
    if module_ref is not None:
        body["moduleRef"] = module_ref
    if consumed is not None:
        body["consumedRetries"] = consumed
    path.write_text(json.dumps(body))


def read_sentinel(path: Path) -> dict:
    return json.loads(path.read_text())


class PluginCrashContainmentScaffoldTest(unittest.TestCase):
    """Lane-agnostic containment behavior (runs in scaffold and libobs CI)."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.root = self.base / "root"
        self.root.mkdir()
        build_bundle(self.root, "alpha", host_arch_macho())
        build_bundle(self.root, "beta", host_arch_macho())
        self.manifest = self.base / "manifest.json"
        write_manifest(
            self.manifest,
            [{"binaryDir": str(self.root)}],
            ["module:alpha", "module:beta"],
            [],
        )
        self.sentinel = self.base / "plugin-load-sentinel.json"

    # -- helpers ------------------------------------------------------------

    def _report(self, *extra: str) -> dict:
        process, port, _ = host.start_host(
            "--user-plugins-manifest", str(self.manifest), *extra
        )
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)
        return json.loads(rpc_raw(sock, 3, "plugins.report", {}))["result"]

    def _report_raw(self, *extra: str) -> str:
        process, port, _ = host.start_host(
            "--user-plugins-manifest", str(self.manifest), *extra
        )
        try:
            sock = host.websocket_connect(port)
            try:
                return rpc_raw(sock, 7, "plugins.report", {})
            finally:
                sock.close()
        finally:
            host.stop_process(process)

    def _by_ref(self, report: dict) -> dict:
        return {m["moduleRef"]: m for m in report["modules"]}

    def _refusal(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(HOST_BIN), *args],
            capture_output=True,
            text=True,
            timeout=15,
        )

    # -- refusal conventions --------------------------------------------------

    def test_relative_sentinel_path_is_refused_without_leaking_it(self) -> None:
        proc = self._refusal(
            "--user-plugins-manifest", str(self.manifest),
            "--plugin-load-sentinel", "relative/sentinel.json",
        )
        self.assertEqual(proc.returncode, USAGE_EXIT)
        combined = proc.stdout + proc.stderr
        self.assertIn("invalid --plugin-load-sentinel: not-absolute-path", combined)
        self.assertNotIn("relative/sentinel.json", combined)

    def test_malformed_sentinel_is_refused_without_leaking_path(self) -> None:
        secret = self.base / "secret-dir"
        secret.mkdir()
        bad = secret / "sentinel.json"
        bad.write_text("{not json")
        proc = self._refusal(
            "--user-plugins-manifest", str(self.manifest),
            "--plugin-load-sentinel", str(bad),
        )
        self.assertEqual(proc.returncode, USAGE_EXIT)
        combined = proc.stdout + proc.stderr
        self.assertIn("invalid --plugin-load-sentinel: malformed-sentinel", combined)
        self.assertNotIn("secret-dir", combined)

    def test_wrong_schema_sentinel_is_refused(self) -> None:
        bad = self.base / "sentinel.json"
        bad.write_text(json.dumps({"schema": "other.v9", "phase": "idle", "suspects": {}}))
        proc = self._refusal(
            "--user-plugins-manifest", str(self.manifest),
            "--plugin-load-sentinel", str(bad),
        )
        self.assertEqual(proc.returncode, USAGE_EXIT)
        self.assertIn(
            "invalid --plugin-load-sentinel: malformed-sentinel",
            proc.stdout + proc.stderr,
        )

    def test_oversized_sentinel_is_refused(self) -> None:
        bad = self.base / "sentinel.json"
        bad.write_text("[" + " " * 70000 + "]")
        proc = self._refusal(
            "--user-plugins-manifest", str(self.manifest),
            "--plugin-load-sentinel", str(bad),
        )
        self.assertEqual(proc.returncode, USAGE_EXIT)
        self.assertIn(
            "invalid --plugin-load-sentinel: sentinel-too-large",
            proc.stdout + proc.stderr,
        )

    def test_sentinel_without_manifest_is_refused(self) -> None:
        proc = self._refusal("--plugin-load-sentinel", str(self.sentinel))
        self.assertEqual(proc.returncode, USAGE_EXIT)
        self.assertIn(
            "invalid --plugin-load-sentinel: requires-user-plugins-manifest",
            proc.stdout + proc.stderr,
        )

    def test_bad_deadline_is_refused(self) -> None:
        for bad in ("0", "-5", "99", "600001", "abc"):
            proc = self._refusal(
                "--user-plugins-manifest", str(self.manifest),
                "--plugin-load-sentinel", str(self.sentinel),
                "--plugin-load-deadline-ms", bad,
            )
            self.assertEqual(proc.returncode, USAGE_EXIT, bad)
            self.assertIn(
                "invalid --plugin-load-deadline-ms: out-of-range",
                proc.stdout + proc.stderr,
            )

    def test_bad_retry_entry_is_refused(self) -> None:
        write_manifest(
            self.manifest,
            [{"binaryDir": str(self.root)}],
            ["module:alpha"],
            [],
            retry=["not a ref"],
        )
        proc = self._refusal("--user-plugins-manifest", str(self.manifest))
        self.assertEqual(proc.returncode, USAGE_EXIT)
        self.assertIn("invalid --user-plugins-manifest: invalid-retry", proc.stdout + proc.stderr)

    # -- suspicion / attribution ----------------------------------------------

    def test_crashed_load_sentinel_suspends_only_the_suspect(self) -> None:
        write_sentinel(self.sentinel, "loading", "module:alpha")
        report = self._report("--plugin-load-sentinel", str(self.sentinel))
        by_ref = self._by_ref(report)
        alpha = by_ref["module:alpha"]
        self.assertEqual(alpha["lifecycle"], "excluded")
        self.assertEqual(alpha["state"], "runtime_crash")
        self.assertEqual(alpha["reasonDetail"], "crash-suspected")
        beta = by_ref["module:beta"]
        self.assertNotEqual(beta["lifecycle"], "excluded")
        self.assertNotEqual(beta.get("state"), "runtime_crash")
        # The sentinel is rewritten idle with a DURABLE suspects map.
        idle = read_sentinel(self.sentinel)
        self.assertEqual(idle["schema"], SENTINEL_SCHEMA)
        self.assertEqual(idle["phase"], "idle")
        self.assertEqual(idle["suspects"], {"module:alpha": "crash"})

    def test_deadline_sentinel_reports_hang_suspected(self) -> None:
        write_sentinel(self.sentinel, "deadline-exceeded", "module:beta")
        report = self._report("--plugin-load-sentinel", str(self.sentinel))
        beta = self._by_ref(report)["module:beta"]
        self.assertEqual(beta["lifecycle"], "excluded")
        self.assertEqual(beta["state"], "runtime_crash")
        self.assertEqual(beta["reasonDetail"], "hang-suspected")
        self.assertEqual(read_sentinel(self.sentinel)["suspects"], {"module:beta": "hang"})

    def test_suspicion_is_durable_across_clean_boots(self) -> None:
        write_sentinel(self.sentinel, "loading", "module:alpha")
        first = self._report("--plugin-load-sentinel", str(self.sentinel))
        self.assertEqual(self._by_ref(first)["module:alpha"]["reasonDetail"], "crash-suspected")
        # Second boot reads the idle sentinel: alpha must STILL be suspected
        # (a period-2 crash loop would otherwise reappear).
        second = self._report("--plugin-load-sentinel", str(self.sentinel))
        alpha = self._by_ref(second)["module:alpha"]
        self.assertEqual(alpha["lifecycle"], "excluded")
        self.assertEqual(alpha["reasonDetail"], "crash-suspected")
        self.assertEqual(read_sentinel(self.sentinel)["suspects"], {"module:alpha": "crash"})

    def test_retry_clears_suspicion_for_exactly_one_attempt(self) -> None:
        write_sentinel(self.sentinel, "idle", suspects={"module:alpha": "crash"})
        write_manifest(
            self.manifest,
            [{"binaryDir": str(self.root)}],
            ["module:alpha", "module:beta"],
            [],
            retry=["module:alpha"],
        )
        report = self._report("--plugin-load-sentinel", str(self.sentinel))
        alpha = self._by_ref(report)["module:alpha"]
        # Scaffold lane: a fresh (non-suspected) candidate is plan-only.
        self.assertEqual(alpha["lifecycle"], "discovered")
        self.assertEqual(alpha["reasonDetail"], "scaffold-not-loaded")
        self.assertIsNone(alpha["state"])
        # The retried ref left the durable suspects map.
        self.assertEqual(read_sentinel(self.sentinel)["suspects"], {})

    def test_retry_is_consumed_once_a_persistent_retry_cannot_loop(self) -> None:
        # F1 (codex): a retry left in the manifest must be honored EXACTLY once.
        # After the honored attempt re-crashes, the same manifest must keep the
        # module suspected (consumedRetries), never re-clearing every boot.
        write_manifest(
            self.manifest,
            [{"binaryDir": str(self.root)}],
            ["module:alpha", "module:beta"],
            [],
            retry=["module:alpha"],
        )
        # Boot 1: suspicion cleared (retry honored) and recorded consumed.
        write_sentinel(self.sentinel, "idle", suspects={"module:alpha": "crash"})
        first = self._report("--plugin-load-sentinel", str(self.sentinel))
        self.assertEqual(self._by_ref(first)["module:alpha"]["lifecycle"], "discovered")
        idle = read_sentinel(self.sentinel)
        self.assertEqual(idle.get("consumedRetries"), ["module:alpha"])
        # Simulate the retried module crashing again (loading record left behind).
        write_sentinel(
            self.sentinel, "loading", "module:alpha",
            suspects={}, consumed=["module:alpha"],
        )
        # Boot 2, SAME manifest: the spent retry must NOT clear the fresh suspicion.
        second = self._report("--plugin-load-sentinel", str(self.sentinel))
        alpha = self._by_ref(second)["module:alpha"]
        self.assertEqual(alpha["lifecycle"], "excluded")
        self.assertEqual(alpha["reasonDetail"], "crash-suspected")

    def test_withdrawing_a_retry_resets_its_consumption(self) -> None:
        write_sentinel(self.sentinel, "idle", suspects={}, consumed=["module:alpha"])
        write_manifest(
            self.manifest,
            [{"binaryDir": str(self.root)}],
            ["module:alpha", "module:beta"],
            [],
        )
        self._report("--plugin-load-sentinel", str(self.sentinel))
        # No retry in the manifest: the consumption record is dropped, so a
        # future re-added retry is a fresh one-shot grant.
        self.assertEqual(read_sentinel(self.sentinel).get("consumedRetries", []), [])

    def test_unwritable_sentinel_parent_refuses_startup(self) -> None:
        # F2 (codex): loading must never proceed when attribution cannot be
        # persisted; an unwritable sentinel location refuses at launch.
        proc = self._refusal(
            "--user-plugins-manifest", str(self.manifest),
            "--plugin-load-sentinel", str(self.base / "missing-dir" / "sentinel.json"),
        )
        self.assertEqual(proc.returncode, USAGE_EXIT)
        self.assertIn(
            "invalid --plugin-load-sentinel: sentinel-unwritable",
            proc.stdout + proc.stderr,
        )

    def test_manifest_exclude_wins_over_suspicion(self) -> None:
        write_sentinel(self.sentinel, "idle", suspects={"module:alpha": "crash"})
        write_manifest(
            self.manifest,
            [{"binaryDir": str(self.root)}],
            None,
            ["module:alpha"],
        )
        report = self._report("--plugin-load-sentinel", str(self.sentinel))
        alpha = self._by_ref(report)["module:alpha"]
        self.assertEqual(alpha["lifecycle"], "excluded")
        # Plain manifest exclusion, not crash attribution.
        self.assertNotEqual(alpha["reasonDetail"], "crash-suspected")
        self.assertIsNone(alpha["state"])

    def test_reports_are_byte_identical_across_suspected_boots(self) -> None:
        write_sentinel(self.sentinel, "idle", suspects={"module:alpha": "crash"})
        first = self._report_raw("--plugin-load-sentinel", str(self.sentinel))
        second = self._report_raw("--plugin-load-sentinel", str(self.sentinel))
        self.assertEqual(first, second)

    def test_containment_boot_is_read_only_on_roots(self) -> None:
        write_sentinel(self.sentinel, "loading", "module:alpha")
        before = tree_digest(self.root)
        self._report("--plugin-load-sentinel", str(self.sentinel))
        self.assertEqual(tree_digest(self.root), before)

    def test_sentinel_arg_absent_keeps_existing_behavior(self) -> None:
        report = self._report()
        for module in report["modules"]:
            self.assertNotEqual(module.get("reasonDetail"), "crash-suspected")
        self.assertFalse(self.sentinel.exists())


class ContainmentStaticContractTest(unittest.TestCase):
    """The seeded crash/hang modules and their CI wiring exist and are honest."""

    def _repo_root(self) -> Path:
        return Path(__file__).resolve().parents[2]

    def test_cmake_declares_seeded_failure_module_targets(self) -> None:
        cmake = (self._repo_root() / "CMakeLists.txt").read_text()
        self.assertIn("streammate-test-crash", cmake)
        self.assertIn("streammate-test-hang", cmake)

    def test_seeded_module_sources_fail_as_documented(self) -> None:
        crash = (self._repo_root() / "src/obs_module/streammate_test_crash_module.cpp").read_text()
        hang = (self._repo_root() / "src/obs_module/streammate_test_hang_module.cpp").read_text()
        self.assertIn("abort()", crash)
        self.assertIn("obs_module_load", crash)
        self.assertIn("obs_module_load", hang)
        self.assertIn("sleep", hang)

    def test_ci_wires_containment_e2e(self) -> None:
        workflow = (self._repo_root() / ".github/workflows/macos-ci.yml").read_text()
        self.assertIn("streammate-test-crash", workflow)
        self.assertIn("streammate-test-hang", workflow)
        self.assertIn("test_plugin_crash_containment.py", workflow)


@unittest.skipUnless(
    __import__("os").environ.get("STREAMMATE_EXPECT_LIBOBS", "") == "1",
    "HAS_LIBOBS lane only (STREAMMATE_EXPECT_LIBOBS=1)",
)
class PluginCrashContainmentLibobsTest(unittest.TestCase):
    """Real seeded crash/hang containment against the packaged HAS_LIBOBS app.

    Env contract (set by the CI step, mirroring the loading e2e):
      STREAMMATE_TEST_SOURCE_PLUGIN — the built test-source .plugin bundle
      STREAMMATE_TEST_CRASH_PLUGIN  — obs_module_load calls abort()
      STREAMMATE_TEST_HANG_PLUGIN   — obs_module_load never returns
    HOST_BIN is the packaged studio-host executable in this lane.
    """

    def setUp(self) -> None:
        import os
        import shutil

        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.root = self.base / "user-plugins"
        self.root.mkdir()
        for env_key, name in (
            ("STREAMMATE_TEST_SOURCE_PLUGIN", "streammate-test-source"),
            ("STREAMMATE_TEST_CRASH_PLUGIN", "streammate-test-crash"),
            ("STREAMMATE_TEST_HANG_PLUGIN", "streammate-test-hang"),
        ):
            bundle = os.environ.get(env_key, "")
            self.assertTrue(bundle and Path(bundle).is_dir(), f"{env_key} must name a bundle dir")
            shutil.copytree(bundle, self.root / f"{name}.plugin", symlinks=False)
        self.manifest = self.base / "manifest.json"
        self.sentinel = self.base / "sentinel.json"

    SELECTED = [
        "module:streammate-test-source",
        "module:streammate-test-crash",
        "module:streammate-test-hang",
    ]

    def _write_manifest(self, retry=None) -> None:
        write_manifest(
            self.manifest,
            [{"binaryDir": str(self.root)}],
            list(self.SELECTED),
            [],
            retry=retry,
        )

    def _containment_args(self):
        return (
            "--user-plugins-manifest", str(self.manifest),
            "--plugin-load-sentinel", str(self.sentinel),
            "--plugin-load-deadline-ms", "3000",
        )

    def _dead_boot(self, timeout: float = 60.0) -> subprocess.CompletedProcess:
        """A boot expected to die during engine start (crash or watchdog)."""
        return subprocess.run(
            [str(HOST_BIN), "--token", "containment-test-token",
             "--host", "127.0.0.1", "--port", "0",
             *self._containment_args()],
            capture_output=True,
            text=True,
            timeout=timeout,
        )

    def test_seeded_crash_and_hang_are_bounded_attributed_and_isolated(self) -> None:
        roots_before = tree_digest(self.root)
        self._write_manifest()

        # Boot 1: the crash module aborts the process mid-load; the sentinel
        # names it (the loading phase survives the crash).
        first = self._dead_boot()
        self.assertNotEqual(first.returncode, 0)
        self.assertEqual(read_sentinel(self.sentinel)["moduleRef"], "module:streammate-test-crash")

        # Boot 2: crash module refused; the hang module trips the watchdog.
        second = self._dead_boot()
        self.assertEqual(second.returncode, WATCHDOG_EXIT, second.stdout + second.stderr)
        sentinel = read_sentinel(self.sentinel)
        self.assertEqual(sentinel["phase"], "deadline-exceeded")
        self.assertEqual(sentinel["moduleRef"], "module:streammate-test-hang")
        self.assertEqual(sentinel["suspects"].get("module:streammate-test-crash"), "crash")

        # Boot 3: both suspects refused; the UNRELATED module loads for real.
        process, port, _ = host.start_host(*self._containment_args())
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)
        report = json.loads(rpc_raw(sock, 31, "plugins.report", {}))["result"]
        by_ref = {m["moduleRef"]: m for m in report["modules"]}
        self.assertEqual(by_ref["module:streammate-test-crash"]["reasonDetail"], "crash-suspected")
        self.assertEqual(by_ref["module:streammate-test-crash"]["state"], "runtime_crash")
        self.assertEqual(by_ref["module:streammate-test-hang"]["reasonDetail"], "hang-suspected")
        source = by_ref["module:streammate-test-source"]
        self.assertEqual(source["lifecycle"], "loaded")
        self.assertIn("streammate_test_source", source["registeredTypes"]["sources"])
        sock.close()
        host.stop_process(process)

        # Boot 4: retrying the crash module gives it exactly one fresh attempt
        # (it crashes again and is re-suspected); bounded, never looping.
        self._write_manifest(retry=["module:streammate-test-crash"])
        fourth = self._dead_boot()
        self.assertNotEqual(fourth.returncode, 0)
        self.assertEqual(read_sentinel(self.sentinel)["moduleRef"], "module:streammate-test-crash")

        # Boot 5, SAME manifest (retry still listed): the spent retry must not
        # re-clear suspicion — the host boots healthy with the crash module
        # refused (the decisive no-crash-loop proof).
        process, port, _ = host.start_host(*self._containment_args())
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)
        report5 = json.loads(rpc_raw(sock, 51, "plugins.report", {}))["result"]
        by_ref5 = {m["moduleRef"]: m for m in report5["modules"]}
        self.assertEqual(by_ref5["module:streammate-test-crash"]["reasonDetail"], "crash-suspected")
        self.assertEqual(by_ref5["module:streammate-test-source"]["lifecycle"], "loaded")
        sock.close()
        host.stop_process(process)

        # The user plugin roots stayed byte-identical through every crash.
        self.assertEqual(tree_digest(self.root), roots_before)


if __name__ == "__main__":
    # test_host_lifecycle resolves HOST_BIN from sys.argv[1] at import time
    # (ctest passes $<TARGET_FILE:studio-host>); keep it out of unittest's argv.
    unittest.main(argv=[sys.argv[0]])
