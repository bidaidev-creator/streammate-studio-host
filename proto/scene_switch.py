#!/usr/bin/env python3
"""PROTOTYPE: frame continuity across scene switches for both feeds, plus feed behaviour when the
program scene is empty / the browser source is removed."""
import json, os, sys, time, pathlib, tempfile
sys.argv = ["sw", "/tmp/sm-host-proto/StreamMateStudioHost.app/Contents/MacOS/studio-host"]
sys.path.insert(0, os.path.expanduser("~/Projects/streammate-studio-host/tests/integration"))
import test_host_lifecycle as host  # noqa
sys.path.insert(0, "/tmp/sm-host-src/proto")
from smoke import rpc, recv_any, parse_header, PAGE  # noqa

def collect(sock, seconds, switch_at, switch_fn):
    t0 = time.time(); switched = None; arrivals = []
    while time.time() - t0 < seconds:
        if switched is None and time.time() - t0 >= switch_at:
            switched = time.time(); r = switch_fn(); arrivals.append(("switch", switched, None, r))
            continue
        op, payload = recv_any(sock, timeout=5)
        if op == 2:
            hd = parse_header(payload); arrivals.append(("frame", time.time(), hd, None))
    frames = [a for a in arrivals if a[0] == "frame"]
    gaps = [(frames[i][1] - frames[i-1][1]) * 1000 for i in range(1, len(frames))]
    seqs = [f[2]["seq"] for f in frames]
    seq_skips = sum(1 for i in range(1, len(seqs)) if seqs[i] != seqs[i-1] + 1)
    sw = next(a for a in arrivals if a[0] == "switch")
    before = [f for f in frames if f[1] < sw[1]]; after = [f for f in frames if f[1] > sw[1]]
    first_after = (after[0][1] - sw[1]) * 1000 if after else None
    keys_after = [i for i, f in enumerate(after[:40]) if f[2]["key"]]
    return dict(frames=len(frames), maxGapMs=round(max(gaps), 1) if gaps else None, gapsOver100ms=sum(1 for g in gaps if g > 100),
                seqSkips=seq_skips, msSwitchToNextFrame=round(first_after, 1) if first_after is not None else None,
                framesUntilKeyframeAfterSwitch=keys_after[0] if keys_after else None, switchRpc=sw[3])

def main():
    tmp = tempfile.mkdtemp()
    pa = pathlib.Path(tmp, "a.html"); pa.write_text(PAGE)
    pb = pathlib.Path(tmp, "b.html"); pb.write_text(PAGE.replace("#123", "#412").replace("hsl(", "hsl(120+"))
    process, port, _ = host.start_host(state_file=pathlib.Path(tmp, "state.json"))
    try:
        sock = host.websocket_connect(port); rpc(sock, 1, "host.hello", {"version": "1"})
        for sid, page in (("A", pa), ("B", pb)):
            rpc(sock, 2, "scene.load", {"sceneId": sid, "width": 128, "height": 72, "background": "#102030"})
            rpc(sock, 3, "source.create", {"sceneId": sid, "sourceId": "web-" + sid, "kind": "browser", "url": page.as_uri(), "width": 1280, "height": 720})
        rpc(sock, 4, "scene.setProgram", {"sceneId": "A"}); time.sleep(4)
        cur = ["A"]
        def switch():
            cur[0] = "B" if cur[0] == "A" else "A"
            r, _ = rpc(sock, 50, "scene.setProgram", {"sceneId": cur[0]}); return (r.get("result") or r.get("error"))
        for params in ({"codec": "jpeg", "width": 1280, "height": 720, "quality": 75},
                       {"codec": "h264", "width": 1280, "height": 720, "bitrateKbps": 2500}):
            r, _ = rpc(sock, 20, "preview.start", params); assert "result" in r, r
            res = collect(sock, 5, 2.5, switch)
            r, _ = rpc(sock, 21, "preview.stop"); st = r["result"]
            print(params["codec"], "switch:", json.dumps(res), "| host droppedBusy", st["droppedBusy"], "avgObsDelayUs", st["avgObsDelayUs"], "hostCpu%",
                  round(100 * (st["cpuUserMs"] + st["cpuSysMs"]) / st["elapsedMs"], 1))
        # source removed mid-feed and empty program: does the feed keep flowing?
        r, _ = rpc(sock, 30, "preview.start", {"codec": "jpeg", "width": 640, "height": 360}); assert "result" in r, r
        def remove():
            r, _ = rpc(sock, 51, "source.remove", {"sourceId": "web-" + cur[0]}); return (r.get("result") or r.get("error"))
        res = collect(sock, 4, 2.0, remove)
        rpc(sock, 31, "preview.stop")
        print("jpeg source.remove mid-feed:", json.dumps(res))
        # two CEF pages alive: helper cpu
        import subprocess
        pids = subprocess.run(["pgrep", "-f", "studio-host Helper"], capture_output=True, text=True).stdout.split()
        if pids:
            ps = subprocess.run(["ps", "-o", "%cpu=,rss=,comm=", "-p", ",".join(pids)], capture_output=True, text=True).stdout
            print("CEF helper processes (%cpu rss comm):\n" + ps.strip())
    finally:
        host.stop_process(process)
if __name__ == "__main__":
    main()
