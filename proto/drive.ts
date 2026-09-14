// PROTOTYPE driver: runs the preview measurement page in Chrome and dumps its results.
import puppeteer from "puppeteer-core";
const PORT = process.env.HOST_PORT!, PID = process.env.HOST_PID!;
const HEADLESS = process.env.HEADFUL ? false : true;
const out: string[] = [];
const say = (s: string) => { console.log(s); out.push(s); };
const sleep = (ms: number) => new Promise(r => setTimeout(r, ms));

const browser = await puppeteer.launch({
  executablePath: "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  headless: HEADLESS, args: ["--window-size=1500,1100", "--no-first-run", "--disable-background-timer-throttling", "--disable-renderer-backgrounding"],
  defaultViewport: { width: 1480, height: 1050 },
});
const page = await browser.newPage();
page.on("console", m => { const t = m.text(); if (/error|fail/i.test(t)) say("console: " + t.slice(0, 200)); });
page.on("pageerror", e => say("pageerror: " + e.message));
await page.goto("http://127.0.0.1:8787/", { waitUntil: "load" });
const ev = (expr: string) => page.evaluate(expr);
await page.$eval("#port", (el, v) => (el as HTMLInputElement).value = v, PORT);
await page.$eval("#pid", (el, v) => (el as HTMLInputElement).value = v, PID);

async function connect(mode: "direct" | "relay") {
  await ev(`action(() => connect('${mode}'))`);
  for (let i = 0; i < 60; i++) { await sleep(500); const c = (await ev(`$('connection').textContent`)) as string; if (/baseline/i.test(c) && !/measuring|wait/i.test(c)) break; }
  say(`connected ${mode}: ` + (await ev(`$('connection').textContent`)));
}
await connect("direct");
await ev("action(setupScenes)"); await sleep(7000);
say("setup log: " + ((await ev("logs.slice(-6).join(' || ')")) as string).slice(0, 600));

const CASES: Record<string, unknown>[] = [
  { codec: "jpeg", width: 1280, height: 720, fpsDivisor: 1, quality: 75, pixfmt: "i420" },
  { codec: "jpeg", width: 960, height: 540, fpsDivisor: 1, quality: 75, pixfmt: "i420" },
  { codec: "jpeg", width: 640, height: 360, fpsDivisor: 1, quality: 75, pixfmt: "i420" },
  { codec: "jpeg", width: 1280, height: 720, fpsDivisor: 2, quality: 75, pixfmt: "i420" },
  { codec: "jpeg", width: 1280, height: 720, fpsDivisor: 1, quality: 75, pixfmt: "i420", capture: "gpu" },
  { codec: "jpeg", width: 1280, height: 720, fpsDivisor: 1, quality: 75, pixfmt: "i420", capture: "gpu", gpuPipeline: 1 },
  { codec: "jpeg", width: 640, height: 360, fpsDivisor: 1, quality: 75, pixfmt: "i420", capture: "gpu" },
  { codec: "h264", width: 1280, height: 720, fpsDivisor: 1, bitrateKbps: 2500 },
  { codec: "h264", width: 960, height: 540, fpsDivisor: 1, bitrateKbps: 1500 },
  { codec: "h264", width: 640, height: 360, fpsDivisor: 1, bitrateKbps: 1000 },
];
async function runCase(c: Record<string, unknown>, seconds = 8, opts: { switchAt?: number; shot?: string } = {}) {
  const r = await ev(`startFeed(${JSON.stringify(c)}).then(() => 'ok', e => 'ERR ' + e.message)`);
  if (r !== "ok") { say(`case ${JSON.stringify(c)} start failed: ${r}`); return; }
  if (opts.switchAt) { await sleep(opts.switchAt * 1000); await ev("$('switch').click()"); await sleep((seconds - opts.switchAt) * 1000); }
  else await sleep(seconds * 1000);
  if (opts.shot) await page.screenshot({ path: opts.shot });
  await ev("stopFeed().then(() => 'ok', e => 'ERR ' + e.message)");
  say("row: " + (await ev("rows.at(-1).join(' | ')")));
}
say("=== pass 1: direct, idle host");
for (const c of CASES) await runCase(c, 8, c.codec === "h264" && c.width === 1280 ? { shot: "/tmp/sm-shot-h264.png" } : c.capture === "gpu" && c.width === 1280 && !c.gpuPipeline ? { shot: "/tmp/sm-shot-jpeg-gpu.png" } : {});
say("=== pass 2: scene switch mid-feed");
await runCase(CASES[0], 8, { switchAt: 4 }); say("switch log: " + ((await ev("logs.filter(l => /switch|gap/i.test(l)).slice(-3).join(' || ')")) as string).slice(0, 500));
await runCase(CASES[7], 8, { switchAt: 4 }); say("switch log: " + ((await ev("logs.filter(l => /switch|gap/i.test(l)).slice(-3).join(' || ')")) as string).slice(0, 500));
say("=== pass 3: snapshot loop on (thumbnails at 1 Hz) during jpeg gpu 720p and h264 720p");
await ev("$('snapshots').checked = true; $('snapshots').onchange()");
await runCase(CASES[4], 8); await runCase(CASES[7], 8);
say("snapshot means: " + (await ev("['program','web-a','web-b'].map(t => t + ': ' + $('snap-' + t).textContent).join(' || ')")));
await page.screenshot({ path: "/tmp/sm-shot-thumbs.png" });
await ev("$('snapshots').checked = false; $('snapshots').onchange()");

