/*
 * Copyright © 2007 Red Hat, Inc.
 * Copyright © 2019 NVIDIA CORPORATION
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Dave Airlie <airlied@redhat.com>
 *    Aaron Plattner <aplattner@nvidia.com>
 *
 */

#include "dix-config.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "dumb_bo.h"
#include "inputstr.h"
#include "xf86str.h"
#include "X11/Xatom.h"
#include "mi.h"
#include "micmap.h"
#include "xf86cmap.h"
#include "xf86DDC.h"
#include <drm_fourcc.h>
#include <drm_mode.h>

#include <xf86drm.h>
#include "xf86Crtc.h"
#include "drmmode_display.h"

#ifdef PRESENT
#include "present.h"
#endif

#include <cursorstr.h>

#include <X11/extensions/dpmsconst.h>

#include "driver.h"

#if X_BYTE_ORDER == X_BIG_ENDIAN
#define cpu_to_le32(x) bswap_32(x)
#else
#define cpu_to_le32(x) (x)
#endif

static Bool drmmode_xf86crtc_resize(ScrnInfoPtr scrn, int width, int height);
static PixmapPtr drmmode_create_pixmap_header(ScreenPtr pScreen, int width, int height,
                                              int depth, int bitsPerPixel, int devKind,
                                              void *pPixData);
static void drmmode_tearfree_realloc_crtc(xf86CrtcPtr crtc);
static void drmmode_FiniSharedPixmapFlipping(xf86CrtcPtr crtc, drmmode_ptr drmmode);

static const struct drm_color_ctm ctm_identity = { {
    1ULL << 32, 0, 0,
    0, 1ULL << 32, 0,
    0, 0, 1ULL << 32
} };

static Bool ctm_is_identity(const struct drm_color_ctm *ctm)
{
    const size_t matrix_len = sizeof(ctm->matrix) / sizeof(ctm->matrix[0]);
    const uint64_t one = 1ULL << 32;
    const uint64_t neg_zero = 1ULL << 63;
    int i;

    for (i = 0; i < matrix_len; i++) {
        const Bool diagonal = i / 3 == i % 3;
        const uint64_t val = ctm->matrix[i];

        if ((diagonal && val != one) ||
            (!diagonal && val != 0 && val != neg_zero)) {
            return FALSE;
        }
    }

    return TRUE;
}

static inline uint32_t *
formats_ptr(struct drm_format_modifier_blob *blob)
{
    return (uint32_t *)(((char *)blob) + blob->formats_offset);
}

static inline struct drm_format_modifier *
modifiers_ptr(struct drm_format_modifier_blob *blob)
{
    return (struct drm_format_modifier *)(((char *)blob) + blob->modifiers_offset);
}

static inline int
get_maximum_cursor_size(drmmode_cursor_rec cursor, Bool is_height)
{
    return is_height ? cursor.dimensions[cursor.num_dimensions - 1].height
                     : cursor.dimensions[cursor.num_dimensions - 1].width;
}

static uint32_t
get_opaque_format(uint32_t format)
{
    switch (format) {
    case DRM_FORMAT_ARGB8888:
        return DRM_FORMAT_XRGB8888;
    case DRM_FORMAT_ARGB2101010:
        return DRM_FORMAT_XRGB2101010;
    default:
        return format;
    }
}

static drmmode_format_ptr get_format(drmmode_crtc_private_ptr drmmode_crtc,
                                     Bool async_flip, int i)
{
    if (async_flip && drmmode_crtc->formats_async)
        return &drmmode_crtc->formats_async[i];
    else
        return &drmmode_crtc->formats[i];
}

static inline Bool
drmmode_is_implicit_modifiers(drmmode_format_ptr iter, uint64_t modifier)
{
    return !iter->num_modifiers || modifier == DRM_FORMAT_MOD_INVALID;
}

Bool
drmmode_is_format_supported(ScrnInfoPtr scrn, uint32_t format,
                            uint64_t modifier, Bool async_flip)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
    int c, i, j;

    /* BO are imported as opaque surface, so let's pretend there is no alpha */
    format = get_opaque_format(format);

    for (c = 0; c < xf86_config->num_crtc; c++) {
        xf86CrtcPtr crtc = xf86_config->crtc[c];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
        Bool found = FALSE;

        if (!crtc->enabled)
            continue;

        if (drmmode_crtc->num_formats == 0)
            continue;

        for (i = 0; i < drmmode_crtc->num_formats; i++) {
            drmmode_format_ptr iter = get_format(drmmode_crtc, async_flip, i);

            if (iter->format != format)
                continue;

            if (drmmode_is_implicit_modifiers(iter, modifier)) {
                found = TRUE;
                break;
            }

            for (j = 0; j < iter->num_modifiers; j++) {
                if (iter->modifiers[j] == modifier) {
                    found = TRUE;
                    break;
                }
            }

            break;
        }

        if (!found)
            return FALSE;
    }

    return TRUE;
}

#if defined(GBM_BO_WITH_MODIFIERS) || defined(FISSION_SOFT2D)
static uint32_t
get_modifiers_set(ScrnInfoPtr scrn, uint32_t format, uint64_t **modifiers,
                  Bool enabled_crtc_only, Bool exclude_multiplane, Bool async_flip)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
    modesettingPtr ms = modesettingPTR(scrn);
    drmmode_ptr drmmode = &ms->drmmode;
    int c, i, j, k, count_modifiers = 0;
    uint64_t *tmp, *ret = NULL;

    /* BOs are imported as opaque surfaces, so pretend the same thing here */
    format = get_opaque_format(format);

    *modifiers = NULL;
    for (c = 0; c < xf86_config->num_crtc; c++) {
        xf86CrtcPtr crtc = xf86_config->crtc[c];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

        if (enabled_crtc_only && !crtc->enabled)
            continue;

        for (i = 0; i < drmmode_crtc->num_formats; i++) {
            drmmode_format_ptr iter = get_format(drmmode_crtc, async_flip, i);

            if (iter->format != format)
                continue;

            for (j = 0; j < iter->num_modifiers; j++) {
                Bool found = FALSE;

                /* Don't choose multi-plane formats for our screen pixmap.
                 * These will get used with frontbuffer rendering, which will
                 * lead to worse-than-tearing with multi-plane formats, as the
                 * primary and auxiliary planes go out of sync. */
                if (exclude_multiplane &&
                    gbm_device_get_format_modifier_plane_count(drmmode->gbm,
                                                               format,
                                                               iter->modifiers[j]) > 1) {
                    continue;
                }

                for (k = 0; k < count_modifiers; k++) {
                    if (iter->modifiers[j] == ret[k])
                        found = TRUE;
                }
                if (!found) {
                    count_modifiers++;
                    tmp = realloc(ret, count_modifiers * sizeof(uint64_t));
                    if (!tmp) {
                        free(ret);
                        return 0;
                    }
                    ret = tmp;
                    ret[count_modifiers - 1] = iter->modifiers[j];
                }
            }
        }
    }

    *modifiers = ret;
    return count_modifiers;
}

static Bool
get_drawable_modifiers(DrawablePtr draw, uint32_t format,
                       uint32_t *num_modifiers, uint64_t **modifiers)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(draw->pScreen);
    modesettingPtr ms = modesettingPTR(scrn);
    Bool async_flip;

    if (!ms->drmmode.pageflip || ms->drmmode.dri2_flipping || !scrn->vtSema
#ifdef PRESENT
    || !present_can_window_flip((WindowPtr) draw)
#endif
        ) {
        *num_modifiers = 0;
        *modifiers = NULL;
        return TRUE;
    }

    async_flip = ms_window_has_async_flip((WindowPtr)draw);
    ms_window_update_async_flip_modifiers((WindowPtr)draw, async_flip);

    *num_modifiers = get_modifiers_set(scrn, format, modifiers,
                                       TRUE, !ms->drmmode.multiplanar, async_flip);
    return TRUE;
}

static inline uint32_t
drmmode_alpha_twin_format_get(uint32_t format)
{
    switch (format) {
    case DRM_FORMAT_XRGB8888:
        return DRM_FORMAT_ARGB8888;
    case DRM_FORMAT_ARGB8888:
        return DRM_FORMAT_XRGB8888;
    case DRM_FORMAT_XBGR8888:
        return DRM_FORMAT_ABGR8888;
    case DRM_FORMAT_ABGR8888:
        return DRM_FORMAT_XBGR8888;
    case DRM_FORMAT_XRGB2101010:
        return DRM_FORMAT_ARGB2101010;
    case DRM_FORMAT_ARGB2101010:
        return DRM_FORMAT_XRGB2101010;
    case DRM_FORMAT_XBGR2101010:
        return DRM_FORMAT_ABGR2101010;
    case DRM_FORMAT_ABGR2101010:
        return DRM_FORMAT_XBGR2101010;
    default:
        return 0;
    }
}

static Bool
drmmode_soft2d_get_formats(ScreenPtr screen, CARD32 *num, CARD32 **formats)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    xf86CrtcConfigPtr xcfg = XF86_CRTC_CONFIG_PTR(scrn);
    int c = 0, i = 0, j = 0, n = 0;

    *num = 0;
    *formats = NULL;

    for (; c < xcfg->num_crtc; c++) {
        xf86CrtcPtr crtc = xcfg->crtc[c];
        drmmode_crtc_private_ptr dcrtc = crtc->driver_private;

        if (!crtc->enabled)
            continue;

        if (dcrtc->num_formats == 0)
            continue;

        *formats = calloc(dcrtc->num_formats * 2, sizeof(CARD32));
        if (!*formats)
            return FALSE;

        for (i = 0; i < dcrtc->num_formats; i++) {
            uint32_t f = dcrtc->formats[i].format;

            for (j = 0; j < n; j++) {
                if ((*formats)[j] == f)
                    break;
            }
            if (j == n)
                (*formats)[n++] = f;

            f = drmmode_alpha_twin_format_get(f);
            if (f) {
                for (j = 0; j < n; j++) {
                    if ((*formats)[j] == f)
                        break;
                }
                if (j == n)
                    (*formats)[n++] = f;
            }
        }

        *num = n;
        break;
    }

    return TRUE;
}

static Bool
drmmode_soft2d_get_modifiers(ScreenPtr screen, uint32_t format,
                             uint32_t *num, uint64_t **modifiers)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    xf86CrtcConfigPtr xcfg = XF86_CRTC_CONFIG_PTR(scrn);
    drmmode_format_ptr dformat = NULL;
    int c = 0, i = 0;

    *num = 0;
    *modifiers = NULL;

    format = get_opaque_format(format);

    for (; c < xcfg->num_crtc; c++) {
        xf86CrtcPtr crtc = xcfg->crtc[c];
        drmmode_crtc_private_ptr dcrtc = crtc->driver_private;

        if (!crtc->enabled)
            continue;

        if (dcrtc->num_formats == 0)
            continue;

        for (i = 0; i < dcrtc->num_formats; i++) {
            if (dcrtc->formats[i].format == format) {
                dformat = &dcrtc->formats[i];

                free(*modifiers);
                *modifiers = calloc(dformat->num_modifiers, sizeof(uint64_t));
                if (!*modifiers)
                    return FALSE;
                memcpy(*modifiers, dformat->modifiers,
                       dformat->num_modifiers * sizeof(uint64_t));
                *num = dformat->num_modifiers;
                break;
            }
        }
    }

    return TRUE;
}

static Bool
drmmode_soft2d_get_drawable_modifiers(DrawablePtr draw, uint32_t format,
                                      uint32_t *num, uint64_t **modifiers)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(draw->pScreen);
    xf86CrtcConfigPtr xcfg = XF86_CRTC_CONFIG_PTR(scrn);
    drmmode_format_ptr dformat = NULL;
    int c = 0, i = 0, j = 0;

    *num = 0;
    *modifiers = NULL;

    format = get_opaque_format(format);

    for (; c < xcfg->num_crtc; c++) {
        xf86CrtcPtr crtc = xcfg->crtc[c];
        drmmode_crtc_private_ptr dcrtc = crtc->driver_private;

        if (!crtc->enabled)
            continue;

        if (dcrtc->num_formats == 0)
            continue;

        for (i = 0; i < dcrtc->num_formats; i++) {
            Bool has_linear = FALSE;
            for (j = 0; j < dcrtc->formats[i].num_modifiers; j++) {
                if (dcrtc->formats[i].modifiers[j] == DRM_FORMAT_MOD_LINEAR) {
                    has_linear = TRUE;
                    break;
                }
            }
            if ((dcrtc->formats[i].format == format) && has_linear) {
                dformat = &dcrtc->formats[i];
                for (j = 0; j < dformat->num_modifiers; j++) {
                    if (dformat->modifiers[j] == DRM_FORMAT_MOD_LINEAR) {
                        free(*modifiers);
                        *modifiers = calloc(1, sizeof(uint64_t));
                        if (!*modifiers)
                            return FALSE;
                        **modifiers = dformat->modifiers[j];
                        *num = 1;
                        return TRUE;
                    }
                }
            }
        }
    }

    return TRUE;
}
#endif

static Bool
drmmode_zaphod_string_matches(ScrnInfoPtr scrn, const char *s, char *output_name)
{
    char **token = xstrtokenize(s, ", \t\n\r");
    Bool ret = FALSE;

    if (!token)
        return FALSE;

    for (int i = 0; token[i]; i++) {
        if (strcmp(token[i], output_name) == 0)
            ret = TRUE;

        free(token[i]);
    }

    free(token);

    return ret;
}

static uint64_t
drmmode_prop_get_value(drmmode_prop_info_ptr info,
                       drmModeObjectPropertiesPtr props,
                       uint64_t def)
{
    unsigned int i;

    if (info->prop_id == 0)
        return def;

    for (i = 0; i < props->count_props; i++) {
        unsigned int j;

        if (props->props[i] != info->prop_id)
            continue;

        /* Simple (non-enum) types can return the value directly */
        if (info->num_enum_values == 0)
            return props->prop_values[i];

        /* Map from raw value to enum value */
        for (j = 0; j < info->num_enum_values; j++) {
            if (!info->enum_values[j].valid)
                continue;
            if (info->enum_values[j].value != props->prop_values[i])
                continue;

            return j;
        }
    }

    return def;
}

static uint32_t
drmmode_prop_info_update(drmmode_ptr drmmode,
                         drmmode_prop_info_ptr info,
                         unsigned int num_infos,
                         drmModeObjectProperties *props)
{
    drmModePropertyRes *prop;
    uint32_t valid_mask = 0;
    unsigned i, j;

    assert(num_infos <= 32 && "update return type");

    for (i = 0; i < props->count_props; i++) {
        Bool props_incomplete = FALSE;
        unsigned int k;

        for (j = 0; j < num_infos; j++) {
            if (info[j].prop_id == props->props[i])
                break;
            if (!info[j].prop_id)
                props_incomplete = TRUE;
        }

        /* We've already discovered this property. */
        if (j != num_infos)
            continue;

        /* We haven't found this property ID, but as we've already
         * found all known properties, we don't need to look any
         * further. */
        if (!props_incomplete)
            break;

        prop = drmModeGetProperty(drmmode->fd, props->props[i]);
        if (!prop)
            continue;

        for (j = 0; j < num_infos; j++) {
            if (!strcmp(prop->name, info[j].name))
                break;
        }

        /* We don't know/care about this property. */
        if (j == num_infos) {
            drmModeFreeProperty(prop);
            continue;
        }

        info[j].prop_id = props->props[i];
        info[j].value = props->prop_values[i];
        valid_mask |= 1U << j;

        if (info[j].num_enum_values == 0) {
            drmModeFreeProperty(prop);
            continue;
        }

        if (!(prop->flags & DRM_MODE_PROP_ENUM)) {
            xf86DrvMsg(drmmode->scrn->scrnIndex, X_WARNING,
                       "expected property %s to be an enum,"
                       " but it is not; ignoring\n", prop->name);
            drmModeFreeProperty(prop);
            continue;
        }

        for (k = 0; k < info[j].num_enum_values; k++) {
            int l;

            if (info[j].enum_values[k].valid)
                continue;

            for (l = 0; l < prop->count_enums; l++) {
                if (!strcmp(prop->enums[l].name,
                            info[j].enum_values[k].name))
                    break;
            }

            if (l == prop->count_enums)
                continue;

            info[j].enum_values[k].valid = TRUE;
            info[j].enum_values[k].value = prop->enums[l].value;
        }

        drmModeFreeProperty(prop);
    }

    return valid_mask;
}

static Bool
drmmode_prop_info_copy(drmmode_prop_info_ptr dst,
		       const drmmode_prop_info_rec *src,
		       unsigned int num_props,
		       Bool copy_prop_id)
{
    unsigned int i;

    memcpy(dst, src, num_props * sizeof(*dst));

    for (i = 0; i < num_props; i++) {
        unsigned int j;

        if (copy_prop_id)
            dst[i].prop_id = src[i].prop_id;
        else
            dst[i].prop_id = 0;

        if (src[i].num_enum_values == 0)
            continue;

        dst[i].enum_values =
            malloc(src[i].num_enum_values *
                    sizeof(*dst[i].enum_values));
        if (!dst[i].enum_values)
            goto err;

        memcpy(dst[i].enum_values, src[i].enum_values,
                src[i].num_enum_values * sizeof(*dst[i].enum_values));

        for (j = 0; j < dst[i].num_enum_values; j++)
            dst[i].enum_values[j].valid = FALSE;
    }

    return TRUE;

err:
    while (i--)
        free(dst[i].enum_values);
    return FALSE;
}

static void
drmmode_prop_info_free(drmmode_prop_info_ptr info, int num_props)
{
    int i;

    for (i = 0; i < num_props; i++)
        free(info[i].enum_values);
}

static void
drmmode_ConvertToKMode(ScrnInfoPtr scrn,
                       drmModeModeInfo * kmode, DisplayModePtr mode);


static int
plane_add_prop(drmModeAtomicReq *req, drmmode_crtc_private_ptr drmmode_crtc,
               enum drmmode_plane_property prop, uint64_t val)
{
    drmmode_prop_info_ptr info = &drmmode_crtc->props_plane[prop];
    int ret;

    if (!info)
        return -1;

    ret = drmModeAtomicAddProperty(req, drmmode_crtc->plane_id,
                                   info->prop_id, val);
    return (ret <= 0) ? -1 : 0;
}

static int
plane_add_props(drmModeAtomicReq *req, xf86CrtcPtr crtc,
                uint32_t fb_id, int x, int y)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    int ret = 0;

    ret |= plane_add_prop(req, drmmode_crtc, DRMMODE_PLANE_FB_ID,
                          fb_id);
    ret |= plane_add_prop(req, drmmode_crtc, DRMMODE_PLANE_CRTC_ID,
                          fb_id ? drmmode_crtc->mode_crtc->crtc_id : 0);
    if (!fb_id)
        return ret;

    ret |= plane_add_prop(req, drmmode_crtc, DRMMODE_PLANE_SRC_X, x << 16);
    ret |= plane_add_prop(req, drmmode_crtc, DRMMODE_PLANE_SRC_Y, y << 16);
    ret |= plane_add_prop(req, drmmode_crtc, DRMMODE_PLANE_SRC_W,
                          crtc->mode.HDisplay << 16);
    ret |= plane_add_prop(req, drmmode_crtc, DRMMODE_PLANE_SRC_H,
                          crtc->mode.VDisplay << 16);
    ret |= plane_add_prop(req, drmmode_crtc, DRMMODE_PLANE_CRTC_X, 0);
    ret |= plane_add_prop(req, drmmode_crtc, DRMMODE_PLANE_CRTC_Y, 0);
    ret |= plane_add_prop(req, drmmode_crtc, DRMMODE_PLANE_CRTC_W,
                          crtc->mode.HDisplay);
    ret |= plane_add_prop(req, drmmode_crtc, DRMMODE_PLANE_CRTC_H,
                          crtc->mode.VDisplay);

    return ret;
}

static int
crtc_add_prop(drmModeAtomicReq *req, drmmode_crtc_private_ptr drmmode_crtc,
              enum drmmode_crtc_property prop, uint64_t val)
{
    drmmode_prop_info_ptr info = &drmmode_crtc->props[prop];
    int ret;

    if (!info)
        return -1;

    ret = drmModeAtomicAddProperty(req, drmmode_crtc->mode_crtc->crtc_id,
                                   info->prop_id, val);
    return (ret <= 0) ? -1 : 0;
}

static int
connector_add_prop(drmModeAtomicReq *req, drmmode_output_private_ptr drmmode_output,
                   enum drmmode_connector_property prop, uint64_t val)
{
    drmmode_prop_info_ptr info = &drmmode_output->props_connector[prop];
    int ret;

    if (!info)
        return -1;

    ret = drmModeAtomicAddProperty(req, drmmode_output->output_id,
                                   info->prop_id, val);
    return (ret <= 0) ? -1 : 0;
}

static inline int
crtc_add_vrr_prop(drmModeAtomicReq *req,
                  drmmode_crtc_private_ptr drmmode_crtc,
                  Bool enabled)
{
    if (!drmmode_crtc->props[DRMMODE_CRTC_VRR_ENABLED].prop_id)
        return -1;
    else
        return crtc_add_prop(req, drmmode_crtc, DRMMODE_CRTC_VRR_ENABLED, enabled);
}

static int
drmmode_CompareKModes(const drmModeModeInfo * kmode, const drmModeModeInfo * other)
{
    return memcmp(kmode, other, sizeof(*kmode));
}

