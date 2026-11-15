#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "tracelite/tracelite.h"

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
    /* Sin SA_RESTART: read(), waitpid() y nanosleep() deben poder despertar. */
    action.sa_flags = 0;

    if (sigaction(SIGINT, &action, NULL) < 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &action, NULL) < 0) {
        return -1;
    }

    return 0;
}

static void terminate_child(pid_t pid);

static int get_executable_dir(char *buffer, size_t buffer_size)
{
    char executable_path[PATH_MAX];
    char *separator;
    ssize_t length;

    length = readlink("/proc/self/exe", executable_path,
                      sizeof(executable_path) - 1);
    if (length < 0) {
        return -1;
    }
    if ((size_t)length >= sizeof(executable_path) - 1) {
        errno = ENAMETOOLONG;
        return -1;
    }
    executable_path[length] = '\0';

    separator = strrchr(executable_path, '/');
    if (separator == NULL) {
        errno = ENOENT;
        return -1;
    }
    *separator = '\0';

    if (strlen(executable_path) >= buffer_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    snprintf(buffer, buffer_size, "%s", executable_path);
    return 0;
}

static int build_sibling_path(char *destination, size_t destination_size,
                              const char *directory, const char *name)
{
    int written = snprintf(destination, destination_size, "%s/%s", directory,
                           name);

    if (written < 0 || (size_t)written >= destination_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int require_executable(const char *path)
{
    if (access(path, X_OK) < 0) {
        fprintf(stderr, "[process-manager] ejecutable no disponible %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    return 0;
}

static int wait_for_ready(int read_fd)
{
    char value;
    ssize_t result;

    do {
        result = read(read_fd, &value, 1);
    } while (result < 0 && errno == EINTR && !g_stop_requested);

    if (g_stop_requested) {
        errno = EINTR;
        return -1;
    }

    if (result < 0) {
        return -1;
    }
    if (result != 1) {
        errno = EPIPE;
        return -1;
    }
    if (value != 'R') {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int wait_child(pid_t pid, const char *name)
{
    int status;
    pid_t result;

    for (;;) {
        result = waitpid(pid, &status, 0);
        if (result == pid) {
            break;
        }

        if (result < 0 && errno == EINTR) {
            /*
             * Una señal de interrupcion solicita el cierre. Se termina el hijo
             * y se sigue esperando para recogerlo y no dejar zombis.
             */
            if (g_stop_requested) {
                terminate_child(pid);
            }
            continue;
        }

        fprintf(stderr, "[process-manager] waitpid(%s): %s\n", name,
                strerror(errno));
        return -1;
    }

    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        printf("[process-manager] %s termino con codigo %d\n", name,
               exit_code);
        return exit_code;
    }

    if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        fprintf(stderr, "[process-manager] %s termino por señal %d\n", name,
                signal_number);
        return 128 + signal_number;
    }

    fprintf(stderr, "[process-manager] %s termino de forma inesperada\n", name);
    return -1;
}

static void terminate_child(pid_t pid)
{
    if (pid <= 0) {
        return;
    }

    if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
        fprintf(stderr, "[process-manager] kill(%ld): %s\n", (long)pid,
                strerror(errno));
    }
}

/*
 * Espera a que el Collector cree el archivo de socket. No modifica el
 * Collector: solo comprueba el sistema de archivos con un limite de tiempo.
 */
static int wait_for_socket(const char *path, pid_t pid)
{
    struct stat info;
    struct timespec delay;
    unsigned int attempt;

    delay.tv_sec = 0;
    delay.tv_nsec = 20000000L; /* 20 ms */

    for (attempt = 0; attempt < 250u; attempt++) {
        if (g_stop_requested) {
            errno = EINTR;
            return -1;
        }

        if (lstat(path, &info) == 0) {
            if (S_ISSOCK(info.st_mode)) {
                return 0;
            }
            errno = ENOTSOCK;
            return -1;
        }

        if (errno != ENOENT) {
            return -1;
        }

        if (kill(pid, 0) < 0 && errno == ESRCH) {
            errno = ECHILD;
            return -1;
        }

        (void)nanosleep(&delay, NULL);
    }

    errno = ETIMEDOUT;
    return -1;
}

int main(void)
{
    char executable_dir[PATH_MAX];
    char inventory_path[PATH_MAX];
    char order_path[PATH_MAX];
    char collector_path[PATH_MAX];
    char runtime_dir[] = "/tmp/tracelite-XXXXXX";
    char socket_path[PATH_MAX] = "";
    char collector_socket_path[PATH_MAX] = "";
    int ready_pipe[2] = {-1, -1};
    pid_t inventory_pid = -1;
    pid_t order_pid = -1;
    pid_t collector_pid = -1;
    int inventory_code = -1;
    int order_code = -1;
    int collector_code = -1;
    int runtime_created = 0;
    int socket_path_length;
    int result = EXIT_FAILURE;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (install_signal_handlers() < 0) {
        perror("[process-manager] sigaction");
        goto cleanup;
    }

    if (get_executable_dir(executable_dir, sizeof(executable_dir)) < 0) {
        perror("[process-manager] get_executable_dir");
        goto cleanup;
    }

    /*
     * inventory_service y order_service son hermanos del process_manager.
     * El collector se construye en TraceLite-Core/bin, dos niveles por encima
     * de Services/bin.
     */
    if (build_sibling_path(inventory_path, sizeof(inventory_path),
                           executable_dir, "inventory_service") < 0 ||
        build_sibling_path(order_path, sizeof(order_path), executable_dir,
                           "order_service") < 0 ||
        build_sibling_path(collector_path, sizeof(collector_path),
                           executable_dir,
                           "../../TraceLite-Core/bin/collector") < 0) {
        perror("[process-manager] build_sibling_path");
        goto cleanup;
    }

    if (require_executable(inventory_path) < 0 ||
        require_executable(order_path) < 0 ||
        require_executable(collector_path) < 0) {
        goto cleanup;
    }

    if (mkdtemp(runtime_dir) == NULL) {
        perror("[process-manager] mkdtemp");
        goto cleanup;
    }
    runtime_created = 1;

    socket_path_length = snprintf(socket_path, sizeof(socket_path),
                                  "%s/inventory.sock", runtime_dir);
    if (socket_path_length < 0 ||
        (size_t)socket_path_length >= sizeof(socket_path)) {
        fprintf(stderr, "[process-manager] ruta de socket demasiado larga\n");
        goto cleanup;
    }

    socket_path_length = snprintf(collector_socket_path,
                                  sizeof(collector_socket_path),
                                  "%s/collector.sock", runtime_dir);
    if (socket_path_length < 0 ||
        (size_t)socket_path_length >= sizeof(collector_socket_path)) {
        fprintf(stderr, "[process-manager] ruta del collector demasiado "
                        "larga\n");
        goto cleanup;
    }

    printf("[process-manager] runtime: %s\n", runtime_dir);
    printf("[process-manager] inventario: %s\n", inventory_path);
    printf("[process-manager] pedidos: %s\n", order_path);
    printf("[process-manager] collector: %s\n", collector_path);
    fflush(stdout);

    if (g_stop_requested) {
        errno = EINTR;
        perror("[process-manager] interrumpido antes de iniciar el collector");
        goto cleanup;
    }

    collector_pid = fork();
    if (collector_pid < 0) {
        perror("[process-manager] fork collector");
        goto cleanup;
    }

    if (collector_pid == 0) {
        execl(collector_path, "collector", collector_socket_path,
              (char *)NULL);
        perror("[process-manager] execl collector");
        _exit(127);
    }

    if (wait_for_socket(collector_socket_path, collector_pid) < 0) {
        perror("[process-manager] collector no listo");
        terminate_child(collector_pid);
        collector_code = wait_child(collector_pid, "collector");
        collector_pid = -1;
        goto cleanup;
    }

    printf("[process-manager] collector listo (pid=%ld)\n",
           (long)collector_pid);
    fflush(stdout);

    /*
     * Los servicios heredan la ruta del Collector. Se usa una variable de
     * entorno para no alterar la firma ni el orden de los argumentos
     * existentes de order_service e inventory_service.
     */
    if (setenv("TRACELITE_COLLECTOR_SOCKET", collector_socket_path, 1) < 0) {
        perror("[process-manager] setenv TRACELITE_COLLECTOR_SOCKET");
        goto cleanup;
    }

    if (g_stop_requested) {
        errno = EINTR;
        perror("[process-manager] interrumpido antes de iniciar inventory");
        goto cleanup;
    }

    if (pipe(ready_pipe) < 0) {
        perror("[process-manager] pipe");
        goto cleanup;
    }

    inventory_pid = fork();
    if (inventory_pid < 0) {
        perror("[process-manager] fork inventory_service");
        goto cleanup;
    }

    if (inventory_pid == 0) {
        char ready_fd_argument[16];
        int written;

        close(ready_pipe[0]);
        written = snprintf(ready_fd_argument, sizeof(ready_fd_argument), "%d",
                           ready_pipe[1]);
        if (written < 0 || (size_t)written >= sizeof(ready_fd_argument)) {
            _exit(127);
        }

        execl(inventory_path, "inventory_service", socket_path,
              ready_fd_argument, (char *)NULL);
        perror("[process-manager] execl inventory_service");
        _exit(127);
    }

    close(ready_pipe[1]);
    ready_pipe[1] = -1;

    if (wait_for_ready(ready_pipe[0]) < 0) {
        perror("[process-manager] inventory_service no notifico readiness");
        close(ready_pipe[0]);
        ready_pipe[0] = -1;
        terminate_child(inventory_pid);
        inventory_code = wait_child(inventory_pid, "inventory_service");
        inventory_pid = -1;
        goto cleanup;
    }

    close(ready_pipe[0]);
    ready_pipe[0] = -1;
    printf("[process-manager] inventory_service listo (pid=%ld)\n",
           (long)inventory_pid);
    fflush(stdout);

    if (g_stop_requested) {
        errno = EINTR;
        perror("[process-manager] interrumpido antes de iniciar order");
        terminate_child(inventory_pid);
        inventory_code = wait_child(inventory_pid, "inventory_service");
        inventory_pid = -1;
        goto cleanup;
    }

    order_pid = fork();
    if (order_pid < 0) {
        perror("[process-manager] fork order_service");
        terminate_child(inventory_pid);
        inventory_code = wait_child(inventory_pid, "inventory_service");
        inventory_pid = -1;
        goto cleanup;
    }

    if (order_pid == 0) {
        execl(order_path, "order_service", socket_path, (char *)NULL);
        perror("[process-manager] execl order_service");
        _exit(127);
    }

    printf("[process-manager] order_service iniciado (pid=%ld)\n",
           (long)order_pid);
    fflush(stdout);

    order_code = wait_child(order_pid, "order_service");
    order_pid = -1;

    if (order_code != 0) {
        fprintf(stderr,
                "[process-manager] order_service fallo; cerrando "
                "inventory_service\n");
        terminate_child(inventory_pid);
    }

    inventory_code = wait_child(inventory_pid, "inventory_service");
    inventory_pid = -1;

    if (collector_pid > 0) {
        if (tl_collector_request_shutdown(collector_socket_path) < 0) {
            fprintf(stderr,
                    "[process-manager] SHUTDOWN no enviado (%s); "
                    "usando SIGTERM\n",
                    strerror(errno));
            terminate_child(collector_pid);
        }

        collector_code = wait_child(collector_pid, "collector");
        collector_pid = -1;
    }

    if (order_code == 0 && inventory_code == 0 && collector_code == 0) {
        printf("[process-manager] ejecucion completada correctamente\n");
        result = EXIT_SUCCESS;
    } else {
        fprintf(stderr,
                "[process-manager] fallo: order_service=%d, "
                "inventory_service=%d, collector=%d\n",
                order_code, inventory_code, collector_code);
    }

cleanup:
    if (ready_pipe[0] >= 0) {
        close(ready_pipe[0]);
    }
    if (ready_pipe[1] >= 0) {
        close(ready_pipe[1]);
    }

    if (order_pid > 0) {
        terminate_child(order_pid);
        order_code = wait_child(order_pid, "order_service");
        order_pid = -1;
        (void)order_code;
    }
    if (inventory_pid > 0) {
        terminate_child(inventory_pid);
        inventory_code = wait_child(inventory_pid, "inventory_service");
        inventory_pid = -1;
        (void)inventory_code;
    }
    if (collector_pid > 0) {
        terminate_child(collector_pid);
        collector_code = wait_child(collector_pid, "collector");
        collector_pid = -1;
        (void)collector_code;
    }

    if (socket_path[0] != '\0') {
        if (unlink(socket_path) < 0 && errno != ENOENT) {
            perror("[process-manager] unlink socket");
        }
    }
    if (collector_socket_path[0] != '\0') {
        if (unlink(collector_socket_path) < 0 && errno != ENOENT) {
            perror("[process-manager] unlink collector socket");
        }
    }
    if (runtime_created && rmdir(runtime_dir) < 0 && errno != ENOENT) {
        perror("[process-manager] rmdir runtime");
    }

    return result;
}
