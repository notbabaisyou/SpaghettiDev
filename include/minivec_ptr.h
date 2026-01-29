/**
 * Spaghetti Display Server
 * Copyright (C) 2025-2026  SpaghettiFork
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
#ifndef _MINI_VEC_PTR_H_
#define _MINI_VEC_PTR_H_

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	void *array;
	size_t used;
	size_t size;
	size_t obj_size;
} mini_vector_ptr;

inline void mini_vector_ptr_init(mini_vector_ptr *a, size_t obj_size, size_t initialSize)
{
	a->array = malloc(initialSize * obj_size);
	a->used = 0;
	a->size = initialSize;
	a->obj_size = obj_size;
}

inline void insert_mini_vector_ptr(mini_vector_ptr *a, void* element)
{
	if (a->used == a->size)
	{
		a->size *= 2;
		a->array = realloc(a->array, a->size * a->obj_size);
	}

	memcpy((char *)a->array + a->used * a->obj_size, element, a->obj_size);
	a->used++;
}

inline void free_mini_vector_ptr(mini_vector_ptr *a)
{
	free(a->array);
	a->array = NULL;
	a->used = a->size = 0;
}

#endif // _MINI_VEC_PTR_H_
