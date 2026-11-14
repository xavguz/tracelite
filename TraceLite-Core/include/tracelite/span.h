#ifndef TRACELITE_SPAN_H
#define TRACELITE_SPAN_H

#include <stdint.h>

/*
 * Un span representa una unidad de trabajo medida con
 * clock_gettime(CLOCK_MONOTONIC).
 *
 * Los identificadores son enteros de 64 bits. El valor 0 se reserva para
 * "sin identificar": un trace_id 0 es invalido y un parent_span_id 0 indica
 * que el span es raiz.
 *
 * Los buffers de texto incluyen el terminador '\0'. Si el texto no cabe, la
 * implementacion debera truncarlo y terminarlo de forma segura.
 */

#define TL_SERVICE_NAME_MAX 32u
#define TL_OPERATION_NAME_MAX 64u
#define TL_ERROR_MESSAGE_MAX 128u

#define TL_INVALID_TRACE_ID UINT64_C(0)
#define TL_INVALID_SPAN_ID UINT64_C(0)

typedef enum tl_span_status {
    TL_SPAN_STATUS_UNSET = 0,
    TL_SPAN_STATUS_OK = 1,
    TL_SPAN_STATUS_ERROR = 2,
    TL_SPAN_STATUS_TIMEOUT = 3,
    TL_SPAN_STATUS_CANCELLED = 4
} tl_span_status_t;

typedef struct tl_span {
    uint64_t trace_id;
    uint64_t span_id;
    uint64_t parent_span_id;      /* 0 si es un span raiz */
    int64_t start_monotonic_ns;   /* clock_gettime(CLOCK_MONOTONIC) */
    int64_t duration_ns;          /* 0 mientras el span esta abierto */
    int32_t status;               /* valor de tl_span_status_t */
    uint32_t flags;               /* reservado; debe valer 0 */
    char service_name[TL_SERVICE_NAME_MAX];
    char operation_name[TL_OPERATION_NAME_MAX];
    char error_message[TL_ERROR_MESSAGE_MAX];
} tl_span_t;

#endif /* TRACELITE_SPAN_H */
