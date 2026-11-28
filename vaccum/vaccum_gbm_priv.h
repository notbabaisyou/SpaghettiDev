#ifndef VACCUM_GBM_PRIV_H
#define VACCUM_GBM_PRIV_H

#include "list.h"
#include "vaccum_linear.h"
#include "vaccum_gbm.h"

struct vaccum_gbm_screen {
    struct vaccum_screen_private *gpu;
    struct gbm_device *gbm;
    int fd;
    struct xorg_list *dmabufs;
    CreatePixmapProcPtr create_pixmap;
    DestroyPixmapProcPtr destroy_pixmap;
    CreateGCProcPtr create_gc;
    CloseScreenProcPtr close;
    ScreenBlockHandlerProcPtr block_handler;
    ModifyPixmapHeaderProcPtr modify_pixmap_header;
    GCOps ops;
};

struct vaccum_gbm_pixmap {
    struct vaccum_linear_pixmap pixels;
    struct gbm_bo *bo;
    struct vaccum_dmabuf *dmabuf;
    void *map;
    size_t length;
};

extern DevPrivateKeyRec vaccum_gbm_screen_key, vaccum_gbm_pixmap_key;

static inline struct vaccum_gbm_screen *
screen_data(ScreenPtr screen)
{
    return dixLookupPrivate(&screen->devPrivates, &vaccum_gbm_screen_key);
}

static inline struct vaccum_gbm_pixmap *
pixmap_data(PixmapPtr pixmap)
{
    return dixLookupPrivate(&pixmap->devPrivates, &vaccum_gbm_pixmap_key);
}

void vaccum_release_backing(PixmapPtr pixmap);
void vaccum_cpu_access(PixmapPtr pixmap, FbPixmapAccess access);
extern const dri3_screen_info_rec vaccum_gbm_dri3;

#endif
