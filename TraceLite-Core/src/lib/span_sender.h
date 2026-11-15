#ifndef TRACELITE_SPAN_SENDER_H
#define TRACELITE_SPAN_SENDER_H

#include "tracelite/span.h"

/*
 * Envio de spans y mensajes de control al Collector mediante
 * AF_UNIX / SOCK_DGRAM, reutilizando el protocolo de collector_protocol.h.
 *
 * Cada envio crea un socket efimero y usa sendto(). No se mantiene una
 * conexion permanente: asi el Collector puede arrancar despues del servicio
 * o reiniciarse sin dejar al emisor en un estado invalido.
 *
 * Devuelve 0 en exito o -1 en error dejando errno establecido.
 */
int tl_span_sender_send(const char *socket_path, const tl_span_t *span);

/* Envia el mensaje de control SHUTDOWN al Collector. */
int tl_span_sender_shutdown(const char *socket_path);

#endif /* TRACELITE_SPAN_SENDER_H */
