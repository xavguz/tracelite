#include "business_protocol.h"
#include "service_config.h"
#include "tracelite/tracelite.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct order_scenario {
    const char *order_id;
    const char *product_id;
    uint32_t quantity;
} order_scenario_t;

static tl_span_status_t span_status_from_stock(int32_t status)
{
    return status == TL_STOCK_STATUS_OK ? TL_SPAN_STATUS_OK
                                        : TL_SPAN_STATUS_ERROR;
}

static void copy_text(char *destination, size_t destination_size,
                      const char *source)
{
    if (destination_size == 0) {
        return;
    }

    snprintf(destination, destination_size, "%s", source);
}

static int connect_to_socket(const char *socket_path)
{
    struct sockaddr_un address;
    size_t path_length;
    int socket_fd;

    path_length = strlen(socket_path);
    if (path_length >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, path_length + 1);

    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved_errno = errno;
        close(socket_fd);
        errno = saved_errno;
        return -1;
    }

    return socket_fd;
}

int main(int argc, char **argv)
{
    static const order_scenario_t scenarios[] = {
        {"ORD-001", "PRODUCT-001", 5},
        {"ORD-002", "PRODUCT-002", 3},
        {"ORD-003", "PRODUCT-003", 1},
    };
    const char *socket_path;
    size_t scenario_count;
    size_t index;
    int socket_fd;
    int test_ok = 1;
    int tracing_enabled;

    _Static_assert(sizeof(scenarios) / sizeof(scenarios[0]) ==
                       TL_TEST_ORDER_COUNT,
                   "el numero de escenarios debe coincidir con "
                   "TL_TEST_ORDER_COUNT");

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc > 2) {
        fprintf(stderr, "uso: %s [socket_path]\n", argv[0]);
        return EXIT_FAILURE;
    }

    socket_path = argc > 1 ? argv[1] : TL_INVENTORY_SOCKET_PATH;
    scenario_count = sizeof(scenarios) / sizeof(scenarios[0]);

    tracing_enabled = tl_init("order-service") == 0;
    if (!tracing_enabled) {
        fprintf(stderr, "[order] trazabilidad deshabilitada: %s\n",
                strerror(errno));
    }

    socket_fd = connect_to_socket(socket_path);
    if (socket_fd < 0) {
        perror("[order] connect");
        if (tracing_enabled) {
            tl_shutdown();
        }
        return EXIT_FAILURE;
    }

    printf("[order] conectado a %s\n", socket_path);
    fflush(stdout);

    for (index = 0; index < scenario_count; index++) {
        tl_stock_request_t request;
        tl_stock_response_t response;
        tl_span_t span;
        tl_trace_context_t span_context;
        uint32_t request_id = (uint32_t)(index + 1);
        uint32_t response_request_id = 0;
        int32_t expected_status;
        int span_started = 0;
        const order_scenario_t *scenario = &scenarios[index];

        memset(&request, 0, sizeof(request));
        copy_text(request.order_id, sizeof(request.order_id),
                  scenario->order_id);
        copy_text(request.product_id, sizeof(request.product_id),
                  scenario->product_id);
        request.quantity = scenario->quantity;

        if (tracing_enabled) {
            if (tl_span_start(&span, "process-order", NULL,
                              &span_context) == 0) {
                span_started = 1;
                request.trace_context = span_context;
            } else {
                fprintf(stderr, "[order] tl_span_start: %s\n",
                        strerror(errno));
            }
        }

        if (business_send_request(socket_fd, request_id, &request) < 0) {
            perror("[order] business_send_request");
            if (span_started) {
                (void)tl_span_finish(&span, TL_SPAN_STATUS_ERROR,
                                     "business_send_request failed");
            }
            test_ok = 0;
            break;
        }

        memset(&response, 0, sizeof(response));
        if (business_recv_response(socket_fd, &response,
                                   &response_request_id) < 0) {
            perror("[order] business_recv_response");
            if (span_started) {
                (void)tl_span_finish(&span, TL_SPAN_STATUS_ERROR,
                                     "business_recv_response failed");
            }
            test_ok = 0;
            break;
        }

        response.order_id[TL_ORDER_ID_MAX - 1] = '\0';
        response.message[TL_MESSAGE_MAX - 1] = '\0';

        if (response_request_id != request_id) {
            fprintf(stderr,
                    "[order] correlacion invalida: esperado %u, recibido %u\n",
                    request_id, response_request_id);
            test_ok = 0;
        }

        if (strcmp(response.order_id, scenario->order_id) != 0) {
            fprintf(stderr,
                    "[order] order_id inesperado: esperado %s, recibido %s\n",
                    scenario->order_id, response.order_id);
            test_ok = 0;
        }

        printf("[order] req=%u %s %s qty=%u -> %s (available=%u): %s\n",
               request_id, request.order_id, request.product_id,
               request.quantity, business_status_name(response.status),
               response.available_quantity, response.message);
        fflush(stdout);

        expected_status = index + 1 < scenario_count
                              ? TL_STOCK_STATUS_OK
                              : TL_STOCK_STATUS_SIMULATED_ERROR;
        if (response.status != expected_status) {
            fprintf(stderr,
                    "[order] estado inesperado para %s: esperado %s, "
                    "recibido %s\n",
                    scenario->order_id,
                    business_status_name(expected_status),
                    business_status_name(response.status));
            test_ok = 0;
        }

        if (span_started) {
            tl_span_status_t span_status =
                span_status_from_stock(response.status);
            const char *span_error =
                response.status == TL_STOCK_STATUS_OK ? NULL
                                                      : response.message;

            if (tl_span_finish(&span, span_status, span_error) < 0) {
                fprintf(stderr, "[order] tl_span_finish: %s\n",
                        strerror(errno));
            }
        }
    }

    if (close(socket_fd) < 0) {
        perror("[order] close");
        test_ok = 0;
    }

    if (tracing_enabled) {
        tl_shutdown();
    }

    if (test_ok) {
        printf("[order] tres pedidos procesados; error controlado de "
               "ORD-003 verificado\n");
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "[order] la prueba de pedidos fallo\n");
    return EXIT_FAILURE;
}
