#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

import test_host_lifecycle as host

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "macos-ci.yml"
SOURCE_ID = "station-overlay"
SCENE_ID = "control-scene"
FILTER_ID = "color-correction"
SECRET_SHAPED = "stm_studio-host_AbCdEfGhIjKlMnOpQrStUvWxYz012345"


def tree_files(root: Path) -> dict[str, bytes]:
    if not root.exists():
        return {}
    return {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def host_tcp_peers(pid: int) -> list[str]:
    result = subprocess.run(
        ["lsof", "-nP", "-a", "-p", str(pid), "-iTCP"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    peers: list[str] = []
    for line in result.stdout.splitlines():
        if "->" not in line:
            continue
        name = line.split()[-2] if line.split()[-1] == "(ESTABLISHED)" else line.split()[-1]
        peers.append(name.split("->", 1)[1])
    return peers


def is_loopback_peer(peer: str) -> bool:
    host_part = peer.rsplit(":", 1)[0].strip("[]")
    return host_part in ("127.0.0.1", "::1", "localhost")


class ProductionControlVerbTest(unittest.TestCase):
    def _connect(self, home: Path | None = None) -> tuple[subprocess.Popen[str], socket.socket]:
        env = {"STREAMMATE_HOME": str(home)} if home is not None else None
        process, port, _ = host.start_host(env=env)
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)
        return process, sock

    def _synthetic_scene(self, sock: socket.socket) -> None:
        loaded = host.rpc(sock, 400, "scene.load", {"sceneId": SCENE_ID, "width": 64, "height": 36})["result"]
        self.assertEqual(loaded["sceneId"], SCENE_ID)
        created = host.rpc(
            sock,
            401,
            "source.create",
            {
                "sceneId": SCENE_ID,
                "sourceId": SOURCE_ID,
                "kind": "browser",
                "url": "https://station.localhost/overlay/control-surface",
                "width": 64,
                "height": 36,
            },
        )["result"]
        self.assertEqual(created["sourceId"], SOURCE_ID)

    def test_scaffold_contract_shape_parity_for_each_new_verb(self) -> None:
        _, sock = self._connect()
        self._synthetic_scene(sock)

        visible = host.rpc(sock, 410, "sceneItem.setVisible", {"sceneId": SCENE_ID, "itemId": SOURCE_ID, "visible": False})["result"]
        self.assertEqual(visible, {"ok": True, "sceneId": SCENE_ID, "itemId": SOURCE_ID, "visible": False})

        ordered = host.rpc(
            sock,
            411,
            "sceneItem.setOrder",
            {"sceneId": SCENE_ID, "itemId": SOURCE_ID, "position": 0, "idempotencyToken": "order-1"},
        )["result"]
        self.assertEqual(ordered, {"ok": True, "sceneId": SCENE_ID, "itemId": SOURCE_ID, "position": 0})

        listed = host.rpc(sock, 412, "filter.list", {"sourceId": SOURCE_ID})["result"]
        self.assertEqual(
            listed,
            {
                "sourceId": SOURCE_ID,
                "filters": [
                    {"filterId": FILTER_ID, "filterKind": "color_filter_v2", "label": "Color Correction", "enabled": True}
                ],
            },
        )

        disabled = host.rpc(sock, 413, "filter.setEnabled", {"sourceId": SOURCE_ID, "filterId": FILTER_ID, "enabled": False})[
            "result"
        ]
        self.assertEqual(disabled, {"ok": True, "sourceId": SOURCE_ID, "filterId": FILTER_ID, "enabled": False})

        settings = {"brightness": 0.125, "relative": False, "key_color_type": "green"}
        applied = host.rpc(
            sock,
            414,
            "filter.setSettings",
            {"sourceId": SOURCE_ID, "filterId": FILTER_ID, "settings": settings, "idempotencyToken": "filter-1"},
        )["result"]
        self.assertEqual(applied, {"ok": True, "sourceId": SOURCE_ID, "filterId": FILTER_ID, "settings": settings})

        volume = host.rpc(sock, 415, "audio.setVolume", {"sourceId": SOURCE_ID, "volumeDb": -12.5})["result"]
        self.assertEqual(volume, {"ok": True, "sourceId": SOURCE_ID, "volumeDb": -12.5})

        for offset, action in enumerate(["play", "pause", "restart", "stop"], start=416):
            controlled = host.rpc(sock, offset, "media.control", {"sourceId": SOURCE_ID, "action": action})["result"]
            self.assertEqual(controlled, {"ok": True, "sourceId": SOURCE_ID, "action": action})

        refreshed = host.rpc(sock, 420, "source.refreshBrowser", {"sourceId": SOURCE_ID})["result"]
        self.assertEqual(refreshed, {"ok": True, "sourceId": SOURCE_ID, "refreshed": True})

    def test_filter_settings_unknown_key_is_refused_fail_closed(self) -> None:
        _, sock = self._connect()
        self._synthetic_scene(sock)

        refused = host.rpc(
            sock,
            430,
            "filter.setSettings",
            {"sourceId": SOURCE_ID, "filterId": FILTER_ID, "settings": {"contrast": 0.1, "plugin_binary_path": "/tmp/x"}},
        )
        self.assertEqual(refused["error"]["code"], -32602)
        self.assertIn("unknown filter-settings key", refused["error"]["message"])

    def test_loopback_binding_and_no_live_egress_remain_unchanged(self) -> None:
        process, sock = self._connect()
        self._synthetic_scene(sock)

        host.rpc(sock, 440, "sceneItem.setVisible", {"sceneId": SCENE_ID, "itemId": SOURCE_ID, "visible": True})
        host.rpc(sock, 441, "filter.setEnabled", {"sourceId": SOURCE_ID, "filterId": FILTER_ID, "enabled": True})
        host.rpc(sock, 442, "media.control", {"sourceId": SOURCE_ID, "action": "restart"})

        deadline = time.time() + 2
        peers: list[str] = []
        while time.time() < deadline:
            peers = host_tcp_peers(process.pid)
            if peers:
                break
            time.sleep(0.05)
        self.assertTrue(all(is_loopback_peer(peer) for peer in peers), peers)

    def test_new_verbs_write_no_journal_and_persist_no_secret_shaped_material(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            home = Path(temp_dir) / "streammate-home"
            home.mkdir()
            _, sock = self._connect(home)
            self._synthetic_scene(sock)
            before = tree_files(home)

            host.rpc(sock, 450, "sceneItem.setVisible", {"sceneId": SCENE_ID, "itemId": SOURCE_ID, "visible": False})
            host.rpc(
                sock,
                451,
                "sceneItem.setOrder",
                {"sceneId": SCENE_ID, "itemId": SOURCE_ID, "position": 0, "idempotencyToken": "disk-1"},
            )
            host.rpc(sock, 452, "filter.setEnabled", {"sourceId": SOURCE_ID, "filterId": FILTER_ID, "enabled": False})
            host.rpc(
                sock,
                453,
                "filter.setSettings",
                {"sourceId": SOURCE_ID, "filterId": FILTER_ID, "settings": {"key_color_type": SECRET_SHAPED}},
            )
            host.rpc(sock, 454, "audio.setVolume", {"sourceId": SOURCE_ID, "volumeDb": -6})
            host.rpc(sock, 455, "media.control", {"sourceId": SOURCE_ID, "action": "stop"})
            host.rpc(sock, 456, "source.refreshBrowser", {"sourceId": SOURCE_ID})

            after = tree_files(home)
            self.assertEqual(after, before)
            self.assertFalse(any("journal" in path.lower() for path in after))
            serialized = b"\n".join(after.values())
            self.assertNotIn(SECRET_SHAPED.encode("utf-8"), serialized)


class ProductionControlCiWorkflowTest(unittest.TestCase):
    def test_has_libobs_lane_runs_new_control_verb_smoke(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("production-control-verbs", workflow)
        self.assertRegex(workflow, r"ctest --test-dir build/host .*production-control-verbs")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
