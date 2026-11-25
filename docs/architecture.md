# TraceLite - Arquitectura

Documento actualizado a la segunda entrega (Fase 2): la biblioteca de
instrumentación sigue integrada con los servicios, pero Service y TraceLite
pueden ejecutarse como aplicaciones independientes.

## Alcance

TraceLite es un sistema ligero de trazabilidad distribuida para GNU/Linux,
escrito en C y ejecutado en un único host. El sistema se divide en dos
proyectos independientes:

- **TraceLite-Core**: biblioteca estática de instrumentación y Collector Lite.
- **Services**: Main / Process Manager, Order Service e Inventory Service.

En el estado actual están implementados:

- El plano de negocio: comunicación síncrona y secuencial entre Order Service e
  Inventory Service.
- La instrumentación real: generación de Trace ID y Span ID, creación y
  finalización de spans, medición con `CLOCK_MONOTONIC` y envío al Collector.
- La propagación del contexto de trazabilidad dentro de la petición de negocio.
- La recepción, validación, agrupación y presentación de spans por el Collector.
- Los tres escenarios de demostración y su verificación automatizada.

## Componentes y responsabilidades

| Componente | Proyecto | Estado | Responsabilidad |
|---|---|---|---|
| Main / Process Manager | Services | Implementado | Punto de entrada único. Verifica ejecutables, crea un directorio runtime temporal, lanza Collector, Inventory y Order con `fork()` + `exec()`, sincroniza el arranque, espera con `waitpid()`, cierra el Collector y limpia recursos. Atiende `SIGINT`/`SIGTERM`. |
| Order Service | Services | Implementado | Cliente del protocolo de negocio. Crea el span raíz `process-order`, propaga el contexto en cada petición, envía ORD-001/002/003 de forma secuencial, espera cada respuesta, fija el estado del span y cierra la conexión. |
| Inventory Service | Services | Implementado | Servidor del protocolo de negocio. Acepta una conexión, procesa exactamente tres peticiones contra un inventario en memoria, crea el span hijo `reserve-stock`, conserva el `trace_id` recibido, simula la demora de ORD-002 y el error de ORD-003, y finaliza. |
| Biblioteca `libtracelite.a` | TraceLite-Core | Implementada | Inicialización, IDs, contextos, ciclo de vida de spans, medición temporal, estados, serialización y envío al Collector. |
| Collector Lite | TraceLite-Core | Implementado | Recibe datagramas `AF_UNIX`/`SOCK_DGRAM`, valida los spans, los agrupa por `trace_id`, reconstruye la jerarquía por `parent_span_id` y los presenta al cerrar. |
| Emisor de pruebas | TraceLite-Core | Implementado | `collector_test_sender` envía spans sintéticos, datagramas malformados y el mensaje `SHUTDOWN`. No se usa en producción. |

## Modelo de procesos

### Ejecución independiente (modo actual)

```text
Terminal A: ./tracelite                    Terminal B: ./service
collector                                  process_manager
  |                                           |-- fork/exec --> inventory_service
  | <--- spans SOCK_DGRAM --------------------|-- fork/exec --> order_service
  |                                           |
  `-- /tmp/tracelite/collector.sock          `-- /tmp/tracelite/inventory.sock
```

`make` crea los enlaces ejecutables `./tracelite` y `./service`. El Collector
usa por defecto `/tmp/tracelite/collector.sock`; la biblioteca permite
sobrescribir esta ruta mediante `TRACELITE_COLLECTOR_SOCKET`. Service no crea,
no detiene ni elimina el socket del Collector. Si el Collector no está activo,
los envíos de spans fallan de forma no bloqueante y el resultado del negocio no
cambia.

Service sigue siendo padre de Inventory y Order, espera ambos con `waitpid()`
y elimina únicamente `inventory.sock`. Si el directorio compartido queda vacío
lo elimina; si TraceLite conserva `collector.sock`, lo deja intacto.

### Modo integrado compatible

