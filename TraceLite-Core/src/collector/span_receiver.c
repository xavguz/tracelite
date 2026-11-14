#include "span_receiver.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "tracelite/collector_protocol.h"

/*
 * Buffer alineado para recibir el datagrama y copiarlo despues con memcpy.
 * Se reserva un byte adicional para detectar datagramas mayores que el maximo
 * incluso cuando recvmsg() no marca MSG_TRUNC.
 */
typedef union tl_receiver_buffer {
    tl_collector_message_t span_message;
    tl_collector_control_t control_message;
    unsigned char raw[TL_COLLECTOR_MAX_DATAGRAM + 1u];
} tl_receiver_buffer_t;

static tl_receive_result_t reject(char *reason, size_t reason_size,
                                  const char *message)
{
    if (reason != NULL && reason_size > 0) {
        snprintf(reason, reason_size, "%s", message);
    }

    return TL_RECEIVE_REJECTED;
}

/*
 * Comprueba que un campo de texto contiene un terminador dentro de su tamano.
 * Si allow_empty es 0, tambien exige que el primer byte no sea '\0'.
 * Nunca recorre mas alla del tamano del campo.
 */
static int text_is_valid(const char *text, size_t size, int allow_empty)
{
    const char *terminator = memchr(text, '\0', size);

    if (terminator == NULL) {
        return 0;
    }

    if (!allow_empty && terminator == text) {
        return 0;
    }

    return 1;
}

static tl_receive_result_t validate_span(const tl_span_t *span,
                                         char *reason, size_t reason_size)
{
    if (span->trace_id == TL_INVALID_TRACE_ID) {
        return reject(reason, reason_size, "trace_id reservado (0)");
    }

    if (span->span_id == TL_INVALID_SPAN_ID) {
        return reject(reason, reason_size, "span_id reservado (0)");
    }

    if (span->parent_span_id == span->span_id) {
        return reject(reason, reason_size, "un span no puede ser su propio padre");
    }

    if (span->status < (int32_t)TL_SPAN_STATUS_UNSET ||
        span->status > (int32_t)TL_SPAN_STATUS_CANCELLED) {
        return reject(reason, reason_size, "estado de span fuera de rango");
    }

    if (span->status == (int32_t)TL_SPAN_STATUS_UNSET) {
        return reject(reason, reason_size, "estado de span no establecido");
    }

    if (span->flags != 0u) {
        return reject(reason, reason_size, "flags reservados distintos de 0");
    }

    if (span->start_monotonic_ns < 0) {
        return reject(reason, reason_size, "start_monotonic_ns negativo");
    }

    if (span->duration_ns < 0) {
        return reject(reason, reason_size, "duration_ns negativa");
    }

    if (span->duration_ns > 0 &&
        span->start_monotonic_ns > INT64_MAX - span->duration_ns) {
        return reject(reason, reason_size, "desbordamiento en start + duration");
    }

    if (!text_is_valid(span->service_name, sizeof(span->service_name), 0)) {
        return reject(reason, reason_size,
                      "service_name vacio o sin terminador");
    }

    if (!text_is_valid(span->operation_name, sizeof(span->operation_name), 0)) {
        return reject(reason, reason_size,
                      "operation_name vacio o sin terminador");
    }

    if (!text_is_valid(span->error_message, sizeof(span->error_message), 1)) {
        return reject(reason, reason_size, "error_message sin terminador");
    }

    return TL_RECEIVE_SPAN;
}

static tl_receive_result_t validate_control(const void *data, char *reason,
                                            size_t reason_size)
{
    tl_collector_control_t control;

    memcpy(&control, data, sizeof(control));

    if (control.magic != TL_COLLECTOR_MAGIC) {
        return reject(reason, reason_size, "magic de control invalido");
    }

    if (control.version != TL_COLLECTOR_VERSION) {
        return reject(reason, reason_size, "version de control invalida");
    }

    if (control.type != TL_COLLECTOR_MESSAGE_SHUTDOWN) {
        return reject(reason, reason_size, "tipo de control no soportado");
    }

    return TL_RECEIVE_SHUTDOWN;
}

static tl_receive_result_t validate_message(const void *data, tl_span_t *span,
                                            char *reason, size_t reason_size)
{
    tl_collector_message_t message;

    memcpy(&message, data, sizeof(message));

    if (message.magic != TL_COLLECTOR_MAGIC) {
        return reject(reason, reason_size, "magic de span invalido");
    }

    if (message.version != TL_COLLECTOR_VERSION) {
        return reject(reason, reason_size, "version de span invalida");
    }

    if (message.type != TL_COLLECTOR_MESSAGE_SPAN) {
        return reject(reason, reason_size, "tipo de mensaje no soportado");
    }

    if (message.span_size != (uint32_t)sizeof(tl_span_t)) {
        return reject(reason, reason_size, "span_size incorrecto");
    }

    if (message.reserved != 0u) {
        return reject(reason, reason_size, "campo reserved distinto de 0");
    }

    if (validate_span(&message.span, reason, reason_size) != TL_RECEIVE_SPAN) {
        return TL_RECEIVE_REJECTED;
    }

    *span = message.span;
    return TL_RECEIVE_SPAN;
}

tl_receive_result_t tl_span_receiver_receive(int fd, tl_span_t *span,
                                             char *reason, size_t reason_size)
{
    tl_receiver_buffer_t buffer;
    struct iovec iov;
    struct msghdr message;
    ssize_t received;

    if (span == NULL) {
        errno = EINVAL;
        return TL_RECEIVE_ERROR;
    }

    if (reason != NULL && reason_size > 0) {
        reason[0] = '\0';
    }

    memset(&message, 0, sizeof(message));
    iov.iov_base = buffer.raw;
    iov.iov_len = sizeof(buffer.raw);
    message.msg_iov = &iov;
    message.msg_iovlen = 1;

    received = recvmsg(fd, &message, 0);
    if (received < 0) {
        return TL_RECEIVE_ERROR;
    }

    if ((message.msg_flags & MSG_TRUNC) != 0 ||
        (size_t)received > TL_COLLECTOR_MAX_DATAGRAM) {
        return reject(reason, reason_size,
                      "datagrama truncado o mayor que el maximo");
    }

    if ((size_t)received == sizeof(tl_collector_control_t)) {
        return validate_control(buffer.raw, reason, reason_size);
    }

    if ((size_t)received != sizeof(tl_collector_message_t)) {
        return reject(reason, reason_size, "tamano de datagrama inesperado");
    }

    return validate_message(buffer.raw, span, reason, reason_size);
}

const char *tl_span_status_name(int32_t status)
{
    switch (status) {
    case TL_SPAN_STATUS_OK:
        return "SUCCESS";
    case TL_SPAN_STATUS_ERROR:
        return "ERROR";
    case TL_SPAN_STATUS_TIMEOUT:
        return "TIMEOUT";
    case TL_SPAN_STATUS_CANCELLED:
        return "CANCELLED";
    case TL_SPAN_STATUS_UNSET:
        return "UNSET";
    default:
        return "UNKNOWN";
    }
}
