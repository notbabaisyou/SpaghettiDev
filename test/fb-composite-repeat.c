/* SPDX-License-Identifier: MIT */
#undef NDEBUG
#include "dix-config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../fb/fbpict.c"

enum {
    TEST_CASES = 100000,
    TEST_MAX_SOURCE_HEIGHT = 17,
    TEST_MAX_DEST_WIDTH = 257,
    TEST_MAX_DEST_HEIGHT = 99,
    TEST_MAX_WIDTH = 300,
    TEST_MAX_HEIGHT = 120,
    TEST_RANDOM_SEED = 0x137a55,
};

BoxRec RegionEmptyBox;
RegDataRec RegionEmptyData, RegionBrokenData;
static uint32_t random_state = TEST_RANDOM_SEED;
static ScreenRec screen;
static PictFormatRec format = { .format = PICT_a1 };

DevPrivateKey
fbGetScreenPrivateKey(void)
{
    static DevPrivateKeyRec key;

    return &key;
}

static uint32_t
random_word(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void
validate_source(DrawablePtr drawable, int x, int y, int width, int height,
                unsigned int mode)
{
    assert(drawable->type == DRAWABLE_PIXMAP);
    assert(mode == IncludeInferiors || mode == ClipByChildren);
}

static void
init_pixmap(PixmapPtr pixmap, int width, int height)
{
    *pixmap = (PixmapRec) {
        .drawable = { .type = DRAWABLE_PIXMAP, .depth = 1, .bitsPerPixel = 1,
                      .width = width, .height = height, .pScreen = &screen },
        .devKind = ((width + FB_MASK) >> FB_SHIFT) * sizeof(FbBits),
    };
    pixmap->devPrivate.ptr = malloc((size_t) pixmap->devKind * height);
    assert(pixmap->devPrivate.ptr);
    for (int i = 0; i < pixmap->devKind * height / sizeof(FbBits); i++)
        ((FbBits *) pixmap->devPrivate.ptr)[i] = random_word();
}

static void
init_picture(PicturePtr picture, PixmapPtr pixmap, RegionPtr clip)
{
    *picture = (PictureRec) {
        .pDrawable = &pixmap->drawable, .pFormat = &format, .format = PICT_a1,
        .pCompositeClip = clip, .repeatType = RepeatNormal,
        .filter = PictFilterNearest,
    };
}

static void
reference_composite(CARD8 op, PicturePtr src, PicturePtr dst,
                    int sx, int sy, int dx, int dy, int width, int height)
{
    int src_xoff, src_yoff, dst_xoff, dst_yoff;
    pixman_image_t *src_image, *dst_image;

    miCompositeSourceValidate(src);
    src_image = image_from_pict(src, FALSE, &src_xoff, &src_yoff);
    dst_image = image_from_pict(dst, TRUE, &dst_xoff, &dst_yoff);
    assert(src_image && dst_image);
    pixman_image_composite(op, src_image, NULL, dst_image,
                          sx + src_xoff, sy + src_yoff, 0, 0,
                          dx + dst_xoff, dy + dst_yoff, width, height);
    free_pixman_pict(src, src_image);
    free_pixman_pict(dst, dst_image);
}

static void
compare_composites(void)
{
    static const int tile_widths[] = {
        1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129,
    };
    static const CARD8 ops[] = { PictOpSrc, PictOpOver, PictOpAdd };

    for (unsigned trial = 0; trial < TEST_CASES; trial++) {
        PixmapRec source, actual, expected;
        PictureRec src, dst, ref;
        int sw = tile_widths[trial % ARRAY_SIZE(tile_widths)];
        int sh = 1 + random_word() % TEST_MAX_SOURCE_HEIGHT;
        int dw = 1 + random_word() % TEST_MAX_DEST_WIDTH;
        int dh = 1 + random_word() % TEST_MAX_DEST_HEIGHT;
        int dx = (int) (random_word() % (dw + 30)) - 15;
        int dy = (int) (random_word() % (dh + 20)) - 10;
        int sx = (int16_t) random_word(), sy = (int16_t) random_word();
        int width = random_word() % TEST_MAX_WIDTH;
        int height = random_word() % TEST_MAX_HEIGHT;
        CARD8 op = ops[trial % ARRAY_SIZE(ops)];
        unsigned char *source_before;
        BoxRec bounds = { 0, 0, dw, dh };
        RegionRec clip, source_clip;

        RegionInit(&clip, &bounds, 1);
        if (trial % 2) {
            BoxRec hole = { dw / 3, dh / 3, dw * 2 / 3, dh * 2 / 3 };
            RegionRec cut;

            RegionInit(&cut, &hole, 1);
            RegionSubtract(&clip, &clip, &cut);
            RegionUninit(&cut);
        }
        init_pixmap(&source, sw, sh);
        init_pixmap(&actual, dw, dh);
        init_pixmap(&expected, dw, dh);
        memcpy(expected.devPrivate.ptr, actual.devPrivate.ptr, actual.devKind * dh);
        source_before = malloc(source.devKind * sh);
        assert(source_before);
        memcpy(source_before, source.devPrivate.ptr, source.devKind * sh);
        init_picture(&src, &source, NULL);
        init_picture(&dst, &actual, &clip);
        init_picture(&ref, &expected, &clip);
        src.filter = trial % 3 == 0 ? PictFilterBilinear : PictFilterNearest;
        bounds = (BoxRec) { 0, 0, sw, sh };
        RegionInit(&source_clip, &bounds, 1);
        if (trial % 4 == 3) {
            src.clientClip = &source_clip;
            src.clipOrigin.x = 3;
            src.clipOrigin.y = -2;
        }

        reference_composite(op, &src, &ref, sx, sy, dx, dy, width, height);
        fbComposite(op, &src, NULL, &dst, sx, sy, 0, 0, dx, dy, width, height);
        if (memcmp(actual.devPrivate.ptr, expected.devPrivate.ptr, actual.devKind * dh)) {
            fprintf(stderr, "trial %u: op %u, tile %dx%d, source %d,%d, "
                    "destination %d,%d, size %dx%d\n",
                    trial, op, sw, sh, sx, sy, dx, dy, width, height);
            abort();
        }
        /* Padding may be replicated for fbEvenTile; source pixels must survive. */
        for (int row = 0; row < sh; row++) {
            for (int bit = 0; bit < sw; bit++) {
                int byte = row * source.devKind + bit / 8;
                unsigned char changed = source_before[byte] ^
                    ((unsigned char *) source.devPrivate.ptr)[byte];

#if BITMAP_BIT_ORDER == LSBFirst
                assert(!(changed & (1 << (bit % 8))));
#else
                assert(!(changed & (128 >> (bit % 8))));
#endif
            }
        }
        free(source_before);
        RegionUninit(&clip);
        RegionUninit(&source_clip);
        free(source.devPrivate.ptr);
        free(actual.devPrivate.ptr);
        free(expected.devPrivate.ptr);
    }
}

static void
check_shared_storage(void)
{
    PixmapRec source, destination;
    PictureRec src, dst;
    BoxRec box = { 0, 0, 8, 2 };
    RegionRec clip;

    RegionInit(&clip, &box, 1);
    init_pixmap(&source, box.x2, box.y2);
    init_picture(&src, &source, NULL);
    destination = source;
    init_picture(&dst, &destination, &clip);
    assert(!fbCompositeRepeatedA1(PictOpOver, &src, NULL, &dst, 0, 0, 0, 0, 8, 2));
    destination.devPrivate.ptr = (char *) source.devPrivate.ptr + sizeof(FbBits);
    assert(!fbCompositeRepeatedA1(PictOpOver, &src, NULL, &dst, 0, 0, 0, 0, 8, 2));
    free(source.devPrivate.ptr);
    RegionUninit(&clip);
}

int
main(void)
{
    screen.SourceValidate = validate_source;
    check_shared_storage();
    compare_composites();
    printf("%u repeated A1 composites matched Pixman\n", TEST_CASES);
    return 0;
}