static int
drm_mode_ensure_blob(xf86CrtcPtr crtc, const drmModeModeInfo* mode_info)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_mode_ptr mode;
    int ret;

    if (drmmode_crtc->current_mode &&
        drmmode_CompareKModes(&drmmode_crtc->current_mode->mode_info, mode_info) == 0)
        return 0;

    mode = calloc(1, sizeof(drmmode_mode_rec));
    if (!mode)
        return -ENOMEM;

    mode->mode_info = *mode_info;
    ret = drmModeCreatePropertyBlob(ms->fd,
                                    &mode->mode_info,
                                    sizeof(mode->mode_info),
                                    &mode->blob_id);
    if (ret) {
        free(mode);
        return ret;
    }

    drmmode_crtc->current_mode = mode;
    xorg_list_add(&mode->entry, &drmmode_crtc->mode_list);

    return 0;
}

static int
crtc_add_mode_props(drmModeAtomicReq *req, xf86CrtcPtr crtc, Bool active)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    uint32_t blob_id = 0;
    int ret;

    if (active) {
        drmModeModeInfo kmode;

        drmmode_ConvertToKMode(crtc->scrn, &kmode, &crtc->mode);
        ret = drm_mode_ensure_blob(crtc, &kmode);
        if (ret)
            return ret;
        blob_id = drmmode_crtc->current_mode->blob_id;
    }

    ret = crtc_add_prop(req, drmmode_crtc, DRMMODE_CRTC_ACTIVE, active);
    if (ret)
        return ret;
    return crtc_add_prop(req, drmmode_crtc, DRMMODE_CRTC_MODE_ID, blob_id);
}

static int
crtc_add_dpms_props(drmModeAtomicReq *req, xf86CrtcPtr crtc,
                    int new_dpms, Bool *active)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    Bool crtc_active = FALSE;
    int i;
    int ret = 0;

    for (i = 0; i < xf86_config->num_output; i++) {
        xf86OutputPtr output = xf86_config->output[i];
        drmmode_output_private_ptr drmmode_output = output->driver_private;

        if (output->crtc != crtc) {
            if (drmmode_output->current_crtc == crtc) {
                ret |= connector_add_prop(req, drmmode_output,
                                          DRMMODE_CONNECTOR_CRTC_ID, 0);
            }
            continue;
        }

        if (drmmode_output->output_id == -1)
            continue;

        if (new_dpms == DPMSModeOn)
            crtc_active = TRUE;

        ret |= connector_add_prop(req, drmmode_output,
                                  DRMMODE_CONNECTOR_CRTC_ID,
                                  crtc_active ?
                                  drmmode_crtc->mode_crtc->crtc_id : 0);
    }

    if (!ret)
        ret = crtc_add_mode_props(req, crtc, crtc_active);

    if (active)
        *active = crtc_active;

    return ret;
}

static void
drm_mode_destroy(xf86CrtcPtr crtc, drmmode_mode_ptr mode)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    if (mode->blob_id)
        drmModeDestroyPropertyBlob(ms->fd, mode->blob_id);
    xorg_list_del(&mode->entry);
    free(mode);
}

Bool
drmmode_crtc_get_fb_id(xf86CrtcPtr crtc, uint32_t *fb_id, int *x, int *y)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;

    *fb_id = 0;

    if (drmmode_crtc->prime_pixmap) {
        if (!drmmode->reverse_prime_offload_mode) {
            msPixmapPrivPtr ppriv =
                msGetPixmapPriv(drmmode, drmmode_crtc->prime_pixmap);
            *fb_id = ppriv->fb_id;
            *x = 0;
        } else {
            *fb_id = drmmode->fb_id;
            *x = drmmode_crtc->prime_pixmap_x;
        }
        *y = 0;
    }
    else if (drmmode_crtc->rotate_fb_id) {
        *fb_id = drmmode_crtc->rotate_fb_id;
        *x = *y = 0;
    }
    else {
        *fb_id = drmmode->fb_id;
        *x = crtc->x;
        *y = crtc->y;
    }

    return TRUE;
}

void
drmmode_set_dpms(ScrnInfoPtr scrn, int dpms, int flags)
{
    modesettingPtr ms = modesettingPTR(scrn);
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    uint32_t mode_flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    int ret = 0;
    int i;

    if (!req)
        return;

    for (i = 0; i < xf86_config->num_output; i++) {
        xf86OutputPtr output = xf86_config->output[i];
        drmmode_output_private_ptr drmmode_output = output->driver_private;

        if (output->crtc != NULL)
            continue;

        ret = connector_add_prop(req, drmmode_output,
                                 DRMMODE_CONNECTOR_CRTC_ID, 0);
    }

    for (i = 0; i < xf86_config->num_crtc; i++) {
        xf86CrtcPtr crtc = xf86_config->crtc[i];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
        Bool active = FALSE;

        ret |= crtc_add_dpms_props(req, crtc, dpms, &active);

        if (dpms == DPMSModeOn && active && drmmode_crtc->need_modeset) {
            uint32_t fb_id;
            int x, y;

            if (!drmmode_crtc_get_fb_id(crtc, &fb_id, &x, &y))
                continue;
            ret |= plane_add_props(req, crtc, fb_id, x, y);
            drmmode_crtc->need_modeset = FALSE;
        }
    }

    if (ret == 0)
        drmModeAtomicCommit(ms->fd, req, mode_flags, NULL);
    drmModeAtomicFree(req);

    ms->pending_modeset = TRUE;
    xf86DPMSSet(scrn, dpms, flags);
    ms->pending_modeset = FALSE;
}

static int
drmmode_output_dpms_atomic(xf86OutputPtr output, int mode)
{
    modesettingPtr ms = modesettingPTR(output->scrn);
    drmmode_output_private_ptr drmmode_output = output->driver_private;
    xf86CrtcPtr crtc = drmmode_output->current_crtc;
    drmModeAtomicReq *req;
    int ret = 0;

    if (mode == DPMSModeOn && !crtc)
        return 1;

    req = drmModeAtomicAlloc();
    if (!req)
        return 1;

    if (crtc)
        ret |= crtc_add_dpms_props(req, crtc, mode, NULL);

    if (mode == DPMSModeOn) {
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

        ret |= connector_add_prop(req, drmmode_output,
                                  DRMMODE_CONNECTOR_CRTC_ID,
                                  drmmode_crtc->mode_crtc->crtc_id);
    } else {
        ret |= connector_add_prop(req, drmmode_output,
                                  DRMMODE_CONNECTOR_CRTC_ID, 0);
    }

    if (ret == 0)
        ret = drmModeAtomicCommit(ms->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

    drmModeAtomicFree(req);
    return ret;
}

static int
drmmode_crtc_disable(xf86CrtcPtr crtc)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    int ret = 0;

    if (!req)
        return 1;

    ret |= crtc_add_prop(req, drmmode_crtc,
                         DRMMODE_CRTC_ACTIVE, 0);
    ret |= crtc_add_prop(req, drmmode_crtc,
                         DRMMODE_CRTC_MODE_ID, 0);

    if (ret == 0)
        ret = drmModeAtomicCommit(ms->fd, req, flags, NULL);

    drmModeAtomicFree(req);
    return ret;
}

static int
drmmode_commit_color(drmmode_ptr drmmode, drmModeAtomicReq *req)
{
    int ret = drmModeAtomicCommit(drmmode->fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);

    /* A pending page flip can reject a nonblocking color update. */
    if (ret == -EBUSY)
        ret = drmModeAtomicCommit(drmmode->fd, req, 0, NULL);
    return ret;
}

static int
crtc_add_ctm_prop(drmModeAtomicReq *req, drmmode_crtc_private_ptr drmmode_crtc,
                  const struct drm_color_ctm *ctm, uint32_t *blob_id)
{
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    int ret;

    if (ctm && drmmode_crtc->use_gamma_lut && !ctm_is_identity(ctm)) {
        ret = drmModeCreatePropertyBlob(drmmode->fd, ctm, sizeof(*ctm), blob_id);
        if (ret)
            return ret;
    }

    return crtc_add_prop(req, drmmode_crtc, DRMMODE_CRTC_CTM, *blob_id);
}

static int
crtc_add_gamma_lut_prop(drmModeAtomicReq *req,
                        drmmode_crtc_private_ptr drmmode_crtc,
                        const uint16_t *red, const uint16_t *green,
                        const uint16_t *blue, int size, uint32_t *blob_id)
{
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    struct drm_color_lut *lut;
    size_t lut_size;
    int ret;

    if (size <= 0 || !red || !green || !blue ||
        size != drmmode_crtc->props[DRMMODE_CRTC_GAMMA_LUT_SIZE].value)
        return -EINVAL;

    lut_size = sizeof(*lut) * size;
    lut = calloc(size, sizeof(*lut));
    if (!lut)
        return -ENOMEM;

    for (int i = 0; i < size; i++) {
        lut[i].red = red[i];
        lut[i].green = green[i];
        lut[i].blue = blue[i];
    }

    ret = drmModeCreatePropertyBlob(drmmode->fd, lut, lut_size, blob_id);
    free(lut);
    if (ret)
        return ret;

    return crtc_add_prop(req, drmmode_crtc, DRMMODE_CRTC_GAMMA_LUT, *blob_id);
}

static void
drmmode_set_ctm(xf86CrtcPtr crtc, const struct drm_color_ctm *ctm)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    uint32_t blob_id = 0;
    drmModeAtomicReq *req;
    int ret;

    if (!drmmode_crtc->props[DRMMODE_CRTC_CTM].prop_id)
        return;

    req = drmModeAtomicAlloc();
    if (!req)
        return;

    ret = crtc_add_ctm_prop(req, drmmode_crtc, ctm, &blob_id);
    if (!ret)
        ret = drmmode_commit_color(drmmode, req);
    if (ret)
        xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
                   "Failed to apply CTM property: %d\n", ret);

    if (blob_id)
        drmModeDestroyPropertyBlob(drmmode->fd, blob_id);
    drmModeAtomicFree(req);
}

/* Leases may not have RandR objects yet during screen initialization. */
static Bool
drmmode_crtc_is_leased(xf86CrtcPtr crtc)
{
    return crtc && crtc->randr_crtc && RRCrtcIsLeased(crtc->randr_crtc);
}

static Bool
drmmode_output_available(xf86OutputPtr output)
{
    drmmode_output_private_ptr drmmode_output = output->driver_private;

    return drmmode_output->output_id != -1 && drmmode_output->mode_output &&
        (!output->randr_output || !RROutputIsLeased(output->randr_output));
}

static xf86OutputPtr
drmmode_target_output(ScrnInfoPtr scrn, xf86CrtcPtr crtc)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    xf86OutputPtr compat = xf86CompatOutput(scrn);

    if (compat && compat->crtc == crtc && drmmode_output_available(compat))
        return compat;

    for (int i = 0; i < config->num_output; i++) {
        xf86OutputPtr output = config->output[i];

        if (output->crtc == crtc && drmmode_output_available(output))
            return output;
    }

    return NULL;
}

struct drmmode_atomic_crtc {
    Bool update;
    xf86OutputPtr output;
    uint32_t gamma_blob;
    uint32_t ctm_blob;
};

struct drmmode_atomic_output {
    Bool update;
    xf86CrtcPtr crtc;
};

static int
drmmode_set_mode_atomic(ScrnInfoPtr scrn, xf86CrtcPtr target, Bool test_only)
{
    modesettingPtr ms = modesettingPTR(scrn);
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    struct drmmode_atomic_crtc *crtcs;
    struct drmmode_atomic_output *outputs;
    drmModeAtomicReq *req;
    const uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    int ret = -ENOMEM;

    crtcs = calloc(config->num_crtc, sizeof(*crtcs));
    outputs = calloc(config->num_output, sizeof(*outputs));
    req = drmModeAtomicAlloc();
    if (!req || (config->num_crtc && !crtcs) ||
        (config->num_output && !outputs))
        goto done;

    ret = 0;
    for (int i = 0; i < config->num_output; i++) {
        xf86OutputPtr output = config->output[i];
        drmmode_output_private_ptr drmmode_output = output->driver_private;
        xf86CrtcPtr crtc = output->crtc;
        xf86CrtcPtr current = drmmode_output->current_crtc;
        uint32_t crtc_id = 0;

        if (!drmmode_output_available(output) ||
            drmmode_crtc_is_leased(crtc) || drmmode_crtc_is_leased(current))
            continue;

        if (target && crtc != target && current != target)
            continue;

        if (crtc && crtc->enabled && (!target || crtc == target)) {
            drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

            crtc_id = drmmode_crtc->mode_crtc->crtc_id;
            outputs[i].crtc = crtc;
        }
        outputs[i].update = TRUE;

        ret = connector_add_prop(req, drmmode_output,
                                  DRMMODE_CONNECTOR_CRTC_ID, crtc_id);
        if (ret)
            goto done;
    }

    for (int i = 0; i < config->num_crtc; i++) {
        xf86CrtcPtr crtc = config->crtc[i];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
        struct drmmode_atomic_crtc *state = &crtcs[i];
        Bool lost_output = FALSE;
        Bool kept_output = FALSE;
        Bool cloned = FALSE;
        uint32_t fb_id = 0;
        int x = 0, y = 0;

        if (drmmode_crtc_is_leased(crtc))
            continue;

        for (int j = 0; j < config->num_output; j++) {
            xf86OutputPtr output = config->output[j];
            drmmode_output_private_ptr drmmode_output = output->driver_private;

            if (outputs[j].crtc == crtc) {
                if (state->output)
                    cloned = TRUE;
                else
                    state->output = output;
            }
            if (drmmode_output->current_crtc == crtc) {
                if (outputs[j].update && outputs[j].crtc != crtc)
                    lost_output = TRUE;
                else
                    kept_output = TRUE;
            }
        }

        /* A RandR request can also orphan the CRTC losing its last output. */
        if (target && target != crtc && (!lost_output || kept_output))
            continue;

        state->update = TRUE;
        if (state->output &&
            (!drmmode_crtc_get_fb_id(crtc, &fb_id, &x, &y) || !fb_id)) {
            ret = -EINVAL;
            goto done;
        }

        ret = crtc_add_mode_props(req, crtc, state->output != NULL);
        if (ret)
            goto done;

        ret = plane_add_props(req, crtc, fb_id, x, y);
        if (ret)
            goto done;

        if (state->output && drmmode_crtc->use_gamma_lut) {
            ret = crtc_add_gamma_lut_prop(req, drmmode_crtc, crtc->gamma_red,
                                          crtc->gamma_green, crtc->gamma_blue,
                                          crtc->gamma_size, &state->gamma_blob);
            if (ret)
                goto done;
        }

        /* Cloned outputs share the existing hardware color transform. */
        if (state->output && !cloned &&
            drmmode_crtc->props[DRMMODE_CRTC_CTM].prop_id) {
            drmmode_output_private_ptr drmmode_output = state->output->driver_private;

            ret = crtc_add_ctm_prop(req, drmmode_crtc,
                                    drmmode_output->ctm_atom ? &drmmode_output->ctm : NULL,
                                    &state->ctm_blob);
            if (ret)
                goto done;
        }
    }

    if (test_only || target) {
        ret = drmModeAtomicCommit(ms->fd, req, flags | DRM_MODE_ATOMIC_TEST_ONLY, NULL);
        if (ret || test_only)
            goto done;
    }

#ifdef HAVE_GBM
    /* Make pending drawing visible before switching scanout buffers. */
    if (ms->drmmode.glamor)
        glamor_finish(scrn->pScreen);
#endif

    ret = drmModeAtomicCommit(ms->fd, req, flags, NULL);
    if (ret)
        goto done;

    for (int i = 0; i < config->num_output; i++) {
        drmmode_output_private_ptr drmmode_output = config->output[i]->driver_private;

        if (outputs[i].update)
            drmmode_output->current_crtc = outputs[i].crtc;
    }

    /* The DPMS hooks update driver state and shared pixmap flipping here. */
    Bool pending_modeset = ms->pending_modeset;
    ms->pending_modeset = TRUE;
    for (int i = 0; i < config->num_crtc; i++) {
        xf86CrtcPtr crtc = config->crtc[i];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

        if (!crtcs[i].update)
            continue;

        crtc->active = crtcs[i].output != NULL;
        drmmode_crtc->need_modeset = FALSE;
        crtc->funcs->dpms(crtc, crtc->active ? DPMSModeOn : DPMSModeOff);

        if (crtc->active && ms->drmmode.tearfree)
            drmmode_tearfree_realloc_crtc(crtc);
        if (!crtc->active)
            drmmode_FiniSharedPixmapFlipping(crtc, &ms->drmmode);
    }
    for (int i = 0; i < config->num_output; i++) {
        xf86OutputPtr output = config->output[i];

        if (outputs[i].crtc)
            output->funcs->dpms(output, DPMSModeOn);
    }
    ms->pending_modeset = pending_modeset;

    if (scrn->pScreen)
        xf86CrtcSetScreenSubpixelOrder(scrn->pScreen);

 done:
    for (int i = 0; crtcs && i < config->num_crtc; i++) {
        if (crtcs[i].gamma_blob)
            drmModeDestroyPropertyBlob(ms->fd, crtcs[i].gamma_blob);
        if (crtcs[i].ctm_blob)
            drmModeDestroyPropertyBlob(ms->fd, crtcs[i].ctm_blob);
    }
    if (req)
        drmModeAtomicFree(req);
    free(outputs);
    free(crtcs);
    return ret;
}

int
drmmode_crtc_flip(xf86CrtcPtr crtc, uint32_t fb_id, int x, int y,
                  uint32_t flags, void *data)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    int ret;

    if (!req)
        return 1;

    ret = plane_add_props(req, crtc, fb_id, x, y);

    if (ret == 0)
        ret = drmModeAtomicCommit(ms->fd, req, flags, data);
    
    drmModeAtomicFree(req);
    return ret;
}

int
drmmode_bo_destroy(drmmode_ptr drmmode, drmmode_bo *bo)
{
    int ret;

#if defined(HAVE_GBM) || defined(FISSION_SOFT2D)
    if (bo->gbm && drmmode->glamor) {
        gbm_bo_destroy(bo->gbm);
        bo->gbm = NULL;
    }
#endif

    if (bo->dumb) {
        ret = dumb_bo_destroy(drmmode->fd, bo->dumb);
        if (ret == 0)
            bo->dumb = NULL;
    }

    return 0;
}

uint32_t
drmmode_bo_get_pitch(drmmode_bo *bo)
{
#if defined(HAVE_GBM) || defined(FISSION_SOFT2D)
    if (bo->gbm)
        return gbm_bo_get_stride(bo->gbm);
#endif

    return bo->dumb->pitch;
}

static Bool
drmmode_bo_has_bo(drmmode_bo *bo)
{
#if defined(HAVE_GBM) || defined(FISSION_SOFT2D)
    if (bo->gbm)
        return TRUE;
#endif

    return bo->dumb != NULL;
}

uint32_t
drmmode_bo_get_handle(drmmode_bo *bo)
{
#if defined(HAVE_GBM) || defined(FISSION_SOFT2D)
    if (bo->gbm)
        return gbm_bo_get_handle(bo->gbm).u32;
#endif

    return bo->dumb->handle;
}

static void *
drmmode_bo_map(drmmode_ptr drmmode, drmmode_bo *bo)
{
    int ret;

#if defined(HAVE_GBM) || defined(FISSION_SOFT2D)
    if (bo->gbm)
        return NULL;
#endif

    if (bo->dumb->ptr)
        return bo->dumb->ptr;

    ret = dumb_bo_map(drmmode->fd, bo->dumb);
    if (ret)
        return NULL;

    return bo->dumb->ptr;
}

static inline int
drmModeAddDumbFB(drmmode_ptr drmmode, struct dumb_bo *bo, uint32_t *fb_id,
                 uint32_t width, uint32_t height, uint32_t drm_format)
{
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

    handles[0] = bo->handle;
    pitches[0] = bo->pitch;

    return drmModeAddFB2(drmmode->fd, width, height, drm_format,
                         handles, pitches, offsets, fb_id, 0);
}

int
drmmode_bo_import(drmmode_ptr drmmode, drmmode_bo *bo,
                  uint32_t *fb_id)
{
    uint32_t format;

#if defined(GBM_BO_WITH_MODIFIERS) || defined(FISSION_SOFT2D)
    if (bo->gbm && gbm_bo_get_modifier(bo->gbm) != DRM_FORMAT_MOD_INVALID) {
        int num_fds;

        num_fds = gbm_bo_get_plane_count(bo->gbm);
        if (num_fds > 0) {
            int i;
            uint32_t handles[4] = { 0 };
            uint32_t strides[4] = { 0 };
            uint32_t offsets[4] = { 0 };
            uint64_t modifiers[4] = { 0 };

            format = gbm_bo_get_format(bo->gbm);
            format = get_opaque_format(format);
            for (i = 0; i < num_fds; i++) {
                handles[i] = gbm_bo_get_handle_for_plane(bo->gbm, i).u32;
                strides[i] = gbm_bo_get_stride_for_plane(bo->gbm, i);
                offsets[i] = gbm_bo_get_offset(bo->gbm, i);
                /*
                 * Note that this is actually a neat feature. Even if no
                 * framebuffer modifiers were used during the framebuffer
                 * allocation, this may return a !DRM_FORMAT_MOD_LINEAR
                 * modifier if the DRI driver decides to choose one. It's
                 * not recommended to do so, but if it is guaranteed that
                 * the display and GPU both support this modifier, they
                 * can both use this modifier rather than the pitch-linear
                 * modifier as a lowest common denominator.
                 *
                 * DRI drivers are still recommended to allocate buffers in
                 * pitch-linear format if no framebuffer modifiers are used
                 * because applications are assumed to not know about these
                 * modifiers at all if they allocate buffers the old way.
                 * In that case they must be assumed to use drmModeAddFB(),
                 * which doesn't allow specifying any framebuffer modifiers
                 * and therefore DRM_FORMAT_MOD_LINEAR is implied.
                 */
                modifiers[i] = gbm_bo_get_modifier(bo->gbm);
            }

            return drmModeAddFB2WithModifiers(drmmode->fd, bo->width, bo->height,
                                              format, handles, strides,
                                              offsets, modifiers, fb_id,
                                              DRM_MODE_FB_MODIFIERS);
        }
    }
#endif

    /* Guess the format from the BPP and depth. */
    switch (drmmode->kbpp) {
        case 16:
            format = (drmmode->scrn->depth == 15) ?
                      DRM_FORMAT_XRGB1555 : DRM_FORMAT_RGB565;
            break;
        case 32:
            format = (drmmode->scrn->depth == 30) ?
                      DRM_FORMAT_XRGB2101010 : DRM_FORMAT_XRGB8888;
            break;
        default:
            format = DRM_FORMAT_XRGB8888;
            break;
    }

    return drmModeAddDumbFB(drmmode, bo->dumb, fb_id, bo->width, bo->height, format);
}

