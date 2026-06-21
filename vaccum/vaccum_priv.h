#ifndef VACCUM_PRIV_H
#define VACCUM_PRIV_H

#include "dix-config.h"

#include "vaccum.h"
#include "vaccum_debug.h"

#include "vulkan/vulkan.h"
#if XSYNC
#include "misyncshm.h"
#include "misyncstr.h"
#endif

struct vaccum_pixmap_private;

struct vaccum_image {
    VkDeviceMemory memory;
    VkImage image;
    VkImageView image_view;
    int width, height;
};

struct vaccum_buffer {
    VkDeviceMemory memory;
    VkBuffer buffer;
    uint32_t size;
};

struct vaccum_format {
    /** X Server's "depth" value */
    int depth;
    VkFormat format;
    /* Render PICT_* matching GL's channel layout for pixels
     * transferred using format/type.
     */
    CARD32 render_format;

    VkFormatProperties format_props;
    VkImageFormatProperties linear_format_props;
    VkImageFormatProperties tiled_format_props;
};

struct vaccum_saved_procs {
    CloseScreenProcPtr close_screen;
    CreateGCProcPtr create_gc;
    CreatePixmapProcPtr create_pixmap;
    DestroyPixmapProcPtr destroy_pixmap;
    GetSpansProcPtr get_spans;
    GetImageProcPtr get_image;
    CompositeProcPtr composite;
    CompositeRectsProcPtr composite_rects;
    TrapezoidsProcPtr trapezoids;
    GlyphsProcPtr glyphs;
    ChangeWindowAttributesProcPtr change_window_attributes;
    CopyWindowProcPtr copy_window;
    BitmapToRegionProcPtr bitmap_to_region;
    TrianglesProcPtr triangles;
    AddTrapsProcPtr addtraps;
#if XSYNC
    SyncScreenFuncsRec sync_screen_funcs;
#endif
    ScreenBlockHandlerProcPtr block_handler;
};

typedef struct vaccum_screen_private {
    struct vaccum_saved_procs saved_procs;
    ScreenPtr screen;

    VkInstance instance;
    VkPhysicalDevice *phys_devices;
    VkPhysicalDevice phys_device;

    VkPhysicalDeviceProperties dev_properties;
    VkPhysicalDeviceFeatures dev_features;
    VkPhysicalDeviceMemoryProperties dev_mem_properties;

    uint32_t num_queues;
    VkQueueFamilyProperties *queue_families;

    VkDevice device;
    VkQueue queue;

    int max_fbo_size;
    int glyph_max_dim;
    struct vaccum_format formats[33];

    VkCommandPool command_pool;
    VkCommandBuffer current_cmd;
} vaccum_screen_private;

typedef enum vaccum_access {
    VACCUM_ACCESS_RO,
    VACCUM_ACCESS_RW,
    VACCUM_ACCESS_WO,
} vaccum_access_t;

enum vaccum_image_state {
    /** There is no storage attached to the pixmap. */
    VACCUM_IMAGE_UNATTACHED,
    /**
     * The pixmap has IMAGE storage attached, but devPrivate.ptr doesn't
     * point at anything.
     */
    VACCUM_IMAGE_NORMAL,
};

typedef struct vaccum_pixmap_private {

    vaccum_pixmap_type_t type;
    enum vaccum_image_state vk_image;
    struct vaccum_image *image;

    vaccum_access_t map_access;
    BoxRec box;

    RegionRec prepare_region;
    Bool prepared;
} vaccum_pixmap_private;

extern DevPrivateKeyRec vaccum_pixmap_private_key;

static inline vaccum_pixmap_private *
vaccum_get_pixmap_private(PixmapPtr pixmap)
{
    if (pixmap == NULL)
        return NULL;

    return dixLookupPrivate(&pixmap->devPrivates, &vaccum_pixmap_private_key);
}

typedef struct {
    PixmapPtr   dash;
    PixmapPtr   stipple;
    DamagePtr   stipple_damage;
} vaccum_gc_private;

extern DevPrivateKeyRec vaccum_gc_private_key;
extern DevPrivateKeyRec vaccum_screen_private_key;

extern vaccum_screen_private *
vaccum_get_screen_private(ScreenPtr screen);

extern void
vaccum_set_screen_private(ScreenPtr screen, vaccum_screen_private *priv);

static inline vaccum_gc_private *
vaccum_get_gc_private(GCPtr gc)
{
    return dixLookupPrivate(&gc->devPrivates, &vaccum_gc_private_key);
}

/**
 * Returns TRUE if the given planemask covers all the significant bits in the
 * pixel values for pDrawable.
 */
