#pragma once

#ifdef ESP_LOG_CAPTURE

#ifdef __cplusplus
extern "C" {
#endif
void test_log_write(const char *level,
                    const char *tag,
                    const char *format,
                    ...);
#ifdef __cplusplus
}
#endif

#define ESP_LOGE(tag, format, ...) \
    test_log_write("E", tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) \
    test_log_write("W", tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) \
    test_log_write("I", tag, format, ##__VA_ARGS__)

#else

#define ESP_LOGE(tag, format, ...) ((void)(tag))
#define ESP_LOGW(tag, format, ...) ((void)(tag))
#define ESP_LOGI(tag, format, ...) ((void)(tag))

#endif
