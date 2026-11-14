# TraceLite - Arquitectura

Documento actualizado a la Etapa 2: servicios de negocio y Collector Lite
implementados. La biblioteca de instrumentación sigue fuera de alcance.

## Alcance

TraceLite es un sistema ligero de trazabilidad distribuida para GNU/Linux,
escrito en C y ejecutado en un único host. El sistema se divide en dos
proyectos independientes:

- **TraceLite-Core**: biblioteca estática de instrumentación y Collector Lite.
- **Services**: Main / Process Manager, Order Service e Inventory Service.

En la Etapa 2 están implementados el plano de negocio (comunicación síncrona
y secuencial entre Order Service e Inventory Service) y la recepción de
telemetría por parte del Collector Lite. Los servicios todavía no generan ni
envían spans.

## Componentes y responsabilidades

| Componente | Proyecto | Estado | Responsabilidad |
|---|---|---|---|
| Main / Process Manager | Services | Implementado | Punto de entrada único. Verifica ejecutables, crea un directorio runtime temporal, lanza Inventory y Order con `fork()` + `exec()`, sincroniza el arranque, espera con `waitpid()` y limpia recursos. |
| Order Service | Services | Implementado | Se conecta a Inventory, envía los pedidos ORD-001, ORD-002 y ORD-003 de forma secuencial, espera cada respuesta, valida el resultado y cierra la conexión. |
| Inventory Service | Services | Implementado | Escucha en un Unix Domain Socket `SOCK_STREAM`, acepta una conexión, procesa exactamente tres peticiones contra un inventario en memoria, simula el error de ORD-003 y finaliza. |
| Biblioteca `libtracelite.a` | TraceLite-Core | Pendiente | Instrumentación, IDs, spans y envío al Collector. |
| Collector Lite | TraceLite-Core | Implementado | Recibe datagramas `AF_UNIX`/`SOCK_DGRAM`, valida los spans, los agrupa por `trace_id`, reconstruye la jerarquía por `parent_span_id` y los presenta al cerrar. |
| Emisor de pruebas | TraceLite-Core | Implementado | `collector_test_sender` envía spans sintéticos, datagramas malformados y el mensaje `SHUTDOWN`. No se usa en producción. |

## Modelo de procesos

```text
process_manager
  |-- fork/exec --> inventory_service <socket_path> <ready_fd>
  `-- fork/exec --> order_service <socket_path>
```

Flujo de arranque:

1. `process_manager` obtiene su propio directorio con `/proc/self/exe` y
   comprueba que `inventory_service` y `order_service` sean ejecutables.
2. Crea un directorio temporal único con `mkdtemp("/tmp/tracelite-XXXXXX")`.
3. Crea una tubería de sincronización y lanza Inventory con `fork()` + `exec()`.
4. Inventory crea, enlaza y pone en escucha el socket; después escribe un byte
   `'R'` en la tubería.
5. `process_manager` bloquea en `read()` hasta recibir la notificación. No usa
   esperas arbitrarias.
6. Lanza Order con `fork()` + `exec()`, pasándole la misma ruta de socket.
7. Espera a Order con `waitpid()`. Si falla, envía `SIGTERM` a Inventory.
8. Espera a Inventory, comprueba ambos códigos de salida y elimina el socket y
   el directorio temporal.

La comunicación de negocio no depende de Process Manager.

## Comunicación

- Unix Domain Socket `AF_UNIX`, tipo `SOCK_STREAM`.
- Order Service es cliente; Inventory Service es servidor.
- Un único intercambio petición/respuesta por vez.
- Framing explícito: cabecera de 16 bytes + payload de longitud exacta.
- Envío y recepción iteran hasta completar todos los bytes.
- El protocolo se define en `Services/include/business_protocol.h`.
- La E/S completa se implementa en `business_protocol.c` e `ipc.c`.

Detalle completo: [protocol.md](protocol.md).

## Flujo de un pedido

```text
Order Service                              Inventory Service
     |                                            |
     |-- connect(socket) -----------------------> |
     |-- REQUEST(ORD-001, PRODUCT-001, 5) ------> |
     |<--------------------- RESPONSE(OK, 15) --- |
     |-- REQUEST(ORD-002, PRODUCT-002, 3) ------> |
     |<--------------------- RESPONSE(OK, 7) ---- |
     |-- REQUEST(ORD-003, PRODUCT-003, 1) ------> |
     |<-------- RESPONSE(SIMULATED_ERROR, 5) ---- |
     |-- close() -------------------------------> |
     |                                            | close()
```

Order Service espera cada respuesta antes de enviar el siguiente pedido. No
hay concurrencia de pedidos.

## Inventario simulado

| Producto | Stock inicial |
|---|---:|
| PRODUCT-001 | 20 |
| PRODUCT-002 | 10 |
| PRODUCT-003 | 5 |

- ORD-001 reserva 5 unidades de PRODUCT-001 y termina con `OK`.
- ORD-002 reserva 3 unidades de PRODUCT-002 y termina con `OK`.
- ORD-003 devuelve `SIMULATED_ERROR` sin modificar el inventario.

## Collector Lite

El Collector es un proceso independiente de `Services`. No ejecuta lógica de
pedidos ni conoce el protocolo de negocio.

```text
collector_test_sender (Etapa 2)      collector
        |                               |
        |-- SPAN (1 datagrama) -------> | valida y almacena
        |-- SPAN (1 datagrama) -------> |
        |-- SHUTDOWN -----------------> | imprime, libera y cierra
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
| `src/collector/collector_test_sender.c` | Emisor temporal de spans sintéticos y casos malformados. |

### Recepción y validación

El receptor usa `recvmsg()` y comprueba:

1. Ausencia de truncamiento (`MSG_TRUNC`) y tamaño máximo.
2. Tamaño exacto del datagrama: control (`sizeof(tl_collector_control_t)`) o
   span (`sizeof(tl_collector_message_t)`).
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
emisor de pruebas envía primero los hijos y después los padres para demostrar
esta propiedad. Un span cuyo padre no está en la traza se marca como
`huerfano`.

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
    └── src/
        ├── lib/
        └── collector/
            ├── collector.c
            ├── collector_test_sender.c
            ├── span_receiver.c
            ├── span_receiver.h
            ├── span_storage.c
            ├── span_storage.h
            ├── trace_printer.c
            └── trace_printer.h
```

`build/`, `bin/` y `lib/` son salidas generadas y están excluidas por
`.gitignore`.

## Dependencias entre proyectos

- `Services` incluye los encabezados públicos del core para compartir
  `TL_RUNTIME_DIR` y los tipos de trazabilidad.
- En la Etapa 2 no se enlaza `libtracelite.a` porque todavía no existe.
- El Makefile de Services enlaza automáticamente la biblioteca si ya está
  construida.
- El Makefile raíz construye primero el core y después Services.
- El Collector solo depende de los encabezados públicos del core.

## Fuera de alcance en la Etapa 2

- `libtracelite.a`.
- Generación real de Trace ID y Span ID.
- Instrumentación de Order Service e Inventory Service.
- Propagación de contexto entre servicios.
- Medición real de spans de negocio.
- Retrasos artificiales.
- Persistencia, red TCP, múltiples hosts y concurrencia.
