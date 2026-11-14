#ifndef TRACELITE_TRACE_CONTEXT_H
#define TRACELITE_TRACE_CONTEXT_H

#include <stdint.h>

/*
 * Contexto minimo que viaja en una peticion de negocio para que el servicio
 * receptor pueda crear un span hijo dentro de la misma traza.
 *
 *   trace_id        Identificador de la traza completa.
 *   parent_span_id  Identificador del span del emisor. El receptor lo usara
 *                   como parent_span_id de su propio span.
 *
 * Un parent_span_id igual a 0 indica que no hay span padre.
 */
typedef struct tl_trace_context {
    uint64_t trace_id;
    uint64_t parent_span_id;
} tl_trace_context_t;

#endif /* TRACELITE_TRACE_CONTEXT_H */
