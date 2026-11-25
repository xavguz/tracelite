#include "business_protocol.h"
#include "service_config.h"
#include "tracelite/tracelite.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_to_socket(const char *path)
{
    struct sockaddr_un address;
    size_t length = strlen(path);
    int fd;
    if (length >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, length + 1u);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}

static int read_line(const char *prompt, char *out, size_t size)
{
    size_t length;
    printf("%s", prompt); fflush(stdout);
    if (fgets(out, (int)size, stdin) == NULL) return -1;
    length = strcspn(out, "\n");
    if (out[length] == '\n') out[length] = '\0';
    else { int c; while ((c = getchar()) != '\n' && c != EOF) {} }
    return 0;
}

static const char *operation_name(uint32_t op)
{
    switch (op) {
    case TL_INVENTORY_LIST_PRODUCTS: return "list-products-request";
    case TL_INVENTORY_FIND_PRODUCT: return "find-product-request";
    default: return "process-order";
    }
}

static void print_response(const tl_inventory_response_t *response)
{
    uint32_t i;
    printf("Resultado: %s — %s\n", business_status_name(response->status), response->message);
    for (i = 0; i < response->product_count && i < TL_PRODUCT_LIST_MAX; i++)
        printf("  %s | %s | stock: %u\n", response->products[i].id,
               response->products[i].name, response->products[i].stock);
}

static int parse_quantity(const char *text, uint32_t *quantity)
{
    char *end; unsigned long value;
    errno = 0; value = strtoul(text, &end, 10);
    if (errno || end == text || *end != '\0' || value == 0 || value > UINT32_MAX) return -1;
    *quantity = (uint32_t)value; return 0;
}

static int send_operation(int fd, uint32_t request_id, uint32_t operation,
                          const char *product, uint32_t quantity, int tracing)
{
    tl_inventory_request_t request;
    tl_inventory_response_t response;
    tl_span_t span;
    tl_trace_context_t context;
    uint32_t response_id;
    int span_started = 0;
    memset(&request, 0, sizeof(request));
    request.operation = operation; request.quantity = quantity;
    if (product != NULL) snprintf(request.product_id, sizeof(request.product_id), "%s", product);
    if (tracing && tl_span_start(&span, operation_name(operation), NULL, &context) == 0) {
        request.trace_context = context; span_started = 1;
    }
    if (business_send_request(fd, request_id, &request) < 0 ||
        business_recv_response(fd, &response, &response_id) < 0 || response_id != request_id) {
        perror("[order] comunicacion con inventory");
        if (span_started) (void)tl_span_finish(&span, TL_SPAN_STATUS_ERROR, "inventory communication failed");
        return -1;
    }
    response.message[TL_MESSAGE_MAX - 1u] = '\0';
    print_response(&response);
    if (span_started && tl_span_finish(&span,
        response.status == TL_STOCK_STATUS_OK ? TL_SPAN_STATUS_OK : TL_SPAN_STATUS_ERROR,
        response.status == TL_STOCK_STATUS_OK ? NULL : response.message) < 0)
        fprintf(stderr, "[order] span no enviado: %s\n", strerror(errno));
    return 0;
}

int main(int argc, char **argv)
{
    const char *socket_path = argc > 1 ? argv[1] : TL_INVENTORY_SOCKET_PATH;
    char input[128], product[TL_PRODUCT_ID_MAX], quantity_text[32];
    uint32_t request_id = 1, quantity;
    int fd, tracing, result = EXIT_SUCCESS;
    if (argc > 2) { fprintf(stderr, "uso: %s [socket_path]\n", argv[0]); return EXIT_FAILURE; }
    tracing = tl_init("order-service") == 0;
    fd = connect_to_socket(socket_path);
    if (fd < 0) { perror("[order] connect"); if (tracing) tl_shutdown(); return EXIT_FAILURE; }
    for (;;) {
        printf("\n========== INVENTORY SYSTEM ==========\n\n1. Listar productos\n2. Consultar producto\n3. Realizar pedido\n0. Salir\n\nSeleccione una opcion: ");
        fflush(stdout);
        if (fgets(input, sizeof(input), stdin) == NULL || input[0] == '0') break;
        if (input[0] == '1') { if (send_operation(fd, request_id++, TL_INVENTORY_LIST_PRODUCTS, NULL, 0, tracing) < 0) { result = EXIT_FAILURE; break; } continue; }
        if (input[0] != '2' && input[0] != '3') { puts("Opcion invalida."); continue; }
        if (read_line("ID de producto: ", product, sizeof(product)) < 0 || product[0] == '\0') { puts("ID invalido."); continue; }
        if (input[0] == '2') { if (send_operation(fd, request_id++, TL_INVENTORY_FIND_PRODUCT, product, 0, tracing) < 0) { result = EXIT_FAILURE; break; } continue; }
        if (read_line("Cantidad: ", quantity_text, sizeof(quantity_text)) < 0 || parse_quantity(quantity_text, &quantity) < 0) { puts("Cantidad invalida."); continue; }
        if (send_operation(fd, request_id++, TL_INVENTORY_RESERVE_STOCK, product, quantity, tracing) < 0) { result = EXIT_FAILURE; break; }
    }
    close(fd); if (tracing) tl_shutdown(); return result;
}
