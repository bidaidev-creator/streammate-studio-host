#!/usr/bin/env python3
"""NIF-H1 (host): user plugin discovery + static probe (no module loading).

`plugins.discover` enumerates candidate OBS user modules from the roots named
in a `--user-plugins-manifest <absolute path>` file (schema
user-plugins-manifest.v1), and computes a STATIC record per candidate: sha256
of the binary bytes plus the Mach-O architecture list read straight from the
header bytes. It must NEVER load anything (no dlopen / obs_open_module --
loading is chunk NIF-H2), enumeration must be read-only (roots byte-for-byte
identical afterwards) and deterministic, and a malformed manifest is refused
at launch with a SANITIZED reason that never contains the raw filesystem path.

Both macOS layouts are inventoried:
  (a) <root>/<name>.plugin CFBundle -> binary at Contents/MacOS/<name>
  (b) legacy <root>/<name>/bin/**/<name>.{so,dylib}
"""
from __future__ import annotations

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import test_host_lifecycle as host

HOST_BIN = host.HOST_BIN

# The host's launch-refusal convention: parse_args throws, main() logs
# host.exited and returns kUsageExit. (This repo's "exit-code-2 semantics".)
USAGE_EXIT = 64

CPU_TYPE_X86_64 = 0x01000007
CPU_TYPE_ARM64 = 0x0100000C


def thin_macho64(cputype: int) -> bytes:
    """A minimal little-endian 64-bit Mach-O header (magic MH_MAGIC_64),
    enough for a header-bytes arch probe; not a loadable module."""
    # mach_header_64: magic cputype cpusubtype filetype ncmds sizeofcmds flags reserved
    return struct.pack("<8I", 0xFEEDFACF, cputype, 0, 6, 0, 0, 0, 0) + b"\x00" * 24


def fat_macho(cputypes: list[int]) -> bytes:
    """A minimal universal (FAT_MAGIC, big-endian) binary wrapping thin
    headers for each cputype, in the given slice order."""
    header = struct.pack(">II", 0xCAFEBABE, len(cputypes))
    entries = b""
    blobs = b""
    offset = 4096
    for cputype in cputypes:
        thin = thin_macho64(cputype)
        entries += struct.pack(">5I", cputype, 0, offset, len(thin), 12)
        blobs += thin + b"\x00" * (4096 - len(thin))
        offset += 4096
    prefix = header + entries
    return prefix + b"\x00" * (4096 - len(prefix)) + blobs


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_digest(root: Path) -> dict[str, str]:
    """Byte-level snapshot of a fixture root (names + content hashes)."""
    out: dict[str, str] = {}
    for path in sorted(root.rglob("*")):
        rel = str(path.relative_to(root))
        out[rel] = "<dir>" if path.is_dir() else hashlib.sha256(path.read_bytes()).hexdigest()
    return out


def build_bundle(root: Path, name: str, binary: bytes) -> Path:
    macos = root / f"{name}.plugin" / "Contents" / "MacOS"
    macos.mkdir(parents=True)
    target = macos / name
    target.write_bytes(binary)
    return target


def build_legacy(root: Path, name: str, binary: bytes, subdir: str = "") -> Path:
    bin_dir = root / name / "bin"
    if subdir:
        bin_dir = bin_dir / subdir
    bin_dir.mkdir(parents=True)
    target = bin_dir / f"{name}.so"
    target.write_bytes(binary)
    return target


