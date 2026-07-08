#!/usr/bin/env bash
set -euo pipefail

host_bin=""
smoke_bin=""
info_plist=""
libobs_framework=""
obs_modules_dir=""
obs_deps_lib_dir=""
obs_graphics_module=""
output_dir="dist"
skip_codesign=0
skip_install_name_tool=0

usage() {
  cat >&2 <<'USAGE'
usage: package-app.sh --host-bin PATH --smoke-bin PATH --info-plist PATH \
  --libobs-framework PATH --obs-modules-dir PATH --obs-deps-lib-dir PATH \
  --obs-graphics-module PATH \
  [--output-dir PATH] [--skip-codesign] [--skip-install-name-tool]
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host-bin) host_bin="${2:-}"; shift 2 ;;
    --smoke-bin) smoke_bin="${2:-}"; shift 2 ;;
    --info-plist) info_plist="${2:-}"; shift 2 ;;
    --libobs-framework) libobs_framework="${2:-}"; shift 2 ;;
    --obs-modules-dir) obs_modules_dir="${2:-}"; shift 2 ;;
    --obs-deps-lib-dir) obs_deps_lib_dir="${2:-}"; shift 2 ;;
    --obs-graphics-module) obs_graphics_module="${2:-}"; shift 2 ;;
    --output-dir) output_dir="${2:-}"; shift 2 ;;
    --skip-codesign) skip_codesign=1; shift ;;
    --skip-install-name-tool) skip_install_name_tool=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

require_file() {
  local label="$1"
  local path="$2"
  if [[ -z "$path" || ! -f "$path" ]]; then
    echo "$label is required: $path" >&2
    exit 1
  fi
}

require_dir() {
  local label="$1"
  local path="$2"
  if [[ -z "$path" || ! -d "$path" ]]; then
    echo "$label is required: $path" >&2
    exit 1
  fi
}

require_file "studio-host binary" "$host_bin"
require_file "studio-host-smoke binary" "$smoke_bin"
require_file "Info.plist" "$info_plist"
require_dir "libobs framework" "$libobs_framework"
require_dir "OBS modules directory" "$obs_modules_dir"
require_dir "OBS deps library directory" "$obs_deps_lib_dir"
require_file "OBS graphics module" "$obs_graphics_module"

app="$output_dir/StreamMateStudioHost.app"
contents="$app/Contents"
macos="$contents/MacOS"
frameworks="$contents/Frameworks"
plugins="$contents/PlugIns/obs-plugins"

rm -rf "$output_dir"
mkdir -p "$macos" "$frameworks" "$plugins"

cp "$host_bin" "$macos/studio-host"
cp "$smoke_bin" "$macos/studio-host-smoke"
chmod 755 "$macos/studio-host" "$macos/studio-host-smoke"
cp "$info_plist" "$contents/Info.plist"

/usr/bin/python3 - "$contents/Info.plist" <<'PY'
import plistlib
import sys
from pathlib import Path
path = Path(sys.argv[1])
with path.open('rb') as handle:
    data = plistlib.load(handle)
data['CFBundleIdentifier'] = 'com.streammate.studio-host'
data['CFBundleExecutable'] = 'studio-host'
data.setdefault('CFBundleName', 'Stream Mate Studio Host')
data.setdefault('CFBundlePackageType', 'APPL')
with path.open('wb') as handle:
    plistlib.dump(data, handle, sort_keys=False)
PY

cp -R "$libobs_framework" "$frameworks/"
cp "$obs_graphics_module" "$frameworks/libobs-opengl.dylib"

