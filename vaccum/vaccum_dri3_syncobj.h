#ifndef VACCUM_DRI3_SYNCOBJ_H
#define VACCUM_DRI3_SYNCOBJ_H

#ifdef DRI3
#include "misc.h"
#include "dri3.h"

Bool vaccum_syncobj_supported(int drm_fd);
struct dri3_syncobj *vaccum_syncobj_import(ScreenPtr screen, XID id,
                                            int drm_fd, int fd);
#endif

#endif
