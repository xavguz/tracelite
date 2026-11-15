#ifndef TRACELITE_ID_GENERATOR_H
#define TRACELITE_ID_GENERATOR_H

#include <stdint.h>

/*
 * Generacion de identificadores de 64 bits para TraceLite.
 *
 * La semilla de cada proceso combina:
 *   - una fuente aleatoria leida de /dev/urandom (o un respaldo local),
 *   - el PID,
 *   - la hora real en el momento de inicializar el generador.
 *
 * Cada identificador se deriva de un contador global que solo avanza y de la
 * semilla del proceso mediante SplitMix64, que es una biyeccion. Por ello no
 * se repiten identificadores dentro del mismo proceso, ni entre trazas ni
 * entre spans, y se descarta el valor 0 (reservado para "sin identificar").
 *
 * Entre procesos distintos la unicidad es probabilistica: depende de que las
 * semillas no coincidan, algo practicamente imposible con una fuente aleatoria
 * de 64 bits.
 */

typedef enum tl_id_kind {
    TL_ID_KIND_TRACE = 1,
    TL_ID_KIND_SPAN = 2
} tl_id_kind_t;

/* Inicializa el generador. Devuelve 0 o -1 con errno establecido. */
int tl_id_generator_init(void);

/* Libera el estado del generador. */
void tl_id_generator_reset(void);

/*
 * Devuelve un identificador nuevo y distinto de 0. Si el generador no esta
 * inicializado devuelve 0 con errno = EPERM.
 */
uint64_t tl_id_generator_next(tl_id_kind_t kind);

#endif /* TRACELITE_ID_GENERATOR_H */