class PluginDiscoveryTest(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.root0 = self.base / "root0"
        self.root1 = self.base / "root1"
        self.root0.mkdir()
        self.root1.mkdir()

        # root0: a REAL Mach-O (the host binary itself) in a CFBundle layout,
        # an excluded fabricated arm64 bundle, a nested legacy universal
        # module, and an unsafe-named bundle that must be skipped.
        self.alpha_bin = build_bundle(self.root0, "alpha", HOST_BIN.read_bytes())
        self.excluded_bin = build_bundle(self.root0, "excluded-mod", thin_macho64(CPU_TYPE_ARM64))
        self.legacy_bin = build_legacy(
            self.root0, "legacy-mod", fat_macho([CPU_TYPE_X86_64, CPU_TYPE_ARM64]), subdir="nested"
        )
        build_bundle(self.root0, "bad name", thin_macho64(CPU_TYPE_ARM64))

        # root1: a duplicate moduleRef (first-root-wins) and a wrong-arch
        # (x86_64-only) candidate fabricated from a thin header.
        self.alpha_dup_bin = build_bundle(self.root1, "alpha", thin_macho64(CPU_TYPE_X86_64))
        self.intel_bin = build_bundle(self.root1, "intel-only", thin_macho64(CPU_TYPE_X86_64))

        self.manifest = self.base / "manifest.json"
        self.manifest.write_text(
            json.dumps(
                {
                    "version": 1,
                    "roots": [
                        {"binaryDir": str(self.root0), "dataDir": None},
                        {"binaryDir": str(self.root1)},
                    ],
                    "selected": None,
                    "exclude": ["module:excluded-mod"],
                }
            )
        )

    # -- helpers ---------------------------------------------------------

    def _connect(self, *extra: str):
        process, port, _ = host.start_host(*extra)
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)
        return sock

    def _refusal(self, manifest_arg: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(HOST_BIN), "--token", host.TOKEN, "--port", "0",
             "--user-plugins-manifest", manifest_arg],
            capture_output=True,
            text=True,
            timeout=15,
        )

    # -- inventory -------------------------------------------------------

    def test_discover_inventories_both_layouts_read_only(self) -> None:
        before0 = tree_digest(self.root0)
        before1 = tree_digest(self.root1)

        sock = self._connect("--user-plugins-manifest", str(self.manifest))
        first = host.rpc(sock, 1, "plugins.discover", {})["result"]
        second = host.rpc(sock, 2, "plugins.discover", {})["result"]

        # Deterministic: byte-identical result across calls.
        self.assertEqual(first, second)

        self.assertIs(first["ok"], True)
        self.assertIn(first["mode"], ("scaffold", "libobs"))
        self.assertEqual(
            first["roots"],
            [
                {"rootRef": "root:0", "candidateCount": 3},
                {"rootRef": "root:1", "candidateCount": 2},
            ],
        )

        modules = first["modules"]
        order = [(m["moduleRef"], m["rootRef"]) for m in modules]
        self.assertEqual(
            order,
            [
                ("module:alpha", "root:0"),
                ("module:excluded-mod", "root:0"),
                ("module:legacy-mod", "root:0"),
                ("module:alpha", "root:1"),
                ("module:intel-only", "root:1"),
            ],
        )

        by_key = {(m["moduleRef"], m["rootRef"]): m for m in modules}

        # Static-probe honesty: arch was read from header bytes; dependencies
        # were NOT probed in this chunk; nothing was loaded.
        for module in modules:
            self.assertEqual(module["moduleClass"], "user")
            self.assertIsNone(module["state"])
            self.assertEqual(module["observed"], {"arch": True, "dependencies": False})
            self.assertIsNone(module["reasonDetail"])

        alpha = by_key[("module:alpha", "root:0")]
        self.assertEqual(alpha["label"], "alpha")
        self.assertEqual(alpha["fileName"], "alpha.plugin/Contents/MacOS/alpha")
        self.assertEqual(alpha["lifecycle"], "discovered")
        self.assertEqual(alpha["sha256"], sha256_of(self.alpha_bin))
        # The host binary is a real Mach-O; arch must be non-empty and known.
        self.assertTrue(alpha["arch"])
        self.assertTrue(set(alpha["arch"]) <= {"arm64", "x86_64"})

        excluded = by_key[("module:excluded-mod", "root:0")]
        self.assertEqual(excluded["lifecycle"], "excluded")
        self.assertEqual(excluded["arch"], ["arm64"])
        self.assertEqual(excluded["sha256"], sha256_of(self.excluded_bin))

        legacy = by_key[("module:legacy-mod", "root:0")]
        self.assertEqual(legacy["lifecycle"], "discovered")
        self.assertEqual(legacy["fileName"], "legacy-mod/bin/nested/legacy-mod.so")
        # Universal binary: slices reported in fat-file order.
        self.assertEqual(legacy["arch"], ["x86_64", "arm64"])
        self.assertEqual(legacy["sha256"], sha256_of(self.legacy_bin))

        duplicate = by_key[("module:alpha", "root:1")]
        self.assertEqual(duplicate["lifecycle"], "duplicate_in_roots")
        self.assertEqual(duplicate["arch"], ["x86_64"])
        self.assertEqual(duplicate["sha256"], sha256_of(self.alpha_dup_bin))

        intel = by_key[("module:intel-only", "root:1")]
        self.assertEqual(intel["lifecycle"], "discovered")
        self.assertEqual(intel["arch"], ["x86_64"])
        self.assertEqual(intel["sha256"], sha256_of(self.intel_bin))

        # Protocol-safe ids only: an unsafe-named candidate is not inventoried.
        self.assertNotIn("module:bad name", {m["moduleRef"] for m in modules})

        # READ-ONLY: enumeration left the roots byte-for-byte identical.
        self.assertEqual(tree_digest(self.root0), before0)
        self.assertEqual(tree_digest(self.root1), before1)

    def test_no_manifest_keeps_current_behavior_and_empty_inventory(self) -> None:
        sock = self._connect()
        result = host.rpc(sock, 1, "plugins.discover", {})["result"]
        self.assertIs(result["ok"], True)
        self.assertIn(result["mode"], ("scaffold", "libobs"))
        self.assertEqual(result["roots"], [])
        self.assertEqual(result["modules"], [])

    def test_hello_advertises_plugin_verbs(self) -> None:
        sock = self._connect()
        commands = host.rpc(sock, 1, "host.hello")["result"]["supportedCommands"]
        self.assertIn("plugins.discover", commands)
        self.assertIn("plugins.report", commands)

    def test_plugins_report_is_honestly_empty_before_nif_h2(self) -> None:
        # Pins the wire contract only: no module has been loaded, so no
        # loaded-module record may be fabricated.
        sock = self._connect("--user-plugins-manifest", str(self.manifest))
        report = host.rpc(sock, 1, "plugins.report", {})["result"]
        self.assertIn(report["mode"], ("scaffold", "libobs"))
        self.assertEqual(report, {"ok": True, "mode": report["mode"], "modules": []})

    # -- launch refusals ---------------------------------------------------

    def _assert_refused(self, manifest_arg: str, reason_code: str, secret_fragment: str) -> None:
        proc = self._refusal(manifest_arg)
        output = proc.stdout + proc.stderr
        self.assertEqual(
            proc.returncode, USAGE_EXIT,
            f"expected launch refusal for {reason_code}: rc={proc.returncode} out={output!r}",
        )
        self.assertIn(f"invalid --user-plugins-manifest: {reason_code}", output)
        # SANITIZED: the raw filesystem path must never be interpolated.
        self.assertNotIn(manifest_arg, output)
        self.assertNotIn(secret_fragment, output)

    def test_relative_manifest_path_is_refused_without_leaking_it(self) -> None:
        self._assert_refused(
            "relative/plugins-manifest-xyzzy71.json", "not-absolute-path", "xyzzy71"
        )

    def test_unreadable_manifest_is_refused_without_leaking_path(self) -> None:
        missing = self.base / "no-such-manifest-xyzzy72.json"
        self._assert_refused(str(missing), "unreadable-file", "xyzzy72")

    def test_malformed_json_is_refused_without_leaking_path(self) -> None:
        corrupt = self.base / "corrupt-manifest-xyzzy73.json"
        corrupt.write_text("{nope")
        self._assert_refused(str(corrupt), "malformed-json", "xyzzy73")

    def test_wrong_version_is_refused(self) -> None:
        wrong = self.base / "wrong-version-manifest-xyzzy74.json"
        wrong.write_text(json.dumps({"version": 2, "roots": []}))
        self._assert_refused(str(wrong), "unsupported-version", "xyzzy74")

    def test_non_absolute_root_is_refused(self) -> None:
        bad_root = self.base / "bad-root-manifest-xyzzy75.json"
        bad_root.write_text(
            json.dumps({"version": 1, "roots": [{"binaryDir": "relative/dir"}]})
        )
        self._assert_refused(str(bad_root), "invalid-roots", "xyzzy75")

    def test_bad_exclude_entry_is_refused(self) -> None:
        bad_exclude = self.base / "bad-exclude-manifest-xyzzy76.json"
        bad_exclude.write_text(
            json.dumps({"version": 1, "roots": [], "exclude": ["not-a-module-ref"]})
        )
        self._assert_refused(str(bad_exclude), "invalid-exclude", "xyzzy76")


if __name__ == "__main__":
    # test_host_lifecycle resolves HOST_BIN from sys.argv[1] at import time
    # (ctest passes $<TARGET_FILE:studio-host>); keep it out of unittest's argv.
    unittest.main(argv=[sys.argv[0]])
