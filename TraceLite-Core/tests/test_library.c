/*
 * Pruebas de libtracelite.a (Etapa 3).
 *
 * Cubre:
 *   - Errores de uso antes y despues de tl_init().
 *   - Ciclo de vida de spans raiz, hijo y nieto.
 *   - Propagacion de contexto y coherencia de identificadores.
 *   - Calculo de duracion y mapeo de estado/mensaje de error.
 *   - Truncado seguro de textos.
 *   - Envio real de spans y de SHUTDOWN por AF_UNIX/SOCK_DGRAM.
 *   - Comportamiento cuando el Collector no esta disponible.
 *   - Unicidad estadistica de los identificadores generados.
 *
 * El programa devuelve 0 si todas las comprobaciones pasan.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "tracelite/collector_protocol.h"
#include "tracelite/tracelite.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
            return -1;                                                       \
        }                                                                    \
    } while (0)

#define CHECK_ERRNO(call, expected)                                          \
    do {                                                                     \
        int check_result;                                                    \
        int saved_errno;                                                     \
        errno = 0;                                                           \
        check_result = (call);                                               \
        saved_errno = errno;                                                 \
        if (check_result != -1 || saved_errno != (expected)) {               \
            fprintf(stderr,                                                  \
                    "FAIL %s:%d: %s devolvio %d, errno=%d (%s); "            \
                    "se esperaba -1 y errno=%d\n",                           \
                    __FILE__, __LINE__, #call, check_result, saved_errno,    \
                    strerror(saved_errno), (expected));                      \
            return -1;                                                       \
        }                                                                    \
    } while (0)

/*
 * Variante de CHECK para funciones que reservan memoria: en lugar de retornar
 * directamente, salta al bloque de limpieza para no dejar fugas en las rutas
 * de fallo.
 */
#define CHECK_GOTO(condition, label)                                         \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
            goto label;                                                      \
        }                                                                    \
    } while (0)

typedef struct test_receiver {
    char runtime_dir[64];
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int fd;
} test_receiver_t;

