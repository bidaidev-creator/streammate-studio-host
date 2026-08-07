#!/usr/bin/env python3
import hashlib
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PACKAGE_SCRIPT = REPO_ROOT / "packaging" / "windows" / "package-dist.sh"


def bash_command() -> str:
    """A REAL bash for running the packaging script.

    On Windows a bare "bash" from a non-Actions-step process resolves through
    the system PATH to the System32 WSL stub (which dies with "no installed
    distributions" when WSL has no distro); Git-Bash's bin directory is not on
    the system PATH. Prefer the Git for Windows bash explicitly.
    """
    if sys.platform != "win32":
        return "bash"
    for env_var in ("ProgramFiles", "ProgramFiles(x86)"):
        base = os.environ.get(env_var)
        if not base:
            continue
        candidate = Path(base) / "Git" / "bin" / "bash.exe"
        if candidate.is_file():
            return str(candidate)
    return "bash"


HOST_SOURCE = REPO_ROOT / "src" / "studio_host.cpp"

REQUIRED_MODULES = ("obs-outputs", "obs-x264", "rtmp-services", "win-capture", "win-wasapi")


class WindowsPackagingTest(unittest.TestCase):
    def write_fixture(self, path: Path, contents: str | None = None) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents or f"fixture for {path.name}\n", encoding="utf-8")
        return path

    def build_input_tree(self, temp_root: Path) -> dict[str, Path]:
        inputs = temp_root / "inputs"
        host = self.write_fixture(inputs / "host" / "studio-host.exe")
        smoke = self.write_fixture(inputs / "host" / "studio-host-smoke.exe")
        obs_dll_dir = inputs / "obs-runtime"
        self.write_fixture(obs_dll_dir / "obs.dll")
        self.write_fixture(obs_dll_dir / "w32-pthreads.dll")
        deps_bin_dir = inputs / "obs-deps" / "bin"
        self.write_fixture(deps_bin_dir / "libcrypto-3-x64.dll")
        self.write_fixture(deps_bin_dir / "not-a-runtime.txt")
        graphics = self.write_fixture(inputs / "graphics" / "libobs-d3d11.dll")
        modules = inputs / "modules"
        for module in REQUIRED_MODULES:
            self.write_fixture(modules / f"{module}.dll")
            self.write_fixture(modules / "data" / module / "locale" / "en-US.ini")
        self.write_fixture(modules / "obs-ffmpeg.dll")
        self.write_fixture(modules / "data" / "obs-ffmpeg" / "locale" / "en-US.ini")
        streammate = self.write_fixture(inputs / "streammate-native-overlay.dll")
        libobs_data = inputs / "libobs-data"
        self.write_fixture(libobs_data / "default.effect")
        self.write_fixture(libobs_data / "format_conversion.effect")
        browser_plugin = self.write_fixture(inputs / "browser" / "obs-browser.dll")
        browser_page = self.write_fixture(inputs / "browser" / "obs-browser-page.exe")
        frontend_api = self.write_fixture(inputs / "frontend" / "obs-frontend-api.dll")
        cef_release = inputs / "cef" / "Release"
        for name in (
            "libcef.dll", "chrome_elf.dll", "libEGL.dll", "libGLESv2.dll",
            "v8_context_snapshot.bin",
        ):
            self.write_fixture(cef_release / name)
        cef_resources = inputs / "cef" / "Resources"
        for name in (
            "chrome_100_percent.pak", "chrome_200_percent.pak", "icudtl.dat",
            "resources.pak",
        ):
            self.write_fixture(cef_resources / name)
        self.write_fixture(cef_resources / "locales" / "en-US.pak")
        self.write_fixture(cef_resources / "not-staged.txt")
        browser_data = inputs / "browser-data"
        self.write_fixture(browser_data / "locale" / "en-US.ini")
        qt_bin = inputs / "qt" / "bin"
        for name in ("Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll"):
            self.write_fixture(qt_bin / name)
        self.write_fixture(qt_bin / "Qt6Network.dll")
        return {
            "host": host,
            "smoke": smoke,
            "obs_dll_dir": obs_dll_dir,
            "deps_bin_dir": deps_bin_dir,
            "graphics": graphics,
            "modules": modules,
            "streammate": streammate,
            "libobs_data": libobs_data,
            "browser_plugin": browser_plugin,
            "browser_page": browser_page,
            "frontend_api": frontend_api,
            "cef_release": cef_release,
            "cef_resources": cef_resources,
            "browser_data": browser_data,
            "qt_bin": qt_bin,
        }

    def package(self, inputs: dict[str, Path], output: Path,
                extra: list[str] | None = None, include_cef: bool = True) -> subprocess.CompletedProcess[str]:
        args = [
            bash_command(), PACKAGE_SCRIPT.as_posix(),
            "--host-bin", inputs["host"].as_posix(),
            "--smoke-bin", inputs["smoke"].as_posix(),
            "--obs-dll-dir", inputs["obs_dll_dir"].as_posix(),
            "--deps-bin-dir", inputs["deps_bin_dir"].as_posix(),
            "--graphics-module", inputs["graphics"].as_posix(),
            "--obs-modules-dir", inputs["modules"].as_posix(),
            "--streammate-plugin", inputs["streammate"].as_posix(),
            "--libobs-data-dir", inputs["libobs_data"].as_posix(),
        ]
        if include_cef:
            args.extend([
                "--obs-browser-plugin", inputs["browser_plugin"].as_posix(),
                "--obs-browser-page", inputs["browser_page"].as_posix(),
                "--cef-release-dir", inputs["cef_release"].as_posix(),
                "--cef-resources-dir", inputs["cef_resources"].as_posix(),
                "--obs-frontend-api", inputs["frontend_api"].as_posix(),
                "--obs-browser-data-dir", inputs["browser_data"].as_posix(),
                "--qt-bin-dir", inputs["qt_bin"].as_posix(),
            ])
        args.extend(["--output-dir", output.as_posix()])
        if extra:
            args.extend(extra)
        return subprocess.run(
            args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False
        )

    def test_bare_root_layout_contains_complete_runtime_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            inputs = self.build_input_tree(root)
            output = root / "dist"
            result = self.package(inputs, output)
            self.assertEqual(result.returncode, 0, result.stdout)

            stage = output / "StreamMateStudioHost"
            for name in (
                "studio-host.exe", "studio-host-smoke.exe", "obs.dll",
                "w32-pthreads.dll", "libcrypto-3-x64.dll", "libobs-d3d11.dll",
                "obs-frontend-api.dll",
            ):
                self.assertTrue((stage / name).is_file(), name)
            self.assertFalse((stage / "not-a-runtime.txt").exists())

            plugins = stage / "obs-plugins" / "64bit"
            for module in (*REQUIRED_MODULES, "obs-ffmpeg", "streammate-native-overlay"):
                self.assertTrue((plugins / f"{module}.dll").is_file(), module)
            for name in (
                "obs-browser.dll", "obs-browser-page.exe", "libcef.dll",
                "chrome_elf.dll", "libEGL.dll", "libGLESv2.dll",
                "v8_context_snapshot.bin", "chrome_100_percent.pak",
                "chrome_200_percent.pak", "icudtl.dat", "resources.pak",
                "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll",
            ):
                self.assertTrue((plugins / name).is_file(), name)
            self.assertTrue((plugins / "locales" / "en-US.pak").is_file())
            self.assertFalse((plugins / "not-staged.txt").exists())
            # Only the load-bearing Qt runtime travels, not the whole Qt bin dir.
            self.assertFalse((plugins / "Qt6Network.dll").exists())
            for module in (*REQUIRED_MODULES, "obs-ffmpeg"):
                self.assertTrue(
                    (stage / "data" / "obs-plugins" / module / "locale" / "en-US.ini").is_file(),
                    module,
                )
            self.assertTrue((stage / "data" / "libobs" / "default.effect").is_file())
            self.assertTrue(
                (stage / "data" / "obs-plugins" / "obs-browser" / "locale" / "en-US.ini").is_file()
            )

    def test_manifest_is_sorted_sha256sum_compatible_and_relative_to_archive_root(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            inputs = self.build_input_tree(root)
            output = root / "dist"
            result = self.package(inputs, output)
            self.assertEqual(result.returncode, 0, result.stdout)

            manifest = output / "sha256-manifest.txt"
            lines = manifest.read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines, sorted(lines, key=lambda line: line.split("  ", 1)[1]))
            self.assertTrue(lines)
            pattern = re.compile(r"^[0-9a-f]{64}  \./[^\r\n]+$")
            for line in lines:
                self.assertRegex(line, pattern)
                digest, relative = line.split("  ", 1)
                payload = output / "StreamMateStudioHost" / relative.removeprefix("./")
                self.assertEqual(hashlib.sha256(payload.read_bytes()).hexdigest(), digest)
                for form in {str(root), str(root).replace(chr(92), '/')}:
                    self.assertNotIn(form, line)
                self.assertNotIn("StreamMateStudioHost/", relative)

    def test_tarball_has_bare_files_at_archive_root_and_is_repack_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            inputs = self.build_input_tree(root)
            first = root / "dist-a"
            second = root / "dist-b"
            result_a = self.package(inputs, first)
            result_b = self.package(inputs, second)
            self.assertEqual(result_a.returncode, 0, result_a.stdout)
            self.assertEqual(result_b.returncode, 0, result_b.stdout)

            archive_name = "StreamMateStudioHost-windows-x64.tar.gz"
            archive_a = first / archive_name
            archive_b = second / archive_name
            with tarfile.open(archive_a, "r:gz") as archive:
                names = {name.removeprefix("./") for name in archive.getnames()}
            self.assertIn("studio-host.exe", names)
            self.assertIn("obs.dll", names)
            self.assertIn("obs-plugins/64bit/streammate-native-overlay.dll", names)
            self.assertIn("obs-plugins/64bit/obs-browser.dll", names)
            self.assertIn("obs-plugins/64bit/libcef.dll", names)
            self.assertIn("obs-plugins/64bit/locales/en-US.pak", names)
            self.assertIn("obs-plugins/64bit/Qt6Widgets.dll", names)
            self.assertIn("data/obs-plugins/obs-browser/locale/en-US.ini", names)
            self.assertIn("obs-frontend-api.dll", names)
            self.assertFalse(any(name.startswith("StreamMateStudioHost/") for name in names))

            self.assertEqual(
                (first / "sha256-manifest.txt").read_bytes(),
                (second / "sha256-manifest.txt").read_bytes(),
            )
            self.assertEqual(archive_a.read_bytes(), archive_b.read_bytes())

    def test_required_inputs_and_module_data_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            inputs = self.build_input_tree(root)
            inputs["libobs_data"].joinpath("default.effect").unlink()
            result = self.package(inputs, root / "missing-effect")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("default effect", result.stdout.lower())

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            inputs = self.build_input_tree(root)
            data_file = inputs["modules"] / "data" / "win-wasapi" / "locale" / "en-US.ini"
            data_file.unlink()
            data_file.parent.rmdir()
            (inputs["modules"] / "data" / "win-wasapi").rmdir()
            result = self.package(inputs, root / "missing-module-data")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("required obs module data win-wasapi", result.stdout.lower())

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            inputs = self.build_input_tree(root)
            inputs["cef_resources"].joinpath("icudtl.dat").unlink()
            result = self.package(inputs, root / "missing-cef-resource")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cef resources payload icudtl.dat", result.stdout.lower())

    def test_partial_cef_argument_groups_fail_closed(self) -> None:
        cef_arguments = {
            "--obs-browser-plugin": "browser_plugin",
            "--obs-browser-page": "browser_page",
            "--cef-release-dir": "cef_release",
            "--cef-resources-dir": "cef_resources",
            "--obs-frontend-api": "frontend_api",
            "--obs-browser-data-dir": "browser_data",
            "--qt-bin-dir": "qt_bin",
        }
        for argument, input_key in cef_arguments.items():
            with self.subTest(argument=argument), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                inputs = self.build_input_tree(root)
                result = self.package(
                    inputs,
                    root / "partial-cef",
                    extra=[argument, inputs[input_key].as_posix()],
                    include_cef=False,
                )
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("is required", result.stdout.lower())

    def test_cef_argument_group_is_optional_as_a_whole(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            inputs = self.build_input_tree(root)
            output = root / "without-cef"
            result = self.package(inputs, output, include_cef=False)
            self.assertEqual(result.returncode, 0, result.stdout)
            stage = output / "StreamMateStudioHost"
            self.assertFalse((stage / "obs-frontend-api.dll").exists())
            self.assertFalse((stage / "obs-plugins" / "64bit" / "obs-browser.dll").exists())

    def test_debug_crt_binaries_are_refused(self) -> None:
        # A debug-CRT PE launches on dev/CI machines (Visual Studio ships the
        # debug runtime) but dies with STATUS_DLL_NOT_FOUND on user hardware;
        # the first owner-hardware studio-host.exe launch caught exactly this.
        for input_key, marker in (
            ("host", "ucrtbased.dll"),
            ("smoke", "VCRUNTIME140D.dll"),
            ("streammate", "MSVCP140D.dll"),
        ):
            with self.subTest(binary=input_key), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                inputs = self.build_input_tree(root)
                self.write_fixture(
                    inputs[input_key],
                    f"pe fixture importing {marker} for {inputs[input_key].name}\n",
                )
                result = self.package(inputs, root / "debug-crt")
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("debug crt", result.stdout.lower())

    def test_script_pins_all_arguments_and_deterministic_tools(self) -> None:
        script = PACKAGE_SCRIPT.read_text(encoding="utf-8")
        for argument in (
            "--host-bin", "--smoke-bin", "--obs-dll-dir", "--deps-bin-dir",
            "--graphics-module", "--obs-modules-dir", "--streammate-plugin",
            "--libobs-data-dir", "--obs-browser-plugin", "--obs-browser-page",
            "--cef-release-dir", "--cef-resources-dir", "--obs-frontend-api",
            "--obs-browser-data-dir", "--qt-bin-dir", "--output-dir",
        ):
            self.assertIn(argument, script)
        self.assertIn("sha256sum", script)
        self.assertIn("sort -z", script)
        self.assertIn("tar --sort=name", script)
        self.assertIn("--uid 0 --gid 0", script)
        self.assertIn("touch -t 198001010000.00", script)
        # Explicit gzip -n pipe: tar -z lets the compressor stamp the gzip
        # header MTIME (BSD tar uses the current second), breaking byte
        # determinism across seconds.
        self.assertIn("| gzip -n", script)
        self.assertNotIn("-czf", script)

    def test_host_resolves_windows_modules_and_core_data_from_executable_root(self) -> None:
        source = HOST_SOURCE.read_text(encoding="utf-8")
        self.assertIn(
            'executable->parent_path() / "obs-plugins" / "64bit"', source
        )
        self.assertIn(
            'executable->parent_path() / "data" / "libobs"', source
        )
        self.assertIn("obs_add_data_path(data_path.c_str())", source)
        self.assertNotIn(
            "executable->parent_path().parent_path().parent_path()", source
        )


if __name__ == "__main__":
    unittest.main()
