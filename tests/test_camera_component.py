import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class CameraComponentBehaviorTest(unittest.TestCase):
    def test_jpeg_buffer_has_explicit_low_resolution_headroom(self):
        defaults_path = ROOT / "sdkconfig.defaults"
        self.assertTrue(
            defaults_path.is_file(),
            msg="sdkconfig.defaults must preserve the JPEG buffer configuration",
        )
        values = {}
        for line in defaults_path.read_text(encoding="utf-8").splitlines():
            if line.startswith("CONFIG_") and "=" in line:
                key, value = line.split("=", 1)
                values[key] = value

        self.assertEqual(
            "y",
            values.get("CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE_CUSTOM"),
        )
        self.assertEqual(
            "8192",
            values.get("CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE"),
        )

    def test_camera_lifecycle_and_capture_ownership(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            executable = Path(temporary_directory) / "camera_component_test"
            compile_result = subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "tests/host/include"),
                    "-I",
                    str(ROOT / "components/CAMERA/include"),
                    str(ROOT / "tests/host/camera_component_test.c"),
                    str(ROOT / "components/CAMERA/CAMERA.c"),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                0,
                compile_result.returncode,
                msg=compile_result.stdout + compile_result.stderr,
            )

            run_result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                0,
                run_result.returncode,
                msg=run_result.stdout + run_result.stderr,
            )
            self.assertIn("camera component behavior passed", run_result.stdout)

            failure_result = subprocess.run(
                [str(executable), "init-failure"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                0,
                failure_result.returncode,
                msg=failure_result.stdout + failure_result.stderr,
            )
            self.assertIn(
                "camera initialization failure cleanup passed",
                failure_result.stdout,
            )


if __name__ == "__main__":
    unittest.main()
