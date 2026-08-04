#define CONFIG_IDF_TARGET_ESP32S3 1
#define EI_PORTING_ESPRESSIF 0
#define EI_MAX_OVERFLOW_BUFFER_COUNT 256

#include "edge-impulse-sdk/porting/ei_classifier_porting.h"

static_assert(EI_MAX_OVERFLOW_BUFFER_COUNT == 256,
              "The ESP32-S3 SDK default must not override project capacity");

int main()
{
    return 0;
}
