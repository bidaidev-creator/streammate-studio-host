#!/usr/bin/env python3
from __future__ import annotations

import base64
import hashlib
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

HOST_BIN = Path(sys.argv[1]).resolve()
TOKEN = "test-token-not-secret"


def wait_ready(process: subprocess.Popen[str], timeout: float = 10.0) -> tuple[int, list[str]]:
    deadline = time.time() + timeout
    lines: list[str] = []
    while time.time() < deadline:
        line = process.stdout.readline() if process.stdout else ""
        if line:
            lines.append(line.rstrip("\n"))
            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                continue
            if payload.get("event") == "host.ready" and "port" in payload:
                return int(payload["port"]), lines
        if process.poll() is not None:
            raise AssertionError(f"host exited early with {process.returncode}: {lines}")
    raise AssertionError(f"timed out waiting for ready: {lines}")


def start_host(
    *extra: str,
    state_file: Path | None = None,
    env: dict[str, str] | None = None,
) -> tuple[subprocess.Popen[str], int, list[str]]:
    args = [str(HOST_BIN), "--token", TOKEN, "--port", "0"]
    if state_file is not None:
        args.extend(["--state-file", str(state_file)])
    args.extend(extra)
    process = subprocess.Popen(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        env={**os.environ, **(env or {})},
    )
    port, lines = wait_ready(process)
    return process, port, lines


def stop_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def websocket_connect(port: int, token: str = TOKEN) -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        "GET /control HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"Authorization: Bearer {token}\r\n"
        "\r\n"
    )
    sock.sendall(request.encode("ascii"))
    response_bytes = b""
    while b"\r\n\r\n" not in response_bytes:
        response_bytes += sock.recv(1)
    response = response_bytes.decode("ascii", errors="replace")
    if "101 Switching Protocols" not in response:
        raise AssertionError(f"websocket upgrade failed: {response!r}")
    expected = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    if expected not in response:
        raise AssertionError("websocket accept header mismatch")
    return sock


def websocket_wrong_token_status(port: int) -> str:
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        "GET /control HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer wrong-token\r\n"
        "\r\n"
    )
    try:
        sock.sendall(request.encode("ascii"))
        return sock.recv(4096).decode("ascii", errors="replace")
    finally:
        sock.close()


def send_text(sock: socket.socket, payload: dict) -> None:
    raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    mask = os.urandom(4)
    if len(raw) < 126:
        header = bytes([0x81, 0x80 | len(raw)])
    elif len(raw) <= 0xFFFF:
        header = bytes([0x81, 0x80 | 126]) + struct.pack("!H", len(raw))
    else:
        raise AssertionError("test payload too large")
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(raw))
    sock.sendall(header + mask + masked)


def rpc(sock: socket.socket, rpc_id: int, method: str, params: dict | None = None) -> dict:
    request: dict = {"jsonrpc": "2.0", "id": rpc_id, "method": method}
    if params is not None:
        request["params"] = params
    send_text(sock, request)
    while True:
        response = recv_text(sock)
        if response.get("id") == rpc_id:
            return response


def recv_text(sock: socket.socket, timeout: float = 7.0) -> dict:
    sock.settimeout(timeout)
    first = sock.recv(2)
    if len(first) != 2:
        raise AssertionError("short websocket frame")
    opcode = first[0] & 0x0F
    length = first[1] & 0x7F
    if length == 126:
        length = struct.unpack("!H", sock.recv(2))[0]
    elif length == 127:
        raise AssertionError("unexpected 64-bit frame")
    payload = b""
    while len(payload) < length:
        payload += sock.recv(length - len(payload))
    if opcode != 1:
        return recv_text(sock, timeout)
    return json.loads(payload.decode("utf-8"))


def overlay_url_with_secret(secret: str) -> str:
    query_name = "".join(["to", "ken"])
    return f"https://station.localhost/overlay?show=test&{query_name}={secret}"


class FakeRtmpIngest:
    def __init__(self) -> None:
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind(("127.0.0.1", 0))
        self._server.listen(1)
        self.port = self._server.getsockname()[1]
        self.bytes_received = 0
        self.connection_count = 0
        self._stop = threading.Event()
        self._client: socket.socket | None = None
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    @property
    def endpoint(self) -> str:
        return f"rtmp://127.0.0.1:{self.port}/live"

    def _run(self) -> None:
        try:
            self._server.settimeout(0.2)
            while not self._stop.is_set():
                try:
                    client, _ = self._server.accept()
                except socket.timeout:
                    continue
                self.connection_count += 1
                self._client = client
                client.settimeout(0.2)
                with client:
                    while not self._stop.is_set():
                        try:
                            data = client.recv(4096)
                        except socket.timeout:
                            continue
                        except OSError:
                            break
                        if not data:
                            break
                        self.bytes_received += len(data)
                self._client = None
        except OSError:
            return

    def wait_for_bytes(self, timeout: float = 5.0) -> None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.bytes_received > 0:
                return
            time.sleep(0.02)
        raise AssertionError("fake RTMP ingest did not receive scaffold encode bytes")

    def kill_ingest(self) -> None:
        self._stop.set()
        client = self._client
        if client is not None:
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                client.close()
            except OSError:
                pass
        try:
            self._server.close()
        except OSError:
            pass
        self._thread.join(timeout=2)


def synthetic_stream_key() -> str:
    return "_".join(["stm", "chunk8", "fixture", "material", "000000000000000000000000"])


def secret_like_fixture_value() -> str:
    return "".join(["fixture", "-", "stream", "-", "key", "-", hashlib.sha256(b"chunk7").hexdigest()[:24]])