static inline Bool
vaccum_pm_is_solid(int depth, unsigned long planemask)
{
    return (planemask & FbFullMask(depth)) ==
        FbFullMask(depth);
}

extern int vaccum_debug_level;

PixmapPtr vaccum_get_drawable_pixmap(DrawablePtr drawable);

struct vaccum_image *vaccum_pixmap_detach_image(vaccum_pixmap_private *
                                              pixmap_priv);
void vaccum_pixmap_attach_image(PixmapPtr pixmap, struct vaccum_image *image);
Bool vaccum_get_drawable_location(const DrawablePtr drawable);
static inline void
vaccum_get_drawable_deltas(DrawablePtr drawable, PixmapPtr pixmap,
                           int *x, int *y)
{
#ifdef COMPOSITE
    if (drawable->type == DRAWABLE_WINDOW) {
        *x = -pixmap->screen_x;
        *y = -pixmap->screen_y;
        return;
    }
#endif

    *x = 0;
    *y = 0;
}

const struct vaccum_format *
vaccum_format_for_pixmap(PixmapPtr pixmap);
Bool vaccum_vulkan_init(struct vaccum_screen_private *vaccum_priv);
void vaccum_vulkan_fini(struct vaccum_screen_private *vaccum_priv);
void vaccum_setup_formats(struct vaccum_screen_private *vaccum_priv);

struct vaccum_image *vaccum_create_image(struct vaccum_screen_private *vaccum_priv, PixmapPtr pixmap,
                                         int w, int h);
void vaccum_destroy_image(struct vaccum_screen_private *vaccum_priv, struct vaccum_image *image);

void vaccum_alloc_cmd_buffer(struct vaccum_screen_private *screen_priv);
void vaccum_flush_cmds(struct vaccum_screen_private *screen_priv);

/* vaccum_text.c */
int vaccum_poly_text8(DrawablePtr pDrawable, GCPtr pGC,
                      int x, int y, int count, char *chars);

int vaccum_poly_text16(DrawablePtr pDrawable, GCPtr pGC,
                       int x, int y, int count, unsigned short *chars);

void vaccum_image_text8(DrawablePtr pDrawable, GCPtr pGC,
                        int x, int y, int count, char *chars);

void vaccum_image_text16(DrawablePtr pDrawable, GCPtr pGC,
                         int x, int y, int count, unsigned short *chars);

/* vaccum_spans.c */
void
vaccum_fill_spans(DrawablePtr drawable,
                  GCPtr gc,
                  int n, DDXPointPtr points, int *widths, int sorted);
void
vaccum_get_spans(DrawablePtr drawable, int wmax,
                 DDXPointPtr points, int *widths, int count, char *dst);

void
vaccum_set_spans(DrawablePtr drawable, GCPtr gc, char *src,
                 DDXPointPtr points, int *widths, int numPoints, int sorted);

/* vaccum_ximage.c */
void
vaccum_put_image(DrawablePtr drawable, GCPtr gc, int depth, int x, int y,
                 int w, int h, int leftPad, int format, char *bits);

/*  vaccum_segs.c */
void
vaccum_poly_segment(DrawablePtr drawable, GCPtr gc,
                    int nseg, xSegment *segs);

void
vaccum_poly_point(DrawablePtr pDrawable, GCPtr pGC, int mode, int npt,
                  DDXPointPtr ppt);

/* vaccum_copy.c */
void
vaccum_copy_window(WindowPtr window, DDXPointRec old_origin, RegionPtr src_region);
RegionPtr
vaccum_copy_area(DrawablePtr src, DrawablePtr dst, GCPtr gc,
                 int srcx, int srcy, int width, int height, int dstx, int dsty);
RegionPtr
vaccum_copy_plane(DrawablePtr src, DrawablePtr dst, GCPtr gc,
                  int srcx, int srcy, int width, int height, int dstx, int dsty,
                  unsigned long bitplane);

void
vaccum_poly_fill_rect(DrawablePtr drawable,
                      GCPtr gc, int nrect, xRectangle *prect);

void
vaccum_poly_lines(DrawablePtr drawable, GCPtr gc,
                  int mode, int n, DDXPointPtr points);
void
vaccum_poly_glyph_blt(DrawablePtr drawable, GCPtr gc,
                      int start_x, int y, unsigned int nglyph,
                      CharInfoPtr *ppci, void *pglyph_base);

void
vaccum_push_pixels(GCPtr pGC, PixmapPtr pBitmap,
                   DrawablePtr pDrawable, int w, int h, int x, int y);
#include "vaccum_utils.h"
#endif
