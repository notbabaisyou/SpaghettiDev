#ifndef _VACCUM_TRANSFER_H_
#define _VACCUM_TRANSFER_H_

void
vaccum_upload_boxes(PixmapPtr pixmap, BoxPtr in_boxes, int in_nbox,
                    int dx_src, int dy_src,
                    int dx_dst, int dy_dst,
                    uint8_t *bits, uint32_t byte_stride);

void
vaccum_upload_region(PixmapPtr pixmap, RegionPtr region,
                     int region_x, int region_y,
                     uint8_t *bits, uint32_t byte_stride);

void
vaccum_upload_pixmap(PixmapPtr pixmap);

void
vaccum_download_boxes(PixmapPtr pixmap, BoxPtr in_boxes, int in_nbox,
                      int dx_src, int dy_src,
                      int dx_dst, int dy_dst,
                      uint8_t *bits, uint32_t byte_stride);

void
vaccum_download_rect(PixmapPtr pixmap, int x, int y, int w, int h, uint8_t *bits);

void
vaccum_download_pixmap(PixmapPtr pixmap);

#endif
