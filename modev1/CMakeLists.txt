include("${CMAKE_CURRENT_LIST_DIR}/edge-impulse-sdk/cmake/utils.cmake")

set(EI_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/edge-impulse-sdk")

RECURSIVE_FIND_FILE(EI_CPP_SOURCES "${EI_SDK_DIR}" "*.cpp")
RECURSIVE_FIND_FILE(EI_CC_SOURCES "${EI_SDK_DIR}" "*.cc")
include("${CMAKE_CURRENT_LIST_DIR}/esp_nn_sources.cmake")

idf_component_register(
    SRCS
        ${EI_CPP_SOURCES}
        ${EI_CC_SOURCES}
        ${EI_ESP_NN_C_SOURCES}
        ${EI_ESP_NN_ASM_SOURCES}
        "${EI_SDK_DIR}/tensorflow/lite/c/common.c"
        "tflite-model/tflite_learn_1053141_4_compiled.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    EIDSP_USE_ESP_DSP=0
    ${EI_ESP_NN_COMPILE_DEFINITIONS}
)

# Edge Impulse SDK 1.94.2 triggers a GCC 14 false positive in TFLM's Select
# kernel. Keep the warning visible, but do not make this third-party warning
# fatal for the component.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=maybe-uninitialized
)
