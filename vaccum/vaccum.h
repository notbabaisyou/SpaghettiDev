#ifndef VACCUM_H
#define VACCUM_H

#include <gbm.h>
#include <scrnintstr.h>
#include <pixmapstr.h>
#include <gcstruct.h>
#include <picturestr.h>
#include <fb.h>
#include <fbpict.h>
#include <xf86str.h>

/*
 * glamor_pixmap_type : glamor pixmap's type.
 * @MEMORY: pixmap is in memory.
 * @TEXTURE_DRM: pixmap is in a texture created from a DRM buffer.
 * @SEPARATE_TEXTURE: The texture is created from a DRM buffer, but
 * 		      the format is incompatible, so this type of pixmap
 * 		      will never fallback to DDX layer.
 * @DRM_ONLY: pixmap is in a external DRM buffer.
 * @TEXTURE_ONLY: pixmap is in an internal texture.
 */
typedef enum vaccum_pixmap_type {
    VACCUM_MEMORY = 0, /* Newly calloc()ed pixmaps are memory. */
    //    VACCUM_TEXTURE_DRM,
    //    VACCUM_DRM_ONLY,
    VACCUM_IMAGE_ONLY,
} vaccum_pixmap_type_t;

extern _X_EXPORT Bool vaccum_egl_init(ScrnInfoPtr scrn, int drm_fd);

extern _X_EXPORT Bool vaccum_init(ScreenPtr screen, unsigned int flags);

extern _X_EXPORT void vaccum_fini(ScreenPtr screen);

extern _X_EXPORT PixmapPtr vaccum_create_pixmap(ScreenPtr screen, int w, int h,
                                                int depth, unsigned int usage);

extern _X_EXPORT Bool vaccum_destroy_pixmap(PixmapPtr pixmap);

#define VACCUM_CREATE_PIXMAP_CPU        0x100
#define VACCUM_CREATE_PIXMAP_FIXUP      0x101
#define VACCUM_CREATE_FBO_NO_FBO        0x103
/* Deprecated - we don't perform nightmares here... */
#define VACCUM_CREATE_NO_LARGE          0x105
#define VACCUM_CREATE_PIXMAP_NO_TEXTURE 0x106
#define VACCUM_CREATE_FORMAT_CBCR       0x107

extern _X_EXPORT Bool vaccum_close_screen(ScreenPtr screen);

extern _X_EXPORT int vaccum_create_gc(GCPtr gc);

extern _X_EXPORT void vaccum_validate_gc(GCPtr gc, unsigned long changes,
                                         DrawablePtr drawable);

extern _X_EXPORT void vaccum_destroy_gc(GCPtr gc);

extern _X_EXPORT void vaccum_finish(ScreenPtr screen);

typedef Bool (*GetDrawableModifiersFuncPtr)(DrawablePtr draw,
                                            uint32_t format,
                                            uint32_t *num_modifiers,
                                            uint64_t **modifiers);

extern _X_EXPORT void vaccum_block_handler(ScreenPtr screen);

extern _X_EXPORT void vaccum_clear_pixmap(PixmapPtr pixmap);
extern _X_EXPORT void vaccum_exchange_buffers(PixmapPtr front, PixmapPtr back);

extern _X_EXPORT Bool vaccum_supports_pixmap_import_export(ScreenPtr screen);

extern _X_EXPORT void vaccum_set_drawable_modifiers_func(ScreenPtr screen,
                                                         GetDrawableModifiersFuncPtr func);

extern _X_EXPORT int vaccum_name_from_pixmap(PixmapPtr pixmap,
                                             CARD16 *stride, CARD32 *size);

extern _X_EXPORT int vaccum_shareable_fd_from_pixmap(ScreenPtr screen,
                                                     PixmapPtr pixmap,
                                                     CARD16 *stride,
                                                     CARD32 *size);

extern _X_EXPORT PixmapPtr vaccum_pixmap_from_fds(ScreenPtr screen,
                                                  CARD8 num_fds,
                                                  const int *fds,
                                                  CARD16 width,
                                                  CARD16 height,
                                                  const CARD32 *strides,
                                                  const CARD32 *offsets,
                                                  CARD8 depth,
                                                  CARD8 bpp,
                                                  uint64_t modifier);

extern _X_EXPORT Bool vaccum_back_pixmap_from_fd(PixmapPtr pixmap,
                                                 int fd,
                                                 CARD16 width,
                                                 CARD16 height,
                                                 CARD16 stride,
                                                 CARD8 depth,
                                                 CARD8 bpp);

extern _X_EXPORT int vaccum_fds_from_pixmap(ScreenPtr screen,
                                            PixmapPtr pixmap,
                                            int *fds,
                                            uint32_t *strides,
                                            uint32_t *offsets,
                                            uint64_t *modifier);

extern _X_EXPORT int vaccum_fd_from_pixmap(ScreenPtr screen,
                                           PixmapPtr pixmap,
                                           CARD16 *stride, CARD32 *size);

extern _X_EXPORT PixmapPtr vaccum_pixmap_from_fd(ScreenPtr screen,
                                                 int fd,
                                                 CARD16 width,
                                                 CARD16 height,
                                                 CARD16 stride,
                                                 CARD8 depth,
                                                 CARD8 bpp);

extern _X_EXPORT Bool vaccum_get_formats(ScreenPtr screen,
                                         CARD32 *num_formats,
                                         CARD32 **formats);

extern _X_EXPORT Bool vaccum_get_modifiers(ScreenPtr screen,
                                           uint32_t format,
                                           uint32_t *num_modifiers,
                                           uint64_t **modifiers);

extern _X_EXPORT Bool vaccum_get_drawable_modifiers(DrawablePtr draw,
                                                    uint32_t format,
                                                    uint32_t *num_modifiers,
                                                    uint64_t **modifiers);

extern _X_EXPORT struct gbm_device *vaccum_egl_get_gbm_device(ScreenPtr screen);

extern _X_EXPORT struct gbm_bo *vaccum_gbm_bo_from_pixmap(ScreenPtr screen, PixmapPtr pixmap);

extern _X_EXPORT Bool vaccum_egl_create_textured_pixmap_from_gbm_bo(PixmapPtr pixmap,
                                                                    struct gbm_bo *bo, Bool used_modifiers);
#endif
