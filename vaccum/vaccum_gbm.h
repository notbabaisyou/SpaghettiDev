#ifndef VACCUM_GBM_H
#define VACCUM_GBM_H

#include "dri3.h"

#define VACCUM_MODULE_NAME "vaccum"

struct gbm_device;
struct gbm_bo;

_X_EXPORT Bool vaccum_gbm_init(ScreenPtr screen, struct gbm_device *gbm,
                               int fd);
_X_EXPORT Bool vaccum_attach_front(PixmapPtr pixmap, struct gbm_bo *bo);
_X_EXPORT struct gbm_bo *vaccum_scanout_bo(ScreenPtr screen,
                                           PixmapPtr pixmap);
_X_EXPORT int vaccum_export_fd(ScreenPtr screen, PixmapPtr pixmap,
                               CARD16 *stride, CARD32 *size);
_X_EXPORT Bool vaccum_attach_fd(PixmapPtr pixmap, int fd, CARD16 width,
                                CARD16 height, CARD16 stride, CARD8 depth,
                                CARD8 bpp);
_X_EXPORT PixmapPtr vaccum_import_fds(ScreenPtr screen, CARD8 num,
                                      const int *fds, CARD16 width,
                                      CARD16 height, const CARD32 *strides,
                                      const CARD32 *offsets, CARD8 depth,
                                      CARD8 bpp, CARD64 modifier);

#endif
