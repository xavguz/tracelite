#include "business_protocol.h"
#include <errno.h>
#include <string.h>
#include "ipc.h"

static int send_message(int fd, uint16_t kind, uint32_t id, const void *payload, uint32_t size)
{
    tl_business_header_t header;
    if (size > TL_BUSINESS_MAX_PAYLOAD) { errno = EMSGSIZE; return -1; }
    memset(&header, 0, sizeof(header));
    header.magic = TL_BUSINESS_MAGIC; header.version = TL_BUSINESS_VERSION;
    header.kind = kind; header.payload_len = size; header.request_id = id;
    if (ipc_send_all(fd, &header, sizeof(header)) < 0 || ipc_send_all(fd, payload, size) < 0) return -1;
    return 0;
}
static int recv_message(int fd, uint16_t kind, void *payload, uint32_t size, uint32_t *id)
{
    tl_business_header_t header;
    if (ipc_recv_all(fd, &header, sizeof(header)) < 0) return -1;
    if (header.magic != TL_BUSINESS_MAGIC || header.version != TL_BUSINESS_VERSION || header.kind != kind) { errno = EPROTO; return -1; }
    if (header.payload_len != size || size > TL_BUSINESS_MAX_PAYLOAD) { errno = EMSGSIZE; return -1; }
    if (ipc_recv_all(fd, payload, size) < 0) return -1;
    if (id != NULL) *id = header.request_id;
    return 0;
}
int business_send_request(int fd, uint32_t id, const tl_inventory_request_t *r) { if (!r) { errno = EINVAL; return -1; } return send_message(fd, TL_BUSINESS_MESSAGE_REQUEST, id, r, sizeof(*r)); }
int business_send_response(int fd, uint32_t id, const tl_inventory_response_t *r) { if (!r) { errno = EINVAL; return -1; } return send_message(fd, TL_BUSINESS_MESSAGE_RESPONSE, id, r, sizeof(*r)); }
int business_recv_request(int fd, tl_inventory_request_t *r, uint32_t *id) { if (!r) { errno = EINVAL; return -1; } memset(r, 0, sizeof(*r)); return recv_message(fd, TL_BUSINESS_MESSAGE_REQUEST, r, sizeof(*r), id); }
int business_recv_response(int fd, tl_inventory_response_t *r, uint32_t *id) { if (!r) { errno = EINVAL; return -1; } memset(r, 0, sizeof(*r)); return recv_message(fd, TL_BUSINESS_MESSAGE_RESPONSE, r, sizeof(*r), id); }
const char *business_status_name(int32_t s) { switch (s) { case TL_STOCK_STATUS_OK: return "OK"; case TL_STOCK_STATUS_OUT_OF_STOCK: return "OUT_OF_STOCK"; case TL_STOCK_STATUS_INVALID_REQUEST: return "INVALID_REQUEST"; case TL_STOCK_STATUS_INTERNAL_ERROR: return "INTERNAL_ERROR"; case TL_STOCK_STATUS_UNKNOWN_PRODUCT: return "UNKNOWN_PRODUCT"; default: return "UNKNOWN_STATUS"; } }
