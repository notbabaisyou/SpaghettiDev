/* SPDX-License-Identifier: MIT */

#undef NDEBUG

#include <dix-config.h>
#include <assert.h>
#include <fcntl.h>
#include <picturestr.h>
#include <pixmapstr.h>
#include <unistd.h>

#include "dmabuf.h"
#include "tests-common.h"

#define TEST_FOURCC UINT32_C(0x34324241) /* AB24 */
#define TEST_MODIFIER UINT64_C(0x0100000000000001) /* Intel X tiled */

enum {
    TEST_WIDTH = 16,
    TEST_HEIGHT = 8,
    TEST_STRIDE = TEST_WIDTH * 4,
    TEST_OFFSET = TEST_STRIDE,
    TEST_DEPTH = 32,
};

static PictFormatRec render_format = { .depth = TEST_DEPTH };
static PixmapRec imported_pixmap;
static Bool destroyed;

static Bool
destroy_pixmap(PixmapPtr pixmap)
{
    assert(pixmap == &imported_pixmap);
    destroyed = TRUE;
    return TRUE;
}

static int
get_modifiers(ScreenPtr screen, uint32_t fourcc, uint32_t *count,
              uint64_t **modifiers)
{
    assert(screen == imported_pixmap.drawable.pScreen);
    if (fourcc != TEST_FOURCC)
        return BadMatch;

    *modifiers = malloc(sizeof(**modifiers));
    assert(*modifiers);
    **modifiers = TEST_MODIFIER;
    *count = 1;
    return Success;
}

static int
import_dmabuf(ScreenPtr screen, const struct dmabuf_buffer *buffer,
              PixmapPtr *pixmap, PictFormatPtr *format)
{
    assert(screen == imported_pixmap.drawable.pScreen);
    if (buffer->fourcc != TEST_FOURCC || buffer->modifier != TEST_MODIFIER)
        return BadMatch;

    assert(buffer->width == TEST_WIDTH && buffer->height == TEST_HEIGHT);
    assert(buffer->num_planes == 1 && fcntl(buffer->fds[0], F_GETFD) >= 0);
    assert(buffer->strides[0] == TEST_STRIDE && buffer->offsets[0] == TEST_OFFSET);
    *pixmap = &imported_pixmap;
    *format = &render_format;
    return Success;
}

static void
check_dmabuf_backend(void)
{
    ScreenRec screen = { .DestroyPixmap = destroy_pixmap }, other = { 0 };
    const struct dmabuf_backend backend = {
        .get_modifiers = get_modifiers,
        .import_buffer = import_dmabuf,
    };
    const struct dmabuf_backend alternate = backend;
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    uint32_t stride = TEST_STRIDE, offset = TEST_OFFSET;
    struct dmabuf_buffer buffer = {
        .fourcc = TEST_FOURCC,
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .modifier = TEST_MODIFIER,
        .num_planes = 1,
        .fds = &fd,
        .strides = &stride,
        .offsets = &offset,
    };
    PictFormatPtr format;
    PixmapPtr pixmap;
    uint32_t count;
    uint64_t *modifiers;

    assert(fd >= 0);
    assert(dixAllocatePrivates(&screen.devPrivates, PRIVATE_SCREEN));
    assert(dixAllocatePrivates(&other.devPrivates, PRIVATE_SCREEN));
    screenInfo.numScreens = 2;
    screenInfo.screens[0] = &screen;
    screenInfo.screens[1] = &other;
    imported_pixmap.drawable = (DrawableRec){
        .pScreen = &screen,
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .depth = TEST_DEPTH,
    };

    assert(dmabuf_get_modifiers(&screen, TEST_FOURCC,
                                  &count, &modifiers) == BadMatch);
    assert(!count && !modifiers);
    assert(dmabuf_register(&screen, &backend));
    assert(dmabuf_register(&screen, &backend));
    assert(!dmabuf_register(&screen, &alternate));
    dmabuf_unregister(&screen, &alternate);
    assert(dmabuf_get_modifiers(&other, TEST_FOURCC,
                                  &count, &modifiers) == BadMatch);
    assert(!count && !modifiers);
    assert(dmabuf_get_modifiers(&screen, TEST_FOURCC,
                                  &count, &modifiers) == Success);
    assert(count == 1 && modifiers[0] == TEST_MODIFIER);
    free(modifiers);

    assert(dmabuf_import(&screen, &buffer, &pixmap, &format) == Success);
    assert(pixmap == &imported_pixmap && format == &render_format);
    buffer.num_planes = DMABUF_MAX_PLANES + 1;
    assert(dmabuf_import(&screen, &buffer, &pixmap, &format) == BadValue);
    assert(!pixmap && !format);
    buffer.num_planes = 1;
    buffer.modifier = 0;
    assert(dmabuf_import(&screen, &buffer, &pixmap, &format) == BadMatch);
    assert(!pixmap && !format);

    buffer.modifier = TEST_MODIFIER;
    imported_pixmap.drawable.height--;
    assert(dmabuf_import(&screen, &buffer, &pixmap, &format) == BadMatch);
    assert(destroyed && !pixmap && !format);

    dmabuf_unregister(&screen, &backend);
    assert(dmabuf_import(&screen, &buffer, &pixmap, &format) == BadMatch);
    assert(fcntl(fd, F_GETFD) >= 0);
    close(fd);

    dixFreePrivates(other.devPrivates, PRIVATE_SCREEN);
    dixFreePrivates(screen.devPrivates, PRIVATE_SCREEN);
    screenInfo.numScreens = 0;
    screenInfo.screens[0] = screenInfo.screens[1] = NULL;
    dixResetPrivates();
}

const testfunc_t *
dmabuf_test(void)
{
    static const testfunc_t tests[] = { check_dmabuf_backend, NULL };
    return tests;
}
