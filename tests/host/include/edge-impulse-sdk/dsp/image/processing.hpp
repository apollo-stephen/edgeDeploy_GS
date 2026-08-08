#pragma once

#include <stdint.h>

namespace ei { namespace image { namespace processing {
int resize_image_using_mode(const uint8_t *src_image,
                            int src_width,
                            int src_height,
                            uint8_t *dst_image,
                            int dst_width,
                            int dst_height,
                            int pixel_size_bytes,
                            int mode);
}}}
