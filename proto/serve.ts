// THROWAWAY preview measurement server. Run: bun proto/serve.ts
import { resolve, sep } from "node:path";
import { realpath } from "node:fs/promises";

const root = await realpath(import.meta.dir);
const sink = Bun.listen({
  hostname: "127.0.0.1", port: 0,
  socket: { data() {}, open() {}, close() {}, error() {} },
});

async function command(args: string[]) {
  try {
    const proc = Bun.spawn(args, { stdout: "pipe", stderr: "pipe", env: { ...process.env, LC_ALL: "C" } });
    const [out, err, code] = await Promise.all([
      new Response(proc.stdout).text(), new Response(proc.stderr).text(), proc.exited,
    ]);
    return { out, error: code === 0 ? null : err.trim() || `${args[0]} exited ${code}`, code, stderr: err.trim() };
  } catch (e) {
    return { out: "", error: String(e), code: -1, stderr: String(e) };
  }
}

async function cpu(pid: string) {
  const [processInfo, threadInfo, helpers] = await Promise.all([
    command(["ps", "-o", "%cpu=,rss=", "-p", pid]),
    command(["ps", "-M", "-p", pid]),
    command(["pgrep", "-f", "studio-host Helper"]),
  ]);
  const values = processInfo.out.trim().split(/\s+/).map(Number);
  const lines = threadInfo.out.trim().split(/\r?\n/);
  const header = (lines.shift() || "").trim().split(/\s+/);
  const cpuColumn = header.findIndex(x => /^(%CPU|CPU%|PCPU)$/i.test(x));
  const nameColumn = header.findIndex(x => /^(COMMAND|CMD|COMM|NAME)$/i.test(x));
  const tidColumn = header.findIndex(x => /^(TID|THREAD|LWP)$/i.test(x));
  const threads = cpuColumn < 0 ? [] : lines.map((line, index) => {
    const parts = line.trim().split(/\s+/);
    return { cpu: Number(parts[cpuColumn]), "name-or-tid":
      tidColumn >= 0 ? parts[tidColumn] : nameColumn >= 0 ? parts.slice(nameColumn).join(" ") : `thread-row-${index + 1}` };
  }).filter(x => Number.isFinite(x.cpu)).sort((a, b) => b.cpu - a.cpu).slice(0, 6);
  const pids = helpers.out.trim().split(/\s+/).filter(x => /^\d+$/.test(x));
  const noHelpers = helpers.code === 1 && !helpers.stderr;
  let helpersCpu: number | null = noHelpers ? 0 : null;
  let helperError = noHelpers ? null : helpers.error;
  if (pids.length) {
    const info = await command(["ps", "-o", "%cpu=", "-p", pids.join(",")]);
    helperError = info.error;
    const numbers = info.out.trim().split(/\s+/).filter(Boolean).map(Number);
    if (!info.error && numbers.length && numbers.every(Number.isFinite)) helpersCpu = numbers.reduce((a, b) => a + b, 0);
  }
  return {
    pid: Number(pid), now: Date.now(),
    cpu: !processInfo.error && values.length >= 2 && Number.isFinite(values[0]) ? values[0] : null,
    rss: !processInfo.error && values.length >= 2 && Number.isFinite(values[1]) ? values[1] : null,
    threads, helpersCpu,
    errors: [processInfo.error, threadInfo.error, !threadInfo.error && cpuColumn < 0 ? "No per-thread %CPU column in ps -M output" : null, helperError].filter(Boolean),
  };
}

type Relay = { url: string; upstream?: WebSocket; queued: (string | Uint8Array)[]; closed: boolean };
const server = Bun.serve({
  hostname: "127.0.0.1", port: 8787,
  async fetch(req, server) {
    const url = new URL(req.url);
    if (url.pathname === "/config") return Response.json({ fakeIngestPort: sink.port });
    if (url.pathname === "/cpu") {
      const pid = url.searchParams.get("pid") || "";
      if (!/^[1-9]\d*$/.test(pid)) return Response.json({ error: "A positive integer pid is required", now: Date.now() }, { status: 400 });
      return Response.json(await cpu(pid), { headers: { "Cache-Control": "no-store" } });
    }
    if (url.pathname === "/relay") {
      const port = url.searchParams.get("port") || "";
      if (!/^\d+$/.test(port) || +port < 1 || +port > 65535) return new Response("Invalid host port", { status: 400 });
      const upstream = new URL(`ws://127.0.0.1:${port}/control`);
      upstream.searchParams.set("token", url.searchParams.get("token") || "");
      if (server.upgrade(req, { data: { url: upstream.href, queued: [], closed: false } })) return;
      return new Response("WebSocket upgrade required", { status: 400 });
    }
    try {
      const path = await realpath(resolve(root, "." + decodeURIComponent(url.pathname === "/" ? "/preview.html" : url.pathname)));
      if (!path.startsWith(root + sep)) return new Response("Forbidden", { status: 403 });
      const file = Bun.file(path);
      if (!await file.exists()) return new Response("Not found", { status: 404 });
      return new Response(file, { headers: { "Cache-Control": "no-store" } });
    } catch {
      return new Response("Not found", { status: 404 });
    }
  },
  websocket: {
    data: {} as Relay,
    perMessageDeflate: false,
    maxPayloadLength: 32 * 1024 * 1024,
    backpressureLimit: 64 * 1024 * 1024,
    closeOnBackpressureLimit: true,
    open(ws) {
      const upstream = ws.data.upstream = new WebSocket(ws.data.url);
      upstream.binaryType = "arraybuffer";
      upstream.onopen = () => {
        if (ws.data.closed) { upstream.close(); return; }
        for (const msg of ws.data.queued) upstream.send(msg);
        ws.data.queued.length = 0;
      };
      upstream.onmessage = event => { if (!ws.data.closed) ws.send(event.data); };
      upstream.onclose = () => { ws.data.closed = true; ws.data.queued.length = 0; ws.close(1000, "Host connection closed"); };
      upstream.onerror = () => { ws.close(1011, "Host connection error"); upstream.close(); };
    },
    message(ws, message) {
      const upstream = ws.data.upstream;
      if (ws.data.closed) return;
      if (upstream?.readyState === WebSocket.OPEN) upstream.send(message);
      else if (upstream?.readyState === WebSocket.CONNECTING) ws.data.queued.push(typeof message === "string" ? message : new Uint8Array(message));
    },
    close(ws) {
      ws.data.closed = true;
      ws.data.queued.length = 0;
      ws.data.upstream?.close();
    },
  },
});
console.log(`Preview: http://127.0.0.1:${server.port} | fake ingest TCP sink: ${sink.port}`);
