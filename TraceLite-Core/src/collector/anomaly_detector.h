#ifndef TRACELITE_ANOMALY_DETECTOR_H
#define TRACELITE_ANOMALY_DETECTOR_H

#include <stdio.h>

#include "tracelite/span.h"

enum {
    TL_ANOMALY_NONE = 0u,
    TL_ANOMALY_ERROR = 1u << 0,
    TL_ANOMALY_LATENCY = 1u << 1
};

unsigned int tl_anomaly_detect(const tl_span_t *span);
void tl_anomaly_print_alert(FILE *stream, const tl_span_t *span,
                            unsigned int flags);

#endif /* TRACELITE_ANOMALY_DETECTOR_H */
