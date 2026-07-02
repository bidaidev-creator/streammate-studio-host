# Stream Mate Studio Host

Public GPL-2.0 native Studio Host for Stream Mate, built around a pinned, unmodified upstream `obs-studio` / `libobs` submodule.

This repository is the GPL boundary described by Stream Mate ADR-0005. The proprietary Stream Mate monorepo must not vendor this code or link GPL/libobs code into Station; integration happens only through a future loopback control protocol and signed/checksummed host artifacts.

## Current scope

Chunk 2 scaffold only:

- GPL-2.0 repository.
- `external/obs-studio` submodule pinned by commit to upstream OBS Studio `32.1.2`.
- CMake scaffold for a minimal headless `obs_startup` / `obs_shutdown` smoke target.
- macOS CI scaffold for building the pinned upstream OBS sources without the Qt frontend, producing an ad-hoc-signed bundle artifact and sha256 manifest.
- Fork guard scripts that fail if the OBS submodule has local modifications or drifts from the recorded pin.

No control server, scene commands, import, output, live egress, real TCC rehearsal, or product logic exists in this chunk.

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
cmake --build build/local --target studio-host-smoke
./build/local/studio-host-smoke --version
```

`STREAMMATE_REQUIRE_LIBOBS=OFF` is a scaffold-only fallback for machines without built libobs. CI is expected to configure the target with libobs enabled once the upstream OBS build dependencies are available.

## Artifact/checksum posture

CI writes release-candidate artifacts under `dist/` and emits `dist/sha256-manifest.txt` with `shasum -a 256`. Downstream Stream Mate installation must verify this manifest before unpacking any host artifact.

## Security and authority posture

The host is intentionally thin:

- no policy, playbooks, connector, agent, MCP, or journal authority;
- future control server binds loopback only;
- no stream keys or credentials are persisted or echoed;
- any future live egress, real macOS TCC rehearsal, or non-ad-hoc signing requires explicit approval.
