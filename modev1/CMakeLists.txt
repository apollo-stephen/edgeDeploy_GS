cmake_minimum_required(VERSION 3.16)

include("${CMAKE_CURRENT_LIST_DIR}/edge-impulse-sdk/cmake/utils.cmake")

set(EI_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/edge-impulse-sdk")

RECURSIVE_FIND_FILE(EI_CPP_SOURCES "${EI_SDK_DIR}" "*.cpp")
RECURSIVE_FIND_FILE(EI_CC_SOURCES "${EI_SDK_DIR}" "*.cc")
RECURSIVE_FIND_FILE(EI_ASM_SOURCES "${EI_SDK_DIR}" "*.s")

set(EI_C_SOURCES)
RECURSIVE_FIND_FILE_APPEND(
    EI_C_SOURCES
    "${EI_SDK_DIR}/CMSIS/DSP/Source/TransformFunctions"
    "*.c"
)
RECURSIVE_FIND_FILE_APPEND(
    EI_C_SOURCES
    "${EI_SDK_DIR}/CMSIS/DSP/Source/CommonTables"
    "*.c"
)
RECURSIVE_FIND_FILE_APPEND(
    EI_C_SOURCES
    "${EI_SDK_DIR}/CMSIS/DSP/Source/BasicMathFunctions"
    "*.c"
)
RECURSIVE_FIND_FILE_APPEND(
    EI_C_SOURCES
    "${EI_SDK_DIR}/CMSIS/DSP/Source/ComplexMathFunctions"
    "*.c"
)
RECURSIVE_FIND_FILE_APPEND(
    EI_C_SOURCES
    "${EI_SDK_DIR}/CMSIS/DSP/Source/FastMathFunctions"
    "*.c"
)
RECURSIVE_FIND_FILE_APPEND(
    EI_C_SOURCES
    "${EI_SDK_DIR}/CMSIS/DSP/Source/SupportFunctions"
    "*.c"
)
RECURSIVE_FIND_FILE_APPEND(
    EI_C_SOURCES
    "${EI_SDK_DIR}/CMSIS/DSP/Source/MatrixFunctions"
    "*.c"
)
RECURSIVE_FIND_FILE_APPEND(
    EI_C_SOURCES
    "${EI_SDK_DIR}/CMSIS/DSP/Source/StatisticsFunctions"
    "*.c"
)
RECURSIVE_FIND_FILE_APPEND(
    EI_C_SOURCES
    "${EI_SDK_DIR}/CMSIS/NN/Source"
    "*.c"
)
list(APPEND EI_C_SOURCES
    "${EI_SDK_DIR}/tensorflow/lite/c/common.c"
)

set(MODEL_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/tflite-model/tflite_learn_1053141_4_compiled.cpp"
)

idf_component_register(
    SRCS
        ${EI_CPP_SOURCES}
        ${EI_CC_SOURCES}
        ${EI_ASM_SOURCES}
        ${EI_C_SOURCES}
        ${MODEL_SOURCES}
    INCLUDE_DIRS "."
    REQUIRES esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN=0
)
