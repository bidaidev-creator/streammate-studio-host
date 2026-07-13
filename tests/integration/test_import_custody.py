#!/usr/bin/env python3
"""NIF-M2: plugin settings/data/assets custody in the import workspace.

Spec 39 (mono docs/production-prototype/spec-39-native-import-completion.md,
"Settings, data, and assets custody"):

- copy-only import: source OBS config hashes unchanged by scan/load; source
  digests recorded before/after in the import map;
- N/N+1 rollback: importing a second collection leaves the first workspace
  byte-identical; deleting a workspace never touches OBS;
- explicit source->workspace resource map: relative assets referenced by
  source settings are mapped (status "copied"); absent assets produce
  actionable `missing-resource` repair records with a remediation hint;
  same-basename candidates elsewhere in the config produce "moved-candidate"
  records naming the candidate; absolute/escaping paths are never copied and
  surface as `external-resource` records;
- credential-class keys in copied scene-collection JSON are redacted in the
  WORKSPACE COPY ONLY (source untouched) and recorded by key name (never
  value) in the import map;
- plugin-private version markers (`plugin_settings_version`) are recorded in
  the map; a marker above the supported v1 degrades that source to
  `settings_migration_required` in the import report (seeded fixture
  convention -- the honest fixture-level signal, not a vendor semantic);
- the import map is byte-deterministic across repeated loads.

All of this is lane-independent filesystem/JSON behavior (no libobs needed),
so the scaffold lane proves it; the HAS_LIBOBS lane runs the same suite in CI.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import test_host_lifecycle as lifecycle

CRED_VALUE = "hunter2-credential-value-canary"
REDACTION_SENTINEL = "__streammate-redacted__"


def tree_digest(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        digest.update(str(path.relative_to(root)).encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


def collection_body() -> str:
    return json.dumps(
        {
            "name": "Custody Main",
            "sources": [
                {
                    "name": "Plugin Sparkle",
                    "id": "streammate_test_source",
                    "settings": {
                        "color": 4278255360,
                        "plugin_settings_version": 1,
                        "lut_file": "assets/luts/warm.cube",
                        "stream_key": CRED_VALUE,
                    },
                },
                {
                    "name": "Missing Mask",
                    "id": "image_source",
                    "settings": {"file": "assets/masks/gone.png"},
                },
                {
                    "name": "Moved Font",
                    "id": "text_ft2_source",
                    "settings": {"font_file": "assets/fonts/title.ttf"},
                },
                {
                    "name": "External Media",
                    "id": "ffmpeg_source",
                    "settings": {"local_file": "/Library/Movies/external.mov"},
                },
                {"name": "Plain Color", "id": "color_source", "settings": {"color": 255}},
            ],
        }
    )


def migration_collection_body() -> str:
    return json.dumps(
        {
            "name": "Custody Migration",
            "sources": [
                {
                    "name": "Future Plugin",
                    "id": "streammate_test_source",
                    "settings": {"plugin_settings_version": 2},
                }
            ],
        }
    )


def build_config(obs_dir: Path, name: str, body: str) -> None:
    scenes = obs_dir / "basic" / "scenes"
    scenes.mkdir(parents=True, exist_ok=True)
    (scenes / f"{name}.json").write_text(body, encoding="utf-8")
    profiles = obs_dir / "basic" / "profiles" / "Untitled"
    profiles.mkdir(parents=True, exist_ok=True)
    (profiles / "basic.ini").write_text("[General]\nName=Untitled\n", encoding="utf-8")
    luts = obs_dir / "assets" / "luts"
    luts.mkdir(parents=True, exist_ok=True)
    (luts / "warm.cube").write_bytes(b"LUT_3D_SIZE 2\n")
    fonts_moved = obs_dir / "assets" / "relocated"
    fonts_moved.mkdir(parents=True, exist_ok=True)
    (fonts_moved / "title.ttf").write_bytes(b"\x00\x01fontdata")
    # assets/masks/gone.png deliberately absent; assets/fonts/title.ttf absent
    # at its recorded path but present under assets/relocated/ (moved).


class ImportCustodyTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="nif-m2-custody-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.obs_dir = self.tmp / "obs-config"
        build_config(self.obs_dir, "custody-main", collection_body())
        self.home = self.tmp / "streammate-home"
        self.home.mkdir()
        env = {
            "STREAMMATE_OBS_CONFIG_DIR": str(self.obs_dir),
            "STREAMMATE_HOME": str(self.home),
        }
        self.process, port, _ = lifecycle.start_host(env=env)
        self.addCleanup(self.process.kill)
        self.sock = lifecycle.websocket_connect(port)

    _rpc_id = 6200

    def rpc(self, method: str, params: dict | None = None) -> dict:
        ImportCustodyTest._rpc_id += 1
        response = lifecycle.rpc(self.sock, ImportCustodyTest._rpc_id, method, params or {})
        self.assertIn("result", response, response)
        return response["result"]

    def load(self, collection_id: str = "custody-main") -> dict:
        result = self.rpc("import.load", {"collectionId": collection_id})
        self.assertTrue(result.get("ok"), result)
        return result

    def workspace(self, collection_id: str = "custody-main") -> Path:
        return self.home / "studio" / "obs-imports" / collection_id

    def import_map(self, collection_id: str = "custody-main") -> dict:
        return json.loads((self.workspace(collection_id) / "streammate-import-map.json").read_text())

    def test_source_hashes_unchanged_and_recorded(self) -> None:
        before = tree_digest(self.obs_dir)
        self.rpc("import.scan", {})
        self.load()
        self.assertEqual(tree_digest(self.obs_dir), before)
        recorded = self.import_map()["sourceDigest"]
        self.assertEqual(recorded["before"], before)
        self.assertEqual(recorded["after"], before)

    def test_workspace_deletion_leaves_obs_untouched(self) -> None:
        before = tree_digest(self.obs_dir)
        self.load()
        shutil.rmtree(self.workspace())
        self.assertEqual(tree_digest(self.obs_dir), before)

    def test_n_plus_1_import_leaves_n_byte_identical(self) -> None:
        build_config(self.obs_dir, "custody-second", migration_collection_body())
        self.load("custody-main")
        first = tree_digest(self.workspace("custody-main"))
        self.load("custody-second")
        self.assertEqual(tree_digest(self.workspace("custody-main")), first)

    def test_resource_map_copied_missing_moved_external(self) -> None:
        self.load()
        resources = {(r["sourceId"], r["settingsKey"]): r for r in self.import_map()["resources"]}
        copied = resources[("source:Plugin Sparkle", "lut_file")]
        self.assertEqual(copied["status"], "copied")
        self.assertEqual(copied["workspacePath"], "assets/luts/warm.cube")
        self.assertTrue((self.workspace() / "assets/luts/warm.cube").is_file())
        missing = resources[("source:Missing Mask", "file")]
        self.assertEqual(missing["status"], "missing-resource")
        self.assertTrue(missing["remediation"])
        moved = resources[("source:Moved Font", "font_file")]
        self.assertEqual(moved["status"], "moved-candidate")
        self.assertEqual(moved["candidatePath"], "assets/relocated/title.ttf")
        external = resources[("source:External Media", "local_file")]
        self.assertEqual(external["status"], "external-resource")
        self.assertNotIn("/Library/Movies", json.dumps(self.import_map()))

    def test_credential_class_keys_redacted_in_workspace_only(self) -> None:
        self.load()
        workspace_scene = (self.workspace() / "basic/scenes/custody-main.json").read_text()
        self.assertNotIn(CRED_VALUE, workspace_scene)
        self.assertIn(REDACTION_SENTINEL, workspace_scene)
        source_scene = (self.obs_dir / "basic/scenes/custody-main.json").read_text()
        self.assertIn(CRED_VALUE, source_scene)
        redacted = self.import_map()["redactedKeys"]
        self.assertIn({"sourceId": "source:Plugin Sparkle", "settingsKey": "stream_key"}, redacted)
        self.assertNotIn(CRED_VALUE, json.dumps(self.import_map()))

    def test_version_markers_recorded_and_migration_degrades(self) -> None:
        build_config(self.obs_dir, "custody-second", migration_collection_body())
        self.load("custody-main")
        markers = self.import_map()["versionMarkers"]
        self.assertIn(
            {"sourceId": "source:Plugin Sparkle", "settingsKey": "plugin_settings_version", "value": 1},
            markers,
        )
        result = self.load("custody-second")
        degraded = result["report"]["degraded"]
        migration = [item for item in degraded if item["reason"] == "settings_migration_required"]
        self.assertEqual(len(migration), 1, degraded)
        self.assertEqual(migration[0]["label"], "Future Plugin")
        self.assertTrue(any("plugin_settings_version" in note for note in migration[0].get("notes", [])))

    def test_second_collection_credentials_also_redacted(self) -> None:
        # A config dir can hold several scene collections; the workspace copy
        # of EVERY collection file must be credential-redacted, not just the
        # loaded one.
        second = json.dumps(
            {
                "name": "Custody Side",
                "sources": [
                    {
                        "name": "Side Plugin",
                        "id": "streammate_test_source",
                        "settings": {"password": CRED_VALUE},
                    }
                ],
            }
        )
        (self.obs_dir / "basic" / "scenes" / "custody-side.json").write_text(second, encoding="utf-8")
        self.load("custody-main")
        side_copy = (self.workspace("custody-main") / "basic/scenes/custody-side.json").read_text()
        self.assertNotIn(CRED_VALUE, side_copy)
        self.assertIn(REDACTION_SENTINEL, side_copy)
        # Source stays untouched.
        self.assertIn(CRED_VALUE, (self.obs_dir / "basic/scenes/custody-side.json").read_text())

    def test_versioned_upstream_ids_never_degrade_missing_plugin(self) -> None:
        # Renamed/versioned upstream module ids at the OBS 32.x pin (macOS
        # capture rename, versioned text/color sources) are upstream capability
        # surface, never third-party placeholders.
        body = json.dumps(
            {
                "name": "Upstream Versions",
                "sources": [
                    {"name": "New Cam", "id": "macos-avcapture", "settings": {}},
                    {"name": "Fast Cam", "id": "macos-avcapture-fast", "settings": {}},
                    {"name": "New Text", "id": "text_ft2_source_v2", "settings": {}},
                    {"name": "Color V3", "id": "color_source_v3", "settings": {}},
                ],
            }
        )
        build_config(self.obs_dir, "custody-upstream", body)
        result = self.load("custody-upstream")
        report = json.dumps(result["report"])
        self.assertNotIn("missing_plugin", report)

    def test_import_map_byte_deterministic(self) -> None:
        self.load()
        first = (self.workspace() / "streammate-import-map.json").read_bytes()
        self.load()
        second = (self.workspace() / "streammate-import-map.json").read_bytes()
        self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
