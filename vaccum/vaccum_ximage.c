#include "vaccum_priv.h"

static void
vaccum_put_image_bail(DrawablePtr drawable, GCPtr gc, int depth, int x, int y,
                      int w, int h, int leftPad, int format, char *bits)
{
    int access = VACCUM_ACCESS_RW;

    if (gc->alu == GXcopy && vaccum_pm_is_solid(gc->depth, gc->planemask)) {
	BoxRec box;
	box.x1 = x;
	box.y1 = y;
	box.x2 = x + w;
	box.y2 = y + h;

	if (RegionContainsRect(gc->pCompositeClip, &box) == rgnIN)
            access = VACCUM_ACCESS_WO;
    }
    if (vaccum_prepare_access_box(drawable, access, x, y, w, h))
        fbPutImage(drawable, gc, depth, x, y, w, h, leftPad, format, bits);
    vaccum_finish_access(drawable);
}

void
vaccum_put_image(DrawablePtr drawable, GCPtr gc, int depth, int x, int y,
                 int w, int h, int leftPad, int format, char *bits)
{
    vaccum_put_image_bail(drawable, gc, depth, x, y, w, h, leftPad, format, bits);
}
