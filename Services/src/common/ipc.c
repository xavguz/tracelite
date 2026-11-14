#include "ipc.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

int ipc_send_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = send(fd, cursor + sent, length - sent, MSG_NOSIGNAL);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = EPIPE;
            return -1;
        }
        sent += (size_t)result;
    }

    return 0;
}

int ipc_recv_all(int fd, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    size_t received = 0;

    while (received < length) {
        ssize_t result = recv(fd, cursor + received, length - received, 0);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            errno = ECONNRESET;
            return -1;
        }
        received += (size_t)result;
    }

    return 0;
}

int ipc_ensure_parent_dir(const char *path)
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

    memcpy(directory, path, length + 1);
    separator = strrchr(directory, '/');
    if (separator == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (separator == directory) {
        return 0; /* El directorio padre es la raiz. */
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
