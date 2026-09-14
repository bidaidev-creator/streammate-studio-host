#!/usr/bin/env python3
"""PROTOTYPE smoke for the preview verbs: boots the rebuilt host, composes a browser source,
takes snapshots, runs the jpeg and h264 feeds for a few seconds each, prints stats."""
import base64, json, os, struct, sys, tempfile, time, pathlib
HOST_BIN = sys.argv[1] if len(sys.argv) > 1 else "/tmp/sm-host-proto/StreamMateStudioHost.app/Contents/MacOS/studio-host"
sys.argv = ["smoke", HOST_BIN]
sys.path.insert(0, os.path.expanduser("~/Projects/streammate-studio-host/tests/integration"))
import test_host_lifecycle as host  # noqa: E402

PAGE = """<!doctype html><html><body style="margin:0;background:#123"><canvas id=c width=1280 height=720></canvas>
<script>const c=document.getElementById('c').getContext('2d');let n=0;function f(){n++;c.fillStyle='#123';c.fillRect(0,0,1280,720);
c.fillStyle=`hsl(${n*3%360} 80% 60%)`;c.beginPath();c.arc(640+500*Math.sin(n/30),360+250*Math.cos(n/30),80,0,7);c.fill();
c.fillStyle='#fff';c.font='120px monospace';c.fillText(String(n),80,200);c.fillText(String(Date.now()%100000),80,600);requestAnimationFrame(f)}f()</script></body></html>"""

def recv_any(sock, timeout=7.0):
    sock.settimeout(timeout)
    first = b""
    while len(first) < 2: first += sock.recv(2 - len(first))
    opcode = first[0] & 0x0F; length = first[1] & 0x7F
    if length == 126: length = struct.unpack("!H", sock.recv(2))[0]
    elif length == 127:
        ext = b""
        while len(ext) < 8: ext += sock.recv(8 - len(ext))
        length = struct.unpack("!Q", ext)[0]
    payload = b""
    while len(payload) < length: payload += sock.recv(min(1 << 16, length - len(payload)))
    return opcode, payload

def rpc(sock, rid, method, params=None):
    req = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None: req["params"] = params
    host.send_text(sock, req)
    binaries = []
    while True:
        op, payload = recv_any(sock)
        if op == 2: binaries.append(payload); continue
        msg = json.loads(payload)
        if msg.get("id") == rid: return msg, binaries

def parse_header(b):
    magic, kind, flags, pixfmt, seq, wall, ts, w, h, enc = struct.unpack("!BBBBIQQHHI", b[:32])
    return dict(kind=kind, key=flags & 1, pixfmt=pixfmt, seq=seq, wall=wall, ts=ts, w=w, h=h, encodeUs=enc, size=len(b) - 32)

def run_feed(sock, rid, params, seconds):
    r, _ = rpc(sock, rid, "preview.start", params)
    print("start", json.dumps(r.get("result") or r.get("error")))
    if "error" in r: return rid + 1
    t_end = time.time() + seconds; frames = []; lat = []
    while time.time() < t_end:
        op, payload = recv_any(sock, timeout=5)
        if op != 2: continue
        hd = parse_header(payload); now = int(time.time() * 1000)
        frames.append(hd); lat.append(now - hd["wall"])
    r, extra = rpc(sock, rid + 1, "preview.stop")
    frames += [parse_header(b) for b in extra]
    st = r.get("result") or r.get("error")
    lat.sort()
    print(f"  frames={len(frames)} fps={len(frames)/seconds:.1f} avgBytes={sum(f['size'] for f in frames)//max(1,len(frames))} "
          f"latency ms p50={lat[len(lat)//2] if lat else None} p95={lat[int(len(lat)*0.95)] if lat else None} keyframes={sum(f['key'] for f in frames)}")
    keys = ("avgEncodeUs", "avgRenderUs", "avgObsDelayUs", "avgCaptureToSentUs", "droppedBusy", "droppedBackpressure", "cpuUserMs", "cpuSysMs", "elapsedMs")
    print("  stats", json.dumps({k: st.get(k) for k in keys}), f"hostCpu={100*(st['cpuUserMs']+st['cpuSysMs'])/max(1,st['elapsedMs']):.1f}%")
    if frames and frames[0]["kind"] == 1:
        pathlib.Path("/tmp/sm-preview-sample.jpg").write_bytes(b"")  # placeholder; real bytes saved below
    return rid + 2

