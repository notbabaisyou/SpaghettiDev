#include "vaccum_priv.h"

#include <drm_fourcc.h>
#include <xf86drm.h>
#ifdef DRI3
#include "dri3.h"
#endif

DevPrivateKeyRec vaccum_screen_private_key;
DevPrivateKeyRec vaccum_pixmap_private_key;
DevPrivateKeyRec vaccum_gc_private_key;

vaccum_screen_private *
vaccum_get_screen_private(ScreenPtr screen)
{
    return (vaccum_screen_private *)
        dixLookupPrivate(&screen->devPrivates, &vaccum_screen_private_key);
}

void
vaccum_set_screen_private(ScreenPtr screen, vaccum_screen_private *priv)
{
    dixSetPrivate(&screen->devPrivates, &vaccum_screen_private_key, priv);
}

/**
 * vaccum_get_drawable_pixmap() returns a backing pixmap for a given drawable.
 *
 * @param drawable the drawable being requested.
 *
 * This function returns the backing pixmap for a drawable, whether it is a
 * redirected window, unredirected window, or already a pixmap.  Note that
 * coordinate translation is needed when drawing to the backing pixmap of a
 * redirected window, and the translation coordinates are provided by calling
 * exaGetOffscreenPixmap() on the drawable.
 */
PixmapPtr
vaccum_get_drawable_pixmap(DrawablePtr drawable)
{
    if (drawable->type == DRAWABLE_WINDOW)
        return drawable->pScreen->GetWindowPixmap((WindowPtr) drawable);
    else
        return (PixmapPtr) drawable;
}

static void
vaccum_init_pixmap_private_small(PixmapPtr pixmap, vaccum_pixmap_private *pixmap_priv)
{
    pixmap_priv->box.x1 = 0;
    pixmap_priv->box.x2 = pixmap->drawable.width;
    pixmap_priv->box.y1 = 0;
    pixmap_priv->box.y2 = pixmap->drawable.height;
}

PixmapPtr
vaccum_create_pixmap(ScreenPtr screen, int w, int h, int depth,
                     unsigned int usage)
{
    PixmapPtr pixmap;
    vaccum_pixmap_private *pixmap_priv;
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    int pitch;
    struct vaccum_image *image = NULL;

    if (w > 32767 || h > 32767)
        return NullPixmap;

    if ((usage == VACCUM_CREATE_PIXMAP_CPU
         || (usage == CREATE_PIXMAP_USAGE_GLYPH_PICTURE &&
             w <= vaccum_priv->glyph_max_dim &&
             h <= vaccum_priv->glyph_max_dim)
         || (w == 0 && h == 0)
         || !vaccum_priv->formats[depth].format))
        return fbCreatePixmap(screen, w, h, depth, usage);

    pixmap = fbCreatePixmap(screen, 0, 0, depth, usage);

    pixmap_priv = vaccum_get_pixmap_private(pixmap);

    pitch = (((w * pixmap->drawable.bitsPerPixel + 7) / 8) + 3) & ~3;
    screen->ModifyPixmapHeader(pixmap, w, h, 0, 0, pitch, NULL);

    pixmap_priv->type = VACCUM_IMAGE_ONLY;

    if (usage == VACCUM_CREATE_PIXMAP_NO_TEXTURE) {
        vaccum_init_pixmap_private_small(pixmap, pixmap_priv);
        return pixmap;
    }

    if (usage == VACCUM_CREATE_NO_LARGE ||
        vaccum_check_fbo_size(vaccum_priv, w, h)) {
        vaccum_init_pixmap_private_small(pixmap, pixmap_priv);

        image = vaccum_create_image(vaccum_priv, pixmap, w, h);
    }

    if (image == NULL) {
        fbDestroyPixmap(pixmap);
        return fbCreatePixmap(screen, w, h, depth, usage);
    }

    vaccum_pixmap_attach_image(pixmap, image);

    return pixmap;
}

Bool
vaccum_destroy_pixmap(PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(pScreen);
    vaccum_pixmap_private *pixmap_priv = vaccum_get_pixmap_private(pixmap);
    if (pixmap_priv->image)
        vaccum_destroy_image(vaccum_priv, pixmap_priv->image);
    return fbDestroyPixmap(pixmap);
}

const struct vaccum_format *
vaccum_format_for_pixmap(PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(pScreen);
    vaccum_pixmap_private *pixmap_priv = vaccum_get_pixmap_private(pixmap);

    return &vaccum_priv->formats[pixmap->drawable.depth];
}

static void vaccum_block_handler_wrapper(ScreenPtr screen, void *timeout);

