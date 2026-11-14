#ifndef TRACELITE_TRACELITE_H
#define TRACELITE_TRACELITE_H

#include "span.h"
#include "trace_context.h"

/*
 * API publica de instrumentacion de TraceLite.
 *
 * Modelo de uso:
 *
 *   tl_init("order-service");
 *   tl_span_start(&span, "process-order", NULL, &ctx); // span raiz
 *   ... trabajo ...
 *   tl_span_finish(&span, TL_SPAN_STATUS_OK, NULL);
 *   tl_shutdown();
 *
 * Para propagar la traza, el emisor incluye el contexto obtenido en
 * tl_span_start() dentro de su peticion. El receptor crea su span usando ese
 * contexto como padre:
 *
 *   tl_span_start(&span, "reserve-stock", &ctx_recibido, &ctx_hijo);
 *
 * La biblioteca no es thread-safe: el diseno actual procesa pedidos de forma
 * secuencial. La implementacion enviara cada span al Collector al finalizar.
 */

/*
 * Inicializa la instrumentacion del proceso actual.
 * service_name se copia y se usa como nombre de servicio en los spans.
 *
 * La ruta del Collector se toma de la variable de entorno
 * TRACELITE_COLLECTOR_SOCKET; si no esta definida se usa
 * TL_COLLECTOR_SOCKET_PATH.
 *
 * Devuelve 0 en exito o -1 en error (errno queda establecido). Un segundo
 * tl_init() sin tl_shutdown() intermedio falla con errno = EALREADY.
 */
int tl_init(const char *service_name);

/*
 * Libera los recursos de la instrumentacion. No envia spans pendientes.
 * Es seguro llamarla aunque tl_init() no se haya ejecutado.
 */
void tl_shutdown(void);

/*
 * Inicia un span y registra su tiempo de comienzo con CLOCK_MONOTONIC.
 *
 * parent puede ser NULL para crear un span raiz. Si out_context no es NULL,
 * se rellena con el contexto que debe propagarse a un posible hijo.
 *
 * Devuelve 0 en exito o -1 en error (errno queda establecido).
 */
int tl_span_start(tl_span_t *span,
                  const char *operation_name,
                  const tl_trace_context_t *parent,
                  tl_trace_context_t *out_context);

/*
 * Finaliza un span, calcula duration_ns, fija status/error_message y lo envia
 * al Collector mediante SOCK_DGRAM.
 *
 * error_message puede ser NULL cuando no hay error. Si se proporciona, se
 * copia de forma truncada y segura.
 *
 * El span queda finalizado aunque el envio falle. Devuelve 0 si se finalizo y
 * se envio correctamente, o -1 en caso contrario (errno establecido). Un
 * fallo de telemetria no debe tratarse como fallo de negocio.
 */
int tl_span_finish(tl_span_t *span,
                   tl_span_status_t status,
                   const char *error_message);

/*
 * Construye el contexto de traza que representa a un span ya iniciado.
 * Devuelve 0 en exito o -1 si los argumentos no son validos.
 */
int tl_span_make_context(const tl_span_t *span,
                         tl_trace_context_t *out_context);

/*
 * Indica si un contexto puede usarse para continuar una traza.
 * Un contexto es valido si no es NULL y su trace_id no es 0.
 */
int tl_context_is_valid(const tl_trace_context_t *context);

/*
 * Envia al Collector un mensaje de control SHUTDOWN por la ruta indicada.
 * Se usa desde el Process Manager para cerrar el Collector de forma
 * ordenada. Devuelve 0 en exito o -1 en error (errno establecido).
 */
int tl_collector_request_shutdown(const char *socket_path);

#endif /* TRACELITE_TRACELITE_H */
