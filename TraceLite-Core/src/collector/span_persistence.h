#ifndef TRACELITE_SPAN_PERSISTENCE_H
#define TRACELITE_SPAN_PERSISTENCE_H

#include "tracelite/span.h"

/* Agrega un span como un objeto JSON independiente en un archivo JSON Lines. */
int tl_span_persistence_append(const char *path, const tl_span_t *span,
                               unsigned int anomaly_flags);

#endif /* TRACELITE_SPAN_PERSISTENCE_H */
