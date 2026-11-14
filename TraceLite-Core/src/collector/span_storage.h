#ifndef TRACELITE_SPAN_STORAGE_H
#define TRACELITE_SPAN_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "tracelite/span.h"

/*
 * Almacenamiento en memoria de los spans recibidos por el Collector.
 *
 * Los spans se agrupan por trace_id. Cada grupo conserva el orden de llegada
 * de sus spans; la jerarquia padre-hijo se resuelve despues usando
 * parent_span_id, nunca el orden de recepcion.
 *
 * No hay persistencia ni base de datos. Toda la memoria se libera con
 * tl_span_storage_destroy().
 */

typedef struct tl_trace_group {
    uint64_t trace_id;
    tl_span_t *spans;
    size_t span_count;
    size_t span_capacity;
} tl_trace_group_t;

typedef struct tl_span_storage {
    tl_trace_group_t *groups;
    size_t group_count;
    size_t group_capacity;
    size_t span_count;
} tl_span_storage_t;

/* Inicializa una estructura vacia. Es seguro llamarla con NULL. */
void tl_span_storage_init(tl_span_storage_t *storage);

/* Libera todos los grupos y los spans almacenados. */
void tl_span_storage_destroy(tl_span_storage_t *storage);

/*
 * Agrega una copia del span en el grupo correspondiente a su trace_id.
 *
 * Devuelve 0 en exito o -1 en error dejando errno establecido:
 *   EINVAL  argumentos invalidos o identificadores reservados.
 *   EEXIST  ya existe un span con el mismo span_id en esa traza.
 *   ENOMEM  no se pudo reservar memoria.
 */
int tl_span_storage_add(tl_span_storage_t *storage, const tl_span_t *span);

/* Busca el grupo de una traza. Devuelve NULL si no existe. */
const tl_trace_group_t *tl_span_storage_find(const tl_span_storage_t *storage,
                                             uint64_t trace_id);

size_t tl_span_storage_trace_count(const tl_span_storage_t *storage);
size_t tl_span_storage_span_count(const tl_span_storage_t *storage);

/* Devuelve el grupo en la posicion indicada, o NULL si el indice no existe. */
const tl_trace_group_t *tl_span_storage_trace_at(const tl_span_storage_t *storage,
                                                 size_t index);

#endif /* TRACELITE_SPAN_STORAGE_H */
