import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    path = ROOT / relative_path
    return path.read_text(encoding="utf-8") if path.is_file() else ""


class WifiApComponentStructureTest(unittest.TestCase):
    def test_public_component_files_exist(self):
        self.assertTrue((ROOT / "components/WIFIAP/include/wifi_ap.h").is_file())
        self.assertTrue((ROOT / "components/WIFIAP/wifi_ap.c").is_file())
        self.assertTrue((ROOT / "components/WIFIAP/CMakeLists.txt").is_file())
        self.assertTrue((ROOT / "components/WIFIAP/Kconfig").is_file())

    def test_public_api_is_minimal(self):
        header = read("components/WIFIAP/include/wifi_ap.h")
        self.assertIn("esp_err_t wifi_ap_init(void);", header)
        self.assertIn("const char *wifi_ap_get_ip(void);", header)

    def test_main_uses_component_interface(self):
        main = read("main/main.c")
        self.assertIn('#include "wifi_ap.h"', main)
        self.assertIn("wifi_ap_init()", main)
        self.assertNotIn('#include "esp_wifi.h"', main)

    def test_old_example_entry_is_removed(self):
        self.assertFalse((ROOT / "main/softap_example_main.c").exists())

    def test_component_dependencies_are_explicit(self):
        component_cmake = read("components/WIFIAP/CMakeLists.txt")
        main_cmake = read("main/CMakeLists.txt")
        self.assertIn('SRCS "wifi_ap.c"', component_cmake)
        self.assertIn("REQUIRES esp_wifi", component_cmake)
        self.assertIn('SRCS "main.c"', main_cmake)
        self.assertIn("WIFIAP", main_cmake)

    def test_password_is_not_logged(self):
        source = read("components/WIFIAP/wifi_ap.c")
        log_lines = [line.lower() for line in source.splitlines() if "ESP_LOG" in line]
        self.assertTrue(log_lines)
        for line in log_lines:
            self.assertNotIn("password", line)


if __name__ == "__main__":
    unittest.main()
