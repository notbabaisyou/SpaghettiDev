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
#include "glamor_priv.h"

static inline size_t
round_up_pow2(size_t n, size_t align)
{
    if (n == 0) {
        return 0;
    } else {
        return (n + align - 1) & ~(align - 1);
    }
}

static void
pbo_slot_cleanup(pbo_slot_t *slot)
{
    if (!slot->id) {
        return;
    }

    pbo_wait_fence(slot);

    if (slot->persistent && slot->map) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, slot->id);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }

    glDeleteBuffers(1, &slot->id);

    slot->id = 0;
    slot->map = NULL;
    slot->size = 0;
    slot->persistent = FALSE;
    slot->coherent = FALSE;
}

Bool
pbo_acquire_upload(glamor_screen_private *glamor_priv,
				   image_state_t *s, size_t required, pbo_slot_t **out)
{
    pbo_slot_t *best_wait = NULL;

    for (int tries = 0; tries < 4; tries++) {
        pbo_slot_t *slot = &s->upload[s->upload_index];
        s->upload_index = (s->upload_index + 1) & 3;

        /* Check fence */
        if (slot->fence) {
            GLenum r = glClientWaitSync(slot->fence, 0, 0);
            if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) {
                glDeleteSync(slot->fence);
                slot->fence = 0;
            } else {
                if (!best_wait) {
                    best_wait = slot;
                }
                continue;
            }
        }

        /* Persistent-mapped path */
        if (glamor_priv->has_buffer_storage) {
            size_t alloc = required < 1048576 ?  round_up_pow2(required, 4096) :
                                                 round_up_pow2(required, 262144);

            Bool need_new = !slot->id || slot->size < alloc || !slot->persistent;

            if (need_new) {
                if (slot->id) {
                    pbo_slot_cleanup(slot);
                }

                glGenBuffers(1, &slot->id);
                if (!slot->id) {
                    return FALSE;
                }

                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, slot->id);

                GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT |
                    (s->prefer_coherent ? GL_MAP_COHERENT_BIT : 0);

                glBufferStorage(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)alloc, NULL, flags);

                if (glGetError() != GL_NO_ERROR) {
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                    glDeleteBuffers(1, &slot->id);
                    slot->id = 0;
                    return FALSE;
                }

                GLbitfield map_flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT |
                    (s->prefer_coherent ? GL_MAP_COHERENT_BIT : GL_MAP_FLUSH_EXPLICIT_BIT);

                slot->map = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0,
                                             (GLsizeiptr)alloc, map_flags);

                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

                if (!slot->map) {
                    glDeleteBuffers(1, &slot->id);
                    slot->id = 0;
                    return FALSE;
                }

                slot->size = alloc;
                slot->persistent = TRUE;
                slot->coherent = s->prefer_coherent;
            }

            *out = slot;
            return TRUE;
        }

        if (!slot->id) {
            glGenBuffers(1, &slot->id);
            if (!slot->id) {
                return FALSE;
            }
            slot->size = 0;
        }

        if (slot->size < required) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, slot->id);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)required, NULL, GL_STREAM_DRAW);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            slot->size = required;
        }

        *out = slot;
        return TRUE;
    }

    if (best_wait) {
        pbo_wait_fence(best_wait);
        *out = best_wait;
        return TRUE;
    }

    return FALSE;
}