import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class HttpImageTransferStartupTest(unittest.TestCase):
    def test_dependencies_start_in_order_and_fail_fast(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            executable = Path(temporary_directory) / "startup_test"
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
                    "-I",
                    str(ROOT / "components/WIFIAP/include"),
                    "-I",
                    str(ROOT / "components/HTTP_CAPTURE/include"),
                    "-I",
                    str(ROOT / "components/INFERENCE/include"),
                    str(ROOT / "tests/host/http_image_transfer_integration_test.c"),
                    str(ROOT / "main/main.c"),
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

            for scenario in (
                "success",
                "nvs-recovery",
                "camera-failure",
                "wifi-failure",
                    "http-failure",
                    "inference-failure",
                ):
                with self.subTest(scenario=scenario):
                    run_result = subprocess.run(
                        [str(executable), scenario],
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
                        "http image transfer startup behavior passed",
                        run_result.stdout,
                    )


if __name__ == "__main__":
    unittest.main()
