# TraceLite: defensa corta del avance

## Idea en una frase

TraceLite demuestra trazabilidad distribuida mínima: un pedido pasa por dos
servicios, ambos generan un span de la misma traza y un Collector independiente
los recibe y muestra su relación padre-hijo.

No pretende reemplazar OpenTelemetry ni Grafana Tempo. Es una implementación
didáctica en C para comprobar los conceptos de procesos, IPC, propagación de
contexto y medición de tiempos.

## Arquitectura que se debe explicar

```text
process_manager
  |-- fork + exec --> collector
  |-- fork + exec --> inventory_service
  `-- fork + exec --> order_service

order_service -- pedido con contexto --> inventory_service
order_service -- span -------------------> collector
inventory_service -- span ---------------> collector
```

- **Process Manager:** crea los procesos y espera su terminación. No procesa
  pedidos ni spans.
- **Order Service:** inicia el pedido y crea el span raíz.
- **Inventory Service:** recibe el pedido y crea el span hijo.
- **Collector:** recibe los spans y los organiza por `trace_id`.

Esta separación prueba que las trazas no son impresiones falsas dentro de un
único programa: cada componente es un proceso GNU/Linux distinto.

## Decisiones mínimas y su justificación

| Decisión | Justificación breve |
|---|---|
| `fork()` + `exec()` | Cumple el requisito de crear procesos independientes. `fork()` crea el hijo y `exec()` carga el programa de cada servicio. |
| Sockets Unix | Son IPC local con una ruta conocida; permiten que los procesos se comuniquen sin compartir memoria ni ser padre e hijo directos. |
| `SOCK_STREAM` para pedidos | Un pedido y su respuesta deben llegar completos y en orden. |
| `SOCK_DGRAM` para spans | Un datagrama contiene exactamente un span; por eso el Collector puede recibir varios emisores sin definir un protocolo de fragmentación. |
| `CLOCK_MONOTONIC` | Mide duración real sin verse afectado si cambia la hora del sistema. |
| IDs de 64 bits | Permiten relacionar los spans de una solicitud sin depender del texto o del orden de llegada. |

No es necesario explicar la implementación interna del generador de IDs, las
validaciones defensivas del Collector ni las funciones auxiliares, salvo que
el profesor lo pregunte. Son detalles de robustez, no la idea principal.

## Cómo se propaga una traza

Para cada pedido, Order crea un span raíz:

```text
trace_id = T
span_id  = O
parent   = 0
```

Luego envía en la petición el contexto `{ trace_id=T, parent_span_id=O }`.
Inventory recibe ese contexto y genera otro identificador propio:

```text
trace_id = T       <- se conserva
span_id  = I       <- lo genera Inventory
parent   = O       <- apunta al span de Order
```

Así el Collector puede reconstruir la traza aunque primero llegue el span hijo
y después el raíz. El `trace_id` responde “¿a qué solicitud pertenece?” y el
`parent_span_id` responde “¿quién inició esta operación?”.

## Qué contiene un span

Cada span registra únicamente lo solicitado por el enunciado:

```text
Trace ID | Span ID | Parent Span ID | servicio | operación |
inicio   | fin     | duración       | estado   | error opcional
```

El inicio se toma al comenzar la operación; al finalizar se calcula la
duración y el fin. Después se envía el span al Collector.

## Demostración de un minuto

1. Ejecutar `make run`.
2. Mostrar que el Process Manager inicia `collector`, `inventory_service` y
   `order_service`.
3. Señalar en la salida final una traza: el span raíz es
   `order-service/process-order` y el hijo es
   `inventory-service/reserve-stock`.
4. Mostrar que ambos tienen el mismo `trace_id` y que el `parent_span_id` del
   hijo coincide con el `span_id` de la raíz.
5. Mostrar `ORD-002`: el span de Inventory tarda aproximadamente 150 ms.
6. Mostrar `ORD-003`: ambos spans aparecen con estado `ERROR` y el mensaje
   de fallo controlado.

Con eso quedan demostrados: procesos, `fork`/`exec`, IPC, contexto, spans,
recolector, visualización y anomalía.

## Respuestas breves para preguntas probables

**¿Por qué no usaron OpenTelemetry?**  El enunciado lo prohíbe; el objetivo
es implementar el mecanismo básico desde Sistemas Operativos.

**¿Por qué no pipes?**  Un pipe es útil para un flujo simple entre procesos
relacionados. Aquí dos servicios deben enviar spans a un recolector
independiente; los sockets Unix tienen direccionamiento por ruta y se adaptan
mejor a ese modelo.

**¿Por qué hay tres pedidos?**  Es el mínimo para demostrar un caso normal,
una demora visible y un error registrado en spans reales.

**¿Qué significa distribuida si corre en un host?**  "Distribuida" se refiere
a que el trabajo y la traza se reparten entre procesos independientes. En este
avance se usa IPC local; una versión posterior podría reemplazar los sockets
Unix por red sin cambiar el modelo de spans.

**¿Cuál es la limitación actual?**  No hay persistencia, dashboard ni varios
hosts. Son extensiones de una versión final, no requisitos de este avance.
