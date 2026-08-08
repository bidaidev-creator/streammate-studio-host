# Stream Mate studio-host — third-party notices and corresponding-source offer

This document accompanies every packaged Stream Mate studio-host payload
(`StreamMateStudioHost.app` on macOS; the `StreamMateStudioHost` distribution
root on Windows). The payload is distributed inside the `@streammate/station`
npm package as a platform-specific archive (mere aggregation; the npm package's
own code is not derived from the GPL payload). A copy of this file ships at the
payload root as `THIRD-PARTY-NOTICES.md`, and a machine-readable pin record
ships beside it as `PAYLOAD-PROVENANCE.json`.

## The studio-host itself

- **streammate-studio-host** — GPL-2.0-only (see `LICENSE`).
  Source: this repository, <https://github.com/bidaidev-creator/streammate-studio-host>.
  Every packaged payload is built by this repository's CI from a single commit;
  that exact commit is recorded in the payload's `PAYLOAD-PROVENANCE.json`
  (`revision`).

## Third-party components in the packaged payload

Every third-party native component is consumed **unmodified** at a pinned
upstream revision. The authoritative pins are the committed `OBS_PIN` and
`DEPS_PIN` files in this repository; the table below restates them for the
revision this document was committed at.

| Component | Pin | License | Source |
| --- | --- | --- | --- |
| OBS Studio: libobs and the bundled OBS modules (obs-outputs, obs-x264, rtmp-services, platform capture/audio modules, obs-browser, obs-frontend-api) | tag `32.1.2`, commit `fb4d98bf88fae5fc85cb11fc57f7c5e309282194` (`OBS_PIN`) | GPL-2.0-or-later | <https://github.com/obsproject/obs-studio> |
| obs-deps prebuilt runtime (FFmpeg, x264, and the other native runtime libraries OBS builds against) | obs-deps release `2025-08-23` (`DEPS_PIN`) | per component: LGPL-2.1-or-later, GPL-2.0-or-later, and other FOSS licenses | <https://github.com/obsproject/obs-deps> (build scripts and exact source pins at the release tag) |
| Qt 6 runtime (obs-deps qt6 build) | obs-deps release `2025-08-23` (`DEPS_PIN`) | LGPL-3.0-only | <https://github.com/obsproject/obs-deps>; Qt sources: <https://download.qt.io/official_releases/qt/> |
| Chromium Embedded Framework (CEF) with Chromium | obs-project CEF distribution `cef_binary_6533` (`DEPS_PIN`), pinned and hash-verified by OBS Studio's `CMakePresets.json` at `OBS_PIN` | CEF: BSD-3-Clause; Chromium: BSD-3-Clause plus the licenses bundled in the CEF binary distribution's `LICENSE.txt` | <https://bitbucket.org/chromiumembedded/cef> |

## Corresponding-source offer (GPL / LGPL components)

Complete corresponding source for the studio-host and for every
copyleft-licensed component of the packaged payload is available at the exact
pinned revisions above:

- **studio-host**: this repository at the `revision` recorded in the payload's
  `PAYLOAD-PROVENANCE.json`;
- **libobs and the OBS modules**: the OBS Studio repository at `OBS_PIN`;
- **FFmpeg, x264, and the rest of the prebuilt runtime, and Qt 6**: the
  obs-deps release recorded in `DEPS_PIN`, whose build scripts pin and fetch
  each component's exact upstream source; Qt sources are additionally
  available from the Qt Project at the version recorded in that release.

If you received a packaged studio-host payload and cannot obtain the
corresponding source from the URLs above, open an issue in this repository —
we will provide the corresponding source at no charge. This offer is valid for
at least three years from your receipt of the payload (GPL-2.0 §3, LGPL).

The LGPL components (Qt 6, FFmpeg's LGPL libraries, and the other LGPL runtime
libraries) are dynamically linked shared libraries in the payload
(`Contents/Frameworks` in the macOS app bundle; DLLs in the Windows
distribution root), so they can be replaced with modified builds as the LGPL
requires.

## Attribution notices

### Chromium Embedded Framework

```
Copyright (c) 2008-2020 Marshall A. Greenblatt. Portions Copyright (c)
2006-2009 Google Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above
     copyright notice, this list of conditions and the following disclaimer
     in the documentation and/or other materials provided with the
     distribution.
   * Neither the name of Google Inc. nor the name Chromium Embedded
     Framework nor the names of its contributors may be used to endorse
     or promote products derived from this software without specific prior
     written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

The CEF binary distribution additionally bundles the licenses of Chromium and
its third-party components in its own `LICENSE.txt`; those terms accompany the
CEF framework as shipped.

### OBS Studio, FFmpeg, x264, Qt

OBS Studio is Copyright (C) Lain Bailey and OBS Studio contributors, licensed
GPL-2.0-or-later. FFmpeg is a trademark of Fabrice Bellard; the FFmpeg
libraries shipped in the prebuilt runtime are licensed LGPL-2.1-or-later with
GPL components as configured by obs-deps. x264 is Copyright (C) x264 project,
licensed GPL-2.0-or-later. Qt is Copyright (C) The Qt Company Ltd., licensed
LGPL-3.0-only as shipped here. Full license texts accompany each component's
source at the pinned revisions above.
