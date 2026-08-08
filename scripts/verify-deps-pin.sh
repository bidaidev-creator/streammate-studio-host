#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps_pin="$repo_root/DEPS_PIN"
obs_tree="$repo_root/external/obs-studio"
presets="$obs_tree/CMakePresets.json"

if [[ ! -f "$deps_pin" ]]; then
  echo "dependency pin file is required: $deps_pin" >&2
  exit 1
fi

read_pin_value() {
  local key="$1"
  local matches
  matches="$(sed -n "s/^${key}=\([^[:space:]][^[:space:]]*\)$/\1/p" "$deps_pin")"
  if [[ -z "$matches" || "$matches" == *$'\n'* ]]; then
    echo "DEPS_PIN must contain exactly one well-formed $key entry" >&2
    exit 1
  fi
  printf '%s' "$matches"
}

while IFS= read -r line || [[ -n "$line" ]]; do
  case "$line" in
    prebuilt=*|qt6=*|cef=*) ;;
    *)
      echo "DEPS_PIN contains an unknown or malformed entry: $line" >&2
      exit 1
      ;;
  esac
done < "$deps_pin"

expected_prebuilt="$(read_pin_value prebuilt)"
expected_qt6="$(read_pin_value qt6)"
expected_cef="$(read_pin_value cef)"

if [[ ! "$expected_prebuilt" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ||
      ! "$expected_qt6" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ||
      ! "$expected_cef" =~ ^[0-9]+$ ]]; then
  echo "DEPS_PIN values are malformed" >&2
  exit 1
fi

if [[ ! -f "$presets" ]]; then
  if [[ -d "$obs_tree" && -n "$(find "$obs_tree" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
    echo "obs-studio checkout is present but CMakePresets.json is missing: $presets" >&2
    exit 1
  fi
  echo "SKIP: external/obs-studio is not populated; dependency pin drift was not verified" >&2
  exit 0
fi

if ! grep -Fq '"obsproject.com/obs-studio"' "$presets" ||
   ! grep -Fq '"dependencies"' "$presets"; then
  echo "OBS CMakePresets.json lacks the obsproject.com/obs-studio dependencies vendor record" >&2
  exit 1
fi

upstream_version() {
  local dependency="$1"
  awk -v dependency="$dependency" '
    index($0, "\"" dependency "\"") { in_dependency = 1; next }
    in_dependency && /"version"[[:space:]]*:/ {
      value = $0
      sub(/^.*"version"[[:space:]]*:[[:space:]]*"/, "", value)
      sub(/".*$/, "", value)
      print value
      exit
    }
  ' "$presets"
}

for dependency in prebuilt qt6 cef; do
  case "$dependency" in
    prebuilt) expected="$expected_prebuilt" ;;
    qt6) expected="$expected_qt6" ;;
    cef) expected="$expected_cef" ;;
  esac
  actual="$(upstream_version "$dependency")"
  if [[ -z "$actual" ]]; then
    echo "OBS CMakePresets.json has no version for dependency $dependency" >&2
    exit 1
  fi
  if [[ "$actual" != "$expected" ]]; then
    echo "obs-deps pin mismatch for $dependency" >&2
    echo "expected: $expected" >&2
    echo "actual:   $actual" >&2
    exit 1
  fi
done

echo "obs-deps pins ok: prebuilt=$expected_prebuilt qt6=$expected_qt6 cef=$expected_cef"
