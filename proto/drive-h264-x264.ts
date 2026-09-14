// Headful re-check of the H.264 + WebCodecs path (hardware decode) and jpeg gpu reference.
import puppeteer from "puppeteer-core";
const PORT = process.env.HOST_PORT!, PID = process.env.HOST_PID!;
const sleep = (ms: number) => new Promise(r => setTimeout(r, ms));
const browser = await puppeteer.launch({ executablePath: "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", headless: true,
  args: ["--window-size=1500,1100", "--no-first-run"], defaultViewport: { width: 1480, height: 1050 } });
const page = await browser.newPage();
await page.goto("http://127.0.0.1:8787/", { waitUntil: "load" });
const ev = (e: string) => page.evaluate(e);
await page.$eval("#port", (el, v) => (el as HTMLInputElement).value = v, PORT);
await page.$eval("#pid", (el, v) => (el as HTMLInputElement).value = v, PID);
await ev("action(() => connect('direct'))"); await sleep(6000);
console.log("conn:", await ev("$('connection').textContent"));
await ev("action(setupScenes)"); await sleep(7000);
console.log("gpu info:", await ev(`(async () => { const a = await navigator.gpu?.requestAdapter?.(); return a ? 'webgpu ok' : 'no webgpu'; })()`));
console.log("h264 hw support:", JSON.stringify(await ev(`VideoDecoder.isConfigSupported({codec:'avc1.4D401F', avc:{format:'annexb'}, hardwareAcceleration:'prefer-hardware'}).then(r => ({supported:r.supported, hw:r.config.hardwareAcceleration}))`)));
for (const c of [{ codec: "h264", width: 1280, height: 720, fpsDivisor: 1, bitrateKbps: 2500 },
                 { codec: "h264", width: 1280, height: 720, fpsDivisor: 1, bitrateKbps: 2500, encoder: "x264" },
                 { codec: "h264", width: 640, height: 360, fpsDivisor: 1, bitrateKbps: 1000, encoder: "x264" },
                 { codec: "h264", width: 1280, height: 720, fpsDivisor: 1, bitrateKbps: 2500, encoder: "x264" }]) {
  console.log(await ev(`startFeed(${JSON.stringify(c)}).then(() => 'ok', e => 'ERR ' + e.message)`));
  await sleep(8000);
  if (c.encoder === "x264" && c.width === 1280) await page.screenshot({ path: "/tmp/sm-shot-h264-x264.png" });
  await ev("stopFeed().then(() => 'ok', e => 'ERR ' + e.message)");
  console.log("row:", await ev("rows.at(-1).join(' | ')"));
}
console.log("decoder log:", (await ev("logs.filter(l => /decod|codec|hardware/i.test(l)).slice(0, 6).join(' || ')") as string).slice(0, 600));
await browser.close();
