#ifndef VACCUM_DRI3_SYNCOBJ_H
#define VACCUM_DRI3_SYNCOBJ_H

#include "vaccum_priv.h"

#ifdef DRI3
#include "dri3.h"

struct dri3_syncobj *
vaccum_import_syncobj(ClientPtr client, ScreenPtr screen, XID id, int fd);
#endif

#endif
