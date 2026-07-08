#!/usr/bin/env python3
"""Spec 34 chunk 34.H4 — replay buffer + local recording outputs.

Scaffold-mode proof: synthetic recording/replay files under the host state
directory (STREAMMATE_HOME), loopback-only, honest status transitions. The
HAS_LIBOBS ffmpeg_muxer/replay_buffer wiring is asserted at the source level
(compile-proof): even in the CI libobs lane, which does build studio-host,
obs_output_create requires a fully initialised obs runtime context that this
lane does not stand up, so output creation itself is not exercised by a smoke.
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# Reuse the lifecycle harness helpers. Importing binds HOST_BIN from sys.argv[1],
# which is the same host binary path this test file is invoked with.
import test_host_lifecycle as host

SOURCE = Path(__file__).resolve().parents[2] / "src" / "studio_host.cpp"
WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "macos-ci.yml"


def host_tcp_peers(pid: int) -> list[str]:
    """Every remote TCP endpoint the host process currently holds open."""
    # `-a` ANDs the -p and -iTCP selectors; without it lsof ORs them and returns
    # every TCP connection on the host, not just this process's.
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
        # NAME looks like 127.0.0.1:51000->127.0.0.1:51001
        match = re.search(r"->(.+)$", name)
        if match:
            peers.append(match.group(1))
    return peers


def is_loopback(peer: str) -> bool:
    host_part = peer.rsplit(":", 1)[0].strip("[]")
    return host_part in ("127.0.0.1", "::1", "localhost")


class RecordReplayScaffoldTest(unittest.TestCase):
    def _connect(self, home: Path):
        process, port, _ = host.start_host(env={"STREAMMATE_HOME": str(home)})
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)
        return process, sock

    def test_record_start_stop_produces_file_and_status_transitions(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            home = Path(temp_dir) / "streammate-home"
            _, sock = self._connect(home)

            idle = host.rpc(sock, 300, "record.status", {"recordId": "session"})["result"]
            self.assertFalse(idle["running"])
            self.assertEqual(idle["status"], "idle")

            started = host.rpc(sock, 301, "record.start", {"recordId": "session"})["result"]
            self.assertTrue(started["ok"])
            self.assertTrue(started["running"])
            self.assertEqual(started["status"], "recording")
            self.assertEqual(started["recordId"], "session")
            self.assertEqual(started["path"], "$STREAMMATE_HOME/studio/recordings/session.mkv")

            recording = host.rpc(sock, 302, "record.status", {"recordId": "session"})["result"]
            self.assertTrue(recording["running"])
            self.assertEqual(recording["status"], "recording")

            stopped = host.rpc(sock, 303, "record.stop", {"recordId": "session"})["result"]
            self.assertTrue(stopped["ok"])
            self.assertFalse(stopped["running"])
            self.assertEqual(stopped["status"], "stopped")
            self.assertEqual(stopped["path"], "$STREAMMATE_HOME/studio/recordings/session.mkv")
            self.assertGreater(stopped["bytes"], 0)
            self.assertEqual(stopped["frameCount"], 3)

            after = host.rpc(sock, 304, "record.status", {"recordId": "session"})["result"]
            self.assertFalse(after["running"])
            self.assertEqual(after["status"], "stopped")

            recording_file = home / "studio" / "recordings" / "session.mkv"
            self.assertTrue(recording_file.exists())
            content = recording_file.read_text(encoding="utf-8")
            self.assertTrue(content.startswith("STMREC1\n"))
            self.assertIn("recordId=session\n", content)
            self.assertIn("STMREC-END frames=3\n", content)
            self.assertEqual(content.count("frame:"), 3)

            # No absolute host path leaks in any response.
            for response in (started, stopped, recording, after):
                serialized = json.dumps(response)
                self.assertNotIn(str(home), serialized)
                self.assertNotIn("/Users/", serialized)

    def test_record_started_and_stopped_events_are_journaled(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            home = Path(temp_dir) / "streammate-home"
            _, sock = self._connect(home)

            host.rpc(sock, 310, "record.start", {"recordId": "evented"})
            started_event = self._await_event(sock, "record.started")
            self.assertEqual(started_event["payload"]["recordId"], "evented")
            self.assertEqual(started_event["payload"]["status"], "recording")
            self.assertEqual(started_event["payload"]["path"], "$STREAMMATE_HOME/studio/recordings/evented.mkv")

            host.rpc(sock, 311, "record.stop", {"recordId": "evented"})
            stopped_event = self._await_event(sock, "record.stopped")
            self.assertEqual(stopped_event["payload"]["recordId"], "evented")
            self.assertEqual(stopped_event["payload"]["status"], "stopped")

    def test_replay_start_save_stop_materializes_synthetic_chunks(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            home = Path(temp_dir) / "streammate-home"
            _, sock = self._connect(home)

            started = host.rpc(sock, 320, "replay.start", {"replayId": "clip"})["result"]
            self.assertTrue(started["ok"])
            self.assertTrue(started["running"])
            self.assertEqual(started["status"], "buffering")
            self.assertEqual(started["replayId"], "clip")

            buffering = host.rpc(sock, 321, "replay.status", {"replayId": "clip"})["result"]
            self.assertTrue(buffering["running"])
            self.assertEqual(buffering["status"], "buffering")

            saved = host.rpc(sock, 322, "replay.save", {"replayId": "clip"})["result"]
            self.assertTrue(saved["ok"])
            self.assertEqual(saved["status"], "saved")
            self.assertEqual(saved["chunkCount"], 4)
            self.assertGreater(saved["savedBytes"], 0)
            self.assertEqual(saved["path"], "$STREAMMATE_HOME/studio/replays/clip-1.mkv")

            saved_event = self._await_event(sock, "replay.saved")
            self.assertEqual(saved_event["payload"]["replayId"], "clip")
            self.assertEqual(saved_event["payload"]["path"], "$STREAMMATE_HOME/studio/replays/clip-1.mkv")

            replay_file = home / "studio" / "replays" / "clip-1.mkv"
            self.assertTrue(replay_file.exists())
            content = replay_file.read_text(encoding="utf-8")
            self.assertTrue(content.startswith("STMREPLAY1\n"))
            for index in range(4):
                self.assertIn(f"STMREPLAY-CHUNK-{index}\n", content)

            # A second save materializes a new segment file without losing the buffer.
            saved_again = host.rpc(sock, 323, "replay.save", {"replayId": "clip"})["result"]
            self.assertEqual(saved_again["path"], "$STREAMMATE_HOME/studio/replays/clip-2.mkv")
            self.assertTrue((home / "studio" / "replays" / "clip-2.mkv").exists())

            stopped = host.rpc(sock, 324, "replay.stop", {"replayId": "clip"})["result"]
            self.assertTrue(stopped["ok"])
            self.assertFalse(stopped["running"])
            self.assertEqual(stopped["status"], "stopped")

            # The buffer is cleared: a save after stop fails with a named error.
            stale = host.rpc(sock, 325, "replay.save", {"replayId": "clip"})
            self.assertEqual(stale["error"]["code"], -32602)
            self.assertIn("replay buffer is not running", stale["error"]["message"])

    def test_record_and_replay_destinations_are_confined_and_relative(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            home = Path(temp_dir) / "streammate-home"
            _, sock = self._connect(home)

            for bad in ["/etc/passwd.mkv", "../escape.mkv", "sub/../../escape.mkv", "rtmp://live.example.invalid/app", "https://x/y.mkv"]:
                rejected = host.rpc(sock, 330, "record.start", {"recordId": "confine", "destination": bad})
                self.assertEqual(rejected["error"]["code"], -32602, bad)
                self.assertIn("relative path under the state directory", rejected["error"]["message"], bad)
                self.assertNotIn(str(home), json.dumps(rejected))

            replay_rejected = host.rpc(sock, 331, "replay.start", {"replayId": "confine", "destination": "/tmp/evil.mkv"})
            self.assertEqual(replay_rejected["error"]["code"], -32602)
            self.assertIn("relative path under the state directory", replay_rejected["error"]["message"])

            # None of the refused destinations created any file anywhere.
            self.assertFalse((home / "studio" / "recordings").exists() and any((home / "studio" / "recordings").iterdir()))
            self.assertFalse(Path("/etc/passwd.mkv").exists())
            self.assertFalse(Path("/tmp/evil.mkv").exists())

    def test_request_cannot_override_env_streammate_home(self) -> None:
        # A caller-supplied streammateHome must not redirect the write: home comes
        # only from the process environment, so the "$STREAMMATE_HOME" label is honest.
        with tempfile.TemporaryDirectory() as temp_dir:
            env_home = Path(temp_dir) / "env-home"
            other_home = Path(temp_dir) / "attacker-home"
            other_home.mkdir(parents=True)
            _, sock = self._connect(env_home)

            started = host.rpc(sock, 360, "record.start", {"recordId": "s", "streammateHome": str(other_home)})["result"]
            self.assertTrue(started["ok"])
            self.assertEqual(started["path"], "$STREAMMATE_HOME/studio/recordings/s.mkv")
            host.rpc(sock, 361, "record.stop", {"recordId": "s"})

            self.assertTrue((env_home / "studio" / "recordings" / "s.mkv").exists())
            self.assertFalse((other_home / "studio").exists())

    @unittest.skipUnless(hasattr(__import__("os"), "symlink"), "symlink support is required")
    def test_symlinked_state_dir_is_refused_for_record(self) -> None:
        import os

        with tempfile.TemporaryDirectory() as temp_dir:
            home = Path(temp_dir) / "home"
            outside = Path(temp_dir) / "outside"
            (home).mkdir(parents=True)
            outside.mkdir(parents=True)
            os.symlink(outside, home / "studio")  # studio -> outside (escape)
            _, sock = self._connect(home)

            refused = host.rpc(sock, 370, "record.start", {"recordId": "esc"})
            self.assertEqual(refused["error"]["code"], -32602)
            self.assertIn("relative path under the state directory", refused["error"]["message"])
            self.assertFalse((outside / "recordings" / "esc.mkv").exists())

    @unittest.skipUnless(hasattr(__import__("os"), "symlink"), "symlink support is required")
    def test_replay_save_revalidates_containment_at_write_time(self) -> None:
        import os

        with tempfile.TemporaryDirectory() as temp_dir:
            home = Path(temp_dir) / "home"
            outside = Path(temp_dir) / "outside"
            outside.mkdir(parents=True)
            _, sock = self._connect(home)

            host.rpc(sock, 380, "replay.start", {"replayId": "clip"})
            # Plant a replays -> outside symlink AFTER start; save must still refuse.
            (home / "studio").mkdir(parents=True, exist_ok=True)
            os.symlink(outside, home / "studio" / "replays")

            refused = host.rpc(sock, 381, "replay.save", {"replayId": "clip"})
            self.assertEqual(refused["error"]["code"], -32602)
            self.assertIn("relative path under the state directory", refused["error"]["message"])
            self.assertFalse((outside / "clip-1.mkv").exists())

    def test_record_requires_streammate_home(self) -> None:
        process, port, _ = host.start_host()  # no STREAMMATE_HOME in env
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)

        refused = host.rpc(sock, 340, "record.start", {"recordId": "no-home"})
        self.assertEqual(refused["error"]["code"], -32602)
        self.assertIn("STREAMMATE_HOME is required", refused["error"]["message"])

    def test_no_non_loopback_socket_opens_during_record_replay(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            home = Path(temp_dir) / "streammate-home"
            process, sock = self._connect(home)

            host.rpc(sock, 350, "record.start", {"recordId": "probe"})
            host.rpc(sock, 351, "replay.start", {"replayId": "probe"})
            host.rpc(sock, 352, "replay.save", {"replayId": "probe"})
            host.rpc(sock, 353, "record.stop", {"recordId": "probe"})
            host.rpc(sock, 354, "replay.stop", {"replayId": "probe"})

            peers = host_tcp_peers(process.pid)
            self.assertTrue(peers, "expected at least the loopback control connection")
            for peer in peers:
                self.assertTrue(is_loopback(peer), f"non-loopback TCP peer opened during record/replay: {peer}")

    def test_has_libobs_ffmpeg_muxer_and_replay_buffer_wiring_present(self) -> None:
        # Compile-proof: the real path wires ffmpeg_muxer (recording) and
        # replay_buffer (replay) under the STREAMMATE_HAS_LIBOBS guard.
        source = SOURCE.read_text(encoding="utf-8")
        self.assertIn("STREAMMATE_HAS_LIBOBS", source)
        self.assertIn('obs_output_create("ffmpeg_muxer"', source)
        self.assertIn('obs_output_create("replay_buffer"', source)

        # The libobs lane builds studio-host (which carries the wiring above)
        # with no `|| true`, so a compile failure fails the job.
        workflow = WORKFLOW.read_text(encoding="utf-8")
        start = workflow.index("- name: Build obs_startup smoke and studio-host")
        end = workflow.index("- name: Run obs_startup smoke headlessly")
        build_step = workflow[start:end]
        self.assertIn("studio-host", build_step)
        self.assertNotIn("|| true", build_step)

    def _await_event(self, sock, event_type: str, timeout: float = 5.0) -> dict:
        import time

        deadline = time.time() + timeout
        while time.time() < deadline:
            message = host.recv_text(sock, timeout=1)
            if message.get("type") == event_type:
                return message
        raise AssertionError(f"did not observe event {event_type}")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
