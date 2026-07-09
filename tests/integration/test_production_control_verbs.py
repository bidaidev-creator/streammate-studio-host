#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import socket
import struct
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


def send_raw_text(sock: socket.socket, raw: str) -> None:
    payload = raw.encode("utf-8")
    mask = os.urandom(4)
    if len(payload) < 126:
        header = bytes([0x81, 0x80 | len(payload)])
    elif len(payload) <= 0xFFFF:
        header = bytes([0x81, 0x80 | 126]) + struct.pack("!H", len(payload))
    else:
        raise AssertionError("test payload too large")
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    sock.sendall(header + mask + masked)


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
        # Take the token that actually carries the connection pair; lsof line
        # shapes vary (state suffix present/absent, LISTEN rows have no "->").
        token = next((part for part in line.split() if "->" in part), None)
        if token is None:
            continue
        peers.append(token.split("->", 1)[1])
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

    def _assert_rpc_error(self, sock: socket.socket, rpc_id: int, method: str, params: dict, code: int = -32602) -> dict:
        response = host.rpc(sock, rpc_id, method, params)
        self.assertNotIn("result", response, response)
        self.assertEqual(response["error"]["code"], code, response)
        return response

    def _assert_host_still_healthy(self, sock: socket.socket, rpc_id: int) -> None:
        health = host.rpc(sock, rpc_id, "host.health", {})["result"]
        self.assertEqual(health["status"], "ready")

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

    def test_scene_reload_then_scene_item_mutations_remain_safe(self) -> None:
        _, sock = self._connect()
        self._synthetic_scene(sock)

        reloaded = host.rpc(sock, 421, "scene.load", {"sceneId": SCENE_ID, "width": 64, "height": 36})["result"]
        self.assertEqual(reloaded["sceneId"], SCENE_ID)

        visible = host.rpc(sock, 422, "sceneItem.setVisible", {"sceneId": SCENE_ID, "itemId": SOURCE_ID, "visible": False})[
            "result"
        ]
        self.assertEqual(visible, {"ok": True, "sceneId": SCENE_ID, "itemId": SOURCE_ID, "visible": False})

        ordered = host.rpc(
            sock,
            423,
            "sceneItem.setOrder",
            {"sceneId": SCENE_ID, "itemId": SOURCE_ID, "position": 0, "idempotencyToken": "reload-order-1"},
        )["result"]
        self.assertEqual(ordered, {"ok": True, "sceneId": SCENE_ID, "itemId": SOURCE_ID, "position": 0})
        self._assert_host_still_healthy(sock, 424)

    def test_scene_item_set_order_requires_valid_idempotency_token(self) -> None:
        _, sock = self._connect()
        self._synthetic_scene(sock)

        missing = self._assert_rpc_error(sock, 425, "sceneItem.setOrder", {"sceneId": SCENE_ID, "itemId": SOURCE_ID, "position": 0})
        self.assertEqual(missing["error"]["message"], "idempotencyToken is required")
        invalid = self._assert_rpc_error(
            sock,
            426,
            "sceneItem.setOrder",
            {"sceneId": SCENE_ID, "itemId": SOURCE_ID, "position": 0, "idempotencyToken": "bad token"},
        )
        self.assertEqual(invalid["error"]["message"], "idempotencyToken is invalid")

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

    def test_filter_settings_refuse_out_of_range_numeric_value(self) -> None:
        _, sock = self._connect()
        self._synthetic_scene(sock)

        self._assert_rpc_error(
            sock,
            431,
            "filter.setSettings",
            {"sourceId": SOURCE_ID, "filterId": FILTER_ID, "settings": {"brightness": 1000000.1}},
        )

    def test_filter_settings_refuse_url_path_and_secret_shaped_strings(self) -> None:
        _, sock = self._connect()
        self._synthetic_scene(sock)

        refused_values = [
            "https://example.invalid/x",
            "rtmp://example.invalid/live",
            "//host/x",
            "file:///Users/x",
            "/Users/x/secret",
            "stm_agent_abc123",
        ]
        for offset, value in enumerate(refused_values, start=432):
            with self.subTest(value=value):
                self._assert_rpc_error(
                    sock,
                    offset,
                    "filter.setSettings",
                    {"sourceId": SOURCE_ID, "filterId": FILTER_ID, "settings": {"key_color_type": value}},
                )

    def test_malformed_new_verb_requests_fail_closed_and_keep_host_alive(self) -> None:
        _, sock = self._connect()
        self._synthetic_scene(sock)

        cases = [
            ("sceneItem.setVisible", {"sceneId": "missing-scene", "itemId": SOURCE_ID, "visible": True}),
            ("sceneItem.setVisible", {"sceneId": SCENE_ID, "itemId": "missing-item", "visible": True}),
            ("filter.list", {"sourceId": "missing-source"}),
            ("filter.setEnabled", {"sourceId": SOURCE_ID, "filterId": "missing-filter", "enabled": True}),
            ("sceneItem.setVisible", {"sceneId": SCENE_ID, "itemId": SOURCE_ID, "visible": "false"}),
            (
                "sceneItem.setOrder",
                {"sceneId": SCENE_ID, "itemId": SOURCE_ID, "position": -1, "idempotencyToken": "bad-position-1"},
            ),
            (
                "sceneItem.setOrder",
                {"sceneId": SCENE_ID, "itemId": SOURCE_ID, "position": 1000001, "idempotencyToken": "bad-position-2"},
            ),
            ("audio.setVolume", {"sourceId": SOURCE_ID, "volumeDb": -101}),
            ("audio.setVolume", {"sourceId": SOURCE_ID, "volumeDb": "loud"}),
            ("media.control", {"sourceId": SOURCE_ID, "action": "scrub"}),
        ]
        for offset, (method, params) in enumerate(cases, start=460):
            with self.subTest(method=method, params=params):
                self._assert_rpc_error(sock, offset, method, params)
                self._assert_host_still_healthy(sock, offset + 100)

    def test_filter_settings_malformed_json_fails_with_timeout_bound(self) -> None:
        _, sock = self._connect()
        self._synthetic_scene(sock)

        raw_cases = [
            (
                580,
                f'{{"jsonrpc":"2.0","id":580,"method":"filter.setSettings","params":{{"sourceId":"{SOURCE_ID}",'
                f'"filterId":"{FILTER_ID}","settings":{{"key_color_type":"green}}}}}}',
            ),
            (
                581,
                f'{{"jsonrpc":"2.0","id":581,"method":"filter.setSettings","params":{{"sourceId":"{SOURCE_ID}",'
                f'"filterId":"{FILTER_ID}","settings":{{"brightness" 0.25}}}}}}',
            ),
            (
                582,
                f'{{"jsonrpc":"2.0","id":582,"method":"filter.setSettings","params":{{"sourceId":"{SOURCE_ID}",'
                f'"filterId":"{FILTER_ID}","settings":{{"key_color_type":"green\\q"}}}}}}',
            ),
            (
                583,
                f'{{"jsonrpc":"2.0","id":583,"method":"filter.setSettings","params":{{"sourceId":"{SOURCE_ID}",'
                f'"filterId":"{FILTER_ID}","settings":{{,"brightness":1}}}}}}',
            ),
            (
                584,
                f'{{"jsonrpc":"2.0","id":584,"method":"filter.setSettings","params":{{"sourceId":"{SOURCE_ID}",'
                f'"filterId":"{FILTER_ID}","settings":{{"brightness":1,,"contrast":2}}}}}}',
            ),
        ]
        for rpc_id, raw in raw_cases:
            with self.subTest(rpc_id=rpc_id):
                send_raw_text(sock, raw)
                response = host.recv_text(sock, timeout=1.0)
                while response.get("id") != rpc_id:
                    response = host.recv_text(sock, timeout=1.0)
                self.assertEqual(response["error"]["code"], -32602, response)
                self._assert_host_still_healthy(sock, rpc_id + 100)

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
            refused_secret = host.rpc(
                sock,
                453,
                "filter.setSettings",
                {"sourceId": SOURCE_ID, "filterId": FILTER_ID, "settings": {"key_color_type": SECRET_SHAPED}},
            )
            self.assertEqual(refused_secret["error"]["code"], -32602)
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
        # The HAS_LIBOBS lane must run this suite against the packaged app (the
        # raw build binary cannot dlopen the bundled libobs graphics module).
        self.assertIn("tests/integration/test_production_control_verbs.py", workflow)
        self.assertIn("dist/StreamMateStudioHost.app/Contents/MacOS/studio-host", workflow)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