```text
process_manager
  |-- fork/exec --> collector          <collector.sock>
  |-- fork/exec --> inventory_service  <inventory.sock> <ready_fd>
  `-- fork/exec --> order_service      <inventory.sock>
```

El modo legado se conserva mediante `./service --integrated` o
`make run-integrated`. En él, los tres procesos son hijos del Process Manager,
que conserva el directorio temporal privado y el cierre por `SHUTDOWN` de la
primera entrega.

Flujo de arranque:

1. `process_manager` instala manejadores de `SIGINT` y `SIGTERM`.
2. Obtiene su propio directorio con `/proc/self/exe` y comprueba que
   `collector`, `inventory_service` y `order_service` sean ejecutables.
3. Crea un directorio temporal único con `mkdtemp("/tmp/tracelite-XXXXXX")`.
4. Lanza el Collector y espera a que cree su socket (comprobación del sistema
   de archivos con límite de tiempo, sin esperas arbitrarias).
5. Exporta `TRACELITE_COLLECTOR_SOCKET` para que los servicios hereden la ruta.
6. Crea una tubería de sincronización y lanza Inventory con `fork()` + `exec()`.
7. Inventory crea, enlaza y pone en escucha su socket; después escribe un byte
   `'R'` en la tubería.
8. `process_manager` bloquea en `read()` hasta recibir la notificación.
9. Lanza Order con `fork()` + `exec()`, pasándole la misma ruta de socket.
10. Espera a Order con `waitpid()`. Si falla, envía `SIGTERM` a Inventory.
11. Espera a Inventory.
12. Envía `SHUTDOWN` al Collector; si el envío falla, usa `SIGTERM`.
13. Espera al Collector y comprueba los tres códigos de salida.
14. Elimina los sockets y el directorio temporal.

Si se recibe `SIGINT` o `SIGTERM`, el Process Manager deja de iniciar hijos
nuevos, termina los que estén vivos, los recoge con `waitpid()` para evitar
zombis y ejecuta la misma limpieza de rutas.

La comunicación de negocio no depende del Process Manager ni del Collector.

## Comunicación de negocio

- Unix Domain Socket `AF_UNIX`, tipo `SOCK_STREAM`.
- Order Service es cliente; Inventory Service es servidor.
- Un único intercambio petición/respuesta por vez.
- Framing explícito: cabecera de 16 bytes + payload de longitud exacta.
- Envío y recepción iteran hasta completar todos los bytes.
- La petición actual incluye `tl_trace_context_t` (16 bytes); el payload mide
  88 bytes en esta compilación.
- El receptor sigue aceptando el payload legado de 68 bytes sin contexto y lo
  interpreta como `{trace_id = 0, parent_span_id = 0}`.
- El protocolo se define en `Services/include/business_protocol.h`.
- La E/S completa se implementa en `business_protocol.c` e `ipc.c`.

Detalle completo: [protocol.md](protocol.md).

## Justificación del mecanismo IPC

Se evaluaron tres mecanismos habituales antes de fijar la solución:

| Mecanismo | Ventajas | Limitaciones para TraceLite |
|---|---|---|
| Pipes (`pipe`/`FIFO`) | Simples y muy rápidos para un flujo unidireccional. | No tienen direccionamiento por nombre; complican un modelo varios-a-uno, el reinicio del receptor y la separación entre negocio y telemetría. Además obligan a framing manual sobre un flujo sin límites de mensaje. |
| Memoria compartida (`shm`) | Máxima velocidad y ausencia de copias. | Requiere sincronización explícita (semáforos o mutex), gestión de ciclo de vida y un protocolo adicional para delimitar mensajes. La complejidad supera el alcance de esta primera versión. |
| Sockets Unix (`AF_UNIX`) | Direccionamiento por ruta, permisos del sistema de archivos, comunicación entre procesos independientes y soporte nativo de flujo y datagramas. | Requieren crear y limpiar el archivo de socket, algo que el Process Manager ya controla. |

La decisión fue usar **sockets Unix** por tres razones:

1. **Procesos realmente independientes.** Los servicios y el Collector tienen ciclos de vida distintos. Un socket con nombre permite que el emisor arranque o se reinicie sin compartir memoria ni heredar descriptores.
2. **Dos semánticas distintas con el mismo mecanismo.**
   - `SOCK_STREAM` para el protocolo de negocio: la petición y la respuesta necesitan orden, entrega completa y control de flujo.
   - `SOCK_DGRAM` para la telemetría: un datagrama equivale a un span, no hace falta framing por longitud y un fallo puntual de envío no bloquea el negocio.
3. **Aislamiento y limpieza sencilla.** Cada ejecución usa un directorio temporal propio con rutas únicas; al terminar se eliminan los sockets. No hay puertos TCP ni conflictos con otras instancias, y el acceso queda restringido por permisos del sistema de archivos.

No se usa TCP porque todos los procesos están en el mismo host y no se necesita comunicación por red.

## Flujo de un pedido con trazabilidad

```text
Order Service                                      Inventory Service
     |                                                    |
     | tl_span_start(process-order, NULL)                 |
     |   trace_id=T, span_id=O                            |
     |-- REQUEST(order, product, qty, ctx{T,O}) --------->|
     |                                                    | tl_span_start(reserve-stock, ctx{T,O})
     |                                                    |   trace_id=T, span_id=I, parent=O
     |                                                    | ...trabajo...
     |                                                    | tl_span_finish(span I)
     |<--------------------- RESPONSE(status) ------------|
     | tl_span_finish(span O)                             |
     |-- close() ---------------------------------------->|
     |                                                    | close()
