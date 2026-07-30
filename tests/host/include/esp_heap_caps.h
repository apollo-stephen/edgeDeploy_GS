#pragma once

#include <stddef.h>

#define MALLOC_CAP_SPIRAM 0x400

size_t heap_caps_get_free_size(unsigned int capabilities);