static Bool
drmmode_create_bo(drmmode_ptr drmmode, drmmode_bo *bo,
                  unsigned width, unsigned height,
                  unsigned bpp, Bool force_dumb)
{
    bo->width = width;
    bo->height = height;

#if defined(HAVE_GBM) || defined(FISSION_SOFT2D)
    if (drmmode->gbm && !force_dumb) {
        uint32_t format;
        uint32_t num_modifiers;
        uint64_t *modifiers = NULL;

        switch (drmmode->scrn->depth) {
        case 15:
            format = GBM_FORMAT_ARGB1555;
            break;
        case 16:
            format = GBM_FORMAT_RGB565;
            break;
        case 30:
            format = GBM_FORMAT_ARGB2101010;
            break;
        default:
            format = GBM_FORMAT_ARGB8888;
            break;
        }

        num_modifiers = get_modifiers_set(drmmode->scrn, format, &modifiers,
                                          FALSE, !drmmode->multiplanar, TRUE);
        if (num_modifiers > 0) {
#ifdef GBM_BO_WITH_MODIFIERS2
            bo->gbm = gbm_bo_create_with_modifiers2(drmmode->gbm, 
                                                    width, height, format,
                                                    modifiers, num_modifiers,
                                                    GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
#else
            /* gbm_bo_create_with_modifiers implies GBM_BO_USE_SCANOUT. */
            bo->gbm = gbm_bo_create_with_modifiers(drmmode->gbm, 
                                                   width, height, format,
                                                   modifiers, num_modifiers);
#endif
            bo->used_modifiers = TRUE;
        }

        if (modifiers)
            free(modifiers);

        if (bo->gbm == NULL) {
            bo->gbm = gbm_bo_create(drmmode->gbm, width, height, format,
                                    GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
            bo->used_modifiers = FALSE;
        }

        return bo->gbm != NULL;
    }
#endif

    bo->dumb = dumb_bo_create(drmmode->fd, width, height, bpp);
    return bo->dumb != NULL;
}

Bool
drmmode_SetSlaveBO(PixmapPtr ppix,
                   drmmode_ptr drmmode, int fd_handle, int pitch, int size)
{
    msPixmapPrivPtr ppriv = msGetPixmapPriv(drmmode, ppix);

    if (fd_handle == -1) {
        dumb_bo_destroy(drmmode->fd, ppriv->backing_bo);
        ppriv->backing_bo = NULL;
        return TRUE;
    }

    ppriv->backing_bo =
        dumb_get_bo_from_fd(drmmode->fd, fd_handle, pitch, size);
    if (!ppriv->backing_bo)
        return FALSE;

    close(fd_handle);
    return TRUE;
}

static Bool
drmmode_SharedPixmapPresent(PixmapPtr ppix, xf86CrtcPtr crtc,
                            drmmode_ptr drmmode)
{
    ScreenPtr primary = crtc->randr_crtc->pScreen->current_primary;

    if (primary->PresentSharedPixmap(ppix)) {
        /* Success, queue flip to back target */
        if (drmmode_SharedPixmapFlip(ppix, crtc, drmmode))
            return TRUE;

        xf86DrvMsg(drmmode->scrn->scrnIndex, X_WARNING,
                   "drmmode_SharedPixmapFlip() failed, trying again next vblank\n");

        return drmmode_SharedPixmapPresentOnVBlank(ppix, crtc, drmmode);
    }

    /* Failed to present, try again on next vblank after damage */
    if (primary->RequestSharedPixmapNotifyDamage) {
        msPixmapPrivPtr ppriv = msGetPixmapPriv(drmmode, ppix);

        /* Set flag first in case we are immediately notified */
        ppriv->wait_for_damage = TRUE;

        if (primary->RequestSharedPixmapNotifyDamage(ppix))
            return TRUE;
        else
            ppriv->wait_for_damage = FALSE;
    }

    /* Damage notification not available, just try again on vblank */
    return drmmode_SharedPixmapPresentOnVBlank(ppix, crtc, drmmode);
}

struct vblank_event_args {
    PixmapPtr frontTarget;
    PixmapPtr backTarget;
    xf86CrtcPtr crtc;
    drmmode_ptr drmmode;
    Bool flip;
};
static void
drmmode_SharedPixmapVBlankEventHandler(uint64_t frame, uint64_t usec,
                                       void *data)
{
    struct vblank_event_args *args = data;

    drmmode_crtc_private_ptr drmmode_crtc = args->crtc->driver_private;

    if (args->flip) {
        /* frontTarget is being displayed, update crtc to reflect */
        drmmode_crtc->prime_pixmap = args->frontTarget;
        drmmode_crtc->prime_pixmap_back = args->backTarget;

        /* Safe to present on backTarget, no longer displayed */
        drmmode_SharedPixmapPresent(args->backTarget, args->crtc, args->drmmode);
    } else {
        /* backTarget is still being displayed, present on frontTarget */
        drmmode_SharedPixmapPresent(args->frontTarget, args->crtc, args->drmmode);
    }

    free(args);
}

static void
drmmode_SharedPixmapVBlankEventAbort(void *data)
{
    struct vblank_event_args *args = data;

    msGetPixmapPriv(args->drmmode, args->frontTarget)->flip_seq = 0;

    free(args);
}

Bool
drmmode_SharedPixmapPresentOnVBlank(PixmapPtr ppix, xf86CrtcPtr crtc,
                                    drmmode_ptr drmmode)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    msPixmapPrivPtr ppriv = msGetPixmapPriv(drmmode, ppix);
    struct vblank_event_args *event_args;

    if (ppix == drmmode_crtc->prime_pixmap)
        return FALSE; /* Already flipped to this pixmap */
    if (ppix != drmmode_crtc->prime_pixmap_back)
        return FALSE; /* Pixmap is not a scanout pixmap for CRTC */

    event_args = calloc(1, sizeof(*event_args));
    if (!event_args)
        return FALSE;

    event_args->frontTarget = ppix;
    event_args->backTarget = drmmode_crtc->prime_pixmap;
    event_args->crtc = crtc;
    event_args->drmmode = drmmode;
    event_args->flip = FALSE;

    ppriv->flip_seq =
        ms_drm_queue_alloc(crtc, event_args,
                           drmmode_SharedPixmapVBlankEventHandler,
                           drmmode_SharedPixmapVBlankEventAbort);

    return ms_queue_vblank(crtc, MS_QUEUE_RELATIVE, 1, NULL, ppriv->flip_seq);
}

Bool
drmmode_SharedPixmapFlip(PixmapPtr frontTarget, xf86CrtcPtr crtc,
                         drmmode_ptr drmmode)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    msPixmapPrivPtr ppriv_front = msGetPixmapPriv(drmmode, frontTarget);

    struct vblank_event_args *event_args;

    event_args = calloc(1, sizeof(*event_args));
    if (!event_args)
        return FALSE;

    event_args->frontTarget = frontTarget;
    event_args->backTarget = drmmode_crtc->prime_pixmap;
    event_args->crtc = crtc;
    event_args->drmmode = drmmode;
    event_args->flip = TRUE;

    ppriv_front->flip_seq =
        ms_drm_queue_alloc(crtc, event_args,
                           drmmode_SharedPixmapVBlankEventHandler,
                           drmmode_SharedPixmapVBlankEventAbort);

    if (drmmode_crtc_flip(crtc, ppriv_front->fb_id, 0, 0,
                          DRM_MODE_PAGE_FLIP_EVENT,
                          (void *)(intptr_t) ppriv_front->flip_seq) != 0) {
        ms_drm_abort_seq(crtc->scrn, ppriv_front->flip_seq);
        return FALSE;
    }

    return TRUE;
}

static Bool
drmmode_InitSharedPixmapFlipping(xf86CrtcPtr crtc, drmmode_ptr drmmode)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    if (!drmmode_crtc->enable_flipping)
        return FALSE;

    if (drmmode_crtc->flipping_active)
        return TRUE;

    drmmode_crtc->flipping_active =
        drmmode_SharedPixmapPresent(drmmode_crtc->prime_pixmap_back,
                                    crtc, drmmode);

    return drmmode_crtc->flipping_active;
}

static void
drmmode_FiniSharedPixmapFlipping(xf86CrtcPtr crtc, drmmode_ptr drmmode)
{
    uint32_t seq;
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    if (!drmmode_crtc->flipping_active)
        return;

    drmmode_crtc->flipping_active = FALSE;

    /* Abort page flip event handler on prime_pixmap */
    seq = msGetPixmapPriv(drmmode, drmmode_crtc->prime_pixmap)->flip_seq;
    if (seq)
        ms_drm_abort_seq(crtc->scrn, seq);

    /* Abort page flip event handler on prime_pixmap_back */
    seq = msGetPixmapPriv(drmmode,
                          drmmode_crtc->prime_pixmap_back)->flip_seq;
    if (seq)
        ms_drm_abort_seq(crtc->scrn, seq);
}

static Bool drmmode_set_target_scanout_pixmap(xf86CrtcPtr crtc, PixmapPtr ppix,
                                              PixmapPtr *target);

Bool
drmmode_EnableSharedPixmapFlipping(xf86CrtcPtr crtc, drmmode_ptr drmmode,
                                   PixmapPtr front, PixmapPtr back)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    drmmode_crtc->enable_flipping = TRUE;

    /* Set front scanout pixmap */
    drmmode_crtc->enable_flipping &=
        drmmode_set_target_scanout_pixmap(crtc, front,
                                          &drmmode_crtc->prime_pixmap);
    if (!drmmode_crtc->enable_flipping)
        return FALSE;

    /* Set back scanout pixmap */
    drmmode_crtc->enable_flipping &=
        drmmode_set_target_scanout_pixmap(crtc, back,
                                          &drmmode_crtc->prime_pixmap_back);
    if (!drmmode_crtc->enable_flipping) {
        drmmode_set_target_scanout_pixmap(crtc, NULL,
                                          &drmmode_crtc->prime_pixmap);
        return FALSE;
    }

    return TRUE;
}

void
drmmode_DisableSharedPixmapFlipping(xf86CrtcPtr crtc, drmmode_ptr drmmode)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    drmmode_crtc->enable_flipping = FALSE;

    drmmode_FiniSharedPixmapFlipping(crtc, drmmode);

    drmmode_set_target_scanout_pixmap(crtc, NULL, &drmmode_crtc->prime_pixmap);

    drmmode_set_target_scanout_pixmap(crtc, NULL,
                                      &drmmode_crtc->prime_pixmap_back);
}

static void
drmmode_ConvertFromKMode(ScrnInfoPtr scrn,
                         drmModeModeInfo * kmode, DisplayModePtr mode)
{
    memset(mode, 0, sizeof(DisplayModeRec));
    mode->status = MODE_OK;

    mode->Clock = kmode->clock;

    mode->HDisplay = kmode->hdisplay;
    mode->HSyncStart = kmode->hsync_start;
    mode->HSyncEnd = kmode->hsync_end;
    mode->HTotal = kmode->htotal;
    mode->HSkew = kmode->hskew;

    mode->VDisplay = kmode->vdisplay;
    mode->VSyncStart = kmode->vsync_start;
    mode->VSyncEnd = kmode->vsync_end;
    mode->VTotal = kmode->vtotal;
    mode->VScan = kmode->vscan;

    mode->Flags = kmode->flags; //& FLAG_BITS;
    mode->name = strdup(kmode->name);

    if (kmode->type & DRM_MODE_TYPE_DRIVER)
        mode->type = M_T_DRIVER;
    if (kmode->type & DRM_MODE_TYPE_PREFERRED)
        mode->type |= M_T_PREFERRED;
    xf86SetModeCrtc(mode, scrn->adjustFlags);
}

static void
drmmode_ConvertToKMode(ScrnInfoPtr scrn,
                       drmModeModeInfo * kmode, DisplayModePtr mode)
{
    memset(kmode, 0, sizeof(*kmode));

    kmode->clock = mode->Clock;
    kmode->hdisplay = mode->HDisplay;
    kmode->hsync_start = mode->HSyncStart;
    kmode->hsync_end = mode->HSyncEnd;
    kmode->htotal = mode->HTotal;
    kmode->hskew = mode->HSkew;

    kmode->vdisplay = mode->VDisplay;
    kmode->vsync_start = mode->VSyncStart;
    kmode->vsync_end = mode->VSyncEnd;
    kmode->vtotal = mode->VTotal;
    kmode->vscan = mode->VScan;

    kmode->flags = mode->Flags; //& FLAG_BITS;
    if (mode->name)
        strncpy(kmode->name, mode->name, DRM_DISPLAY_MODE_LEN);
    kmode->name[DRM_DISPLAY_MODE_LEN - 1] = 0;

}

static void
drmmode_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    /* XXX Check if DPMS mode is already the right one */

    drmmode_crtc->dpms_mode = mode;

    if (mode != DPMSModeOn && !ms->pending_modeset)
        drmmode_crtc_disable(crtc);

}

#if defined(HAVE_GBM) || defined(FISSION_SOFT2D)
static Bool
depth_bpp_from_format(uint32_t pixel_format, int* depth, int* bpp)
{
    switch (pixel_format)
    {
        case DRM_FORMAT_XRGB1555:
            *depth = 15;
            *bpp = 16;
            return TRUE;
        
        case DRM_FORMAT_RGB565:
            *depth = 16;
            *bpp = 16;
            return TRUE;

        case DRM_FORMAT_XRGB8888:
            *depth = 24;
            *bpp = 32;
            return TRUE;

        case DRM_FORMAT_XRGB2101010:
            *depth = 30;
            *bpp = 32;
            return TRUE;

        case DRM_FORMAT_ARGB8888:
            *depth = 32;
            *bpp = 32;
            return TRUE;

        default:
            return FALSE;
    }
}

static int
handles_to_fds(drmmode_ptr drmmode, uint32_t *handles, int *fds)
{
    int err;
    int i;

    for (i = 0; i < 4 && handles[i]; i++) {
        err = drmPrimeHandleToFD(drmmode->fd, handles[i], O_CLOEXEC, &fds[i]);
        if (err != 0)
            return 0;
    }

    return i;
}

static PixmapPtr
create_pixmap_for_fbcon(drmmode_ptr drmmode, ScrnInfoPtr pScrn, int fbcon_id)
{
    PixmapPtr pixmap = drmmode->fbcon_pixmap;
    drmModeFB2Ptr fbcon2;
    ScreenPtr pScreen = xf86ScrnToScreen(pScrn);
    modesettingPtr ms = modesettingPTR(pScrn);
    uint32_t handles[4] = { 0, };
    CARD32 strides[4] = { 0, }, offsets[4] = { 0, };
    int fds[4] = { -1, -1, -1, -1 };
    uint64_t modifier;
    int num_fds, i;
    int width, height;
    int depth = 0, bpp = 0;

    if (pixmap)
        return pixmap;

    fbcon2 = drmModeGetFB2(drmmode->fd, fbcon_id);
    if (fbcon2 == NULL)
        return NULL;

    width = fbcon2->width;
    height = fbcon2->height;
    memcpy(handles, fbcon2->handles, sizeof(handles));
    memcpy(strides, fbcon2->pitches, sizeof(strides));
    memcpy(offsets, fbcon2->offsets, sizeof(offsets));
    modifier = fbcon2->modifier;

    if (!depth_bpp_from_format(fbcon2->pixel_format, &depth, &bpp))
        goto out_free_fb;

    if (depth != pScrn->depth ||
        width != pScrn->virtualX ||
        height != pScrn->virtualY)
        goto out_free_fb;

    /* GBM doesn't have an import path from handles, so we make a
     * DMA-BUF file descriptor from it and then go through that. */
    num_fds = handles_to_fds(drmmode, handles, fds);
    if (num_fds == 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to make prime FD for handle: %d\n", errno);
        goto out_close_fds;
    }

    if (ms->drmmode.glamor)
        pixmap = ms->glamor.glamor_pixmap_from_fds(pScreen, num_fds, fds,
                                                   width, height, strides, offsets,
                                                   depth, bpp, modifier);
#if defined(FISSION_SOFT2D)
    else if (ms->soft2d.pixmap_from_fds)
        pixmap = ms->soft2d.pixmap_from_fds(pScreen, num_fds, fds,
                                            width, height, strides, offsets,
                                            depth, bpp, modifier);
#endif

    if (!pixmap)
        goto out_close_fds;

    drmmode->fbcon_pixmap = pixmap;
out_close_fds:
    for (i = 0; i < ARRAY_SIZE(fds); i++) {
        if (fds[i] != -1)
            close(fds[i]);
    }
out_free_fb:
    drmModeFreeFB2(fbcon2);
    return pixmap;
}
#endif


void
drmmode_copy_fb(ScrnInfoPtr pScrn, drmmode_ptr drmmode)
{
#if defined(HAVE_GBM) || defined(FISSION_SOFT2D)
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    ScreenPtr pScreen = xf86ScrnToScreen(pScrn);
    PixmapPtr src, dst;
    int fbcon_id = 0;
    GCPtr gc;
    int i;

    for (i = 0; i < xf86_config->num_crtc; i++) {
        drmmode_crtc_private_ptr drmmode_crtc = xf86_config->crtc[i]->driver_private;
        if (drmmode_crtc->mode_crtc->buffer_id)
            fbcon_id = drmmode_crtc->mode_crtc->buffer_id;
    }

    if (!fbcon_id)
        return;

    if (fbcon_id == drmmode->fb_id) {
        /* in some rare case there might be no fbcon and we might already
         * be the one with the current fb to avoid a false deadlck in
         * kernel ttm code just do nothing as anyway there is nothing
         * to do
         */
        return;
    }

    src = create_pixmap_for_fbcon(drmmode, pScrn, fbcon_id);
    if (!src)
        return;

    dst = pScreen->GetScreenPixmap(pScreen);

    gc = GetScratchGC(pScrn->depth, pScreen);
    ValidateGC(&dst->drawable, gc);

    (void) (*gc->ops->CopyArea)(&src->drawable, &dst->drawable, gc, 0, 0,
                         pScrn->virtualX, pScrn->virtualY, 0, 0);

    FreeScratchGC(gc);

    pScreen->canDoBGNoneRoot = TRUE;

    if (drmmode->fbcon_pixmap)
        pScrn->pScreen->DestroyPixmap(drmmode->fbcon_pixmap);
    drmmode->fbcon_pixmap = NULL;
#endif
}

static void drmmode_prepare_modeset(ScrnInfoPtr scrn)
{
    ScreenPtr pScreen = scrn->pScreen;
    modesettingPtr ms = modesettingPTR(scrn);

    if (!ms->drmmode.present_flipping)
        return;

    if (ms->drmmode.pending_modeset)
        return;

    /*
     * Force present to unflip everything before we might
     * try lighting up new displays. This makes sure fancy
     * modifiers can't cause the modeset to fail.
     */
    ms->drmmode.pending_modeset = TRUE;
    present_check_flips(pScreen->root);
    ms->drmmode.pending_modeset = FALSE;

    ms_drain_drm_events(pScreen);
}

struct drmmode_saved_rotation {
    Bool prepared;
    uint32_t fb_id;
    drmmode_bo bo;
    PixmapPtr pixmap;
    void *data;
};

static void
drmmode_free_rotation(drmmode_ptr drmmode, PixmapPtr pixmap,
                      drmmode_bo *bo, uint32_t fb_id)
{
    if (pixmap)
        pixmap->drawable.pScreen->DestroyPixmap(pixmap);
    if (bo) {
        drmModeRmFB(drmmode->fd, fb_id);
        drmmode_bo_destroy(drmmode, bo);
    }
}

static Bool
drmmode_prepare_rotation(xf86CrtcPtr crtc, struct drmmode_saved_rotation *saved)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    Bool ret;

    saved->prepared = TRUE;
    saved->fb_id = drmmode_crtc->rotate_fb_id;
    drmmode_crtc->saved_rotation = saved;
    ret = xf86CrtcRotate(crtc);
    drmmode_crtc->saved_rotation = NULL;

    return ret;
}

