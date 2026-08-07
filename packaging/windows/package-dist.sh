#!/usr/bin/env bash
set -euo pipefail

host_bin=""
smoke_bin=""
obs_dll_dir=""
deps_bin_dir=""
graphics_module=""
obs_modules_dir=""
streammate_plugin=""
libobs_data_dir=""
output_dir="dist"

usage() {
  cat >&2 <<'USAGE'
usage: package-dist.sh --host-bin PATH --smoke-bin PATH \
  --obs-dll-dir PATH --deps-bin-dir PATH --graphics-module PATH \
  --obs-modules-dir PATH --streammate-plugin PATH --libobs-data-dir PATH \
  [--output-dir PATH]
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host-bin) host_bin="${2:-}"; shift 2 ;;
    --smoke-bin) smoke_bin="${2:-}"; shift 2 ;;
    --obs-dll-dir) obs_dll_dir="${2:-}"; shift 2 ;;
    --deps-bin-dir) deps_bin_dir="${2:-}"; shift 2 ;;
    --graphics-module) graphics_module="${2:-}"; shift 2 ;;
    --obs-modules-dir) obs_modules_dir="${2:-}"; shift 2 ;;
    --streammate-plugin) streammate_plugin="${2:-}"; shift 2 ;;
    --libobs-data-dir) libobs_data_dir="${2:-}"; shift 2 ;;
    --output-dir) output_dir="${2:-}"; shift 2 ;;
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
require_dir "obs.dll directory" "$obs_dll_dir"
require_dir "OBS deps bin directory" "$deps_bin_dir"
require_file "OBS graphics module" "$graphics_module"
require_dir "OBS modules directory" "$obs_modules_dir"
require_file "Stream Mate plugin" "$streammate_plugin"
require_dir "libobs data directory" "$libobs_data_dir"

obs_dll="$(find "$obs_dll_dir" -maxdepth 1 -type f -iname 'obs.dll' -print -quit)"
require_file "obs.dll" "$obs_dll"
if [[ "$(basename "$graphics_module" | tr '[:upper:]' '[:lower:]')" != "libobs-d3d11.dll" ]]; then
  echo "OBS graphics module must be libobs-d3d11.dll: $graphics_module" >&2
  exit 1
fi
if [[ "$(basename "$streammate_plugin" | tr '[:upper:]' '[:lower:]')" != "streammate-native-overlay.dll" ]]; then
  echo "Stream Mate plugin must be streammate-native-overlay.dll: $streammate_plugin" >&2
  exit 1
fi
require_file "libobs default effect" "$libobs_data_dir/default.effect"
require_dir "OBS module data directory" "$obs_modules_dir/data"

if [[ -z "$output_dir" || "$output_dir" == "/" ]]; then
  echo "refusing unsafe output directory: $output_dir" >&2
  exit 1
fi

rm -rf "$output_dir"
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd)"
stage="$output_dir/StreamMateStudioHost"
plugins="$stage/obs-plugins/64bit"
plugin_data="$stage/data/obs-plugins"
core_data="$stage/data/libobs"

mkdir -p "$stage" "$plugins" "$plugin_data" "$core_data"

cp "$host_bin" "$stage/studio-host.exe"
cp "$smoke_bin" "$stage/studio-host-smoke.exe"

copy_flat_dlls() {
  local source_dir="$1"
  while IFS= read -r -d '' dll; do
    cp "$dll" "$stage/$(basename "$dll")"
  done < <(find "$source_dir" -maxdepth 1 -type f -iname '*.dll' -print0 | sort -z)
}

# Windows resolves dependent DLLs beside the executable. Stage every runtime
# DLL from the pinned libobs output and obs-deps bin directory into that root.
copy_flat_dlls "$deps_bin_dir"
copy_flat_dlls "$obs_dll_dir"
cp "$obs_dll" "$stage/obs.dll"
cp "$graphics_module" "$stage/libobs-d3d11.dll"

while IFS= read -r -d '' module; do
  cp "$module" "$plugins/$(basename "$module")"
done < <(find "$obs_modules_dir" -maxdepth 1 -type f -iname '*.dll' -print0 | sort -z)
cp "$streammate_plugin" "$plugins/streammate-native-overlay.dll"
cp -R "$obs_modules_dir/data/." "$plugin_data/"
cp -R "$libobs_data_dir/." "$core_data/"

required_modules=(obs-outputs obs-x264 rtmp-services win-capture win-wasapi)
for module in "${required_modules[@]}"; do
  require_file "required OBS module $module" "$plugins/$module.dll"
  require_dir "required OBS module data $module" "$plugin_data/$module"
done

# Normalize archive-visible metadata. Modes are fixed explicitly; mtimes use
# the ZIP epoch because it is representable on Windows and every Unix runner.
find "$stage" -type d -exec chmod 755 {} +
find "$stage" -type f -exec chmod 644 {} +
chmod 755 "$stage/studio-host.exe" "$stage/studio-host-smoke.exe"
find "$stage" -exec touch -t 198001010000.00 {} +

manifest="$output_dir/sha256-manifest.txt"
manifest_tmp="$output_dir/.sha256-manifest.tmp"
(
  cd "$stage"
  if command -v sha256sum >/dev/null 2>&1; then
    # Windows coreutils defaults to binary mode and emits "HASH *path"; the
    # sed rewrites only that one-space-asterisk marker to the canonical
    # two-space form. Digests are computed on raw bytes either way.
    find . -type f -print0 | sort -z | xargs -0 sha256sum \
      | sed -e 's/^\([0-9a-f]\{64\}\) \*/\1  /'
  else
    # Local macOS contract tests use shasum; its output is byte-compatible
    # with sha256sum (digest, two spaces, relative path).
    find . -type f -print0 | sort -z | xargs -0 shasum -a 256
  fi
) > "$manifest_tmp"
mv "$manifest_tmp" "$manifest"

archive="$output_dir/StreamMateStudioHost-windows-x64.tar.gz"
archive_list="$output_dir/.archive-files"
(
  cd "$stage"
  find . -type f -print0 | sort -z > "$archive_list"
  if tar --version 2>&1 | grep -q 'GNU tar'; then
    COPYFILE_DISABLE=1 tar --sort=name --mtime='UTC 1980-01-01' \
      --owner=0 --group=0 --numeric-owner -czf "$PWD/../StreamMateStudioHost-windows-x64.tar.gz" \
      --null -T "$archive_list"
  else
    COPYFILE_DISABLE=1 tar --uid 0 --gid 0 --uname root --gname root \
      -czf "$PWD/../StreamMateStudioHost-windows-x64.tar.gz" --null -T "$archive_list"
  fi
)
rm -f "$archive_list"

echo "packaged $stage"
echo "wrote $manifest"
echo "wrote $archive"
