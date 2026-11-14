# TraceLite - Compilación y ejecución

## Requisitos

- GNU/Linux
- Compilador C11 (`cc`, `gcc` o `clang`)
- GNU Make
- `ar` para la biblioteca estática

No se usan dependencias externas.

## Estado actual: Etapa 2

Están implementados:

- `Services/bin/process_manager`
- `Services/bin/order_service`
- `Services/bin/inventory_service`
- `TraceLite-Core/bin/collector`
- `TraceLite-Core/bin/collector_test_sender` (solo pruebas)

Todavía no existe:

- `TraceLite-Core/lib/libtracelite.a`

Por ello, el objetivo `lib` de TraceLite-Core muestra un aviso de "pendiente".
Los servicios de negocio todavía no generan ni envían spans.

## Compilar todo

Desde la raíz:

```sh
make check   # valida encabezados
make         # compila el Collector y los servicios de negocio
```

## Compilar TraceLite-Core

```sh
make -C TraceLite-Core
```

Genera:

```text
TraceLite-Core/bin/collector
```

Objetivos parciales:

```sh
make -C TraceLite-Core collector
make -C TraceLite-Core test-sender
make -C TraceLite-Core check
```

## Probar el Collector

Desde la raíz:

```sh
make test-collector
```

Equivalente:

```sh
make -C TraceLite-Core test-collector
```

El objetivo:

1. Compila `collector` y `collector_test_sender`.
2. Crea un directorio temporal único con `mktemp -d`.
3. Arranca el Collector con una ruta de socket propia.
4. Espera a que el archivo de socket exista.
5. Ejecuta el emisor, que envía casos malformados, cuatro spans válidos y
   `SHUTDOWN`.
6. Espera el cierre del Collector.
7. Comprueba que el socket fue eliminado y que no quedan procesos.

No se usa una ruta fija compartida entre ejecuciones.

### Ejecución manual

En una terminal:

```sh
./TraceLite-Core/bin/collector /tmp/tracelite-manual/collector.sock
```

En otra:

```sh
./TraceLite-Core/bin/collector_test_sender /tmp/tracelite-manual/collector.sock
```

El emisor envía `SHUTDOWN` al final, por lo que el Collector termina solo.
Para enviar únicamente spans válidos:

```sh
./TraceLite-Core/bin/collector_test_sender \
    /tmp/tracelite-manual/collector.sock --valid-only
```

## Compilar y ejecutar Services

```sh
make -C Services
make run
```

El Process Manager crea un directorio temporal
`/tmp/tracelite-XXXXXX`, lanza Inventory y Order, espera a ambos y elimina el
socket y el directorio al terminar. La lógica de negocio de la Etapa 1 no ha
cambiado.

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
`-Wpedantic`, incluso si el usuario proporciona `CFLAGS`.

## Resultado esperado de `make test-collector`

```text
test-collector: collector listo en /tmp/tracelite-collector-XXXXXX/collector.sock
[sender] datos sinteticos de prueba; no son mediciones reales
...
[sender] enviado span A hijo (SUCCESS) (288 bytes)
[sender] enviado span A raiz (SUCCESS) (288 bytes)
...
[sender] enviado SHUTDOWN (8 bytes)
[collector] escuchando en /tmp/tracelite-collector-XXXXXX/collector.sock (pid=...)
[collector] datagrama rechazado: magic de span invalido
[collector] datagrama rechazado: span_size incorrecto
...
[collector] span aceptado trace_id=0x1111111111111111 span_id=0x1000000000000002 ...
[collector] SHUTDOWN recibido; cerrando
[collector] spans aceptados=4 rechazados=8
=== TraceLite Collector: 2 traza(s), 4 span(s) ===

Traza 1/2: trace_id=0x1111111111111111 spans=2
[raiz] depth=0 trace_id=0x1111111111111111 span_id=0x1000000000000001 parent_span_id=none
  service=order-service operation=process-order status=SUCCESS
  start_ns=1000000000 end_ns=1012500000 duration_ms=12.500
  [hijo] depth=1 trace_id=0x1111111111111111 span_id=0x1000000000000002 parent_span_id=0x1000000000000001
    service=inventory-service operation=reserve-stock status=SUCCESS
    start_ns=1002000000 end_ns=1010000000 duration_ms=8.000

Traza 2/2: trace_id=0x2222222222222222 spans=2
[raiz] depth=0 trace_id=0x2222222222222222 span_id=0x2000000000000001 parent_span_id=none
  service=order-service operation=process-order status=ERROR
  start_ns=2000000000 end_ns=2009250000 duration_ms=9.250
  error="simulated order failure"
  [hijo] depth=1 trace_id=0x2222222222222222 span_id=0x2000000000000002 parent_span_id=0x2000000000000001
    service=inventory-service operation=reserve-stock status=ERROR
    start_ns=2003000000 end_ns=2007500000 duration_ms=4.500
    error="simulated inventory failure"
test-collector: OK
```

Los spans de este emisor son sintéticos: los tiempos y estados no proceden de
mediciones reales de Order Service ni Inventory Service.

## Resultado esperado de `make run`

```text
[inventory] req=1 ORD-001 PRODUCT-001 qty=5 -> OK (available=15)
[order]     req=1 ORD-001 PRODUCT-001 qty=5 -> OK (available=15)
[inventory] req=2 ORD-002 PRODUCT-002 qty=3 -> OK (available=7)
[order]     req=2 ORD-002 PRODUCT-002 qty=3 -> OK (available=7)
[inventory] req=3 ORD-003 PRODUCT-003 qty=1 -> SIMULATED_ERROR (available=5)
[order]     req=3 ORD-003 PRODUCT-003 qty=1 -> SIMULATED_ERROR (available=5)
[process-manager] order_service termino con codigo 0
[process-manager] inventory_service termino con codigo 0
[process-manager] ejecucion completada correctamente
```

El error de ORD-003 es controlado y esperado: ambos servicios terminan con
código 0 porque el protocolo y el caso de error se procesaron correctamente.
