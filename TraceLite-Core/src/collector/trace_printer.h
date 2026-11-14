#ifndef TRACELITE_TRACE_PRINTER_H
#define TRACELITE_TRACE_PRINTER_H

#include <stdio.h>

#include "span_storage.h"

/*
 * Presenta por pantalla los spans agrupados por trace_id.
 *
 * Dentro de cada traza la jerarquia se construye con parent_span_id:
 * primero se imprimen los spans raiz y despues sus descendientes. Los spans
 * cuyo padre no esta en la traza se marcan como huerfanos.
 *
 * Devuelve 0 en exito o -1 si hubo un error de escritura o de memoria.
 */
int tl_trace_printer_print(const tl_span_storage_t *storage, FILE *stream);

#endif /* TRACELITE_TRACE_PRINTER_H */
