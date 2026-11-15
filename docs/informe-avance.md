# TraceLite — Informe de avance

**Proyecto:** TraceLite — sistema ligero de trazabilidad distribuida  
**Tipo de entrega:** Avance parcial — primera versión funcional  
**Plataforma:** GNU/Linux  
**Lenguaje:** C11  
**Fecha:** noviembre de 2026

---

## 1. Descripción

TraceLite es una implementación propia de trazabilidad distribuida inspirada
en OpenTelemetry y Grafana Tempo, sin utilizar sus SDK ni plataformas. Una
solicitud recorre dos servicios de negocio, genera spans correlacionados por un
mismo `trace_id` y los transmite a un proceso Collector, que los agrupa y los
presenta en terminal.

El sistema se compone de:

- un proceso principal (`process_manager`);
- dos servicios de negocio (`order_service` e `inventory_service`);
- un proceso recolector (`collector`);
- una biblioteca estática de instrumentación (`libtracelite.a`).

Todos los procesos se ejecutan en el mismo host y se comunican mediante
sockets Unix.

---

## 2. Objetivo y alcance

El objetivo del avance es demostrar que una solicitud puede:

1. Recorrer al menos dos servicios o procesos.
2. Generar spans relacionados mediante un mismo Trace ID.
3. Propagar el contexto de trazabilidad entre servicios.
4. Transmitir los spans a un proceso recolector mediante IPC.
5. Visualizar las trazas agrupadas y con jerarquía padre-hijo.
6. Registrar al menos una anomalía de tiempo o un error de negocio.

Quedan fuera del alcance de esta primera versión: persistencia, dashboard de
actualización periódica, comunicación entre hosts, autenticación y
procesamiento concurrente.

---

## 3. Arquitectura de software

```text
                    process_manager
                    /      |       \
          fork/exec       fork/exec       fork/exec
               |              |                |
         collector       inventory_service   order_service
        (SOCK_DGRAM)     (SOCK_STREAM)       (SOCK_STREAM)
               ^              ^                    |
               |              |                    |
               |              +---- petición -----+
               |              <---- respuesta -----
               |
               +---- spans de Order e Inventory
```

| Componente | Responsabilidad |
|---|---|
| `process_manager` | Crea el runtime temporal, lanza los procesos, sincroniza el arranque, espera con `waitpid()`, cierra el Collector y limpia recursos. |
| `order_service` | Cliente del negocio. Crea el span raíz `process-order`, propaga el contexto y envía tres pedidos. |
| `inventory_service` | Servidor del negocio. Crea el span hijo `reserve-stock`, conserva el `trace_id`, simula la demora de ORD-002 y el error de ORD-003. |
| `collector` | Recibe, valida, agrupa e imprime los spans. |
| `libtracelite.a` | IDs, contextos, ciclo de vida de spans, medición temporal y envío de telemetría. |

---

## 4. Modelo de procesos

```text
process_manager (padre común)
  ├── collector
  ├── inventory_service
  └── order_service
```

Flujo de arranque:

1. El Process Manager instala manejadores de `SIGINT` y `SIGTERM`.
2. Verifica que los tres ejecutables existan.
3. Crea `/tmp/tracelite-XXXXXX` con `mkdtemp()`.
4. Lanza el Collector y espera su socket.
5. Exporta `TRACELITE_COLLECTOR_SOCKET`.
6. Lanza Inventory Service y espera la notificación de readiness por tubería.
7. Lanza Order Service.
8. Espera a Order y a Inventory.
9. Envía `SHUTDOWN` al Collector y espera su cierre.
10. Elimina sockets y directorio temporal.

Los tres hijos se crean con `fork()` y se inician con `execl()`. La comunicación
de negocio no depende del Process Manager ni del Collector.

---

## 5. Mecanismo IPC y justificación

Se eligieron **sockets Unix con nombre**:

- `AF_UNIX` + `SOCK_STREAM` para la comunicación de negocio.
- `AF_UNIX` + `SOCK_DGRAM` para la telemetría de spans.

### Alternativas evaluadas

