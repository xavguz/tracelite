# TraceLite

TraceLite es un sistema ligero de trazabilidad distribuida para GNU/Linux,
escrito en C11 y sin dependencias externas. Un pedido recorre dos servicios de
negocio, genera spans correlacionados por el mismo `trace_id` y los envía a un
Collector local, que reconstruye la jerarquía y la muestra en terminal.

## Inicio rápido

Requisitos: GNU/Linux, compilador C11 (`cc`/`gcc`/`clang`), GNU Make, `ar` y un
shell POSIX.

```sh
make          # compila biblioteca, Collector y servicios
make test     # ejecuta las tres suites de prueba
make run      # ejecuta el escenario completo de tres pedidos
```

Todos los comandos se ejecutan desde la raíz del repositorio. `make` usa
siempre `-std=c11 -Wall -Wextra -Wpedantic` y no oculta advertencias.

## Qué deberías ver

`make run` termina con algo similar a esto (identificadores y tiempos varían):

```text
[collector] spans aceptados=6 rechazados=0
=== TraceLite Collector: 3 traza(s), 6 span(s) ===

Traza 1/3: trace_id=0xda50e243e1ef74b8 spans=2
[raiz] depth=0 trace_id=0xda50e243e1ef74b8 span_id=0xd199fcea0257b363 parent_span_id=none
  service=order-service operation=process-order status=SUCCESS
  start_ns=2478411536331 end_ns=2478412104406 duration_ms=0.568
  [hijo] depth=1 trace_id=0xda50e243e1ef74b8 span_id=0x25cd7c9af0af6bd4 parent_span_id=0xd199fcea0257b363
    service=inventory-service operation=reserve-stock status=SUCCESS
    start_ns=2478411779814 end_ns=2478411791944 duration_ms=0.012

Traza 2/3 ... status=SUCCESS duration_ms=152.962 (hijo: 152.778)  # demora ORD-002
Traza 3/3 ... status=ERROR   duration_ms=0.275  (hijo: 0.011)
  error="simulated inventory failure for ORD-003"

[process-manager] ejecucion completada correctamente
```

Interpretación:

- **3 trazas / 6 spans**: un pedido = una traza con un span raíz y un hijo.
- **`trace_id`**: se repite entre raíz e hijo; identifica la solicitud.
- **`parent_span_id` del hijo**: coincide con el `span_id` de su raíz.
- **`duration_ms`**: tiempo real medido con `CLOCK_MONOTONIC`.
- **ORD-003** es un error de negocio intencional; el proceso sale con código 0.

## Guía de prueba

### 1. Prueba automatizada completa

```sh
make clean
make
make check
make test
```

Resultado esperado:

```text
TraceLite-Core: encabezados OK
Services: encabezados OK
test-collector: OK
test-library: OK
test-tracing: jerarquia OK (3 raices, 3 hijos)
test-tracing: demora ORD-002 OK (150 ms; duracion max ~150 ms)
test-tracing: OK
```

Criterio de éxito: código de salida `0` y ninguna advertencia de compilación.

### 2. Prueba end-to-end visible

```sh
make run
```

Comprueba manualmente:

- [ ] Se muestran **3 trazas** y **6 spans**.
- [ ] Cada traza tiene un `[raiz]` de `order-service` y un `[hijo]` de `inventory-service`.
- [ ] El hijo conserva el `trace_id` y su `parent_span_id` es el `span_id` de la raíz.
- [ ] ORD-002 tarda claramente más (~150 ms).
- [ ] ORD-003 aparece como `ERROR` con el mensaje de fallo simulado.
- [ ] La última línea es `ejecucion completada correctamente`.

### 3. Pruebas individuales

```sh
make check          # encabezados públicos
make test-collector # Collector con spans sintéticos y datagramas malformados
make test-library   # biblioteca de instrumentación
make test-tracing   # escenario end-to-end de los tres pedidos
```

### 4. Prueba aislada del Collector (IPC sin los servicios)

Terminal A:

```sh
./TraceLite-Core/bin/collector /tmp/tracelite-manual/collector.sock
```

Terminal B:

```sh
./TraceLite-Core/bin/collector_test_sender /tmp/tracelite-manual/collector.sock
```

El emisor envía spans sintéticos y un `SHUTDOWN` al final, por lo que el
Collector termina solo. Añade `--valid-only` para omitir los datagramas
malformados de prueba.

### 5. Verificar que no quedan residuos

```sh
pgrep -af 'process_manager|order_service|inventory_service' || echo "sin procesos"
find /tmp -maxdepth 1 -type d -name 'tracelite-*'
```

Esperado: ningún proceso y ningún directorio temporal.

### Problemas comunes

| Síntoma | Solución |
|---|---|
| No encuentra `Services/bin/process_manager` | Ejecuta `make` desde la raíz. |
| Falla al crear o enlazar un socket | Revisa `/tmp/tracelite-*` y ejecuta `make clean`. |
| `make test-tracing` falla | Ejecuta `make` y repite; revisa la salida que imprime el script. |
| ORD-003 devuelve `SIMULATED_ERROR` | Es intencional; no es un fallo del programa. |

## Arquitectura

```text
                    process_manager
                    /      |       \
              fork/exec  fork/exec  fork/exec
                   |         |          |
               collector  inventory   order
             (SOCK_DGRAM) (SOCK_STREAM)
                   ^          ^          |
                   |          +- petición/respuesta -+
                   |                                 |
                   +------ spans de ambos servicios -+
```

| Proceso | Responsabilidad |
|---|---|
| `process_manager` | Crea el runtime, lanza los hijos con `fork()` + `execl()`, espera y limpia. |
| `order_service` | Cliente de negocio; crea el span raíz y propaga el contexto. |
| `inventory_service` | Servidor de negocio; crea el span hijo y simula demora/error. |
| `collector` | Recibe, valida, agrupa e imprime los spans. |

La comunicación usa sockets Unix con nombre: `SOCK_STREAM` para el negocio y
`SOCK_DGRAM` para telemetría (un datagrama = un span).

## Escenarios

| Pedido | Producto | Cantidad | Resultado |
|---|---|---:|---|
| `ORD-001` | `PRODUCT-001` | 5 | `OK`, stock 20 → 15 |
| `ORD-002` | `PRODUCT-002` | 3 | `OK`, stock 10 → 7, demora de 150 ms |
| `ORD-003` | `PRODUCT-003` | 1 | `SIMULATED_ERROR`, stock sin cambios |

## Documentación

- [Arquitectura y justificación del IPC](docs/architecture.md)
- [Protocolos y formato de mensajes](docs/protocol.md)
- [Compilación y resultados detallados](docs/build.md)
- [Informe de avance](docs/informe-avance.md)
- [Checklist de funcionalidades](docs/checklist.md)
- [Evidencias reales de ejecución](docs/evidencias/)

## Limitaciones

- Un único host; no hay comunicación por red.
- Procesamiento secuencial; la biblioteca no es thread-safe.
- Sin persistencia: los spans viven en memoria hasta cerrar el Collector.
- Sin autenticación ni cifrado en los sockets locales.
- Escenario de negocio fijo de tres pedidos.
