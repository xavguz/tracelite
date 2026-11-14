#ifndef TRACELITE_SERVICE_CONFIG_H
#define TRACELITE_SERVICE_CONFIG_H

#include "tracelite/config.h"

/*
 * Rutas de los recursos locales compartidos por los servicios.
 * TL_RUNTIME_DIR y TL_COLLECTOR_SOCKET_PATH provienen de TraceLite-Core.
 */

#define TL_INVENTORY_SOCKET_PATH TL_RUNTIME_DIR "/inventory.sock"

/*
 * Escenario de prueba de la Etapa 1: Order Service envia exactamente tres
 * pedidos y Inventory Service termina cuando ha procesado los tres.
 */
#define TL_TEST_ORDER_COUNT 3u

#endif /* TRACELITE_SERVICE_CONFIG_H */
