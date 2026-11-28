#include "dix-config.h"
#include "vaccum_gbm_priv.h"
#include "vaccum_dri3_syncobj.h"
#include "gcstruct.h"
#include "present.h"
#ifdef HAVE_XSHMFENCE
#include "misyncshm.h"
#endif

DevPrivateKeyRec vaccum_gbm_screen_key, vaccum_gbm_pixmap_key;

static PixmapPtr
create_pixmap(ScreenPtr screen, int w, int h, int depth, unsigned usage)
{
    struct vaccum_gbm_screen *priv = screen_data(screen);
    Bool valid_width = w > 0 && w <= INT16_MAX;
    Bool valid_height = h > 0 && h <= INT16_MAX;
    Bool eligible = !usage && BitsPerPixel(depth) == 32;

    if (!eligible || !valid_width || !valid_height)
        goto fallback;

    PixmapPtr pixmap = priv->create_pixmap(screen, 0, 0, depth, usage);

    if (!pixmap)
        goto fallback;

    struct vaccum_linear_pixmap *pixels = &pixmap_data(pixmap)->pixels;
    int stride = PixmapBytePad(w, depth);

    if (vaccum_linear_allocate(priv->gpu, pixels, w, h, stride) &&
        screen->ModifyPixmapHeader(pixmap, w, h, depth, 32, pixels->stride,
                                   pixels->map))
        return pixmap;
    screen->DestroyPixmap(pixmap);
fallback:
    return priv->create_pixmap(screen, w, h, depth, usage);
}

static Bool
destroy_pixmap(PixmapPtr pixmap)
{
    if (pixmap->refcnt == 1)
        vaccum_release_backing(pixmap);
    return screen_data(pixmap->drawable.pScreen)->destroy_pixmap(pixmap);
}

static void
wait_pixels(PixmapPtr pixmap)
{
    struct vaccum_linear_pixmap *pixels = &pixmap_data(pixmap)->pixels;
    struct vaccum_gbm_screen *priv = screen_data(pixmap->drawable.pScreen);

    vaccum_linear_wait(priv->gpu, pixels);
}

static Bool
modify_pixmap_header(PixmapPtr pixmap, int width, int height, int depth,
                      int bpp, int stride, void *pixels)
{
    struct vaccum_gbm_screen *priv = screen_data(pixmap->drawable.pScreen);

    wait_pixels(pixmap);
    return priv->modify_pixmap_header(pixmap, width, height, depth, bpp,
                                     stride, pixels);
}

static void
block_handler(ScreenPtr screen, void *timeout)
{
    struct vaccum_gbm_screen *priv = screen_data(screen);

    vaccum_linear_flush(priv->gpu);
    screen->BlockHandler = priv->block_handler;
    screen->BlockHandler(screen, timeout);
    priv->block_handler = screen->BlockHandler;
    screen->BlockHandler = block_handler;
}

static void
copy_aliased_boxes(DrawablePtr src, DrawablePtr dst, GCPtr gc, BoxPtr boxes,
                    int count, int dx, int dy, Bool reverse, Bool upsidedown,
                    Pixel bitplane, void *closure)
{
    if (!count)
        return;

    BoxRec bounds = boxes[0];

    for (int i = 1; i < count; i++) {
        bounds.x1 = min(bounds.x1, boxes[i].x1);
        bounds.y1 = min(bounds.y1, boxes[i].y1);
        bounds.x2 = max(bounds.x2, boxes[i].x2);
        bounds.y2 = max(bounds.y2, boxes[i].y2);
    }

    int width = bounds.x2 - bounds.x1;
    int height = bounds.y2 - bounds.y1;
    ScreenPtr screen = src->pScreen;
    PixmapPtr snapshot = fbCreatePixmap(screen, width, height, src->depth, 0);

    if (!snapshot)
        return;

    /* The destination may overlap every source row in a different mapping. */
    uint64_t failures = fbPixmapAccessFailures(screen);
    BoxRec area = { 0, 0, width, height };

    fbCopyNtoN(src, &snapshot->drawable, NULL, &area, 1, dx + bounds.x1,
               dy + bounds.y1, FALSE, FALSE, 0, NULL);
    if (failures == fbPixmapAccessFailures(screen))
        fbCopyNtoN(&snapshot->drawable, dst, gc, boxes, count, -bounds.x1,
                   -bounds.y1, FALSE, FALSE, 0, NULL);
    fbDestroyPixmap(snapshot);
}

static RegionPtr
copy_area(DrawablePtr src, DrawablePtr dst, GCPtr gc, int x, int y, int w,
          int h, int tx, int ty)
{
    Bool pixmaps = src->type == DRAWABLE_PIXMAP &&
                   dst->type == DRAWABLE_PIXMAP;

    if (!pixmaps || src->pScreen != dst->pScreen)
        return fbCopyArea(src, dst, gc, x, y, w, h, tx, ty);

    PixmapPtr source = (PixmapPtr)src;
    PixmapPtr target = (PixmapPtr)dst;
    struct vaccum_gbm_pixmap *s = pixmap_data(source);
    struct vaccum_gbm_pixmap *d = pixmap_data(target);
    Bool shared_buffer = s->dmabuf && s->dmabuf == d->dmabuf;
    Bool original_mappings = s->map == source->devPrivate.ptr &&
                             d->map == target->devPrivate.ptr;

    if (source != target && shared_buffer && original_mappings)
        return miDoCopy(src, dst, gc, x, y, w, h, tx, ty, copy_aliased_boxes,
                        0, NULL);

    return vaccum_linear_copy_area(screen_data(dst->pScreen)->gpu, &s->pixels,
                                   &d->pixels, src, dst, gc, x, y, w, h, tx,
                                   ty);
}

