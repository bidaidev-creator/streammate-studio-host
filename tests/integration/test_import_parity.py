#!/usr/bin/env python3
"""NIF-M3: remaining OBS import parity slices — exhaustive report accounting.

Spec 39 (mono docs/production-prototype/spec-39-native-import-completion.md,
NIF-M3 workpack): close the recorded Q-103 iteration points and the observed
classification gaps so that no scene-collection entry silently drops from the
ImportReport, every unsupported item carries a precise Spec 39 reason, and
filters/transitions/scripts are first-class accounted items.

Covered here (all lane-independent classification behavior; the HAS_LIBOBS CI
lane runs the same suite):

- upstream FILTER ids on a mapped source classify mapped/mapped_native with
  kind "filter" (the native host operates filter chains via its filter verbs);
- non-upstream, non-loaded plugin filters degrade missing_plugin (kind filter)
  — never the generic unsupported_frontend_feature (NIF-M2 codex F5);
- `group` and `coreaudio_output_capture` are accounted mapped_native (they
  previously emitted nothing — the recorded Q-103 drift points);
- display/window capture defer screen permission (permission_required/screen);
- device-backed upstream ids (syphon-input, decklink-input, audio_line)
  degrade missing_device;
- other-platform upstream ids (wasapi/pulse/v4l2/...) degrade missing_device
  with a platform note;
- vlc_source degrades dependency_missing (VLC libraries are not bundled);
- transitions are parsed and accounted: upstream transition ids map
  mapped_native (kind transition), unknown plugin transitions degrade
  missing_plugin (kind transition);
- Lua/Python scripts are inventoried unresolved/unsupported_plugin_class
  (Q-137 inventory-and-defer);
- import.report stays byte-deterministic.
"""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import test_host_lifecycle as lifecycle


def parity_collection_body() -> str:
    return json.dumps(
        {
            "name": "Parity Main",
            "sources": [
                {"name": "Main Stage", "id": "scene"},
                {
                    "name": "Overlay",
                    "id": "browser_source",
                    "filters": [
                        {"name": "Warm LUT", "id": "color_filter_v2"},
                        {"name": "Shader Glow", "id": "shader_filter"},
                    ],
                },
                {"name": "Lower Thirds", "id": "group"},
                {"name": "Desktop Audio", "id": "coreaudio_output_capture"},
                {"name": "Display Mirror", "id": "display_capture"},
                {"name": "App Window", "id": "window_capture"},
                {"name": "Syphon Feed", "id": "syphon-input"},
                {"name": "Deck Link", "id": "decklink-input"},
                {"name": "Aux Line", "id": "audio_line"},
                {"name": "Windows Mic", "id": "wasapi_input_capture"},
                {"name": "VLC Playlist", "id": "vlc_source"},
                {"name": "Vacation Slides", "id": "slideshow"},
            ],
            "transitions": [
                {"name": "Fade", "id": "fade_transition"},
                {"name": "Move Magic", "id": "move_transition"},
            ],
            "modules": {
                "scripts-tool": [
                    {"path": "scripts/confetti.lua", "settings": {}},
                    {"path": "scripts/chat_poll.py", "settings": {}},
                ]
            },
        },
        indent=2,
    )


def build_config(obs_dir: Path, name: str, body: str) -> None:
    scenes = obs_dir / "basic" / "scenes"
    scenes.mkdir(parents=True, exist_ok=True)
    (scenes / f"{name}.json").write_text(body, encoding="utf-8")
    profiles = obs_dir / "basic" / "profiles" / "Untitled"
    profiles.mkdir(parents=True, exist_ok=True)
    (profiles / "basic.ini").write_text("[General]\nName=Untitled\n", encoding="utf-8")


class ImportParityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="nif-m3-parity-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.obs_dir = self.tmp / "obs-config"
        build_config(self.obs_dir, "parity-main", parity_collection_body())
        self.home = self.tmp / "streammate-home"
        self.home.mkdir()
        env = {
            "STREAMMATE_OBS_CONFIG_DIR": str(self.obs_dir),
            "STREAMMATE_HOME": str(self.home),
        }
        self.process, port, _ = lifecycle.start_host(env=env)
        self.addCleanup(self.process.kill)
        self.sock = lifecycle.websocket_connect(port)

    _rpc_id = 7300

    def rpc(self, method: str, params: dict | None = None) -> dict:
        ImportParityTest._rpc_id += 1
        response = lifecycle.rpc(self.sock, ImportParityTest._rpc_id, method, params or {})
        self.assertIn("result", response, response)
        return response["result"]

    def report(self) -> dict:
        result = self.rpc("import.load", {"collectionId": "parity-main"})
        self.assertTrue(result.get("ok"), result)
        return result["report"]

    @staticmethod
    def by_id(report: dict) -> dict:
        items = {}
        for bucket in ("mapped", "degraded", "unresolved"):
            for item in report[bucket]:
                items[item["id"]] = dict(item, bucket=bucket)
        return items

    def test_zero_silent_drops_every_entry_accounted(self) -> None:
        items = self.by_id(self.report())
        expected_ids = {
            "scene:Main Stage",
            "source:Overlay",
            "filter:Overlay:Warm LUT",
            "filter:Overlay:Shader Glow",
            "source:Lower Thirds",
            "source:Desktop Audio",
            "source:Display Mirror",
            "source:App Window",
            "source:Syphon Feed",
            "source:Deck Link",
            "source:Aux Line",
            "source:Windows Mic",
            "source:VLC Playlist",
            "source:Vacation Slides",
            "transition:Fade",
            "transition:Move Magic",
            "script:confetti.lua",
            "script:chat_poll.py",
        }
        self.assertEqual(set(items.keys()) - {"profile:Untitled:encoder"}, expected_ids)

    def test_upstream_filter_maps_native_with_filter_kind(self) -> None:
        item = self.by_id(self.report())["filter:Overlay:Warm LUT"]
        self.assertEqual(item["bucket"], "mapped")
        self.assertEqual(item["kind"], "filter")
        self.assertEqual(item["reason"], "mapped_native")
        self.assertEqual(item["moduleName"], "color_filter_v2")
        self.assertEqual(item["label"], "Overlay / Warm LUT")

    def test_plugin_filter_degrades_missing_plugin_not_frontend(self) -> None:
        item = self.by_id(self.report())["filter:Overlay:Shader Glow"]
        self.assertEqual(item["bucket"], "degraded")
        self.assertEqual(item["kind"], "filter")
        self.assertEqual(item["reason"], "missing_plugin")
        self.assertEqual(item["moduleName"], "shader_filter")
        self.assertIn("not loaded by the native host", " ".join(item.get("notes", [])))

    def test_group_and_output_capture_are_mapped(self) -> None:
        items = self.by_id(self.report())
        group = items["source:Lower Thirds"]
        self.assertEqual((group["bucket"], group["reason"]), ("mapped", "mapped_native"))
        self.assertNotIn("moduleName", group)
        output = items["source:Desktop Audio"]
        self.assertEqual((output["bucket"], output["reason"]), ("mapped", "mapped_native"))
        self.assertEqual(output["moduleName"], "coreaudio_output_capture")

    def test_display_and_window_capture_defer_screen_permission(self) -> None:
        items = self.by_id(self.report())
        for item_id in ("source:Display Mirror", "source:App Window"):
            item = items[item_id]
            self.assertEqual((item["bucket"], item["reason"]), ("degraded", "permission_required"), item)
            self.assertEqual(item["tccClass"], "screen")

    def test_device_backed_ids_degrade_missing_device(self) -> None:
        items = self.by_id(self.report())
        for item_id in ("source:Syphon Feed", "source:Deck Link", "source:Aux Line"):
            item = items[item_id]
            self.assertEqual((item["bucket"], item["reason"]), ("degraded", "missing_device"), item)

    def test_other_platform_ids_degrade_missing_device_with_note(self) -> None:
        item = self.by_id(self.report())["source:Windows Mic"]
        self.assertEqual((item["bucket"], item["reason"]), ("degraded", "missing_device"))
        self.assertIn("another platform", " ".join(item.get("notes", [])))

    def test_vlc_source_degrades_dependency_missing(self) -> None:
        item = self.by_id(self.report())["source:VLC Playlist"]
        self.assertEqual((item["bucket"], item["reason"]), ("degraded", "dependency_missing"))
        self.assertIn("VLC", " ".join(item.get("notes", [])))

    def test_slideshow_maps_native(self) -> None:
        item = self.by_id(self.report())["source:Vacation Slides"]
        self.assertEqual((item["bucket"], item["reason"]), ("mapped", "mapped_native"))

    def test_transitions_are_accounted(self) -> None:
        items = self.by_id(self.report())
        fade = items["transition:Fade"]
        self.assertEqual((fade["bucket"], fade["kind"], fade["reason"]), ("mapped", "transition", "mapped_native"))
        self.assertEqual(fade["moduleName"], "fade_transition")
        move = items["transition:Move Magic"]
        self.assertEqual((move["bucket"], move["kind"], move["reason"]), ("degraded", "transition", "missing_plugin"))
        self.assertEqual(move["moduleName"], "move_transition")

    def test_scripts_inventoried_unsupported_plugin_class(self) -> None:
        items = self.by_id(self.report())
        for item_id in ("script:confetti.lua", "script:chat_poll.py"):
            item = items[item_id]
            self.assertEqual(item["bucket"], "unresolved")
            self.assertEqual(item["reason"], "unsupported_plugin_class")
            self.assertIn("Q-137", " ".join(item.get("notes", [])))

    def test_report_byte_deterministic(self) -> None:
        first = self.rpc("import.report", {"collectionId": "parity-main"})
        second = self.rpc("import.report", {"collectionId": "parity-main"})
        self.assertEqual(json.dumps(first, sort_keys=True), json.dumps(second, sort_keys=True))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
