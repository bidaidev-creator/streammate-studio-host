#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

expected_commit="$(awk -F= '$1 == "commit" { print $2 }' OBS_PIN)"
actual_commit="$(git -C external/obs-studio rev-parse HEAD)"

if [[ "$actual_commit" != "$expected_commit" ]]; then
  echo "obs-studio pin mismatch" >&2
  echo "expected: $expected_commit" >&2
  echo "actual:   $actual_commit" >&2
  exit 1
fi

expected_tag="$(awk -F= '$1 == "tag" { print $2 }' OBS_PIN)"
if ! git -C external/obs-studio describe --tags --exact-match HEAD | grep -Fxq "$expected_tag"; then
  echo "obs-studio HEAD is not exactly tag $expected_tag" >&2
  exit 1
fi

echo "obs-studio pin ok: $expected_tag $actual_commit"