def main():
    tmp = tempfile.mkdtemp(); page = pathlib.Path(tmp, "page.html"); page.write_text(PAGE)
    process, port, _ = host.start_host(state_file=pathlib.Path(tmp, "state.json"))
    try:
        sock = host.websocket_connect(port)
        # drain host.started/host.ready events
        hello, _ = rpc(sock, 1, "host.hello", {"version": "1"})
        cmds = hello.get("result", {}).get("supportedCommands", [])
        print("preview verbs advertised:", [c for c in cmds if c.startswith("preview.")])
        r, _ = rpc(sock, 2, "scene.load", {"sceneId": "A", "width": 128, "height": 72, "background": "#102030"})
        print("scene.load", json.dumps(r))
        r, _ = rpc(sock, 3, "scene.setProgram", {"sceneId": "A"})
        print("scene.setProgram", json.dumps(r))
        base, _ = rpc(sock, 5, "preview.stats"); print("baseline stats", json.dumps(base.get("result")))
        r, _ = rpc(sock, 4, "source.create", {"sceneId": "A", "sourceId": "web", "kind": "browser", "url": page.as_uri(), "width": 1280, "height": 720})
        print("source.create", json.dumps(r.get("result") or r.get("error"))[:200])
        time.sleep(4)  # let CEF paint
        for target in ({}, {"sceneId": "A"}, {"sourceId": "web"}):
            timings = []
            for i in range(8):
                r, _ = rpc(sock, 10 + i, "preview.snapshot", {**target, "width": 320, "height": 180, "quality": 70})
                res = r.get("result") or r.get("error")
                if "jpegBase64" in res:
                    data = base64.b64decode(res.pop("jpegBase64"))
                    pathlib.Path(f"/tmp/sm-snapshot-{res['target']}.jpg").write_bytes(data)
                    timings.append((res["renderUs"], res["encodeUs"], res["bytes"]))
                else:
                    print("snapshot", json.dumps(res)); break
            if timings:
                print(f"snapshot {res['target']}: first renderUs={timings[0][0]} steady renderUs={[t[0] for t in timings[1:]]} encodeUs~{timings[-1][1]} bytes~{timings[-1][2]}")
        rid = 20
        rid = run_feed(sock, rid, {"codec": "jpeg", "width": 1280, "height": 720, "quality": 75, "pixfmt": "i420"}, 4)
        rid = run_feed(sock, rid, {"codec": "jpeg", "width": 640, "height": 360, "quality": 75, "pixfmt": "i420"}, 4)
        rid = run_feed(sock, rid, {"codec": "jpeg", "width": 1280, "height": 720, "quality": 75, "pixfmt": "bgra"}, 4)
        rid = run_feed(sock, rid, {"codec": "jpeg", "capture": "gpu", "width": 1280, "height": 720, "quality": 75}, 4)
        rid = run_feed(sock, rid, {"codec": "jpeg", "capture": "gpu", "gpuPipeline": 1, "width": 1280, "height": 720, "quality": 75}, 4)
        rid = run_feed(sock, rid, {"codec": "jpeg", "capture": "gpu", "width": 640, "height": 360, "quality": 75}, 4)
        rid = run_feed(sock, rid, {"codec": "h264", "width": 1280, "height": 720, "bitrateKbps": 2500}, 4)
        rid = run_feed(sock, rid, {"codec": "h264", "width": 640, "height": 360, "bitrateKbps": 1000}, 4)
        # save one real jpeg frame for eyeballing
        r, _ = rpc(sock, rid, "preview.start", {"codec": "jpeg", "width": 640, "height": 360, "quality": 80}); rid += 1
        op, payload = recv_any(sock)
        while op != 2: op, payload = recv_any(sock)
        pathlib.Path("/tmp/sm-preview-sample.jpg").write_bytes(payload[32:])
        rpc(sock, rid, "preview.stop")
        print("saved /tmp/sm-preview-sample.jpg and /tmp/sm-snapshot-*.jpg")
    finally:
        host.stop_process(process)
        out = process.stdout.read() if process.stdout else ""
        errs = [l for l in out.splitlines() if "error" in l.lower() or "crash" in l.lower()]
        print("host log error lines:", errs[:10])

if __name__ == "__main__":
    main()
