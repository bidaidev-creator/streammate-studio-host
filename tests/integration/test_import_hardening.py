#!/usr/bin/env python3
"""Per-finding security regression tests for the OBS config import path.

Chunk 37.H1 (native-host live demo program, security lane). One named
regression test per finding, L1-A pattern, proving the studio-host #9
remediation holds at head. Every assertion here was calibrated against a
faithfully-reverted binary (the exact #9 hunks reverted in the head
source) so the tests FAIL against the vulnerable code and PASS at head -
see the PR body's revert-experiment table.

- PR07-07 (prototype review 2026-07-07, src/studio_host.cpp
  copy_config_without_service_json): copy_file dereferenced symlinks, so
  a symlink planted inside an imported OBS config directory copied an
  arbitrary target file's content into the import output. Under the
  vulnerable code std::filesystem::relative resolves the symlink and the
  secret lands OUTSIDE the collection directory (a sibling under
  studio/obs-imports/), so a scan confined to the collection dir misses
  it. These tests scan the whole STREAMMATE_HOME for the canary and
  assert obs-imports/ contains ONLY the collection directory.

- PR07-08 (same review, parse_obs_source_entries + read cap): a
  backtracking std::regex with a lazy "name"..gap.."id" pattern gave
  catastrophic blow-up on input with many "name" anchors and no matching
  "id" (a single anchor does NOT trigger it), and imports had no size
  cap. The fix is a brace-aware single-pass scanner plus
  kMaxSceneCollectionBytes (8 MiB). The timing test uses a many-anchor,
  no-id body (head ~0.00s; reverted >7s) with a comfortable 2.0s bound;
  the cap test asserts a >8 MiB collection is refused.

- NHR-01 (native-host IPC review 2026-07-07, contested only because the
  host C++ source was outside that review's worktree): the combined
  hostile-config class - a genuinely pathological (many-anchor, no-id)
  scene-collection AND planted file/dir symlinks in one import.

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

CANARY = "nhr-37h1-outside-secret-material"

# Many "name" anchors with NO matching "id": the shape that drives the pre-fix
# backtracking regex quadratic. Head single-pass scanner handles it in ~0.00s;
# the reverted binary exceeds 7s. Kept well above the 2.0s bound either way.
PATHOLOGICAL_ANCHORS = 1500


def pathological_collection_body(name: str = "Hostile") -> str:
    anchors = ',"name":"YYYYYYYYYY"' * PATHOLOGICAL_ANCHORS
    return '{"name":"' + name + '"' + anchors + "}"


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


def home_contains_canary(streammate_home: Path, canary: str) -> list[str]:
    """Scan the ENTIRE STREAMMATE_HOME (not just the collection dir) for the
    canary content. The vulnerable copy lands the secret at a sibling of the
    collection directory, so a collection-scoped scan misses it."""
    hits: list[str] = []
    for path in streammate_home.rglob("*"):
        if path.is_file():
            try:
                if canary in path.read_text(encoding="utf-8", errors="replace"):
                    hits.append(str(path.relative_to(streammate_home)))
            except OSError:
                continue
    return hits


def obs_imports_children(streammate_home: Path) -> list[str]:
    imports = streammate_home / "studio" / "obs-imports"
    if not imports.is_dir():
        return []
    return sorted(child.name for child in imports.iterdir())


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
            secret_file = outside / "credential.txt"
            secret_file.write_text(CANARY, encoding="utf-8")
            outside_dir = outside / "keys"
            outside_dir.mkdir()
            (outside_dir / "nested-credential.txt").write_text(CANARY, encoding="utf-8")

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
            self.assertFalse((imported_root / "service.json").exists())

            # Teeth against the real defect: the vulnerable copy dereferences
            # symlinks and lands the target under a SIBLING of the collection
            # dir. Assert obs-imports/ holds only the collection, and scan the
            # whole home for the canary bytes.
            self.assertEqual(obs_imports_children(streammate_home), ["fixture-main"])
            self.assertEqual(home_contains_canary(streammate_home, CANARY), [])
            self.assertNotIn(CANARY, json.dumps(loaded))


class Pr0708ParseRedosRegressionTest(unittest.TestCase):
    """PR07-08: scene-collection parsing is single-pass and size-capped."""

    def test_pr07_08_multi_anchor_hostile_collection_scans_promptly(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            obs_dir = Path(temp_dir) / "obs-config"
            # Many "name" anchors, no matching "id": the pattern that makes the
            # pre-fix backtracking regex blow up (a single anchor does not).
            write_minimal_collection(obs_dir, pathological_collection_body("Brace Free"))

            process, sock = start_connected_host()
            self.addCleanup(lifecycle.stop_process, process)
            self.addCleanup(sock.close)

            started = time.monotonic()
            scan = lifecycle.rpc(sock, 3801, "import.scan", {"configDir": str(obs_dir)})
            elapsed = time.monotonic() - started
            # Head: ~0.00s. Reverted: >7s (rpc recv times out). 2.0s bound has
            # a >30x margin at head yet fails hard against the vulnerable parser.
            self.assertLess(elapsed, 2.0)
            self.assertIn("result", scan)
            self.assertEqual(scan["result"]["collections"][0]["name"], "Brace Free")
            self.assertEqual(scan["result"]["collections"][0]["sourceCount"], 0)

    def test_pr07_08_oversized_scene_collection_refused_with_size_limit_error(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            obs_dir = temp_root / "obs-config"
            # One object just past kMaxSceneCollectionBytes (8 MiB).
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
    """NHR-01: the combined hostile-config class (pathological scene-collection
    + planted symlinks) is dead at head - previously contested because the host
    source was outside the monorepo review's worktree."""

    @unittest.skipUnless(hasattr(os, "symlink"), "symlink support is required")
    def test_nhr_01_combined_hostile_config_imports_bounded_with_symlinks_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            obs_dir = temp_root / "obs-config"
            streammate_home = temp_root / "streammate-home"
            # Genuinely pathological: many "name" anchors, no matching "id".
            write_minimal_collection(obs_dir, pathological_collection_body("Hostile"))

            outside = temp_root / "outside"
            outside.mkdir()
            (outside / "loot.txt").write_text(CANARY, encoding="utf-8")
            outside_dir = outside / "loot-keys"
            outside_dir.mkdir()
            (outside_dir / "nested.txt").write_text(CANARY, encoding="utf-8")
            try:
                os.symlink(outside / "loot.txt", obs_dir / "basic" / "scenes" / "loot-link.json")
                os.symlink(outside_dir, obs_dir / "loot-dir")
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
            # Bounded parse (reverted parser exceeds 7s on this input).
            self.assertLess(elapsed, 2.0)
            self.assertIn("result", loaded)

            imported_root = streammate_home / "studio" / "obs-imports" / "fixture-main"
            self.assertFalse((imported_root / "basic" / "scenes" / "loot-link.json").exists())
            self.assertFalse((imported_root / "loot-dir").exists())
            # Whole-home canary scan + obs-imports has only the collection dir.
            self.assertEqual(obs_imports_children(streammate_home), ["fixture-main"])
            self.assertEqual(home_contains_canary(streammate_home, CANARY), [])
            self.assertNotIn(CANARY, json.dumps(loaded))

            # The host stays serviceable after digesting the hostile config.
            report = lifecycle.rpc(sock, 3902, "import.report", {"collectionId": "fixture-main"})
            self.assertIn("result", report)
            self.assertNotIn(CANARY, json.dumps(report))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
