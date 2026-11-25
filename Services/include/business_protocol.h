#ifndef TRACELITE_BUSINESS_PROTOCOL_H
#define TRACELITE_BUSINESS_PROTOCOL_H

#include <stdint.h>
#include "tracelite/trace_context.h"

#define TL_BUSINESS_MAGIC UINT32_C(0x544C4250)
#define TL_BUSINESS_VERSION 2u
#define TL_BUSINESS_MAX_PAYLOAD 1024u
#define TL_PRODUCT_ID_MAX 32u
#define TL_PRODUCT_NAME_MAX 64u
#define TL_MESSAGE_MAX 128u
#define TL_PRODUCT_LIST_MAX 8u

typedef enum tl_business_message_kind { TL_BUSINESS_MESSAGE_REQUEST = 1,
                                        TL_BUSINESS_MESSAGE_RESPONSE = 2 } tl_business_message_kind_t;
typedef enum tl_inventory_operation { TL_INVENTORY_LIST_PRODUCTS = 1,
                                      TL_INVENTORY_FIND_PRODUCT = 2,
                                      TL_INVENTORY_RESERVE_STOCK = 3 } tl_inventory_operation_t;
typedef enum tl_stock_status { TL_STOCK_STATUS_OK = 0,
                               TL_STOCK_STATUS_OUT_OF_STOCK = 1,
                               TL_STOCK_STATUS_INVALID_REQUEST = 2,
                               TL_STOCK_STATUS_INTERNAL_ERROR = 3,
                               TL_STOCK_STATUS_UNKNOWN_PRODUCT = 4 } tl_stock_status_t;

typedef struct tl_business_header { uint32_t magic; uint16_t version; uint16_t kind;
                                    uint32_t payload_len; uint32_t request_id; } tl_business_header_t;
typedef struct tl_inventory_request { uint32_t operation; char product_id[TL_PRODUCT_ID_MAX];
                                      uint32_t quantity; tl_trace_context_t trace_context; } tl_inventory_request_t;
typedef struct tl_product_record { char id[TL_PRODUCT_ID_MAX]; char name[TL_PRODUCT_NAME_MAX];
                                   uint32_t stock; } tl_product_record_t;
typedef struct tl_inventory_response { int32_t status; uint32_t product_count;
                                       tl_product_record_t products[TL_PRODUCT_LIST_MAX];
                                       char message[TL_MESSAGE_MAX]; } tl_inventory_response_t;

_Static_assert(sizeof(tl_business_header_t) == 16u, "cabecera invalida");
_Static_assert(sizeof(tl_inventory_request_t) <= TL_BUSINESS_MAX_PAYLOAD, "peticion grande");
_Static_assert(sizeof(tl_inventory_response_t) <= TL_BUSINESS_MAX_PAYLOAD, "respuesta grande");

int business_send_request(int fd, uint32_t request_id, const tl_inventory_request_t *request);
int business_send_response(int fd, uint32_t request_id, const tl_inventory_response_t *response);
int business_recv_request(int fd, tl_inventory_request_t *request, uint32_t *request_id);
int business_recv_response(int fd, tl_inventory_response_t *response, uint32_t *request_id);
const char *business_status_name(int32_t status);
#endif
