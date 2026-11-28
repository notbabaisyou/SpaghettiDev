#include "dix-config.h"

#ifdef DRI3
#include "vaccum_dri3_syncobj.h"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <xf86drm.h>

struct vaccum_syncobj {
    struct dri3_syncobj base;
    int drm_fd;
    uint32_t handle;
};

static void
vaccum_syncobj_free(struct dri3_syncobj *syncobj)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;

    drmSyncobjDestroy(vso->drm_fd, vso->handle);
    close(vso->drm_fd);
    free(vso);
}

static Bool
vaccum_syncobj_has_fence(struct dri3_syncobj *syncobj, uint64_t point)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;

    return drmSyncobjTimelineWait(vso->drm_fd, &vso->handle, &point, 1, 0,
                                 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
                                 NULL) == 0;
}

static Bool
vaccum_syncobj_is_signaled(struct dri3_syncobj *syncobj, uint64_t point)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;

    return drmSyncobjTimelineWait(vso->drm_fd, &vso->handle, &point, 1, 0,
                                 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
                                 NULL) == 0;
}

static int
vaccum_syncobj_export_fence(struct dri3_syncobj *syncobj, uint64_t point)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;
    uint32_t binary;
    int fd = -1;

    if (drmSyncobjCreate(vso->drm_fd, 0, &binary) != 0)
        return -1;
    if (drmSyncobjTransfer(vso->drm_fd, binary, 0, vso->handle, point, 0) ==
        0)
        drmSyncobjExportSyncFile(vso->drm_fd, binary, &fd);

    drmSyncobjDestroy(vso->drm_fd, binary);
    return fd;
}

/* Present transfers ownership of the sync_file FD to this callback. */
static void
vaccum_syncobj_import_fence(struct dri3_syncobj *syncobj, uint64_t point,
                             int fd)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;
    uint32_t binary;

    if (fd < 0)
        return;
    if (drmSyncobjCreate(vso->drm_fd, 0, &binary) != 0)
        goto close_fence;

    if (drmSyncobjImportSyncFile(vso->drm_fd, binary, fd) == 0)
        drmSyncobjTransfer(vso->drm_fd, vso->handle, point, binary, 0, 0);
    drmSyncobjDestroy(vso->drm_fd, binary);
close_fence:
    close(fd);
}

static void
vaccum_syncobj_signal(struct dri3_syncobj *syncobj, uint64_t point)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;

    drmSyncobjTimelineSignal(vso->drm_fd, &vso->handle, &point, 1);
}

static void
vaccum_syncobj_submitted_eventfd(struct dri3_syncobj *syncobj, uint64_t point,
                                 int efd)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;

    drmSyncobjEventfd(vso->drm_fd, vso->handle, point, efd,
                      DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
}

static void
vaccum_syncobj_signaled_eventfd(struct dri3_syncobj *syncobj, uint64_t point,
                                int efd)
{
    struct vaccum_syncobj *vso = (struct vaccum_syncobj *)syncobj;

    drmSyncobjEventfd(vso->drm_fd, vso->handle, point, efd, 0);
}

Bool
vaccum_syncobj_supported(int drm_fd)
{
    uint64_t timeline;
    uint32_t handle;

    if (drmGetCap(drm_fd, DRM_CAP_SYNCOBJ_TIMELINE, &timeline) != 0 ||
        !timeline)
        return FALSE;

    int notify = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    if (notify < 0)
        return FALSE;

    Bool supported = FALSE;

    if (drmSyncobjCreate(drm_fd, 0, &handle) == 0) {
        struct vaccum_syncobj probe = { .drm_fd = drm_fd, .handle = handle };
        uint64_t point = 1;
        eventfd_t count = 0;

        if (drmSyncobjEventfd(drm_fd, handle, point, notify, 0) == 0 &&
            drmSyncobjTimelineSignal(drm_fd, &handle, &point, 1) == 0 &&
            eventfd_read(notify, &count) == 0) {
            int fd = vaccum_syncobj_export_fence(&probe.base, point);

            supported = fd >= 0;
            if (supported)
                close(fd);
        }

        drmSyncobjDestroy(drm_fd, handle);
    }

    close(notify);
    return supported;
}

struct dri3_syncobj *
vaccum_syncobj_import(ScreenPtr screen, XID id, int drm_fd, int fd)
{
    struct vaccum_syncobj *vso = calloc(1, sizeof(*vso));

    if (!vso)
        return NULL;

    /* Keep the DRM file alive while queued Present work holds a reference. */
    vso->drm_fd = fcntl(drm_fd, F_DUPFD_CLOEXEC, 0);
    if (vso->drm_fd < 0)
        goto free_object;
    if (drmSyncobjFDToHandle(vso->drm_fd, fd, &vso->handle) != 0)
        goto close_drm;

    vso->base = (struct dri3_syncobj) {
        .id = id,
        .screen = screen,
        .refcount = 1,
        .free = vaccum_syncobj_free,
        .has_fence = vaccum_syncobj_has_fence,
        .is_signaled = vaccum_syncobj_is_signaled,
        .export_fence = vaccum_syncobj_export_fence,
        .import_fence = vaccum_syncobj_import_fence,
        .signal = vaccum_syncobj_signal,
        .submitted_eventfd = vaccum_syncobj_submitted_eventfd,
        .signaled_eventfd = vaccum_syncobj_signaled_eventfd,
    };
    return &vso->base;

close_drm:
    close(vso->drm_fd);
free_object:
    free(vso);
    return NULL;
}
#endif
