# Stream Mate Studio Host

Public GPL-2.0 native Studio Host for Stream Mate, built around a pinned, unmodified upstream `obs-studio` / `libobs` submodule.

This repository is the GPL boundary described by Stream Mate ADR-0005. The proprietary Stream Mate monorepo must not vendor this code or link GPL/libobs code into Station; integration happens only through a future loopback control protocol and signed/checksummed host artifacts.

## Current scope

Current prototype scope:

- GPL-2.0 repository.
- `external/obs-studio` submodule pinned by commit to upstream OBS Studio `32.1.2`.
- CMake scaffold for the `studio-host` binary plus a minimal headless `obs_startup` / `obs_shutdown` smoke target.
- Loopback-only WebSocket JSON-RPC control server. Implemented requests include `host.hello`/`host.health`/`host.shutdown`, scene commands (`scene.load`, `scene.setProgram`, `scene.captureFrame`), source commands (`source.create`, `source.update`, `source.mute`), OBS-config import (`import.scan`, `import.load`, `import.report`), the fake-ingest output surface (`output.configure`/`start`/`stop`/`status`), and `stats.subscribe`.
- Token-authenticated control connections, 5,000 ms heartbeat events, sanitized structured logs, and atomic state-file writes for supervisor probes.
- macOS CI scaffold for building the pinned upstream OBS sources without the Qt frontend, producing an ad-hoc-signed bundle artifact and sha256 manifest.
- Fork guard scripts that fail if the OBS submodule has local modifications or drifts from the recorded pin.

Output defaults to fake-ingest only (a synthetic loopback writer). Real RTMP/SRT live egress is default-off: `output.configure` refuses it unless a caller explicitly requests it against a libobs build, it is never enabled or exercised in CI, and — per the security posture below — turning it on requires explicit owner approval. Real macOS TCC rehearsal, Developer ID / notarized signing, and Stream Mate product logic remain out of scope — the IPC boundary is the GPL license boundary (ADR-0005).

## Pin-not-fork rule

`external/obs-studio` must stay an unmodified upstream submodule. Stream Mate-specific behavior belongs in this repository's own source tree or future OBS plugin modules, never as patches to `external/obs-studio`.

Current pin:

- Upstream: <https://github.com/obsproject/obs-studio>
- Tag: `32.1.2`
- Commit: `fb4d98bf88fae5fc85cb11fc57f7c5e309282194`

Changing this pin requires a normal PR and must re-run the fork guard, smoke target, sha256 manifest generation, and future import/parity regression suites.

## Local scaffold verification

```sh
git submodule update --init --recursive
./scripts/verify-obs-pin.sh
./scripts/fork-guard.sh
cmake -S . -B build/local -DSTREAMMATE_REQUIRE_LIBOBS=OFF
cmake --build build/local --target studio-host studio-host-smoke
./build/local/studio-host-smoke --version
ctest --test-dir build/local --output-on-failure
```

`STREAMMATE_REQUIRE_LIBOBS=OFF` is a scaffold-only fallback for machines without built libobs. CI is expected to configure the target with libobs enabled once the upstream OBS build dependencies are available.

## Control skeleton

The current `studio-host` binary accepts:

```sh
./build/local/studio-host --token <station-minted-token> --host 127.0.0.1 --port 0 --state-file /tmp/studio-host-state.json
```

It refuses non-loopback bind hosts, upgrades authorized WebSocket clients on `/control`, emits `host.started` / `host.ready` / periodic `host.health` events, and handles the JSON-RPC requests listed under Current scope (lifecycle, scene, source, import, output, and stats). The host has no Station journal authority; Station will translate host lifecycle observations into `studio.*` journal events in the monorepo adapter chunk.

The optional `--allow-live-egress` launch flag (default **off**) is a defense-in-depth gate over the caller-supplied `allowLiveEgress` JSON flag: unless the process was launched with `--allow-live-egress`, an `output.configure`/`output.start` request carrying `allowLiveEgress:true` is refused with a sanitized reason before any endpoint is contacted. Both gates must be open for the live-egress path; the fake loopback ingest path is unaffected. This is a provisional hardening pending owner ratification (Q-122); it is additive and trivially removable.

## Artifact/checksum posture

CI writes release-candidate artifacts under `dist/` and emits `dist/sha256-manifest.txt` with `shasum -a 256`. Downstream Stream Mate installation must verify this manifest before unpacking any host artifact.

The bundle is ad-hoc signed **inside-out**: `packaging/macos/package-app.sh` signs each nested component (dylibs, `*.plugin` bundles, `libobs.framework`, and the secondary executable) with its own per-component identifier first, then signs the outer `.app` on its own (no recursive pass) with the stable identifier `com.streammate.studio-host`. This gives each nested component its own distinct per-component identifier (namespaced under `com.streammate.studio-host.vendored.*`) instead of the outer app identifier, so `codesign --verify --strict --deep` passes; the earlier single recursive pass clobbered every nested identifier with the outer one and left the strict verification ambiguous. The identifier, the ad-hoc `--sign -` approach, and the install path are unchanged (Q-123, provisional). Ad-hoc signatures are content-derived, so packaging the same build tree twice is byte-deterministic; CI asserts both the strict verification and repack determinism before uploading the artifact (artifact upload does not preserve framework symlinks, so verification runs first).

## Security and authority posture

The host is intentionally thin:

- no policy, playbooks, connector, agent, MCP, or journal authority;
- future control server binds loopback only;
- no stream keys or credentials are persisted or echoed;
- any future live egress, real macOS TCC rehearsal, or non-ad-hoc signing requires explicit approval.
