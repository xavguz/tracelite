#include "anomaly_detector.h"

#include <inttypes.h>

#include "span_receiver.h"
#include "tracelite/config.h"

unsigned int tl_anomaly_detect(const tl_span_t *span)
{
    unsigned int flags = TL_ANOMALY_NONE;

    if (span == NULL) {
        return flags;
    }
    if (span->status == TL_SPAN_STATUS_ERROR) {
        flags |= TL_ANOMALY_ERROR;
    }
    if ((uint64_t)span->duration_ns >
        (uint64_t)TL_LATENCY_ANOMALY_THRESHOLD_MS * UINT64_C(1000000)) {
        flags |= TL_ANOMALY_LATENCY;
    }
    return flags;
}

void tl_anomaly_print_alert(FILE *stream, const tl_span_t *span,
                            unsigned int flags)
{
    double duration_ms;

    if (stream == NULL || span == NULL || flags == TL_ANOMALY_NONE) {
        return;
    }

    duration_ms = (double)span->duration_ns / 1000000.0;
    if ((flags & TL_ANOMALY_ERROR) != 0u) {
        fprintf(stream,
                "\n[ALERT] Error anomaly detected\n"
                "Trace ID: 0x%016" PRIx64 "\n"
                "Span ID: 0x%016" PRIx64 "\n"
                "Service: %s\nOperation: %s\nDuration: %.2f ms\n"
                "Status: %s\nReason: %s\n",
                span->trace_id, span->span_id, span->service_name,
                span->operation_name, duration_ms,
                tl_span_status_name(span->status),
                span->error_message[0] == '\0' ? "operation failed"
                                                 : span->error_message);
    }
    if ((flags & TL_ANOMALY_LATENCY) != 0u) {
        fprintf(stream,
                "\n[ALERT] Latency anomaly detected\n"
                "Trace ID: 0x%016" PRIx64 "\n"
                "Span ID: 0x%016" PRIx64 "\n"
                "Service: %s\nOperation: %s\nDuration: %.2f ms\n"
                "Threshold: %u ms\nStatus: %s\n"
                "Reason: duration exceeds configured threshold\n",
                span->trace_id, span->span_id, span->service_name,
                span->operation_name, duration_ms,
                TL_LATENCY_ANOMALY_THRESHOLD_MS,
                tl_span_status_name(span->status));
    }
}