static void
drmmode_finish_rotation(xf86CrtcPtr crtc, struct drmmode_saved_rotation *saved,
                        Bool success)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    if (!saved->prepared)
        return;

    if (success) {
        if (saved->data)
            drmmode_free_rotation(drmmode_crtc->drmmode, saved->pixmap,
                                   &saved->bo, saved->fb_id);
        return;
    }

    if (saved->data) {
        xf86RotateDestroy(crtc);
        drmmode_crtc->rotate_bo = saved->bo;
        drmmode_crtc->rotate_fb_id = saved->fb_id;
        crtc->rotatedData = saved->data;
        crtc->rotatedPixmap = saved->pixmap;
        if (!crtc->rotatedPixmap && crtc->mode.HDisplay) {
            crtc->rotatedPixmap = crtc->funcs->shadow_create(crtc, crtc->rotatedData,
                                                            crtc->mode.HDisplay,
                                                            crtc->mode.VDisplay);
            if (!crtc->rotatedPixmap)
                return;
        }
    }

    if (!crtc->mode.HDisplay)
        xf86RotateDestroy(crtc);
    else if (!xf86CrtcRotate(crtc))
        xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
                   "Failed to restore CRTC rotation\n");
}

static Bool
drmmode_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
                       Rotation rotation, int x, int y)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    DisplayModeRec saved_mode = crtc->mode;
    int saved_x = crtc->x, saved_y = crtc->y;
    Rotation saved_rotation = crtc->rotation;
    struct drmmode_saved_rotation saved_shadow = { 0 };
    int ret;

    if (!mode)
        return TRUE;

    drmmode_prepare_modeset(crtc->scrn);

    /* Present unflip can drop the old scanout framebuffer ID. */
    if (crtc->enabled && !drmmode->fb_id) {
        ret = drmmode_bo_import(drmmode, &drmmode->front_bo, &drmmode->fb_id);
        if (ret) {
            xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
                       "Failed to restore the front framebuffer: %s\n", strerror(-ret));
            return FALSE;
        }
    }

    crtc->mode = *mode;
    crtc->x = x;
    crtc->y = y;
    crtc->rotation = rotation;

    if (drmmode_prepare_rotation(crtc, &saved_shadow)) {
        ret = drmmode_set_mode_atomic(crtc->scrn, crtc, FALSE);
        if (!ret) {
            drmmode_finish_rotation(crtc, &saved_shadow, TRUE);
            return TRUE;
        }
        xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
                   "Failed to set CRTC mode: %s\n", strerror(-ret));
    }

    crtc->mode = saved_mode;
    crtc->x = saved_x;
    crtc->y = saved_y;
    crtc->rotation = saved_rotation;
    drmmode_finish_rotation(crtc, &saved_shadow, FALSE);

    return FALSE;
}

static void
drmmode_set_cursor_colors(xf86CrtcPtr crtc, int bg, int fg)
{

}

static void
drmmode_set_cursor_position(xf86CrtcPtr crtc, int x, int y)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;

    drmModeMoveCursor(drmmode->fd, drmmode_crtc->mode_crtc->crtc_id, x, y);
}

static Bool
drmmode_set_cursor(xf86CrtcPtr crtc, int width, int height, int xhot, int yhot)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    uint32_t handle = drmmode_crtc->cursor.bo->handle;
    CursorPtr cursor = xf86CurrentCursor(crtc->scrn->pScreen);
    int ret = -EINVAL;

    if (cursor == NullCursor)
	    return TRUE;

    ret = drmModeSetCursor2(drmmode->fd, drmmode_crtc->mode_crtc->crtc_id,
                            handle, width, height, xhot, yhot);

    /* -EINVAL can mean that an old kernel supports drmModeSetCursor but
     * not drmModeSetCursor2, though it can mean other things too. */
    if (ret == -EINVAL)
        ret = drmModeSetCursor(drmmode->fd, drmmode_crtc->mode_crtc->crtc_id,
                               handle, width, height);

    /* -ENXIO normally means that the current drm driver supports neither
     * cursor_set nor cursor_set2.  Disable hardware cursor support for
     * the rest of the session in that case. */
    if (ret == -ENXIO) {
        xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
        xf86CursorInfoPtr cursor_info = xf86_config->cursor_info;

        cursor_info->MaxWidth = cursor_info->MaxHeight = 0;
        drmmode_crtc->drmmode->sw_cursor = TRUE;
    }

    if (ret)
        /* fallback to swcursor */
        return FALSE;

    drmmode_crtc->cursor.width = width;
    drmmode_crtc->cursor.height = height;
    drmmode_crtc->cursor.src_x = xhot;
    drmmode_crtc->cursor.src_y = yhot;

    return TRUE;
}

static void drmmode_hide_cursor(xf86CrtcPtr crtc);

static Bool
drmmode_load_cursor_argb_check(xf86CrtcPtr crtc, CARD32 *image)
{
    CursorPtr cursor = xf86CurrentCursor(crtc->scrn->pScreen);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_cursor_rec drmmode_cursor = drmmode_crtc->cursor;
    int width, height, x, y, i;
    int glyph_x, glyph_y, glyph_width, glyph_height;
    int max_width, max_height;
    const Rotation rotation = crtc->rotation;
    uint32_t *ptr;

    /* cursor should be mapped already */
    ptr = (uint32_t *) (drmmode_cursor.bo->ptr);

    /* We need to know what our limit is for HW cursors. */
    max_width  = get_maximum_cursor_size(drmmode_cursor, FALSE);
    max_height = get_maximum_cursor_size(drmmode_cursor, TRUE);

    /* Find where the cursor glyph is located in the rotated buffer */
    xf86_crtc_transform_box(rotation, max_width, max_height,
                            cursor->bits->width, cursor->bits->height,
                            &glyph_x, &glyph_y, &glyph_width, &glyph_height);

    /* Find the most compatiable size. */
    for (i = 0; i < drmmode_cursor.num_dimensions; i++) {
        drmmode_cursor_dim_rec dimensions = drmmode_cursor.dimensions[i];

        if (dimensions.width >= glyph_width)
            break;

        if (dimensions.height >= glyph_height)
            break;
    }

    /* If we cannot get a cursor dimension then fallback to SWcursor. */
    if (i >= drmmode_cursor.num_dimensions)
        return FALSE;

    /* Get the resolution of the cursor. */
    width  = drmmode_cursor.dimensions[i].width;
    height = drmmode_cursor.dimensions[i].height;

    /* Copy the cursor image over. */
    i = 0;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            if (x < glyph_width && y < glyph_height)
                ptr[i++] =
                    cpu_to_le32(image[(glyph_y + y) * max_width + (glyph_x + x)]);
            else
                ptr[i++] = 0;
        }
    }

    /* Clear the remainder for good measure. */
    for (; i < max_width * max_height; i++)
        ptr[i] = 0;

    if (drmmode_crtc->cursor.up)
        return drmmode_set_cursor(crtc, width, height, glyph_x, glyph_y);
    else
        return TRUE;
}

static void
drmmode_hide_cursor(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;

    drmmode_crtc->cursor.up = FALSE;
    drmModeSetCursor(drmmode->fd, drmmode_crtc->mode_crtc->crtc_id, 0,
                     drmmode_crtc->cursor.width, drmmode_crtc->cursor.height);
}

static Bool
drmmode_show_cursor(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    drmmode_crtc->cursor.up = TRUE;

    return drmmode_set_cursor(crtc,
                              drmmode_crtc->cursor.width, drmmode_crtc->cursor.height,
                              drmmode_crtc->cursor.src_x, drmmode_crtc->cursor.src_y);
}

static void
drmmode_set_gamma_lut(drmmode_crtc_private_ptr drmmode_crtc,
                      uint16_t * red, uint16_t * green, uint16_t * blue,
                      int size)
{
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    uint32_t blob_id = 0;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    int ret;

    if (!req)
        return;

    ret = crtc_add_gamma_lut_prop(req, drmmode_crtc, red, green, blue,
                                  size, &blob_id);
    if (!ret)
        ret = drmmode_commit_color(drmmode, req);
    if (ret)
        xf86DrvMsg(drmmode->scrn->scrnIndex, X_ERROR,
                   "Failed to apply gamma LUT: %d\n", ret);

    if (blob_id)
        drmModeDestroyPropertyBlob(drmmode->fd, blob_id);
    drmModeAtomicFree(req);
}

static void
drmmode_crtc_gamma_set(xf86CrtcPtr crtc, uint16_t * red, uint16_t * green,
                       uint16_t * blue, int size)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    if (drmmode_crtc->use_gamma_lut) {
        drmmode_set_gamma_lut(drmmode_crtc, red, green, blue, size);
    } else {
        xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
                   "GAMMA_LUT not supported; gamma correction unavailable\n");
    }
}

static Bool
drmmode_set_target_scanout_pixmap_gpu(xf86CrtcPtr crtc, PixmapPtr ppix,
                                      PixmapPtr *target)
{
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    PixmapPtr screenpix = screen->GetScreenPixmap(screen);
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    int c, total_width = 0, max_height = 0, this_x = 0;

    if (*target) {
        PixmapStopDirtyTracking(&(*target)->drawable, screenpix);
        if (drmmode->fb_id) {
            drmModeRmFB(drmmode->fd, drmmode->fb_id);
            drmmode->fb_id = 0;
        }
        drmmode_crtc->prime_pixmap_x = 0;
        *target = NULL;
    }

    if (!ppix)
        return TRUE;

    /* iterate over all the attached crtcs to work out the bounding box */
    for (c = 0; c < xf86_config->num_crtc; c++) {
        xf86CrtcPtr iter = xf86_config->crtc[c];
        if (!iter->enabled && iter != crtc)
            continue;
        if (iter == crtc) {
            this_x = total_width;
            total_width += ppix->drawable.width;
            if (max_height < ppix->drawable.height)
                max_height = ppix->drawable.height;
        } else {
            total_width += iter->mode.HDisplay;
            if (max_height < iter->mode.VDisplay)
                max_height = iter->mode.VDisplay;
        }
    }

    if (total_width != screenpix->drawable.width ||
        max_height != screenpix->drawable.height) {

        if (!drmmode_xf86crtc_resize(crtc->scrn, total_width, max_height))
            return FALSE;

        screenpix = screen->GetScreenPixmap(screen);
        screen->width = screenpix->drawable.width = total_width;
        screen->height = screenpix->drawable.height = max_height;
    }
    drmmode_crtc->prime_pixmap_x = this_x;
    PixmapStartDirtyTracking(&ppix->drawable, screenpix, 0, 0, this_x, 0,
                             RR_Rotate_0);
    *target = ppix;
    return TRUE;
}

static inline int
drmModeAddFBPixmap(drmmode_ptr drmmode, PixmapPtr ppix, msPixmapPrivPtr ppriv)
{
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint32_t drm_format;

    /* Derive the fourcc format from depth/bitsPerPixel — drmModeAddFB2
     * requires an explicit format rather than the legacy depth/bpp pair. */
    switch (ppix->drawable.bitsPerPixel) {
    case 16:
        drm_format = (ppix->drawable.depth == 15) ?
                     DRM_FORMAT_XRGB1555 : DRM_FORMAT_RGB565;
        break;
    case 32:
        drm_format = (ppix->drawable.depth == 30) ?
                     DRM_FORMAT_XRGB2101010 : DRM_FORMAT_XRGB8888;
        break;
    default:
        drm_format = DRM_FORMAT_XRGB8888;
        break;
    }

    handles[0] = ppriv->backing_bo->handle;
    pitches[0] = ppix->devKind;
    
    return drmModeAddFB2(drmmode->fd, ppix->drawable.width,
                         ppix->drawable.height, drm_format,
                         handles, pitches, offsets, &ppriv->fb_id, 0);
}

static Bool
drmmode_set_target_scanout_pixmap_cpu(xf86CrtcPtr crtc, PixmapPtr ppix,
                                      PixmapPtr *target)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    msPixmapPrivPtr ppriv;
    void *ptr;

    if (*target) {
        ppriv = msGetPixmapPriv(drmmode, *target);
        drmModeRmFB(drmmode->fd, ppriv->fb_id);
        ppriv->fb_id = 0;
        if (ppriv->secondary_damage) {
            DamageUnregister(ppriv->secondary_damage);
            ppriv->secondary_damage = NULL;
        }
        *target = NULL;
    }

    if (!ppix)
        return TRUE;

    ppriv = msGetPixmapPriv(drmmode, ppix);
    if (!ppriv->secondary_damage) {
        ppriv->secondary_damage = DamageCreate(NULL, NULL,
                                           DamageReportNone,
                                           TRUE,
                                           crtc->randr_crtc->pScreen,
                                           NULL);
    }
    ptr = drmmode_map_secondary_bo(drmmode, ppriv);
    ppix->devPrivate.ptr = ptr;
    DamageRegister(&ppix->drawable, ppriv->secondary_damage);

    if (ppriv->fb_id == 0) {
        drmModeAddFBPixmap(drmmode, ppix, ppriv);
    }

    *target = ppix;
    return TRUE;
}

static Bool
drmmode_set_target_scanout_pixmap(xf86CrtcPtr crtc, PixmapPtr ppix,
                                  PixmapPtr *target)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;

    if (drmmode->reverse_prime_offload_mode)
        return drmmode_set_target_scanout_pixmap_gpu(crtc, ppix, target);
    else
        return drmmode_set_target_scanout_pixmap_cpu(crtc, ppix, target);
}

static Bool
drmmode_set_scanout_pixmap(xf86CrtcPtr crtc, PixmapPtr ppix)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    /* Use DisableSharedPixmapFlipping before switching to single buf */
    if (drmmode_crtc->enable_flipping)
        return FALSE;

    return drmmode_set_target_scanout_pixmap(crtc, ppix,
                                             &drmmode_crtc->prime_pixmap);
}

static void
drmmode_clear_pixmap(PixmapPtr pixmap)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    GCPtr gc;
#ifdef HAVE_GBM
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(screen));

    if (ms->drmmode.glamor) {
        ms->glamor.clear_pixmap(pixmap);
        return;
    }
#endif

    gc = GetScratchGC(pixmap->drawable.depth, screen);
    if (gc) {
        miClearDrawable(&pixmap->drawable, gc);
        FreeScratchGC(gc);
    }
}

static void *
drmmode_shadow_allocate(xf86CrtcPtr crtc, int width, int height)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    int ret;

    if (!drmmode_create_bo(drmmode, &drmmode_crtc->rotate_bo,
                           width, height, drmmode->kbpp, FALSE)) {
        xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
               "Couldn't allocate shadow memory for rotated CRTC\n");
        return NULL;
    }

    ret = drmmode_bo_import(drmmode, &drmmode_crtc->rotate_bo,
                            &drmmode_crtc->rotate_fb_id);

    if (ret) {
        ErrorF("failed to add rotate fb\n");
        drmmode_bo_destroy(drmmode, &drmmode_crtc->rotate_bo);
        return NULL;
    }

#if defined(HAVE_GBM) || defined(FISSION_SOFT2D)
    if (drmmode->gbm)
        return drmmode_crtc->rotate_bo.gbm;
#endif
    return drmmode_crtc->rotate_bo.dumb;
}

static PixmapPtr
drmmode_create_pixmap_header(ScreenPtr pScreen, int width, int height,
                             int depth, int bitsPerPixel, int devKind,
                             void *pPixData)
{
    PixmapPtr pixmap;

    /* width and height of 0 means don't allocate any pixmap data */
    pixmap = (*pScreen->CreatePixmap)(pScreen, 0, 0, depth, 0);

    if (pixmap) {
        if ((*pScreen->ModifyPixmapHeader)(pixmap, width, height, depth,
                                           bitsPerPixel, devKind, pPixData))
            return pixmap;
        (*pScreen->DestroyPixmap)(pixmap);
    }
    return NullPixmap;
}

static Bool
drmmode_set_pixmap_bo(drmmode_ptr drmmode, PixmapPtr pixmap, drmmode_bo *bo);

static PixmapPtr
drmmode_shadow_create(xf86CrtcPtr crtc, void *data, int width, int height)
{
    ScrnInfoPtr scrn = crtc->scrn;
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    uint32_t rotate_pitch;
    PixmapPtr rotate_pixmap;
    void *pPixData = NULL;

    if (!data) {
        data = drmmode_shadow_allocate(crtc, width, height);
        if (!data) {
            xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                       "Couldn't allocate shadow pixmap for rotated CRTC\n");
            return NULL;
        }
    }

    if (!drmmode_bo_has_bo(&drmmode_crtc->rotate_bo)) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "Couldn't allocate shadow pixmap for rotated CRTC\n");
        return NULL;
    }

    pPixData = drmmode_bo_map(drmmode, &drmmode_crtc->rotate_bo);
    rotate_pitch = drmmode_bo_get_pitch(&drmmode_crtc->rotate_bo);

    rotate_pixmap = drmmode_create_pixmap_header(scrn->pScreen,
                                                 width, height,
                                                 scrn->depth,
                                                 drmmode->kbpp,
                                                 rotate_pitch,
                                                 pPixData);

    if (rotate_pixmap == NULL) {
        xf86DrvMsg(scrn->scrnIndex, X_ERROR,
                   "Couldn't allocate shadow pixmap for rotated CRTC\n");
        return NULL;
    }

    drmmode_set_pixmap_bo(drmmode, rotate_pixmap, &drmmode_crtc->rotate_bo);
    return rotate_pixmap;
}

static void
drmmode_shadow_destroy(xf86CrtcPtr crtc, PixmapPtr rotate_pixmap, void *data)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    struct drmmode_saved_rotation *saved = drmmode_crtc->saved_rotation;

    /* The old framebuffer may still be scanning out until commit. */
    if (data && saved && saved->fb_id == drmmode_crtc->rotate_fb_id) {
        saved->bo = drmmode_crtc->rotate_bo;
        saved->pixmap = rotate_pixmap;
        saved->data = data;
    } else
        drmmode_free_rotation(drmmode, rotate_pixmap,
                               data ? &drmmode_crtc->rotate_bo : NULL,
                               drmmode_crtc->rotate_fb_id);

    if (data) {
        drmmode_crtc->rotate_fb_id = 0;
        memset(&drmmode_crtc->rotate_bo, 0, sizeof drmmode_crtc->rotate_bo);
    }
}

static void
drmmode_crtc_destroy(xf86CrtcPtr crtc)
{
    drmmode_mode_ptr iterator, next;
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    free(drmmode_crtc->cursor.dimensions);

    drmmode_prop_info_free(drmmode_crtc->props_plane, DRMMODE_PLANE__COUNT);
    xorg_list_for_each_entry_safe(iterator, next, &drmmode_crtc->mode_list, entry) {
        drm_mode_destroy(crtc, iterator);
    }
}

static const xf86CrtcFuncsRec drmmode_crtc_funcs = {
    .dpms = drmmode_crtc_dpms,
    .set_mode_major = drmmode_set_mode_major,
    .set_cursor_colors = drmmode_set_cursor_colors,
    .set_cursor_position = drmmode_set_cursor_position,
    .show_cursor_check = drmmode_show_cursor,
    .hide_cursor = drmmode_hide_cursor,
    .load_cursor_argb_check = drmmode_load_cursor_argb_check,

    .gamma_set = drmmode_crtc_gamma_set,
    .destroy = drmmode_crtc_destroy,
    .set_scanout_pixmap = drmmode_set_scanout_pixmap,
    .shadow_allocate = drmmode_shadow_allocate,
    .shadow_create = drmmode_shadow_create,
    .shadow_destroy = drmmode_shadow_destroy,
};

static uint32_t
drmmode_crtc_vblank_pipe(int crtc_id)
{
    if (crtc_id > 1)
        return crtc_id << DRM_VBLANK_HIGH_CRTC_SHIFT;
    else if (crtc_id > 0)
        return DRM_VBLANK_SECONDARY;
    else
        return 0;
}

static Bool
is_plane_assigned(ScrnInfoPtr scrn, int plane_id)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
    int c;

    for (c = 0; c < xf86_config->num_crtc; c++) {
        xf86CrtcPtr iter = xf86_config->crtc[c];
        drmmode_crtc_private_ptr drmmode_crtc = iter->driver_private;
        if (drmmode_crtc->plane_id == plane_id)
            return TRUE;
    }

    return FALSE;
}

/**
 * Populates the formats array, and the modifiers of each format for a drm_plane.
 */
static Bool
populate_format_modifiers(xf86CrtcPtr crtc, const drmModePlane *kplane,
                          drmmode_format_rec *formats, uint32_t blob_id)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    unsigned i, j;
    drmModePropertyBlobRes *blob;
    struct drm_format_modifier_blob *fmt_mod_blob;
    uint32_t *blob_formats;
    struct drm_format_modifier *blob_modifiers;

    if (!blob_id)
        return FALSE;

    blob = drmModeGetPropertyBlob(drmmode->fd, blob_id);
    if (!blob)
        return FALSE;

    fmt_mod_blob = blob->data;
    blob_formats = formats_ptr(fmt_mod_blob);
    blob_modifiers = modifiers_ptr(fmt_mod_blob);

    assert(drmmode_crtc->num_formats == fmt_mod_blob->count_formats);

    for (i = 0; i < fmt_mod_blob->count_formats; i++) {
        uint32_t num_modifiers = 0;
        uint64_t *modifiers = NULL;
        uint64_t *tmp;
        for (j = 0; j < fmt_mod_blob->count_modifiers; j++) {
            struct drm_format_modifier *mod = &blob_modifiers[j];

            if ((i < mod->offset) || (i > mod->offset + 63))
                continue;

            if (!(mod->formats & (1 << (i - mod->offset))))
                continue;

            /* We should never arrive on this. */
            if (mod->modifier == DRM_FORMAT_MOD_INVALID)
                continue;

            num_modifiers++;
            tmp = realloc(modifiers, num_modifiers * sizeof(modifiers[0]));
            if (!tmp) {
                free(modifiers);
                drmModeFreePropertyBlob(blob);
                return FALSE;
            }
            modifiers = tmp;
            modifiers[num_modifiers - 1] = mod->modifier;
        }

        formats[i].format = blob_formats[i];
        formats[i].modifiers = modifiers;
        formats[i].num_modifiers = num_modifiers;
    }

    drmModeFreePropertyBlob(blob);

    return TRUE;
}

