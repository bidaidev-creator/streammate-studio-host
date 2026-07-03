#!/usr/bin/env python3
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


def start_host(*extra: str, state_file: Path | None = None) -> tuple[subprocess.Popen[str], int, list[str]]:
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
    if len(raw) >= 126:
        raise AssertionError("test payload too large")
    header = bytes([0x81, 0x80 | len(raw)])
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(raw))
    sock.sendall(header + mask + masked)


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


class StudioHostLifecycleTest(unittest.TestCase):
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
