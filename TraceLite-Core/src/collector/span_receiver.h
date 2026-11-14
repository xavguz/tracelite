#ifndef TRACELITE_SPAN_RECEIVER_H
#define TRACELITE_SPAN_RECEIVER_H

#include <stddef.h>

#include "tracelite/span.h"

/*
 * Recepcion y validacion de datagramas del Collector.
 *
 * Cada llamada consume exactamente un datagrama. No se usa framing por
 * longitud: SOCK_DGRAM conserva los limites de mensaje.
 */

typedef enum tl_receive_result {
    TL_RECEIVE_SPAN = 0,      /* span valido; se copia en *span */
    TL_RECEIVE_SHUTDOWN = 1,  /* mensaje de control SHUTDOWN valido */
    TL_RECEIVE_REJECTED = 2,  /* datagrama malformado; reason describe el motivo */
    TL_RECEIVE_ERROR = -1     /* error del sistema; errno queda establecido */
} tl_receive_result_t;

/*
 * Recibe un datagrama y lo valida.
 *
 * En TL_RECEIVE_SPAN, span recibe una copia del span validado.
 * En TL_RECEIVE_REJECTED, reason recibe un texto terminado en '\0'.
 * En TL_RECEIVE_ERROR, errno indica el error; EINTR permite comprobar si el
 * proceso recibio una señal de cierre.
 */
tl_receive_result_t tl_span_receiver_receive(int fd,
                                             tl_span_t *span,
                                             char *reason,
                                             size_t reason_size);

/* Nombre legible de un estado de span. */
const char *tl_span_status_name(int32_t status);

#endif /* TRACELITE_SPAN_RECEIVER_H */