#ifdef LIBDRM_PLANE_SIZE_HINTS
static void
populate_cursor_sizes(drmmode_ptr drmmode, drmmode_crtc_private_ptr drmmode_crtc, int size_hints_blob)
{
    drmModePropertyBlobRes *blob;

    if (!drmmode_crtc)
        return;

    if (!size_hints_blob)
        return;

    blob = drmModeGetPropertyBlob(drmmode->fd, size_hints_blob);

    if (!blob)
        return;

    if (!blob->length)
        goto fail;

    const struct drm_plane_size_hint *size_hints = blob->data;
    size_t size_hints_len = blob->length / sizeof(size_hints[0]);

    if (!size_hints_len)
        goto fail;
    
    drmmode_crtc->cursor.num_dimensions = size_hints_len;
    drmmode_crtc->cursor.dimensions = xnfrealloc(drmmode_crtc->cursor.dimensions, size_hints_len * sizeof(drmmode_cursor_dim_rec));

    for (int idx = 0; idx < size_hints_len; idx++)
    {
        struct drm_plane_size_hint size_hint = size_hints[idx];

        drmmode_crtc->cursor.dimensions[idx].width = size_hint.width;
        drmmode_crtc->cursor.dimensions[idx].height = size_hint.height;
    }
fail:
    drmModeFreePropertyBlob(blob);
}
#endif

static void
drmmode_crtc_create_planes(xf86CrtcPtr crtc, int num)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    drmModePlaneRes *kplane_res;
    drmModePlane *kplane, *best_kplane = NULL;
    drmModeObjectProperties *props;
    uint32_t i, type, blob_id, async_blob_id;
    int current_crtc, best_plane = 0;

    static drmmode_prop_enum_info_rec plane_type_enums[] = {
        [DRMMODE_PLANE_TYPE_PRIMARY] = {
            .name = "Primary",
        },
        [DRMMODE_PLANE_TYPE_OVERLAY] = {
            .name = "Overlay",
        },
        [DRMMODE_PLANE_TYPE_CURSOR] = {
            .name = "Cursor",
        },
    };
    static const drmmode_prop_info_rec plane_props[] = {
        [DRMMODE_PLANE_TYPE] = {
            .name = "type",
            .enum_values = plane_type_enums,
            .num_enum_values = DRMMODE_PLANE_TYPE__COUNT,
        },
        [DRMMODE_PLANE_FB_ID] = { .name = "FB_ID", },
        [DRMMODE_PLANE_CRTC_ID] = { .name = "CRTC_ID", },
        [DRMMODE_PLANE_IN_FORMATS] = { .name = "IN_FORMATS", },
        [DRMMODE_PLANE_IN_FORMATS_ASYNC] = { .name = "IN_FORMATS_ASYNC", },
        [DRMMODE_PLANE_SIZE_HINTS] = { .name = "SIZE_HINTS" },
        [DRMMODE_PLANE_SRC_X] = { .name = "SRC_X", },
        [DRMMODE_PLANE_SRC_Y] = { .name = "SRC_Y", },
        [DRMMODE_PLANE_SRC_W] = { .name = "SRC_W", },
        [DRMMODE_PLANE_SRC_H] = { .name = "SRC_H", },
        [DRMMODE_PLANE_CRTC_X] = { .name = "CRTC_X", },
        [DRMMODE_PLANE_CRTC_Y] = { .name = "CRTC_Y", },
        [DRMMODE_PLANE_CRTC_W] = { .name = "CRTC_W", },
        [DRMMODE_PLANE_CRTC_H] = { .name = "CRTC_H", },
    };
    drmmode_prop_info_rec tmp_props[DRMMODE_PLANE__COUNT];

    if (!drmmode_prop_info_copy(tmp_props, plane_props, DRMMODE_PLANE__COUNT, 0)) {
        xf86DrvMsg(drmmode->scrn->scrnIndex, X_ERROR,
                   "failed to copy plane property info\n");
        drmmode_prop_info_free(tmp_props, DRMMODE_PLANE__COUNT);
        return;
    }

    kplane_res = drmModeGetPlaneResources(drmmode->fd);
    if (!kplane_res) {
        xf86DrvMsg(drmmode->scrn->scrnIndex, X_ERROR,
                   "failed to get plane resources: %s\n", strerror(errno));
        drmmode_prop_info_free(tmp_props, DRMMODE_PLANE__COUNT);
        return;
    }

    for (i = 0; i < kplane_res->count_planes; i++) {
        int plane_id;

        kplane = drmModeGetPlane(drmmode->fd, kplane_res->planes[i]);
        if (!kplane)
            continue;

        if (!(kplane->possible_crtcs & (1 << num)) ||
            is_plane_assigned(drmmode->scrn, kplane->plane_id)) {
            drmModeFreePlane(kplane);
            continue;
        }

        plane_id = kplane->plane_id;

        props = drmModeObjectGetProperties(drmmode->fd, plane_id,
                                           DRM_MODE_OBJECT_PLANE);
        if (!props) {
            xf86DrvMsg(drmmode->scrn->scrnIndex, X_ERROR,
                    "couldn't get plane properties\n");
            drmModeFreePlane(kplane);
            continue;
        }

        drmmode_prop_info_update(drmmode, tmp_props, DRMMODE_PLANE__COUNT, props);

        type = drmmode_prop_get_value(&tmp_props[DRMMODE_PLANE_TYPE],
                                      props, DRMMODE_PLANE_TYPE__COUNT);

        switch (type)
        {
            case DRMMODE_PLANE_TYPE_PRIMARY: {
                current_crtc = drmmode_prop_get_value(&tmp_props[DRMMODE_PLANE_CRTC_ID], props, 0);

                if (current_crtc == drmmode_crtc->mode_crtc->crtc_id) {
                    if (best_plane) {
                        drmModeFreePlane(best_kplane);
                        drmmode_prop_info_free(drmmode_crtc->props_plane,
                                               DRMMODE_PLANE__COUNT);
                        drmModeFreeObjectProperties(props);
                        break;
                    } else {
                        best_plane = plane_id;
                        best_kplane = kplane;
                        blob_id = drmmode_prop_get_value(
                            &tmp_props[DRMMODE_PLANE_IN_FORMATS], props, 0);
                        async_blob_id = drmmode_prop_get_value(
                            &tmp_props[DRMMODE_PLANE_IN_FORMATS_ASYNC], props, 0);
                        drmmode_prop_info_copy(drmmode_crtc->props_plane, tmp_props,
                                       DRMMODE_PLANE__COUNT, 1);
                        drmModeFreeObjectProperties(props);
                        break;
                    }
                }

                if (!best_plane) {
                    best_plane = plane_id;
                    best_kplane = kplane;
                    blob_id = drmmode_prop_get_value(
                        &tmp_props[DRMMODE_PLANE_IN_FORMATS], props, 0);
                    async_blob_id = drmmode_prop_get_value(
                        &tmp_props[DRMMODE_PLANE_IN_FORMATS_ASYNC], props, 0);
                    drmmode_prop_info_copy(drmmode_crtc->props_plane, tmp_props,
                                       DRMMODE_PLANE__COUNT, 1);
                } else {
                    drmModeFreePlane(kplane);
                }

                drmModeFreeObjectProperties(props);
                break;
            }

#ifdef LIBDRM_PLANE_SIZE_HINTS
            case DRMMODE_PLANE_TYPE_CURSOR: {
                int size_hint = drmmode_prop_get_value(&tmp_props[DRMMODE_PLANE_SIZE_HINTS], props, 0);
                populate_cursor_sizes(drmmode, drmmode_crtc, size_hint);

                drmModeFreePlane(kplane);
                drmModeFreeObjectProperties(props);
                break;
            }
#endif

            default: {
                drmModeFreePlane(kplane);
                drmModeFreeObjectProperties(props);
                break;
            }
        }
    }

    drmmode_crtc->plane_id = best_plane;
    if (best_kplane) {
        drmmode_crtc->num_formats = best_kplane->count_formats;
        drmmode_crtc->formats = xnfcalloc(best_kplane->count_formats,
                                          sizeof(drmmode_format_rec));
        if (!populate_format_modifiers(crtc, best_kplane,
                                       drmmode_crtc->formats, blob_id)) {
            for (i = 0; i < best_kplane->count_formats; i++)
                drmmode_crtc->formats[i].format = best_kplane->formats[i];
        } else {
            drmmode_crtc->formats_async = xnfcalloc(best_kplane->count_formats,
                                                    sizeof(drmmode_format_rec));
            if (!populate_format_modifiers(crtc, best_kplane,
                                           drmmode_crtc->formats_async, async_blob_id)) {
                free(drmmode_crtc->formats_async);
                drmmode_crtc->formats_async = NULL;
            }
        }
        drmModeFreePlane(best_kplane);
    }

    drmmode_prop_info_free(tmp_props, DRMMODE_PLANE__COUNT);
    drmModeFreePlaneResources(kplane_res);
}

static unsigned int
drmmode_crtc_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode, drmModeResPtr mode_res, drmmode_cursor_dim_rec fallback, int num)
{
    xf86CrtcPtr crtc;
    drmmode_crtc_private_ptr drmmode_crtc;
    modesettingEntPtr ms_ent = ms_ent_priv(pScrn);
    drmModeObjectPropertiesPtr props;
    static const drmmode_prop_info_rec crtc_props[] = {
        [DRMMODE_CRTC_ACTIVE] = { .name = "ACTIVE" },
        [DRMMODE_CRTC_MODE_ID] = { .name = "MODE_ID" },
        [DRMMODE_CRTC_GAMMA_LUT] = { .name = "GAMMA_LUT" },
        [DRMMODE_CRTC_GAMMA_LUT_SIZE] = { .name = "GAMMA_LUT_SIZE" },
        [DRMMODE_CRTC_CTM] = { .name = "CTM" },
        [DRMMODE_CRTC_VRR_ENABLED] = { .name = "VRR_ENABLED" },
    };

    crtc = xf86CrtcCreate(pScrn, &drmmode_crtc_funcs);
    if (crtc == NULL)
        return 0;
    drmmode_crtc = XNFcallocarray(1, sizeof(drmmode_crtc_private_rec));
    crtc->driver_private = drmmode_crtc;
    drmmode_crtc->mode_crtc =
        drmModeGetCrtc(drmmode->fd, mode_res->crtcs[num]);
    drmmode_crtc->drmmode = drmmode;
    drmmode_crtc->vblank_pipe = drmmode_crtc_vblank_pipe(num);
    xorg_list_init(&drmmode_crtc->mode_list);

    /* Setup the fallback cursor immediately. */
    drmmode_crtc->cursor.num_dimensions = 1;
    drmmode_crtc->cursor.dimensions = xnfalloc(sizeof(drmmode_cursor_dim_rec));

    drmmode_crtc->cursor.dimensions[0].width = fallback.width;
    drmmode_crtc->cursor.dimensions[0].height = fallback.height;
    /* */

    props = drmModeObjectGetProperties(drmmode->fd, mode_res->crtcs[num],
                                       DRM_MODE_OBJECT_CRTC);
    if (!props || !drmmode_prop_info_copy(drmmode_crtc->props, crtc_props,
                                          DRMMODE_CRTC__COUNT, 0)) {
        xf86CrtcDestroy(crtc);
        return 0;
    }

    drmmode_prop_info_update(drmmode, drmmode_crtc->props,
                             DRMMODE_CRTC__COUNT, props);
    drmModeFreeObjectProperties(props);
    drmmode_crtc_create_planes(crtc, num);

    /* Mark num'th crtc as in use on this device. */
    ms_ent->assigned_crtcs |= (1 << num);
    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, MS_LOGLEVEL_DEBUG,
                   "Allocated crtc nr. %d to this screen.\n", num);

    if (drmmode_crtc->props[DRMMODE_CRTC_GAMMA_LUT].prop_id &&
        drmmode_crtc->props[DRMMODE_CRTC_GAMMA_LUT_SIZE].prop_id &&
        drmmode_crtc->props[DRMMODE_CRTC_GAMMA_LUT_SIZE].value) {
        /*
         * GAMMA_LUT property supported, and so far tested to be safe to use by
         * default for lut sizes up to 4096 slots. Intel Tigerlake+ has some
         * issues, and a large GAMMA_LUT with 262145 slots, so keep GAMMA_LUT
         * off for large lut sizes by default for now.
         */
        drmmode_crtc->use_gamma_lut = drmmode_crtc->props[DRMMODE_CRTC_GAMMA_LUT_SIZE].value <= 4096;

        /* Allow config override. */
        drmmode_crtc->use_gamma_lut = xf86ReturnOptValBool(drmmode->Options,
                                                           OPTION_USE_GAMMA_LUT,
                                                           drmmode_crtc->use_gamma_lut);
    } else {
        drmmode_crtc->use_gamma_lut = FALSE;
    }

    if (drmmode_crtc->use_gamma_lut &&
        drmmode_crtc->props[DRMMODE_CRTC_CTM].prop_id) {
        drmmode->use_ctm = TRUE;
    }

    return 1;
}

/*
 * Update all of the property values for an output
 */
static void
drmmode_output_update_properties(xf86OutputPtr output)
{
    drmmode_output_private_ptr drmmode_output = output->driver_private;
    int i, j, k;
    int err;
    drmModeConnectorPtr koutput;

    /* Use the most recently fetched values from the kernel */
    koutput = drmmode_output->mode_output;

    if (!koutput)
        return;

    for (i = 0; i < drmmode_output->num_props; i++) {
        drmmode_prop_ptr p = &drmmode_output->props[i];

        for (j = 0; koutput && j < koutput->count_props; j++) {
            if (koutput->props[j] == p->mode_prop->prop_id) {

                /* Check to see if the property value has changed */
                if (koutput->prop_values[j] != p->value) {

                    p->value = koutput->prop_values[j];

                    if (p->mode_prop->flags & DRM_MODE_PROP_RANGE) {
                        INT32 value = p->value;

                        err = RRChangeOutputProperty(output->randr_output, p->atoms[0],
                                                     XA_INTEGER, 32, PropModeReplace, 1,
                                                     &value, FALSE, TRUE);

                        if (err != 0) {
                            xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                                       "RRChangeOutputProperty error, %d\n", err);
                        }
                    }
                    else if (p->mode_prop->flags & DRM_MODE_PROP_ENUM) {
                        for (k = 0; k < p->mode_prop->count_enums; k++)
                            if (p->mode_prop->enums[k].value == p->value)
                                break;
                        if (k < p->mode_prop->count_enums) {
                            err = RRChangeOutputProperty(output->randr_output, p->atoms[0],
                                                         XA_ATOM, 32, PropModeReplace, 1,
                                                         &p->atoms[k + 1], FALSE, TRUE);
                            if (err != 0) {
                                xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                                           "RRChangeOutputProperty error, %d\n", err);
                            }
                        }
                    }
                }
                break;
            }
        }
    }

    /* Update the CTM property */
    if (drmmode_output->ctm_atom) {
        err = RRChangeOutputProperty(output->randr_output,
                                     drmmode_output->ctm_atom,
                                     XA_INTEGER, 32, PropModeReplace, 18,
                                     &drmmode_output->ctm,
                                     FALSE, TRUE);
        if (err != 0) {
            xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                       "RRChangeOutputProperty error, %d\n", err);
        }
    }

}

static xf86OutputStatus
drmmode_output_detect(xf86OutputPtr output)
{
    /* go to the hw and retrieve a new output struct */
    drmmode_output_private_ptr drmmode_output = output->driver_private;
    drmmode_ptr drmmode = drmmode_output->drmmode;
    xf86OutputStatus status;

    if (drmmode_output->output_id == -1)
        return XF86OutputStatusDisconnected;

    drmModeFreeConnector(drmmode_output->mode_output);

    drmmode_output->mode_output =
        drmModeGetConnector(drmmode->fd, drmmode_output->output_id);

    if (!drmmode_output->mode_output) {
        drmmode_output->output_id = -1;
        return XF86OutputStatusDisconnected;
    }

    drmmode_output_update_properties(output);

    switch (drmmode_output->mode_output->connection) {
    case DRM_MODE_CONNECTED:
        status = XF86OutputStatusConnected;
        break;
    case DRM_MODE_DISCONNECTED:
        status = XF86OutputStatusDisconnected;
        break;
    default:
    case DRM_MODE_UNKNOWNCONNECTION:
        status = XF86OutputStatusUnknown;
        break;
    }
    return status;
}

static Bool
drmmode_output_mode_valid(xf86OutputPtr output, DisplayModePtr pModes)
{
    return MODE_OK;
}

static int
koutput_get_prop_idx(int fd, drmModeConnectorPtr koutput,
        int type, const char *name)
{
    int idx = -1;

    for (int i = 0; i < koutput->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, koutput->props[i]);

        if (!prop)
            continue;

        if (drm_property_type_is(prop, type) && !strcmp(prop->name, name))
            idx = i;

        drmModeFreeProperty(prop);

        if (idx > -1)
            break;
    }

    return idx;
}

static drmModePropertyBlobPtr
koutput_get_prop_blob(int fd, drmModeConnectorPtr koutput, const char *name)
{
    drmModePropertyBlobPtr blob = NULL;
    int idx = koutput_get_prop_idx(fd, koutput, DRM_MODE_PROP_BLOB, name);

    if (idx > -1)
        blob = drmModeGetPropertyBlob(fd, koutput->prop_values[idx]);

    return blob;
}

static void
drmmode_output_attach_tile(xf86OutputPtr output)
{
    drmmode_output_private_ptr drmmode_output = output->driver_private;
    drmModeConnectorPtr koutput = drmmode_output->mode_output;
    drmmode_ptr drmmode = drmmode_output->drmmode;
    struct xf86CrtcTileInfo tile_info, *set = NULL;

    if (!koutput) {
        xf86OutputSetTile(output, NULL);
        return;
    }

    drmModeFreePropertyBlob(drmmode_output->tile_blob);

    /* look for a TILE property */
    drmmode_output->tile_blob =
        koutput_get_prop_blob(drmmode->fd, koutput, "TILE");

    if (drmmode_output->tile_blob) {
        if (xf86OutputParseKMSTile(drmmode_output->tile_blob->data, drmmode_output->tile_blob->length, &tile_info) == TRUE)
            set = &tile_info;
    }
    xf86OutputSetTile(output, set);
}

static Bool
has_panel_fitter(xf86OutputPtr output)
{
    drmmode_output_private_ptr drmmode_output = output->driver_private;
    drmModeConnectorPtr koutput = drmmode_output->mode_output;
    drmmode_ptr drmmode = drmmode_output->drmmode;
    int idx;

    /* Presume that if the output supports scaling, then we have a
     * panel fitter capable of adjust any mode to suit.
     */
    idx = koutput_get_prop_idx(drmmode->fd, koutput,
            DRM_MODE_PROP_ENUM, "scaling mode");

    return (idx > -1);
}

static DisplayModePtr
drmmode_output_add_gtf_modes(xf86OutputPtr output, DisplayModePtr Modes)
{
    xf86MonPtr mon = output->MonInfo;
    DisplayModePtr i, j, m, preferred = NULL;
    int max_x = 0, max_y = 0;
    float max_vrefresh = 0.0;

    if (mon && gtf_supported(mon))
        return Modes;

    if (!xf86OutputDefaultModesEnabled(output))
        return Modes;

    if (!has_panel_fitter(output))
        return Modes;

    for (m = Modes; m; m = m->next) {
        if (m->type & M_T_PREFERRED)
            preferred = m;
        max_x = max(max_x, m->HDisplay);
        max_y = max(max_y, m->VDisplay);
        max_vrefresh = max(max_vrefresh, xf86ModeVRefresh(m));
    }

    max_vrefresh = max(max_vrefresh, 60.0);
    max_vrefresh *= (1 + SYNC_TOLERANCE);

    m = xf86GetDefaultModes();
    xf86ValidateModesSize(output->scrn, m, max_x, max_y, 0);

    for (i = m; i; i = i->next) {
        if (xf86ModeVRefresh(i) > max_vrefresh)
            i->status = MODE_VSYNC;
        if (preferred &&
            i->HDisplay >= preferred->HDisplay &&
            i->VDisplay >= preferred->VDisplay &&
            xf86ModeVRefresh(i) >= xf86ModeVRefresh(preferred))
            i->status = MODE_VSYNC;
        if (preferred && xf86ModeVRefresh(i) > 0.0) {
            i->Clock = i->Clock * xf86ModeVRefresh(preferred) / xf86ModeVRefresh(i);
            i->VRefresh = xf86ModeVRefresh(preferred);
        }
        for (j = m; j != i; j = j->next) {
            if (!strcmp(i->name, j->name) &&
                xf86ModeVRefresh(i) * (1 + SYNC_TOLERANCE) >= xf86ModeVRefresh(j) &&
                xf86ModeVRefresh(i) * (1 - SYNC_TOLERANCE) <= xf86ModeVRefresh(j)) {
                i->status = MODE_DUPLICATE;
            }
        }
    }

    xf86PruneInvalidModes(output->scrn, &m, FALSE);

    return xf86ModesAdd(Modes, m);
}

