"""NIF-CM: derive-from-pin guard for the hand-pinned upstream id sets.

M3 (codex F1 fix direction) left the upstream filter/transition/source id sets
hand-pinned in ``src/studio_host.cpp``. This suite mechanically re-derives the
ground truth from the *pinned* obs-studio tree (``external/obs-studio``, held
at the OBS_PIN commit by fork-guard) and fails when:

  * a pinned id does not exist as a registered ``.id`` (or id string) in the
    pinned tree (the opus-N1 "spurious pin" class), or
  * a recorded cross-platform/device id at the pin is missing from the sets
    (the M3-handoff omissions: aja_source, pipewire-*, xshm_input_v2).

A pin bump changes the submodule tree, so this scan re-runs against the new
pin and catches silent drift — the host-side half of the NIF-CM pin-bump gate.

The scan needs the submodule tree. Locally it may be absent (worktrees do not
auto-populate submodules); the suite then SKIPS loudly. CI must run it with
``STREAMMATE_REQUIRE_OBS_TREE=1`` (after fork-guard inits the submodule), which
turns an absent tree into a hard failure so the guard cannot silently skip in
the enforcement lane.
"""

from __future__ import annotations

import os
import re
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HOST_CPP = REPO / "src" / "studio_host.cpp"
OBS_TREE = REPO / "external" / "obs-studio"
OBS_PIN = REPO / "OBS_PIN"

# Set-accessor functions in studio_host.cpp whose ids must all exist in the
# pinned obs-studio tree.
PINNED_SET_FUNCTIONS = [
    "upstream_source_ids",
    "upstream_filter_ids",
    "upstream_transition_ids",
    "mapped_native_source_ids",
    "screen_permission_source_ids",
    "camera_permission_source_ids",
    "device_backed_source_ids",
    "other_platform_source_ids",
]

# Ids that are id-string aliases or grouping constructs the tree spells
# differently from a literal .id initializer; each carries its ground-truth
# evidence location so the exemption stays reviewable.
EXEMPT_IDS = {
    # "group" is a scene-collection construct (obs_group_from_source), not a
    # registered source info id.
    "group",
    # Registered by the obs-browser plugin (plugins/obs-browser is a NESTED
    # submodule not populated by fork-guard; ground truth: obs-browser
    # browser-source.cpp .id = "browser_source" at the pin's obs-browser rev).
    "browser_source",
}

VERSIONED_ID = re.compile(r"^(?P<base>.+)_v(?P<version>[0-9]+)$")


def id_exists_in_tree(pinned_id: str, haystack: str) -> bool:
    """True when the id is registered in the pinned tree.

    Encodes the obs versioned-id convention: a struct with .id = "<base>" and
    .version = N registers the effective lookup id "<base>_vN", which never
    appears as a string literal. For such ids the BASE id must exist as a
    literal (and the _v suffix is accepted as the upstream convention).
    """
    if f'"{pinned_id}"' in haystack:
        return True
    versioned = VERSIONED_ID.match(pinned_id)
    if versioned is not None:
        return f'"{versioned.group("base")}"' in haystack
    return False


def extract_set(function_name: str) -> list[str]:
    text = HOST_CPP.read_text(encoding="utf-8")
    pattern = re.compile(
        r"&" + re.escape(function_name) + r"\(\)\s*\{.*?ids\s*=\s*\{(.*?)\};",
        re.S,
    )
    match = pattern.search(text)
    if match is None:
        raise AssertionError(f"could not locate id set {function_name} in studio_host.cpp")
    return re.findall(r'"([^"]+)"', match.group(1))


def obs_tree_present() -> bool:
    return (OBS_TREE / "libobs").is_dir() and (OBS_TREE / "plugins").is_dir()


def obs_tree_id_index() -> str:
    """One concatenated haystack of every quoted string in plausible id sites."""
    chunks: list[str] = []
    for sub in ("plugins", "libobs", "UI"):
        root = OBS_TREE / sub
        if not root.is_dir():
            continue
        proc = subprocess.run(
            ["grep", "-r", "--include=*.c", "--include=*.cpp", "--include=*.h",
             "--include=*.hpp", "--include=*.m", "--include=*.mm", "-h", '"', str(root)],
            capture_output=True,
            text=True,
            check=False,
        )
        chunks.append(proc.stdout)
    return "\n".join(chunks)


class UpstreamPinSetDerivationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not obs_tree_present():
            if os.environ.get("STREAMMATE_REQUIRE_OBS_TREE") == "1":
                raise AssertionError(
                    "STREAMMATE_REQUIRE_OBS_TREE=1 but external/obs-studio is not populated; "
                    "run scripts/fork-guard.sh (or git submodule update --init) first"
                )
            raise unittest.SkipTest(
                "external/obs-studio not populated locally; CI enforces this suite with "
                "STREAMMATE_REQUIRE_OBS_TREE=1"
            )
        cls.haystack = obs_tree_id_index()

    def test_pin_matches_submodule(self) -> None:
        pin = dict(
            line.split("=", 1)
            for line in OBS_PIN.read_text(encoding="utf-8").splitlines()
            if "=" in line
        )
        head = subprocess.run(
            ["git", "-C", str(OBS_TREE), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
        self.assertEqual(head, pin["commit"], "submodule is not at OBS_PIN; the scan would derive from the wrong tree")

    def test_every_pinned_id_exists_in_the_pinned_tree(self) -> None:
        missing: list[str] = []
        for function_name in PINNED_SET_FUNCTIONS:
            for pinned_id in extract_set(function_name):
                if pinned_id in EXEMPT_IDS:
                    continue
                if not id_exists_in_tree(pinned_id, self.haystack):
                    missing.append(f"{function_name}: {pinned_id}")
        self.assertEqual(
            missing,
            [],
            "pinned ids not found anywhere in the pinned obs-studio tree (spurious pins): "
            + ", ".join(missing),
        )

    def test_exempt_ids_stay_minimal_and_reviewed(self) -> None:
        self.assertEqual(EXEMPT_IDS, {"group", "browser_source"})

    def test_m3_handoff_omissions_are_pinned(self) -> None:
        device_backed = set(extract_set("device_backed_source_ids"))
        other_platform = set(extract_set("other_platform_source_ids"))
        self.assertIn("aja_source", device_backed, "aja_source (macOS device plugin at the pin) missing")
        for linux_id in (
            "xshm_input_v2",
            "pipewire-camera-source",
            "pipewire-desktop-capture-source",
            "pipewire-window-capture-source",
            "pipewire-screen-capture-source",
        ):
            self.assertIn(linux_id, other_platform, f"{linux_id} missing from other_platform_source_ids")


if __name__ == "__main__":
    unittest.main()
