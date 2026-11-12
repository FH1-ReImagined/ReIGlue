"""Exercise the real marker generator with synthetic release-plugin bytes."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "cmake/Fh1NativeSceneCapabilities.cmake"
CMAKE = os.environ.get("CMAKE", "cmake")


class CapabilityTests(unittest.TestCase):
    def test_enabled_disabled_and_missing_plugin(self):
        with tempfile.TemporaryDirectory() as d:
            folder = Path(d)
            plugin = folder / "librexgpu-xenos.so"
            marker = folder / "fh1-native-scene.json"
            plugin.write_bytes(b"linked and installed plugin bytes")
            def generate(enabled):
                return subprocess.run([CMAKE, f"-DFH1_GPU_PLUGIN={plugin}",
                                       f"-DFH1_CAPABILITIES={marker}", f"-DFH1_ENABLED={enabled}",
                                       "-P", str(SCRIPT)], capture_output=True)
            self.assertEqual(generate("ON").returncode, 0)
            self.assertEqual(json.loads(marker.read_text()), {"api": 1,
                "renderer_sha256": hashlib.sha256(plugin.read_bytes()).hexdigest()})
            plugin.write_bytes(b"rpath adjusted installed bytes")
            self.assertEqual(generate("ON").returncode, 0)
            self.assertEqual(json.loads(marker.read_text())["renderer_sha256"], hashlib.sha256(plugin.read_bytes()).hexdigest())
            self.assertEqual(generate("OFF").returncode, 0)
            self.assertFalse(marker.exists())
            marker.write_text("stale")
            plugin.unlink()
            self.assertNotEqual(generate("ON").returncode, 0)
            self.assertFalse(marker.exists())

    def test_install_hook_runs_after_plugin_install_and_removes_stale(self):
        root = SCRIPT.parents[1]
        install_rules = (root / "cmake/rexglue_install.cmake").read_text()
        begin = install_rules.index("# This must follow the target install:")
        end = install_rules.index("# A Debug codegen tool", begin)
        hook = install_rules[begin:end]
        with tempfile.TemporaryDirectory() as d:
            folder = Path(d)
            project, build, prefix = folder / "project", folder / "build", folder / "prefix"
            project.mkdir()
            windows = os.name == "nt"
            plugin = folder / ("rexgpu-xenos.dll" if windows else "librexgpu-xenos.so")
            target_dir = "bin" if windows else "lib"
            plugin.write_bytes(b"installed renderer")
            (project / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\nproject(marker NONE)\n"
                "set(CMAKE_INSTALL_BINDIR bin)\nset(CMAKE_INSTALL_LIBDIR lib)\n"
                f'set(REXGLUE_ROOT "{root.as_posix()}")\n'
                "add_library(rexgpu-xenos SHARED IMPORTED GLOBAL)\n"
                f'set_target_properties(rexgpu-xenos PROPERTIES IMPORTED_LOCATION "{plugin.as_posix()}")\n'
                f'install(FILES "{plugin.as_posix()}" DESTINATION {target_dir})\n' + hook)
            def configure(enabled):
                return subprocess.run([CMAKE, "-S", str(project), "-B", str(build),
                    f"-DCMAKE_INSTALL_PREFIX={prefix}", f"-DREXGLUE_USE_VULKAN={enabled}"],
                    capture_output=True)
            self.assertEqual(configure("ON").returncode, 0)
            self.assertEqual(subprocess.run([CMAKE, "--install", str(build), "--config", "Release"],
                                            capture_output=True).returncode, 0)
            installed, marker = prefix / target_dir / plugin.name, prefix / target_dir / "fh1-native-scene.json"
            self.assertEqual(json.loads(marker.read_text())["renderer_sha256"], hashlib.sha256(installed.read_bytes()).hexdigest())
            self.assertEqual(configure("OFF").returncode, 0)
            self.assertEqual(subprocess.run([CMAKE, "--install", str(build), "--config", "Release"],
                                            capture_output=True).returncode, 0)
            self.assertFalse(marker.exists())


if __name__ == "__main__":
    unittest.main()
