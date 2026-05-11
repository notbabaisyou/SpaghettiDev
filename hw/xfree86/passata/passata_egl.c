/**
 * Spaghetti Display Server
 * Copyright (C) 2026  SpaghettiFork
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include "passata_priv.h"

#include <xf86drm.h>
#include <string.h>

/*
 * EGL_NO_CONFIG_KHR may not be defined in older EGL headers.
 */
#ifndef EGL_NO_CONFIG_KHR
#define EGL_NO_CONFIG_KHR ((EGLConfig) 0)
#endif

static Bool
passata_has_extension(const char *list, const char *ext)
{
    const char *p;
    size_t len;

    if (!list || !ext)
        return FALSE;

    len = strlen(ext);
    p   = list;

    while ((p = strstr(p, ext)) != NULL) {
        if ((p == list || p[-1] == ' ') &&
            (p[len] == '\0' || p[len] == ' '))
            return TRUE;
        p += len;
    }
    return FALSE;
}

/*
 * passata_find_egl_device: enumerate EGL devices and return the one
 * whose DRM node matches |fd|.
 *
 * We use drmGetDevice2() to obtain all DRM nodes (primary + render)
 * for the fd, then compare each against EGL_DRM_DEVICE_FILE_EXT so
 * that a card0 fd correctly matches a renderD128 EGL device.
 */
static EGLDeviceEXT
passata_find_egl_device(ScrnInfoPtr scrn, int fd)
{
    EGLDeviceEXT *devices = NULL;
    EGLDeviceEXT  found   = EGL_NO_DEVICE_EXT;
    EGLint        ndevices, i;
    drmDevicePtr  drm_dev = NULL;
    int           node;

    if (drmGetDevice2(fd, 0, &drm_dev) != 0) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: drmGetDevice2 failed\n", PASSATA_LOG_PREFIX);
        return EGL_NO_DEVICE_EXT;
    }

    if (!eglQueryDevicesEXT(0, NULL, &ndevices) || ndevices == 0) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: no EGL devices found\n", PASSATA_LOG_PREFIX);
        goto out_drm;
    }

    devices = xallocarray(ndevices, sizeof(*devices));
    if (!devices)
        goto out_drm;

    if (!eglQueryDevicesEXT(ndevices, devices, &ndevices))
        goto out;

    for (i = 0; i < ndevices && found == EGL_NO_DEVICE_EXT; i++) {
        const char *egl_node =
            eglQueryDeviceStringEXT(devices[i], EGL_DRM_DEVICE_FILE_EXT);

        if (!egl_node)
            continue;

        for (node = 0; node < DRM_NODE_MAX; node++) {
            if (!(drm_dev->available_nodes & (1 << node)))
                continue;
            if (strcmp(drm_dev->nodes[node], egl_node) == 0) {
                found = devices[i];
                break;
            }
        }
    }

    if (found == EGL_NO_DEVICE_EXT)
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: no EGL device matched DRM fd\n", PASSATA_LOG_PREFIX);

out:
    free(devices);
out_drm:
    drmFreeDevice(&drm_dev);
    return found;
}

