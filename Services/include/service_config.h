#ifndef TRACELITE_SERVICE_CONFIG_H
#define TRACELITE_SERVICE_CONFIG_H

#include "tracelite/config.h"

/*
 * Rutas de los recursos locales compartidos por los servicios.
 * TL_RUNTIME_DIR y TL_COLLECTOR_SOCKET_PATH provienen de TraceLite-Core.
 */

#define TL_INVENTORY_SOCKET_PATH TL_RUNTIME_DIR "/inventory.sock"

/*
 * Escenario de prueba: Order Service envia exactamente tres pedidos y
 * Inventory Service termina cuando ha procesado los tres.
 */
#define TL_TEST_ORDER_COUNT 3u

/*
 * ORD-002 simula una operacion lenta. La espera se realiza dentro del span de
 * Inventory para que la duracion registrada refleje la demora.
 */
#define TL_ORD002_DELAY_MS 150u

#endif /* TRACELITE_SERVICE_CONFIG_H */
