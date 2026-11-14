#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "tracelite/collector_protocol.h"
#include "tracelite/config.h"
#include "tracelite/span.h"

/*
 * Emisor temporal de la Etapa 2.
 *
 * Genera spans sinteticos para comprobar el Collector antes de integrar
 * libtracelite.a. Los tiempos y resultados NO son mediciones reales de los
 * servicios de negocio.
 */

#define TEST_TRACE_A UINT64_C(0x1111111111111111)
#define TEST_TRACE_B UINT64_C(0x2222222222222222)

#define TEST_SPAN_A_ROOT UINT64_C(0x1000000000000001)
#define TEST_SPAN_A_CHILD UINT64_C(0x1000000000000002)
#define TEST_SPAN_B_ROOT UINT64_C(0x2000000000000001)
#define TEST_SPAN_B_CHILD UINT64_C(0x2000000000000002)

static void copy_text(char *destination, size_t destination_size,
                      const char *source)
{
    if (destination_size == 0u) {
        return;
    }

    snprintf(destination, destination_size, "%s", source);
}

static void init_span(tl_span_t *span, uint64_t trace_id, uint64_t span_id,
                      uint64_t parent_span_id, const char *service_name,
                      const char *operation_name, int64_t start_monotonic_ns,
                      int64_t duration_ns, int32_t status,
                      const char *error_message)
{
    memset(span, 0, sizeof(*span));
    span->trace_id = trace_id;
    span->span_id = span_id;
    span->parent_span_id = parent_span_id;
    span->start_monotonic_ns = start_monotonic_ns;
    span->duration_ns = duration_ns;
    span->status = status;
    span->flags = 0u;

    copy_text(span->service_name, sizeof(span->service_name), service_name);
    copy_text(span->operation_name, sizeof(span->operation_name),
              operation_name);
    copy_text(span->error_message, sizeof(span->error_message), error_message);
}

static void build_span_message(tl_collector_message_t *message,
                               const tl_span_t *span)
{
    memset(message, 0, sizeof(*message));
    message->magic = TL_COLLECTOR_MAGIC;
    message->version = TL_COLLECTOR_VERSION;
    message->type = TL_COLLECTOR_MESSAGE_SPAN;
    message->span_size = (uint32_t)sizeof(tl_span_t);
    message->reserved = 0u;
    message->span = *span;
}

static int send_datagram(int fd, const void *data, size_t size,
                         const char *label)
{
    ssize_t sent;

    do {
        sent = send(fd, data, size, 0);
    } while (sent < 0 && errno == EINTR);

    if (sent < 0) {
        fprintf(stderr, "[sender] send(%s): %s\n", label, strerror(errno));
        return -1;
    }

    if ((size_t)sent != size) {
        fprintf(stderr, "[sender] envio parcial en %s: %zd de %zu bytes\n",
                label, sent, size);
        return -1;
    }

    printf("[sender] enviado %s (%zu bytes)\n", label, size);
    return 0;
}

static int send_span(int fd, const tl_span_t *span, const char *label)
{
    tl_collector_message_t message;

    build_span_message(&message, span);
    return send_datagram(fd, &message, sizeof(message), label);
}

static int connect_to_collector(const char *socket_path)
{
    struct sockaddr_un address;
    size_t path_length;
    int socket_fd;

    path_length = strlen(socket_path);
    if (path_length >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, path_length + 1u);

    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved_errno = errno;
        close(socket_fd);
        errno = saved_errno;
        return -1;
    }

    return socket_fd;
}

static int send_malformed(int fd)
{
    tl_span_t base_span;
    tl_collector_message_t message;
    tl_collector_control_t control;
    unsigned char unexpected_size[96];
    unsigned char oversized[TL_COLLECTOR_MAX_DATAGRAM + 64u];

    init_span(&base_span, TEST_TRACE_A, TEST_SPAN_A_ROOT, TL_INVALID_SPAN_ID,
              "order-service", "process-order", INT64_C(1000000000),
              INT64_C(12500000), TL_SPAN_STATUS_OK, "");

    build_span_message(&message, &base_span);
    message.magic = UINT32_C(0xDEADBEEF);
    if (send_datagram(fd, &message, sizeof(message),
                      "malformado: magic incorrecto") < 0) {
        return -1;
    }

    build_span_message(&message, &base_span);
    message.span_size = (uint32_t)(sizeof(tl_span_t) - 1u);
    if (send_datagram(fd, &message, sizeof(message),
                      "malformado: span_size incorrecto") < 0) {
        return -1;
    }

    build_span_message(&message, &base_span);
    message.reserved = 1u;
    if (send_datagram(fd, &message, sizeof(message),
                      "malformado: reserved distinto de 0") < 0) {
        return -1;
    }

    build_span_message(&message, &base_span);
    memset(message.span.service_name, 'A',
           sizeof(message.span.service_name));
    if (send_datagram(fd, &message, sizeof(message),
                      "malformado: service_name sin terminador") < 0) {
        return -1;
    }

    build_span_message(&message, &base_span);
    message.type = TL_COLLECTOR_MESSAGE_SHUTDOWN;
    if (send_datagram(fd, &message, sizeof(message),
                      "malformado: tipo de span incorrecto") < 0) {
        return -1;
    }

    memset(unexpected_size, 0, sizeof(unexpected_size));
    if (send_datagram(fd, unexpected_size, sizeof(unexpected_size),
                      "malformado: tamano inesperado") < 0) {
        return -1;
    }

    memset(oversized, 0x5A, sizeof(oversized));
    if (send_datagram(fd, oversized, sizeof(oversized),
                      "malformado: datagrama excesivo") < 0) {
        return -1;
    }

    control.magic = TL_COLLECTOR_MAGIC;
    control.version = (uint16_t)(TL_COLLECTOR_VERSION + 1u);
    control.type = TL_COLLECTOR_MESSAGE_SHUTDOWN;
    if (send_datagram(fd, &control, sizeof(control),
                      "malformado: version de control incorrecta") < 0) {
        return -1;
    }

    return 0;
}

