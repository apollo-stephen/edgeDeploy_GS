import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class HttpCaptureComponentBehaviorTest(unittest.TestCase):
    def test_timer_dependency_is_explicit(self):
        cmake = (
            ROOT / "components/HTTP_CAPTURE/CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertIn("esp_timer", cmake)

    def test_routes_capture_ownership_and_preview_controls(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            executable = Path(temporary_directory) / "http_capture_component_test"
            compile_result = subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-D_POSIX_C_SOURCE=200809L",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "tests/host/include"),
                    "-I",
                    str(ROOT / "components/CAMERA/include"),
                    "-I",
                    str(ROOT / "components/WIFIAP/include"),
                    "-I",
                    str(ROOT / "components/HTTP_CAPTURE/include"),
                    str(ROOT / "tests/host/http_capture_component_test.c"),
                    str(ROOT / "components/HTTP_CAPTURE/http_capture.c"),
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
            self.assertIn(
                "http capture component behavior passed",
                run_result.stdout,
            )


if __name__ == "__main__":
    unittest.main()
