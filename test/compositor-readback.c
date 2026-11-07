/* SPDX-License-Identifier: MIT */
#undef NDEBUG
#include "dix-config.h"
#include <assert.h>
#include <string.h>
#include "../Xext/shm.c"
#include "dix/dispatch.h"
#include "compositeext.h"

enum {
    TEST_WIDTH = 8,
    TEST_HEIGHT = 6,
    TEST_DEPTH = 24,
    TEST_BITS_PER_PIXEL = 32,
    TEST_BYTES_PER_PIXEL = TEST_BITS_PER_PIXEL / 8,
    TEST_DRAWABLE = 1,
    TEST_SHMSEG,
    TEST_PIXEL = 0x5a,
    TEST_IMAGE_BYTES = TEST_WIDTH * TEST_HEIGHT * TEST_BYTES_PER_PIXEL,
};

BoxRec RegionEmptyBox;
RegDataRec RegionEmptyData, RegionBrokenData;
static unsigned char output[sizeof(xGetImageReply) + TEST_IMAGE_BYTES];
static unsigned char shared_image[TEST_IMAGE_BYTES];
static ShmDescRec segment;
static void *volatile segment_resource;
static DrawablePtr source;
static PixmapRec backing;
static RegionPtr expected_visible;
static unsigned captures, lookups;
#ifdef XACE
static unsigned censored;
#endif
static Bool compositor;

Bool
CompositeIsServerRoot(WindowPtr window)
{
    return compositor && !window->parent;
}

int
dixLookupDrawable(DrawablePtr *result, XID id, ClientPtr client,
                   Mask type_mask, Mask access_mode)
{
    assert(id == TEST_DRAWABLE);
    *result = source;
    return Success;
}

int
dixLookupResourceByType(void **result, XID id, RESTYPE type, ClientPtr client,
                         Mask access_mode)
{
    if (id != TEST_SHMSEG || type != ShmSegType)
        return BadValue;
    *result = segment_resource;
    return Success;
}

void *
ReserveClientOutputSpace(ClientPtr client, int count)
{
    assert(count <= sizeof(output));
    memset(output, 0, sizeof(output));
    return output;
}

int
CommitClientOutputSpace(ClientPtr client, int count)
{
    assert(count <= sizeof(output));
    return Success;
}

int
WriteToClient(ClientPtr client, int count, const void *data)
{
    assert(count == sizeof(xShmGetImageReply));
    return Success;
}

#ifdef XACE
void
XaceCensorImage(ClientPtr client, RegionPtr visible, long stride,
                DrawablePtr drawable, int x, int y, int width, int height,
                unsigned int format, char *pixels)
{
    assert(visible == expected_visible);
    assert(!RegionNil(visible));
    censored++;
}
#endif

static PixmapPtr
readback_pixmap(WindowPtr window)
{
    lookups++;
    return &backing;
}

static void
readback_validate(DrawablePtr drawable, int x, int y, int width, int height,
                    unsigned int subwindow_mode)
{
    assert(subwindow_mode == IncludeInferiors);
}

static void
readback_image(DrawablePtr drawable, int x, int y, int width, int height,
                unsigned int format, unsigned long planes, char *pixels)
{
    int stride = format == ZPixmap ? width * TEST_BYTES_PER_PIXEL : BitmapBytePad(width);

    memset(pixels, TEST_PIXEL, stride * height);
    captures++;
}

static void
capture(ClientPtr client, Bool shared, int format, int x, int y, int width,
         int height)
{
    xGetImageReq request = {
        .format = format, .drawable = TEST_DRAWABLE, .x = x, .y = y,
        .width = width, .height = height, .planeMask = 1,
    };
    xShmGetImageReq shm_request = {
        .format = format, .drawable = TEST_DRAWABLE, .x = x, .y = y,
        .width = width, .height = height, .planeMask = 1, .shmseg = TEST_SHMSEG,
    };
    unsigned char *pixels;
    unsigned before = captures;
    int stride = format == ZPixmap ? width * TEST_BYTES_PER_PIXEL : BitmapBytePad(width);
    int i;

    if (shared) {
        memset(shared_image, 0, sizeof(shared_image));
        client->requestBuffer = &shm_request;
        client->req_len = sizeof(shm_request) / 4;
        assert(ProcShmGetImage(client) == Success);
        pixels = shared_image;
    } else {
        client->requestBuffer = &request;
        client->req_len = sizeof(request) / 4;
        assert(ProcGetImage(client) == Success);
        pixels = output + sizeof(xGetImageReply);
    }
    assert(captures == before + 1);
#ifdef XACE
    assert(censored == captures);
#endif
    for (i = 0; i < stride * height; i++)
        assert(pixels[i] == TEST_PIXEL);
}

