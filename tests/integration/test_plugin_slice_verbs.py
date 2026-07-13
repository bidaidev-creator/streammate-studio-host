#!/usr/bin/env python3
"""NIF-V1 (host): vertical-slice verbs over loaded user plugins.

Three additive host capabilities make the source+filter vertical slice
drivable from the Station harness against a packaged HAS_LIBOBS host:

  1. `source.create` accepts a user-plugin source `kind` (a source type
     registered by a manifest-loaded user module) with a fail-closed flat
     `settings` object, and never substitutes a placeholder: if the plugin
     instance cannot be created the RPC errors. The browser default path is
     byte-for-byte untouched (ADR-0003 stands). An optional
     `pluginFilterKind`/`filterId` pair attaches a user-plugin filter to the
     created source.
  2. `source.captureFrame` returns the source's real async video frame facts
     (dimensions, format, sha256 of the frame bytes) under libobs; in the
     scaffold lane it refuses honestly (no fabricated pixels are ever
     presented as a captured frame).
  3. `import.report` classifies a collection entry whose type is registered
     by a LOADED user plugin as mapped/`mapped_plugin` instead of a
     placeholder. In the scaffold lane nothing is loaded, so the classifier
     never fires (scaffold honesty).

The scaffold lane proves the refusal/labeling contracts; the packaged
HAS_LIBOBS CI e2e (test_user_plugin_loading.py) proves the real
instantiation, capture, and mapped_plugin legs.
"""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

import test_host_lifecycle as lifecycle

HOST_BIN = lifecycle.HOST_BIN

TEST_SOURCE_KIND = "streammate_test_source"
TEST_FILTER_KIND = "streammate_test_filter"


def start_connected_host(env: dict | None = None):
    process, port, _ = lifecycle.start_host(env=env)
    sock = lifecycle.websocket_connect(port)
    return process, sock


class PluginSliceHelloTest(unittest.TestCase):
    def test_hello_advertises_source_capture_frame(self) -> None:
        process, sock = start_connected_host()
        self.addCleanup(lifecycle.stop_process, process)
        self.addCleanup(sock.close)
        hello = lifecycle.rpc(sock, 9101, "host.hello")
        commands = hello["result"]["supportedCommands"]
        self.assertIn("source.captureFrame", commands)
        self.assertIn("source.create", commands)


