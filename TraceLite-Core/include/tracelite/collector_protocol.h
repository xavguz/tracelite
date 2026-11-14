#ifndef TRACELITE_COLLECTOR_PROTOCOL_H
#define TRACELITE_COLLECTOR_PROTOCOL_H

#include <stdint.h>

#include "config.h"
#include "span.h"

/*
 * Protocolo de envio de spans al Collector mediante
 * Unix Domain Socket SOCK_DGRAM.
 *
 * Cada datagrama contiene exactamente una tl_collector_message_t. Al ser un
 * socket de datagramas no se necesita framing por longitud, pero se mantiene
 * una cabecera con magic/version/type para validar el mensaje.
 *
 * El emisor y el receptor se ejecutan en el mismo host y se compilan con el
 * mismo ABI; por ello los campos se envian en el orden y endianness nativos.
 */

#define TL_COLLECTOR_MAGIC UINT32_C(0x544C4350) /* "TLCP" */
#define TL_COLLECTOR_VERSION 1u

typedef enum tl_collector_message_type {
    TL_COLLECTOR_MESSAGE_SPAN = 1,
    /*
     * Ampliacion compatible de la Etapa 2: mensaje de control sin span.
     * Se envia como un datagrama de exactamente
     * sizeof(tl_collector_control_t) bytes. Los emisores de la Etapa 0/1
     * que solo conocen TL_COLLECTOR_MESSAGE_SPAN no se ven afectados.
     */
    TL_COLLECTOR_MESSAGE_SHUTDOWN = 2
} tl_collector_message_type_t;

typedef struct tl_collector_message {
    uint32_t magic;       /* TL_COLLECTOR_MAGIC */
    uint16_t version;     /* TL_COLLECTOR_VERSION */
    uint16_t type;        /* valor de tl_collector_message_type_t */
    uint32_t span_size;   /* sizeof(tl_span_t) */
    uint32_t reserved;    /* debe valer 0 */
    tl_span_t span;
} tl_collector_message_t;

/*
 * Mensaje de control. El receptor lo distingue de un span por el tamano
 * exacto del datagrama: sizeof(tl_collector_control_t) frente a
 * sizeof(tl_collector_message_t).
 */
typedef struct tl_collector_control {
    uint32_t magic;       /* TL_COLLECTOR_MAGIC */
    uint16_t version;     /* TL_COLLECTOR_VERSION */
    uint16_t type;        /* TL_COLLECTOR_MESSAGE_SHUTDOWN */
} tl_collector_control_t;

_Static_assert(sizeof(tl_collector_message_t) <= TL_COLLECTOR_MAX_DATAGRAM,
               "el datagrama del collector supera TL_COLLECTOR_MAX_DATAGRAM");
_Static_assert(sizeof(tl_collector_control_t) == 8u,
               "el mensaje de control debe ocupar 8 bytes");

#endif /* TRACELITE_COLLECTOR_PROTOCOL_H */
