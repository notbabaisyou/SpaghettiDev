#ifndef VACCUM_H
#define VACCUM_H

#include <scrnintstr.h>
#include <pixmapstr.h>
#include <gcstruct.h>
#include <picturestr.h>
#include <fb.h>
#include <fbpict.h>

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

extern _X_EXPORT Bool vaccum_init(ScreenPtr screen, unsigned int flags);
extern _X_EXPORT void vaccum_fini(ScreenPtr screen);

extern _X_EXPORT PixmapPtr vaccum_create_pixmap(ScreenPtr screen, int w, int h,
                                                int depth, unsigned int usage);
extern _X_EXPORT Bool vaccum_destroy_pixmap(PixmapPtr pixmap);

#define VACCUM_CREATE_PIXMAP_CPU        0x100
#define VACCUM_CREATE_PIXMAP_FIXUP      0x101
#define VACCUM_CREATE_FBO_NO_FBO        0x103
#define VACCUM_CREATE_NO_LARGE          0x105
#define VACCUM_CREATE_PIXMAP_NO_TEXTURE 0x106
#define VACCUM_CREATE_FORMAT_CBCR       0x107

extern _X_EXPORT Bool vaccum_close_screen(ScreenPtr screen);

extern _X_EXPORT int vaccum_create_gc(GCPtr gc);

extern _X_EXPORT void vaccum_validate_gc(GCPtr gc, unsigned long changes,
                                         DrawablePtr drawable);

extern _X_EXPORT void vaccum_destroy_gc(GCPtr gc);

extern _X_EXPORT void vaccum_finish(ScreenPtr screen);
#endif
