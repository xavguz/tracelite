#ifndef TRACELITE_BUSINESS_PROTOCOL_H
#define TRACELITE_BUSINESS_PROTOCOL_H

#include <stdint.h>

#include "tracelite/trace_context.h"

/*
 * Protocolo de negocio Order Service <-> Inventory Service sobre
 * Unix Domain Socket SOCK_STREAM.
 *
 * Un socket de flujo no conserva los limites de mensaje, por lo que cada
 * mensaje se envia como:
 *
 *   [ tl_business_header_t ][ payload de payload_len bytes ]
 *
 * Las funciones business_send_*() y business_recv_*() de
 * business_protocol.c implementan el envio/recepcion completa de la cabecera
 * y del payload, sin suponer que send() o recv() transmiten todo de una vez.
 *
 * Se usa un unico intercambio peticion/respuesta síncrono: el cliente envia
 * una peticion y espera la respuesta correspondiente antes de enviar otra.
 *
 * Emisor y receptor se ejecutan en el mismo host y comparten ABI; los campos
 * se envian en el orden y endianness nativos. Las estructuras se inicializan
 * a cero antes de rellenarlas para que los bytes de padding sean deterministas.
 */

#define TL_BUSINESS_MAGIC UINT32_C(0x544C4250) /* "TLBP" */
#define TL_BUSINESS_VERSION 1u

#define TL_BUSINESS_MAX_PAYLOAD 256u
#define TL_ORDER_ID_MAX 32u
#define TL_PRODUCT_ID_MAX 32u
#define TL_MESSAGE_MAX 128u

/*
 * Tamano del payload de peticion anterior a la Etapa 3, cuando todavia no se
 * propagaba el contexto de trazabilidad. El receptor nuevo acepta tanto este
 * tamano como el actual. El discriminante es payload_len, no version.
 */
#define TL_STOCK_REQUEST_LEGACY_SIZE 68u

typedef enum tl_business_message_kind {
    TL_BUSINESS_MESSAGE_REQUEST = 1,
    TL_BUSINESS_MESSAGE_RESPONSE = 2
} tl_business_message_kind_t;

typedef struct tl_business_header {
    uint32_t magic;        /* TL_BUSINESS_MAGIC */
    uint16_t version;      /* TL_BUSINESS_VERSION */
    uint16_t kind;         /* valor de tl_business_message_kind_t */
    uint32_t payload_len;  /* longitud exacta del payload que sigue */
    uint32_t request_id;   /* correlacion peticion/respuesta */
} tl_business_header_t;

typedef enum tl_stock_status {
    TL_STOCK_STATUS_OK = 0,
    TL_STOCK_STATUS_OUT_OF_STOCK = 1,
    TL_STOCK_STATUS_INVALID_REQUEST = 2,
    TL_STOCK_STATUS_SIMULATED_ERROR = 3,
    TL_STOCK_STATUS_INTERNAL_ERROR = 4,
    TL_STOCK_STATUS_UNKNOWN_PRODUCT = 5
} tl_stock_status_t;

typedef struct tl_stock_request {
    char order_id[TL_ORDER_ID_MAX];
    char product_id[TL_PRODUCT_ID_MAX];
    uint32_t quantity;
    /*
     * Etapa 3: contexto de trazabilidad que el emisor propaga al receptor.
     * Un emisor anterior a la Etapa 3 envia TL_STOCK_REQUEST_LEGACY_SIZE
     * bytes y este campo se recibe como {0, 0}.
     */
    tl_trace_context_t trace_context;
} tl_stock_request_t;

typedef struct tl_stock_response {
    char order_id[TL_ORDER_ID_MAX];  /* eco del pedido para verificacion */
    int32_t status;                  /* valor de tl_stock_status_t */
    uint32_t available_quantity;     /* stock disponible tras la operacion */
    char message[TL_MESSAGE_MAX];
} tl_stock_response_t;

_Static_assert(sizeof(tl_business_header_t) == 16u,
               "la cabecera del protocolo de negocio debe ocupar 16 bytes");
_Static_assert(TL_STOCK_REQUEST_LEGACY_SIZE == 68u,
               "el payload legado de peticion debe ocupar 68 bytes");
_Static_assert(sizeof(tl_stock_request_t) > TL_STOCK_REQUEST_LEGACY_SIZE,
               "la peticion actual debe ser mayor que la legada");
_Static_assert(sizeof(tl_stock_request_t) <= TL_BUSINESS_MAX_PAYLOAD,
               "tl_stock_request_t supera TL_BUSINESS_MAX_PAYLOAD");
_Static_assert(sizeof(tl_stock_response_t) <= TL_BUSINESS_MAX_PAYLOAD,
               "tl_stock_response_t supera TL_BUSINESS_MAX_PAYLOAD");

/*
 * Envio y recepcion de mensajes completos.
 *
 * Todas las funciones devuelven 0 en exito o -1 en error, dejando errno
 * establecido. La recepcion valida magic, version, tipo y longitud exacta
 * del payload antes de darlo por valido.
 */
int business_send_request(int fd, uint32_t request_id,
                          const tl_stock_request_t *request);
int business_send_response(int fd, uint32_t request_id,
                           const tl_stock_response_t *response);
int business_recv_request(int fd, tl_stock_request_t *request,
                          uint32_t *request_id);
int business_recv_response(int fd, tl_stock_response_t *response,
                           uint32_t *request_id);

/* Devuelve un nombre legible para un valor de tl_stock_status_t. */
const char *business_status_name(int32_t status);

#endif /* TRACELITE_BUSINESS_PROTOCOL_H */
