set(EI_ESP_NN_ROOT "${EI_SDK_DIR}/porting/espressif/ESP-NN")
# This EON model has 52 convolution nodes. ESP-NN registers persistent and
# scratch allocations per node, so the SDK's ESP32-S3 default of 30 slots is
# exhausted during model setup. The table stores pointers only; buffers remain
# demand-allocated by Edge Impulse.
set(EI_ESP_NN_COMPILE_DEFINITIONS
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=1
    EI_MAX_OVERFLOW_BUFFER_COUNT=256
)

RECURSIVE_FIND_FILE(EI_ESP_NN_C_SOURCES "${EI_ESP_NN_ROOT}" "*.c")
RECURSIVE_FIND_FILE(EI_ESP_NN_ASM_SOURCES "${EI_ESP_NN_ROOT}" "*.S")