shopt -s nullglob
for dylib in "$obs_deps_lib_dir"/*.dylib; do
  cp -R "$dylib" "$frameworks/"
done
for bundle in "$obs_modules_dir"/*.plugin "$obs_modules_dir"/*.so "$obs_modules_dir"/*.dylib; do
  cp -R "$bundle" "$plugins/"
done
shopt -u nullglob

required_modules=(mac-avcapture mac-capture obs-outputs obs-x264)
missing_modules=()
for module in "${required_modules[@]}"; do
  if [[ ! -e "$plugins/$module.plugin" && ! -e "$plugins/$module.so" && ! -e "$plugins/$module.dylib" ]]; then
    missing_modules+=("$module")
  fi
done
if [[ ${#missing_modules[@]} -gt 0 ]]; then
  echo "required OBS modules missing from app bundle: ${missing_modules[*]}" >&2
  exit 1
fi

if [[ ! -f "$frameworks/libobs-opengl.dylib" ]]; then
  echo "required OBS graphics module missing from app bundle" >&2
  exit 1
fi

if [[ $skip_install_name_tool -eq 0 && -x /usr/bin/install_name_tool ]]; then
  for exe in "$macos/studio-host" "$macos/studio-host-smoke"; do
    /usr/bin/install_name_tool -add_rpath '@executable_path/../Frameworks' "$exe" 2>/dev/null || true
    /usr/bin/install_name_tool -add_rpath '@executable_path/../PlugIns/obs-plugins' "$exe" 2>/dev/null || true
  done
fi

if [[ $skip_codesign -eq 0 ]]; then
  # Inside-out ad-hoc signing (Spec 34 Capability 8 / Q-123). Sign nested code
  # (dylibs, OBS plugin bundles, libobs.framework, the secondary executable)
  # each with its own distinct namespaced identifier first, then sign the outer
  # bundle on its own (no recursive pass). The previous single recursive signing
  # pass propagated the outer com.streammate.studio-host identifier onto every
  # nested component, producing the recorded strictDeepCodesignOk:false ambiguity.
  # The bundle identifier (com.streammate.studio-host), the ad-hoc signing
  # approach, and the install path are byte-for-byte unchanged; reverting to
  # the prior behavior is a single-block change if Q-123 is not ratified.
  sign_adhoc() {
    # $1 = code-signing identifier, $2 = target path. --timestamp=none keeps
    # the ad-hoc signature offline and content-derived (repack determinism).
    /usr/bin/codesign --force --sign - --timestamp=none --identifier "$1" "$2"
  }
  nested_identifier() {
    # Derive a stable per-component identifier from the basename, namespaced
    # under our reverse-DNS so it can never equal the outer app identifier.
    local base
    base="$(basename "$1")"
    base="${base%.*}"
    base="$(printf '%s' "$base" | LC_ALL=C tr -c 'A-Za-z0-9.-' '-')"
    printf 'com.streammate.studio-host.vendored.%s' "$base"
  }

  shopt -s nullglob
  # 1. Leaf dynamic libraries staged in Contents/Frameworks.
  for dylib in "$frameworks"/*.dylib; do
    sign_adhoc "$(nested_identifier "$dylib")" "$dylib"
  done
  # 2. OBS plugin/module bundles in Contents/PlugIns/obs-plugins.
  for module in "$plugins"/*.plugin "$plugins"/*.so "$plugins"/*.dylib; do
    sign_adhoc "$(nested_identifier "$module")" "$module"
  done
  # 3. Versioned frameworks (deepest bundles) after their own leaf code.
  for framework_bundle in "$frameworks"/*.framework; do
    sign_adhoc "$(nested_identifier "$framework_bundle")" "$framework_bundle"
  done
  # 4. Secondary Mach-O executable (the main executable is signed with the app).
  sign_adhoc "com.streammate.studio-host.vendored.smoke" "$macos/studio-host-smoke"
  shopt -u nullglob
  # 5. Outer bundle on its own (no recursive pass), stable app identifier.
  /usr/bin/codesign --force --sign - --identifier com.streammate.studio-host "$app"
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$script_dir/../../scripts/write-sha256-manifest.sh" "$output_dir"

echo "packaged $app"