class PluginSliceScaffoldRefusalTest(unittest.TestCase):
    """The scaffold lane must refuse plugin-kind work honestly, never fake it."""

    def setUp(self) -> None:
        self.process, self.sock = start_connected_host()
        self.addCleanup(lifecycle.stop_process, self.process)
        self.addCleanup(self.sock.close)
        loaded = lifecycle.rpc(self.sock, 9200, "scene.load", {"sceneId": "slice-scene", "width": 128, "height": 72})
        self.assertTrue(loaded["result"]["ok"])

    def test_plugin_kind_create_refused_in_scaffold(self) -> None:
        response = lifecycle.rpc(
            self.sock,
            9201,
            "source.create",
            {
                "sceneId": "slice-scene",
                "sourceId": "slice-plugin-source",
                "kind": TEST_SOURCE_KIND,
                "settings": {"color": 4278255360},
            },
        )
        self.assertIn("error", response)
        message = response["error"]["message"]
        self.assertIn("plugin source kinds require libobs", message)
        # The refusal must not leak the requested kind back unvalidated
        # beyond naming it as unavailable, and must never create the source.
        listed = lifecycle.rpc(self.sock, 9202, "scene.list")
        source_ids = [s["sourceId"] for s in listed["result"].get("sources", [])]
        self.assertNotIn("slice-plugin-source", source_ids)

    def test_browser_default_path_is_untouched(self) -> None:
        response = lifecycle.rpc(
            self.sock,
            9203,
            "source.create",
            {"sceneId": "slice-scene", "sourceId": "slice-browser", "url": "https://overlay.invalid/x"},
        )
        self.assertTrue(response["result"]["ok"])
        self.assertEqual(response["result"]["kind"], "browser")

    def test_unknown_kind_still_refused(self) -> None:
        response = lifecycle.rpc(
            self.sock,
            9204,
            "source.create",
            {"sceneId": "slice-scene", "sourceId": "slice-unknown", "kind": "totally_unknown_kind"},
        )
        self.assertIn("error", response)

    def test_source_capture_frame_refused_in_scaffold(self) -> None:
        created = lifecycle.rpc(
            self.sock,
            9205,
            "source.create",
            {"sceneId": "slice-scene", "sourceId": "slice-cap", "url": "https://overlay.invalid/y"},
        )
        self.assertTrue(created["result"]["ok"])
        response = lifecycle.rpc(self.sock, 9206, "source.captureFrame", {"sourceId": "slice-cap"})
        self.assertIn("error", response)
        self.assertIn("source frame capture requires libobs", response["error"]["message"])

    def test_plugin_settings_are_fail_closed(self) -> None:
        response = lifecycle.rpc(
            self.sock,
            9207,
            "source.create",
            {
                "sceneId": "slice-scene",
                "sourceId": "slice-bad-settings",
                "kind": TEST_SOURCE_KIND,
                "settings": {"totally_unknown_setting": "x"},
            },
        )
        # Refused for the unknown settings key (fail-closed) OR for the
        # scaffold lane (no loaded plugin kinds) -- both are refusals; the
        # settings gate must run first so a bad key is named without
        # depending on the lane.
        self.assertIn("error", response)
        self.assertIn("unknown plugin-settings key", response["error"]["message"])


class PluginSliceScaffoldImportHonestyTest(unittest.TestCase):
    """Scaffold lane: no loaded plugins => no mapped_plugin classification."""

    def test_import_report_never_maps_plugin_types_in_scaffold(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            obs_dir = lifecycle.write_obs_fixture(temp_root)
            scenes = json.loads((obs_dir / "basic" / "scenes" / "fixture-main.json").read_text(encoding="utf-8"))
            scenes["sources"].append({"name": "Slice Plugin Source", "id": TEST_SOURCE_KIND, "settings": {"color": 4278255360}})
            scenes["sources"].append({"name": "Slice Plugin Filter", "id": TEST_FILTER_KIND})
            (obs_dir / "basic" / "scenes" / "fixture-main.json").write_text(
                json.dumps(scenes, indent=2) + "\n", encoding="utf-8"
            )
            streammate_home = temp_root / "streammate-home"
            process, sock = start_connected_host(
                env={
                    "STREAMMATE_OBS_CONFIG_DIR": str(obs_dir),
                    "STREAMMATE_HOME": str(streammate_home),
                }
            )
            self.addCleanup(lifecycle.stop_process, process)
            self.addCleanup(sock.close)

            scan = lifecycle.rpc(sock, 9301, "import.scan", {"configDir": str(obs_dir)})
            collection_id = scan["result"]["collections"][0]["collectionId"]
            loaded = lifecycle.rpc(sock, 9302, "import.load", {"collectionId": collection_id})
            report = loaded["result"]["report"]
            mapped_modules = [item.get("moduleName", "") for item in report["mapped"]]
            self.assertNotIn(TEST_SOURCE_KIND, mapped_modules)
            self.assertNotIn(TEST_FILTER_KIND, mapped_modules)
            for bucket in ("mapped", "degraded", "unresolved"):
                for item in report[bucket]:
                    self.assertNotEqual(item.get("reason"), "mapped_plugin")


if __name__ == "__main__":
    # test_host_lifecycle resolves HOST_BIN from sys.argv[1] at import time
    # (ctest passes $<TARGET_FILE:studio-host>); keep it out of unittest's argv.
    unittest.main(argv=[sys.argv[0]], verbosity=2)
