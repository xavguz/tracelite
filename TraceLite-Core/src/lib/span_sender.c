#include "span_sender.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "tracelite/collector_protocol.h"

static int send_datagram(const char *socket_path, const void *data, size_t size,
                         int flags)
{
    struct sockaddr_un address;
    size_t path_length;
    ssize_t sent;
    int socket_fd;
    int saved_errno;

    if (socket_path == NULL || socket_path[0] == '\0' || data == NULL) {
        errno = EINVAL;
        return -1;
    }

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

    do {
        sent = sendto(socket_fd, data, size, flags,
                      (struct sockaddr *)&address, sizeof(address));
    } while (sent < 0 && errno == EINTR);

    saved_errno = errno;
    (void)close(socket_fd);

    if (sent < 0) {
        errno = saved_errno;
        return -1;
    }

    if ((size_t)sent != size) {
        errno = EIO;
        return -1;
    }

    return 0;
}

int tl_span_sender_send(const char *socket_path, const tl_span_t *span)
{
    tl_collector_message_t message;

    if (span == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(&message, 0, sizeof(message));
    message.magic = TL_COLLECTOR_MAGIC;
    message.version = TL_COLLECTOR_VERSION;
    message.type = TL_COLLECTOR_MESSAGE_SPAN;
    message.span_size = (uint32_t)sizeof(tl_span_t);
    message.reserved = 0;
    message.span = *span;

    /*
     * MSG_DONTWAIT evita que una telemetria saturada bloquee el negocio. Si
     * el datagrama no se puede entregar, el span se descarta y se informa.
     */
    return send_datagram(socket_path, &message, sizeof(message), MSG_DONTWAIT);
}

int tl_span_sender_shutdown(const char *socket_path)
{
    tl_collector_control_t control;

    memset(&control, 0, sizeof(control));
    control.magic = TL_COLLECTOR_MAGIC;
    control.version = TL_COLLECTOR_VERSION;
    control.type = TL_COLLECTOR_MESSAGE_SHUTDOWN;

    return send_datagram(socket_path, &control, sizeof(control), 0);
}