static DisplayModePtr
drmmode_output_get_modes(xf86OutputPtr output)
{
    drmmode_output_private_ptr drmmode_output = output->driver_private;
    drmModeConnectorPtr koutput = drmmode_output->mode_output;
    drmmode_ptr drmmode = drmmode_output->drmmode;
    int i;
    DisplayModePtr Modes = NULL, Mode;
    xf86MonPtr mon = NULL;

    if (!koutput)
        return NULL;

    drmModeFreePropertyBlob(drmmode_output->edid_blob);

    /* look for an EDID property */
    drmmode_output->edid_blob =
        koutput_get_prop_blob(drmmode->fd, koutput, "EDID");

    if (drmmode_output->edid_blob) {
        mon = xf86InterpretEDID(output->scrn->scrnIndex,
                                drmmode_output->edid_blob->data);
        if (mon && drmmode_output->edid_blob->length > 128)
            mon->flags |= MONITOR_EDID_COMPLETE_RAWDATA;
    }
    xf86OutputSetEDID(output, mon);

    drmmode_output_attach_tile(output);

    /* modes should already be available */
    for (i = 0; i < koutput->count_modes; i++) {
        Mode = XNFalloc(sizeof(DisplayModeRec));

        drmmode_ConvertFromKMode(output->scrn, &koutput->modes[i], Mode);
        Modes = xf86ModesAdd(Modes, Mode);

    }

    return drmmode_output_add_gtf_modes(output, Modes);
}

static void
drmmode_output_destroy(xf86OutputPtr output)
{
    drmmode_output_private_ptr drmmode_output = output->driver_private;
    int i;

    drmModeFreePropertyBlob(drmmode_output->edid_blob);
    drmModeFreePropertyBlob(drmmode_output->tile_blob);

    for (i = 0; i < drmmode_output->num_props; i++) {
        drmModeFreeProperty(drmmode_output->props[i].mode_prop);
        free(drmmode_output->props[i].atoms);
    }
    free(drmmode_output->props);
    if (drmmode_output->mode_output) {
        for (i = 0; i < drmmode_output->mode_output->count_encoders; i++) {
            drmModeFreeEncoder(drmmode_output->mode_encoders[i]);
        }
        drmModeFreeConnector(drmmode_output->mode_output);
    }
    free(drmmode_output->mode_encoders);
    free(drmmode_output);
    output->driver_private = NULL;
}

static void
drmmode_output_dpms(xf86OutputPtr output, int mode)
{
    modesettingPtr ms = modesettingPTR(output->scrn);
    drmmode_output_private_ptr drmmode_output = output->driver_private;
    drmmode_ptr drmmode = drmmode_output->drmmode;
    xf86CrtcPtr crtc = output->crtc;
    drmModeConnectorPtr koutput = drmmode_output->mode_output;

    if (!koutput)
        return;

    /* XXX Check if DPMS mode is already the right one */

    drmmode_output->dpms = mode;

    if (!ms->pending_modeset)
        drmmode_output_dpms_atomic(output, mode);

    if (crtc) {
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

        if (mode == DPMSModeOn) {
            if (drmmode_crtc->need_modeset)
                drmmode_set_mode_major(crtc, &crtc->mode, crtc->rotation,
                                       crtc->x, crtc->y);

            if (drmmode_crtc->enable_flipping)
                drmmode_InitSharedPixmapFlipping(crtc, drmmode);
        } else {
            if (drmmode_crtc->enable_flipping)
                drmmode_FiniSharedPixmapFlipping(crtc, drmmode);
        }
    }
}

static Bool
drmmode_property_ignore(drmModePropertyPtr prop)
{
    if (!prop)
        return TRUE;
    /* ignore blob prop */
    if (prop->flags & DRM_MODE_PROP_BLOB)
        return TRUE;
    /* ignore standard property */
    if (!strcmp(prop->name, "EDID") || !strcmp(prop->name, "DPMS") ||
        !strcmp(prop->name, "CRTC_ID"))
        return TRUE;

    return FALSE;
}

static void
drmmode_output_create_resources(xf86OutputPtr output)
{
    drmmode_output_private_ptr drmmode_output = output->driver_private;
    drmModeConnectorPtr mode_output = drmmode_output->mode_output;
    drmmode_ptr drmmode = drmmode_output->drmmode;
    drmModePropertyPtr drmmode_prop;
    int i, j, err;

    BUG_RETURN(drmmode_output->props != NULL);

    drmmode_output->props =
        calloc(mode_output->count_props, sizeof(drmmode_prop_rec));
    if (!drmmode_output->props)
        return;

    drmmode_output->num_props = 0;
    for (i = 0, j = 0; i < mode_output->count_props; i++) {
        drmmode_prop = drmModeGetProperty(drmmode->fd, mode_output->props[i]);
        if (drmmode_property_ignore(drmmode_prop)) {
            drmModeFreeProperty(drmmode_prop);
            continue;
        }
        drmmode_output->props[j].mode_prop = drmmode_prop;
        drmmode_output->props[j].value = mode_output->prop_values[i];
        drmmode_output->num_props++;
        j++;
    }

    /* Create CONNECTOR_ID property */
    {
        Atom    name = MakeAtom("CONNECTOR_ID", 12, TRUE);
        INT32   value = mode_output->connector_id;

        if (name != BAD_RESOURCE) {
            err = RRConfigureOutputProperty(output->randr_output, name,
                                            FALSE, FALSE, TRUE,
                                            1, &value);
            if (err != 0) {
                xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                           "RRConfigureOutputProperty error, %d\n", err);
            }
            err = RRChangeOutputProperty(output->randr_output, name,
                                         XA_INTEGER, 32, PropModeReplace, 1,
                                         &value, FALSE, FALSE);
            if (err != 0) {
                xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                           "RRChangeOutputProperty error, %d\n", err);
            }
        }
    }

    if (drmmode->use_ctm) {
        Atom name = MakeAtom("CTM", 3, TRUE);

        if (name != BAD_RESOURCE) {
            drmmode_output->ctm_atom = name;

            err = RRConfigureOutputProperty(output->randr_output, name,
                                            FALSE, FALSE, TRUE, 0, NULL);
            if (err != 0) {
                xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                           "RRConfigureOutputProperty error, %d\n", err);
            }

            err = RRChangeOutputProperty(output->randr_output, name,
                                         XA_INTEGER, 32, PropModeReplace, 18,
                                         &ctm_identity, FALSE, FALSE);
            if (err != 0) {
                xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                           "RRChangeOutputProperty error, %d\n", err);
            }

            drmmode_output->ctm = ctm_identity;
        }
    }

    for (i = 0; i < drmmode_output->num_props; i++) {
        drmmode_prop_ptr p = &drmmode_output->props[i];

        drmmode_prop = p->mode_prop;

        if (drmmode_prop->flags & DRM_MODE_PROP_RANGE) {
            INT32 prop_range[2];
            INT32 value = p->value;

            p->num_atoms = 1;
            p->atoms = calloc(p->num_atoms, sizeof(Atom));
            if (!p->atoms)
                continue;
            p->atoms[0] =
                MakeAtom(drmmode_prop->name, strlen(drmmode_prop->name), TRUE);
            prop_range[0] = drmmode_prop->values[0];
            prop_range[1] = drmmode_prop->values[1];
            err = RRConfigureOutputProperty(output->randr_output, p->atoms[0],
                                            FALSE, TRUE,
                                            drmmode_prop->
                                            flags & DRM_MODE_PROP_IMMUTABLE ?
                                            TRUE : FALSE, 2, prop_range);
            if (err != 0) {
                xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                           "RRConfigureOutputProperty error, %d\n", err);
            }
            err = RRChangeOutputProperty(output->randr_output, p->atoms[0],
                                         XA_INTEGER, 32, PropModeReplace, 1,
                                         &value, FALSE, TRUE);
            if (err != 0) {
                xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                           "RRChangeOutputProperty error, %d\n", err);
            }
        }
        else if (drmmode_prop->flags & DRM_MODE_PROP_ENUM) {
            p->num_atoms = drmmode_prop->count_enums + 1;
            p->atoms = calloc(p->num_atoms, sizeof(Atom));
            if (!p->atoms)
                continue;
            p->atoms[0] =
                MakeAtom(drmmode_prop->name, strlen(drmmode_prop->name), TRUE);
            for (j = 1; j <= drmmode_prop->count_enums; j++) {
                struct drm_mode_property_enum *e = &drmmode_prop->enums[j - 1];

                p->atoms[j] = MakeAtom(e->name, strlen(e->name), TRUE);
            }
            err = RRConfigureOutputProperty(output->randr_output, p->atoms[0],
                                            FALSE, FALSE,
                                            drmmode_prop->
                                            flags & DRM_MODE_PROP_IMMUTABLE ?
                                            TRUE : FALSE, p->num_atoms - 1,
                                            (INT32 *) &p->atoms[1]);
            if (err != 0) {
                xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                           "RRConfigureOutputProperty error, %d\n", err);
            }
            for (j = 0; j < drmmode_prop->count_enums; j++)
                if (drmmode_prop->enums[j].value == p->value)
                    break;
            /* there's always a matching value */
            err = RRChangeOutputProperty(output->randr_output, p->atoms[0],
                                         XA_ATOM, 32, PropModeReplace, 1,
                                         &p->atoms[j + 1], FALSE, TRUE);
            if (err != 0) {
                xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
                           "RRChangeOutputProperty error, %d\n", err);
            }
        }
    }
}

static inline void
drmmode_output_commit(drmmode_ptr drmmode, uint32_t object_id,
                      uint32_t property_id, uint64_t value)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    int r;

    if (!req)
        return;

    r = drmModeAtomicAddProperty(req, object_id, property_id, value);

    if (r >= 0)
        drmModeAtomicCommit(drmmode->fd, req,
                            DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    
    drmModeAtomicFree(req);
}

static Bool
drmmode_output_set_property(xf86OutputPtr output, Atom property,
                            RRPropertyValuePtr value)
{
    drmmode_output_private_ptr drmmode_output = output->driver_private;
    drmmode_ptr drmmode = drmmode_output->drmmode;

    for (int i = 0; i < drmmode_output->num_props; i++) {
        drmmode_prop_ptr p = &drmmode_output->props[i];

        if (!p->atoms)
            continue;

        if (p->atoms[0] != property)
            continue;

        if (p->mode_prop->flags & DRM_MODE_PROP_RANGE) {
            uint32_t val;

            if (value->type != XA_INTEGER || value->format != 32 ||
                value->size != 1)
                return FALSE;
            val = *(uint32_t *) value->data;

            drmmode_output_commit(drmmode, drmmode_output->output_id,
                                  p->mode_prop->prop_id, (uint64_t) val);
            return TRUE;
        }
        else if (p->mode_prop->flags & DRM_MODE_PROP_ENUM) {
            Atom atom;
            const char *name;

            if (value->type != XA_ATOM || value->format != 32 ||
                value->size != 1)
                return FALSE;
            memcpy(&atom, value->data, 4);
            if (!(name = NameForAtom(atom)))
                return FALSE;

            /* search for matching name string, then set its value down */
            for (int j = 0; j < p->mode_prop->count_enums; j++) {
                if (!strcmp(p->mode_prop->enums[j].name, name)) {
                    drmmode_output_commit(drmmode, drmmode_output->output_id,
                                          p->mode_prop->prop_id, p->mode_prop->enums[j].value);
                    return TRUE;
                }
            }
        }
    }

    if (property == drmmode_output->ctm_atom) {
        const size_t matrix_size = sizeof(drmmode_output->ctm);

        if (value->type != XA_INTEGER || value->format != 32 ||
            value->size * 4 != matrix_size)
            return FALSE;

        memcpy(&drmmode_output->ctm, value->data, matrix_size);

        // Update the CRTC if there is one bound to this output.
        if (output->crtc) {
            drmmode_set_ctm(output->crtc, &drmmode_output->ctm);
        }
    }

    return TRUE;
}

static Bool
drmmode_output_get_property(xf86OutputPtr output, Atom property)
{
    return TRUE;
}

static const xf86OutputFuncsRec drmmode_output_funcs = {
    .dpms = drmmode_output_dpms,
    .create_resources = drmmode_output_create_resources,
    .set_property = drmmode_output_set_property,
    .get_property = drmmode_output_get_property,
    .detect = drmmode_output_detect,
    .mode_valid = drmmode_output_mode_valid,

    .get_modes = drmmode_output_get_modes,
    .destroy = drmmode_output_destroy
};

static int subpixel_conv_table[7] = {
    0,
    SubPixelUnknown,
    SubPixelHorizontalRGB,
    SubPixelHorizontalBGR,
    SubPixelVerticalRGB,
    SubPixelVerticalBGR,
    SubPixelNone
};

static const char *const output_names[] = {
    "None",
    "VGA",
    "DVI-I",
    "DVI-D",
    "DVI-A",
    "Composite",
    "SVIDEO",
    "LVDS",
    "Component",
    "DIN",
    "DP",
    "HDMI",
    "HDMI-B",
    "TV",
    "eDP",
    "Virtual",
    "DSI",
    "DPI",
};

static xf86OutputPtr find_output(ScrnInfoPtr pScrn, int id)
{
    xf86CrtcConfigPtr   xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    int i;
    for (i = 0; i < xf86_config->num_output; i++) {
        xf86OutputPtr output = xf86_config->output[i];
        drmmode_output_private_ptr drmmode_output;

        drmmode_output = output->driver_private;
        if (drmmode_output->output_id == id)
            return output;
    }
    return NULL;
}

static int parse_path_blob(drmModePropertyBlobPtr path_blob, int *conn_base_id, char **path)
{
    char *conn;
    char conn_id[5];
    int id, len;
    char *blob_data;

    if (!path_blob)
        return -1;

    blob_data = path_blob->data;
    /* we only handle MST paths for now */
    if (strncmp(blob_data, "mst:", 4))
        return -1;

    conn = strchr(blob_data + 4, '-');
    if (!conn)
        return -1;
    len = conn - (blob_data + 4);
    if (len + 1> 5)
        return -1;
    memcpy(conn_id, blob_data + 4, len);
    conn_id[len] = '\0';
    id = strtoul(conn_id, NULL, 10);

    *conn_base_id = id;

    *path = conn + 1;
    return 0;
}

static void
drmmode_create_name(ScrnInfoPtr pScrn, drmModeConnectorPtr koutput, char *name,
		    drmModePropertyBlobPtr path_blob)
{
    int ret;
    char *extra_path;
    int conn_id;
    xf86OutputPtr output;

    ret = parse_path_blob(path_blob, &conn_id, &extra_path);
    if (ret == -1)
        goto fallback;

    output = find_output(pScrn, conn_id);
    if (!output)
        goto fallback;

    snprintf(name, 32, "%s-%s", output->name, extra_path);
    return;

 fallback:
    if (koutput->connector_type >= ARRAY_SIZE(output_names))
        snprintf(name, 32, "Unknown%d-%d", koutput->connector_type, koutput->connector_type_id);
    else if (pScrn->is_gpu)
        snprintf(name, 32, "%s-%d-%d", output_names[koutput->connector_type], pScrn->scrnIndex - GPU_SCREEN_OFFSET + 1, koutput->connector_type_id);
    else
        snprintf(name, 32, "%s-%d", output_names[koutput->connector_type], koutput->connector_type_id);
}

static Bool
drmmode_connector_check_vrr_capable(uint32_t drm_fd, int connector_id)
{
    uint32_t i;
    Bool found = FALSE;
    uint64_t prop_value = 0;
    drmModeObjectPropertiesPtr props;
    const char* prop_name = "VRR_CAPABLE";

    props = drmModeObjectGetProperties(drm_fd, connector_id,
                                       DRM_MODE_OBJECT_CONNECTOR);

    if (!props)
        return FALSE;

    for (i = 0; !found && i < props->count_props; ++i) {
        drmModePropertyPtr drm_prop = drmModeGetProperty(drm_fd, props->props[i]);

        if (!drm_prop)
            continue;

        if (strcasecmp(drm_prop->name, prop_name) == 0) {
            prop_value = props->prop_values[i];
            found = TRUE;
        }

        drmModeFreeProperty(drm_prop);
    }

    drmModeFreeObjectProperties(props);

    if(found)
        return prop_value ? TRUE : FALSE;

    return FALSE;
}

static unsigned int
drmmode_output_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode, drmModeResPtr mode_res, int num, Bool dynamic, int crtcshift)
{
    xf86OutputPtr output;
    xf86CrtcConfigPtr   xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    modesettingPtr ms = modesettingPTR(pScrn);
    drmModeConnectorPtr koutput;
    drmModeEncoderPtr *kencoders = NULL;
    drmmode_output_private_ptr drmmode_output;
    char name[32];
    int i;
    Bool nonDesktop = FALSE;
    drmModePropertyBlobPtr path_blob = NULL;
    const char *s;
    drmModeObjectPropertiesPtr props;
    static const drmmode_prop_info_rec connector_props[] = {
        [DRMMODE_CONNECTOR_CRTC_ID] = { .name = "CRTC_ID", },
    };

    koutput =
        drmModeGetConnector(drmmode->fd, mode_res->connectors[num]);
    if (!koutput)
        return 0;

    path_blob = koutput_get_prop_blob(drmmode->fd, koutput, "PATH");
    i = koutput_get_prop_idx(drmmode->fd, koutput, DRM_MODE_PROP_RANGE, RR_PROPERTY_NON_DESKTOP);
    if (i >= 0)
        nonDesktop = koutput->prop_values[i] != 0;

    drmmode_create_name(pScrn, koutput, name, path_blob);

    if (path_blob)
        drmModeFreePropertyBlob(path_blob);

    if (path_blob && dynamic) {
        /* see if we have an output with this name already
           and hook stuff up */
        for (i = 0; i < xf86_config->num_output; i++) {
            output = xf86_config->output[i];

            if (strncmp(output->name, name, 32))
                continue;

            drmmode_output = output->driver_private;
            drmmode_output->output_id = mode_res->connectors[num];
            drmmode_output->mode_output = koutput;
            output->non_desktop = nonDesktop;
            return 1;
        }
    }

    kencoders = calloc(koutput->count_encoders, sizeof(drmModeEncoderPtr));
    if (!kencoders) {
        goto out_free_encoders;
    }

    for (i = 0; i < koutput->count_encoders; i++) {
        kencoders[i] = drmModeGetEncoder(drmmode->fd, koutput->encoders[i]);
        if (!kencoders[i]) {
            goto out_free_encoders;
        }
    }

    if (xf86IsEntityShared(pScrn->entityList[0])) {
        if ((s = xf86GetOptValString(drmmode->Options, OPTION_ZAPHOD_HEADS))) {
            if (!drmmode_zaphod_string_matches(pScrn, s, name))
                goto out_free_encoders;
        } else {
            if (!drmmode->is_secondary && (num != 0))
                goto out_free_encoders;
            else if (drmmode->is_secondary && (num != 1))
                goto out_free_encoders;
        }
    }

    output = xf86OutputCreate(pScrn, &drmmode_output_funcs, name);
    if (!output) {
        goto out_free_encoders;
    }

    drmmode_output = calloc(1, sizeof(drmmode_output_private_rec));
    if (!drmmode_output) {
        xf86OutputDestroy(output);
        goto out_free_encoders;
    }

    drmmode_output->output_id = mode_res->connectors[num];
    drmmode_output->mode_output = koutput;
    drmmode_output->mode_encoders = kencoders;
    drmmode_output->drmmode = drmmode;
    output->mm_width = koutput->mmWidth;
    output->mm_height = koutput->mmHeight;

    output->subpixel_order = subpixel_conv_table[koutput->subpixel];
    output->interlaceAllowed = TRUE;
    output->doubleScanAllowed = TRUE;
    output->driver_private = drmmode_output;
    output->non_desktop = nonDesktop;

    output->possible_crtcs = 0;
    for (i = 0; i < koutput->count_encoders; i++) {
        output->possible_crtcs |= (kencoders[i]->possible_crtcs >> crtcshift) & 0x7f;
    }
    /* work out the possible clones later */
    output->possible_clones = 0;

    if (!drmmode_prop_info_copy(drmmode_output->props_connector,
                                connector_props, DRMMODE_CONNECTOR__COUNT,
                                0)) {
        goto out_free_encoders;
    }

    props = drmModeObjectGetProperties(drmmode->fd,
                                       drmmode_output->output_id,
                                       DRM_MODE_OBJECT_CONNECTOR);
    drmmode_prop_info_update(drmmode, drmmode_output->props_connector,
                             DRMMODE_CONNECTOR__COUNT, props);

    if (dynamic) {
        output->randr_output = RROutputCreate(xf86ScrnToScreen(pScrn), output->name, strlen(output->name), output);
        if (output->randr_output) {
            drmmode_output_create_resources(output);
            RRPostPendingProperties(output->randr_output);
        }
    }

    ms->is_connector_vrr_capable |=
	         drmmode_connector_check_vrr_capable(drmmode->fd,
                                                  drmmode_output->output_id);
    return 1;

 out_free_encoders:
    if (kencoders) {
        for (i = 0; i < koutput->count_encoders; i++)
            drmModeFreeEncoder(kencoders[i]);
        free(kencoders);
    }
    drmModeFreeConnector(koutput);

    return 0;
}

static uint32_t
find_clones(ScrnInfoPtr scrn, xf86OutputPtr output)
{
    drmmode_output_private_ptr drmmode_output =
        output->driver_private, clone_drmout;
    int i;
    xf86OutputPtr clone_output;
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);
    int index_mask = 0;

    if (drmmode_output->enc_clone_mask == 0)
        return index_mask;

    for (i = 0; i < xf86_config->num_output; i++) {
        clone_output = xf86_config->output[i];
        clone_drmout = clone_output->driver_private;
        if (output == clone_output)
            continue;

        if (clone_drmout->enc_mask == 0)
            continue;
        if (drmmode_output->enc_clone_mask == clone_drmout->enc_mask)
            index_mask |= (1 << i);
    }
    return index_mask;
}

