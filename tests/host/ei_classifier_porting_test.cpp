#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edge-impulse-sdk/porting/ei_classifier_porting.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"

namespace {

size_t aligned_calloc_calls;
size_t ordinary_calloc_calls;
size_t last_alignment;
size_t last_nitems;
size_t last_item_size;
uint32_t last_capabilities;

void *allocate_aligned(size_t alignment, size_t size)
{
    void *allocation = nullptr;
    if (posix_memalign(&allocation, alignment, size) != 0) {
        return nullptr;
    }
    return allocation;
}

} // namespace

extern "C" void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

extern "C" int64_t esp_timer_get_time(void)
{
    return 0;
}

extern "C" size_t heap_caps_get_free_size(unsigned int capabilities)
{
    (void)capabilities;
    return 0;
}

extern "C" void *heap_caps_malloc(size_t size, unsigned int capabilities)
{
    (void)capabilities;
    return malloc(size);
}

extern "C" void *heap_caps_aligned_alloc(size_t alignment,
                                           size_t size,
                                           uint32_t capabilities)
{
    (void)capabilities;
    return allocate_aligned(alignment, size);
}

extern "C" void *heap_caps_aligned_calloc(size_t alignment,
                                            size_t n,
                                            size_t size,
                                            uint32_t capabilities)
{
    aligned_calloc_calls++;
    last_alignment = alignment;
    last_nitems = n;
    last_item_size = size;
    last_capabilities = capabilities;

    void *allocation = allocate_aligned(alignment, n * size);
    if (allocation != nullptr) {
        memset(allocation, 0, n * size);
    }
    return allocation;
}

extern "C" void *heap_caps_calloc(size_t n,
                                    size_t size,
                                    uint32_t capabilities)
{
    ordinary_calloc_calls++;
    (void)capabilities;
    return calloc(n, size);
}

extern "C" void heap_caps_free(void *pointer)
{
    free(pointer);
}

int main()
{
    constexpr size_t kItems = 9;
    constexpr size_t kItemSize = 64;

    void *allocation = ei_calloc(kItems, kItemSize);
    assert(allocation != nullptr);
    assert(aligned_calloc_calls == 1);
    assert(ordinary_calloc_calls == 0);
    assert(last_alignment == 16);
    assert(last_nitems == kItems);
    assert(last_item_size == kItemSize);
    assert(last_capabilities == MALLOC_CAP_DEFAULT);
    assert(reinterpret_cast<uintptr_t>(allocation) % 16 == 0);

    const auto *bytes = static_cast<const unsigned char *>(allocation);
    for (size_t index = 0; index < kItems * kItemSize; ++index) {
        assert(bytes[index] == 0);
    }

    ei_free(allocation);
    puts("ei calloc alignment behavior passed");
    return 0;
}