int
main(int argc, char **argv)
{
    ScreenRec screen = { .width = TEST_WIDTH, .height = TEST_HEIGHT,
                         .GetWindowPixmap = readback_pixmap,
                         .SourceValidate = readback_validate, .GetImage = readback_image };
    WindowOptRec optional = { 0 };
    WindowRec root = {
        .drawable = { .type = DRAWABLE_WINDOW, .depth = TEST_DEPTH,
                      .bitsPerPixel = TEST_BITS_PER_PIXEL, .pScreen = &screen,
                      .width = TEST_WIDTH, .height = TEST_HEIGHT },
        .optional = &optional, .viewable = TRUE, .realized = TRUE,
    };
    ClientRec client = { 0 };
    BoxRec bounds = { 0, 0, TEST_WIDTH, TEST_HEIGHT };
    Bool shared = argc > 1 && !strcmp(argv[1], "shm");
    int format;

    screen.root = &root;
    PixmapWidthPaddingInfo[TEST_DEPTH] = (PaddingInfo) {
        .padBytesLog2 = 2, .bitsPerPixel = TEST_BITS_PER_PIXEL,
    };
    PixmapWidthPaddingInfo[1] = (PaddingInfo) {
        .padRoundUp = BITMAP_SCANLINE_PAD - 1,
        .padPixelsLog2 = LOG2_BITMAP_PAD,
        .padBytesLog2 = LOG2_BYTES_PER_SCANLINE_PAD,
        .bitsPerPixel = 1,
    };
    source = &root.drawable;
    segment.addr = (char *) shared_image;
    segment.size = sizeof(shared_image);
    segment.writable = TRUE;
    segment_resource = &segment;
    RegionInit(&root.winSize, &bounds, 1);
    RegionNull(&root.borderClip);
    expected_visible = &root.winSize;
    compositor = TRUE;

    /* Composited root images use logical bounds and the composed visibility. */
    for (format = XYPixmap; format <= ZPixmap; format++) {
        capture(&client, shared, format, 0, 0, TEST_WIDTH, TEST_HEIGHT);
        capture(&client, shared, format, 2, 1, TEST_WIDTH / 2, TEST_HEIGHT / 2);
    }
    assert(!lookups);

    /* Redirected child windows still read their own backing pixmap. */
    {
        WindowRec child = root;
        BoxRec child_bounds = { 1, 1, TEST_WIDTH / 2 + 1, TEST_HEIGHT / 2 + 1 };

        child.parent = &root;
        child.drawable.x = child_bounds.x1;
        child.drawable.y = child_bounds.y1;
        child.drawable.width = TEST_WIDTH / 2;
        child.drawable.height = TEST_HEIGHT / 2;
        RegionInit(&child.borderClip, &child_bounds, 1);
        source = &child.drawable;
        expected_visible = &child.borderClip;
        backing.drawable.width = child.drawable.width;
        backing.drawable.height = child.drawable.height;
        backing.screen_x = child.drawable.x;
        backing.screen_y = child.drawable.y;
        capture(&client, shared, ZPixmap, 0, 0,
                 child.drawable.width, child.drawable.height);
        assert(lookups == (shared ? 0 : 1));
        RegionUninit(&child.borderClip);
    }

    /* Ordinary roots retain their backing-pixmap and border-clip checks. */
    source = &root.drawable;
    lookups = 0;
    compositor = FALSE;
    expected_visible = &root.borderClip;
    RegionReset(&root.borderClip, &bounds);
    backing.drawable.width = TEST_WIDTH;
    backing.drawable.height = TEST_HEIGHT;
    backing.screen_x = backing.screen_y = 0;
    capture(&client, shared, ZPixmap, 0, 0, TEST_WIDTH, TEST_HEIGHT);
    assert(lookups == (shared ? 0 : 1));

    RegionUninit(&root.winSize);
    RegionUninit(&root.borderClip);
    return 0;
}
