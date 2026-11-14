#include "span_storage.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TL_STORAGE_INITIAL_CAPACITY 4u

/*
 * Duplica la capacidad de un array dinamico.
 *
 * Se usa un puntero temporal para no perder la referencia original si
 * realloc() falla. Tambien se comprueba el desbordamiento de size_t antes de
 * calcular el nuevo tamano en bytes.
 */
static int grow_array(void **data, size_t *capacity, size_t element_size)
{
    size_t new_capacity;
    void *new_data;

    if (*capacity == 0) {
        new_capacity = TL_STORAGE_INITIAL_CAPACITY;
    } else {
        if (*capacity > SIZE_MAX / 2u) {
            errno = ENOMEM;
            return -1;
        }
        new_capacity = *capacity * 2u;
    }

    if (element_size == 0 || new_capacity > SIZE_MAX / element_size) {
        errno = ENOMEM;
        return -1;
    }

    new_data = realloc(*data, new_capacity * element_size);
    if (new_data == NULL) {
        return -1;
    }

    *data = new_data;
    *capacity = new_capacity;
    return 0;
}

void tl_span_storage_init(tl_span_storage_t *storage)
{
    if (storage == NULL) {
        return;
    }

    memset(storage, 0, sizeof(*storage));
}

void tl_span_storage_destroy(tl_span_storage_t *storage)
{
    size_t index;

    if (storage == NULL) {
        return;
    }

    for (index = 0; index < storage->group_count; index++) {
        free(storage->groups[index].spans);
    }

    free(storage->groups);
    memset(storage, 0, sizeof(*storage));
}

const tl_trace_group_t *tl_span_storage_find(const tl_span_storage_t *storage,
                                             uint64_t trace_id)
{
    size_t index;

    if (storage == NULL || trace_id == TL_INVALID_TRACE_ID) {
        return NULL;
    }

    for (index = 0; index < storage->group_count; index++) {
        if (storage->groups[index].trace_id == trace_id) {
            return &storage->groups[index];
        }
    }

    return NULL;
}

int tl_span_storage_add(tl_span_storage_t *storage, const tl_span_t *span)
{
    tl_trace_group_t *group;
    size_t index;
    int new_group = 0;

    if (storage == NULL || span == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (span->trace_id == TL_INVALID_TRACE_ID ||
        span->span_id == TL_INVALID_SPAN_ID) {
        errno = EINVAL;
        return -1;
    }

    group = (tl_trace_group_t *)tl_span_storage_find(storage, span->trace_id);
    if (group == NULL) {
        if (storage->group_count == storage->group_capacity) {
            if (grow_array((void **)&storage->groups, &storage->group_capacity,
                           sizeof(storage->groups[0])) < 0) {
                return -1;
            }
        }

        group = &storage->groups[storage->group_count];
        memset(group, 0, sizeof(*group));
        group->trace_id = span->trace_id;
        new_group = 1;
    }

    for (index = 0; index < group->span_count; index++) {
        if (group->spans[index].span_id == span->span_id) {
            errno = EEXIST;
            return -1;
        }
    }

    if (group->span_count == group->span_capacity) {
        if (grow_array((void **)&group->spans, &group->span_capacity,
                       sizeof(group->spans[0])) < 0) {
            /*
             * Si el grupo era nuevo, aun no forma parte del almacenamiento:
             * no se incrementa group_count y el siguiente intento reutiliza
             * la misma posicion.
             */
            return -1;
        }
    }

    group->spans[group->span_count] = *span;
    group->span_count++;
    if (new_group) {
        storage->group_count++;
    }
    storage->span_count++;
    return 0;
}

size_t tl_span_storage_trace_count(const tl_span_storage_t *storage)
{
    return storage == NULL ? 0u : storage->group_count;
}

size_t tl_span_storage_span_count(const tl_span_storage_t *storage)
{
    return storage == NULL ? 0u : storage->span_count;
}

const tl_trace_group_t *tl_span_storage_trace_at(const tl_span_storage_t *storage,
                                                 size_t index)
{
    if (storage == NULL || index >= storage->group_count) {
        return NULL;
    }

    return &storage->groups[index];
}
