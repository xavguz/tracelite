#ifndef TRACELITE_TEXT_UTIL_H
#define TRACELITE_TEXT_UTIL_H

#include <stddef.h>

/*
 * Copia src en dst truncando de forma segura y garantizando el terminador.
 * Si src es NULL se escribe una cadena vacia. dst_size debe ser mayor que 0.
 *
 * Devuelve 0 en exito o -1 en error (errno = EINVAL).
 */
int tl_copy_text(char *dst, size_t dst_size, const char *src);

#endif /* TRACELITE_TEXT_UTIL_H */
