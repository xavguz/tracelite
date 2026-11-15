#include "business_protocol.h"
#include "ipc.h"
#include "service_config.h"
#include "tracelite/tracelite.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct inventory_item {
    const char *product_id;
    uint32_t available;
} inventory_item_t;

static tl_span_status_t span_status_from_stock(int32_t status)
{
    return status == TL_STOCK_STATUS_OK ? TL_SPAN_STATUS_OK
                                        : TL_SPAN_STATUS_ERROR;
}

/*
 * Espera de duracion fija usada por ORD-002. nanosleep() puede interrumpirse
 * por una señal; en ese caso se reanuda con el tiempo restante.
 */
static int simulate_processing_delay(unsigned int delay_ms)
{
    struct timespec remaining;

    remaining.tv_sec = (time_t)(delay_ms / 1000u);
    remaining.tv_nsec = (long)(delay_ms % 1000u) * 1000000L;

    while (nanosleep(&remaining, &remaining) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }

    return 0;
}

static inventory_item_t *find_item(inventory_item_t *items, size_t item_count,
                                   const char *product_id)
{
    size_t index;

    for (index = 0; index < item_count; index++) {
        if (strcmp(items[index].product_id, product_id) == 0) {
            return &items[index];
        }
    }

    return NULL;
}

static void build_response(const tl_stock_request_t *request,
                           inventory_item_t *items, size_t item_count,
                           tl_stock_response_t *response)
{
    inventory_item_t *item;

    memset(response, 0, sizeof(*response));
    snprintf(response->order_id, sizeof(response->order_id), "%s",
             request->order_id);

    if (request->order_id[0] == '\0' || request->product_id[0] == '\0' ||
        request->quantity == 0) {
        response->status = TL_STOCK_STATUS_INVALID_REQUEST;
        snprintf(response->message, sizeof(response->message),
                 "invalid order request");
        return;
    }

    item = find_item(items, item_count, request->product_id);
    if (item == NULL) {
        response->status = TL_STOCK_STATUS_UNKNOWN_PRODUCT;
        snprintf(response->message, sizeof(response->message),
                 "unknown product: %s", request->product_id);
        return;
    }

    response->available_quantity = item->available;

    /*
     * ORD-003 es el pedido reservado para comprobar el camino de error
     * controlado. No modifica el inventario.
     */
    if (strcmp(request->order_id, "ORD-003") == 0) {
        response->status = TL_STOCK_STATUS_SIMULATED_ERROR;
        snprintf(response->message, sizeof(response->message),
                 "simulated inventory failure for %s", request->order_id);
        return;
    }

    if (request->quantity > item->available) {
        response->status = TL_STOCK_STATUS_OUT_OF_STOCK;
        snprintf(response->message, sizeof(response->message),
                 "insufficient stock for %s", request->product_id);
        return;
    }

    item->available -= request->quantity;
    response->status = TL_STOCK_STATUS_OK;
    response->available_quantity = item->available;
    snprintf(response->message, sizeof(response->message),
             "reserved %u unit(s) of %s", request->quantity,
             request->product_id);
}

static int create_listening_socket(const char *socket_path)
{
    struct sockaddr_un address;
    struct stat info;
    size_t path_length;
    int socket_fd;

    if (ipc_ensure_parent_dir(socket_path) < 0) {
        return -1;
    }

    path_length = strlen(socket_path);
    if (path_length >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (lstat(socket_path, &info) == 0) {
        if (!S_ISSOCK(info.st_mode)) {
            errno = EEXIST;
            return -1;
        }
        if (unlink(socket_path) < 0) {
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }

    socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, path_length + 1);

    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved_errno = errno;
        close(socket_fd);
        errno = saved_errno;
        return -1;
    }

    if (listen(socket_fd, 1) < 0) {
        int saved_errno = errno;
        close(socket_fd);
        unlink(socket_path);
        errno = saved_errno;
        return -1;
    }

    return socket_fd;
}

static int parse_ready_fd(const char *text)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value <= STDERR_FILENO || value > INT_MAX) {
        errno = EINVAL;
        return -1;
    }

    return (int)value;
}

static int notify_ready(int ready_fd)
{
    const char ready = 'R';
    ssize_t written;

    do {
        written = write(ready_fd, &ready, 1);
    } while (written < 0 && errno == EINTR);

    if (written != 1) {
        if (written >= 0) {
            errno = EIO;
        }
        close(ready_fd);
        return -1;
    }

    close(ready_fd);
    return 0;
}

