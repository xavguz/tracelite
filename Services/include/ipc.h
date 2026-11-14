#ifndef TRACELITE_IPC_H
#define TRACELITE_IPC_H

#include <stddef.h>

/*
 * Utilidades de E/S y de sistema de archivos compartidas por los servicios.
 *
 * ipc_send_all() e ipc_recv_all() repiten la operacion hasta transferir
 * exactamente length bytes o hasta que se produce un error. No asumen que
 * send()/recv() completen la transferencia en una sola llamada.
 */

int ipc_send_all(int fd, const void *buffer, size_t length);
int ipc_recv_all(int fd, void *buffer, size_t length);

/*
 * Crea el directorio padre de path con permisos 0700. Si ya existe, verifica
 * que sea un directorio. No crea jerarquias de mas de un nivel.
 */
int ipc_ensure_parent_dir(const char *path);

#endif /* TRACELITE_IPC_H */