static Bool
passata_query_capabilities(ScrnInfoPtr scrn)
{
    passata_screen_priv *priv  = passata_get_screen_priv(scrn->pScreen);
    const char          *gl_exts, *egl_exts;

    gl_exts  = (const char *) glGetString(GL_EXTENSIONS);
    egl_exts = eglQueryString(priv->display, EGL_EXTENSIONS);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &priv->max_texture_size);
    if (!priv->max_texture_size) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: invalid GL_MAX_TEXTURE_SIZE value\n", PASSATA_LOG_PREFIX);
        return FALSE;
    }

    priv->has_fbo =
        passata_has_extension(gl_exts, "GL_ARB_framebuffer_object") ||
        passata_has_extension(gl_exts, "GL_EXT_framebuffer_object");

    if (!priv->has_fbo) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: GL_ARB/EXT_framebuffer_object not available\n", PASSATA_LOG_PREFIX);
        return FALSE;
    }

    priv->has_shaders =
        passata_has_extension(gl_exts, "GL_ARB_shader_objects")  &&
        passata_has_extension(gl_exts, "GL_ARB_fragment_shader") &&
        passata_has_extension(gl_exts, "GL_ARB_vertex_shader");

    priv->has_npot =
        passata_has_extension(gl_exts, "GL_ARB_texture_non_power_of_two");

    priv->has_texture_rg =
        passata_has_extension(gl_exts, "GL_ARB_texture_rg") ||
        passata_has_extension(gl_exts, "GL_EXT_texture_rg");

    priv->has_texture_swizzle =
        passata_has_extension(gl_exts, "GL_ARB_texture_swizzle") ||
        passata_has_extension(gl_exts, "GL_EXT_texture_swizzle");

    priv->has_texture_barrier =
        passata_has_extension(gl_exts, "GL_ARB_texture_barrier");

    /* Optional EGL capabilities */
    priv->has_dma_buf_export =
        passata_has_extension(egl_exts, "EGL_MESA_image_dma_buf_export");

    priv->has_dma_buf_modifiers =
        passata_has_extension(egl_exts,
                              "EGL_EXT_image_dma_buf_import_modifiers");

    xf86DrvMsg(scrn->scrnIndex, X_DEBUG,
               "%s(GL): renderer \"%s\"\n",
               PASSATA_LOG_PREFIX,
               glGetString(GL_RENDERER));
    xf86DrvMsg(scrn->scrnIndex, X_DEBUG,
               "%s(GL): max_texture_size=%d shaders=%d npot=%d "
               "swizzle=%d barrier=%d\n",
               PASSATA_LOG_PREFIX,
               priv->max_texture_size, priv->has_shaders, priv->has_npot,
               priv->has_texture_swizzle, priv->has_texture_barrier);
    xf86DrvMsg(scrn->scrnIndex, X_DEBUG,
               "%s(EGL): dma_buf_export=%d dma_buf_modifiers=%d\n",
               PASSATA_LOG_PREFIX,
               priv->has_dma_buf_export, priv->has_dma_buf_modifiers);

    return TRUE;
}
Bool
passata_egl_init(ScrnInfoPtr scrn, int fd)
{
    passata_screen_priv *priv = passata_get_screen_priv(scrn->pScreen);
    const char          *client_exts, *display_exts;
    EGLDeviceEXT         device;
    EGLint               major, minor;
    EGLContext           ctx;

    client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!client_exts) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: failed to query EGL client extensions\n", PASSATA_LOG_PREFIX);
        return FALSE;
    }

    if (!passata_has_extension(client_exts, "EGL_EXT_device_enumeration") ||
        !passata_has_extension(client_exts, "EGL_EXT_device_query")       ||
        !passata_has_extension(client_exts, "EGL_EXT_platform_device")) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: EGL_EXT_platform_device not available\n", PASSATA_LOG_PREFIX);
        return FALSE;
    }

    device = passata_find_egl_device(scrn, fd);
    if (device == EGL_NO_DEVICE_EXT)
        return FALSE;

    priv->display =
        eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, device, NULL);
    if (priv->display == EGL_NO_DISPLAY) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: eglGetPlatformDisplayEXT failed\n", PASSATA_LOG_PREFIX);
        return FALSE;
    }

    if (!eglInitialize(priv->display, &major, &minor)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: eglInitialize failed\n", PASSATA_LOG_PREFIX);
        goto fail_display;
    }

    xf86DrvMsg(scrn->scrnIndex, X_INFO,
               "%s: EGL %d.%d on \"%s\"\n", 
               PASSATA_LOG_PREFIX, major, minor,
               eglQueryString(priv->display, EGL_VENDOR));

    display_exts = eglQueryString(priv->display, EGL_EXTENSIONS);
    if (!passata_has_extension(display_exts, "EGL_KHR_no_config_context") &&
        !passata_has_extension(display_exts, "EGL_MESA_configless_context")) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: no configless context support "
                   "(EGL_KHR_no_config_context)\n", PASSATA_LOG_PREFIX);
        goto fail_display;
    }

    if (!eglBindAPI(EGL_OPENGL_API)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: eglBindAPI(EGL_OPENGL_API) failed\n", PASSATA_LOG_PREFIX);
        goto fail_display;
    }

    /*
     * Create a configless context.  All rendering targets are FBOs so
     * we never need an EGL surface and have no surface format to match.
     */
    ctx = eglCreateContext(priv->display, EGL_NO_CONFIG_KHR,
                           EGL_NO_CONTEXT, NULL);
    if (ctx == EGL_NO_CONTEXT) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: eglCreateContext failed\n", PASSATA_LOG_PREFIX);
        goto fail_display;
    }
    priv->context = ctx;

    /* Make current with no surface — FBOs only from here on */
    if (!eglMakeCurrent(priv->display,
                        EGL_NO_SURFACE, EGL_NO_SURFACE,
                        priv->context)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "%s: eglMakeCurrent failed\n", PASSATA_LOG_PREFIX);
        goto fail_context;
    }

    if (!passata_query_capabilities(scrn))
        goto fail_current;

    return TRUE;

fail_current:
    eglMakeCurrent(priv->display,
                   EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
fail_context:
    eglDestroyContext(priv->display, priv->context);
    priv->context = EGL_NO_CONTEXT;
fail_display:
    eglTerminate(priv->display);
    priv->display = EGL_NO_DISPLAY;
    return FALSE;
}

void
passata_egl_fini(ScrnInfoPtr scrn)
{
    passata_screen_priv *priv = passata_get_screen_priv(scrn->pScreen);

    if (!priv)
        return;

    if (priv->display == EGL_NO_DISPLAY)
        return;

    eglMakeCurrent(priv->display,
                   EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (priv->context != EGL_NO_CONTEXT) {
        eglDestroyContext(priv->display, priv->context);
        priv->context = EGL_NO_CONTEXT;
    }

    eglTerminate(priv->display);
    priv->display = EGL_NO_DISPLAY;
}