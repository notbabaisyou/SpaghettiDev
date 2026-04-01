#include "vaccum_priv.h"

int
vaccum_poly_text8(DrawablePtr drawable, GCPtr gc,
                   int x, int y, int count, char *chars)
{
    int final_pos;

    //    if (vaccum_poly_text(drawable, gc, x, y, count, chars, FALSE, &final_pos))
    //        return final_pos;
    return miPolyText8(drawable, gc, x, y, count, chars);
}

int
vaccum_poly_text16(DrawablePtr drawable, GCPtr gc,
                    int x, int y, int count, unsigned short *chars)
{
    int final_pos;

    //    if (vaccum_poly_text(drawable, gc, x, y, count, (char *) chars, TRUE, &final_pos))
    //        return final_pos;
    return miPolyText16(drawable, gc, x, y, count, chars);
}

void
vaccum_image_text8(DrawablePtr drawable, GCPtr gc,
                   int x, int y, int count, char *chars)
{
    //    if (!vaccum_image_text(drawable, gc, x, y, count, chars, FALSE))
    miImageText8(drawable, gc, x, y, count, chars);
}

void
vaccum_image_text16(DrawablePtr drawable, GCPtr gc,
                    int x, int y, int count, unsigned short *chars)
{
    //    if (!vaccum_image_text(drawable, gc, x, y, count, (char *) chars, TRUE))
    miImageText16(drawable, gc, x, y, count, chars);
}

