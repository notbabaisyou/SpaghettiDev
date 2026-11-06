/* SPDX-License-Identifier: MIT */
#include "dix-config.h"
#ifdef TEST_LEGACY_PRESENT_ABI
#undef SPAGHETTI
#endif
#include <assert.h>
#include <stddef.h>
#include "misc.h"
#include "present.h"

enum {
    TEST_PRESENT_VERSION_LEGACY = 1,
    TEST_PRESENT_VERSION_SPAGHETTI = 2,
};

/* The version 1 prefix is part of the installed video driver API. */
struct present_screen_info_v1 {
    uint32_t version;
    present_get_crtc_ptr get_crtc;
    present_get_ust_msc_ptr get_ust_msc;
    present_queue_vblank_ptr queue_vblank;
    present_abort_vblank_ptr abort_vblank;
    present_flush_ptr flush;
    uint32_t capabilities;
    present_check_flip_ptr check_flip;
    present_flip_ptr flip;
    present_unflip_ptr unflip;
    present_check_flip2_ptr check_flip2;
};

int
main(void)
{
    assert(offsetof(present_screen_info_rec, check_flip2) ==
           offsetof(struct present_screen_info_v1, check_flip2));
#ifdef SPAGHETTI
    assert(PRESENT_SCREEN_INFO_VERSION == TEST_PRESENT_VERSION_SPAGHETTI);
    assert(offsetof(present_screen_info_rec, window_flip) ==
           sizeof(struct present_screen_info_v1));
#else
    assert(PRESENT_SCREEN_INFO_VERSION == TEST_PRESENT_VERSION_LEGACY);
    assert(sizeof(present_screen_info_rec) == sizeof(struct present_screen_info_v1));
#endif
    return 0;
}
