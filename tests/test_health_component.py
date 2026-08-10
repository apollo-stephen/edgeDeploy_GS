import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class HealthComponentBehaviorTest(unittest.TestCase):
    def test_lifecycle_transitions_and_resource_snapshot(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            executable = Path(temporary_directory) / "health_component_test"
            compile_result = subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-DESP_LOG_CAPTURE",
                    "-I",
                    str(ROOT / "tests/host/include"),
                    "-I",
                    str(ROOT / "components/INFERENCE/include"),
                    "-I",
                    str(ROOT / "components/HEALTH/include"),
                    "-I",
                    str(ROOT / "components/HEALTH"),
                    str(ROOT / "tests/host/health_component_test.c"),
                    str(ROOT / "components/HEALTH/health.c"),
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
                "lifecycle": "health lifecycle behavior passed",
                "transitions": "health transition behavior passed",
                "resources": "health resource snapshot behavior passed",
                "stats-unavailable": (
                    "health unavailable statistics behavior passed"
                ),
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
