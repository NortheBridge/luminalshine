"""Standalone CMake metadata tests; no GPU or running host required.

Run with python tests/integration/test_server_metadata_build.py.
Requires cmake and ninja on PATH.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CMAKE = os.environ.get("CMAKE_COMMAND", "cmake")


class ServerMetadataBuild(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name)
        self.build = self.source / "build"
        self.env = os.environ.copy()
        for name in ("SUNSHINE_SERVER_RELEASE", "SUNSHINE_SERVER_BUILD", "SOURCE_DATE_EPOCH"):
            self.env.pop(name, None)
        (self.source / "input.cpp").write_text("// build input\n")
        (self.source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.25)\nproject(metadata NONE)\n'
            'set(SUNSHINE_TARGET_FILES "${CMAKE_SOURCE_DIR}/input.cpp")\n'
            f'include("{ROOT.as_posix()}/cmake/prep/server_metadata.cmake")\n'
        )

    def configure(self, *options, success=True):
        result = subprocess.run(
            [CMAKE, "-G", "Ninja", "-S", str(self.source), "-B", str(self.build),
             *(["-DCMAKE_MAKE_PROGRAM=" + os.environ["NINJA_COMMAND"]] if "NINJA_COMMAND" in os.environ else []),
             *options],
            env=self.env, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)

    def generate(self):
        subprocess.run([CMAKE, "--build", str(self.build), "--target", "server_metadata"],
                       env=self.env, check=True, capture_output=True)
        return (self.build / "generated/server_metadata.h").read_text()

    def test_local_default_and_reproducible_timestamp(self):
        self.env["SOURCE_DATE_EPOCH"] = "0"
        self.configure()
        header = self.generate()
        self.assertIn("release = 0;", header)
        self.assertIn('"1970-01-01T00:00:00Z"', header)
        self.env["SOURCE_DATE_EPOCH"] = "86400"
        # Even an option-only or dependency-only build must get its own timestamp.
        self.assertIn('"1970-01-02T00:00:00Z"', self.generate())

    def test_official_classifications_and_signing_reconfigure(self):
        for channel in ("1", "2"):
            self.env["SUNSHINE_SERVER_RELEASE"] = channel
            self.env["SUNSHINE_SERVER_BUILD"] = "2026-11-01T21:00:00Z"
            self.configure()
            self.assertIn(f"release = {channel};", self.generate())
            header = self.build / "generated/server_metadata.h"
            before = header.stat().st_mtime_ns
            self.configure()
            self.assertIn('"2026-11-01T21:00:00Z"', self.generate())
            self.assertEqual(before, header.stat().st_mtime_ns)

    def test_invalid_channel_is_rejected(self):
        self.configure("-DSUNSHINE_SERVER_RELEASE=3", success=False)

    def test_invalid_timestamp_is_rejected(self):
        self.configure("-DSUNSHINE_SERVER_BUILD=not-a-timestamp", success=False)


if __name__ == "__main__":
    unittest.main()