static void
drmmode_clones_init(ScrnInfoPtr scrn, drmmode_ptr drmmode, drmModeResPtr mode_res)
{
    int i, j;
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(scrn);

    for (i = 0; i < xf86_config->num_output; i++) {
        xf86OutputPtr output = xf86_config->output[i];
        drmmode_output_private_ptr drmmode_output;

        drmmode_output = output->driver_private;
        drmmode_output->enc_clone_mask = 0xff;
        /* and all the possible encoder clones for this output together */
        for (j = 0; j < drmmode_output->mode_output->count_encoders; j++) {
            int k;

            for (k = 0; k < mode_res->count_encoders; k++) {
                if (mode_res->encoders[k] ==
                    drmmode_output->mode_encoders[j]->encoder_id)
                    drmmode_output->enc_mask |= (1 << k);
            }

            drmmode_output->enc_clone_mask &=
                drmmode_output->mode_encoders[j]->possible_clones;
        }
    }

    for (i = 0; i < xf86_config->num_output; i++) {
        xf86OutputPtr output = xf86_config->output[i];

        output->possible_clones = find_clones(scrn, output);
    }
}

static Bool
drmmode_set_pixmap_bo(drmmode_ptr drmmode, PixmapPtr pixmap, drmmode_bo *bo)
{
#if defined(HAVE_GBM) || defined(FISSION_SOFT2D)
    ScrnInfoPtr scrn = drmmode->scrn;
    modesettingPtr ms = modesettingPTR(scrn);

    if (drmmode->glamor) {
        if (!ms->glamor.egl_create_textured_pixmap_from_gbm_bo(pixmap, bo->gbm,
                                                               bo->used_modifiers)) {
            xf86DrvMsg(scrn->scrnIndex, X_ERROR, "Failed to create pixmap\n");
            return FALSE;
        }
    }
#ifdef FISSION_SOFT2D
    else if (bo->gbm) {
        if (!ms->soft2d.pixmap_from_gbm_bo(pixmap, bo->gbm)) {
            xf86DrvMsg(scrn->scrnIndex, X_ERROR, "Failed to create pixmap\n");
            return FALSE;
        }
    }
#endif
#endif

    return TRUE;
}

Bool
drmmode_glamor_handle_new_screen_pixmap(drmmode_ptr drmmode)
{
    ScreenPtr screen = xf86ScrnToScreen(drmmode->scrn);
    PixmapPtr screen_pixmap = screen->GetScreenPixmap(screen);

    if (!drmmode_set_pixmap_bo(drmmode, screen_pixmap, &drmmode->front_bo))
        return FALSE;

    return TRUE;
}

static Bool
drmmode_xf86crtc_resize(ScrnInfoPtr scrn, int width, int height)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    modesettingPtr ms = modesettingPTR(scrn);
    drmmode_ptr drmmode = &ms->drmmode;
    ScreenPtr screen = xf86ScrnToScreen(scrn);
    PixmapPtr pixmap = screen->GetScreenPixmap(screen);
    drmmode_bo new_front = { 0 }, old_front = { 0 };
    uint32_t new_fb_id = 0, old_fb_id = 0;
    int cpp = (scrn->bitsPerPixel + 7) / 8;
    int kcpp = (drmmode->kbpp + 7) / 8;
    int old_width = scrn->virtualX, old_height = scrn->virtualY;
    int old_display_width = scrn->displayWidth;
    int old_pixmap_width = pixmap->drawable.width;
    int old_pixmap_height = pixmap->drawable.height;
    int old_stride = pixmap->devKind;
    void *old_pixels = pixmap->devPrivate.ptr;
    void *new_pixels = NULL;
    const char *error;
    int pitch, ret;
    struct drmmode_saved_rotation *saved;

    if (old_width == width && old_height == height)
        return TRUE;

    drmmode_prepare_modeset(scrn);

    saved = calloc(config->num_crtc, sizeof(*saved));
    if (config->num_crtc && !saved) {
        error = "failed to save CRTC rotation";
        goto fail;
    }

    if (!drmmode_create_bo(drmmode, &new_front, width, height, drmmode->kbpp,
                           !drmmode->glamor)) {
        error = "failed to allocate the new framebuffer";
        goto fail;
    }

    if (drmmode_bo_import(drmmode, &new_front, &new_fb_id)) {
        error = "failed to register the new framebuffer";
        goto fail;
    }

    if (new_front.dumb) {
        new_pixels = drmmode_bo_map(drmmode, &new_front);
        if (!new_pixels) {
            error = "failed to map the new framebuffer";
            goto fail;
        }
    }

    pitch = drmmode_bo_get_pitch(&new_front);
    XORG_EXCHANGE(old_front, drmmode->front_bo);
    XORG_EXCHANGE(old_fb_id, drmmode->fb_id);
    drmmode->front_bo = new_front;
    drmmode->fb_id = new_fb_id;
    scrn->virtualX = width;
    scrn->virtualY = height;
    scrn->displayWidth = pitch / kcpp;

    if (!screen->ModifyPixmapHeader(pixmap, width, height, -1, -1,
                                    scrn->displayWidth * cpp, new_pixels) ||
        !drmmode_glamor_handle_new_screen_pixmap(drmmode)) {
        error = "failed to bind the new screen pixmap";
        goto restore;
    }

    drmmode_clear_pixmap(pixmap);

    for (int i = 0; i < config->num_crtc; i++) {
        xf86CrtcPtr crtc = config->crtc[i];

        if (crtc->enabled && !drmmode_crtc_is_leased(crtc) &&
            drmmode_target_output(scrn, crtc) &&
            !drmmode_prepare_rotation(crtc, &saved[i])) {
            error = "failed to prepare CRTC rotation";
            goto restore;
        }
    }

    ret = drmmode_set_mode_atomic(scrn, NULL, FALSE);
    if (ret) {
        error = strerror(-ret);
        goto restore;
    }

    for (int i = 0; i < config->num_crtc; i++)
        drmmode_finish_rotation(config->crtc[i], &saved[i], TRUE);

    xf86DrvMsg(scrn->scrnIndex, X_INFO,
               "Resized framebuffer to %dx%d with pitch %d\n", width, height, pitch);
    if (old_fb_id)
        drmModeRmFB(drmmode->fd, old_fb_id);
    drmmode_bo_destroy(drmmode, &old_front);
    free(saved);
    return TRUE;

 restore:
    drmmode->front_bo = old_front;
    drmmode->fb_id = old_fb_id;
    scrn->virtualX = old_width;
    scrn->virtualY = old_height;
    scrn->displayWidth = old_display_width;

    if (!screen->ModifyPixmapHeader(pixmap, old_pixmap_width, old_pixmap_height,
                                    -1, -1, old_stride, old_pixels) ||
        !drmmode_glamor_handle_new_screen_pixmap(drmmode))
        xf86DrvMsg(scrn->scrnIndex, X_ERROR, "Failed to restore the screen pixmap\n");

    for (int i = 0; i < config->num_crtc; i++)
        drmmode_finish_rotation(config->crtc[i], &saved[i], FALSE);

 fail:
    free(saved);
    xf86DrvMsg(scrn->scrnIndex, X_ERROR,
               "Failed to resize framebuffer to %dx%d (%s)\n", width, height, error);
    if (new_fb_id)
        drmModeRmFB(drmmode->fd, new_fb_id);
    drmmode_bo_destroy(drmmode, &new_front);

    return FALSE;
}

static void
drmmode_validate_leases(ScrnInfoPtr scrn)
{
    ScreenPtr screen = scrn->pScreen;
    rrScrPrivPtr scr_priv;
    modesettingPtr ms = modesettingPTR(scrn);
    drmmode_ptr drmmode = &ms->drmmode;
    drmModeLesseeListPtr lessees;
    RRLeasePtr lease, next;
    int l;

    /* Bail out if RandR wasn't initialized. */
    if (!dixPrivateKeyRegistered(rrPrivKey))
        return;

    scr_priv = rrGetScrPriv(screen);

    /* We can't talk to the kernel about leases when VT switched */
    if (!scrn->vtSema)
        return;

    lessees = drmModeListLessees(drmmode->fd);
    if (!lessees)
        return;

    xorg_list_for_each_entry_safe(lease, next, &scr_priv->leases, list) {
        drmmode_lease_private_ptr lease_private = lease->devPrivate;

        for (l = 0; l < lessees->count; l++) {
            if (lessees->lessees[l] == lease_private->lessee_id)
                break;
        }

        /* check to see if the lease has gone away */
        if (l == lessees->count) {
            free(lease_private);
            lease->devPrivate = NULL;
            xf86CrtcLeaseTerminated(lease);
        }
    }

    free(lessees);
}

static int
drmmode_create_lease(RRLeasePtr lease, int *fd)
{
    ScreenPtr screen = lease->screen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    drmmode_ptr drmmode = &ms->drmmode;
    int ncrtc = lease->numCrtcs;
    int noutput = lease->numOutputs;
    int nobjects;
    int c, o;
    int i;
    int lease_fd;
    uint32_t *objects;
    drmmode_lease_private_ptr   lease_private;

    nobjects = ncrtc + noutput;
    nobjects += ncrtc; /* account for planes as well */

    if (nobjects == 0)
        return BadValue;

    lease_private = calloc(1, sizeof (drmmode_lease_private_rec));
    if (!lease_private)
        return BadAlloc;

    objects = xallocarray(nobjects, sizeof (uint32_t));

    if (!objects) {
        free(lease_private);
        return BadAlloc;
    }

    i = 0;

    /* Add CRTC and plane ids */
    for (c = 0; c < ncrtc; c++) {
        xf86CrtcPtr crtc = lease->crtcs[c]->devPrivate;
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

        objects[i++] = drmmode_crtc->mode_crtc->crtc_id;
        objects[i++] = drmmode_crtc->plane_id;
    }

    /* Add connector ids */
    for (o = 0; o < noutput; o++) {
        xf86OutputPtr   output = lease->outputs[o]->devPrivate;
        drmmode_output_private_ptr drmmode_output = output->driver_private;

        objects[i++] = drmmode_output->mode_output->connector_id;
    }

    /* call kernel to create lease */
    assert (i == nobjects);

    lease_fd = drmModeCreateLease(drmmode->fd, objects, nobjects, 0, &lease_private->lessee_id);

    free(objects);

    if (lease_fd < 0) {
        free(lease_private);
        return BadMatch;
    }

    lease->devPrivate = lease_private;

    xf86CrtcLeaseStarted(lease);

    *fd = lease_fd;
    return Success;
}

static void
drmmode_terminate_lease(RRLeasePtr lease)
{
    ScreenPtr screen = lease->screen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    drmmode_ptr drmmode = &ms->drmmode;
    drmmode_lease_private_ptr lease_private = lease->devPrivate;

    if (drmModeRevokeLease(drmmode->fd, lease_private->lessee_id) == 0) {
        free(lease_private);
        lease->devPrivate = NULL;
        xf86CrtcLeaseTerminated(lease);
    }
}

static const xf86CrtcConfigFuncsRec drmmode_xf86crtc_config_funcs = {
    .resize = drmmode_xf86crtc_resize,
    .create_lease = drmmode_create_lease,
    .terminate_lease = drmmode_terminate_lease
};

Bool
drmmode_pre_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode, int cpp)
{
    modesettingEntPtr ms_ent = ms_ent_priv(pScrn);
    int i;
    int ret;
    uint64_t value = 0;
    unsigned int crtcs_needed = 0;
    drmModeResPtr mode_res;
    int crtcshift;

    /* check for dumb capability */
    ret = drmGetCap(drmmode->fd, DRM_CAP_DUMB_BUFFER, &value);
    if (ret > 0 || value != 1) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "KMS doesn't support dumb interface\n");
        return FALSE;
    }

    xf86CrtcConfigInit(pScrn, &drmmode_xf86crtc_config_funcs);

    drmmode->scrn = pScrn;
    drmmode->cpp = cpp;
    mode_res = drmModeGetResources(drmmode->fd);
    if (!mode_res)
        return FALSE;

    crtcshift = ffs(ms_ent->assigned_crtcs ^ 0xffffffff) - 1;
    for (i = 0; i < mode_res->count_connectors; i++)
        crtcs_needed += drmmode_output_init(pScrn, drmmode, mode_res, i, FALSE,
                                            crtcshift);

    xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, MS_LOGLEVEL_DEBUG,
                   "Up to %d crtcs needed for screen.\n", crtcs_needed);

    /* This is the safest fallback value. */
    drmmode_cursor_dim_rec fallback = {
        .width  = 64,
        .height = 64,
    };

    ret = drmGetCap(drmmode->fd, DRM_CAP_CURSOR_WIDTH, &value);
    if (!ret) {
        fallback.width = value;
    }

    ret = drmGetCap(drmmode->fd, DRM_CAP_CURSOR_HEIGHT, &value);
    if (!ret) {
        fallback.height = value;
    }

    xf86CrtcSetSizeRange(pScrn, 320, 200, mode_res->max_width,
                         mode_res->max_height);
    for (i = 0; i < mode_res->count_crtcs; i++)
        if (!xf86IsEntityShared(pScrn->entityList[0]) ||
            (crtcs_needed && !(ms_ent->assigned_crtcs & (1 << i))))
            crtcs_needed -= drmmode_crtc_init(pScrn, drmmode, mode_res, fallback, i);

    /* All ZaphodHeads outputs provided with matching crtcs? */
    if (xf86IsEntityShared(pScrn->entityList[0]) && (crtcs_needed > 0))
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "%d ZaphodHeads crtcs unavailable. Some outputs will stay off.\n",
                   crtcs_needed);

    /* workout clones */
    drmmode_clones_init(pScrn, drmmode, mode_res);

    drmModeFreeResources(mode_res);
    xf86ProviderSetup(pScrn, NULL, "modesetting");

    xf86InitialConfiguration(pScrn, TRUE);

    return TRUE;
}

Bool
drmmode_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode)
{
    ScreenPtr pScreen = xf86ScrnToScreen(pScrn);
    modesettingPtr ms = modesettingPTR(pScrn);

#ifdef HAVE_GBM
    if (drmmode->glamor) {
        if (!ms->glamor.init(pScreen, GLAMOR_USE_EGL_SCREEN)) {
            return FALSE;
        }
#ifdef GBM_BO_WITH_MODIFIERS
        ms->glamor.set_drawable_modifiers_func(pScreen, get_drawable_modifiers);
#endif
        return TRUE;
    }
#endif
#ifdef FISSION_SOFT2D
    if (ms->soft2d.init) {
        if (!ms->soft2d.init(pScreen, ms->drmmode.gbm, ms->fd))
            return FALSE;
        
        ms->soft2d.set_drawable_modifiers_func(pScreen,
                                               drmmode_soft2d_get_drawable_modifiers);
        ms->soft2d.set_formats_func(pScreen, drmmode_soft2d_get_formats);
        ms->soft2d.set_modifiers_func(pScreen, drmmode_soft2d_get_modifiers);
    }
#endif

    return TRUE;
}

void
drmmode_adjust_frame(ScrnInfoPtr pScrn, drmmode_ptr drmmode, int x, int y)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
    xf86OutputPtr output = config->output[config->compat_output];
    xf86CrtcPtr crtc = output->crtc;

    if (crtc && crtc->enabled) {
        drmmode_set_mode_major(crtc, &crtc->mode, crtc->rotation, x, y);
    }
}

struct drmmode_saved_crtc {
    Rotation rotation;
    int x, y;
    DisplayModeRec mode;
    struct drmmode_saved_rotation shadow;
};

Bool
drmmode_set_desired_modes(ScrnInfoPtr pScrn, drmmode_ptr drmmode, Bool set_hw,
                          Bool ign_err)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
    struct drmmode_saved_crtc *saved;
    Bool success = TRUE;
    int ret = 0;

    drmmode_validate_leases(pScrn);
    drmmode_prepare_modeset(pScrn);

    saved = calloc(config->num_crtc, sizeof(*saved));
    if (config->num_crtc && !saved)
        return FALSE;

    for (int i = 0; i < config->num_crtc; i++) {
        xf86CrtcPtr crtc = config->crtc[i];
        xf86OutputPtr output;

        if (!crtc->enabled || drmmode_crtc_is_leased(crtc))
            continue;

        output = drmmode_target_output(pScrn, crtc);
        if (!output)
            continue;

        if (!crtc->desiredMode.CrtcHDisplay) {
            DisplayModePtr mode = xf86OutputFindClosestMode(output, pScrn->currentMode);

            if (!mode) {
                ret = -EINVAL;
                break;
            }
            crtc->desiredMode = *mode;
            crtc->desiredRotation = RR_Rotate_0;
            crtc->desiredX = 0;
            crtc->desiredY = 0;
        }

        saved[i].mode = crtc->mode;
        saved[i].rotation = crtc->rotation;
        saved[i].x = crtc->x;
        saved[i].y = crtc->y;

        crtc->mode = crtc->desiredMode;
        crtc->rotation = crtc->desiredRotation;
        crtc->x = crtc->desiredX;
        crtc->y = crtc->desiredY;
        if (!drmmode_prepare_rotation(crtc, &saved[i].shadow)) {
            ret = -ENOMEM;
            break;
        }
    }

    if (!ret)
        ret = drmmode_set_mode_atomic(pScrn, NULL, !set_hw);

    /* CreateScreenResources needs the desired layout after a successful test. */
    for (int i = 0; i < config->num_crtc; i++) {
        xf86CrtcPtr crtc = config->crtc[i];

        if (ret && saved[i].shadow.prepared) {
            crtc->mode = saved[i].mode;
            crtc->rotation = saved[i].rotation;
            crtc->x = saved[i].x;
            crtc->y = saved[i].y;
        }
        drmmode_finish_rotation(crtc, &saved[i].shadow, ret == 0);
    }
    free(saved);

    if (!ret)
        return TRUE;

    xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
               "Failed to set desired modes: %s\n", strerror(-ret));
    if (!set_hw || !ign_err)
        return FALSE;

    /* A connector may have disappeared while the VT was switched away. */
    for (int i = 0; i < config->num_crtc; i++) {
        xf86CrtcPtr crtc = config->crtc[i];
        xf86OutputPtr output;

        if (drmmode_crtc_is_leased(crtc))
            continue;

        if (!crtc->enabled) {
            if (drmmode_set_mode_atomic(pScrn, crtc, FALSE))
                success = FALSE;
            continue;
        }

        output = drmmode_target_output(pScrn, crtc);
        if (output && crtc->desiredMode.CrtcHDisplay &&
            drmmode_set_mode_major(crtc, &crtc->desiredMode, crtc->desiredRotation,
                                   crtc->desiredX, crtc->desiredY))
            continue;

        success = FALSE;
        crtc->enabled = FALSE;
        if (output)
            xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                       "Failed to set the desired mode on connector %s\n", output->name);
    }

    return success;
}

static void
drmmode_load_palette(ScrnInfoPtr pScrn, int numColors,
                     int *indices, LOCO * colors, VisualPtr pVisual)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    uint16_t lut_r[256], lut_g[256], lut_b[256];
    int index, j, i;
    int c;

    for (c = 0; c < xf86_config->num_crtc; c++) {
        xf86CrtcPtr crtc = xf86_config->crtc[c];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

        for (i = 0; i < 256; i++) {
            lut_r[i] = drmmode_crtc->lut_r[i] << 6;
            lut_g[i] = drmmode_crtc->lut_g[i] << 6;
            lut_b[i] = drmmode_crtc->lut_b[i] << 6;
        }

        switch (pScrn->depth) {
        case 15:
            for (i = 0; i < numColors; i++) {
                index = indices[i];
                for (j = 0; j < 8; j++) {
                    lut_r[index * 8 + j] = colors[index].red << 6;
                    lut_g[index * 8 + j] = colors[index].green << 6;
                    lut_b[index * 8 + j] = colors[index].blue << 6;
                }
            }
            break;
        case 16:
            for (i = 0; i < numColors; i++) {
                index = indices[i];

                if (i <= 31) {
                    for (j = 0; j < 8; j++) {
                        lut_r[index * 8 + j] = colors[index].red << 6;
                        lut_b[index * 8 + j] = colors[index].blue << 6;
                    }
                }

                for (j = 0; j < 4; j++) {
                    lut_g[index * 4 + j] = colors[index].green << 6;
                }
            }
            break;
        default:
            for (i = 0; i < numColors; i++) {
                index = indices[i];
                lut_r[index] = colors[index].red << 6;
                lut_g[index] = colors[index].green << 6;
                lut_b[index] = colors[index].blue << 6;
            }
            break;
        }

        /* Make the change through RandR */
        if (crtc->randr_crtc)
            RRCrtcGammaSet(crtc->randr_crtc, lut_r, lut_g, lut_b);
        else
            crtc->funcs->gamma_set(crtc, lut_r, lut_g, lut_b, 256);
    }
}

static Bool
drmmode_crtc_upgrade_lut(xf86CrtcPtr crtc, int num)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    uint64_t size;

    if (!drmmode_crtc->use_gamma_lut)
        return TRUE;

    assert(drmmode_crtc->props[DRMMODE_CRTC_GAMMA_LUT_SIZE].prop_id);

    size = drmmode_crtc->props[DRMMODE_CRTC_GAMMA_LUT_SIZE].value;

    if (size != crtc->gamma_size) {
        ScrnInfoPtr pScrn = crtc->scrn;
        uint16_t *gamma = malloc(3 * size * sizeof(uint16_t));

        if (gamma) {
            free(crtc->gamma_red);

            crtc->gamma_size = size;
            crtc->gamma_red = gamma;
            crtc->gamma_green = gamma + size;
            crtc->gamma_blue = gamma + size * 2;

            xf86DrvMsgVerb(pScrn->scrnIndex, X_INFO, MS_LOGLEVEL_DEBUG,
                           "Gamma ramp set to %lld entries on CRTC %d\n",
                           (long long)size, num);
        } else {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "Failed to allocate memory for %lld gamma ramp entries "
                       "on CRTC %d.\n",
                       (long long)size, num);
            return FALSE;
        }
    }

    return TRUE;
}

