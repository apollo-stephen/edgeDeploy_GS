import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class Modev2ComponentConfigurationTest(unittest.TestCase):
    def test_modev2_is_the_active_model_component(self):
        project_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        inference_cmake = (
            ROOT / "components/INFERENCE/CMakeLists.txt"
        ).read_text(encoding="utf-8")

        self.assertIn(
            'EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/modev2"',
            project_cmake,
        )
        self.assertNotIn(
            'EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/modev1"',
            project_cmake,
        )
        self.assertIn(
            "REQUIRES CAMERA esp32-camera modev2",
            inference_cmake,
        )

    def test_modev2_metadata_matches_deployment(self):
        metadata = (
            ROOT / "modev2/model-parameters/model_metadata.h"
        ).read_text(encoding="utf-8")

        for definition in (
            "#define EI_CLASSIFIER_PROJECT_DEPLOY_VERSION     2",
            "#define EI_CLASSIFIER_INPUT_WIDTH                96",
            "#define EI_CLASSIFIER_INPUT_HEIGHT               96",
            "#define EI_CLASSIFIER_LABEL_COUNT                3",
            "#define EI_CLASSIFIER_TFLITE_INPUT_DATATYPE      EI_CLASSIFIER_DATATYPE_INT8",
            "#define EI_CLASSIFIER_TFLITE_OUTPUT_DATATYPE     EI_CLASSIFIER_DATATYPE_INT8",
            "#define EI_CLASSIFIER_RESIZE_MODE                EI_CLASSIFIER_RESIZE_FIT_SHORTEST",
        ):
            self.assertIn(definition, metadata)

    def test_modev2_idf_component_keeps_esp_nn_configuration(self):
        component = (ROOT / "modev2/CMakeLists.txt").read_text(encoding="utf-8")
        esp_nn = (ROOT / "modev2/esp_nn_sources.cmake").read_text(
            encoding="utf-8"
        )

        self.assertIn("idf_component_register(", component)
        self.assertIn("${EI_ESP_NN_C_SOURCES}", component)
        self.assertIn("${EI_ESP_NN_ASM_SOURCES}", component)
        self.assertIn("EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1", esp_nn)
        self.assertIn("EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=1", esp_nn)
        self.assertIn("EI_MAX_OVERFLOW_BUFFER_COUNT=256", esp_nn)


class InferenceComponentBehaviorTest(unittest.TestCase):
    def test_esp32s3_calloc_uses_aligned_zeroed_allocator(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            executable = Path(temporary_directory) / "ei_classifier_porting_test"
            compile_result = subprocess.run(
                [
                    "c++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-format-security",
                    "-DCONFIG_IDF_TARGET_ESP32S3=1",
                    "-I",
                    str(ROOT / "tests/host/include"),
                    "-I",
                    str(ROOT / "modev2"),
                    str(ROOT / "tests/host/ei_classifier_porting_test.cpp"),
                    str(
                        ROOT
                        / "modev2/edge-impulse-sdk/porting/espressif/ei_classifier_porting.cpp"
                    ),
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
                "ei calloc alignment behavior passed",
                run_result.stdout,
            )

    def test_esp_nn_overflow_capacity_override_reaches_generated_model(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            executable = Path(temporary_directory) / "esp_nn_overflow_config_test"
            result = subprocess.run(
                [
                    "c++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "modev2"),
                    str(ROOT / "tests/host/esp_nn_overflow_config_test.cpp"),
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
                result.returncode,
                msg=result.stdout + result.stderr,
            )

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
                "mutex-failure": "inference mutex failure passed",
                "no-memory-capture-rgb": "inference allocation failure passed",
                "no-memory-model-rgb": "inference allocation failure passed",
                "no-memory-staging": "inference allocation failure passed",
                "no-memory-published": "inference allocation failure passed",
                "task-failure": "inference task failure rollback passed",
                "invalid-frame": "inference invalid frame cleanup passed",
                "oversized-frame": "inference oversized frame preservation passed",
                "decode-failure": "inference decode failure cleanup passed",
                "classifier-failure": "inference classifier failure passed",
                "resize-failure": "inference resize failure passed",
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
