# TraceLite - Compilación y ejecución

## Requisitos

- GNU/Linux.
- Compilador C11 (`cc`, `gcc` o `clang`).
- GNU Make.
- `ar` para la biblioteca estática.
- Un shell POSIX para los scripts de prueba.

No se usan dependencias externas.

## Estado actual

Están implementados y se construyen desde cero:

- `TraceLite-Core/lib/libtracelite.a`
- `TraceLite-Core/bin/collector`
- `TraceLite-Core/bin/collector_test_sender` (solo pruebas)
- `Services/bin/process_manager`
- `Services/bin/order_service`
- `Services/bin/inventory_service`

Los servicios generan y envían spans reales, propagan el contexto de
trazabilidad y el Collector presenta las tres trazas al finalizar.

## Compilar todo

Desde la raíz del repositorio:

```sh
make clean
make
make check
```

`make` construye primero `TraceLite-Core` y después `Services`. Los Makefiles
añaden siempre `-std=c11 -Wall -Wextra -Wpedantic`, incluso si el usuario
proporciona `CFLAGS`.

Salidas principales:

```text
TraceLite-Core/lib/libtracelite.a
TraceLite-Core/bin/collector
TraceLite-Core/bin/collector_test_sender
TraceLite-Core/bin/test_library
Services/bin/process_manager
Services/bin/order_service
Services/bin/inventory_service
```

## Objetivos parciales

### TraceLite-Core

```sh
make -C TraceLite-Core              # lib + collector
make -C TraceLite-Core lib          # solo lib/libtracelite.a
make -C TraceLite-Core collector    # solo bin/collector
make -C TraceLite-Core test-sender  # solo bin/collector_test_sender
make -C TraceLite-Core test-library # compila y ejecuta la prueba de la biblioteca
make -C TraceLite-Core test-collector
make -C TraceLite-Core check
```

### Services

```sh
make -C Services        # construye los tres ejecutables
make -C Services check  # valida los encabezados de Services
```

El Makefile de Services enlaza `../TraceLite-Core/lib/libtracelite.a` y la
construye automáticamente si falta.

## Ejecutar el escenario completo

```sh
make run
```

El Process Manager:

1. Crea un directorio temporal `/tmp/tracelite-XXXXXX`.
2. Arranca el Collector y espera su socket.
3. Arranca Inventory Service y espera la notificación de readiness.
4. Arranca Order Service.
5. Espera a Order e Inventory con `waitpid()`.
6. Envía `SHUTDOWN` al Collector y espera su cierre.
7. Elimina los sockets y el directorio temporal.

La ruta del socket del Collector se puede ajustar con
`TRACELITE_COLLECTOR_SOCKET`. El Process Manager siempre usa una ruta única
dentro de su directorio temporal y la exporta a los servicios.

Ante `SIGINT` o `SIGTERM`, el Process Manager termina los hijos vivos, los
recoge con `waitpid()` y ejecuta la misma limpieza. No deja procesos huérfanos,
zombis, sockets ni directorios temporales.

## Pruebas

| Comando | Cobertura |
|---|---|
| `make check` | Compilación sintáctica de los encabezados públicos. |
| `make test-collector` | Collector: 4 spans sintéticos, 8 datagramas malformados, agrupación, jerarquía, `SHUTDOWN` y eliminación del socket. |
| `make test-library` | Biblioteca: errores de uso, ciclo de vida, contextos, truncado, unicidad de 10 000 IDs, duración real, serialización, doble finalización y Collector no disponible. |
| `make test-tracing` | End-to-end: 3 trazas, 6 spans, 3 raíces, 3 hijos, 4 `SUCCESS`, 2 `ERROR`, demora de ORD-002, jerarquía y limpieza. |
| `make test` | Ejecuta las tres suites anteriores. |

### Prueba del Collector

```sh
make test-collector
```

1. Compila `collector` y `collector_test_sender`.
2. Crea un directorio temporal único con `mktemp -d`.
3. Arranca el Collector con una ruta de socket propia.
4. Espera a que exista el archivo de socket.
5. Ejecuta el emisor, que envía 8 casos malformados, 4 spans válidos y
   `SHUTDOWN`.
6. Espera el cierre del Collector.
7. Comprueba que el socket fue eliminado y que no quedan procesos.

No se usa una ruta fija compartida entre ejecuciones.

Resultado observado:

```text
[collector] spans aceptados=4 rechazados=8
=== TraceLite Collector: 2 traza(s), 4 span(s) ===
...
test-collector: OK
```

Los spans de este emisor son sintéticos: los tiempos y estados no proceden de
mediciones reales de Order Service ni Inventory Service.

### Prueba de la biblioteca

```sh
make test-library
```

Resultado observado:

```text
test-library: OK
```

Cubre, entre otros:

- `tl_init()`/`tl_shutdown()` y errores de uso.
- Spans raíz, hijo y hermano; `parent_span_id` correcto.
- Contexto propagado y validación de contextos.
- Truncado seguro de nombres y mensajes.
- Unicidad de 10 000 identificadores generados.
- Medición real de una espera de 10 ms.
- Serialización exacta del mensaje de span.
- Rechazo de una segunda finalización con `EALREADY`.
- Comportamiento cuando el Collector no está disponible.

### Prueba end-to-end

```sh
make test-tracing
```

Resultado observado:

```text
test-tracing: ejecutando process_manager
test-tracing: jerarquia OK (3 raices, 3 hijos)
test-tracing: demora ORD-002 OK (150 ms; duracion max 150.908 ms)
test-tracing: OK
```

La prueba se repitió cinco veces consecutivas durante la validación. La
duración varía algunas milésimas entre ejecuciones; en la validación final el
máximo observado fue 150.908 ms, sin procesos, sockets ni directorios
temporales residuales.

