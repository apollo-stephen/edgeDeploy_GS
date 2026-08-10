#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_DEFAULT (1U << 12)
#define MALLOC_CAP_INTERNAL (1U << 11)
#define MALLOC_CAP_8BIT 0x004
#define MALLOC_CAP_SPIRAM 0x400

#ifdef __cplusplus
extern "C" {
#endif
size_t heap_caps_get_free_size(unsigned int capabilities);
size_t heap_caps_get_minimum_free_size(unsigned int capabilities);
size_t heap_caps_get_largest_free_block(unsigned int capabilities);
void *heap_caps_malloc(size_t size, unsigned int capabilities);
void *heap_caps_aligned_alloc(size_t alignment,
                              size_t size,
                              uint32_t capabilities);
void *heap_caps_aligned_calloc(size_t alignment,
                               size_t n,
                               size_t size,
                               uint32_t capabilities);
void *heap_caps_calloc(size_t n,
                       size_t size,
                       uint32_t capabilities);
void heap_caps_free(void *pointer);
#ifdef __cplusplus
}
#endif