static void
validate_gc(GCPtr gc, unsigned long changes, DrawablePtr drawable)
{
    fbValidateGC(gc, changes, drawable);
    gc->ops = &screen_data(drawable->pScreen)->ops;
}

static GCFuncs gc_funcs = {
    validate_gc,  miChangeGC,    miCopyGC,   miDestroyGC,
    miChangeClip, miDestroyClip, miCopyClip,
};

static Bool
create_gc(GCPtr gc)
{
    if (!screen_data(gc->pScreen)->create_gc(gc))
        return FALSE;
    gc->funcs = &gc_funcs;
    return TRUE;
}

static Bool
close_screen(ScreenPtr screen)
{
    struct vaccum_gbm_screen *priv = screen_data(screen);

    vaccum_linear_flush(priv->gpu);
    screen->BlockHandler = priv->block_handler;
    screen->CreateGC = priv->create_gc;
    screen->CreatePixmap = priv->create_pixmap;
    screen->CloseScreen = priv->close;
    Bool closed = screen->CloseScreen(screen);

    screen->DestroyPixmap = priv->destroy_pixmap;
    screen->ModifyPixmapHeader = priv->modify_pixmap_header;
    present_set_copy_failures_proc(screen, NULL);
    fbSetPixmapCheckedAccessProc(screen, NULL);
    fbSetPixmapAccessProc(screen, NULL);
    assert(xorg_list_is_empty(priv->dmabufs));
    free(priv->dmabufs);
    vaccum_linear_fini(priv->gpu);
    return closed;
}

#ifdef DRI3
static struct dri3_syncobj *
import_syncobj(ClientPtr client, ScreenPtr screen, XID id, int fd)
{
    return vaccum_syncobj_import(screen, id, screen_data(screen)->fd, fd);
}
#endif

Bool
vaccum_gbm_init(ScreenPtr screen, struct gbm_device *gbm, int fd)
{
    if (!dixRegisterPrivateKey(&vaccum_gbm_screen_key, PRIVATE_SCREEN,
                               sizeof(struct vaccum_gbm_screen)) ||
        !dixRegisterPrivateKey(&vaccum_gbm_pixmap_key, PRIVATE_PIXMAP,
                               sizeof(struct vaccum_gbm_pixmap)))
        return FALSE;

    /* Screen private storage may move while extensions initialize. */
    struct xorg_list *dmabufs = malloc(sizeof(*dmabufs));

    if (!dmabufs)
        return FALSE;
    xorg_list_init(dmabufs);

    struct vaccum_screen_private *gpu = vaccum_linear_init(fd);

    if (!gpu)
        goto free_dmabufs;
    if (!fbSetPixmapAccessProc(screen, wait_pixels))
        goto fail;
    if (!fbSetPixmapCheckedAccessProc(screen, vaccum_cpu_access))
        goto clear_wait;
    if (!present_set_copy_failures_proc(screen, fbPixmapAccessFailures))
        goto clear_access;

#ifdef HAVE_XSHMFENCE
    if (!miSyncShmScreenInit(screen))
        goto clear_present;
#endif
#ifdef DRI3
    static dri3_screen_info_rec timeline_info;
    const dri3_screen_info_rec *info = &vaccum_gbm_dri3;

    if (vaccum_syncobj_supported(fd)) {
        timeline_info = vaccum_gbm_dri3;
        timeline_info.version = 4;
        timeline_info.import_syncobj = import_syncobj;
        info = &timeline_info;
    }
    if (!dri3_screen_init(screen, info))
        goto clear_present;
#endif

    struct vaccum_gbm_screen *priv = screen_data(screen);

    priv->gpu = gpu;
    priv->gbm = gbm;
    priv->fd = fd;
    priv->dmabufs = dmabufs;
    priv->ops = fbGCOps;
    priv->ops.CopyArea = copy_area;
    priv->create_gc = screen->CreateGC;
    priv->create_pixmap = screen->CreatePixmap;
    priv->destroy_pixmap = screen->DestroyPixmap;
    priv->close = screen->CloseScreen;
    priv->block_handler = screen->BlockHandler;
    priv->modify_pixmap_header = screen->ModifyPixmapHeader;
    screen->BlockHandler = block_handler;
    screen->ModifyPixmapHeader = modify_pixmap_header;
    screen->CreatePixmap = create_pixmap;
    screen->DestroyPixmap = destroy_pixmap;
    screen->CreateGC = create_gc;
    screen->CloseScreen = close_screen;
    return TRUE;
#if defined(HAVE_XSHMFENCE) || defined(DRI3)
clear_present:
    present_set_copy_failures_proc(screen, NULL);
#endif
clear_access:
    fbSetPixmapCheckedAccessProc(screen, NULL);
clear_wait:
    fbSetPixmapAccessProc(screen, NULL);
fail:
    vaccum_linear_fini(gpu);
free_dmabufs:
    free(dmabufs);
    return FALSE;
}
