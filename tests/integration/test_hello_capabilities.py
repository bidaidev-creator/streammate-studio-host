#!/usr/bin/env python3
"""Spec 36 chunk 36.6 (host): host.hello declares supportedCommands derived
from the dispatch table.

Capability truth (D36-2 / Capability 6): a track must never be credited with a
verb the host cannot perform. host.hello therefore declares the commands the
host actually dispatches, and the declaration is derived from the ONE command
table handle_message iterates -- never a hand-typed wish list. These tests prove
the declaration equals the real dispatch set structurally (by parsing the table
region of the C++ source), so the two cannot drift.
"""
from __future__ import annotations

import json
import re
import socket
import sys
import unittest
from pathlib import Path

import test_host_lifecycle as host

REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE = REPO_ROOT / "src" / "studio_host.cpp"

# The command table in studio_host.cpp is fenced by these sentinels. Both
# handle_message (dispatch) and host.hello (declaration) consume the entries
# between them, so parsing this region yields the ACTUAL dispatch set -- not a
# duplicated literal that could drift from it.
TABLE_BEGIN = ">>> STUDIO_HOST_COMMAND_TABLE"
TABLE_END = "<<< STUDIO_HOST_COMMAND_TABLE"

# A plausible-but-absent verb: it must never appear in supportedCommands and
# must be rejected by the unknown-method path.
ABSENT_VERB = "filter.reorder"

# The real invariant the host declares (and validates at construction): a JSON
# string method name -- alpha-led, then alnum/dot. Uppercase is expected
# (setProgram, refreshBrowser, exerciseTccPrompts, ...).
SAFE_METHOD_NAME = re.compile(r"^[A-Za-z][A-Za-z0-9.]*$")


