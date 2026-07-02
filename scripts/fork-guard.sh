#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

git submodule update --init --recursive external/obs-studio

if ! git diff --quiet -- external/obs-studio .gitmodules; then
  echo "obs-studio submodule or .gitmodules has unstaged differences" >&2
  git diff -- external/obs-studio .gitmodules >&2
  exit 1
fi

if ! git -C external/obs-studio diff --quiet --ignore-submodules=none; then
  echo "obs-studio submodule has local modifications" >&2
  git -C external/obs-studio diff --stat >&2
  exit 1
fi

if [[ -n "$(git -C external/obs-studio status --short)" ]]; then
  echo "obs-studio submodule is not clean" >&2
  git -C external/obs-studio status --short >&2
  exit 1
fi

./scripts/verify-obs-pin.sh

echo "fork guard ok: obs-studio submodule is pinned and unmodified"
