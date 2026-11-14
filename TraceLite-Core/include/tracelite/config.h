#ifndef TRACELITE_CONFIG_H
#define TRACELITE_CONFIG_H

/*
 * Valores por defecto de la instalacion local. Las rutas son compartidas por
 * los servicios y el collector; podran hacerse configurables en una etapa
 * posterior sin cambiar las estructuras del protocolo.
 */

#define TL_RUNTIME_DIR "/tmp/tracelite"
#define TL_COLLECTOR_SOCKET_PATH TL_RUNTIME_DIR "/collector.sock"

/* Tamano maximo de un datagrama completo, cabecera incluida. */
#define TL_COLLECTOR_MAX_DATAGRAM 512u

#endif /* TRACELITE_CONFIG_H */