Bool
vaccum_init(ScreenPtr screen, unsigned int flags, int drm_fd)
{
    vaccum_screen_private *vaccum_priv;

    vaccum_priv = calloc(1, sizeof(*vaccum_priv));
    if (vaccum_priv == NULL)
        return FALSE;

    if (!dixRegisterPrivateKey(&vaccum_screen_private_key, PRIVATE_SCREEN, 0)) {
        LogMessage(X_WARNING,
                   "vaccum%d: Failed to allocate screen private\n",
                   screen->myNum);
        goto free_vaccum_private;
    }
    vaccum_set_screen_private(screen, vaccum_priv);

    if (!dixRegisterPrivateKey(&vaccum_pixmap_private_key, PRIVATE_PIXMAP,
                               sizeof(struct vaccum_pixmap_private))) {
        LogMessage(X_WARNING,
                   "vaccum%d: Failed to allocate pixmap private\n",
                   screen->myNum);
        goto free_vaccum_private;
    }

    if (!dixRegisterPrivateKey(&vaccum_gc_private_key, PRIVATE_GC,
                               sizeof (vaccum_gc_private))) {
        LogMessage(X_WARNING,
                   "vaccum%d: Failed to allocate gc private\n",
                   screen->myNum);
        goto free_vaccum_private;
    }

    vaccum_priv->saved_procs.close_screen = screen->CloseScreen;
    screen->CloseScreen = vaccum_close_screen;

    vaccum_priv->saved_procs.destroy_pixmap = screen->DestroyPixmap;
    screen->DestroyPixmap = vaccum_destroy_pixmap;
    if (!vaccum_vulkan_init(vaccum_priv, drm_fd))
        goto free_vaccum_private;

    vaccum_priv->saved_procs.create_pixmap = screen->CreatePixmap;
    screen->CreatePixmap = vaccum_create_pixmap;

    vaccum_priv->saved_procs.block_handler = screen->BlockHandler;
    screen->BlockHandler = vaccum_block_handler_wrapper;

    vaccum_priv->max_fbo_size = vaccum_priv->dev_properties.limits.maxFramebufferWidth;
    vaccum_setup_formats(vaccum_priv);

    vaccum_alloc_cmd_buffer(vaccum_priv);

#ifdef DRI3
    if (drm_fd >= 0) {
        vaccum_priv->drm_device_path = drmGetRenderDeviceNameFromFd(drm_fd);
        if (!vaccum_priv->drm_device_path)
            vaccum_priv->drm_device_path = drmGetDeviceNameFromFd2(drm_fd);
        vaccum_enable_dri3(screen);
        if (!vaccum_dri3_screen_init(screen))
            LogMessage(X_WARNING,
                       "vaccum%d: Failed to initialize DRI3\n",
                       screen->myNum);
    }
#endif

    return TRUE;
 free_vaccum_private:
    free(vaccum_priv);
    vaccum_set_screen_private(screen, NULL);
    return FALSE;

}

static void
vaccum_release_screen_priv(ScreenPtr screen)
{
    vaccum_screen_private *vaccum_priv;

    vaccum_priv = vaccum_get_screen_private(screen);

    vaccum_vulkan_fini(vaccum_priv);
    free(vaccum_priv);

    vaccum_set_screen_private(screen, NULL);
}

Bool
vaccum_close_screen(ScreenPtr screen)
{
   vaccum_screen_private *vaccum_priv;

   vaccum_priv = vaccum_get_screen_private(screen);

   screen->CloseScreen = vaccum_priv->saved_procs.close_screen;
       
   vaccum_release_screen_priv(screen);
   return screen->CloseScreen(screen);
}

void
vaccum_fini(ScreenPtr screen)
{
    /* Do nothing currently. */
}

static void
vaccum_block_handler_wrapper(ScreenPtr screen, void *timeout)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);

    screen->BlockHandler = vaccum_priv->saved_procs.block_handler;
    screen->BlockHandler(screen, timeout);
    vaccum_priv->saved_procs.block_handler = screen->BlockHandler;
    screen->BlockHandler = vaccum_block_handler_wrapper;

    vaccum_block_handler(screen);
}

void
vaccum_block_handler(ScreenPtr screen)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    if (vaccum_priv)
        vaccum_flush_cmds(vaccum_priv);
}

void
vaccum_clear_pixmap(PixmapPtr pixmap)
{
    if (pixmap->devPrivate.ptr)
        memset(pixmap->devPrivate.ptr, 0,
               pixmap->devKind * pixmap->drawable.height);
}

void
vaccum_exchange_buffers(PixmapPtr front, PixmapPtr back)
{
    vaccum_pixmap_private *front_priv = vaccum_get_pixmap_private(front);
    vaccum_pixmap_private *back_priv = vaccum_get_pixmap_private(back);

    XORG_EXCHANGE(front_priv->image, back_priv->image)
}

void
vaccum_set_pixmap_type(PixmapPtr pixmap, vaccum_pixmap_type_t type)
{
    vaccum_pixmap_private *pixmap_priv = vaccum_get_pixmap_private(pixmap);
    if (pixmap_priv)
        pixmap_priv->type = type;
}

uint32_t
vaccum_get_pixmap_texture(PixmapPtr pixmap)
{
    vaccum_pixmap_private *pixmap_priv = vaccum_get_pixmap_private(pixmap);
    if (pixmap_priv && pixmap_priv->image)
        return (uint32_t)(uintptr_t)pixmap_priv->image->image;
    return 0;
}

Bool
vaccum_set_pixmap_texture(PixmapPtr pixmap, unsigned int tex)
{
    return FALSE;
}