| Mecanismo | Limitación para este proyecto |
|---|---|
| Pipes / FIFO | Unidireccionales y sin direccionamiento; dificultan el modelo varios-a-uno y el reinicio independiente del receptor. |
| Memoria compartida | Exige sincronización manual y un protocolo adicional de delimitación. |
| Sockets Unix | Direccionamiento por ruta, permisos del sistema de archivos y soporte nativo de flujo y datagramas. |

### Motivos de la decisión

1. Los procesos son independientes y tienen ciclos de vida distintos.
2. `SOCK_STREAM` ofrece orden y entrega completa para petición/respuesta.
3. `SOCK_DGRAM` mantiene la relación “un datagrama = un span”, sin framing
   adicional sobre un flujo.
4. Cada ejecución usa rutas únicas dentro de un directorio temporal, sin
   puertos TCP ni conflictos entre instancias.
5. La limpieza se reduce a eliminar los archivos de socket.

No se usa TCP porque todos los procesos están en el mismo host.

---

## 6. Formato del span

`tl_span_t` ocupa **272 bytes** en la compilación actual:

| Campo | Tipo | Descripción |
|---|---|---|
| `trace_id` | `uint64_t` | Traza completa; 0 es inválido |
| `span_id` | `uint64_t` | Identificador del span; 0 es inválido |
| `parent_span_id` | `uint64_t` | 0 para el span raíz |
| `start_monotonic_ns` | `int64_t` | Inicio con `CLOCK_MONOTONIC` |
| `duration_ns` | `int64_t` | Duración en nanosegundos |
| `status` | `int32_t` | `SUCCESS`, `ERROR`, `TIMEOUT` o `CANCELLED` |
| `flags` | `uint32_t` | Reservado; debe valer 0 |
| `service_name` | `char[32]` | Servicio que genera el span |
| `operation_name` | `char[64]` | Operación medida |
| `error_message` | `char[128]` | Mensaje de error, si existe |

El fin se calcula como `start_monotonic_ns + duration_ns` y la duración se
presenta en milisegundos.

---

## 7. Formatos de mensajes

### 7.1. Petición de negocio

Cabecera de 16 bytes + payload de 88 bytes (petición actual):

| Campo | Tipo |
|---|---|
| `magic`, `version`, `kind`, `payload_len`, `request_id` | cabecera |
| `order_id` | `char[32]` |
| `product_id` | `char[32]` |
| `quantity` | `uint32_t` |
| `trace_context { trace_id, parent_span_id }` | 16 bytes |

El receptor también acepta el payload legado de 68 bytes sin contexto.

### 7.2. Respuesta de negocio

Payload de 168 bytes:

| Campo | Tipo |
|---|---|
| `order_id` | `char[32]` |
| `status` | `int32_t` |
| `available_quantity` | `uint32_t` |
| `message` | `char[128]` |

### 7.3. Mensaje de telemetría

Mensaje de span de 288 bytes:

| Campo | Tipo |
|---|---|
| `magic` | `uint32_t` |
| `version` | `uint16_t` |
| `type` | `uint16_t` |
| `span_size` | `uint32_t` |
| `reserved` | `uint32_t` |
| `span` | `tl_span_t` (272 bytes) |

Mensaje de control `SHUTDOWN` de 8 bytes: `magic`, `version`, `type`.

El Collector valida tamaño, cabecera, identificadores, estado, tiempos,
desbordamientos y terminadores de texto. Un datagrama inválido se registra y se
descarta sin detener el proceso.

---

## 8. Flujo de una solicitud

```text
Order Service                                      Inventory Service
     |                                                    |
     | tl_span_start(process-order)                       |
     |   trace_id=T, span_id=O                            |
     |-- REQUEST(ctx{T,O}) ------------------------------>|
     |                                                    | tl_span_start(reserve-stock, parent=O)
     |                                                    |   trace_id=T, span_id=I
     |                                                    | (ORD-002: espera 150 ms)
     |                                                    | tl_span_finish(I)
     |<----------------------------- RESPONSE(status) ----|
     | tl_span_finish(O)                                  |
```

Cada pedido genera una traza independiente:

```text
trace T
├── order-service     process-order   span O   parent = 0
└── inventory-service reserve-stock   span I   parent = O
```

---

## 9. Escenarios de prueba

| Pedido | Producto | Cantidad | Resultado | Spans |
|---|---|---:|---|---|
| ORD-001 | PRODUCT-001 | 5 | `OK`, stock 20 → 15 | `SUCCESS` / `SUCCESS` |
| ORD-002 | PRODUCT-002 | 3 | `OK`, stock 10 → 7, demora de 150 ms | `SUCCESS` / `SUCCESS` |
| ORD-003 | PRODUCT-003 | 1 | `SIMULATED_ERROR`, stock sin cambios | `ERROR` / `ERROR` |

La anomalía de ORD-002 queda registrada en `duration_ns`. El error de ORD-003
queda registrado en `status` y `error_message` de ambos spans.

---

## 10. Resultados reales

### 10.1. Compilación

```text
make clean && make
```

Compilación correcta con `-std=c11 -Wall -Wextra -Wpedantic`, sin advertencias.

### 10.2. Pruebas

```text
TraceLite-Core: encabezados OK
Services: encabezados OK
test-collector: OK
test-library: OK
test-tracing: jerarquia OK (3 raices, 3 hijos)
test-tracing: demora ORD-002 OK (150 ms; duracion max 150.908 ms)
test-tracing: OK
```

### 10.3. Ejecución completa

```text
[collector] spans aceptados=6 rechazados=0
=== TraceLite Collector: 3 traza(s), 6 span(s) ===

Traza 1/3 ... status=SUCCESS  duration_ms=0.568  (hijo: 0.012)
Traza 2/3 ... status=SUCCESS  duration_ms=152.962 (hijo: 152.778)
Traza 3/3 ... status=ERROR    duration_ms=0.275   error="simulated inventory failure for ORD-003"

[process-manager] order_service termino con codigo 0
[process-manager] inventory_service termino con codigo 0
[process-manager] collector termino con codigo 0
[process-manager] ejecucion completada correctamente
```

La prueba end-to-end se repitió cinco veces consecutivas durante la validación;
las duraciones varían algunas milésimas entre ejecuciones. En la validación
final, la prueba automatizada midió un máximo de 150.908 ms y la ejecución de
referencia registró 152.962 ms en el span raíz y 152.778 ms en el hijo. No
quedaron procesos, sockets ni directorios temporales residuales.

Las transcripciones completas están en [`docs/evidencias/`](evidencias/).

---

## 11. Pruebas automatizadas

| Comando | Cobertura |
|---|---|
| `make check` | Encabezados públicos |
| `make test-collector` | 4 spans sintéticos, 8 datagramas malformados, agrupación y `SHUTDOWN` |
| `make test-library` | Ciclo de vida, contextos, truncado, 10 000 IDs, duración real, doble finalización y Collector ausente |
| `make test-tracing` | 3 trazas, 6 spans, jerarquía, estados, demora de ORD-002 y limpieza |
| `make test` | Ejecuta las tres suites |

---

## 12. Limitaciones

- Un único host; no hay comunicación TCP entre máquinas.
- Procesamiento secuencial; la biblioteca no es thread-safe.
- Sin persistencia: los spans viven en memoria hasta el cierre del Collector.
- Sin autenticación ni cifrado en los sockets locales.
- Escenario de negocio fijo de tres pedidos.
- Sin reintentos de telemetría si el Collector no está disponible.

---

## 13. Checklist

El estado detallado de funcionalidades implementadas y pendientes para la
entrega final está en [`docs/checklist.md`](checklist.md).

---

## 14. Conclusiones

El avance cumple los requerimientos funcionales del enunciado:

- dos servicios de negocio más un Collector, creados con `fork()` y `exec()`;
- spans con identificadores, tiempos y estados;
- propagación correcta del contexto entre servicios;
- telemetría real por IPC;
- visualización en terminal agrupada por traza y con jerarquía;
- tres solicitudes completas, con demora y error registrados.

La implementación es propia, no usa SDK de trazabilidad y mide tiempos reales
con `CLOCK_MONOTONIC`.
