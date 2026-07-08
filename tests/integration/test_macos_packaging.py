#!/usr/bin/env python3
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parents[2]
PACKAGE_SCRIPT = REPO_ROOT / "packaging" / "macos" / "package-app.sh"
INFO_PLIST = REPO_ROOT / "packaging" / "macos" / "Info.plist"
README = REPO_ROOT / "README.md"
CI_NOTES = REPO_ROOT / "docs" / "ci-notes.md"

OUTER_IDENTIFIER = "com.streammate.studio-host"
VENDORED_PREFIX = "com.streammate.studio-host.vendored."
NESTED_PLUGINS = ["mac-avcapture", "mac-capture", "obs-outputs", "obs-x264"]

CODESIGN_AVAILABLE = sys.platform == "darwin" and shutil.which("codesign") is not None


class MacosPackagingTest(unittest.TestCase):
    def make_executable(self, path: Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        path.chmod(0o755)

    def make_framework(self, root: Path) -> Path:
        # A canonical macOS framework layout (Versions/A + Current symlink +
        # top-level symlinks + a versioned Info.plist) so that `codesign` can
        # sign it as a framework. The real upstream libobs.framework ships this
        # structure; the earlier flat fixture could not be code-signed.
        framework = root / "libobs.framework"
        versions_a = framework / "Versions" / "A"
        (versions_a / "Resources").mkdir(parents=True)
        (versions_a / "libobs").write_text("fake libobs binary\n", encoding="utf-8")
        info = {
            "CFBundleIdentifier": "com.streammate.vendored.libobs",
            "CFBundleExecutable": "libobs",
            "CFBundlePackageType": "FMWK",
            "CFBundleName": "libobs",
        }
        with (versions_a / "Resources" / "Info.plist").open("wb") as handle:
            plistlib.dump(info, handle)
        (framework / "Versions" / "Current").symlink_to("A")
        (framework / "libobs").symlink_to("Versions/Current/libobs")
        (framework / "Resources").symlink_to("Versions/Current/Resources")
        return framework

    def make_plugin_bundle(self, modules_dir: Path, name: str) -> None:
        binary = modules_dir / f"{name}.plugin" / "Contents" / "MacOS" / name
        binary.parent.mkdir(parents=True)
        binary.write_text(f"fake {name} plugin\n", encoding="utf-8")

    def build_input_tree(self, temp_root: Path, name: str = "inputs") -> dict:
        # Build one on-disk input (build) tree of fixtures. Returned separately
        # from packaging so a single test can package the *same* tree twice to
        # exercise repack determinism (rather than two look-alike trees).
        inputs = temp_root / name
        build_root = inputs / "build"
        host_bin = build_root / "studio-host"
        smoke_bin = build_root / "studio-host-smoke"
        self.make_executable(host_bin, "#!/usr/bin/env bash\nif [[ ${1:-} == --version ]]; then echo streammate-studio-host 0.0.0; fi\n")
        self.make_executable(smoke_bin, "#!/usr/bin/env bash\nif [[ ${1:-} == --version ]]; then echo streammate-studio-host-smoke 0.0.0; fi\n")
        framework = self.make_framework(inputs / "obs-frameworks")
        modules_dir = inputs / "obs-modules"
        for name in NESTED_PLUGINS:
            self.make_plugin_bundle(modules_dir, name)
        deps_dir = inputs / "obs-deps"
        deps_dir.mkdir(parents=True)
        (deps_dir / "libdependency.dylib").write_text("fake dependency\n", encoding="utf-8")
        graphics_module = inputs / "graphics" / "libobs-opengl.dylib"
        graphics_module.parent.mkdir(parents=True)
        graphics_module.write_text("fake graphics module\n", encoding="utf-8")
        return {
            "host_bin": host_bin,
            "smoke_bin": smoke_bin,
            "framework": framework,
            "modules_dir": modules_dir,
            "deps_dir": deps_dir,
            "graphics_module": graphics_module,
        }

    def package_from_inputs(
        self,
        temp_root: Path,
        inputs: dict,
        extra: Optional[list[str]] = None,
        codesign: bool = False,
        output_name: str = "dist",
    ) -> subprocess.CompletedProcess:
        output_dir = temp_root / output_name
        args = [
            str(PACKAGE_SCRIPT),
            "--host-bin", str(inputs["host_bin"]),
            "--smoke-bin", str(inputs["smoke_bin"]),
            "--info-plist", str(INFO_PLIST),
            "--libobs-framework", str(inputs["framework"]),
            "--obs-modules-dir", str(inputs["modules_dir"]),
            "--obs-deps-lib-dir", str(inputs["deps_dir"]),
            "--obs-graphics-module", str(inputs["graphics_module"]),
            "--output-dir", str(output_dir),
            "--skip-install-name-tool",
        ]
        if not codesign:
            args.append("--skip-codesign")
        if extra:
            args.extend(extra)
        return subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)

    def run_package(
        self,
        temp_root: Path,
        extra: Optional[list[str]] = None,
        codesign: bool = False,
        output_name: str = "dist",
    ) -> subprocess.CompletedProcess:
        inputs = self.build_input_tree(temp_root, name=f"inputs-{output_name}")
        return self.package_from_inputs(
            temp_root, inputs, extra=extra, codesign=codesign, output_name=output_name
        )

    def codesign_identifier(self, target: Path) -> Optional[str]:
        result = subprocess.run(
            ["codesign", "-dvvv", str(target)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        for line in result.stdout.splitlines():
            if line.startswith("Identifier="):
                return line.split("=", 1)[1].strip()
        return None

    def package_signed_app(self, temp_root: Path, output_name: str = "dist") -> Path:
        result = self.run_package(temp_root, codesign=True, output_name=output_name)
        self.assertEqual(result.returncode, 0, result.stdout)
        return temp_root / output_name / "StreamMateStudioHost.app"

    def test_app_bundle_contains_relocatable_libobs_payload_and_stable_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            result = self.run_package(temp_root)
            self.assertEqual(result.returncode, 0, result.stdout)

            app = temp_root / "dist" / "StreamMateStudioHost.app"
            self.assertTrue((app / "Contents" / "MacOS" / "studio-host").exists())
            self.assertTrue((app / "Contents" / "MacOS" / "studio-host-smoke").exists())
            self.assertTrue((app / "Contents" / "Frameworks" / "libobs.framework" / "Versions" / "A" / "libobs").exists())
            self.assertTrue((app / "Contents" / "Frameworks" / "libdependency.dylib").exists())
            self.assertTrue((app / "Contents" / "Frameworks" / "libobs-opengl.dylib").exists())
            for name in ["mac-avcapture", "mac-capture", "obs-outputs", "obs-x264"]:
                self.assertTrue((app / "Contents" / "PlugIns" / "obs-plugins" / f"{name}.plugin").exists())

            with (app / "Contents" / "Info.plist").open("rb") as handle:
                plist = plistlib.load(handle)
            self.assertEqual(plist["CFBundleIdentifier"], "com.streammate.studio-host")
            self.assertEqual(plist["CFBundleExecutable"], "studio-host")
            self.assertIn("operator enables local capture sources", plist["NSCameraUsageDescription"])
            self.assertIn("operator enables local audio capture sources", plist["NSMicrophoneUsageDescription"])

            scratch = temp_root / "scratch-copy"
            shutil.copytree(app, scratch / app.name, symlinks=True)
            smoke = subprocess.run(
                [str(scratch / app.name / "Contents" / "MacOS" / "studio-host-smoke"), "--version"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(smoke.returncode, 0, smoke.stdout)
            self.assertIn("streammate-studio-host-smoke", smoke.stdout)

            manifest = temp_root / "dist" / "sha256-manifest.txt"
            self.assertTrue(manifest.exists())
            manifest_text = manifest.read_text(encoding="utf-8")
            self.assertIn("./StreamMateStudioHost.app/Contents/Frameworks/libobs.framework/Versions/A/libobs", manifest_text)
            self.assertIn("./StreamMateStudioHost.app/Contents/Frameworks/libobs-opengl.dylib", manifest_text)
            self.assertNotIn(str(temp_root), manifest_text)
            verify = subprocess.run(
                ["shasum", "-a", "256", "-c", "sha256-manifest.txt"],
                cwd=temp_root / "dist",
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(verify.returncode, 0, verify.stdout)

    def make_streammate_plugin(self, root: Path, name: str = "streammate-native-overlay") -> Path:
        # A minimal CFBundle .plugin layout matching what the CMake MODULE
        # target emits and what the host's normal module load path enumerates
        # (Contents/MacOS/<name>).
        bundle = root / f"{name}.plugin"
        binary = bundle / "Contents" / "MacOS" / name
        binary.parent.mkdir(parents=True)
        binary.write_text(f"fake {name} obs module\n", encoding="utf-8")
        return bundle

    def test_streammate_module_staged_into_obs_plugins_alongside_upstream(self) -> None:
        # Spec 34 Capability 3 / chunk 34.H3: the in-tree Stream Mate OBS plugin
        # module is staged under Contents/PlugIns/obs-plugins/ by package-app.sh
        # alongside the upstream modules (loaded by the host's normal module load
        # path). Q-121 provisional: in-host-repo tree, bundle-staged.
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            inputs = self.build_input_tree(temp_root, name="inputs-streammate")
            plugin = self.make_streammate_plugin(temp_root / "streammate-module")
            result = self.package_from_inputs(
                temp_root,
                inputs,
                extra=["--streammate-plugin", str(plugin)],
                output_name="dist",
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            app = temp_root / "dist" / "StreamMateStudioHost.app"
            plugins = app / "Contents" / "PlugIns" / "obs-plugins"
            staged = plugins / "streammate-native-overlay.plugin"
            self.assertTrue(
                (staged / "Contents" / "MacOS" / "streammate-native-overlay").exists(),
                "streammate-native-overlay.plugin missing from staged obs-plugins",
            )
            # Staged alongside (not instead of) the upstream modules.
            for name in NESTED_PLUGINS:
                self.assertTrue((plugins / f"{name}.plugin").exists())

    @unittest.skipUnless(CODESIGN_AVAILABLE, "codesign unavailable (non-macOS)")
    def test_streammate_module_gets_distinct_vendored_identity(self) -> None:
        # The staged module is signed inside-out with its own namespaced
        # identifier (never the outer com.streammate.studio-host), same as the
        # upstream plugins — the ad-hoc, first-party in-tree module is not a
        # third-party binary and does not change the signing approach (Q-123).
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            inputs = self.build_input_tree(temp_root, name="inputs-streammate-sign")
            plugin = self.make_streammate_plugin(temp_root / "streammate-module")
            result = self.package_from_inputs(
                temp_root,
                inputs,
                extra=["--streammate-plugin", str(plugin)],
                codesign=True,
                output_name="dist",
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            app = temp_root / "dist" / "StreamMateStudioHost.app"
            staged = app / "Contents" / "PlugIns" / "obs-plugins" / "streammate-native-overlay.plugin"
            identifier = self.codesign_identifier(staged)
            self.assertEqual(identifier, VENDORED_PREFIX + "streammate-native-overlay")
            self.assertNotEqual(identifier, OUTER_IDENTIFIER)

    def test_libobs_framework_is_required_for_l4_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            result = self.run_package(temp_root, ["--libobs-framework", str(temp_root / "missing.framework")])
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("libobs framework", result.stdout.lower())

    # --- Q-123: inside-out ad-hoc signing (Spec 34 Capability 8) ---

    @unittest.skipUnless(CODESIGN_AVAILABLE, "codesign unavailable (non-macOS)")
    def test_inside_out_signing_passes_strict_deep_verify(self) -> None:
        # Replaces the recorded L4 strictDeepCodesignOk:false ambiguity with a
        # passing strict --deep verification of the packaged bundle, and proves
        # every nested code component verifies individually as well.
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            app = self.package_signed_app(temp_root)

            verify = subprocess.run(
                ["codesign", "--verify", "--strict", "--deep", "--verbose=2", str(app)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            self.assertEqual(verify.returncode, 0, verify.stdout)

            frameworks = app / "Contents" / "Frameworks"
            plugins = app / "Contents" / "PlugIns" / "obs-plugins"
            nested = [
                frameworks / "libobs.framework",
                frameworks / "libdependency.dylib",
                frameworks / "libobs-opengl.dylib",
                app / "Contents" / "MacOS" / "studio-host-smoke",
                *[plugins / f"{name}.plugin" for name in NESTED_PLUGINS],
            ]
            for component in nested:
                with self.subTest(component=component.name):
                    result = subprocess.run(
                        ["codesign", "--verify", "--strict", str(component)],
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        check=False,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout)

    @unittest.skipUnless(CODESIGN_AVAILABLE, "codesign unavailable (non-macOS)")
    def test_nested_components_get_distinct_vendored_identities(self) -> None:
        # The outer bundle is signed with exactly com.streammate.studio-host,
        # while every nested component is re-signed with its own distinct,
        # namespaced com.streammate.studio-host.vendored.<name> identifier (the
        # single `--deep` pass previously clobbered every nested identifier to
        # the outer one). We assert the *exact* per-component identifier rather
        # than a loose suffix/inequality check. (Ad-hoc signatures have a
        # cdhash-based designated requirement, so the signing identifier is the
        # meaningful, deterministic identity to pin here.)
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            app = self.package_signed_app(temp_root)

            self.assertEqual(self.codesign_identifier(app), OUTER_IDENTIFIER)

            frameworks = app / "Contents" / "Frameworks"
            plugins = app / "Contents" / "PlugIns" / "obs-plugins"
            component_expectations = {
                frameworks / "libobs.framework": VENDORED_PREFIX + "libobs",
                frameworks / "libdependency.dylib": VENDORED_PREFIX + "libdependency",
                frameworks / "libobs-opengl.dylib": VENDORED_PREFIX + "libobs-opengl",
                app / "Contents" / "MacOS" / "studio-host-smoke": VENDORED_PREFIX + "smoke",
            }
            for name in NESTED_PLUGINS:
                component_expectations[plugins / f"{name}.plugin"] = VENDORED_PREFIX + name

            for component, expected in component_expectations.items():
                with self.subTest(component=component.name):
                    identifier = self.codesign_identifier(component)
                    self.assertEqual(
                        identifier,
                        expected,
                        f"{component.name} identifier {identifier!r} != expected {expected!r}",
                    )
                    self.assertNotEqual(identifier, OUTER_IDENTIFIER)

    @unittest.skipUnless(CODESIGN_AVAILABLE, "codesign unavailable (non-macOS)")
    def test_repack_of_same_build_tree_is_byte_deterministic(self) -> None:
        # Package the *same* on-disk build tree twice (one shared input tree, not
        # two look-alike trees) and require identical bundle content hashes;
        # ad-hoc signatures are content-derived, so the sha256 manifests (which
        # cover the embedded _CodeSignature data) must match exactly.
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            inputs = self.build_input_tree(temp_root, name="inputs-shared")
            first = self.package_from_inputs(
                temp_root, inputs, codesign=True, output_name="dist-a"
            )
            self.assertEqual(first.returncode, 0, first.stdout)
            second = self.package_from_inputs(
                temp_root, inputs, codesign=True, output_name="dist-b"
            )
            self.assertEqual(second.returncode, 0, second.stdout)

            manifest_a = (temp_root / "dist-a" / "sha256-manifest.txt").read_text(encoding="utf-8")
            manifest_b = (temp_root / "dist-b" / "sha256-manifest.txt").read_text(encoding="utf-8")
            self.assertEqual(manifest_a, manifest_b)
            # The signed bundle actually carries a signature (guards against an
            # accidental unsigned no-op passing the equality check).
            self.assertIn("_CodeSignature", manifest_a)

    def test_packaging_signs_only_with_adhoc_identity_and_no_deep_pass(self) -> None:
        # Grep guard: ad-hoc `--sign -` only, no Developer ID / keychain / real
        # identity, and no lingering `--deep` signing pass anywhere.
        script = PACKAGE_SCRIPT.read_text(encoding="utf-8")
        self.assertNotIn("--deep", script)
        self.assertNotIn("Developer ID", script)
        self.assertNotIn("--keychain", script)
        sign_targets = re.findall(r"--sign\s+(\S+)", script)
        self.assertTrue(sign_targets, "expected at least one codesign --sign invocation")
        for target in sign_targets:
            self.assertEqual(target, "-", f"non-ad-hoc signing identity found: {target!r}")
        # The stable outer identifier is preserved verbatim.
        self.assertIn("--identifier com.streammate.studio-host", script)

    def test_readme_no_longer_lists_implemented_surfaces_out_of_scope(self) -> None:
        readme = README.read_text(encoding="utf-8")
        self.assertNotIn(
            "Scene commands, import, output, live egress, real TCC rehearsal, and product logic remain out of scope",
            readme,
        )
        self.assertNotRegex(
            readme,
            r"[Ss]cene commands,\s*import,\s*(?:and\s*)?output[^\n]*out of scope",
        )
        # Implemented surfaces are now described as present.
        for surface in ["scene", "import", "output"]:
            with self.subTest(surface=surface):
                self.assertRegex(readme, rf"(?i){surface}")
        self.assertRegex(readme, r"(?i)inside-out|per-component")

    def test_ci_notes_document_inside_out_strict_verification(self) -> None:
        notes = CI_NOTES.read_text(encoding="utf-8")
        self.assertRegex(notes, r"(?i)inside-out")
        self.assertIn("--strict", notes)

    def test_docs_do_not_overclaim_nested_identity_preservation(self) -> None:
        # Inside-out signing re-signs nested components with new namespaced
        # vendored.* identifiers; it does NOT preserve their pre-existing
        # identities. Docs must describe the distinct per-component identifiers
        # rather than claim identity is kept "intact" (would be an overclaim).
        for doc in (README, CI_NOTES):
            text = doc.read_text(encoding="utf-8")
            with self.subTest(doc=doc.name):
                self.assertNotIn("identity intact", text)
                self.assertNotIn("identities intact", text)
                self.assertIn(VENDORED_PREFIX.rstrip("."), text)

    def test_readme_frames_live_egress_as_default_off_not_available_opt_in(self) -> None:
        # Guards the live-egress hard-gate posture: the README must not soften
        # real RTMP/SRT egress into a routine "opt-in", and must state it is
        # default-off, fake-ingest-only by default, and approval-gated.
        readme = README.read_text(encoding="utf-8")
        self.assertNotIn("gated behind an explicit per-run opt-in", readme)
        self.assertRegex(readme, r"(?i)default-off")
        self.assertRegex(readme, r"(?i)fake-ingest only")
        self.assertRegex(readme, r"(?i)requires explicit (?:owner )?approval")


if __name__ == "__main__":
    unittest.main()
