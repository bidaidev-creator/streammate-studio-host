# Stream Mate Studio Host

Public GPL-2.0 native Studio Host for Stream Mate, built around a pinned, unmodified upstream `obs-studio` / `libobs` submodule.

This repository is the GPL boundary described by Stream Mate ADR-0005. The proprietary Stream Mate monorepo must not vendor this code or link GPL/libobs code into Station; integration happens only through a future loopback control protocol and signed/checksummed host artifacts.

## Current scope

Current prototype scope:

- GPL-2.0 repository.
- `external/obs-studio` submodule pinned by commit to upstream OBS Studio `32.1.2`.
- CMake scaffold for the `studio-host` binary plus a minimal headless `obs_startup` / `obs_shutdown` smoke target.
- Loopback-only WebSocket JSON-RPC control skeleton with `host.hello`, `host.health`, and `host.shutdown`.
- Token-authenticated control connections, 5,000 ms heartbeat events, sanitized structured logs, and atomic state-file writes for supervisor probes.
- macOS CI scaffold for building the pinned upstream OBS sources without the Qt frontend, producing an ad-hoc-signed bundle artifact and sha256 manifest.
- Fork guard scripts that fail if the OBS submodule has local modifications or drifts from the recorded pin.

Scene commands, import, output, live egress, real TCC rehearsal, and product logic remain out of scope.

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

It refuses non-loopback bind hosts, upgrades authorized WebSocket clients on `/control`, emits `host.started` / `host.ready` / periodic `host.health` events, and handles `host.hello`, `host.health`, and `host.shutdown` JSON-RPC requests. The host has no Station journal authority; Station will translate host lifecycle observations into `studio.*` journal events in the monorepo adapter chunk.

The optional `--allow-live-egress` launch flag (default **off**) is a defense-in-depth gate over the caller-supplied `allowLiveEgress` JSON flag: unless the process was launched with `--allow-live-egress`, an `output.configure`/`output.start` request carrying `allowLiveEgress:true` is refused with a sanitized reason before any endpoint is contacted. Both gates must be open for the live-egress path; the fake loopback ingest path is unaffected. This is a provisional hardening pending owner ratification (Q-122); it is additive and trivially removable.

## Artifact/checksum posture

CI writes release-candidate artifacts under `dist/` and emits `dist/sha256-manifest.txt` with `shasum -a 256`. Downstream Stream Mate installation must verify this manifest before unpacking any host artifact.

## Security and authority posture

The host is intentionally thin:

- no policy, playbooks, connector, agent, MCP, or journal authority;
- future control server binds loopback only;
- no stream keys or credentials are persisted or echoed;
- any future live egress, real macOS TCC rehearsal, or non-ad-hoc signing requires explicit approval.
