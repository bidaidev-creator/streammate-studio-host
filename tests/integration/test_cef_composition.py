#!/usr/bin/env python3
"""CEF composition product path (#535 / B0-13 Phase A).

Scaffold-parity class (every lane): `rerouteAudio` is model state echoed by
`source.create`, `program.captureAudio` refuses honestly without libobs, and
the CI workflow keeps the CEF end-to-end lane wired (an unwired env gate would
skip the whole class silently).

CEF end-to-end class (STREAMMATE_EXPECT_CEF=1, CI packaged-app lane): a real
local page renders through obs-browser onto program video (frame-digest
departure from a stable baseline), the packaged CEF helpers spawn, the page's
WebAudio tone reaches the program mix through `reroute_audio`
(program.captureAudio nonSilent against a silent baseline), and a packaged
copy without the obs-browser module refuses browser sources with no placeholder.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

import test_host_lifecycle as host

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = (
    REPO_ROOT / ".github" / "workflows" / "macos-ci.yml",
    REPO_ROOT / ".github" / "workflows" / "windows-ci.yml",
)

EXPECT_LIBOBS = os.environ.get("STREAMMATE_EXPECT_LIBOBS", "") == "1"
EXPECT_CEF = os.environ.get("STREAMMATE_EXPECT_CEF", "") == "1"

SCENE_ID = "cef-composition-scene"
SOURCE_ID = "station-overlay"

# Bright animated gradient + an immediately-started WebAudio tone. obs-browser
# runs Chromium with autoplay-policy=no-user-gesture-required, but the resume
# retry keeps the tone deterministic if the context still starts suspended.
COMPOSITION_PAGE = """<!doctype html>
<html><body style="margin:0">
<div id="stage" style="width:100vw;height:100vh;background:linear-gradient(90deg,#ff2d95,#2dd4ff)"></div>
<script>
  const context = new AudioContext();
  const oscillator = context.createOscillator();
  const gain = context.createGain();
  gain.gain.value = 0.4;
  oscillator.frequency.value = 440;
  oscillator.connect(gain);
  gain.connect(context.destination);
  oscillator.start();
  const resumeTimer = setInterval(() => {
    if (context.state === "running") { clearInterval(resumeTimer); return; }
    context.resume();
  }, 250);
  let tick = 0;
  setInterval(() => {
    tick += 1;
    document.getElementById("stage").style.filter = `hue-rotate(${(tick * 37) % 360}deg)`;
  }, 100);
