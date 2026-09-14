#!/usr/bin/env python3
"""PROTOTYPE: run the preview feeds while a REAL libobs RTMP output (VideoToolbox H.264 + AAC) is live
against a loopback ffmpeg RTMP listener. Prints host CPU with/without preview while live."""
import json, os, subprocess, sys, time, pathlib, tempfile, struct
sys.argv = ["live", "/tmp/sm-host-proto/StreamMateStudioHost.app/Contents/MacOS/studio-host"]
sys.path.insert(0, os.path.expanduser("~/Projects/streammate-studio-host/tests/integration"))
import test_host_lifecycle as host  # noqa
sys.path.insert(0, "/tmp/sm-host-src/proto")
from smoke import rpc, recv_any, parse_header, PAGE  # noqa

RTMP_PORT = 19350
def main():
    ff = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "warning", "-listen", "1", "-i", f"rtmp://127.0.0.1:{RTMP_PORT}/live/proto",
                           "-f", "null", "-"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    time.sleep(1)
    tmp = tempfile.mkdtemp(); page = pathlib.Path(tmp, "page.html"); page.write_text(PAGE)
    process, port, _ = host.start_host("--allow-live-egress", state_file=pathlib.Path(tmp, "state.json"))
    try:
        sock = host.websocket_connect(port)
        rpc(sock, 1, "host.hello", {"version": "1"})
        rpc(sock, 2, "scene.load", {"sceneId": "A", "width": 128, "height": 72, "background": "#102030"})
        rpc(sock, 3, "scene.setProgram", {"sceneId": "A"})
        rpc(sock, 4, "source.create", {"sceneId": "A", "sourceId": "web", "kind": "browser", "url": page.as_uri(), "width": 1280, "height": 720})
        time.sleep(4)
        def cpu():
            r, _ = rpc(sock, 99, "preview.stats"); return r["result"]["cpuTotalMs"]
        def measure(label, seconds=6):
            c0 = cpu(); t0 = time.time(); time.sleep(seconds); c1 = cpu()
            print(f"{label}: host cpu {100*(c1-c0)/((time.time()-t0)*1000):.1f}%")
        measure("idle, no output, no preview")
        r, _ = rpc(sock, 10, "output.configure", {"outputId": "rtmp-main", "endpoint": f"rtmp://127.0.0.1:{RTMP_PORT}/live", "allowLiveEgress": True,
                                                   "videoEncoder": "videotoolbox_h264", "audioEncoder": "aac"})
        print("configure", json.dumps(r.get("result") or r.get("error"))[:300])
        r, _ = rpc(sock, 11, "output.start", {"outputId": "rtmp-main", "streamKey": "proto", "allowLiveEgress": True})
        print("start", json.dumps(r.get("result") or r.get("error"))[:300])
        time.sleep(3)
        r, _ = rpc(sock, 12, "output.status", {"outputId": "rtmp-main"}); print("status", json.dumps(r.get("result") or r.get("error"))[:400])
        measure("live output, no preview")
        for params in ({"codec": "jpeg", "width": 1280, "height": 720, "quality": 75, "pixfmt": "i420"},
                       {"codec": "jpeg", "width": 640, "height": 360, "quality": 75, "pixfmt": "i420"},
                       {"codec": "h264", "width": 1280, "height": 720, "bitrateKbps": 2500},
                       {"codec": "h264", "width": 640, "height": 360, "bitrateKbps": 1000}):
            r, _ = rpc(sock, 20, "preview.start", params)
            if "error" in r: print("preview.start error", r["error"]); continue
            t_end = time.time() + 6; frames = 0; lat = []
            while time.time() < t_end:
                op, payload = recv_any(sock, timeout=5)
                if op == 2:
                    frames += 1; lat.append(int(time.time()*1000) - parse_header(payload)["wall"])
            r, extra = rpc(sock, 21, "preview.stop"); st = r["result"]; frames += len(extra)
            lat.sort()
            print(f"live + preview {params['codec']} {params['width']}x{params['height']}: fps={frames/6:.1f} hostCpu={(100*(st['cpuUserMs']+st['cpuSysMs'])/st['elapsedMs']):.1f}% "
                  f"avgEncodeUs={st['avgEncodeUs']} avgObsDelayUs={st['avgObsDelayUs']} droppedBusy={st['droppedBusy']} lat p50={lat[len(lat)//2] if lat else None} p95={lat[int(len(lat)*.95)] if lat else None}")
        r, _ = rpc(sock, 30, "output.status", {"outputId": "rtmp-main"}); print("status after", json.dumps(r.get("result") or r.get("error"))[:400])
        rpc(sock, 31, "output.stop", {"outputId": "rtmp-main"})
    finally:
        host.stop_process(process); ff.terminate()
        try: err = ff.communicate(timeout=3)[1]
        except Exception: err = ""
        print("ffmpeg stderr:", err[-600:])
        out = process.stdout.read() if process.stdout else ""
        print("host log tail:", "\n".join(l for l in out.splitlines() if 'rtmp' in l.lower() or 'output' in l.lower() or 'error' in l.lower())[-1500:])

if __name__ == "__main__":
    main()
