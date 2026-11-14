#include "business_protocol.h"

#include <errno.h>
#include <string.h>

#include "ipc.h"

static int send_message(int fd, uint16_t kind, uint32_t request_id,
                        const void *payload, uint32_t payload_len)
{
    tl_business_header_t header;

    if (payload_len > TL_BUSINESS_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }

    memset(&header, 0, sizeof(header));
    header.magic = TL_BUSINESS_MAGIC;
    header.version = TL_BUSINESS_VERSION;
    header.kind = kind;
    header.payload_len = payload_len;
    header.request_id = request_id;

    if (ipc_send_all(fd, &header, sizeof(header)) < 0) {
        return -1;
    }
    if (payload_len > 0 && ipc_send_all(fd, payload, payload_len) < 0) {
        return -1;
    }

    return 0;
}

static int recv_message(int fd, uint16_t expected_kind, void *payload,
                        uint32_t payload_capacity,
                        uint32_t legacy_payload_len, uint32_t *request_id)
{
    tl_business_header_t header;

    if (ipc_recv_all(fd, &header, sizeof(header)) < 0) {
        return -1;
    }

    if (header.magic != TL_BUSINESS_MAGIC ||
        header.version != TL_BUSINESS_VERSION ||
        header.kind != expected_kind) {
        errno = EPROTO;
        return -1;
    }

    if (header.payload_len > TL_BUSINESS_MAX_PAYLOAD ||
        header.payload_len > payload_capacity) {
        errno = EMSGSIZE;
        return -1;
    }

    /*
     * Se acepta el tamano actual y, cuando se indica, el tamano legado sin
     * contexto. Cualquier otra longitud se rechaza.
     */
    if (header.payload_len != payload_capacity &&
        (legacy_payload_len == 0 ||
         header.payload_len != legacy_payload_len)) {
        errno = EMSGSIZE;
        return -1;
    }

    if (header.payload_len > 0 &&
        ipc_recv_all(fd, payload, header.payload_len) < 0) {
        return -1;
    }

    if (request_id != NULL) {
        *request_id = header.request_id;
    }

    return 0;
}

int business_send_request(int fd, uint32_t request_id,
                          const tl_stock_request_t *request)
{
    if (request == NULL) {
        errno = EINVAL;
        return -1;
    }

    return send_message(fd, TL_BUSINESS_MESSAGE_REQUEST, request_id, request,
                        (uint32_t)sizeof(*request));
}

int business_send_response(int fd, uint32_t request_id,
                           const tl_stock_response_t *response)
{
    if (response == NULL) {
        errno = EINVAL;
        return -1;
    }

    return send_message(fd, TL_BUSINESS_MESSAGE_RESPONSE, request_id, response,
                        (uint32_t)sizeof(*response));
}

int business_recv_request(int fd, tl_stock_request_t *request,
                          uint32_t *request_id)
{
    if (request == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Se parte de cero para que, si llega una peticion legada sin contexto,
     * tanto el padding como trace_context queden deterministas.
     */
    memset(request, 0, sizeof(*request));

    return recv_message(fd, TL_BUSINESS_MESSAGE_REQUEST, request,
                        (uint32_t)sizeof(*request),
                        TL_STOCK_REQUEST_LEGACY_SIZE, request_id);
}

int business_recv_response(int fd, tl_stock_response_t *response,
                           uint32_t *request_id)
{
    if (response == NULL) {
        errno = EINVAL;
        return -1;
    }

    return recv_message(fd, TL_BUSINESS_MESSAGE_RESPONSE, response,
                        (uint32_t)sizeof(*response), 0u, request_id);
}

const char *business_status_name(int32_t status)
{
    switch (status) {
    case TL_STOCK_STATUS_OK:
        return "OK";
    case TL_STOCK_STATUS_OUT_OF_STOCK:
        return "OUT_OF_STOCK";
    case TL_STOCK_STATUS_INVALID_REQUEST:
        return "INVALID_REQUEST";
    case TL_STOCK_STATUS_SIMULATED_ERROR:
        return "SIMULATED_ERROR";
    case TL_STOCK_STATUS_INTERNAL_ERROR:
        return "INTERNAL_ERROR";
    case TL_STOCK_STATUS_UNKNOWN_PRODUCT:
        return "UNKNOWN_PRODUCT";
    default:
        return "UNKNOWN_STATUS";
    }
}
