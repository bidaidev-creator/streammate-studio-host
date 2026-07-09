#!/usr/bin/env python3
"""Per-finding security regression tests for the OBS config import path.

Chunk 37.H1 (native-host live demo program, security lane). One named
regression test per finding, L1-A pattern, proving the studio-host #9
remediation holds at head:

- PR07-07 (prototype review 2026-07-07, src/studio_host.cpp
  copy_config_without_service_json): copy_file used to dereference
  symlinks, so a symlink planted inside an imported OBS config directory
  copied an arbitrary target file's content into the import output. The
  fix skips every symlink entry (file and directory) during the
  recursive copy. A regression re-introducing dereferencing copy makes
  test_pr07_07_* fail on the "symlink target content in output" asserts.

- PR07-08 (same review, parse_obs_source_entries): a backtracking
  std::regex with a lazy gap gave quadratic blow-up on brace-less
  hostile scene-collection input (88 KB -> ~16 s) on the RPC thread,
  and imports had no size cap. The fix is a brace-aware single-pass
  scanner plus kMaxSceneCollectionBytes (8 MiB) enforced at read time.
  A regression makes test_pr07_08_* fail either the elapsed-time bound
  (scanner) or the -32602 size-limit refusal (cap).

- NHR-01 (native-host IPC review 2026-07-07, contested only because the
  host C++ source was outside that review's worktree): the combined
  hostile-config class - symlinks plus pathological scene-collection
  content in one import. test_nhr_01_* proves at the host repo's head
  that such a config imports with symlinks skipped, bounded parse time,
  intact output, and a host that stays serviceable afterwards.

Finding-register status flips remain owner-ratification-pending; this
file only proves behavior at head.
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

# Reuses the lifecycle harness (start_host/rpc/websocket helpers); both files
# are invoked as `python3 <file> <path-to-studio-host>` so HOST_BIN resolves
# identically in the imported module.
import test_host_lifecycle as lifecycle


def start_connected_host(env: dict[str, str] | None = None):
    process, port, _ = lifecycle.start_host(env=env)
    sock = lifecycle.websocket_connect(port)
    return process, sock


def write_minimal_collection(obs_dir: Path, body: str, name: str = "fixture-main") -> Path:
    scenes_dir = obs_dir / "basic" / "scenes"
    scenes_dir.mkdir(parents=True, exist_ok=True)
    collection = scenes_dir / f"{name}.json"
    collection.write_text(body, encoding="utf-8")
    return collection


class Pr0707SymlinkExfilRegressionTest(unittest.TestCase):
    """PR07-07: symlinks inside an imported OBS config dir are never followed."""

    @unittest.skipUnless(hasattr(os, "symlink"), "symlink support is required")
    def test_pr07_07_symlinked_file_and_directory_never_copied(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            obs_dir = lifecycle.write_obs_fixture(temp_root)
            streammate_home = temp_root / "streammate-home"

            outside = temp_root / "outside"
            outside.mkdir()
            secret = "pr07-07-outside-secret-material"
            secret_file = outside / "credential.txt"
            secret_file.write_text(secret, encoding="utf-8")
            outside_dir = outside / "keys"
            outside_dir.mkdir()
            (outside_dir / "nested-credential.txt").write_text(secret, encoding="utf-8")

            # A benign regular file that MUST still be copied (the fix skips
            # symlinks, not regular content).
            (obs_dir / "global.ini").write_text("[General]\nName=Fixture\n", encoding="utf-8")
            try:
                # File symlink at the config root, file symlink nested under
                # basic/scenes, and a directory symlink.
                os.symlink(secret_file, obs_dir / "innocent-name.json")
                os.symlink(secret_file, obs_dir / "basic" / "scenes" / "linked-scene.json")
                os.symlink(outside_dir, obs_dir / "linked-profile-dir")
            except OSError as exc:
                self.skipTest(f"symlink creation unavailable: {exc}")

            process, sock = start_connected_host(
                env={
                    "STREAMMATE_OBS_CONFIG_DIR": str(obs_dir),
                    "STREAMMATE_HOME": str(streammate_home),
                }
            )
            self.addCleanup(lifecycle.stop_process, process)
            self.addCleanup(sock.close)

            loaded = lifecycle.rpc(sock, 3701, "import.load", {"collectionId": "fixture-main"})
            self.assertIn("result", loaded)

            imported_root = streammate_home / "studio" / "obs-imports" / "fixture-main"
            # Regular content copied; every symlink entry absent by path.
            self.assertEqual(
                (imported_root / "global.ini").read_text(encoding="utf-8"),
                "[General]\nName=Fixture\n",
            )
            self.assertFalse((imported_root / "innocent-name.json").exists())
            self.assertFalse((imported_root / "basic" / "scenes" / "linked-scene.json").exists())
            self.assertFalse((imported_root / "linked-profile-dir").exists())
            # service.json exclusion is unchanged by the hardening.
            self.assertFalse((imported_root / "service.json").exists())
            # The regression signature: symlink-target bytes anywhere in the
            # import output tree or in the RPC response.
            for path in imported_root.rglob("*"):
                if path.is_file():
                    self.assertNotIn(secret, path.read_text(encoding="utf-8", errors="replace"))
            self.assertNotIn(secret, json.dumps(loaded))


class Pr0708ParseRedosRegressionTest(unittest.TestCase):
    """PR07-08: scene-collection parsing is single-pass and size-capped."""

    def test_pr07_08_braceless_hostile_collection_scans_promptly(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            obs_dir = Path(temp_dir) / "obs-config"
            # ~400 KB brace-free body: quadratic backtracking (the pre-fix
            # behavior blew past 16 s at 88 KB) cannot survive the bound below;
            # the single-pass scanner finishes in milliseconds.
            write_minimal_collection(
                obs_dir,
                '"name":"Brace Free",' + ('"padding":"xxxxxxxxxxxxxxxxxxxx",' * 16000),
            )

            process, sock = start_connected_host()
            self.addCleanup(lifecycle.stop_process, process)
            self.addCleanup(sock.close)

            started = time.monotonic()
            scan = lifecycle.rpc(sock, 3801, "import.scan", {"configDir": str(obs_dir)})
            elapsed = time.monotonic() - started
            self.assertLess(elapsed, 2.0)
            self.assertIn("result", scan)
            self.assertEqual(scan["result"]["collections"][0]["name"], "Brace Free")
            self.assertEqual(scan["result"]["collections"][0]["sourceCount"], 0)

    def test_pr07_08_oversized_scene_collection_refused_with_size_limit_error(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            obs_dir = temp_root / "obs-config"
            # One byte past kMaxSceneCollectionBytes (8 MiB).
            oversized = '{"name":"Too Big","padding":"' + ("x" * (8 * 1024 * 1024 - 28)) + '"}'
            write_minimal_collection(obs_dir, oversized)

            process, sock = start_connected_host()
            self.addCleanup(lifecycle.stop_process, process)
            self.addCleanup(sock.close)

            scan = lifecycle.rpc(sock, 3802, "import.scan", {"configDir": str(obs_dir)})
            self.assertEqual(scan["error"]["code"], -32602)
            self.assertIn("size limit", scan["error"]["message"])

            loaded = lifecycle.rpc(
                sock,
                3803,
                "import.load",
                {
                    "configDir": str(obs_dir),
                    "streammateHome": str(temp_root / "streammate-home"),
                    "collectionId": "fixture-main",
                },
            )
            self.assertEqual(loaded["error"]["code"], -32602)
            self.assertIn("size limit", loaded["error"]["message"])
            # Nothing was imported by the refused load.
            self.assertFalse((temp_root / "streammate-home" / "studio").exists())


class Nhr01HostileConfigRegressionTest(unittest.TestCase):
    """NHR-01: the combined hostile-config class (symlink + pathological
    scene-collection) is dead at head - previously contested because the host
    source was outside the monorepo review's worktree."""

    @unittest.skipUnless(hasattr(os, "symlink"), "symlink support is required")
    def test_nhr_01_combined_hostile_config_imports_bounded_with_symlinks_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            obs_dir = temp_root / "obs-config"
            streammate_home = temp_root / "streammate-home"
            # Pathological content: a hostile name full of regex metacharacters
            # plus a large brace-free tail, in one loadable collection.
            hostile_name = "((((a+)+)+)+)$" + ".*" * 32
            write_minimal_collection(
                obs_dir,
                json.dumps({"name": hostile_name, "sources": [{"name": "Backdrop", "id": "color_source"}]})[:-1]
                + ","
                + ('"padding":"xxxxxxxxxxxxxxxxxxxx",' * 8000)[:-1]
                + "}",
            )

            outside = temp_root / "outside"
            outside.mkdir()
            secret = "nhr-01-outside-secret-material"
            (outside / "loot.txt").write_text(secret, encoding="utf-8")
            try:
                os.symlink(outside / "loot.txt", obs_dir / "basic" / "scenes" / "loot-link.json")
                os.symlink(outside, obs_dir / "loot-dir")
            except OSError as exc:
                self.skipTest(f"symlink creation unavailable: {exc}")

            process, sock = start_connected_host(
                env={
                    "STREAMMATE_OBS_CONFIG_DIR": str(obs_dir),
                    "STREAMMATE_HOME": str(streammate_home),
                }
            )
            self.addCleanup(lifecycle.stop_process, process)
            self.addCleanup(sock.close)

            started = time.monotonic()
            loaded = lifecycle.rpc(sock, 3901, "import.load", {"collectionId": "fixture-main"})
            elapsed = time.monotonic() - started
            self.assertLess(elapsed, 2.0)
            self.assertIn("result", loaded)

            imported_root = streammate_home / "studio" / "obs-imports" / "fixture-main"
            self.assertFalse((imported_root / "basic" / "scenes" / "loot-link.json").exists())
            self.assertFalse((imported_root / "loot-dir").exists())
            for path in imported_root.rglob("*"):
                if path.is_file():
                    self.assertNotIn(secret, path.read_text(encoding="utf-8", errors="replace"))
            self.assertNotIn(secret, json.dumps(loaded))

            # The host stays serviceable after digesting the hostile config.
            report = lifecycle.rpc(sock, 3902, "import.report", {"collectionId": "fixture-main"})
            self.assertIn("result", report)
            self.assertNotIn(secret, json.dumps(report))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