say("=== pass 4: REAL live output (VideoToolbox H.264 + AAC over RTMP to ffmpeg) then feeds");
const ff = Bun.spawn(["ffmpeg", "-hide_banner", "-loglevel", "error", "-listen", "1", "-i", "rtmp://127.0.0.1:19350/live/proto", "-f", "null", "-"], { stdout: "ignore", stderr: "pipe" });
await sleep(1000);
say("configure: " + JSON.stringify(await ev(`rpc('output.configure', {outputId:'rtmp-main', endpoint:'rtmp://127.0.0.1:19350/live', allowLiveEgress:true, videoEncoder:'videotoolbox_h264', audioEncoder:'aac'})`)).slice(0, 200));
say("start: " + JSON.stringify(await ev(`rpc('output.start', {outputId:'rtmp-main', streamKey:'proto', allowLiveEgress:true})`)).slice(0, 200));
await ev("fakeOutput = true");
await sleep(3000);
// baseline while live, no preview
{ const a = await ev("hostSample()") as any; await sleep(4000); const b = await ev("hostSample()") as any; say(`live output, no preview: host CPU ${((b.stats.cpuTotalMs - a.stats.cpuTotalMs) / (b.at - a.at) * 100).toFixed(1)}%`); }
for (const c of [CASES[0], CASES[2], CASES[4], CASES[6], CASES[7], CASES[9]]) await runCase(c, 8);
say("status: " + JSON.stringify(await ev(`rpc('output.status', {outputId:'rtmp-main'})`)).slice(0, 300));
say("stop: " + JSON.stringify(await ev(`rpc('output.stop', {outputId:'rtmp-main'})`)).slice(0, 120));
await ev("fakeOutput = false");
ff.kill();

say("=== pass 5: via Bun relay hop (daemon-in-the-middle shape)");
await connect("relay");
for (const c of [CASES[0], CASES[4], CASES[7]]) await runCase(c, 8);

const md = await ev("markdown()") as string;
await Bun.write("/tmp/sm-browser-results.md", md + "\n\n## Driver log\n" + out.join("\n") + "\n\n## Page log\n" + ((await ev("logs.join('\\n')")) as string));
await page.screenshot({ path: "/tmp/sm-shot-final.png", fullPage: true });
await browser.close();
say("wrote /tmp/sm-browser-results.md");
