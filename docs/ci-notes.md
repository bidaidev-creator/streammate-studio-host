# Stream Mate Studio Host CI Notes

The macOS workflow is intentionally split into guard/build/package phases:

1. Verify the OBS pin and fork guard before any build.
2. Configure upstream OBS from the pinned submodule with the Qt frontend disabled.
3. Build `libobs` and the upstream modules required by ADR-0005 / Spec 18 Chunk 2.
4. Configure `studio-host-smoke` against the built libobs library.
5. Run the headless `obs_startup` / `obs_shutdown` smoke.
6. Package the binaries into an ad-hoc-signed app bundle and emit `dist/sha256-manifest.txt`.
7. Verify the ad-hoc signature and repack determinism **before** uploading the artifact.

The workflow may need dependency tuning as upstream OBS CMake options change. Any future pin bump must keep the submodule unmodified and make CI green in the same PR.

## Inside-out code signing (Q-123)

`packaging/macos/package-app.sh` signs the bundle inside-out: each nested component (dylibs, `*.plugin` bundles, `libobs.framework`, and the secondary `studio-host-smoke` executable) is ad-hoc signed with its own per-component identifier first, then the outer `.app` is signed on its own (no recursive pass) with the stable identifier `com.streammate.studio-host`. A single recursive signing pass previously propagated the outer identifier onto every nested component and left `codesign --verify --strict --deep` ambiguous (`bundle format is ambiguous`); inside-out signing keeps each component's identity intact so the strict verification passes.

CI runs the strict verification and a repack-determinism check on the real bundle **before** `actions/upload-artifact`. The upload step does not preserve the framework's `Versions/Current` symlinks, so any verification performed on the downloaded artifact would be meaningless — the strict verify has to run on the freshly packaged bundle. Ad-hoc signatures are content-derived (no secure timestamp), so repackaging the same build tree twice yields byte-identical `sha256-manifest.txt` output.

The identifier `com.streammate.studio-host`, the ad-hoc `--sign -` approach, and the install path are unchanged; this is a mechanics-only change flagged Q-123 (`owner-ratification-pending`) and is a single-block revert away from the prior behavior.
