#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "span_receiver.h"
#include "anomaly_detector.h"
#include "history.h"
#include "span_persistence.h"
#include "span_storage.h"
#include "trace_printer.h"
#include "tracelite/config.h"

static volatile sig_atomic_t g_stop_requested = 0;

#define TL_TRACES_FILE "traces.jsonl"

static const char *configured_traces_file(void);

static int child_already_explains_anomaly(const tl_span_storage_t *storage,
                                          const tl_span_t *span,
                                          unsigned int flags)
{
    const tl_trace_group_t *group;
    size_t index;

    if (span->parent_span_id != TL_INVALID_SPAN_ID) {
        return 0;
    }
    group = tl_span_storage_find(storage, span->trace_id);
    if (group == NULL) {
        return 0;
    }
    for (index = 0; index < group->span_count; index++) {
        const tl_span_t *candidate = &group->spans[index];

        if (candidate->parent_span_id == span->span_id &&
            (tl_anomaly_detect(candidate) & flags) != 0u) {
            return 1;
        }
    }
    return 0;
}

static int trace_is_complete(const tl_trace_group_t *group)
{
    size_t parent_index;
    size_t child_index;

    for (parent_index = 0; parent_index < group->span_count; parent_index++) {
        if (group->spans[parent_index].parent_span_id != TL_INVALID_SPAN_ID) {
            continue;
        }
        for (child_index = 0; child_index < group->span_count; child_index++) {
            if (group->spans[child_index].parent_span_id ==
                group->spans[parent_index].span_id) {
                return 1;
            }
        }
    }
    return 0;
}

static void print_session_summary(const tl_span_storage_t *storage,
                                  size_t persisted, size_t alerts)
{
    size_t index;
    size_t anomalous = 0;
    size_t complete = 0;

    for (index = 0; index < storage->group_count; index++) {
        const tl_trace_group_t *group = &storage->groups[index];
        size_t span_index;
        unsigned int flags = TL_ANOMALY_NONE;

        for (span_index = 0; span_index < group->span_count; span_index++) {
            flags |= tl_anomaly_detect(&group->spans[span_index]);
        }
        if (flags != TL_ANOMALY_NONE) {
            anomalous++;
        }
        if (trace_is_complete(group)) {
            complete++;
        }
    }

    printf("\n========== TRACELITE SUMMARY ==========\n\n"
           "Total requests:       %zu\n"
           "Spans received:       %zu\n"
           "Spans persisted:      %zu\n"
           "Normal requests:      %zu\n"
           "Anomalous requests:   %zu\n"
           "Complete traces:      %zu\n"
           "Incomplete traces:    %zu\n"
           "Total alerts:         %zu\n\n"
           "=======================================\n",
           storage->group_count, storage->span_count, persisted,
           storage->group_count - anomalous, anomalous, complete,
           storage->group_count - complete, alerts);
}

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    g_stop_requested = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    /* Sin SA_RESTART: recvmsg() debe devolver EINTR para poder cerrar. */
    action.sa_flags = 0;

    if (sigaction(SIGINT, &action, NULL) < 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &action, NULL) < 0) {
        return -1;
    }

    return 0;
}

/*
 * Crea el directorio padre inmediato de path con permisos 0700. Si ya existe,
 * comprueba que sea un directorio. No crea jerarquias de varios niveles.
 */