</script>
</body></html>
"""


class CefCompositionScaffoldParityTest(unittest.TestCase):
    def _connect(self) -> host.socket.socket:
        process, port, _ = host.start_host()
        self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)
        return sock

    def _load_scene(self, sock) -> None:
        loaded = host.rpc(sock, 500, "scene.load", {"sceneId": SCENE_ID, "width": 64, "height": 36})["result"]
        self.assertEqual(loaded["sceneId"], SCENE_ID)

    @unittest.skipIf(EXPECT_LIBOBS, "scaffold-lane browser sources are model-only")
    def test_reroute_audio_is_echoed_model_state(self) -> None:
        sock = self._connect()
        self._load_scene(sock)
        created = host.rpc(
            sock,
            501,
            "source.create",
            {"sceneId": SCENE_ID, "sourceId": SOURCE_ID, "kind": "browser",
             "url": "https://station.localhost/overlay/", "rerouteAudio": True},
        )["result"]
        self.assertIs(created["rerouteAudio"], True)
        defaulted = host.rpc(
            sock,
            502,
            "source.create",
            {"sceneId": SCENE_ID, "sourceId": "overlay-default", "kind": "browser",
             "url": "https://station.localhost/overlay/"},
        )["result"]
        self.assertIs(defaulted["rerouteAudio"], False)

    def test_reroute_audio_is_refused_for_plugin_kinds(self) -> None:
        # Identical in both lanes: reroute_audio is an obs-browser setting, and
        # echoing it for a plugin kind that never applies it would fabricate
        # routing state. The rejection fires before lane divergence.
        sock = self._connect()
        self._load_scene(sock)
        response = host.rpc(
            sock,
            520,
            "source.create",
            {"sceneId": SCENE_ID, "sourceId": "plugin-reroute", "kind": "some-plugin", "rerouteAudio": True},
        )
        self.assertNotIn("result", response, response)
        self.assertEqual(response["error"]["code"], -32602, response)
        self.assertIn("only valid for browser sources", response["error"]["message"])

    @unittest.skipIf(EXPECT_LIBOBS, "libobs lane captures real program audio")
    def test_program_capture_audio_refuses_honestly_without_libobs(self) -> None:
        sock = self._connect()
        response = host.rpc(sock, 510, "program.captureAudio", {})
        self.assertNotIn("result", response, response)
        self.assertEqual(response["error"]["code"], -32603, response)
        self.assertIn("requires libobs", response["error"]["message"])
        self.assertIn("refuses honestly", response["error"]["message"])

    def test_ci_workflow_keeps_the_cef_lane_wired(self) -> None:
        for workflow_path in WORKFLOWS:
            with self.subTest(workflow=workflow_path.name):
                workflow = workflow_path.read_text(encoding="utf-8")
                self.assertIn("STREAMMATE_EXPECT_CEF=1", workflow)
                self.assertIn("test_cef_composition.py", workflow)


@unittest.skipUnless(EXPECT_CEF, "CEF lane only (STREAMMATE_EXPECT_CEF=1)")
class CefCompositionLibobsTest(unittest.TestCase):
    """End-to-end CEF composition against the packaged host (CI lane)."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        # CEF subprocess/DLL handles can take a beat to clear on Windows; this
        # bounded cleanup retries and still fails loudly if a process leaked.
        if sys.platform == "win32":
            self.addCleanup(host.cleanup_tempdir_with_retry, tmp)
        else:
            self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        page = self.tmp / "composition.html"
        page.write_text(COMPOSITION_PAGE, encoding="utf-8")
        self.page_url = page.as_uri()

    def _connect(self, binary: Path | None = None) -> tuple[subprocess.Popen[str], host.socket.socket]:
        if binary is None:
            process, port, _ = host.start_host()
        else:
            process = subprocess.Popen(
                [str(binary), "--token", host.TOKEN, "--port", "0"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
            )
            port, _ = host.wait_ready(process)
        if sys.platform == "win32":
            self.addCleanup(self._stop_process_and_wait_for_helpers, process)
        else:
            self.addCleanup(host.stop_process, process)
        sock = host.websocket_connect(port)
        self.addCleanup(sock.close)
        return process, sock

    @staticmethod
    def _windows_browser_page_processes() -> str:
        result = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq obs-browser-page.exe"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        return result.stdout

    def _stop_process_and_wait_for_helpers(self, process: subprocess.Popen[str]) -> None:
        host.stop_process(process)
        if sys.platform != "win32":
            return
        deadline = time.time() + 10
        last_output = ""
        while time.time() < deadline:
            last_output = self._windows_browser_page_processes()
            if "obs-browser-page.exe" not in last_output.lower():
                return
            time.sleep(0.5)
        self.fail(f"obs-browser-page.exe leaked after host shutdown: {last_output}")

    def _program_scene(self, sock) -> None:
        loaded = host.rpc(sock, 600, "scene.load",
                          {"sceneId": SCENE_ID, "width": 128, "height": 72, "background": "#102030"})["result"]
        self.assertEqual(loaded["sceneId"], SCENE_ID)
        host.rpc(sock, 601, "scene.setProgram", {"sceneId": SCENE_ID})

    def _capture_frame(self, sock, rpc_id: int) -> dict:
        return host.rpc(sock, rpc_id, "program.captureFrame", {})["result"]

    def _capture_audio(self, sock, rpc_id: int, duration_ms: int = 300) -> dict:
        return host.rpc(sock, rpc_id, "program.captureAudio", {"durationMs": duration_ms})["result"]

    def test_browser_page_renders_and_sounds_on_program(self) -> None:
        _, sock = self._connect()
        self._program_scene(sock)

        baseline = self._capture_frame(sock, 610)
        confirm = self._capture_frame(sock, 611)
        self.assertEqual(baseline["frameSha256"], confirm["frameSha256"], "baseline program video is not stable")

        silent = self._capture_audio(sock, 612)
        self.assertIs(silent["nonSilent"], False, f"program mix already carries audio: {silent}")

        created = host.rpc(
            sock,
            620,
            "source.create",
            {"sceneId": SCENE_ID, "sourceId": SOURCE_ID, "kind": "browser",
             "url": self.page_url, "width": 1280, "height": 720, "rerouteAudio": True},
        )["result"]
        self.assertIs(created["rerouteAudio"], True)

        # CEF spins up subprocesses and paints asynchronously; the digest
        # departs the baseline once real page pixels hit program video.
        departed: dict | None = None
        deadline = time.time() + 90
        rpc_id = 630
        while time.time() < deadline:
            frame = self._capture_frame(sock, rpc_id)
            rpc_id += 1
            if frame["frameSha256"] != baseline["frameSha256"] and frame["nonZeroBytes"]:
                departed = frame
                break
            time.sleep(1)
        self.assertIsNotNone(departed, "program video digest never departed the baseline (no CEF pixels)")

        if sys.platform == "win32":
            helpers = self._windows_browser_page_processes()
            self.assertIn(
                "obs-browser-page.exe", helpers.lower(),
                "no packaged obs-browser-page.exe process spawned",
            )
        else:
            helpers = subprocess.run(
                ["pgrep", "-f", "studio-host Helper"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False,
            )
            self.assertTrue(helpers.stdout.strip(), "no packaged CEF helper processes spawned")

        audible: dict | None = None
        deadline = time.time() + 45
        while time.time() < deadline:
            audio = self._capture_audio(sock, rpc_id)
            rpc_id += 1
            if audio["nonSilent"]:
                audible = audio
                break
            time.sleep(1)
        self.assertIsNotNone(audible, "program mix never carried the page's tone (reroute_audio path)")
        self.assertGreater(audible["peak"], 0.001)
        self.assertGreater(audible["frames"], 0)

    def test_reroute_audio_off_keeps_program_mix_silent(self) -> None:
        # The false direction: a mirror that unconditionally wrote
        # reroute_audio=true would pass the audible test above, so prove the
        # same tone page WITHOUT rerouteAudio leaves the program mix silent
        # even after its pixels reach program video.
        _, sock = self._connect()
        self._program_scene(sock)
        baseline = self._capture_frame(sock, 800)

        created = host.rpc(
            sock,
            801,
            "source.create",
            {"sceneId": SCENE_ID, "sourceId": SOURCE_ID, "kind": "browser",
             "url": self.page_url, "width": 1280, "height": 720},
        )["result"]
        self.assertIs(created["rerouteAudio"], False)

        rendered = False
        deadline = time.time() + 90
        rpc_id = 810
        while time.time() < deadline:
            frame = self._capture_frame(sock, rpc_id)
            rpc_id += 1
            if frame["frameSha256"] != baseline["frameSha256"] and frame["nonZeroBytes"]:
                rendered = True
                break
            time.sleep(1)
        self.assertTrue(rendered, "tone page never rendered (cannot prove the silent direction)")

        audio = self._capture_audio(sock, rpc_id, duration_ms=1500)
        self.assertIs(audio["nonSilent"], False, f"program mix carried audio without reroute_audio: {audio}")

    def test_browser_source_refuses_without_obs_browser_plugin(self) -> None:
        if sys.platform == "win32":
            package_root = host.HOST_BIN.parent
            stripped = self.tmp / package_root.name
            shutil.copytree(package_root, stripped, symlinks=True)
            browser_plugin = stripped / "obs-plugins" / "64bit" / "obs-browser.dll"
            self.assertTrue(browser_plugin.is_file(), "packaged dist is missing obs-browser.dll")
            browser_plugin.unlink()
            stripped_host = stripped / host.HOST_BIN.name
        else:
            package_root = host.HOST_BIN.parents[2]
            self.assertEqual(
                package_root.suffix, ".app",
                f"CEF lane expects a packaged .app bundle, got {host.HOST_BIN}",
            )
            stripped = self.tmp / package_root.name
            shutil.copytree(package_root, stripped, symlinks=True)
            browser_plugin = stripped / "Contents" / "PlugIns" / "obs-plugins" / "obs-browser.plugin"
            self.assertTrue(browser_plugin.exists(), "packaged bundle is missing obs-browser.plugin")
            shutil.rmtree(browser_plugin)
            stripped_host = stripped / "Contents" / "MacOS" / host.HOST_BIN.name

        _, sock = self._connect(stripped_host)
        self._program_scene(sock)
        response = host.rpc(
            sock,
            700,
            "source.create",
            {"sceneId": SCENE_ID, "sourceId": SOURCE_ID, "kind": "browser", "url": self.page_url},
        )
        self.assertNotIn("result", response, response)
        self.assertEqual(response["error"]["code"], -32603, response)
        self.assertIn("obs-browser (CEF) is unavailable", response["error"]["message"])
        self.assertIn("no placeholder substituted", response["error"]["message"])

        # The refusal rolled the model back and the host stays healthy.
        removal = host.rpc(sock, 701, "source.remove", {"sourceId": SOURCE_ID})
        self.assertNotIn("result", removal, removal)
        health = host.rpc(sock, 702, "host.health", {})["result"]
        self.assertEqual(health["status"], "ready")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
