#include "vaccum_priv.h"
#include "vaccum_transfer.h"
static void
vaccum_allocate_staging_buffer(vaccum_screen_private *vaccum_priv)
{
    VkBufferCreateInfo buffer_create_info = {};

    VkMemoryRequirements buf_reqs;

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = buf_reqs.size;
    allocate_info.memoryTypeIndex = 0;

    //    vkAllocateMemory(vaccum_priv->device, &allocate_info, NULL, &image->memory);

    //    vkBindBufferMemory(vaccum_priv->device, image->image, image->memory, 0);
}

void
vaccum_upload_boxes(PixmapPtr pixmap, BoxPtr in_boxes, int in_nbox,
                    int dx_src, int dy_src,
                    int dx_dst, int dy_dst,
                    uint8_t *bits, uint32_t byte_stride)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    vaccum_pixmap_private *priv = vaccum_get_pixmap_private(pixmap);
    int box_index;
    int bytes_per_pixel = pixmap->drawable.bitsPerPixel >> 3;
    const struct vaccum_format *f = vaccum_format_for_pixmap(pixmap);

}

void
vaccum_upload_region(PixmapPtr pixmap, RegionPtr region,
                     int region_x, int region_y,
                     uint8_t *bits, uint32_t byte_stride)
{
    vaccum_upload_boxes(pixmap, RegionRects(region), RegionNumRects(region),
                        -region_x, -region_y,
                        0, 0,
                        bits, byte_stride);
}

/*
 * Take the data in the pixmap and stuff it back into the image
 */
void
vaccum_upload_pixmap(PixmapPtr pixmap)
{
    BoxRec box;

    box.x1 = 0;
    box.x2 = pixmap->drawable.width;
    box.y1 = 0;
    box.y2 = pixmap->drawable.height;
    vaccum_upload_boxes(pixmap, &box, 1, 0, 0, 0, 0,
                        pixmap->devPrivate.ptr, pixmap->devKind);
}

void
vaccum_download_boxes(PixmapPtr pixmap, BoxPtr in_boxes, int in_nbox,
                      int dx_src, int dy_src,
                      int dx_dst, int dy_dst,
                      uint8_t *bits, uint32_t byte_stride)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    vaccum_pixmap_private *priv = vaccum_get_pixmap_private(pixmap);
    int box_index;
    int bytes_per_pixel = pixmap->drawable.bitsPerPixel >> 3;
    const struct vaccum_format *f = vaccum_format_for_pixmap(pixmap);

    {
        
        
    }
}

/*
 * Read data from the pixmap FBO
 */
void
vaccum_download_rect(PixmapPtr pixmap, int x, int y, int w, int h, uint8_t *bits)
{
    BoxRec      box;

    box.x1 = x;
    box.x2 = x + w;
    box.y1 = y;
    box.y2 = y + h;

    vaccum_download_boxes(pixmap, &box, 1, 0, 0, -x, -y,
                          bits, PixmapBytePad(w, pixmap->drawable.depth));
}

/*
 * Pull the data from the FBO down to the pixmap
 */
void
vaccum_download_pixmap(PixmapPtr pixmap)
{
    BoxRec      box;

    box.x1 = 0;
    box.x2 = pixmap->drawable.width;
    box.y1 = 0;
    box.y2 = pixmap->drawable.height;

    vaccum_download_boxes(pixmap, &box, 1, 0, 0, 0, 0,
                          pixmap->devPrivate.ptr, pixmap->devKind);
}
