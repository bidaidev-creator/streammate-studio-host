#!/usr/bin/env bash
set -euo pipefail

# Byte-order collation everywhere: the CEF payload introduced mixed-case names
# (libEGL.dll vs libcef.dll) whose relative order differs between the C locale
# and UTF-8 locales, which would make the manifest and tarball ordering depend
# on the packaging machine's locale instead of the payload.
export LC_ALL=C

host_bin=""
smoke_bin=""
obs_dll_dir=""
deps_bin_dir=""
graphics_module=""
obs_modules_dir=""
streammate_plugin=""
libobs_data_dir=""
obs_browser_plugin=""
obs_browser_page=""
cef_release_dir=""
cef_resources_dir=""
obs_frontend_api=""
obs_browser_data_dir=""
qt_bin_dir=""
output_dir="dist"

usage() {
  cat >&2 <<'USAGE'
usage: package-dist.sh --host-bin PATH --smoke-bin PATH \
  --obs-dll-dir PATH --deps-bin-dir PATH --graphics-module PATH \
  --obs-modules-dir PATH --streammate-plugin PATH --libobs-data-dir PATH \
  [--obs-browser-plugin PATH --obs-browser-page PATH --cef-release-dir PATH \
   --cef-resources-dir PATH --obs-frontend-api PATH --obs-browser-data-dir PATH \
   --qt-bin-dir PATH] \
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
    --obs-browser-plugin) obs_browser_plugin="${2:-}"; shift 2 ;;
    --obs-browser-page) obs_browser_page="${2:-}"; shift 2 ;;
    --cef-release-dir) cef_release_dir="${2:-}"; shift 2 ;;
    --cef-resources-dir) cef_resources_dir="${2:-}"; shift 2 ;;
    --obs-frontend-api) obs_frontend_api="${2:-}"; shift 2 ;;
    --obs-browser-data-dir) obs_browser_data_dir="${2:-}"; shift 2 ;;
    --qt-bin-dir) qt_bin_dir="${2:-}"; shift 2 ;;
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

# obs-browser is optional for callers that package a non-CEF build, but its
# runtime is indivisible: the module, subprocess executable, CEF binaries and
# resources, frontend API dependency, and module data must travel together.
cef_enabled=0
if [[ -n "$obs_browser_plugin" || -n "$obs_browser_page" || -n "$cef_release_dir" ||
      -n "$cef_resources_dir" || -n "$obs_frontend_api" || -n "$obs_browser_data_dir" ||
      -n "$qt_bin_dir" ]]; then
  cef_enabled=1
  require_file "obs-browser plugin" "$obs_browser_plugin"
  require_file "obs-browser page executable" "$obs_browser_page"
  require_dir "CEF Release directory" "$cef_release_dir"
  require_dir "CEF Resources directory" "$cef_resources_dir"
  require_file "obs-frontend-api DLL" "$obs_frontend_api"
  require_dir "obs-browser module data directory" "$obs_browser_data_dir"
  # Windows obs-browser is built in the panels configuration (the only
  # upstream-supported one), so it load-depends on the Qt runtime.
  require_dir "Qt runtime bin directory" "$qt_bin_dir"
  for qt_dll in Qt6Core.dll Qt6Gui.dll Qt6Widgets.dll; do
    require_file "Qt runtime DLL $qt_dll" "$qt_bin_dir/$qt_dll"
  done
  if [[ "$(basename "$obs_browser_plugin" | tr '[:upper:]' '[:lower:]')" != "obs-browser.dll" ]]; then
    echo "obs-browser plugin must be obs-browser.dll: $obs_browser_plugin" >&2
    exit 1
  fi
  if [[ "$(basename "$obs_browser_page" | tr '[:upper:]' '[:lower:]')" != "obs-browser-page.exe" ]]; then
    echo "obs-browser page executable must be obs-browser-page.exe: $obs_browser_page" >&2
    exit 1
  fi
  if [[ "$(basename "$obs_frontend_api" | tr '[:upper:]' '[:lower:]')" != "obs-frontend-api.dll" ]]; then
    echo "obs-frontend-api DLL must be obs-frontend-api.dll: $obs_frontend_api" >&2
    exit 1
  fi

  for cef_file in libcef.dll chrome_elf.dll libEGL.dll libGLESv2.dll v8_context_snapshot.bin; do
    require_file "CEF Release payload $cef_file" "$cef_release_dir/$cef_file"
  done
  for cef_file in chrome_100_percent.pak chrome_200_percent.pak icudtl.dat resources.pak; do
    require_file "CEF Resources payload $cef_file" "$cef_resources_dir/$cef_file"
  done
  require_dir "CEF locales directory" "$cef_resources_dir/locales"
fi

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

if [[ "$cef_enabled" -eq 1 ]]; then
  cp "$obs_browser_plugin" "$plugins/obs-browser.dll"
  cp "$obs_browser_page" "$plugins/obs-browser-page.exe"
  cp "$obs_frontend_api" "$stage/obs-frontend-api.dll"
  for cef_file in libcef.dll chrome_elf.dll libEGL.dll libGLESv2.dll v8_context_snapshot.bin; do
    cp "$cef_release_dir/$cef_file" "$plugins/$cef_file"
  done
  for cef_file in chrome_100_percent.pak chrome_200_percent.pak icudtl.dat resources.pak; do
    cp "$cef_resources_dir/$cef_file" "$plugins/$cef_file"
  done
  mkdir -p "$plugins/locales" "$plugin_data/obs-browser"
  cp -R "$cef_resources_dir/locales/." "$plugins/locales/"
  cp -R "$obs_browser_data_dir/." "$plugin_data/obs-browser/"
  # Beside obs-browser.dll: libobs loads modules with LOAD_WITH_ALTERED_SEARCH_PATH,
  # so the module's own directory resolves its Qt load-time dependents.
  for qt_dll in Qt6Core.dll Qt6Gui.dll Qt6Widgets.dll; do
    cp "$qt_bin_dir/$qt_dll" "$plugins/$qt_dll"
  done
fi

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
  # gzip -n, via an explicit pipe: tar -z lets the compressor stamp the gzip
  # header MTIME (BSD tar stamps the current second, so two packagings that
  # straddle a second boundary differed byte-wise even with identical tar
  # contents). gzip -n on a stream writes MTIME 0 and no name on both paths.
  if tar --version 2>&1 | grep -q 'GNU tar'; then
    COPYFILE_DISABLE=1 tar --sort=name --mtime='UTC 1980-01-01' \
      --owner=0 --group=0 --numeric-owner -cf - \
      --null -T "$archive_list" | gzip -n > "$PWD/../StreamMateStudioHost-windows-x64.tar.gz"
  else
    COPYFILE_DISABLE=1 tar --uid 0 --gid 0 --uname root --gname root \
      -cf - --null -T "$archive_list" | gzip -n > "$PWD/../StreamMateStudioHost-windows-x64.tar.gz"
  fi
)
rm -f "$archive_list"

echo "packaged $stage"
echo "wrote $manifest"
echo "wrote $archive"