```

Cada pedido genera una traza independiente:

```text
trace T
├── order-service     process-order   span O   parent = 0
└── inventory-service reserve-stock   span I   parent = O
```

Order Service espera cada respuesta antes de enviar el siguiente pedido. No hay
concurrencia de pedidos y la biblioteca no es thread-safe.

## Instrumentación (`libtracelite.a`)

### API pública

| Función | Responsabilidad |
|---|---|
| `tl_init(service_name)` | Inicializa el estado del proceso y el generador de IDs. Falla con `EALREADY` si ya estaba inicializado. |
| `tl_shutdown()` | Libera el estado de instrumentación. Es seguro llamarla sin `tl_init()`. |
| `tl_span_start(span, operation, parent, out_context)` | Crea un span, registra el inicio con `CLOCK_MONOTONIC` y devuelve el contexto que debe propagarse. |
| `tl_span_finish(span, status, error_message)` | Calcula `duration_ns`, fija estado y mensaje, y envía el span al Collector. |
| `tl_span_make_context(span, out_context)` | Construye el contexto de traza de un span ya iniciado. |
| `tl_context_is_valid(context)` | Indica si un contexto puede continuar una traza (`trace_id != 0`). |
| `tl_collector_request_shutdown(path)` | Envía el mensaje de control `SHUTDOWN` al Collector. |

### Módulos

| Archivo | Responsabilidad |
|---|---|
| `src/lib/tracelite.c` | Inicialización, finalización, ciclo de vida de spans, contextos y validación de estado. |
| `src/lib/id_generator.c` | Generación de Trace ID y Span ID. |
| `src/lib/span_sender.c` | Cliente `AF_UNIX`/`SOCK_DGRAM` del Collector. |
| `src/lib/text_util.c` | Copia segura y truncado de textos. |

La separación es equivalente a la propuesta `span.c` / `trace_context.c` /
`collector_client.c`, pero se mantiene la organización existente para no
romper interfaces ni duplicar módulos que ya cumplen su responsabilidad.

### Generación de identificadores

- Semilla por proceso: `/dev/urandom` (o respaldo local), PID, dirección de
  estado y hora real de inicialización.
- Cada ID se deriva de un **contador global que solo avanza** mediante
  SplitMix64, que es una biyección.
- Consecuencia: no se repiten IDs dentro del mismo proceso, ni entre trazas ni
  entre spans. El valor 0 se descarta porque está reservado para
  "sin identificar".
- Entre procesos distintos la unicidad es probabilística, apoyada en una
  semilla aleatoria de 64 bits.

### Medición temporal

- `clock_gettime(CLOCK_MONOTONIC, ...)` al iniciar y al finalizar.
- `duration_ns = fin - inicio`.
- `CLOCK_MONOTONIC` no se ve afectado por cambios del reloj de pared.

### Estados y errores

- `TL_SPAN_STATUS_OK` se presenta como `SUCCESS`.
- `TL_SPAN_STATUS_ERROR` se usa para errores de negocio y de protocolo.
- `TL_SPAN_STATUS_UNSET` solo existe mientras el span está abierto.
- Un span solo puede finalizarse una vez; la segunda llamada falla con
  `errno = EALREADY` sin modificar datos ni reenviar el span.
- Un fallo de telemetría no invalida el resultado del negocio: el span queda
  finalizado aunque `sendto()` falle.

## Inventario simulado

| Producto | Stock inicial |
|---|---:|
| PRODUCT-001 | 20 |
| PRODUCT-002 | 10 |
| PRODUCT-003 | 5 |

- ORD-001 reserva 5 unidades de PRODUCT-001 y termina con `OK`.
- ORD-002 reserva 3 unidades de PRODUCT-002 y termina con `OK`. Inventory
  espera `TL_ORD002_DELAY_MS = 150` ms **dentro** del intervalo medido por el
  span, por lo que la duración registrada refleja la demora.
- ORD-003 devuelve `SIMULATED_ERROR` sin modificar el inventario.

## Collector Lite

El Collector es un proceso independiente de `Services`. No ejecuta lógica de
pedidos ni conoce el protocolo de negocio.

```text
order_service / inventory_service        collector
        |                                   |
        |-- SPAN (1 datagrama, 288 B) ----> | valida y almacena
        |-- SPAN (1 datagrama, 288 B) ----> |
        |-- SHUTDOWN (1 datagrama, 8 B) --> | imprime, libera y cierra