static int send_valid_spans(int fd)
{
    tl_span_t spans[4];
    static const char *const labels[] = {
        "span A raiz (SUCCESS)", "span A hijo (SUCCESS)",
        "span B raiz (ERROR)", "span B hijo (ERROR)",
    };
    /*
     * Se envian primero los hijos y despues los padres para demostrar que el
     * Collector no depende del orden de llegada: la jerarquia se reconstruye
     * exclusivamente con parent_span_id.
     */
    static const size_t send_order[] = {1u, 0u, 3u, 2u};
    size_t index;

    init_span(&spans[0], TEST_TRACE_A, TEST_SPAN_A_ROOT, TL_INVALID_SPAN_ID,
              "order-service", "process-order", INT64_C(1000000000),
              INT64_C(12500000), TL_SPAN_STATUS_OK, "");
    init_span(&spans[1], TEST_TRACE_A, TEST_SPAN_A_CHILD, TEST_SPAN_A_ROOT,
              "inventory-service", "reserve-stock", INT64_C(1002000000),
              INT64_C(8000000), TL_SPAN_STATUS_OK, "");
    init_span(&spans[2], TEST_TRACE_B, TEST_SPAN_B_ROOT, TL_INVALID_SPAN_ID,
              "order-service", "process-order", INT64_C(2000000000),
              INT64_C(9250000), TL_SPAN_STATUS_ERROR,
              "simulated order failure");
    init_span(&spans[3], TEST_TRACE_B, TEST_SPAN_B_CHILD, TEST_SPAN_B_ROOT,
              "inventory-service", "reserve-stock", INT64_C(2003000000),
              INT64_C(4500000), TL_SPAN_STATUS_ERROR,
              "simulated inventory failure");

    for (index = 0; index < sizeof(send_order) / sizeof(send_order[0]);
         index++) {
        size_t span_index = send_order[index];

        if (send_span(fd, &spans[span_index], labels[span_index]) < 0) {
            return -1;
        }
    }

    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr, "uso: %s [socket_path] [--valid-only]\n", program);
    fprintf(stderr,
            "  socket_path   ruta del socket del Collector "
            "(por defecto %s)\n",
            TL_COLLECTOR_SOCKET_PATH);
    fprintf(stderr,
            "  --valid-only  no envia datagramas malformados de prueba\n");
}

int main(int argc, char **argv)
{
    const char *socket_path = TL_COLLECTOR_SOCKET_PATH;
    int socket_path_set = 0;
    int valid_only = 0;
    tl_collector_control_t shutdown_message;
    int socket_fd;
    int index;

    setvbuf(stdout, NULL, _IOLBF, 0);

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--valid-only") == 0) {
            valid_only = 1;
        } else if (strcmp(argv[index], "--help") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (!socket_path_set) {
            socket_path = argv[index];
            socket_path_set = 1;
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    printf("[sender] datos sinteticos de prueba; no son mediciones reales\n");
    printf("[sender] collector: %s\n", socket_path);

    socket_fd = connect_to_collector(socket_path);
    if (socket_fd < 0) {
        perror("[sender] connect");
        return EXIT_FAILURE;
    }

    if (!valid_only && send_malformed(socket_fd) < 0) {
        close(socket_fd);
        return EXIT_FAILURE;
    }

    if (send_valid_spans(socket_fd) < 0) {
        close(socket_fd);
        return EXIT_FAILURE;
    }

    memset(&shutdown_message, 0, sizeof(shutdown_message));
    shutdown_message.magic = TL_COLLECTOR_MAGIC;
    shutdown_message.version = TL_COLLECTOR_VERSION;
    shutdown_message.type = TL_COLLECTOR_MESSAGE_SHUTDOWN;

    if (send_datagram(socket_fd, &shutdown_message, sizeof(shutdown_message),
                      "SHUTDOWN") < 0) {
        close(socket_fd);
        return EXIT_FAILURE;
    }

    if (close(socket_fd) < 0) {
        perror("[sender] close");
        return EXIT_FAILURE;
    }

    printf("[sender] 4 spans validos y cierre enviados\n");
    return EXIT_SUCCESS;
}
