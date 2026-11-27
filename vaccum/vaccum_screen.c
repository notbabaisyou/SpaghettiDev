#include "dix-config.h"
#include "vaccum_gbm_priv.h"
#include "gcstruct.h"
#include "picturestr.h"
#include "os/xserver_poll.h"
#include <errno.h>
#include <gbm.h>
#include <poll.h>
#include <unistd.h>
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

    if (vaccum_linear_allocate(priv->gpu, pixels, stride, h) &&
        screen->ModifyPixmapHeader(pixmap, w, h, depth, 32, stride,
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
wait_drawable(DrawablePtr drawable, short events)
{
    PixmapPtr pixmap =
        drawable->type == DRAWABLE_PIXMAP
            ? (PixmapPtr)drawable
            : drawable->pScreen->GetWindowPixmap((WindowPtr)drawable);
    struct vaccum_gbm_pixmap *priv = pixmap_data(pixmap);

    if (!priv->bo)
        return;

    struct pollfd fence = {.fd = gbm_bo_get_fd(priv->bo), .events = events};

    if (fence.fd < 0)
        return;

    while (xserver_poll(&fence, 1, -1) < 0 && errno == EINTR)
        continue;

    close(fence.fd);
}

static void
get_image(DrawablePtr draw, int x, int y, int w, int h, unsigned format,
          unsigned long mask, char *pixels)
{
    struct vaccum_gbm_screen *priv = screen_data(draw->pScreen);

    wait_drawable(draw, POLLIN);
    priv->get_image(draw, x, y, w, h, format, mask, pixels);
}

static void
composite(CARD8 op, PicturePtr src, PicturePtr mask, PicturePtr dst, INT16 sx,
          INT16 sy, INT16 mx, INT16 my, INT16 dx, INT16 dy, CARD16 width,
          CARD16 height)
{
    struct vaccum_gbm_screen *priv = screen_data(dst->pDrawable->pScreen);

    if (src && src->pDrawable)
        wait_drawable(src->pDrawable, POLLIN);
    if (mask && mask->pDrawable)
        wait_drawable(mask->pDrawable, POLLIN);
    wait_drawable(dst->pDrawable, POLLOUT);
    priv->composite(op, src, mask, dst, sx, sy, mx, my, dx, dy, width,
                    height);
}

static RegionPtr
copy_area(DrawablePtr src, DrawablePtr dst, GCPtr gc, int x, int y, int w,
          int h, int tx, int ty)
{
    wait_drawable(src, POLLIN);
    wait_drawable(dst, POLLOUT);
    if (src->type != DRAWABLE_PIXMAP || dst->type != DRAWABLE_PIXMAP)
        return fbCopyArea(src, dst, gc, x, y, w, h, tx, ty);

    struct vaccum_linear_pixmap *s = &pixmap_data((PixmapPtr)src)->pixels;
    struct vaccum_linear_pixmap *d = &pixmap_data((PixmapPtr)dst)->pixels;

    return vaccum_linear_copy_area(screen_data(dst->pScreen)->gpu, s, d, src,
                                   dst, gc, x, y, w, h, tx, ty);
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

    screen->GetImage = priv->get_image;
    if (priv->composite)
        GetPictureScreen(screen)->Composite = priv->composite;
    screen->CreateGC = priv->create_gc;
    screen->CreatePixmap = priv->create_pixmap;
    screen->CloseScreen = priv->close;
    Bool closed = screen->CloseScreen(screen);

    screen->DestroyPixmap = priv->destroy_pixmap;
    vaccum_linear_fini(priv->gpu);
    return closed;
}

Bool
vaccum_gbm_init(ScreenPtr screen, struct gbm_device *gbm, int fd)
{
    if (!dixRegisterPrivateKey(&vaccum_gbm_screen_key, PRIVATE_SCREEN,
                               sizeof(struct vaccum_gbm_screen)) ||
        !dixRegisterPrivateKey(&vaccum_gbm_pixmap_key, PRIVATE_PIXMAP,
                               sizeof(struct vaccum_gbm_pixmap)))
        return FALSE;

    struct vaccum_screen_private *gpu = vaccum_linear_init(fd);

    if (!gpu)
        return FALSE;
#ifdef HAVE_XSHMFENCE
    if (!miSyncShmScreenInit(screen))
        goto fail;
#endif
#ifdef DRI3
    if (!dri3_screen_init(screen, &vaccum_gbm_dri3))
        goto fail;
#endif

    struct vaccum_gbm_screen *priv = screen_data(screen);
    PictureScreenPtr picture = GetPictureScreenIfSet(screen);

    priv->gpu = gpu;
    priv->gbm = gbm;
    priv->fd = fd;

    if (picture) {
        priv->composite = picture->Composite;
        picture->Composite = composite;
    }

    priv->get_image = screen->GetImage;
    screen->GetImage = get_image;
    priv->ops = fbGCOps;
    priv->ops.CopyArea = copy_area;
    priv->create_gc = screen->CreateGC;
    priv->create_pixmap = screen->CreatePixmap;
    priv->destroy_pixmap = screen->DestroyPixmap;
    priv->close = screen->CloseScreen;
    screen->CreatePixmap = create_pixmap;
    screen->DestroyPixmap = destroy_pixmap;
    screen->CreateGC = create_gc;
    screen->CloseScreen = close_screen;
    return TRUE;
fail:
    vaccum_linear_fini(gpu);
    return FALSE;
}
