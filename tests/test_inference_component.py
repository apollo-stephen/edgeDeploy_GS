import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class InferenceComponentBehaviorTest(unittest.TestCase):
    def test_bundled_esp_nn_sources_are_discovered(self):
        result = subprocess.run(
            [
                "cmake",
                "-P",
                str(ROOT / "tests/cmake/test_esp_nn_sources.cmake"),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(
            0,
            result.returncode,
            msg=result.stdout + result.stderr,
        )

    def test_build_defaults_enable_external_ram_for_model_arena(self):
        defaults_path = ROOT / "sdkconfig.defaults"
        values = {}
        for line in defaults_path.read_text(encoding="utf-8").splitlines():
            if line.startswith("CONFIG_") and "=" in line:
                key, value = line.split("=", 1)
                values[key] = value

        self.assertEqual('"16MB"', values.get("CONFIG_ESPTOOLPY_FLASHSIZE"))
        self.assertEqual("y", values.get("CONFIG_SPIRAM"))
        self.assertEqual("y", values.get("CONFIG_SPIRAM_MODE_OCT"))
        self.assertEqual("y", values.get("CONFIG_SPIRAM_USE_MALLOC"))
        self.assertEqual(
            "16384",
            values.get("CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL"),
        )

    def test_task_lifecycle_frame_ownership_and_classifier_bridge(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            executable = Path(temporary_directory) / "inference_component_test"
            compile_result = subprocess.run(
                [
                    "c++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-DESP_LOG_CAPTURE",
                    "-I",
                    str(ROOT / "tests/host/include"),
                    "-I",
                    str(ROOT / "components/CAMERA/include"),
                    "-I",
                    str(ROOT / "components/INFERENCE/include"),
                    str(ROOT / "tests/host/inference_component_test.cpp"),
                    str(ROOT / "components/INFERENCE/inference.cpp"),
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

            scenarios = {
                "success": "inference success behavior passed",
                "no-memory": "inference allocation failure passed",
                "task-failure": "inference task failure rollback passed",
                "invalid-frame": "inference invalid frame cleanup passed",
                "decode-failure": "inference decode failure cleanup passed",
                "classifier-failure": "inference classifier failure passed",
                "uncertain": "inference uncertainty reporting passed",
            }
            for scenario, expected_output in scenarios.items():
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
                    self.assertIn(expected_output, run_result.stdout)


if __name__ == "__main__":
    unittest.main()
