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
IS_MACOS = sys.platform == "darwin"

# The host's launch-refusal convention: parse_args throws, main() logs
# host.exited and returns kUsageExit. (This repo's "exit-code-2 semantics".)
USAGE_EXIT = 64

CPU_TYPE_X86_64 = 0x01000007
CPU_TYPE_ARM64 = 0x0100000C


def recv_raw_text(sock, timeout: float = 7.0) -> str:
    """Like host.recv_text but returns the raw frame payload text, so tests
    can assert FRAME-LEVEL determinism (byte-identical JSON, not merely equal
    parsed dicts)."""
    sock.settimeout(timeout)
    first = sock.recv(2)
    if len(first) != 2:
        raise AssertionError("short websocket frame")
    opcode = first[0] & 0x0F
    length = first[1] & 0x7F
    if length == 126:
        length = struct.unpack("!H", sock.recv(2))[0]
    elif length == 127:
        ext = b""
        while len(ext) < 8:
            ext += sock.recv(8 - len(ext))
        length = struct.unpack("!Q", ext)[0]
    payload = b""
    while len(payload) < length:
        payload += sock.recv(length - len(payload))
    if opcode != 1:
        return recv_raw_text(sock, timeout)
    return payload.decode("utf-8")


def rpc_raw(sock, rpc_id: int, method: str, params: dict) -> str:
    host.send_text(sock, {"jsonrpc": "2.0", "id": rpc_id, "method": method, "params": params})
    while True:
        raw = recv_raw_text(sock)
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            continue
        if parsed.get("id") == rpc_id:
            return raw


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

        # root0: a REAL Mach-O (the host binary itself, macOS only -- on other
        # platforms the host binary is not Mach-O, so a fabricated thin arm64
        # header stands in) in a CFBundle layout, an excluded fabricated arm64
        # bundle, a nested legacy universal module, and an unsafe-named bundle
        # that must be skipped.
        alpha_bytes = HOST_BIN.read_bytes() if IS_MACOS else thin_macho64(CPU_TYPE_ARM64)
        self.alpha_bin = build_bundle(self.root0, "alpha", alpha_bytes)
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
        # Deterministic at FRAME level: the same request (same id) twice must
        # produce byte-identical raw JSON text, not merely equal parsed dicts.
        raw_first = rpc_raw(sock, 7, "plugins.discover", {})
        raw_second = rpc_raw(sock, 7, "plugins.discover", {})
        self.assertEqual(raw_first, raw_second)
        first = json.loads(raw_first)["result"]

        self.assertIs(first["ok"], True)
        self.assertIn(first["mode"], ("scaffold", "libobs"))
        self.assertEqual(
            first["roots"],
            [
                {"rootRef": "root:0", "candidateCount": 3, "truncated": False},
                {"rootRef": "root:1", "candidateCount": 2, "truncated": False},
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
        if IS_MACOS:
            # The host binary is a real Mach-O; arch must be non-empty and known.
            self.assertTrue(alpha["arch"])
            self.assertTrue(set(alpha["arch"]) <= {"arm64", "x86_64"})
        else:
            self.assertEqual(alpha["arch"], ["arm64"])

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

    # -- hardening: symlink confinement, walk bounds, Mach-O strictness ----

    def test_symlink_escape_is_refused_without_disclosure(self) -> None:
        # A candidate binary that is a symlink (here: escaping the root) must
        # never be read or hashed -- its record says symlink-refused, and the
        # outside file's bytes are never disclosed (no sha256 anywhere).
        outside = self.base / "outside-secret.bin"
        outside.write_bytes(b"OUTSIDE-SECRET-" + thin_macho64(CPU_TYPE_ARM64))
        outside_sha = hashlib.sha256(outside.read_bytes()).hexdigest()

        root = self.base / "symroot"
        macos = root / "sneaky.plugin" / "Contents" / "MacOS"
        macos.mkdir(parents=True)
        (macos / "sneaky").symlink_to(outside)

        manifest = self.base / "symlink-manifest.json"
        manifest.write_text(json.dumps({"version": 1, "roots": [{"binaryDir": str(root)}]}))

        sock = self._connect("--user-plugins-manifest", str(manifest))
        raw = rpc_raw(sock, 1, "plugins.discover", {})
        result = json.loads(raw)["result"]

        self.assertEqual(
            result["roots"], [{"rootRef": "root:0", "candidateCount": 1, "truncated": False}]
        )
        record = result["modules"][0]
        self.assertEqual(record["moduleRef"], "module:sneaky")
        self.assertEqual(record["lifecycle"], "discovered")
        self.assertEqual(record["reasonDetail"], "symlink-refused")
        self.assertIsNone(record["sha256"])
        self.assertEqual(record["arch"], [])
        self.assertEqual(record["observed"], {"arch": False, "dependencies": False})
        # Non-disclosure: neither the hash nor the path of the outside file
        # appears anywhere in the raw response.
        self.assertNotIn(outside_sha, raw)
        self.assertNotIn(str(outside), raw)

    def test_truncated_macho_headers_report_unreadable(self) -> None:
        # Strictness: arch facts must come only from fully-read structures.
        root = self.base / "shortroot"
        root.mkdir()
        # FAT table claiming 2 entries but truncated mid-second-entry (only
        # the second entry's cputype word is present, not the full fat_arch).
        cut_fat = (
            struct.pack(">II", 0xCAFEBABE, 2)
            + struct.pack(">5I", CPU_TYPE_X86_64, 0, 4096, 32, 12)
            + struct.pack(">I", CPU_TYPE_ARM64)
        )
        fat_bin = build_bundle(root, "cut-fat", cut_fat)
        # Thin file shorter than a complete mach_header_64 (magic + cputype
        # only -- 8 of 32 bytes).
        cut_thin = struct.pack("<II", 0xFEEDFACF, CPU_TYPE_ARM64)
        thin_bin = build_bundle(root, "cut-thin", cut_thin)

        manifest = self.base / "short-manifest.json"
        manifest.write_text(json.dumps({"version": 1, "roots": [{"binaryDir": str(root)}]}))

        sock = self._connect("--user-plugins-manifest", str(manifest))
        result = host.rpc(sock, 1, "plugins.discover", {})["result"]
        by_ref = {m["moduleRef"]: m for m in result["modules"]}
        for module_ref, path in (
            ("module:cut-fat", fat_bin),
            ("module:cut-thin", thin_bin),
        ):
            record = by_ref[module_ref]
            self.assertEqual(record["reasonDetail"], "unreadable-macho-header")
            self.assertEqual(record["arch"], [])
            self.assertEqual(record["observed"], {"arch": False, "dependencies": False})
            # The file itself is readable and small: it is still hashed.
            self.assertEqual(record["sha256"], sha256_of(path))

    def test_depth_cap_truncates_root_scan(self) -> None:
        # Walk bounds: a module binary buried deeper than the depth cap is not
        # reached, and the root is flagged truncated (deterministically).
        root = self.base / "deeproot"
        deep = root / "deep-mod" / "bin"
        for i in range(10):
            deep = deep / f"d{i}"
        deep.mkdir(parents=True)
        (deep / "deep-mod.so").write_bytes(thin_macho64(CPU_TYPE_ARM64))

        manifest = self.base / "deep-manifest.json"
        manifest.write_text(json.dumps({"version": 1, "roots": [{"binaryDir": str(root)}]}))

        sock = self._connect("--user-plugins-manifest", str(manifest))
        result = host.rpc(sock, 1, "plugins.discover", {})["result"]
        self.assertEqual(
            result["roots"], [{"rootRef": "root:0", "candidateCount": 0, "truncated": True}]
        )
        self.assertEqual(result["modules"], [])

    def test_unicode_escaped_manifest_path_resolves(self) -> None:
        # A Station-side JSON serializer may \uXXXX-escape non-ASCII path
        # bytes; the manifest scanner must decode them to real UTF-8.
        root = self.base / "root-café"
        build_bundle(root, "uni-mod", thin_macho64(CPU_TYPE_ARM64))

        manifest = self.base / "unicode-manifest.json"
        manifest_text = json.dumps(
            {"version": 1, "roots": [{"binaryDir": str(root)}]}, ensure_ascii=True
        )
        self.assertIn("\\u00e9", manifest_text)  # really escaped on disk
        manifest.write_text(manifest_text)

        sock = self._connect("--user-plugins-manifest", str(manifest))
        result = host.rpc(sock, 1, "plugins.discover", {})["result"]
        self.assertEqual(
            result["roots"], [{"rootRef": "root:0", "candidateCount": 1, "truncated": False}]
        )
        record = result["modules"][0]
        self.assertEqual(record["moduleRef"], "module:uni-mod")
        self.assertEqual(record["lifecycle"], "discovered")
        self.assertEqual(record["arch"], ["arm64"])

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