### Ejecución manual del Collector

En una terminal:

```sh
./TraceLite-Core/bin/collector /tmp/tracelite-manual/collector.sock
```

En otra:

```sh
./TraceLite-Core/bin/collector_test_sender /tmp/tracelite-manual/collector.sock
```

El emisor envía `SHUTDOWN` al final, por lo que el Collector termina solo. Para
enviar únicamente spans válidos:

```sh
./TraceLite-Core/bin/collector_test_sender \
    /tmp/tracelite-manual/collector.sock --valid-only
```

## Resultado de `make run`

Salida real de una ejecución (identificadores y tiempos varían; se omiten
algunas líneas de progreso):

```text
[process-manager] runtime: /tmp/tracelite-60M4O6
[collector] escuchando en /tmp/tracelite-60M4O6/collector.sock (pid=6298)
[process-manager] collector listo (pid=6298)
[inventory] escuchando en /tmp/tracelite-60M4O6/inventory.sock
[process-manager] inventory_service listo (pid=6299)
[process-manager] order_service iniciado (pid=6300)
[order] conectado a /tmp/tracelite-60M4O6/inventory.sock
[inventory] conexion aceptada; procesando 3 pedidos
[inventory] req=1 ORD-001 PRODUCT-001 qty=5 -> OK (available=15): reserved 5 unit(s) of PRODUCT-001
[order] req=1 ORD-001 PRODUCT-001 qty=5 -> OK (available=15): reserved 5 unit(s) of PRODUCT-001
[inventory] demora simulada de 150 ms para ORD-002
[inventory] req=2 ORD-002 PRODUCT-002 qty=3 -> OK (available=7): reserved 3 unit(s) of PRODUCT-002
[order] req=2 ORD-002 PRODUCT-002 qty=3 -> OK (available=7): reserved 3 unit(s) of PRODUCT-002
[inventory] req=3 ORD-003 PRODUCT-003 qty=1 -> SIMULATED_ERROR (available=5): simulated inventory failure for ORD-003
[inventory] tres pedidos procesados correctamente
[order] req=3 ORD-003 PRODUCT-003 qty=1 -> SIMULATED_ERROR (available=5): simulated inventory failure for ORD-003
[order] tres pedidos procesados; error controlado de ORD-003 verificado
[process-manager] order_service termino con codigo 0
[process-manager] inventory_service termino con codigo 0
[collector] SHUTDOWN recibido; cerrando
[collector] spans aceptados=6 rechazados=0
=== TraceLite Collector: 3 traza(s), 6 span(s) ===

Traza 1/3: trace_id=0xda50e243e1ef74b8 spans=2
[raiz] depth=0 trace_id=0xda50e243e1ef74b8 span_id=0xd199fcea0257b363 parent_span_id=none
  service=order-service operation=process-order status=SUCCESS
  start_ns=2478411536331 end_ns=2478412104406 duration_ms=0.568
  [hijo] depth=1 trace_id=0xda50e243e1ef74b8 span_id=0x25cd7c9af0af6bd4 parent_span_id=0xd199fcea0257b363
    service=inventory-service operation=reserve-stock status=SUCCESS
    start_ns=2478411779814 end_ns=2478411791944 duration_ms=0.012

Traza 2/3: trace_id=0x3a6274ef7584e374 spans=2
[raiz] depth=0 trace_id=0x3a6274ef7584e374 span_id=0xeebb48426b36ca63 parent_span_id=none
  service=order-service operation=process-order status=SUCCESS
  start_ns=2478412121438 end_ns=2478565083457 duration_ms=152.962
  [hijo] depth=1 trace_id=0x3a6274ef7584e374 span_id=0x77e331eed78c66c1 parent_span_id=0xeebb48426b36ca63
    service=inventory-service operation=reserve-stock status=SUCCESS
    start_ns=2478412126908 end_ns=2478564905138 duration_ms=152.778

Traza 3/3: trace_id=0x8fd2d7f0ab27dced spans=2
[raiz] depth=0 trace_id=0x8fd2d7f0ab27dced span_id=0xf4796cace0460314 parent_span_id=none
  service=order-service operation=process-order status=ERROR
  start_ns=2478565366617 end_ns=2478565642379 duration_ms=0.275
  error="simulated inventory failure for ORD-003"
  [hijo] depth=1 trace_id=0x8fd2d7f0ab27dced span_id=0xa939084761b6443d parent_span_id=0xf4796cace0460314
    service=inventory-service operation=reserve-stock status=ERROR
    start_ns=2478565537670 end_ns=2478565548721 duration_ms=0.011
    error="simulated inventory failure for ORD-003"
[process-manager] collector termino con codigo 0
[process-manager] ejecucion completada correctamente
```

El error de ORD-003 es controlado y esperado: los tres procesos terminan con
código 0 porque el protocolo y el caso de error se procesaron correctamente.

## Limpieza

```sh
make clean      # elimina build/, bin/, lib/*.a y ejecutables generados
make distclean  # elimina también los directorios de salida
```

## Variables ajustables

```sh
make CC=clang CFLAGS="-O0 -g3"
```

Los Makefiles agregan obligatoriamente `-std=c11`, `-Wall`, `-Wextra` y
`-Wpedantic`. No se ocultan advertencias.

## Diagnóstico rápido

- `make test-tracing` falla con "falta process_manager": ejecutar `make` antes.
- El Process Manager busca el Collector en
  `../../TraceLite-Core/bin/collector` relativo a `Services/bin`.
- Si el Collector no está disponible, los servicios registran el error de
  telemetría pero no fallan por ello.
- Si una ejecución se interrumpe, comprobar que no queden directorios
  `/tmp/tracelite-*`; el Process Manager los elimina al recibir la señal.
