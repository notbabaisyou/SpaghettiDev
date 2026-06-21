#include "vaccum_priv.h"

void
vaccum_poly_point(DrawablePtr drawable, GCPtr gc, int mode, int npt,
                  DDXPointPtr ppt)
{
    //    if (vaccum_poly_point_gl(drawable, gc, mode, npt, ppt))
    //        return;
    miPolyPoint(drawable, gc, mode, npt, ppt);
}