```

- Socket `AF_UNIX` de tipo `SOCK_DGRAM`.
- Ruta recibida como argumento; por defecto `TL_COLLECTOR_SOCKET_PATH`.
- Cada datagrama contiene exactamente un mensaje; no hay framing de stream.
- El Collector acepta datagramas hasta `TL_COLLECTOR_MAX_DATAGRAM` bytes.

### Módulos

| Archivo | Responsabilidad |
|---|---|
| `src/collector/collector.c` | Argumentos, creación del socket, bucle de recepción, cierre ordenado y limpieza. |
| `src/collector/span_receiver.c` | Recepción de un datagrama y validación completa del mensaje y del span. |
| `src/collector/span_storage.c` | Almacenamiento dinámico agrupado por `trace_id`, búsqueda y liberación. |
| `src/collector/trace_printer.c` | Presentación agrupada y reconstrucción de la jerarquía padre-hijo. |
| `src/collector/collector_test_sender.c` | Emisor de spans sintéticos y casos malformados para pruebas. |

### Recepción y validación

El receptor usa `recvmsg()` y comprueba:

1. Ausencia de truncamiento (`MSG_TRUNC`) y tamaño máximo.
2. Tamaño exacto del datagrama: control (8 bytes) o span (288 bytes en esta
   compilación).
3. `magic`, `version`, `type`, `span_size` y `reserved`.
4. Identificadores no reservados y `parent_span_id != span_id`.
5. Estado de span válido y terminado, `flags == 0`, tiempos no negativos y
   ausencia de desbordamiento en `start + duration`.
6. Terminación de `service_name`, `operation_name` y `error_message` dentro de
   sus buffers; `service_name` y `operation_name` no pueden estar vacíos.

Un datagrama rechazado se registra y se descarta; el Collector sigue
escuchando. Los textos recibidos nunca se tratan como cadenas terminadas sin
haber verificado antes su terminador.

### Agrupación y jerarquía

Los spans se agrupan por `trace_id`. Dentro de cada traza, la relación
padre-hijo se calcula con `parent_span_id`, no con el orden de recepción. El
emisor real envía a veces el hijo antes que el padre, lo que demuestra que la
jerarquía no depende del orden de llegada. Un span cuyo padre no está en la
traza se marca como `huerfano`.

### Memoria

El almacenamiento usa arrays dinámicos con `realloc()`. Cada grupo de traza
crece de forma independiente. `tl_span_storage_destroy()` libera todos los
spans y grupos. No hay persistencia ni bases de datos.

### Cierre

El cierre normal se solicita con un mensaje de control `SHUTDOWN` de 8 bytes.
El Collector también atiende `SIGINT` y `SIGTERM` para cerrar de forma
ordenada. En ambos casos:

1. Deja de recibir mensajes.
2. Imprime los spans almacenados.
3. Libera la memoria.
4. Cierra el socket.
5. Elimina el archivo de socket.
6. Finaliza con código 0 si no hubo errores de E/S.

## Estructura del repositorio

```text
tracelite/
├── Makefile
├── README.md
├── docs/
│   ├── architecture.md
│   ├── build.md
│   └── protocol.md
├── scripts/
│   └── test_tracing.sh
├── Services/
│   ├── Makefile
│   ├── include/
│   │   ├── business_protocol.h
│   │   ├── ipc.h
│   │   └── service_config.h
│   └── src/
│       ├── common/
│       │   ├── business_protocol.c
│       │   └── ipc.c
│       ├── process_manager/
│       │   └── main.c
│       ├── order_service/
│       │   └── order_service.c
│       └── inventory_service/
│           └── inventory_service.c
└── TraceLite-Core/
    ├── Makefile
    ├── scripts/
    │   └── test_collector.sh
    ├── include/tracelite/
    │   ├── collector_protocol.h
    │   ├── config.h
    │   ├── span.h
    │   ├── trace_context.h
    │   └── tracelite.h
    ├── src/
    │   ├── lib/
    │   │   ├── id_generator.c
    │   │   ├── id_generator.h
    │   │   ├── span_sender.c
    │   │   ├── span_sender.h
    │   │   ├── text_util.c
    │   │   ├── text_util.h
    │   │   └── tracelite.c
    │   └── collector/
    │       ├── collector.c
    │       ├── collector_test_sender.c
    │       ├── span_receiver.c
    │       ├── span_receiver.h
    │       ├── span_storage.c
    │       ├── span_storage.h
    │       ├── trace_printer.c
    │       └── trace_printer.h
    └── tests/
        └── test_library.c
```

`build/`, `bin/` y `lib/` son salidas generadas y están excluidas por
`.gitignore`. Las fuentes de `TraceLite-Core/src/lib/` forman parte del
proyecto y no están ignoradas.

## Dependencias entre proyectos

- `Services` incluye los encabezados públicos del core para compartir
  `TL_RUNTIME_DIR`, `tl_trace_context_t`, `tl_span_t` y la API de
  instrumentación.
- `Services` enlaza `TraceLite-Core/lib/libtracelite.a`.
- El Makefile de Services construye la biblioteca automáticamente si falta.
- El Makefile raíz construye primero el core y después Services.
- El Collector solo depende de los encabezados públicos del core.
- No hay dependencias circulares: la biblioteca no conoce los servicios y el
  Collector no conoce el protocolo de negocio.

## Fuera de alcance y limitaciones

- Persistencia: los spans viven en memoria hasta el cierre del Collector.
- Red TCP, múltiples hosts y autenticación.
- Concurrencia: procesamiento secuencial y biblioteca no thread-safe.
- Escenario de negocio fijo de tres pedidos.
- Reintentos de telemetría: un fallo de envío se reporta, pero no se reencola.
