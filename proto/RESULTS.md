# Preview feed prototype — results (streammate-pivot #11)

Machine: Apple M4 (10 cores, 16 GB), macOS 26.5.2. Host: CI bundle of `streammate-studio-host` main `ef9f64a` (libobs 32.1.2, CEF 6533, VideoToolbox), rebuilt with `proto-build.sh` (no Xcode on this Mac: the host is compiled against the bundle's own libobs + Qt frameworks). Browser: Google Chrome 152, driven by `proto/drive.ts` (puppeteer). Program: 1280x720@30, one CEF browser source per scene rendering `page-a.html` / `page-b.html`. CPU % = share of one core, host process only (CEF helpers are separate processes, ~20-30% of a core between them regardless of preview). Idle host with one CEF source composited: ~6%.

Raw data: `results/browser-results.md` (full table, driver log, page log), `results/python-*.txt`, screenshots `results/*.png`.

## Program preview shapes (8 s runs, direct WebSocket to the host, host idle otherwise)

| shape | fps recv / shown | paint latency p50 / p95 ms | host CPU % | loopback bandwidth | notes |
| --- | --- | --- | --- | --- | --- |
| JPEG, raw-video callback, 1280x720 q75 | 29.9 / 29.8 | 12 / 21 | 31 | 16 Mbit/s | libobs raw path adds ~40 ms before the callback fires (`avgObsDelayUs`), included in paint latency |
| JPEG raw 960x540 | 30.0 / 29.2 | 8 / 21 | 24 | 9.7 Mbit/s | downscale is CPU swscale inside libobs |
| JPEG raw 640x360 | 30.0 / 29.2 | 5 / 20 | 18.6 | 5.7 Mbit/s | |
| JPEG raw 1280x720, every 2nd frame (15 fps) | 15.0 / 14.7 | 11 / 16 | 22.6 | 8.2 Mbit/s | |
| JPEG, GPU texrender capture, 1280x720 | 28.6 / 27.8 | 17 / 21 | 23 | 15 Mbit/s | no libobs pipeline delay; ~2 ms render+readback under the graphics lock per frame |
| JPEG GPU 1280x720, pipelined readback | 30.0 / 29.6 | 13 / 26 | 23 | 16 Mbit/s | two stage surfaces, map one tick late |
| JPEG GPU 640x360 | 28.8 / 28.5 | 6 / 25 | 12.4 | 5.2 Mbit/s | GPU downscale |
| H.264 VideoToolbox 1280x720 2.5 Mbit/s → WebCodecs | 29.0 / 23.8 | 169 / 188 | 9.6 | 2.3 Mbit/s | decode 158 ms avg: browser holds 5 frames because the VT SPS has no VUI bitstream_restriction (no `max_num_reorder_frames=0`) |
| H.264 VT 960x540 1.5 Mbit/s | 29.4 / 22.2 | 269 / 286 | 11.1 | 1.4 Mbit/s | 8-frame reorder window at the lower level |
| H.264 VT 640x360 1 Mbit/s | 29.4 / 21.6 | 268 / 287 | 9.9 | 0.9 Mbit/s | |
| H.264 x264 (veryfast, zerolatency) 1280x720 | 30.0 / 29.7 | 3 / 18 | 30.6 | 2.5 Mbit/s | x264 writes the VUI; decode 1.8 ms; software encode cost |
| H.264 x264 640x360 | 29.8 / 29.3 | 3 / 19 | 15.3 | 1.0 Mbit/s | |

Same headful (non-headless) Chrome, hardware decode confirmed: identical numbers.

## While a real live output runs (VideoToolbox H.264 4.5 Mbit/s + AAC over RTMP to a loopback ffmpeg)

Live output alone: host 11.0-11.1%. Every feed kept 29-30 fps received with zero host-side drops; the RTMP output kept running.

| feed | fps shown | paint p50 / p95 ms | host CPU % |
| --- | --- | --- | --- |
| JPEG raw 1280x720 | 28.4 | 11 / 28 | 31 |
| JPEG raw 640x360 | 29.8 | 9 / 17 | 21.5 |
| JPEG GPU 1280x720 | 28.6 | 14 / 29 | 25 |
| JPEG GPU 640x360 | 28.9 | 7 / 25 | 17.8 |
| H.264 VT 1280x720 | 24.2 | 170 / 184 | 13.4 |
| H.264 VT 640x360 | 22.2 | 270 / 283 | 16.6 |

Scaled H.264 encoders needed `obs_encoder_set_gpu_scale_type`; without it libobs downscales on the CPU and the 640x360 encoder cost more than 1280x720 (20% vs 13%).

## Scene switch, source removal

`scene.setProgram` mid-feed: next frame arrives 23-34 ms after the request, sequence numbers continuous, no gap over one frame for JPEG; H.264 shows one 129 ms gap and carries the new scene in delta frames until the next 1 s keyframe (16 frames). Removing the program's only source mid-feed: feed continues (black frames), no gap.

## Thumbnails (`preview.snapshot`, texrender → stage → JPEG, base64 in the RPC reply)

Program 320x180: first call 5-13 ms (shader warm-up), then 0.5-1.3 ms render + 0.1-0.2 ms encode, 3.5 KB. Source 160x90: 0.6-1.1 ms. While a feed runs the render rises to 3-6 ms (graphics-lock contention). Three targets at 1 Hz during a feed: no measurable CPU change, no feed drops.

## Daemon in the middle (page → Bun relay → host)

Adds 1-4 ms at p50 (JPEG raw 4/8 vs 3/5 ms; JPEG GPU 8/15 vs 8/10; H.264 1/1 vs 0/1). Frame rates unchanged.

## Not built

WebRTC (WHIP/WHEP): obs-webrtc is compiled out of the host bundle and libdatachannel has no local headers; a local-peer relay (Bun + node-datachannel, which loads and exposes an H.264 RTP packetizer) is feasible but sits on top of the same H.264 encode and adds RTP plus a jitter buffer. Not measured.

## Host facts surfaced

- Control server is single-client and blocking: one WebSocket at a time; a second client waits in `accept`.
- Raw-video callbacks see the frame ~40 ms after the render tick; the GPU texture is readable immediately.
- libobs output is limited-range YUV; JPEG from I420 planes needs `VIDEO_RANGE_FULL` in the conversion or it looks washed out.
- VideoToolbox H.264 SPS lacks the VUI bitstream restriction; any WebCodecs / MSE consumer will buffer 5-8 frames unless the SPS is rewritten or x264 is used.
