/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <errno.h>
#include <linux/dma-buf.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/ioctl.h>

/* Load into the test server only. Fail its first read of a shared pixmap. */
__attribute__((visibility("default"))) int
ioctl(int fd, unsigned long request, ...)
{
    static bool injected;
    int (*next_ioctl)(int, unsigned long, ...) = dlsym(RTLD_NEXT, "ioctl");
    va_list args;

    va_start(args, request);
    void *arg = va_arg(args, void *);

    va_end(args);
    if (!injected && request == DMA_BUF_IOCTL_SYNC) {
        const struct dma_buf_sync *sync = arg;

        if (sync->flags == (DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) {
            injected = true;
            errno = EIO;
            return -1;
        }
    }

    return next_ioctl(fd, request, arg);
}
