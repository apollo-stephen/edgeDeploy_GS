import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    path = ROOT / relative_path
    return path.read_text(encoding="utf-8") if path.is_file() else ""


def esp_log_calls(source: str) -> list[str]:
    return [
        match.group(0)
        for match in re.finditer(r"ESP_LOG[A-Z]*\s*\(.*?\);", source, re.DOTALL)
    ]


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
        log_calls = esp_log_calls(source)
        self.assertTrue(log_calls)
        for call in log_calls:
            self.assertNotIn("password", call.lower())
            self.assertNotIn("WIFI_AP_PASSWORD", call)

    def test_password_log_scanner_handles_multiline_calls(self):
        unsafe_source = """
        ESP_LOGI(TAG,
                 "password=%s",
                 WIFI_AP_PASSWORD);
        """
        log_calls = esp_log_calls(unsafe_source)
        self.assertEqual(1, len(log_calls))
        self.assertIn("password", log_calls[0].lower())
        self.assertIn("WIFI_AP_PASSWORD", log_calls[0])

    def test_partial_initialization_has_reverse_order_cleanup(self):
        source = read("components/WIFIAP/wifi_ap.c")
        self.assertIn("cleanup_partial_init", source)
        self.assertIn("esp_wifi_stop()", source)
        self.assertIn("esp_event_handler_unregister", source)
        self.assertIn("esp_netif_destroy_default_wifi", source)
        self.assertIn("esp_wifi_deinit()", source)
        self.assertIn("esp_event_loop_delete_default()", source)

    def test_ip_getter_distinguishes_not_ready(self):
        header = read("components/WIFIAP/include/wifi_ap.h")
        source = read("components/WIFIAP/wifi_ap.c")
        self.assertIn("empty string until", header)
        self.assertIn('static char s_ap_ip[16] = "";', source)


if __name__ == "__main__":
    unittest.main()