static int receiver_open(test_receiver_t *receiver)
{
    struct sockaddr_un address;
    size_t path_length;
    char *created;

    memset(receiver, 0, sizeof(*receiver));
    receiver->fd = -1;

    snprintf(receiver->runtime_dir, sizeof(receiver->runtime_dir),
             "/tmp/tracelite-libtests-XXXXXX");
    created = mkdtemp(receiver->runtime_dir);
    if (created == NULL) {
        perror("mkdtemp");
        return -1;
    }

    if (snprintf(receiver->socket_path, sizeof(receiver->socket_path),
                 "%s/collector.sock", receiver->runtime_dir) >=
        (int)sizeof(receiver->socket_path)) {
        fprintf(stderr, "ruta de socket demasiado larga\n");
        return -1;
    }

    path_length = strlen(receiver->socket_path);

    receiver->fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (receiver->fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, receiver->socket_path, path_length + 1u);

    if (bind(receiver->fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        (void)close(receiver->fd);
        receiver->fd = -1;
        return -1;
    }

    return 0;
}

static void receiver_close(test_receiver_t *receiver)
{
    if (receiver->fd >= 0) {
        (void)close(receiver->fd);
        receiver->fd = -1;
    }
    (void)unlink(receiver->socket_path);
    (void)rmdir(receiver->runtime_dir);
}

static ssize_t receive_datagram(int fd, void *buffer, size_t size)
{
    ssize_t received;

    do {
        received = recv(fd, buffer, size, 0);
    } while (received < 0 && errno == EINTR);

    return received;
}

static int check_span_message(int fd, const tl_span_t *expected,
                              tl_span_status_t status,
                              const char *error_message)
{
    tl_collector_message_t message;
    ssize_t received;

    memset(&message, 0, sizeof(message));
    received = receive_datagram(fd, &message, sizeof(message));
    CHECK(received == (ssize_t)sizeof(message));
    CHECK(message.magic == TL_COLLECTOR_MAGIC);
    CHECK(message.version == TL_COLLECTOR_VERSION);
    CHECK(message.type == TL_COLLECTOR_MESSAGE_SPAN);
    CHECK(message.span_size == (uint32_t)sizeof(tl_span_t));
    CHECK(message.reserved == 0u);
    CHECK(message.span.trace_id == expected->trace_id);
    CHECK(message.span.span_id == expected->span_id);
    CHECK(message.span.parent_span_id == expected->parent_span_id);
    CHECK(message.span.service_name[TL_SERVICE_NAME_MAX - 1] == '\0');
    CHECK(strcmp(message.span.service_name, expected->service_name) == 0);
    CHECK(strcmp(message.span.operation_name, expected->operation_name) == 0);
    CHECK(message.span.duration_ns >= 0);
    CHECK(message.span.status == (int32_t)status);
    CHECK(message.span.flags == 0u);
    if (error_message == NULL) {
        CHECK(message.span.error_message[0] == '\0');
    } else {
        CHECK(strcmp(message.span.error_message, error_message) == 0);
    }

    return 0;
}

static int test_before_init(void)
{
    tl_span_t span;
    tl_trace_context_t context = {1u, 1u};

    memset(&span, 0, sizeof(span));

    CHECK_ERRNO(tl_span_start(&span, "op", NULL, NULL), EPERM);
    CHECK_ERRNO(tl_span_finish(&span, TL_SPAN_STATUS_OK, NULL), EPERM);
    CHECK_ERRNO(tl_init(NULL), EINVAL);
    CHECK_ERRNO(tl_init(""), EINVAL);
    CHECK_ERRNO(tl_span_make_context(NULL, &context), EINVAL);
    CHECK_ERRNO(tl_span_make_context(&span, NULL), EINVAL);
    CHECK_ERRNO(tl_collector_request_shutdown(NULL), EINVAL);
    CHECK_ERRNO(tl_collector_request_shutdown(""), EINVAL);

    return 0;
}

static int test_init_and_lifecycle(const test_receiver_t *receiver)
{
    tl_span_t root;
    tl_span_t child;
    tl_span_t sibling;
    tl_trace_context_t root_context;
    tl_trace_context_t child_context;
    tl_trace_context_t sibling_context;
    tl_trace_context_t invalid_context = {TL_INVALID_TRACE_ID, 7u};
    tl_trace_context_t rebuilt;
    tl_collector_control_t control;
    ssize_t received;

    CHECK(tl_init("test-service") == 0);
    /* Segunda inicializacion sin shutdown. */
    CHECK_ERRNO(tl_init("test-service"), EALREADY);

    /* Argumentos invalidos de span_start. */
    CHECK_ERRNO(tl_span_start(NULL, "op", NULL, NULL), EINVAL);
    CHECK_ERRNO(tl_span_start(&root, NULL, NULL, NULL), EINVAL);
    CHECK_ERRNO(tl_span_start(&root, "", NULL, NULL), EINVAL);
    CHECK_ERRNO(tl_span_start(&child, "child", &invalid_context, NULL),
                EINVAL);

    /* Span raiz. */
    memset(&root, 0xa5, sizeof(root));
    memset(&root_context, 0xa5, sizeof(root_context));
    CHECK(tl_span_start(&root, "root-operation", NULL, &root_context) == 0);
    CHECK(root.trace_id != TL_INVALID_TRACE_ID);
    CHECK(root.span_id != TL_INVALID_SPAN_ID);
    CHECK(root.parent_span_id == TL_INVALID_SPAN_ID);
    CHECK(root.start_monotonic_ns >= 0);
    CHECK(root.duration_ns == 0);
    CHECK(root.status == (int32_t)TL_SPAN_STATUS_UNSET);
    CHECK(root.flags == 0u);
    CHECK(strcmp(root.service_name, "test-service") == 0);
    CHECK(strcmp(root.operation_name, "root-operation") == 0);
    CHECK(root.error_message[0] == '\0');
    CHECK(tl_context_is_valid(&root_context));
    CHECK(root_context.trace_id == root.trace_id);
    CHECK(root_context.parent_span_id == root.span_id);

    /* Contexto hijo heredado. */
    CHECK(tl_span_start(&child, "child-operation", &root_context,
                        &child_context) == 0);
    CHECK(child.trace_id == root.trace_id);
    CHECK(child.span_id != root.span_id);
    CHECK(child.parent_span_id == root.span_id);
    CHECK(tl_context_is_valid(&child_context));
    CHECK(child_context.trace_id == child.trace_id);
    CHECK(child_context.parent_span_id == child.span_id);

    /* Otro hijo del mismo padre: hermano del anterior. */
    CHECK(tl_span_start(&sibling, "sibling-operation", &root_context,
                        &sibling_context) == 0);
    CHECK(sibling.trace_id == root.trace_id);
    CHECK(sibling.parent_span_id == root.span_id);
    CHECK(sibling.span_id != child.span_id);
    CHECK(sibling_context.parent_span_id == sibling.span_id);

    /* make_context coincide con el contexto devuelto por span_start. */
    memset(&rebuilt, 0, sizeof(rebuilt));
    CHECK(tl_span_make_context(&child, &rebuilt) == 0);
    CHECK(memcmp(&rebuilt, &child_context, sizeof(rebuilt)) == 0);
    CHECK(tl_context_is_valid(NULL) == 0);
    CHECK(tl_context_is_valid(&invalid_context) == 0);

    /* Finalizacion invalida. */
    CHECK_ERRNO(tl_span_finish(NULL, TL_SPAN_STATUS_OK, NULL), EINVAL);
    CHECK_ERRNO(tl_span_finish(&root, TL_SPAN_STATUS_UNSET, NULL), EINVAL);
    CHECK_ERRNO(tl_span_finish(&root, (tl_span_status_t)99, NULL), EINVAL);

    /* Raiz correcta. */
    CHECK(tl_span_finish(&root, TL_SPAN_STATUS_OK, NULL) == 0);
    CHECK(root.status == (int32_t)TL_SPAN_STATUS_OK);
    CHECK(root.duration_ns >= 0);
    CHECK(check_span_message(receiver->fd, &root, TL_SPAN_STATUS_OK,
                             NULL) == 0);

    /* Un span finalizado no puede volver a finalizarse ni reenviarse. */
    CHECK_ERRNO(tl_span_finish(&root, TL_SPAN_STATUS_OK, NULL), EALREADY);

    /* Hijo con error: el mensaje se transporta intacto. */
    CHECK(tl_span_finish(&child, TL_SPAN_STATUS_ERROR,
                         "simulated inventory failure") == 0);
    CHECK(child.status == (int32_t)TL_SPAN_STATUS_ERROR);
    CHECK(child.duration_ns >= 0);
    CHECK(check_span_message(receiver->fd, &child, TL_SPAN_STATUS_ERROR,
                             "simulated inventory failure") == 0);

    /* Hermano cancelado. */
    CHECK(tl_span_finish(&sibling, TL_SPAN_STATUS_CANCELLED, NULL) == 0);
    CHECK(check_span_message(receiver->fd, &sibling, TL_SPAN_STATUS_CANCELLED,
                             NULL) == 0);

    /* SHUTDOWN reutiliza el protocolo existente: 8 bytes exactos. */
    CHECK(tl_collector_request_shutdown(receiver->socket_path) == 0);
    memset(&control, 0, sizeof(control));
    received = receive_datagram(receiver->fd, &control, sizeof(control));
    CHECK(received == (ssize_t)sizeof(control));
    CHECK(control.magic == TL_COLLECTOR_MAGIC);
    CHECK(control.version == TL_COLLECTOR_VERSION);
    CHECK(control.type == TL_COLLECTOR_MESSAGE_SHUTDOWN);

    return 0;
}

static int test_duration_measurement(const test_receiver_t *receiver)
{
    const int64_t requested_ns = INT64_C(10000000); /* 10 ms */
    const int64_t minimum_ns = INT64_C(8000000);    /* margen de 2 ms */
    const int64_t maximum_ns = INT64_C(5000000000); /* 5 s */
    struct timespec remaining;
    tl_span_t span;
    tl_trace_context_t context;

    tl_shutdown();
    CHECK(tl_init("duration-service") == 0);

    CHECK(tl_span_start(&span, "measure-duration", NULL, &context) == 0);

    remaining.tv_sec = 0;
    remaining.tv_nsec = (long)requested_ns;
    while (nanosleep(&remaining, &remaining) < 0) {
        CHECK(errno == EINTR);
    }

    CHECK(tl_span_finish(&span, TL_SPAN_STATUS_OK, NULL) == 0);
    CHECK(span.duration_ns >= minimum_ns);
    CHECK(span.duration_ns < maximum_ns);
    CHECK(check_span_message(receiver->fd, &span, TL_SPAN_STATUS_OK,
                             NULL) == 0);

    tl_shutdown();
    return 0;
}

static int test_truncation(const test_receiver_t *receiver)
{
    char long_service[TL_SERVICE_NAME_MAX + 20u];
    char long_operation[TL_OPERATION_NAME_MAX + 20u];
    char long_error[TL_ERROR_MESSAGE_MAX + 20u];
    tl_span_t span;
    tl_trace_context_t context;

    memset(long_service, 'S', sizeof(long_service) - 1u);
    long_service[sizeof(long_service) - 1u] = '\0';
    memset(long_operation, 'O', sizeof(long_operation) - 1u);
    long_operation[sizeof(long_operation) - 1u] = '\0';
    memset(long_error, 'E', sizeof(long_error) - 1u);
    long_error[sizeof(long_error) - 1u] = '\0';

    tl_shutdown();
    CHECK(tl_init(long_service) == 0);

    CHECK(tl_span_start(&span, long_operation, NULL, &context) == 0);
    CHECK(strlen(span.service_name) == TL_SERVICE_NAME_MAX - 1u);
    CHECK(strlen(span.operation_name) == TL_OPERATION_NAME_MAX - 1u);
    CHECK(span.service_name[TL_SERVICE_NAME_MAX - 1u] == '\0');
    CHECK(span.operation_name[TL_OPERATION_NAME_MAX - 1u] == '\0');
    CHECK(strncmp(span.service_name, long_service,
                  TL_SERVICE_NAME_MAX - 1u) == 0);
    CHECK(strncmp(span.operation_name, long_operation,
                  TL_OPERATION_NAME_MAX - 1u) == 0);

    CHECK(tl_span_finish(&span, TL_SPAN_STATUS_TIMEOUT, long_error) == 0);
    CHECK(strlen(span.error_message) == TL_ERROR_MESSAGE_MAX - 1u);
    CHECK(span.error_message[TL_ERROR_MESSAGE_MAX - 1u] == '\0');
    CHECK(strncmp(span.error_message, long_error,
                  TL_ERROR_MESSAGE_MAX - 1u) == 0);
    CHECK(check_span_message(receiver->fd, &span, TL_SPAN_STATUS_TIMEOUT,
                             span.error_message) == 0);

    tl_shutdown();
    return 0;
}

static int test_id_uniqueness(void)
{
    const size_t sample_size = 10000u;
    uint64_t *trace_ids;
    uint64_t *span_ids;
    tl_span_t span;
    tl_trace_context_t context;
    size_t index;
    size_t other;
    int result = -1;

    trace_ids = calloc(sample_size, sizeof(*trace_ids));
    span_ids = calloc(sample_size, sizeof(*span_ids));
    if (trace_ids == NULL || span_ids == NULL) {
        fprintf(stderr, "calloc: memoria insuficiente\n");
        goto cleanup;
    }

    CHECK_GOTO(tl_init("id-test") == 0, cleanup);

    for (index = 0; index < sample_size; index++) {
        CHECK_GOTO(tl_span_start(&span, "generate", NULL, &context) == 0,
                   cleanup);
        CHECK_GOTO(span.trace_id != TL_INVALID_TRACE_ID, cleanup);
        CHECK_GOTO(span.span_id != TL_INVALID_SPAN_ID, cleanup);
        CHECK_GOTO(span.parent_span_id == TL_INVALID_SPAN_ID, cleanup);
        trace_ids[index] = span.trace_id;
        span_ids[index] = span.span_id;
    }

    /*
     * Comprobacion O(n^2) deliberada: el objetivo es detectar repeticiones
     * reales en la muestra, no medir rendimiento.
     */
    for (index = 0; index < sample_size; index++) {
        for (other = index + 1u; other < sample_size; other++) {
            CHECK_GOTO(trace_ids[index] != trace_ids[other], cleanup);
            CHECK_GOTO(span_ids[index] != span_ids[other], cleanup);
        }
    }

    result = 0;

cleanup:
    tl_shutdown();
    free(trace_ids);
    free(span_ids);
    return result;
}

static int test_collector_unavailable(const test_receiver_t *receiver)
{
    char missing_path[sizeof(receiver->socket_path)];
    tl_span_t span;
    tl_trace_context_t context;

    CHECK(snprintf(missing_path, sizeof(missing_path), "%s/missing.sock",
                   receiver->runtime_dir) > 0);

    CHECK(setenv("TRACELITE_COLLECTOR_SOCKET", missing_path, 1) == 0);
    CHECK(tl_init("offline-service") == 0);

    CHECK(tl_span_start(&span, "offline-operation", NULL, &context) == 0);
    /*
     * El envio falla, pero el span debe quedar correctamente finalizado. Un
     * fallo de telemetria no invalida el resultado del negocio.
     */
    CHECK(tl_span_finish(&span, TL_SPAN_STATUS_OK, NULL) == -1);
    CHECK(span.status == (int32_t)TL_SPAN_STATUS_OK);
    CHECK(span.duration_ns >= 0);

    tl_shutdown();

    /* Tras shutdown, la API vuelve a exigir inicializacion. */
    CHECK_ERRNO(tl_span_start(&span, "after-shutdown", NULL, NULL), EPERM);
    tl_shutdown();

    return 0;
}

int main(void)
{
    test_receiver_t receiver;

    if (test_before_init() < 0) {
        return EXIT_FAILURE;
    }

    if (receiver_open(&receiver) < 0) {
        return EXIT_FAILURE;
    }

    if (setenv("TRACELITE_COLLECTOR_SOCKET", receiver.socket_path, 1) < 0) {
        perror("setenv");
        receiver_close(&receiver);
        return EXIT_FAILURE;
    }

    if (test_init_and_lifecycle(&receiver) < 0 ||
        test_duration_measurement(&receiver) < 0 ||
        test_truncation(&receiver) < 0 ||
        test_id_uniqueness() < 0 ||
        test_collector_unavailable(&receiver) < 0) {
        receiver_close(&receiver);
        return EXIT_FAILURE;
    }

    receiver_close(&receiver);
    printf("test-library: OK\n");
    return EXIT_SUCCESS;
}
