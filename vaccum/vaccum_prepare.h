#ifndef _VACCUM_PREPARE_H_
#define _VACCUM_PREPARE_H_

Bool
vaccum_prepare_access(DrawablePtr drawable, vaccum_access_t access);

Bool
vaccum_prepare_access_box(DrawablePtr drawable, vaccum_access_t access,
                         int x, int y, int w, int h);

void
vaccum_finish_access(DrawablePtr drawable);

Bool
vaccum_prepare_access_picture(PicturePtr picture, vaccum_access_t access);

Bool
vaccum_prepare_access_picture_box(PicturePtr picture, vaccum_access_t access,
                        int x, int y, int w, int h);

void
vaccum_finish_access_picture(PicturePtr picture);

Bool
vaccum_prepare_access_gc(GCPtr gc);

void
vaccum_finish_access_gc(GCPtr gc);

#endif /* _VACCUM_PREPARE_H_ */
