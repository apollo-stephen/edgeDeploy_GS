#pragma once

#include <stddef.h>

#define MALLOC_CAP_8BIT 0x004
#define MALLOC_CAP_SPIRAM 0x400

#ifdef __cplusplus
extern "C" {
#endif
size_t heap_caps_get_free_size(unsigned int capabilities);
void *heap_caps_malloc(size_t size, unsigned int capabilities);
void heap_caps_free(void *pointer);
#ifdef __cplusplus
}
#endif
