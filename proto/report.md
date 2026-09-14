# Preview feed throwaway prototype

Created only these files under `/tmp/sm-host-src/proto/`:

- `serve.ts`: Bun HTTP server, loopback TCP discard sink, WebSocket relay, process and helper CPU endpoint.
- `preview.html`: inline vanilla JS controls, JPEG/WebCodecs rendering, snapshots, live measurement windows, eight-case matrix, results table and Markdown copy.
- `page-a.html` and `page-b.html`: contrasting 1280x720 canvas animations with frame counters and wall-clock timestamps.
- `report.md`: this report.

Existing `smoke.py`, `live_test.py`, `run-host.sh`, and `__pycache__` were preserved. No changes outside `proto/`, commits, pushes, dependency installs, global configuration changes, tests, or native builds were made.

## Run

From `/tmp/sm-host-src`:

```sh
bun proto/serve.ts
```

Open `http://127.0.0.1:8787/` in a browser supporting H.264 WebCodecs for the H.264 cases. Enter the running host's port and PID; the default token is `protok`. Connect direct or through the relay and wait for the four-second baseline. Only the page connects to the native control socket; the server makes the upstream connection only when relaying.

Click Setup scenes, allow a few seconds for CEF to paint, then start a manual feed or the matrix. Stop feed saves a manual row. Starting another feed also saves the previous run. Matrix cases run for eight seconds after the start response, then stop and save. Cancellation saves the partial run. Unsupported or failed cases get a marked row. Copy results table exports Markdown. Data stays in page memory.

For a baseline that includes the loaded browser sources, reconnect after scene setup. The baseline always stops preview first, but leaves the current scene and any previously started output in place. Disable snapshots before comparisons unless their cost is what you want to measure. A slow snapshot cycle skips overlapping one-second ticks.

The fake-output checkbox starts output before the matrix and stops it afterward if the matrix started it. An already-started output remains on. Output errors are logged without retries. The sink only accepts and discards TCP bytes: it does not perform an RTMP handshake. `fakeOutput=on` means `output.start` succeeded, not independent proof that encoding continues. Inspect the logged host events. This prototype deliberately does not infer output status from unspecified event payloads.

## Measurement definitions and protocol assumptions

- Binary WebSocket messages contain exactly one complete 32-byte big-endian header and JPEG picture or Annex B H.264 access unit. Sequence numbers wrap as unsigned 32-bit integers. Both u64 values use `getBigUint64` followed by `Number`, as requested. H.264 keyframes contain SPS/PPS. Feed counters other than cumulative process CPU are assumed to reset on `preview.start`.
- Arrival latency is `Date.now() - wallMs` at the browser message callback; paint latency uses the same formula immediately after `drawImage` in rAF. These measure browser delivery and drawing submission, not physical display scanout. Clock adjustments can affect these latencies; values are not clamped. Rates and duration use the monotonic performance clock.
- Live client metrics use successive roughly two-second windows with actual elapsed time as denominator. Run rows use all samples for that run, including startup and stop-response time. Percentiles use nearest rank over individual latency samples, not averages of window percentiles. Throughput and average payload size exclude the 32-byte header and WebSocket/TCP overhead.
- JPEG decoding has one operation in flight, the newest waiting JPEG, and one newest decoded picture awaiting rAF. Replaced pictures, decode failures, H.264 packets discarded before the first keyframe, and pending work discarded at stop count toward `displayDropped`. Decoders and pending frames are closed/reset between runs. H.264 decode timing matches callback timestamps to decode-call timestamps. Decode averages include successfully decoded pictures even if they are subsequently replaced before painting.
- Host CPU uses `delta(cpuTotalMs) / delta(timeMs) * 100`, with stats request/response midpoints estimating the sample time. The baseline uses a four-second delta; run CPU uses pre-start stats and final stop stats. Delta versus baseline is percentage points. Values can exceed 100% on multiple cores. These are different from the OS's `ps %cpu` averaging policy.
- Table `ps %cpu` and `helpersCpu` are means of available samples during the run. Missing samples show `n/a`. `/cpu` returns `cpu`, `rss` in KiB, `threads`, `helpersCpu`, `now`, and `errors`. Thread CPU is located by the `ps -M` header and sorted descending, with a TID or command when supplied by ps. If neither exists, the label is a thread-row ordinal. Helpers include all processes matching the requested `studio-host Helper` pattern.
- Host `avgEncodeUs` and `avgObsDelayUs` are the host-reported running averages; final rows use the stop response. Live busy/backpressure deltas accompany cumulative counts; table counts are final per-feed totals.
- Scene-switch gap is between the last received packet before the request and the first received packet afterward. The log reports sequence continuity modulo 2^32. Headers do not identify scenes, so this cannot confirm the first visible new-scene picture. Scene switches, source setup, snapshot activity, and hidden-tab runs are annotated in results.
- Relay forwarding retains text/binary message types and order. Bun handles socket buffering; exceeding the relay's 64 MiB downstream backpressure limit closes the connection rather than silently dropping preview packets. Switching transports closes the previous connection first.

## Verification and limits

Bun 1.4.0 is installed. Ran `bun proto/serve.ts`; Bun parsed the TypeScript and reached `Bun.listen`, but the managed sandbox denied the loopback bind with `EPERM`. The process exited with code 1. Both requested curl checks were attempted and failed to connect because the server could not start. No server was left running or needed killing.

A direct `ps -M -p 1` attempt was also blocked by the sandbox with `operation not permitted`. The CPU endpoint handles command failures as JSON with null values and errors, but its HTTP response and real macOS per-thread output could not be verified here. The endpoint checks must be repeated in a terminal permitted to bind localhost and inspect processes:

```sh
curl -s http://127.0.0.1:8787/config
curl -s 'http://127.0.0.1:8787/cpu?pid=1'
```

Stop the server with Ctrl-C afterward. All inline page scripts were re-read for syntax and lifecycle issues; no script extraction, `node --check`, build step, or tests were used. Browser rendering, relay traffic, native-host verbs, actual CPU measurements, and live-output behavior remain unverified. The native host was not started or built.

API references consulted: [Bun WebSockets](https://bun.sh/docs/runtime/http/websockets), [Bun TCP](https://bun.sh/docs/runtime/networking/tcp), and [VideoDecoder.configure](https://developer.mozilla.org/en-US/docs/Web/API/VideoDecoder/configure).