def _strip_cxx_comments(text: str) -> str:
    """Remove C/C++ comments so parsing cannot be defeated by a commented-out or
    documentation `add("phantom", ...)` or a quoted method name inside a comment
    (e.g. `// kind "native-overlay"`)."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def _brace_body(text: str, signature: str) -> str:
    """Return the brace-matched body of the function whose declaration contains
    `signature`."""
    start = text.index(signature)
    open_brace = text.index("{", start)
    depth = 0
    for index in range(open_brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace + 1 : index]
    raise AssertionError(f"unbalanced braces after {signature!r}")


def dispatch_table_commands() -> list[str]:
    """Return the ordered method names registered in the dispatch table, parsed
    straight from the C++ source's command-table region.

    Hardened against parser defeats: comments/raw text are stripped, and EVERY
    add() call must take a single string-literal method name as its first
    argument. A concatenated (`add("new." "verb", ...)`) or computed
    (`add(kName, ...)`) name -- which would diverge from what the runtime
    dispatches -- makes the strict count mismatch the total and fails loudly."""
    text = SOURCE.read_text(encoding="utf-8")
    begin = text.index(TABLE_BEGIN)
    end = text.index(TABLE_END, begin)
    region = _strip_cxx_comments(text[begin:end])
    total_adds = re.findall(r"\badd\s*\(", region)
    # Each entry is: add("method.name", [this](...){ ... }); -- the first arg is
    # exactly one string literal immediately followed by a comma.
    names = re.findall(r'\badd\s*\(\s*"((?:[^"\\]|\\.)*)"\s*,', region)
    if len(names) != len(total_adds):
        raise AssertionError(
            f"every command-table add() must take a single string-literal method "
            f"name (found {len(total_adds)} add() calls but "
            f"{len(names)} single-literal names); a concatenated or computed name "
            "would diverge from the runtime dispatch"
        )
    return names


def hello_supported_commands(sock: socket.socket) -> object:
    result = host.rpc(sock, 1, "host.hello")["result"]
    return result.get("supportedCommands", "__missing__")


class HelloCapabilitiesTest(unittest.TestCase):
    def _connect(self):
        process, port, _ = host.start_host()
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)
        return sock

    def test_hello_declares_supported_commands_as_string_array(self) -> None:
        # #1 + #5: supportedCommands is a JSON array of plain strings. The
        # monorepo parser voids the whole declaration on any non-string entry,
        # so a malformed array would silently disable the native-host track.
        sock = self._connect()
        commands = hello_supported_commands(sock)
        self.assertIsInstance(commands, list, "supportedCommands must be a JSON array")
        self.assertTrue(commands, "supportedCommands must not be empty")
        for entry in commands:
            self.assertIsInstance(entry, str, f"non-string supportedCommands entry: {entry!r}")

    def test_declared_list_equals_dispatch_table(self) -> None:
        # #2: the declared list EXACTLY equals the set the dispatch handles,
        # proved structurally against the parsed table (same members, same
        # order) -- not against a second hand-maintained literal.
        sock = self._connect()
        declared = hello_supported_commands(sock)
        dispatched = dispatch_table_commands()
        self.assertTrue(dispatched, "failed to parse the command table from source")
        self.assertEqual(
            declared,
            dispatched,
            "host.hello supportedCommands drifted from the dispatch table",
        )

    def test_absent_verb_is_neither_declared_nor_dispatched(self) -> None:
        # #3: a method that is not implemented must not appear in the
        # declaration AND must be rejected by the unknown-method path.
        sock = self._connect()
        declared = hello_supported_commands(sock)
        self.assertNotIn(ABSENT_VERB, declared)
        self.assertNotIn(ABSENT_VERB, dispatch_table_commands())
        response = host.rpc(sock, 2, ABSENT_VERB, {})
        self.assertIn("error", response)
        self.assertEqual(response["error"]["code"], -32601)

    def test_declared_commands_are_actually_dispatched(self) -> None:
        # A live cross-check that the declaration is wired to real handlers:
        # a sample of declared verbs must NOT hit the unknown-method path.
        # (Side-effectful verbs -- output.*/record.*/replay.*/host.shutdown/
        # host.exerciseTccPrompts -- are validated via the source equality test
        # above rather than probed, to honor the no-egress/no-TCC gates.)
        sock = self._connect()
        declared = hello_supported_commands(sock)
        for verb in ("host.hello", "host.health", "scene.list", "filter.list"):
            self.assertIn(verb, declared)
            response = host.rpc(sock, 100, verb, {})
            code = response.get("error", {}).get("code")
            self.assertNotEqual(code, -32601, f"declared verb not dispatched: {verb}")

    def test_dispatch_only_through_command_table(self) -> None:
        # H1: prove the table is the ONLY dispatch path, so "declaration ==
        # table" also means "table == everything actually dispatched". A future
        # direct branch (e.g. `if (method == "new.verb") { ... }` before the
        # table loop) would dispatch an UNDECLARED command while every other
        # capability test still passed. Assert handle_message contains no method
        # matching against a string literal outside the table scan; the loop's
        # own `method == entry.method` (a variable, no literal) is allowed.
        body = _strip_cxx_comments(
            _brace_body(
                SOURCE.read_text(encoding="utf-8"),
                "void handle_message(int fd, const std::string &payload) {",
            )
        )
        forbidden = [
            (r'method\s*==\s*"', "method == <literal>"),
            (r'"[^"]*"\s*==\s*method\b', "<literal> == method"),
            (r"method\s*\.\s*compare\s*\(", "method.compare("),
            (r"method\s*\.\s*rfind\s*\(", "method.rfind( (prefix dispatch)"),
            (r"method\s*\.\s*find\s*\(", "method.find("),
            (r"method\s*\.\s*starts_with\s*\(", "method.starts_with("),
            (r"strcmp\s*\([^)]*\bmethod\b", "strcmp( ... method"),
        ]
        offenders: list[str] = []
        for pattern, label in forbidden:
            for match in re.finditer(pattern, body):
                line = body[: match.start()].count("\n") + 1
                offenders.append(f"{label} at handle_message body line {line}: {match.group(0)!r}")
        self.assertEqual(
            offenders,
            [],
            "handle_message must dispatch ONLY via the command-table scan; a "
            "direct method comparison would dispatch an undeclared verb:\n"
            + "\n".join(offenders),
        )

    def test_hello_result_is_valid_json_of_safe_strings(self) -> None:
        # H2: the whole host.hello result must be valid JSON and every declared
        # entry a plain, safe string. A `"`/`\\` in a future method name would
        # emit malformed JSON and (per the monorepo parser) void the ENTIRE
        # declaration on any non-string entry -- silently disabling the track.
        sock = self._connect()
        result = host.rpc(sock, 1, "host.hello")["result"]
        # Round-trips as JSON (rpc() already parsed the raw frame; a malformed
        # frame would have raised in recv_text before reaching here).
        commands = json.loads(json.dumps(result))["supportedCommands"]
        self.assertIsInstance(commands, list)
        for entry in commands:
            self.assertIsInstance(entry, str, f"non-string entry: {entry!r}")
            self.assertRegex(entry, SAFE_METHOD_NAME, f"unsafe method name: {entry!r}")

    def test_operator_verbs_present_only_when_dispatched(self) -> None:
        # output.*/import.* are operator-only and structurally unmappable by the
        # monorepo parser; they may be declared only because the host truly
        # dispatches them. Confirm the declared operator verbs match the table.
        sock = self._connect()
        declared = hello_supported_commands(sock)
        dispatched = dispatch_table_commands()
        for prefix in ("output.", "import."):
            declared_ops = [c for c in declared if c.startswith(prefix)]
            dispatched_ops = [c for c in dispatched if c.startswith(prefix)]
            self.assertEqual(declared_ops, dispatched_ops)
            self.assertTrue(declared_ops, f"expected {prefix}* verbs in the table")


if __name__ == "__main__":
    # test_host_lifecycle resolves HOST_BIN from sys.argv[1] at import time
    # (ctest passes $<TARGET_FILE:studio-host>); keep it out of unittest's argv.
    unittest.main(argv=[sys.argv[0]])