static int handle_connection(int client_fd, inventory_item_t *items,
                             size_t item_count, int tracing_enabled)
{
    unsigned int processed = 0;
    unsigned int index;

    for (index = 0; index < TL_TEST_ORDER_COUNT; index++) {
        tl_stock_request_t request;
        tl_stock_response_t response;
        tl_span_t span;
        tl_trace_context_t child_context;
        uint32_t request_id = 0;
        int span_started = 0;

        memset(&request, 0, sizeof(request));
        if (business_recv_request(client_fd, &request, &request_id) < 0) {
            perror("[inventory] business_recv_request");
            return -1;
        }

        /* El receptor no debe confiar en que el texto venga terminado. */
        request.order_id[TL_ORDER_ID_MAX - 1] = '\0';
        request.product_id[TL_PRODUCT_ID_MAX - 1] = '\0';

        if (tracing_enabled) {
            const tl_trace_context_t *parent =
                tl_context_is_valid(&request.trace_context)
                    ? &request.trace_context
                    : NULL;

            if (tl_span_start(&span, "reserve-stock", parent,
                              &child_context) == 0) {
                span_started = 1;
            } else {
                fprintf(stderr, "[inventory] tl_span_start: %s\n",
                        strerror(errno));
            }
        }

        /*
         * ORD-002 representa una operacion lenta. La espera ocurre despues de
         * iniciar el span para que su duracion incluya la demora simulada.
         */
        if (strcmp(request.order_id, "ORD-002") == 0) {
            printf("[inventory] demora simulada de %u ms para %s\n",
                   TL_ORD002_DELAY_MS, request.order_id);
            fflush(stdout);

            if (simulate_processing_delay(TL_ORD002_DELAY_MS) < 0) {
                perror("[inventory] simulate_processing_delay");
                if (span_started) {
                    (void)tl_span_finish(&span, TL_SPAN_STATUS_ERROR,
                                         "simulated delay failed");
                }
                return -1;
            }
        }

        build_response(&request, items, item_count, &response);

        if (business_send_response(client_fd, request_id, &response) < 0) {
            perror("[inventory] business_send_response");
            if (span_started) {
                (void)tl_span_finish(&span, TL_SPAN_STATUS_ERROR,
                                     "business_send_response failed");
            }
            return -1;
        }

        processed++;
        printf("[inventory] req=%u %s %s qty=%u -> %s (available=%u): %s\n",
               request_id, request.order_id, request.product_id,
               request.quantity, business_status_name(response.status),
               response.available_quantity, response.message);
        fflush(stdout);

        if (span_started) {
            tl_span_status_t span_status =
                span_status_from_stock(response.status);
            const char *span_error =
                response.status == TL_STOCK_STATUS_OK ? NULL
                                                      : response.message;

            if (tl_span_finish(&span, span_status, span_error) < 0) {
                fprintf(stderr, "[inventory] tl_span_finish: %s\n",
                        strerror(errno));
            }
        }
    }

    return processed == TL_TEST_ORDER_COUNT ? 0 : -1;
}

int main(int argc, char **argv)
{
    inventory_item_t inventory[] = {
        {"PRODUCT-001", 20},
        {"PRODUCT-002", 10},
        {"PRODUCT-003", 5},
    };
    const char *socket_path;
    int listen_fd = -1;
    int client_fd = -1;
    int ready_fd = -1;
    int result = EXIT_FAILURE;
    int tracing_enabled;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc > 3) {
        fprintf(stderr, "uso: %s [socket_path] [ready_fd]\n", argv[0]);
        return EXIT_FAILURE;
    }

    socket_path = argc > 1 ? argv[1] : TL_INVENTORY_SOCKET_PATH;

    if (argc > 2) {
        ready_fd = parse_ready_fd(argv[2]);
        if (ready_fd < 0) {
            perror("[inventory] ready_fd");
            return EXIT_FAILURE;
        }
    }

    tracing_enabled = tl_init("inventory-service") == 0;
    if (!tracing_enabled) {
        fprintf(stderr, "[inventory] trazabilidad deshabilitada: %s\n",
                strerror(errno));
    }

    listen_fd = create_listening_socket(socket_path);
    if (listen_fd < 0) {
        perror("[inventory] create_listening_socket");
        goto cleanup;
    }

    if (ready_fd >= 0) {
        if (notify_ready(ready_fd) < 0) {
            perror("[inventory] notify_ready");
            ready_fd = -1;
            goto cleanup;
        }
        ready_fd = -1;
    }

    printf("[inventory] escuchando en %s\n", socket_path);
    fflush(stdout);

    do {
        client_fd = accept(listen_fd, NULL, NULL);
    } while (client_fd < 0 && errno == EINTR);

    if (client_fd < 0) {
        perror("[inventory] accept");
        goto cleanup;
    }

    printf("[inventory] conexion aceptada; procesando %u pedidos\n",
           TL_TEST_ORDER_COUNT);
    fflush(stdout);

    if (handle_connection(client_fd, inventory,
                          sizeof(inventory) / sizeof(inventory[0]),
                          tracing_enabled) < 0) {
        goto cleanup;
    }

    printf("[inventory] tres pedidos procesados correctamente\n");
    result = EXIT_SUCCESS;

cleanup:
    if (client_fd >= 0) {
        close(client_fd);
    }
    if (listen_fd >= 0) {
        close(listen_fd);
        if (unlink(socket_path) < 0 && errno != ENOENT) {
            perror("[inventory] unlink");
        }
    }
    if (ready_fd >= 0) {
        close(ready_fd);
    }

    if (tracing_enabled) {
        tl_shutdown();
    }

    return result;
}
