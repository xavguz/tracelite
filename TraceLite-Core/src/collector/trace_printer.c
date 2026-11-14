#include "trace_printer.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "span_receiver.h"
#include "tracelite/span.h"

#define TL_PRINTER_MAX_DEPTH 128u

static void format_duration_ms(int64_t duration_ns, char *buffer,
                               size_t buffer_size)
{
    int64_t whole_ms;
    int64_t microseconds;

    if (duration_ns < 0) {
        snprintf(buffer, buffer_size, "%" PRId64, duration_ns);
        return;
    }

    whole_ms = duration_ns / INT64_C(1000000);
    microseconds = (duration_ns % INT64_C(1000000)) / INT64_C(1000);
    snprintf(buffer, buffer_size, "%" PRId64 ".%03" PRId64, whole_ms,
             microseconds);
}

static int compute_end_ns(const tl_span_t *span, int64_t *end_ns)
{
    if (span->duration_ns > 0 &&
        span->start_monotonic_ns > INT64_MAX - span->duration_ns) {
        return -1;
    }

    *end_ns = span->start_monotonic_ns + span->duration_ns;
    return 0;
}

static void print_span(FILE *stream, const tl_span_t *span, unsigned depth,
                       const char *role)
{
    char duration[64];
    int64_t end_ns = 0;
    unsigned indent = depth > TL_PRINTER_MAX_DEPTH ? TL_PRINTER_MAX_DEPTH
                                                   : depth;
    int indentation = (int)(indent * 2u);

    format_duration_ms(span->duration_ns, duration, sizeof(duration));

    fprintf(stream,
            "%*s[%s] depth=%u trace_id=0x%016" PRIx64
            " span_id=0x%016" PRIx64 " parent_span_id=",
            indentation, "", role, depth, span->trace_id, span->span_id);

    if (span->parent_span_id == TL_INVALID_SPAN_ID) {
        fputs("none", stream);
    } else {
        fprintf(stream, "0x%016" PRIx64, span->parent_span_id);
    }

    fprintf(stream,
            "\n%*s  service=%.*s operation=%.*s status=%s\n",
            indentation, "", (int)(TL_SERVICE_NAME_MAX - 1u),
            span->service_name, (int)(TL_OPERATION_NAME_MAX - 1u),
            span->operation_name, tl_span_status_name(span->status));

    if (compute_end_ns(span, &end_ns) == 0) {
        fprintf(stream, "%*s  start_ns=%" PRId64 " end_ns=%" PRId64
                        " duration_ms=%s\n",
                indentation, "", span->start_monotonic_ns, end_ns, duration);
    } else {
        fprintf(stream, "%*s  start_ns=%" PRId64
                        " end_ns=overflow duration_ms=%s\n",
                indentation, "", span->start_monotonic_ns, duration);
    }

    if (span->error_message[0] != '\0') {
        fprintf(stream, "%*s  error=\"%.*s\"\n", indentation, "",
                (int)(TL_ERROR_MESSAGE_MAX - 1u), span->error_message);
    }
}

static void print_subtree(FILE *stream, const tl_trace_group_t *group,
                          size_t index, unsigned depth, const char *role,
                          unsigned char *visited)
{
    const tl_span_t *span;
    size_t child_index;

    if (visited[index] != 0u) {
        return;
    }

    visited[index] = 1u;
    span = &group->spans[index];
    print_span(stream, span, depth, role);

    if (depth >= TL_PRINTER_MAX_DEPTH) {
        return;
    }

    for (child_index = 0; child_index < group->span_count; child_index++) {
        if (visited[child_index] == 0u &&
            group->spans[child_index].parent_span_id == span->span_id) {
            print_subtree(stream, group, child_index, depth + 1u, "hijo",
                          visited);
        }
    }
}

static void print_flat(FILE *stream, const tl_trace_group_t *group)
{
    size_t index;

    for (index = 0; index < group->span_count; index++) {
        print_span(stream, &group->spans[index], 0u, "span");
    }
}

int tl_trace_printer_print(const tl_span_storage_t *storage, FILE *stream)
{
    size_t trace_index;
    int failed = 0;

    if (storage == NULL || stream == NULL) {
        errno = EINVAL;
        return -1;
    }

    fprintf(stream, "=== TraceLite Collector: %zu traza(s), %zu span(s) ===\n",
            tl_span_storage_trace_count(storage),
            tl_span_storage_span_count(storage));

    for (trace_index = 0; trace_index < storage->group_count; trace_index++) {
        const tl_trace_group_t *group = &storage->groups[trace_index];
        unsigned char *visited;
        size_t span_index;

        fprintf(stream, "\nTraza %zu/%zu: trace_id=0x%016" PRIx64
                        " spans=%zu\n",
                trace_index + 1u, storage->group_count, group->trace_id,
                group->span_count);

        visited = calloc(group->span_count, sizeof(*visited));
        if (visited == NULL) {
            /*
             * Sin memoria auxiliar se pierde la jerarquia, pero no los datos.
             * Se imprimen todos los spans en orden de llegada.
             */
            print_flat(stream, group);
            failed = 1;
            continue;
        }

        /* Primero los spans raiz. */
        for (span_index = 0; span_index < group->span_count; span_index++) {
            if (group->spans[span_index].parent_span_id ==
                TL_INVALID_SPAN_ID) {
                print_subtree(stream, group, span_index, 0u, "raiz", visited);
            }
        }

        /* Despues los huerfanos o ciclos sin raiz alcanzable. */
        for (span_index = 0; span_index < group->span_count; span_index++) {
            if (visited[span_index] == 0u) {
                print_subtree(stream, group, span_index, 0u, "huerfano",
                              visited);
            }
        }

        free(visited);
    }

    if (fflush(stream) != 0) {
        return -1;
    }

    if (failed || ferror(stream)) {
        return -1;
    }

    return 0;
}