def write_obs_fixture(root: Path) -> Path:
    obs_dir = root / "obs-config"
    scenes_dir = obs_dir / "basic" / "scenes"
    profile_dir = obs_dir / "basic" / "profiles" / "Fixture Profile"
    scenes_dir.mkdir(parents=True)
    profile_dir.mkdir(parents=True)
    collection = {
        "name": "Fixture Main",
        "sources": [
            {"name": "Main", "id": "scene"},
            {"name": "Color Backdrop", "id": "color_source"},
            {"id": "color_source", "name": "ID First Color"},
            {"name": "Station Overlay", "id": "browser_source", "filters": [{"name": "LUT", "id": "color_filter_v2"}]},
            {"name": "Headline", "id": "text_ft2_source"},
            {"name": "BRB Image", "id": "image_source"},
            {"name": "Media Clip", "id": "ffmpeg_source"},
            {"name": "Face Camera", "id": "av_capture_input", "device_id": "fixture-camera"},
            {"name": "Desk Mic", "id": "coreaudio_input_capture", "device_id": "fixture-mic"},
            {"name": "Screen Share", "id": "screen_capture", "display_id": "fixture-display"},
            {"name": "Missing Plugin Source", "id": "third_party_camera_fx", "module": "obs-third-party-fx"},
            {"name": "Unplugged Camera", "id": "av_capture_input"},
        ],
    }
    (scenes_dir / "fixture-main.json").write_text(json.dumps(collection, indent=2) + "\n", encoding="utf-8")
    (profile_dir / "basic.ini").write_text(
        "[Output]\nMode=Advanced\n[AdvOut]\nEncoder=obs_x264\nTrack1Bitrate=320\n",
        encoding="utf-8",
    )
    (obs_dir / "service.json").write_text(
        json.dumps(
            {
                "service": "Twitch",
                "settings": {
                    "server": "rtmp://fixture.invalid/app",
                    "key": secret_like_fixture_value(),
                },
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return obs_dir


def hash_tree(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        digest.update(path.relative_to(root).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def expected_import_report(service_key_action: str = "requires-consent") -> dict:
    return {
        "reportId": "import-fixture-main",
        "collectionId": "fixture-main",
        "generatedAt": "1970-01-01T00:00:00.000Z",
        "mapped": [
            {"id": "scene:Main", "kind": "scene", "label": "Main", "state": "mapped", "reason": "mapped_native"},
            {"id": "source:Color Backdrop", "kind": "source", "label": "Color Backdrop", "state": "mapped", "reason": "mapped_native", "moduleName": "color_source"},
            {"id": "source:ID First Color", "kind": "source", "label": "ID First Color", "state": "mapped", "reason": "mapped_native", "moduleName": "color_source"},
            {"id": "source:Station Overlay", "kind": "source", "label": "Station Overlay", "state": "mapped", "reason": "mapped_native", "moduleName": "browser_source"},
            {"id": "source:Headline", "kind": "source", "label": "Headline", "state": "mapped", "reason": "mapped_native", "moduleName": "text_ft2_source"},
            {"id": "source:BRB Image", "kind": "source", "label": "BRB Image", "state": "mapped", "reason": "mapped_native", "moduleName": "image_source"},
            {"id": "source:Media Clip", "kind": "source", "label": "Media Clip", "state": "mapped", "reason": "mapped_native", "moduleName": "ffmpeg_source"},
        ],
        "degraded": [
            {"id": "source:Face Camera", "kind": "source", "label": "Face Camera", "state": "degraded", "reason": "permission_required", "moduleName": "av_capture_input", "tccClass": "camera", "notes": ["Native import defers camera permission until explicit operator approval."]},
            {"id": "source:Desk Mic", "kind": "source", "label": "Desk Mic", "state": "degraded", "reason": "permission_required", "moduleName": "coreaudio_input_capture", "tccClass": "microphone", "notes": ["Native import defers microphone permission until explicit operator approval."]},
            {"id": "source:Screen Share", "kind": "source", "label": "Screen Share", "state": "degraded", "reason": "permission_required", "moduleName": "screen_capture", "tccClass": "screen", "notes": ["Native import defers screen permission until explicit operator approval."]},
            {"id": "source:Missing Plugin Source", "kind": "source", "label": "Missing Plugin Source", "state": "degraded", "reason": "missing_plugin", "moduleName": "third_party_camera_fx", "notes": ["Replaced with placeholder source because obs-third-party-fx is not bundled upstream."]},
            {"id": "source:Unplugged Camera", "kind": "source", "label": "Unplugged Camera", "state": "degraded", "reason": "missing_device", "moduleName": "av_capture_input", "tccClass": "camera", "notes": ["Original device identifier was absent from the fixture."]},
        ],
        "unresolved": [
            {"id": "filter:Station Overlay:LUT", "kind": "filter", "label": "Station Overlay / LUT", "state": "unresolved", "reason": "unsupported_frontend_feature", "moduleName": "color_filter_v2", "notes": ["OBS frontend filter is not imported by the native host scaffold."]},
        ],
        "profile": {
            "mappedEncoder": "x264",
            "mappedOutput": "rtmp",
            "downgrades": [
                {"id": "profile:Fixture Profile:encoder", "kind": "profile", "label": "Fixture Profile", "state": "degraded", "reason": "profile_downgraded", "notes": ["OBS encoder obs_x264 mapped to native x264 fallback."]}
            ],
            "serviceKeyAction": service_key_action,
        },
    }


class StudioHostLifecycleTest(unittest.TestCase):
    def test_tcc_prompt_exercise_scaffold_reports_no_tcc_claims(self) -> None:
        process, port, _ = start_host()
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)

        exercised = rpc(sock, 90, "host.exerciseTccPrompts", {"reason": "l4-rehearsal"})
        self.assertEqual(
            exercised["result"],
            {
                "ok": True,
                "mode": "scaffold-no-tcc",
                "promptCapable": False,
                "cameraAttempted": False,
                "microphoneAttempted": False,
                "screenAttempted": False,
                "cameraActivated": False,
                "microphoneActivated": False,
                "screenActivated": False,
                "instantiatedCount": 0,
                "failedCount": 0,
                "sanitizedFailureClasses": [],
            },
        )
        serialized = json.dumps(exercised)
        for forbidden in ["l4-rehearsal", "/Users/", "device", "display", "fixture-camera", "fixture-mic"]:
            self.assertNotIn(forbidden, serialized)

    def test_tcc_screen_exercise_has_display_capture_fallback(self) -> None:
        source = Path(__file__).resolve().parents[2] / "src" / "studio_host.cpp"
        contents = source.read_text(encoding="utf-8")

        self.assertIn('return {"screen_capture", "display_capture"};', contents)

    def test_obs_fixture_import_scan_load_report_is_copy_only_and_redacted(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            obs_dir = write_obs_fixture(temp_root)
            streammate_home = temp_root / "streammate-home"
            before_hash = hash_tree(obs_dir)
            fixture_secret = secret_like_fixture_value()

            process, port, _ = start_host(env={"STREAMMATE_OBS_CONFIG_DIR": str(obs_dir), "STREAMMATE_HOME": str(streammate_home)})
            self.addCleanup(stop_process, process)
            sock = websocket_connect(port)
            self.addCleanup(sock.close)

            scan = rpc(sock, 100, "import.scan", {})
            self.assertEqual(
                scan["result"],
                {
                    "ok": True,
                    "configDirLabel": "$OBS_CONFIG_DIR",
                    "collections": [
                        {
                            "collectionId": "fixture-main",
                            "name": "Fixture Main",
                            "sourceCount": 12,
                            "profileCount": 1,
                            "serviceKeyAction": "requires-consent",
                        }
                    ],
                },
            )
            self.assertNotIn(fixture_secret, json.dumps(scan))
            self.assertNotIn(str(obs_dir), json.dumps(scan))

            loaded = rpc(sock, 101, "import.load", {"collectionId": "fixture-main"})
            self.assertEqual(loaded["result"]["report"], expected_import_report())
            self.assertEqual(
                loaded["result"]["promptSources"],
                {
                    "mode": "scaffold-no-tcc",
                    "promptCapable": False,
                    "cameraCount": 2,
                    "microphoneCount": 1,
                    "screenCount": 1,
                    "instantiatedCount": 0,
                    "deferredCount": 4,
                    "failedCount": 0,
                },
            )
            self.assertEqual(loaded["result"]["destinationLabel"], "$STREAMMATE_HOME/studio/obs-imports/fixture-main")
            self.assertEqual(hash_tree(obs_dir), before_hash)
            serialized_loaded = json.dumps(loaded)
            for forbidden in [fixture_secret, "fixture-camera", "fixture-mic", "fixture-display", str(obs_dir)]:
                self.assertNotIn(forbidden, serialized_loaded)

            copied_collection = streammate_home / "studio" / "obs-imports" / "fixture-main" / "basic" / "scenes" / "fixture-main.json"
            self.assertEqual(json.loads(copied_collection.read_text(encoding="utf-8"))["name"], "Fixture Main")
            placeholder = json.loads(
                (streammate_home / "studio" / "obs-imports" / "fixture-main" / "streammate-import-map.json").read_text(encoding="utf-8")
            )
            self.assertEqual(
                placeholder["placeholderSources"],
                [{"sourceId": "source:Missing Plugin Source", "label": "Missing Plugin Source", "reason": "missing_plugin"}],
            )
            self.assertFalse((streammate_home / "studio" / "obs-imports" / "fixture-main" / "service.json").exists())
            self.assertNotIn(fixture_secret, json.dumps(placeholder))

            report = rpc(sock, 102, "import.report", {"collectionId": "fixture-main"})
            self.assertEqual(report["result"], expected_import_report())
            self.assertNotIn(fixture_secret, json.dumps(report))

    @unittest.skipUnless(hasattr(os, "symlink"), "symlink support is required")
    def test_obs_import_skips_symlinked_config_entries(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            obs_dir = write_obs_fixture(temp_root)
            streammate_home = temp_root / "streammate-home"
            outside = temp_root / "outside"
            outside.mkdir()
            secret_file = outside / "secret.txt"
            secret = "outside-secret-material"
            secret_file.write_text(secret, encoding="utf-8")
            outside_dir = outside / "secret-dir"
            outside_dir.mkdir()
            (outside_dir / "nested.txt").write_text(secret, encoding="utf-8")
            regular_file = obs_dir / "global.ini"
            regular_file.write_text("[General]\nName=Fixture\n", encoding="utf-8")
            try:
                os.symlink(secret_file, obs_dir / "scene.json")
                os.symlink(secret_file, obs_dir / "basic" / "scenes" / "secret-scene.json")
                os.symlink(outside_dir, obs_dir / "linked-dir")
            except OSError as exc:
                self.skipTest(f"symlink creation unavailable: {exc}")

            process, port, _ = start_host(env={"STREAMMATE_OBS_CONFIG_DIR": str(obs_dir), "STREAMMATE_HOME": str(streammate_home)})
            self.addCleanup(stop_process, process)
            sock = websocket_connect(port)
            self.addCleanup(sock.close)

            loaded = rpc(sock, 103, "import.load", {"collectionId": "fixture-main"})
            self.assertIn("result", loaded)
            imported_root = streammate_home / "studio" / "obs-imports" / "fixture-main"
            self.assertEqual((imported_root / "global.ini").read_text(encoding="utf-8"), "[General]\nName=Fixture\n")
            self.assertFalse((imported_root / "service.json").exists())
            self.assertFalse((imported_root / "scene.json").exists())
            self.assertFalse((imported_root / "basic" / "scenes" / "secret-scene.json").exists())
            self.assertFalse((imported_root / "linked-dir").exists())
            for path in imported_root.rglob("*"):
                if path.is_file():
                    self.assertNotIn(secret, path.read_text(encoding="utf-8"))

    def test_obs_import_pathological_braceless_collection_scan_is_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            obs_dir = temp_root / "obs-config"
            scenes_dir = obs_dir / "basic" / "scenes"
            scenes_dir.mkdir(parents=True)
            collection = scenes_dir / "fixture-main.json"
            collection.write_text('"name":"Brace Free",' + ('"padding":"xxxxxxxxxx",' * 10000), encoding="utf-8")

            process, port, _ = start_host()
            self.addCleanup(stop_process, process)
            sock = websocket_connect(port)
            self.addCleanup(sock.close)

            started = time.monotonic()
            scan = rpc(sock, 104, "import.scan", {"configDir": str(obs_dir)})
            elapsed = time.monotonic() - started
            self.assertLess(elapsed, 1.0)
            self.assertEqual(
                scan["result"],
                {
                    "ok": True,
                    "configDirLabel": "$OBS_CONFIG_DIR",
                    "collections": [
                        {
                            "collectionId": "fixture-main",
                            "name": "Brace Free",
                            "sourceCount": 0,
                            "profileCount": 0,
                            "serviceKeyAction": "not-present",
                        }
                    ],
                },
            )

            collection.write_text(
                json.dumps(
                    {
                        "name": "Small",
                        "sources": [
                            {"name": "Cam", "id": "av_capture_input"},
                            {"id": "color_source", "name": "Backdrop"},
                        ],
                    }
                ),
                encoding="utf-8",
            )
            loaded = rpc(
                sock,
                105,
                "import.load",
                {"configDir": str(obs_dir), "streammateHome": str(temp_root / "streammate-home"), "collectionId": "fixture-main"},
            )
            self.assertEqual(loaded["result"]["report"]["mapped"][0]["label"], "Backdrop")
            self.assertEqual(loaded["result"]["report"]["degraded"][0]["label"], "Cam")

    def test_obs_import_failure_leaves_prior_host_state_intact(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            obs_dir = write_obs_fixture(temp_root)
            streammate_home = temp_root / "streammate-home"
            process, port, _ = start_host(env={"STREAMMATE_OBS_CONFIG_DIR": str(obs_dir), "STREAMMATE_HOME": str(streammate_home)})
            self.addCleanup(stop_process, process)
            sock = websocket_connect(port)
            self.addCleanup(sock.close)

            ok = rpc(sock, 110, "import.load", {"collectionId": "fixture-main"})
            self.assertEqual(ok["result"]["report"], expected_import_report())
            imported_root = streammate_home / "studio" / "obs-imports" / "fixture-main"
            before = hash_tree(imported_root)

            failed = rpc(sock, 111, "import.load", {"collectionId": "missing-collection"})
            self.assertEqual(failed["error"]["code"], -32602)
            self.assertIn("collection not found", failed["error"]["message"])
            self.assertEqual(hash_tree(imported_root), before)

    def test_ready_hello_health_heartbeat_and_shutdown(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            state_file = Path(temp_dir) / "host-state.json"
            process, port, lines = start_host(state_file=state_file)
            self.addCleanup(stop_process, process)
            self.assertFalse(any("/Users/" in line or str(temp_dir) in line for line in lines), lines)
            self.assertEqual(json.loads(state_file.read_text())["status"], "ready")

            sock = websocket_connect(port)
            self.addCleanup(sock.close)
            self.assertEqual(recv_text(sock)["type"], "host.started")
            self.assertEqual(recv_text(sock)["type"], "host.ready")

            send_text(sock, {"jsonrpc": "2.0", "id": 1, "method": "host.hello"})
            hello = recv_text(sock)
            self.assertEqual(hello["result"]["hostId"], "studio-host-1")
            self.assertEqual(hello["result"]["heartbeatMs"], 5000)

            send_text(sock, {"jsonrpc": "2.0", "id": 2, "method": "host.health"})
            health = recv_text(sock)
            self.assertEqual(health["result"]["status"], "ready")
            self.assertTrue(health["result"]["engineStarted"])

            heartbeat = recv_text(sock, timeout=7)
            self.assertEqual(heartbeat["type"], "host.health")

            send_text(sock, {"jsonrpc": "2.0", "id": 3, "method": "host.shutdown"})
            shutdown = recv_text(sock)
            self.assertTrue(shutdown["result"]["ok"])
            self.assertEqual(process.wait(timeout=5), 0)
            self.assertEqual(json.loads(state_file.read_text())["status"], "stopped")

    def test_scene_source_commands_and_offscreen_capture_png_are_sanitized(self) -> None:
        process, port, _ = start_host()
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)

        overlay_url = overlay_url_with_secret("render-parity-secret")
        updated_url = overlay_url_with_secret("updated-render-secret")

        loaded = rpc(
            sock,
            10,
            "scene.load",
            {"sceneId": "parity-scene", "width": 64, "height": 36, "background": "#203040"},
        )
        self.assertTrue(loaded["result"]["ok"])
        self.assertEqual(loaded["result"]["sceneId"], "parity-scene")
        self.assertEqual(loaded["result"]["width"], 64)
        self.assertEqual(loaded["result"]["height"], 36)

        created = rpc(
            sock,
            11,
            "source.create",
            {
                "sceneId": "parity-scene",
                "sourceId": "station-overlay",
                "kind": "browser",
                "url": overlay_url,
                "x": 0,
                "y": 0,
                "width": 64,
                "height": 36,
            },
        )
        self.assertTrue(created["result"]["ok"])
        self.assertEqual(created["result"]["sourceId"], "station-overlay")
        self.assertEqual(created["result"]["kind"], "browser")
        self.assertEqual(created["result"]["sceneId"], "parity-scene")
        self.assertEqual(created["result"]["urlStatus"], "stored-redacted")
        self.assertNotIn("render-parity-secret", json.dumps(created))
        self.assertNotIn(overlay_url, json.dumps(created))

        updated = rpc(
            sock,
            12,
            "source.update",
            {"sourceId": "station-overlay", "url": updated_url, "opacity": 0.5},
        )
        self.assertTrue(updated["result"]["ok"])
        self.assertEqual(updated["result"]["urlStatus"], "stored-redacted")
        self.assertEqual(updated["result"]["opacity"], 0.5)
        self.assertNotIn("updated-render-secret", json.dumps(updated))
        self.assertNotIn(updated_url, json.dumps(updated))

        program = rpc(sock, 13, "scene.setProgram", {"sceneId": "parity-scene"})
        self.assertTrue(program["result"]["ok"])
        self.assertEqual(program["result"]["programSceneId"], "parity-scene")

        muted = rpc(sock, 14, "source.mute", {"sourceId": "station-overlay", "muted": True})
        self.assertTrue(muted["result"]["ok"])
        self.assertTrue(muted["result"]["muted"])

        captured = rpc(sock, 15, "scene.captureFrame", {"sceneId": "parity-scene", "format": "png"})
        result = captured["result"]
        self.assertTrue(result["ok"])
        self.assertEqual(result["format"], "png")
        self.assertEqual(result["sceneId"], "parity-scene")
        self.assertEqual(result["width"], 64)
        self.assertEqual(result["height"], 36)
        self.assertEqual(result["sourceCount"], 1)
        self.assertEqual(result["mutedSourceCount"], 1)
        self.assertEqual(result["renderer"], "offscreen-scaffold")
        decoded_png = base64.b64decode(result["pngBase64"])
        self.assertTrue(decoded_png.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(struct.unpack("!II", decoded_png[16:24]), (64, 36))
        sanitized_capture = json.dumps(result)
        self.assertNotIn("render-parity-secret", sanitized_capture)
        self.assertNotIn("updated-render-secret", sanitized_capture)
        self.assertNotIn("/Users/", sanitized_capture)

    def test_output_configure_start_stop_status_stats_and_ingest_error_are_sanitized(self) -> None:
        ingest = FakeRtmpIngest()
        self.addCleanup(ingest.kill_ingest)
        process, port, _ = start_host()
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)
        stream_key = synthetic_stream_key()

        configured = rpc(
            sock,
            30,
            "output.configure",
            {"outputId": "rtmp-main", "endpoint": ingest.endpoint, "videoEncoder": "videotoolbox_h264", "audioEncoder": "aac"},
        )
        self.assertTrue(configured["result"]["ok"])
        self.assertEqual(configured["result"]["streamKeyStatus"], "not-stored")
        self.assertIn(configured["result"]["encoder"]["actual"], ["videotoolbox_h264", "x264-scaffold"])
        self.assertNotIn(stream_key, json.dumps(configured))

        subscribed = rpc(sock, 31, "stats.subscribe", {"intervalMs": 100})
        self.assertTrue(subscribed["result"]["ok"])
        self.assertEqual(subscribed["result"]["sampleShape"], "spec18-item8")

        started = rpc(sock, 32, "output.start", {"outputId": "rtmp-main", "endpoint": ingest.endpoint, "streamKey": stream_key})
        self.assertTrue(started["result"]["ok"])
        self.assertTrue(started["result"]["running"])
        self.assertEqual(started["result"]["streamKeyStatus"], "memory-only-redacted")
        self.assertNotIn(stream_key, json.dumps(started))
        ingest.wait_for_bytes()

        seen_started = False
        seen_stats = False
        deadline = time.time() + 5
        while time.time() < deadline and not (seen_started and seen_stats):
            event = recv_text(sock, timeout=1)
            if event.get("type") == "output.started":
                seen_started = True
                self.assertNotIn(stream_key, json.dumps(event))
            if event.get("type") == "stats.sample":
                seen_stats = True
                sample = event["payload"]["sample"]
                self.assertEqual(sample["shape"], "spec18-item8")
                self.assertGreaterEqual(sample["encodedFrames"], 0)
                self.assertLess(sample["severeFrameLossPercent"], 1.0)
                self.assertIn("congestion", sample)
                self.assertNotIn(stream_key, json.dumps(event))
        self.assertTrue(seen_started)
        self.assertTrue(seen_stats)

        status = rpc(sock, 33, "output.status", {"outputId": "rtmp-main"})["result"]
        self.assertTrue(status["running"])
        self.assertEqual(status["endpoint"]["host"], "127.0.0.1")
        self.assertEqual(status["endpoint"]["port"], ingest.port)
        self.assertEqual(status["streamKeyStatus"], "memory-only-redacted")
        self.assertTrue(status["panicMute"]["hostAudioHardMuted"])
        self.assertNotIn(stream_key, json.dumps(status))
        query_marker = "".join(["to", "ken"]) + "="
        self.assertNotIn(query_marker, json.dumps(status))

        ingest.kill_ingest()
        error_event = None
        deadline = time.time() + 5
        while time.time() < deadline and error_event is None:
            event = recv_text(sock, timeout=1)
            if event.get("type") == "output.error":
                error_event = event
        self.assertIsNotNone(error_event)
        serialized_error = json.dumps(error_event)
        self.assertIn("fake ingest disconnected", serialized_error)
        self.assertNotIn(stream_key, serialized_error)
        self.assertNotIn("/Users/", serialized_error)
        self.assertFalse(rpc(sock, 34, "output.status", {"outputId": "rtmp-main"})["result"]["running"])

        stopped = rpc(sock, 35, "output.stop", {"outputId": "rtmp-main"})["result"]
        self.assertTrue(stopped["ok"])
        self.assertFalse(stopped["running"])
        self.assertEqual(stopped["streamKeyStatus"], "cleared")
        self.assertNotIn(stream_key, json.dumps(stopped))

    def test_output_rejects_non_loopback_rtmp_endpoint_without_exposing_key(self) -> None:
        process, port, _ = start_host()
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)
        stream_key = synthetic_stream_key()

        rejected = rpc(
            sock,
            40,
            "output.start",
            {"outputId": "rtmp-main", "endpoint": "rtmp://live.example.invalid/app", "streamKey": stream_key},
        )
        self.assertEqual(rejected["error"]["code"], -32602)
        self.assertIn("local fake RTMP ingest", rejected["error"]["message"])
        self.assertNotIn(stream_key, json.dumps(rejected))

    def test_scene_source_commands_validate_required_state(self) -> None:
        process, port, _ = start_host()
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)

        create_before_scene = rpc(
            sock,
            20,
            "source.create",
            {"sceneId": "missing-scene", "sourceId": "overlay", "kind": "browser", "url": "https://station.localhost/overlay"},
        )
        self.assertEqual(create_before_scene["error"]["code"], -32602)
        self.assertIn("scene not loaded", create_before_scene["error"]["message"])

        missing_source = rpc(sock, 21, "source.mute", {"sourceId": "missing-source", "muted": True})
        self.assertEqual(missing_source["error"]["code"], -32602)
        self.assertIn("source not found", missing_source["error"]["message"])

        missing_program_scene = rpc(sock, 22, "scene.setProgram", {"sceneId": "missing-scene"})
        self.assertEqual(missing_program_scene["error"]["code"], -32602)
        self.assertIn("scene not loaded", missing_program_scene["error"]["message"])

    def test_wrong_token_is_rejected(self) -> None:
        process, port, _ = start_host()
        self.addCleanup(stop_process, process)
        self.assertIn("401 Unauthorized", websocket_wrong_token_status(port))

    def test_refuses_non_loopback_bind(self) -> None:
        process = subprocess.run(
            [str(HOST_BIN), "--token", TOKEN, "--host", "0.0.0.0"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=5,
            check=False,
        )
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("refuses non-loopback", process.stdout)
        self.assertNotIn("/Users/", process.stdout)

    def test_scene_list_returns_loaded_scenes_with_program_marked(self) -> None:
        process, port, _ = start_host()
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)

        rpc(sock, 200, "scene.load", {"sceneId": "scene-a", "width": 64, "height": 36})
        rpc(sock, 201, "scene.load", {"sceneId": "scene-b", "width": 64, "height": 36})
        rpc(sock, 202, "scene.setProgram", {"sceneId": "scene-b"})

        listed = rpc(sock, 203, "scene.list", {})["result"]
        self.assertTrue(listed["ok"])
        self.assertEqual(listed["programSceneId"], "scene-b")
        by_id = {scene["sceneId"]: scene for scene in listed["scenes"]}
        self.assertEqual(set(by_id), {"scene-a", "scene-b"})
        self.assertFalse(by_id["scene-a"]["program"])
        self.assertTrue(by_id["scene-b"]["program"])

        # The program marker round-trips after a subsequent scene.setProgram.
        rpc(sock, 204, "scene.setProgram", {"sceneId": "scene-a"})
        relisted = rpc(sock, 205, "scene.list", {})["result"]
        self.assertEqual(relisted["programSceneId"], "scene-a")
        reindexed = {scene["sceneId"]: scene for scene in relisted["scenes"]}
        self.assertTrue(reindexed["scene-a"]["program"])
        self.assertFalse(reindexed["scene-b"]["program"])

    def test_scene_item_transform_is_observable_in_capture_and_state(self) -> None:
        process, port, _ = start_host()
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)

        rpc(sock, 210, "scene.load", {"sceneId": "xf-scene", "width": 64, "height": 36})
        rpc(
            sock,
            211,
            "source.create",
            {
                "sceneId": "xf-scene",
                "sourceId": "item",
                "kind": "browser",
                "url": overlay_url_with_secret("transform-secret"),
                "x": 0,
                "y": 0,
                "width": 64,
                "height": 36,
            },
        )

        before = rpc(sock, 212, "scene.captureFrame", {"sceneId": "xf-scene", "format": "png"})["result"]["pngBase64"]

        transformed = rpc(
            sock,
            213,
            "scene.itemTransform",
            {"sceneId": "xf-scene", "sourceId": "item", "x": 4, "y": 6, "width": 10, "height": 8},
        )["result"]
        self.assertTrue(transformed["ok"])
        self.assertEqual(transformed["sceneId"], "xf-scene")
        self.assertEqual(transformed["sourceId"], "item")
        self.assertEqual(transformed["transform"], {"x": 4, "y": 6, "width": 10, "height": 8})
        self.assertNotIn("transform-secret", json.dumps(transformed))

        after = rpc(sock, 214, "scene.captureFrame", {"sceneId": "xf-scene", "format": "png"})["result"]["pngBase64"]
        self.assertNotEqual(before, after)

        # A transform against an unknown scene item fails with a named error.
        ghost = rpc(sock, 215, "scene.itemTransform", {"sceneId": "xf-scene", "sourceId": "ghost", "x": 1})
        self.assertEqual(ghost["error"]["code"], -32602)
        self.assertIn("source not found", ghost["error"]["message"])

        # A transform against an unloaded scene fails with a named error.
        no_scene = rpc(sock, 216, "scene.itemTransform", {"sceneId": "missing-scene", "sourceId": "item", "x": 1})
        self.assertEqual(no_scene["error"]["code"], -32602)
        self.assertIn("scene not loaded", no_scene["error"]["message"])

        # A rejected transform (invalid dimension) must not partially apply x/y.
        rejected = rpc(
            sock,
            217,
            "scene.itemTransform",
            {"sceneId": "xf-scene", "sourceId": "item", "x": 50, "height": 0},
        )
        self.assertEqual(rejected["error"]["code"], -32602)
        self.assertIn("height must be positive", rejected["error"]["message"])
        # The prior valid transform (x=4) is still in effect — x=50 was not applied.
        reread = rpc(
            sock,
            218,
            "scene.itemTransform",
            {"sceneId": "xf-scene", "sourceId": "item"},
        )["result"]
        self.assertEqual(reread["transform"], {"x": 4, "y": 6, "width": 10, "height": 8})

    def test_source_remove_deletes_source_and_stale_update_fails_named(self) -> None:
        process, port, _ = start_host()
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)

        rpc(sock, 220, "scene.load", {"sceneId": "rm-scene", "width": 64, "height": 36})
        rpc(
            sock,
            221,
            "source.create",
            {"sceneId": "rm-scene", "sourceId": "doomed", "kind": "browser", "url": "https://station.localhost/overlay"},
        )

        removed = rpc(sock, 222, "source.remove", {"sourceId": "doomed"})["result"]
        self.assertTrue(removed["ok"])
        self.assertEqual(removed["sourceId"], "doomed")
        self.assertTrue(removed["removed"])

        # A subsequent source.update on the removed id fails with a named error.
        stale = rpc(sock, 223, "source.update", {"sourceId": "doomed", "opacity": 0.5})
        self.assertEqual(stale["error"]["code"], -32602)
        self.assertIn("source not found", stale["error"]["message"])

        # Removing an already-removed id fails with the same named error.
        again = rpc(sock, 224, "source.remove", {"sourceId": "doomed"})
        self.assertEqual(again["error"]["code"], -32602)
        self.assertIn("source not found", again["error"]["message"])

        # The removed source no longer contributes to the offscreen composite.
        captured = rpc(sock, 225, "scene.captureFrame", {"sceneId": "rm-scene", "format": "png"})["result"]
        self.assertEqual(captured["sourceCount"], 0)

    def test_native_overlay_source_is_explicit_opt_in_and_renders_action(self) -> None:
        # Spec 34 Capability 2: source.create kind "native-overlay" is the explicit
        # opt-in surface; it renders OverlayAction payloads at 1280x720 with
        # apply->rendered timing keyed to the Spec 07 budget keys.
        process, port, _ = start_host()
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)

        created = rpc(
            sock,
            240,
            "source.create",
            {"sourceId": "phase-b-overlay", "kind": "native-overlay"},
        )["result"]
        self.assertTrue(created["ok"])
        self.assertEqual(created["sourceId"], "phase-b-overlay")
        self.assertEqual(created["kind"], "native-overlay")
        self.assertEqual(created["renderer"], "native-overlay-rasterizer")
        self.assertEqual(created["width"], 1280)
        self.assertEqual(created["height"], 720)

        applied = rpc(
            sock,
            241,
            "source.update",
            {
                "sourceId": "phase-b-overlay",
                "overlayAction": {
                    "type": "toast",
                    "layer": "foreground",
                    "payload": {"message": "NOW LIVE", "tone": "success"},
                },
            },
        )["result"]
        self.assertTrue(applied["ok"])
        self.assertEqual(applied["kind"], "native-overlay")
        self.assertEqual(applied["category"], "toast")
        self.assertEqual(set(applied["timing"].keys()), {"enterMs", "exitMs"})
        self.assertFalse(applied["empty"])
        decoded = base64.b64decode(applied["pngBase64"])
        self.assertTrue(decoded.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(struct.unpack("!II", decoded[16:24]), (1280, 720))

        # clear empties the raster within one apply cycle.
        cleared = rpc(
            sock,
            242,
            "source.update",
            {"sourceId": "phase-b-overlay", "overlayAction": {"type": "clear", "layer": "panic", "payload": {"reason": "panic"}}},
        )["result"]
        self.assertEqual(cleared["category"], "clear")
        self.assertEqual(set(cleared["timing"].keys()), {"appliesMs"})
        self.assertTrue(cleared["empty"])

    def test_native_overlay_action_url_is_redacted(self) -> None:
        process, port, _ = start_host()
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)

        rpc(sock, 250, "source.create", {"sourceId": "gen-overlay", "kind": "native-overlay"})
        secret_url = overlay_url_with_secret("native-overlay-secret")
        applied = rpc(
            sock,
            251,
            "source.update",
            {
                "sourceId": "gen-overlay",
                "overlayAction": {
                    "type": "generated-image",
                    "payload": {"assetId": "img-1", "alt": "A CASTLE", "caption": "CASTLE", "url": secret_url},
                },
            },
        )
        self.assertTrue(applied["result"]["ok"])
        self.assertEqual(applied["result"]["category"], "generated-image")
        self.assertNotIn("native-overlay-secret", json.dumps(applied))
        self.assertNotIn(secret_url, json.dumps(applied))

    def test_session_without_native_overlay_instantiates_zero_renderer_state(self) -> None:
        # Opt-in isolation: a session that never sends kind "native-overlay" holds
        # zero native renderer state and the default browser source.create path is
        # byte-compatible with pre-chunk behavior.
        process, port, _ = start_host()
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)

        health_before = rpc(sock, 260, "host.health")["result"]
        self.assertEqual(health_before["nativeOverlaySourceCount"], 0)

        rpc(sock, 261, "scene.load", {"sceneId": "iso-scene", "width": 64, "height": 36})
        created = rpc(
            sock,
            262,
            "source.create",
            {"sceneId": "iso-scene", "sourceId": "browser-only", "kind": "browser", "url": "https://station.localhost/overlay"},
        )["result"]
        # Default browser create is byte-compatible with pre-chunk tests.
        self.assertTrue(created["ok"])
        self.assertEqual(created["kind"], "browser")
        self.assertEqual(created["urlStatus"], "stored-redacted")
        self.assertNotIn("renderer", created)

        health_still_zero = rpc(sock, 263, "host.health")["result"]
        self.assertEqual(health_still_zero["nativeOverlaySourceCount"], 0)

        # Opting in instantiates exactly one native renderer; removing it returns to zero.
        rpc(sock, 264, "source.create", {"sourceId": "opt-in", "kind": "native-overlay"})
        health_opted = rpc(sock, 265, "host.health")["result"]
        self.assertEqual(health_opted["nativeOverlaySourceCount"], 1)
        rpc(sock, 266, "source.remove", {"sourceId": "opt-in"})
        health_after = rpc(sock, 267, "host.health")["result"]
        self.assertEqual(health_after["nativeOverlaySourceCount"], 0)

    def test_live_egress_refused_without_launch_flag_and_no_endpoint_contact(self) -> None:
        ingest = FakeRtmpIngest()
        self.addCleanup(ingest.kill_ingest)
        process, port, _ = start_host()  # launched WITHOUT --allow-live-egress
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)
        stream_key = synthetic_stream_key()

        # The configure gate refuses the JSON allowLiveEgress flag when the launch flag is absent.
        configured = rpc(
            sock,
            230,
            "output.configure",
            {"outputId": "rtmp-main", "endpoint": ingest.endpoint, "allowLiveEgress": True},
        )
        self.assertEqual(configured["error"]["code"], -32602)
        self.assertIn("allow-live-egress", configured["error"]["message"])
        self.assertNotIn(stream_key, json.dumps(configured))

        # output.start carrying allowLiveEgress:true fails closed before any endpoint is touched.
        started = rpc(
            sock,
            231,
            "output.start",
            {"outputId": "rtmp-main", "endpoint": ingest.endpoint, "streamKey": stream_key, "allowLiveEgress": True},
        )
        self.assertEqual(started["error"]["code"], -32602)
        self.assertIn("allow-live-egress", started["error"]["message"])
        self.assertNotIn(stream_key, json.dumps(started))
        self.assertNotIn("/Users/", json.dumps(started))

        # Connection-attempt probe: no socket was opened to the (loopback) fake ingest endpoint.
        time.sleep(0.3)
        self.assertEqual(ingest.connection_count, 0)

    def test_launch_flag_opens_configure_live_gate(self) -> None:
        ingest = FakeRtmpIngest()
        self.addCleanup(ingest.kill_ingest)
        process, port, _ = start_host("--allow-live-egress")
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)

        configured = rpc(
            sock,
            240,
            "output.configure",
            {"outputId": "rtmp-main", "endpoint": ingest.endpoint, "allowLiveEgress": True},
        )["result"]
        self.assertTrue(configured["ok"])
        self.assertTrue(configured["allowLiveEgress"])

    def test_rejected_live_start_does_not_arm_live_egress_for_later_call(self) -> None:
        # Regression: a rejected output.start carrying allowLiveEgress:true must not
        # leave the live-egress flag armed for a subsequent start that omits it.
        ingest = FakeRtmpIngest()
        self.addCleanup(ingest.kill_ingest)
        process, port, _ = start_host("--allow-live-egress")
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)
        stream_key = synthetic_stream_key()

        # Configure the fake-ingest output WITHOUT requesting live egress.
        rpc(sock, 260, "output.configure", {"outputId": "rtmp-main", "endpoint": ingest.endpoint})

        # A start for the wrong output id carrying allowLiveEgress:true is rejected.
        rejected = rpc(
            sock,
            261,
            "output.start",
            {"outputId": "wrong-id", "endpoint": "rtmp://live.example.invalid/app", "allowLiveEgress": True, "streamKey": stream_key},
        )
        self.assertEqual(rejected["error"]["code"], -32602)
        self.assertIn("output is not configured", rejected["error"]["message"])
        self.assertNotIn(stream_key, json.dumps(rejected))

        # A plain start for the configured output (no allowLiveEgress) still takes
        # the fake-ingest path — the rejected call did not arm live egress.
        started = rpc(
            sock,
            262,
            "output.start",
            {"outputId": "rtmp-main", "endpoint": ingest.endpoint, "streamKey": stream_key},
        )["result"]
        self.assertTrue(started["ok"])
        self.assertTrue(started["running"])
        self.assertEqual(started["streamKeyStatus"], "memory-only-redacted")
        ingest.wait_for_bytes()
        rpc(sock, 263, "output.stop", {"outputId": "rtmp-main"})

    def test_launch_flag_present_keeps_fake_ingest_path_byte_compatible(self) -> None:
        ingest = FakeRtmpIngest()
        self.addCleanup(ingest.kill_ingest)
        process, port, _ = start_host("--allow-live-egress")
        self.addCleanup(stop_process, process)
        sock = websocket_connect(port)
        self.addCleanup(sock.close)
        stream_key = synthetic_stream_key()

        configured = rpc(
            sock,
            250,
            "output.configure",
            {"outputId": "rtmp-main", "endpoint": ingest.endpoint, "videoEncoder": "videotoolbox_h264", "audioEncoder": "aac"},
        )["result"]
        self.assertTrue(configured["ok"])
        self.assertEqual(configured["streamKeyStatus"], "not-stored")
        self.assertNotIn("allowLiveEgress", configured)

        started = rpc(
            sock,
            251,
            "output.start",
            {"outputId": "rtmp-main", "endpoint": ingest.endpoint, "streamKey": stream_key},
        )["result"]
        self.assertTrue(started["ok"])
        self.assertTrue(started["running"])
        self.assertEqual(started["streamKeyStatus"], "memory-only-redacted")
        self.assertNotIn(stream_key, json.dumps(started))
        ingest.wait_for_bytes()

        stopped = rpc(sock, 252, "output.stop", {"outputId": "rtmp-main"})["result"]
        self.assertTrue(stopped["ok"])
        self.assertFalse(stopped["running"])
        self.assertEqual(stopped["streamKeyStatus"], "cleared")

    def test_sigkill_leaves_valid_state_file(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            state_file = Path(temp_dir) / "host-state.json"
            process, _, _ = start_host(state_file=state_file)
            os.kill(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
            state = json.loads(state_file.read_text())
            self.assertEqual(state["status"], "ready")
            self.assertEqual(state["hostId"], "studio-host-1")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
