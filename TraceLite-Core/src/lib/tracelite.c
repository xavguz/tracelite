#include "tracelite/tracelite.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <time.h>

#include "id_generator.h"
#include "span_sender.h"
#include "text_util.h"
#include "tracelite/config.h"

/*
 * Estado de la instrumentacion del proceso actual.
 *
 * La biblioteca no es thread-safe: el diseno de TraceLite procesa pedidos de
 * forma secuencial y no se usan locks.
 */
typedef struct tl_library_state {
    int initialized;
    char service_name[TL_SERVICE_NAME_MAX];
    char collector_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} tl_library_state_t;

static tl_library_state_t g_state;

static int64_t monotonic_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }

    return (int64_t)now.tv_sec * INT64_C(1000000000) + (int64_t)now.tv_nsec;
}

static const char *configured_collector_path(void)
{
    const char *path = getenv("TRACELITE_COLLECTOR_SOCKET");

    if (path == NULL || path[0] == '\0') {
        path = TL_COLLECTOR_SOCKET_PATH;
    }

    return path;
}

int tl_init(const char *service_name)
{
    const char *collector_path;
    size_t path_length;

    if (g_state.initialized) {
        errno = EALREADY;
        return -1;
    }

    if (service_name == NULL || service_name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    collector_path = configured_collector_path();
    path_length = strlen(collector_path);
    if (path_length >= sizeof(g_state.collector_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memset(&g_state, 0, sizeof(g_state));

    if (tl_copy_text(g_state.service_name, sizeof(g_state.service_name),
                     service_name) < 0) {
        return -1;
    }

    memcpy(g_state.collector_path, collector_path, path_length + 1u);

    if (tl_id_generator_init() < 0) {
        memset(&g_state, 0, sizeof(g_state));
        return -1;
    }

    g_state.initialized = 1;
    return 0;
}

void tl_shutdown(void)
{
    tl_id_generator_reset();
    memset(&g_state, 0, sizeof(g_state));
}

int tl_span_start(tl_span_t *span, const char *operation_name,
                  const tl_trace_context_t *parent,
                  tl_trace_context_t *out_context)
{
    uint64_t trace_id;
    uint64_t parent_span_id = TL_INVALID_SPAN_ID;
    int64_t start_ns;

    if (!g_state.initialized) {
        errno = EPERM;
        return -1;
    }

    if (span == NULL || operation_name == NULL || operation_name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (parent != NULL) {
        if (parent->trace_id == TL_INVALID_TRACE_ID) {
            errno = EINVAL;
            return -1;
        }
        trace_id = parent->trace_id;
        parent_span_id = parent->parent_span_id;
    } else {
        trace_id = tl_id_generator_next(TL_ID_KIND_TRACE);
        if (trace_id == 0) {
            return -1;
        }
    }

    start_ns = monotonic_now_ns();
    if (start_ns < 0) {
        return -1;
    }

    memset(span, 0, sizeof(*span));
    span->trace_id = trace_id;
    span->span_id = tl_id_generator_next(TL_ID_KIND_SPAN);
    if (span->span_id == TL_INVALID_SPAN_ID) {
        return -1;
    }

    /*
     * La generacion ya evita repeticiones dentro del proceso, pero se
     * comprueba de forma explicita que un span no sea su propio padre.
     */
    while (span->span_id == parent_span_id) {
        span->span_id = tl_id_generator_next(TL_ID_KIND_SPAN);
        if (span->span_id == TL_INVALID_SPAN_ID) {
            return -1;
        }
    }

    span->parent_span_id = parent_span_id;
    span->start_monotonic_ns = start_ns;
    span->duration_ns = 0;
    span->status = (int32_t)TL_SPAN_STATUS_UNSET;
    span->flags = 0;

    if (tl_copy_text(span->service_name, sizeof(span->service_name),
                     g_state.service_name) < 0 ||
        tl_copy_text(span->operation_name, sizeof(span->operation_name),
                     operation_name) < 0) {
        return -1;
    }

    span->error_message[0] = '\0';

    if (out_context != NULL) {
        out_context->trace_id = span->trace_id;
        out_context->parent_span_id = span->span_id;
    }

    return 0;
}

int tl_span_finish(tl_span_t *span, tl_span_status_t status,
                   const char *error_message)
{
    int64_t end_ns;

    if (!g_state.initialized) {
        errno = EPERM;
        return -1;
    }

    if (span == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (status != TL_SPAN_STATUS_OK && status != TL_SPAN_STATUS_ERROR &&
        status != TL_SPAN_STATUS_TIMEOUT &&
        status != TL_SPAN_STATUS_CANCELLED) {
        errno = EINVAL;
        return -1;
    }

    if (span->trace_id == TL_INVALID_TRACE_ID ||
        span->span_id == TL_INVALID_SPAN_ID ||
        span->start_monotonic_ns < 0) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Un span solo puede finalizarse una vez. Evita reenviar el mismo span y
     * sobrescribir su duracion o su estado.
     */
    if (span->status != (int32_t)TL_SPAN_STATUS_UNSET) {
        errno = EALREADY;
        return -1;
    }

    end_ns = monotonic_now_ns();
    if (end_ns < 0) {
        return -1;
    }

    if (end_ns < span->start_monotonic_ns) {
        span->duration_ns = 0;
    } else {
        span->duration_ns = end_ns - span->start_monotonic_ns;
    }

    span->status = (int32_t)status;
    span->flags = 0;

    if (tl_copy_text(span->error_message, sizeof(span->error_message),
                     error_message) < 0) {
        return -1;
    }

    /*
     * El span queda finalizado aunque el envio falle. La telemetria no debe
     * interrumpir el negocio; el llamador decide si registra el error.
     */
    return tl_span_sender_send(g_state.collector_path, span);
}

int tl_span_make_context(const tl_span_t *span, tl_trace_context_t *out_context)
{
    if (span == NULL || out_context == NULL ||
        span->trace_id == TL_INVALID_TRACE_ID ||
        span->span_id == TL_INVALID_SPAN_ID) {
        errno = EINVAL;
        return -1;
    }

    out_context->trace_id = span->trace_id;
    out_context->parent_span_id = span->span_id;
    return 0;
}

int tl_context_is_valid(const tl_trace_context_t *context)
{
    return context != NULL && context->trace_id != TL_INVALID_TRACE_ID;
}

int tl_collector_request_shutdown(const char *socket_path)
{
    if (socket_path == NULL || socket_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    return tl_span_sender_shutdown(socket_path);
}
