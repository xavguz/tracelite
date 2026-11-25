#ifndef TRACELITE_SERVICE_CONFIG_H
#define TRACELITE_SERVICE_CONFIG_H

#include "tracelite/config.h"

/*
 * Rutas de los recursos locales compartidos por los servicios.
 * TL_RUNTIME_DIR y TL_COLLECTOR_SOCKET_PATH provienen de TraceLite-Core.
 */

#define TL_INVENTORY_SOCKET_PATH TL_RUNTIME_DIR "/inventory.sock"
#define TL_INVENTORY_FILE "data/inventory.json"

/* Parametros iniciales de latencia; se pueden sustituir al compilar. */
#ifndef TL_ORDER_DELAY_MIN_MS
#define TL_ORDER_DELAY_MIN_MS 10u
#endif

#ifndef TL_ORDER_DELAY_MAX_MS
#define TL_ORDER_DELAY_MAX_MS 500u
#endif

#ifndef TL_LATENCY_ANOMALY_THRESHOLD_MS
#define TL_LATENCY_ANOMALY_THRESHOLD_MS 400u
#endif

#endif /* TRACELITE_SERVICE_CONFIG_H */
