cmake_minimum_required(VERSION 3.16)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(MODEV1_ROOT "${PROJECT_ROOT}/modev1")
set(EI_SDK_DIR "${MODEV1_ROOT}/edge-impulse-sdk")
set(ESP_NN_ROOT "${EI_SDK_DIR}/porting/espressif/ESP-NN")

include("${EI_SDK_DIR}/cmake/utils.cmake")
include("${MODEV1_ROOT}/esp_nn_sources.cmake")

list(LENGTH EI_ESP_NN_C_SOURCES ESP_NN_C_COUNT)
list(LENGTH EI_ESP_NN_ASM_SOURCES ESP_NN_ASM_COUNT)
if(ESP_NN_C_COUNT LESS 1)
    message(FATAL_ERROR "No bundled ESP-NN C sources were discovered")
endif()
if(ESP_NN_ASM_COUNT LESS 1)
    message(FATAL_ERROR "No bundled ESP-NN assembly sources were discovered")
endif()

set(S3_CONV_SOURCE
    "${ESP_NN_ROOT}/src/convolution/esp_nn_conv_esp32s3.c"
)
set(S3_COMMON_ASM
    "${ESP_NN_ROOT}/src/common/esp_nn_common_functions_esp32s3.S"
)
list(FIND EI_ESP_NN_C_SOURCES "${S3_CONV_SOURCE}" S3_CONV_INDEX)
list(FIND EI_ESP_NN_ASM_SOURCES "${S3_COMMON_ASM}" S3_COMMON_INDEX)
if(S3_CONV_INDEX EQUAL -1)
    message(FATAL_ERROR "ESP32-S3 convolution implementation is missing")
endif()
if(S3_COMMON_INDEX EQUAL -1)
    message(FATAL_ERROR "ESP32-S3 common assembly implementation is missing")
endif()

list(FIND EI_ESP_NN_COMPILE_DEFINITIONS
    "EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=1"
    ESP_NN_DEFINE_INDEX
)
list(FIND EI_ESP_NN_COMPILE_DEFINITIONS
    "EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3=1"
    ESP_NN_S3_DEFINE_INDEX
)
if(ESP_NN_DEFINE_INDEX EQUAL -1)
    message(FATAL_ERROR "ESP-NN compile definition is not enabled")
endif()
if(ESP_NN_S3_DEFINE_INDEX EQUAL -1)
    message(FATAL_ERROR "ESP32-S3 ESP-NN compile definition is not enabled")
endif()

foreach(SOURCE_FILE IN LISTS EI_ESP_NN_C_SOURCES EI_ESP_NN_ASM_SOURCES)
    string(FIND "${SOURCE_FILE}" "${ESP_NN_ROOT}/" SOURCE_PREFIX_INDEX)
    if(NOT SOURCE_PREFIX_INDEX EQUAL 0)
        message(FATAL_ERROR "Source outside bundled ESP-NN tree: ${SOURCE_FILE}")
    endif()
endforeach()

message(STATUS
    "Discovered ${ESP_NN_C_COUNT} ESP-NN C and ${ESP_NN_ASM_COUNT} assembly sources"
)
