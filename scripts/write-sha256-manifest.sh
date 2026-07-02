#!/usr/bin/env bash
set -euo pipefail

root="${1:-dist}"
manifest="$root/sha256-manifest.txt"

if [[ ! -d "$root" ]]; then
  echo "artifact directory does not exist: $root" >&2
  exit 1
fi

tmp="$(mktemp)"
find "$root" -type f ! -name 'sha256-manifest.txt' -print0 \
  | sort -z \
  | xargs -0 shasum -a 256 > "$tmp"

mv "$tmp" "$manifest"
echo "wrote $manifest"
