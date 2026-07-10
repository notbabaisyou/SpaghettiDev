/**
 * Spaghetti Display Server
 * Copyright (C) 2026  SpaghettiFork
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * Tests for rapidhash and HashGlyph.
 */

/* Test relies on assert() */
#undef NDEBUG

#include <dix-config.h>

#include <assert.h>
#include "os.h"
#include "os/rapidhash.h"
#include "glyphstr.h"
#include "tests-common.h"

static void
rapidhash_determinism(void)
{
    const char *data = "test data for rapidhash";
    uint64_t h1, h2;

    h1 = rapidhash(data, strlen(data));
    h2 = rapidhash(data, strlen(data));
    assert(h1 == h2);
}

static void
rapidhash_different_inputs(void)
{
    const char *a = "hello";
    const char *b = "world";
    uint64_t ha, hb;

    ha = rapidhash(a, strlen(a));
    hb = rapidhash(b, strlen(b));
    assert(ha != hb);
}

static void
rapidhash_empty_input(void)
{
    uint64_t h1, h2;

    h1 = rapidhash("", 0);
    h2 = rapidhash("", 0);
    assert(h1 == h2);
}

static void
rapidhash_seed_sensitivity(void)
{
    const char *data = "seed test";
    uint64_t h1, h2;

    h1 = rapidhash_withSeed(data, strlen(data), 0);
    h2 = rapidhash_withSeed(data, strlen(data), 1);
    assert(h1 != h2);
}

static void
hashglyph_produces_20_bytes(void)
{
    xGlyphInfo gi = { 0 };
    CARD8 bits[64];
    unsigned char hash[20];
    int ret;

    memset(bits, 0x42, sizeof(bits));
    gi.width = 8;
    gi.height = 8;

    ret = HashGlyph(&gi, bits, sizeof(bits), hash);
    assert(ret == 0);

    /* Verify determinism */
    unsigned char hash2[20];
    ret = HashGlyph(&gi, bits, sizeof(bits), hash2);
    assert(ret == 0);
    assert(memcmp(hash, hash2, 20) == 0);
}

static void
hashglyph_different_glyphs(void)
{
    xGlyphInfo gi1 = { 0 }, gi2 = { 0 };
    CARD8 bits1[64], bits2[64];
    unsigned char hash1[20], hash2[20];

    memset(bits1, 0x41, sizeof(bits1));
    memset(bits2, 0x42, sizeof(bits2));
    gi1.width = 8;
    gi1.height = 8;
    gi2.width = 16;
    gi2.height = 16;

    HashGlyph(&gi1, bits1, sizeof(bits1), hash1);
    HashGlyph(&gi2, bits2, sizeof(bits2), hash2);
    assert(memcmp(hash1, hash2, 20) != 0);
}

const testfunc_t*
rapidhash_test(void)
{
    static const testfunc_t testfuncs[] = {
        rapidhash_determinism,
        rapidhash_different_inputs,
        rapidhash_empty_input,
        rapidhash_seed_sensitivity,
        hashglyph_produces_20_bytes,
        hashglyph_different_glyphs,
        NULL,
    };

    return testfuncs;
}
