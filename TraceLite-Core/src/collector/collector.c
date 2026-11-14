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
#include "span_storage.h"
#include "trace_printer.h"
#include "tracelite/config.h"

static volatile sig_atomic_t g_stop_requested = 0;

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

int main(int argc, char **argv)
{
    const char *socket_path;
    tl_span_storage_t storage;
    int socket_fd = -1;
    int socket_bound = 0;
    int fatal_error = 0;
    size_t accepted = 0;
    size_t rejected = 0;
    int exit_code = EXIT_FAILURE;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc > 2) {
        fprintf(stderr, "uso: %s [socket_path]\n", argv[0]);
        return EXIT_FAILURE;
    }

    socket_path = argc > 1 ? argv[1] : TL_COLLECTOR_SOCKET_PATH;
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
            printf("[collector] span aceptado trace_id=0x%016" PRIx64
                   " span_id=0x%016" PRIx64 " service=%.*s\n",
                   span.trace_id, span.span_id,
                   (int)(TL_SERVICE_NAME_MAX - 1u), span.service_name);
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
