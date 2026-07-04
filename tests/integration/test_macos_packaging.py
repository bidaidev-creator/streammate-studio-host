#!/usr/bin/env python3
import os
import plistlib
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parents[2]
PACKAGE_SCRIPT = REPO_ROOT / "packaging" / "macos" / "package-app.sh"
INFO_PLIST = REPO_ROOT / "packaging" / "macos" / "Info.plist"


class MacosPackagingTest(unittest.TestCase):
    def make_executable(self, path: Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        path.chmod(0o755)

    def make_framework(self, root: Path) -> Path:
        framework = root / "libobs.framework"
        binary = framework / "Versions" / "A" / "libobs"
        binary.parent.mkdir(parents=True)
        binary.write_text("fake libobs binary\n", encoding="utf-8")
        (framework / "libobs").symlink_to("Versions/A/libobs")
        return framework

    def make_plugin_bundle(self, modules_dir: Path, name: str) -> None:
        binary = modules_dir / f"{name}.plugin" / "Contents" / "MacOS" / name
        binary.parent.mkdir(parents=True)
        binary.write_text(f"fake {name} plugin\n", encoding="utf-8")

    def run_package(self, temp_root: Path, extra: Optional[list[str]] = None) -> subprocess.CompletedProcess:
        build_root = temp_root / "build"
        host_bin = build_root / "studio-host"
        smoke_bin = build_root / "studio-host-smoke"
        self.make_executable(host_bin, "#!/usr/bin/env bash\nif [[ ${1:-} == --version ]]; then echo streammate-studio-host 0.0.0; fi\n")
        self.make_executable(smoke_bin, "#!/usr/bin/env bash\nif [[ ${1:-} == --version ]]; then echo streammate-studio-host-smoke 0.0.0; fi\n")
        framework = self.make_framework(temp_root / "obs-frameworks")
        modules_dir = temp_root / "obs-modules"
        for name in ["mac-avcapture", "mac-capture", "obs-outputs", "obs-x264"]:
            self.make_plugin_bundle(modules_dir, name)
        deps_dir = temp_root / "obs-deps"
        deps_dir.mkdir()
        (deps_dir / "libdependency.dylib").write_text("fake dependency\n", encoding="utf-8")
        graphics_module = temp_root / "graphics" / "libobs-opengl.dylib"
        graphics_module.parent.mkdir()
        graphics_module.write_text("fake graphics module\n", encoding="utf-8")
        output_dir = temp_root / "dist"
        args = [
            str(PACKAGE_SCRIPT),
            "--host-bin", str(host_bin),
            "--smoke-bin", str(smoke_bin),
            "--info-plist", str(INFO_PLIST),
            "--libobs-framework", str(framework),
            "--obs-modules-dir", str(modules_dir),
            "--obs-deps-lib-dir", str(deps_dir),
            "--obs-graphics-module", str(graphics_module),
            "--output-dir", str(output_dir),
            "--skip-codesign",
            "--skip-install-name-tool",
        ]
        if extra:
            args.extend(extra)
        return subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)

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

    def test_libobs_framework_is_required_for_l4_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            result = self.run_package(temp_root, ["--libobs-framework", str(temp_root / "missing.framework")])
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("libobs framework", result.stdout.lower())


if __name__ == "__main__":
    unittest.main()
