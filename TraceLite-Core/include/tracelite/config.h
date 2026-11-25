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

#ifndef TL_LATENCY_ANOMALY_THRESHOLD_MS
#define TL_LATENCY_ANOMALY_THRESHOLD_MS 400u
#endif

#endif /* TRACELITE_CONFIG_H */