static int ensure_parent_directory(const char *path)
{
    char directory[PATH_MAX];
    struct stat info;
    char *separator;
    size_t length;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    length = strlen(path);
    if (length >= sizeof(directory)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(directory, path, length + 1u);
    separator = strrchr(directory, '/');
    if (separator == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (separator == directory) {
        return 0;
    }
    *separator = '\0';

    if (mkdir(directory, 0700) < 0) {
        if (errno != EEXIST) {
            return -1;
        }
        if (stat(directory, &info) < 0) {
            return -1;
        }
        if (!S_ISDIR(info.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
    }

    return 0;
}

static int create_bound_socket(const char *socket_path)
{
    struct sockaddr_un address;
    struct stat info;
    size_t path_length;
    int socket_fd;

    path_length = strlen(socket_path);
    if (path_length >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (ensure_parent_directory(socket_path) < 0) {
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

    socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, path_length + 1u);

    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved_errno = errno;
        close(socket_fd);
        errno = saved_errno;
        return -1;
    }

    return socket_fd;
}

static int collector_listen(const char *socket_path, int print_realtime)
{
    tl_span_storage_t storage;
    int socket_fd = -1;
    int socket_bound = 0;
    int fatal_error = 0;
    size_t accepted = 0;
    size_t rejected = 0;
    size_t persisted = 0;
    size_t alerts = 0;
    int exit_code = EXIT_FAILURE;

    setvbuf(stdout, NULL, _IOLBF, 0);

    g_stop_requested = 0;
    tl_span_storage_init(&storage);

    if (install_signal_handlers() < 0) {
        perror("[collector] sigaction");
        goto cleanup;
    }

    socket_fd = create_bound_socket(socket_path);
    if (socket_fd < 0) {
        perror("[collector] socket/bind");
        goto cleanup;
    }
    socket_bound = 1;

    printf("[collector] escuchando en %s (pid=%ld)\n", socket_path,
           (long)getpid());

    while (!g_stop_requested) {
        tl_span_t span;
        char reason[192];
        tl_receive_result_t result;

        result = tl_span_receiver_receive(socket_fd, &span, reason,
                                          sizeof(reason));

        if (result == TL_RECEIVE_SPAN) {
            unsigned int anomaly_flags;

            if (tl_span_storage_add(&storage, &span) < 0) {
                if (errno == EEXIST || errno == EINVAL) {
                    fprintf(stderr,
                            "[collector] span rechazado (trace_id=0x%016"
                            PRIx64 " span_id=0x%016" PRIx64 "): %s\n",
                            span.trace_id, span.span_id, strerror(errno));
                    rejected++;
                    continue;
                }
                perror("[collector] tl_span_storage_add");
                fatal_error = 1;
                break;
            }

            accepted++;
            anomaly_flags = tl_anomaly_detect(&span);
            if (tl_span_persistence_append(configured_traces_file(), &span,
                                           anomaly_flags) < 0) {
                fprintf(stderr, "[collector] no se pudo persistir el span: %s\n",
                        strerror(errno));
            } else {
                persisted++;
            }
            printf("[collector] span aceptado trace_id=0x%016" PRIx64
                   " span_id=0x%016" PRIx64 " service=%.*s\n",
                   span.trace_id, span.span_id,
                   (int)(TL_SERVICE_NAME_MAX - 1u), span.service_name);
            if (!child_already_explains_anomaly(&storage, &span,
                                                anomaly_flags)) {
                tl_anomaly_print_alert(stdout, &span, anomaly_flags);
                if (anomaly_flags != TL_ANOMALY_NONE) {
                    alerts++;
                }
            }
            if (print_realtime && tl_trace_printer_print(&storage, stdout) < 0) {
                perror("[collector] visualizacion en tiempo real");
                fatal_error = 1;
                break;
            }
            continue;
        }

        if (result == TL_RECEIVE_SHUTDOWN) {
            printf("[collector] SHUTDOWN recibido; cerrando\n");
            break;
        }

        if (result == TL_RECEIVE_REJECTED) {
            fprintf(stderr, "[collector] datagrama rechazado: %s\n",
                    reason[0] != '\0' ? reason : "motivo desconocido");
            rejected++;
            continue;
        }

        /* TL_RECEIVE_ERROR */
        if (errno == EINTR) {
            continue;
        }
        perror("[collector] recvmsg");
        fatal_error = 1;
        break;
    }

    if (g_stop_requested) {
        printf("[collector] cierre solicitado por señal\n");
    }

    printf("[collector] spans aceptados=%zu rechazados=%zu\n", accepted,
           rejected);

    if (tl_trace_printer_print(&storage, stdout) < 0) {
        perror("[collector] tl_trace_printer_print");
        fatal_error = 1;
    }
    print_session_summary(&storage, persisted, alerts);

    exit_code = fatal_error ? EXIT_FAILURE : EXIT_SUCCESS;

cleanup:
    tl_span_storage_destroy(&storage);

    if (socket_fd >= 0) {
        close(socket_fd);
    }

    if (socket_bound && unlink(socket_path) < 0 && errno != ENOENT) {
        perror("[collector] unlink socket");
        exit_code = EXIT_FAILURE;
    }

    return exit_code;
}

static const char *configured_socket_path(void)
{
    const char *path = getenv("TRACELITE_COLLECTOR_SOCKET");

    return path != NULL && path[0] != '\0' ? path : TL_COLLECTOR_SOCKET_PATH;
}

static const char *configured_traces_file(void)
{
    const char *path = getenv("TRACELITE_TRACES_FILE");

    return path != NULL && path[0] != '\0' ? path : TL_TRACES_FILE;
}

static void print_menu(void)
{
    printf("\n============= TRACELITE =============\n\n"
           "1. Escuchar servicios\n"
           "2. Historial de trazas\n"
           "0. Salir\n\n"
           "Seleccione una opcion: ");
    fflush(stdout);
}

static void history_menu(void)
{
    char input[64];

    for (;;) {
        printf("\n========== TRACE HISTORY ==========\n\n"
               "1. Listar trazas\n2. Buscar por Trace ID\n"
               "3. Consultar trazas con anomalías\n0. Regresar\n\n"
               "Seleccione una opcion: ");
        fflush(stdout);
        if (fgets(input, sizeof(input), stdin) == NULL || input[0] == '0') {
            return;
        }
        if (input[0] == '1') {
            if (tl_history_list(configured_traces_file(), stdout) < 0)
                perror("[history] lectura");
            continue;
        }
        if (input[0] == '2') {
            printf("Trace ID (decimal o 0x...): ");
            fflush(stdout);
            if (fgets(input, sizeof(input), stdin) == NULL) return;
            input[strcspn(input, "\n")] = '\0';
            if (tl_history_find(configured_traces_file(), input, stdout) < 0)
                perror("[history] busqueda");
            continue;
        }
        if (input[0] == '3') {
            if (tl_history_anomalies(configured_traces_file(), stdout) < 0)
                perror("[history] lectura");
            continue;
        }
        puts("Opcion invalida.");
    }
}

int main(int argc, char **argv)
{
    char input[32];

    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Mantiene el modo directo para pruebas y el modo integrado legado. */
    if (argc == 2) {
        return collector_listen(argv[1], 0);
    }
    if (argc != 1) {
        fprintf(stderr, "uso: %s [socket_path]\n", argv[0]);
        return EXIT_FAILURE;
    }

    for (;;) {
        print_menu();
        if (fgets(input, sizeof(input), stdin) == NULL) {
            if (ferror(stdin) && errno == EINTR) {
                clearerr(stdin);
                continue;
            }
            break;
        }

        if (input[0] == '0') {
            break;
        }
        if (input[0] == '1') {
            printf("[tracelite] modo escucha; Ctrl+C para regresar al menu\n");
            if (collector_listen(configured_socket_path(), 1) != EXIT_SUCCESS) {
                fprintf(stderr, "[tracelite] el modo escucha finalizo con error\n");
            }
            continue;
        }
        if (input[0] == '2') {
            history_menu();
            continue;
        }
        puts("Opcion invalida.");
    }

    return EXIT_SUCCESS;
}
