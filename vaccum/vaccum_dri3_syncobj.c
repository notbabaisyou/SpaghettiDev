#include "vaccum_priv.h"

#include <xf86drm.h>
#ifdef DRI3
#include "dri3.h"
#include "vaccum_dri3_syncobj.h"

struct vaccum_syncobj {
    struct dri3_syncobj base;
    int drm_fd;
    uint32_t handle;
};

static void
vaccum_syncobj_free(struct dri3_syncobj *syncobj)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;
    if (vso->handle) {
        drmSyncobjDestroy(vso->drm_fd, vso->handle);
        vso->handle = 0;
    }
    free(vso);
}

static Bool
vaccum_syncobj_has_fence(struct dri3_syncobj *syncobj, uint64_t point)
{
    (void)point;
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;
    return vso->handle != 0;
}

static Bool
vaccum_syncobj_is_signaled(struct dri3_syncobj *syncobj, uint64_t point)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;
    uint32_t handle = vso->handle;
    int ret;

    if (!handle)
        return FALSE;

    ret = drmSyncobjWait(vso->drm_fd, &handle, 1, 0,
                         DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
    return ret == 0;
}

static int
vaccum_syncobj_export_fence(struct dri3_syncobj *syncobj, uint64_t point)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;
    int fd = -1;

    (void)point;

    if (!vso->handle)
        return -1;

    if (drmSyncobjExportSyncFile(vso->drm_fd, vso->handle, &fd) != 0)
        return -1;

    return fd;
}

static void
vaccum_syncobj_import_fence(struct dri3_syncobj *syncobj, uint64_t point, int fd)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;
    uint32_t new_handle;

    (void)point;

    if (!vso->handle)
        return;

    if (drmSyncobjCreate(vso->drm_fd, 0, &new_handle) != 0)
        return;

    if (drmSyncobjImportSyncFile(vso->drm_fd, new_handle, fd) != 0) {
        drmSyncobjDestroy(vso->drm_fd, new_handle);
        return;
    }

    drmSyncobjDestroy(vso->drm_fd, vso->handle);
    vso->handle = new_handle;
}

static void
vaccum_syncobj_signal(struct dri3_syncobj *syncobj, uint64_t point)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;

    (void)point;

    if (!vso->handle)
        return;

    drmSyncobjSignal(vso->drm_fd, &vso->handle, 1);
}

static void
vaccum_syncobj_submitted_eventfd(struct dri3_syncobj *syncobj, uint64_t point, int efd)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;

    (void)point;

    if (!vso->handle)
        return;

    drmSyncobjEventfd(vso->drm_fd, vso->handle, 0, efd,
                      DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
}

static void
vaccum_syncobj_signaled_eventfd(struct dri3_syncobj *syncobj, uint64_t point, int efd)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;

    (void)point;

    if (!vso->handle)
        return;

    drmSyncobjEventfd(vso->drm_fd, vso->handle, 0, efd, 0);
}

struct dri3_syncobj *
vaccum_import_syncobj(ClientPtr client, ScreenPtr screen, XID id, int fd)
{
    vaccum_screen_private *vaccum_priv = vaccum_get_screen_private(screen);
    struct vaccum_syncobj *syncobj;
    uint32_t handle;

    if (!vaccum_priv || vaccum_priv->drm_fd < 0)
        return NULL;

    if (drmSyncobjCreate(vaccum_priv->drm_fd, 0, &handle) != 0)
        return NULL;

    if (drmSyncobjImportSyncFile(vaccum_priv->drm_fd, handle, fd) != 0) {
        drmSyncobjDestroy(vaccum_priv->drm_fd, handle);
        return NULL;
    }

    syncobj = calloc(1, sizeof(*syncobj));
    if (!syncobj) {
        drmSyncobjDestroy(vaccum_priv->drm_fd, handle);
        return NULL;
    }

    syncobj->base.id = id;
    syncobj->base.screen = screen;
    syncobj->base.refcount = 1;
    syncobj->base.free = vaccum_syncobj_free;
    syncobj->base.has_fence = vaccum_syncobj_has_fence;
    syncobj->base.is_signaled = vaccum_syncobj_is_signaled;
    syncobj->base.export_fence = vaccum_syncobj_export_fence;
    syncobj->base.import_fence = vaccum_syncobj_import_fence;
    syncobj->base.signal = vaccum_syncobj_signal;
    syncobj->base.submitted_eventfd = vaccum_syncobj_submitted_eventfd;
    syncobj->base.signaled_eventfd = vaccum_syncobj_signaled_eventfd;
    syncobj->drm_fd = vaccum_priv->drm_fd;
    syncobj->handle = handle;

    return &syncobj->base;
}
#endif
