#ifndef VACCUM_LINEAR_H
#define VACCUM_LINEAR_H

#include "fb.h"
#include <vulkan/vulkan.h>

struct vaccum_screen_private;

struct vaccum_linear_pixmap {
    VkBuffer buffer;
    VkImage image;
    VkImageLayout layout;
    VkDeviceMemory memory;
    void *map;
    int stride;
    int width;
    int height;
};

struct vaccum_screen_private *vaccum_linear_init(int fd);
void vaccum_linear_fini(struct vaccum_screen_private *screen);
Bool vaccum_linear_allocate(struct vaccum_screen_private *screen,
                            struct vaccum_linear_pixmap *pixels,
                            int width, int height, int stride);
void vaccum_linear_release(struct vaccum_screen_private *screen,
                           struct vaccum_linear_pixmap *pixels);
Bool vaccum_linear_copy(struct vaccum_screen_private *screen,
                        struct vaccum_linear_pixmap *src,
                        struct vaccum_linear_pixmap *dst,
                        BoxPtr boxes, int count, int dx, int dy);

RegionPtr vaccum_linear_copy_area(struct vaccum_screen_private *screen,
                                  struct vaccum_linear_pixmap *src,
                                  struct vaccum_linear_pixmap *dst,
                                  DrawablePtr source, DrawablePtr dest,
                                  GCPtr gc, int srcx, int srcy, int width,
                                  int height, int dstx, int dsty);

#endif
