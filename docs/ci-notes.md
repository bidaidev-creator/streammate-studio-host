# Stream Mate Studio Host CI Notes

The macOS workflow is intentionally split into guard/build/package phases:

1. Verify the OBS pin and fork guard before any build.
2. Configure upstream OBS from the pinned submodule with the Qt frontend disabled.
3. Build `libobs` and the upstream modules required by ADR-0005 / Spec 18 Chunk 2.
4. Configure `studio-host-smoke` against the built libobs library.
5. Run the headless `obs_startup` / `obs_shutdown` smoke.
6. Package the smoke binary into an ad-hoc-signed app bundle placeholder and emit `dist/sha256-manifest.txt`.

The workflow may need dependency tuning as upstream OBS CMake options change. Any future pin bump must keep the submodule unmodified and make CI green in the same PR.
