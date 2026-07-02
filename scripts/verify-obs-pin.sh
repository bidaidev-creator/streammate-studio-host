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
upstream="$(awk -F= '$1 == "upstream" { print $2 }' OBS_PIN)"
if git -C external/obs-studio describe --tags --exact-match HEAD >/tmp/streammate-obs-tag 2>/dev/null; then
  if ! grep -Fxq "$expected_tag" /tmp/streammate-obs-tag; then
    echo "obs-studio HEAD is not exactly tag $expected_tag" >&2
    exit 1
  fi
else
  remote_tag_commit="$(git ls-remote --tags "$upstream" "refs/tags/${expected_tag}^{}" | awk '{ print $1 }')"
  if [[ -z "$remote_tag_commit" ]]; then
    remote_tag_commit="$(git ls-remote --tags "$upstream" "refs/tags/${expected_tag}" | awk '{ print $1 }')"
  fi
  if [[ "$remote_tag_commit" != "$expected_commit" ]]; then
    echo "obs-studio local checkout has no tag metadata and remote tag does not resolve to the expected commit" >&2
    echo "tag:      $expected_tag" >&2
    echo "expected: $expected_commit" >&2
    echo "remote:   $remote_tag_commit" >&2
    exit 1
  fi
fi

echo "obs-studio pin ok: $expected_tag $actual_commit"