Bool
drmmode_setup_colormap(ScreenPtr pScreen, ScrnInfoPtr pScrn)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    int i;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
              "Initializing kms color map for depth %d, %d bpc.\n",
              pScrn->depth, pScrn->rgbBits);
    if (!miCreateDefColormap(pScreen))
        return FALSE;

    /* If the GAMMA_LUT property is available, replace the server's default
     * gamma ramps with ones of the appropriate size. */
    for (i = 0; i < xf86_config->num_crtc; i++)
        if (!drmmode_crtc_upgrade_lut(xf86_config->crtc[i], i))
            return FALSE;

    /* Adapt color map size and depth to color depth of screen. */
    if (!xf86HandleColormaps(pScreen, 1 << pScrn->rgbBits, 10,
                             drmmode_load_palette, NULL,
                             CMAP_PALETTED_TRUECOLOR |
                             CMAP_RELOAD_ON_MODE_SWITCH))
        return FALSE;
    return TRUE;
}

#define DRM_MODE_LINK_STATUS_GOOD       0
#define DRM_MODE_LINK_STATUS_BAD        1

void
drmmode_update_kms_state(drmmode_ptr drmmode)
{
    ScrnInfoPtr scrn = drmmode->scrn;
    drmModeResPtr mode_res;
    xf86CrtcConfigPtr  config = XF86_CRTC_CONFIG_PTR(scrn);
    int i, j;
    Bool found = FALSE;
    Bool changed = FALSE;
    Bool link_fixed = FALSE;

    /* Try to re-set the mode on all the connectors with a BAD link-state:
     * This may happen if a link degrades and a new modeset is necessary, using
     * different link-training parameters. If the kernel found that the current
     * mode is not achievable anymore, it should have pruned the mode before
     * sending the hotplug event. Try to re-set the currently-set mode to keep
     * the display alive, this will fail if the mode has been pruned.
     * In any case, we will send randr events for the Desktop Environment to
     * deal with it, if it wants to.
     */
    for (i = 0; i < config->num_output; i++) {
        xf86OutputPtr output = config->output[i];
        drmmode_output_private_ptr drmmode_output = output->driver_private;

        drmmode_output_detect(output);

        /* Get an updated view of the properties for the current connector and
         * look for the link-status property
         */
        for (j = 0; j < drmmode_output->num_props; j++) {
            drmmode_prop_ptr p = &drmmode_output->props[j];

            if (!strcmp(p->mode_prop->name, "link-status")) {
                if (p->value == DRM_MODE_LINK_STATUS_BAD) {
                    xf86CrtcPtr crtc = output->crtc;
                    if (!crtc)
                        continue;

                    /* the connector got a link failure, re-set the current mode */
                    drmmode_set_mode_major(crtc, &crtc->mode, crtc->rotation,
                                           crtc->x, crtc->y);
                    link_fixed = TRUE;

                    xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                               "hotplug event: connector %u's link-state is BAD, "
                               "tried resetting the current mode. You may be left"
                               "with a black screen if this fails...\n",
                               drmmode_output->mode_output->connector_id);
                }
                break;
            }
        }
    }

    mode_res = drmModeGetResources(drmmode->fd);
    if (!mode_res)
        goto out;

    if (mode_res->count_crtcs != config->num_crtc) {
        /* this triggers with Zaphod mode where we don't currently support connector hotplug or MST. */
        goto out_free_res;
    }

    /* figure out if we have gotten rid of any connectors
       traverse old output list looking for outputs */
    for (i = 0; i < config->num_output; i++) {
        xf86OutputPtr output = config->output[i];
        drmmode_output_private_ptr drmmode_output;

        drmmode_output = output->driver_private;
        found = FALSE;
        for (j = 0; j < mode_res->count_connectors; j++) {
            if (mode_res->connectors[j] == drmmode_output->output_id) {
                found = TRUE;
                break;
            }
        }
        if (found)
            continue;

        drmModeFreeConnector(drmmode_output->mode_output);
        drmmode_output->mode_output = NULL;
        drmmode_output->output_id = -1;

        changed = TRUE;
    }

    /* find new output ids we don't have outputs for */
    for (i = 0; i < mode_res->count_connectors; i++) {
        found = FALSE;

        for (j = 0; j < config->num_output; j++) {
            xf86OutputPtr output = config->output[j];
            drmmode_output_private_ptr drmmode_output;

            drmmode_output = output->driver_private;
            if (mode_res->connectors[i] == drmmode_output->output_id) {
                found = TRUE;
                break;
            }
        }
        if (found)
            continue;

        changed = TRUE;
        drmmode_output_init(scrn, drmmode, mode_res, i, TRUE, 0);
    }

    if (changed) {
        RRSetChanged(xf86ScrnToScreen(scrn));
        RRTellChanged(xf86ScrnToScreen(scrn));
    }

out_free_res:

    /* Check to see if a lessee has disappeared */
    drmmode_validate_leases(scrn);

    drmModeFreeResources(mode_res);
out:
    RRGetInfo(xf86ScrnToScreen(scrn), (changed || link_fixed));
}

#undef DRM_MODE_LINK_STATUS_BAD
#undef DRM_MODE_LINK_STATUS_GOOD

#ifdef CONFIG_UDEV_KMS

static void
drmmode_handle_uevents(int fd, void *closure)
{
    drmmode_ptr drmmode = closure;
    struct udev_device *dev;
    Bool found = FALSE;

    while ((dev = udev_monitor_receive_device(drmmode->uevent_monitor))) {
        udev_device_unref(dev);
        found = TRUE;
    }
    if (!found)
        return;

    drmmode_update_kms_state(drmmode);
}

#endif

void
drmmode_uevent_init(ScrnInfoPtr scrn, drmmode_ptr drmmode)
{
#ifdef CONFIG_UDEV_KMS
    struct udev *u;
    struct udev_monitor *mon;

    u = udev_new();
    if (!u)
        return;
    mon = udev_monitor_new_from_netlink(u, "udev");
    if (!mon) {
        udev_unref(u);
        return;
    }

    if (udev_monitor_filter_add_match_subsystem_devtype(mon,
                                                        "drm",
                                                        "drm_minor") < 0 ||
        udev_monitor_enable_receiving(mon) < 0) {
        udev_monitor_unref(mon);
        udev_unref(u);
        return;
    }

    drmmode->uevent_handler =
        xf86AddGeneralHandler(udev_monitor_get_fd(mon),
                              drmmode_handle_uevents, drmmode);

    drmmode->uevent_monitor = mon;
#endif
}

void
drmmode_uevent_fini(ScrnInfoPtr scrn, drmmode_ptr drmmode)
{
#ifdef CONFIG_UDEV_KMS
    if (drmmode->uevent_handler) {
        struct udev *u = udev_monitor_get_udev(drmmode->uevent_monitor);

        xf86RemoveGeneralHandler(drmmode->uevent_handler);

        udev_monitor_unref(drmmode->uevent_monitor);
        udev_unref(u);
    }
#endif
}

Bool
drmmode_get_largest_cursor(ScrnInfoPtr pScrn, drmmode_cursor_dim_ptr cursor_lim)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    int max_width = 0, max_height = 0, i;

    if (!cursor_lim)
        return FALSE;

    for (i = 0; i < xf86_config->num_crtc; i++) {
        xf86CrtcPtr crtc = xf86_config->crtc[i];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
        drmmode_cursor_rec cursor = drmmode_crtc->cursor;

        /* Get the largest cursor available. */
        drmmode_cursor_dim_rec largest = cursor.dimensions[cursor.num_dimensions - 1];

        int width  = largest.width;
        int height = largest.height;

        /* Future work:
         * - We should only let sizes that all CRTCs support. */
        if (width > max_width && height > max_height) {
            max_width  = width;
            max_height = height;
        }
    }

    cursor_lim->width  = max_width;
    cursor_lim->height = max_height;
    return TRUE;
}

/* create front and cursor BOs */
Bool
drmmode_create_initial_bos(ScrnInfoPtr pScrn, drmmode_ptr drmmode)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    int width;
    int height;
    int bpp = ms->drmmode.kbpp;
    int i;
    int cpp = (bpp + 7) / 8;

    width = pScrn->virtualX;
    height = pScrn->virtualY;

    if (!drmmode_create_bo(drmmode, &drmmode->front_bo,
                           width, height, bpp, !drmmode->glamor))
        return FALSE;

    if (drmmode_bo_import(drmmode, &drmmode->front_bo, &drmmode->fb_id) < 0)
        return FALSE;

    pScrn->displayWidth = drmmode_bo_get_pitch(&drmmode->front_bo) / cpp;

    bpp = 32;
    for (i = 0; i < xf86_config->num_crtc; i++) {
        xf86CrtcPtr crtc = xf86_config->crtc[i];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
        drmmode_cursor_rec cursor = drmmode_crtc->cursor;

        /* If we don't have any dimensions then 
         * something has gone terribly wrong. */
        assert(cursor.num_dimensions);

        /* Use the maximum available size. */
        width  = get_maximum_cursor_size(cursor, FALSE);
        height = get_maximum_cursor_size(cursor, TRUE);

        drmmode_crtc->cursor.bo =
            dumb_bo_create(drmmode->fd, width, height, bpp);
    }

    return TRUE;
}

Bool
drmmode_tearfree_alloc_crtc(xf86CrtcPtr crtc)
{
    ScrnInfoPtr scrn = crtc->scrn;
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    ScreenPtr screen = xf86ScrnToScreen(scrn);
    unsigned width  = scrn->virtualX;
    unsigned height = scrn->virtualY;
    int i;

    RegionNull(&drmmode_crtc->tearfree.stale[0]);
    RegionNull(&drmmode_crtc->tearfree.stale[1]);

    for (i = 0; i < 2; i++) {
        if (!drmmode_create_bo(drmmode, &drmmode_crtc->tearfree.bo[i],
                               width, height, drmmode->kbpp, FALSE))
            goto fail;

        if (drmmode_bo_import(drmmode, &drmmode_crtc->tearfree.bo[i],
                              &drmmode_crtc->tearfree.fb_id[i]) < 0) {
            drmmode_bo_destroy(drmmode, &drmmode_crtc->tearfree.bo[i]);
            goto fail;
        }

        drmmode_crtc->tearfree.pixmap[i] =
            drmmode_create_pixmap_header(screen, width, height,
                                         scrn->depth, scrn->bitsPerPixel,
                                         drmmode_bo_get_pitch(&drmmode_crtc->tearfree.bo[i]),
                                         NULL);
        if (!drmmode_crtc->tearfree.pixmap[i])
            goto fail;

        if (!drmmode_set_pixmap_bo(drmmode, drmmode_crtc->tearfree.pixmap[i],
                                   &drmmode_crtc->tearfree.bo[i]))
            goto fail;
    }

    drmmode_crtc->tearfree.back_idx     = 0;
    drmmode_crtc->tearfree.flip_seq     = 0;

    /*
     * Mark both buffers as fully stale so the first blit to each
     * populates the entire CRTC viewport rather than just the
     * first damaged region.
     */
    BoxRec full_viewport = { .x1 = 0, .y1 = 0, .x2 = width, .y2 = height };

    RegionInitBoxes(&drmmode_crtc->tearfree.stale[0], &full_viewport, 1);
    RegionInitBoxes(&drmmode_crtc->tearfree.stale[1], &full_viewport, 1);

    return TRUE;

fail:
    while (--i >= 0) {
        if (drmmode_crtc->tearfree.pixmap[i]) {
            screen->DestroyPixmap(drmmode_crtc->tearfree.pixmap[i]);
            drmmode_crtc->tearfree.pixmap[i] = NULL;
        }
        if (drmmode_crtc->tearfree.fb_id[i]) {
            drmModeRmFB(drmmode->fd, drmmode_crtc->tearfree.fb_id[i]);
            drmmode_crtc->tearfree.fb_id[i] = 0;
        }
        drmmode_bo_destroy(drmmode, &drmmode_crtc->tearfree.bo[i]);
    }

    return FALSE;
}

void
drmmode_tearfree_free_crtc(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;
    ScreenPtr screen = xf86ScrnToScreen(crtc->scrn);
    int i;

    if (!drmmode_crtc->tearfree.fb_id[0] &&
        !drmmode_crtc->tearfree.fb_id[1])
        return;

    RegionUninit(&drmmode_crtc->tearfree.stale[0]);
    RegionUninit(&drmmode_crtc->tearfree.stale[1]);

    for (i = 0; i < 2; i++) {
        if (drmmode_crtc->tearfree.pixmap[i]) {
            screen->DestroyPixmap(drmmode_crtc->tearfree.pixmap[i]);
            drmmode_crtc->tearfree.pixmap[i] = NULL;
        }
        if (drmmode_crtc->tearfree.fb_id[i]) {
            drmModeRmFB(drmmode->fd, drmmode_crtc->tearfree.fb_id[i]);
            drmmode_crtc->tearfree.fb_id[i] = 0;
        }
        drmmode_bo_destroy(drmmode, &drmmode_crtc->tearfree.bo[i]);
    }
}

static void
drmmode_tearfree_realloc_crtc(xf86CrtcPtr crtc)
{
    drmmode_tearfree_free_crtc(crtc);
    if (!drmmode_tearfree_alloc_crtc(crtc))
        xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
                   "TearFree reallocation failed; "
                   "TearFree inactive on this CRTC until next resize\n");
}

void *
drmmode_map_front_bo(drmmode_ptr drmmode)
{
    return drmmode_bo_map(drmmode, &drmmode->front_bo);
}

void *
drmmode_map_secondary_bo(drmmode_ptr drmmode, msPixmapPrivPtr ppriv)
{
    int ret;

    if (ppriv->backing_bo->ptr)
        return ppriv->backing_bo->ptr;

    ret = dumb_bo_map(drmmode->fd, ppriv->backing_bo);
    if (ret)
        return NULL;

    return ppriv->backing_bo->ptr;
}

Bool
drmmode_map_cursor_bos(ScrnInfoPtr pScrn, drmmode_ptr drmmode)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    int i, ret;

    for (i = 0; i < xf86_config->num_crtc; i++) {
        xf86CrtcPtr crtc = xf86_config->crtc[i];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

        ret = dumb_bo_map(drmmode->fd, drmmode_crtc->cursor.bo);
        if (ret)
            return FALSE;
    }
    return TRUE;
}

void
drmmode_free_bos(ScrnInfoPtr pScrn, drmmode_ptr drmmode)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    int i;

    if (drmmode->fb_id) {
        drmModeRmFB(drmmode->fd, drmmode->fb_id);
        drmmode->fb_id = 0;
    }

    drmmode_bo_destroy(drmmode, &drmmode->front_bo);

    for (i = 0; i < xf86_config->num_crtc; i++) {
        xf86CrtcPtr crtc = xf86_config->crtc[i];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

        /* Release the atomic cursor plane framebuffer if one was registered. */
        if (drmmode_crtc->cursor.fb_id) {
            drmModeRmFB(drmmode->fd, drmmode_crtc->cursor.fb_id);
            drmmode_crtc->cursor.fb_id = 0;
        }

        dumb_bo_destroy(drmmode->fd, drmmode_crtc->cursor.bo);

        drmmode_tearfree_free_crtc(crtc);
    }
}

/* ugly workaround to see if we can create 32bpp */
void
drmmode_get_default_bpp(ScrnInfoPtr pScrn, drmmode_ptr drmmode, int *depth,
                        int *bpp)
{
    drmModeResPtr mode_res;
    uint64_t value;
    struct dumb_bo *bo;
    uint32_t fb_id;
    int ret;

    /* 16 is fine */
    ret = drmGetCap(drmmode->fd, DRM_CAP_DUMB_PREFERRED_DEPTH, &value);
    if (!ret && (value == 16 || value == 8)) {
        *depth = value;
        *bpp = value;
        return;
    }

    *depth = 24;
    mode_res = drmModeGetResources(drmmode->fd);
    if (!mode_res)
        return;

    if (mode_res->min_width == 0)
        mode_res->min_width = 1;
    if (mode_res->min_height == 0)
        mode_res->min_height = 1;

    /* create a bo */
    bo = dumb_bo_create(drmmode->fd, mode_res->min_width, mode_res->min_height, 32);
    if (!bo) {
        *bpp = 24;
        goto out;
    }

    ret = drmModeAddDumbFB(drmmode, bo, &fb_id,
                           mode_res->min_width, mode_res->min_height, DRM_FORMAT_XRGB8888);
    if (ret) {
        *bpp = 24;
        dumb_bo_destroy(drmmode->fd, bo);
        goto out;
    }

    drmModeRmFB(drmmode->fd, fb_id);
    *bpp = 32;

    dumb_bo_destroy(drmmode->fd, bo);
 out:
    drmModeFreeResources(mode_res);
    return;
}

void
drmmode_crtc_set_vrr(xf86CrtcPtr crtc, Bool enabled)
{
    modesettingPtr ms = modesettingPTR(crtc->scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    if (drmmode_crtc->vrr_enabled == enabled)
        return;

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req)
        return;

    if (crtc_add_vrr_prop(req, drmmode_crtc, enabled) == 0 &&
        drmModeAtomicCommit(ms->fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL) == 0)
        drmmode_crtc->vrr_enabled = enabled;

    drmModeAtomicFree(req);
}

/*
 * We hook the screen's cursor-sprite (swcursor) functions to see if a swcursor
 * is active. When a swcursor is active we disable page-flipping.
 */

static inline msSpritePrivPtr
msGetSpritePriv(DeviceIntPtr pDev, modesettingPtr ms, ScreenPtr pScreen)
{
    if (!IsFloating(pDev))
        pDev = GetMaster(pDev, MASTER_POINTER);
    return dixLookupScreenPrivate(&(pDev)->devPrivates,
                                  &(ms)->drmmode.spritePrivateKeyRec,
                                  pScreen);
}

static void drmmode_sprite_do_set_cursor(msSpritePrivPtr sprite_priv,
                                         ScrnInfoPtr scrn, int x, int y)
{
    modesettingPtr ms = modesettingPTR(scrn);
    CursorPtr cursor = sprite_priv->cursor;
    Bool sprite_visible = sprite_priv->sprite_visible;

    if (cursor) {
        x -= cursor->bits->xhot;
        y -= cursor->bits->yhot;

        sprite_priv->sprite_visible =
            x < scrn->virtualX && y < scrn->virtualY &&
            (x + cursor->bits->width > 0) &&
            (y + cursor->bits->height > 0);
    } else {
        sprite_priv->sprite_visible = FALSE;
    }

    ms->drmmode.sprites_visible += sprite_priv->sprite_visible - sprite_visible;
}

static void drmmode_sprite_set_cursor(DeviceIntPtr pDev, ScreenPtr pScreen,
                                      CursorPtr pCursor, int x, int y)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(scrn);
    msSpritePrivPtr sprite_priv = msGetSpritePriv(pDev, ms, pScreen);

    sprite_priv->cursor = pCursor;
    drmmode_sprite_do_set_cursor(sprite_priv, scrn, x, y);

    ms->SpriteFuncs->SetCursor(pDev, pScreen, pCursor, x, y);
}

static void drmmode_sprite_move_cursor(DeviceIntPtr pDev, ScreenPtr pScreen,
                                       int x, int y)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(scrn);
    msSpritePrivPtr sprite_priv = msGetSpritePriv(pDev, ms, pScreen);

    drmmode_sprite_do_set_cursor(sprite_priv, scrn, x, y);

    ms->SpriteFuncs->MoveCursor(pDev, pScreen, x, y);
}

static Bool drmmode_sprite_realize_realize_cursor(DeviceIntPtr pDev,
                                                  ScreenPtr pScreen,
                                                  CursorPtr pCursor)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(scrn);

    return ms->SpriteFuncs->RealizeCursor(pDev, pScreen, pCursor);
}

static Bool drmmode_sprite_realize_unrealize_cursor(DeviceIntPtr pDev,
                                                    ScreenPtr pScreen,
                                                    CursorPtr pCursor)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(scrn);

    return ms->SpriteFuncs->UnrealizeCursor(pDev, pScreen, pCursor);
}

static Bool drmmode_sprite_device_cursor_initialize(DeviceIntPtr pDev,
                                                    ScreenPtr pScreen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(scrn);

    return ms->SpriteFuncs->DeviceCursorInitialize(pDev, pScreen);
}

static void drmmode_sprite_device_cursor_cleanup(DeviceIntPtr pDev,
                                                 ScreenPtr pScreen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(scrn);

    ms->SpriteFuncs->DeviceCursorCleanup(pDev, pScreen);
}

miPointerSpriteFuncRec drmmode_sprite_funcs = {
    .RealizeCursor = drmmode_sprite_realize_realize_cursor,
    .UnrealizeCursor = drmmode_sprite_realize_unrealize_cursor,
    .SetCursor = drmmode_sprite_set_cursor,
    .MoveCursor = drmmode_sprite_move_cursor,
    .DeviceCursorInitialize = drmmode_sprite_device_cursor_initialize,
    .DeviceCursorCleanup = drmmode_sprite_device_cursor_cleanup,
};
