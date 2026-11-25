# TraceLite

TraceLite es un proyecto universitario de Sistemas Operativos en C11 para GNU/Linux. Combina una aplicación de inventario con un sistema ligero de trazabilidad inspirado en OpenTelemetry.

Las aplicaciones se ejecutan de forma independiente:

~~~
./tracelite     # Collector, alertas e historial
./service        # Process Manager, Order Service e Inventory Service
~~~

## Qué hace

- Lista productos, consulta por ID y procesa pedidos sobre `data/inventory.json`.
- Genera un span raíz en `order-service` y uno hijo en `inventory-service` por solicitud.
- Propaga Trace ID, Span ID y Parent Span ID entre procesos.
- Mide duración real con `CLOCK_MONOTONIC`.
- Simula demora aleatoria de 10 a 500 ms en pedidos.
- Detecta errores y duraciones mayores a 400 ms.
- Guarda spans en `traces.jsonl` y permite consultarlos después de reiniciar.

## Requisitos e instalación

Se requiere GNU/Linux, un compilador C11 (GCC, Clang o `cc`), GNU Make, `ar` y un shell POSIX. cJSON se incluye en `third_party/cjson`; su licencia está en `third_party/cjson/LICENSE`.

Desde la raíz:

~~~
make
make check
~~~

Se crean `./tracelite`, `./service`, `libtracelite.a` y los ejecutables de los servicios.

## Tutorial: ejecutar en dos terminales

### Terminal 1: TraceLite

~~~
./tracelite
~~~

Selecciona `1. Escuchar servicios`. TraceLite crea el socket del Collector en `/tmp/tracelite/collector.sock` y espera spans.

~~~
============= TRACELITE =============

1. Escuchar servicios
2. Historial de trazas
0. Salir
~~~

Presiona `Ctrl+C` para detener sólo la escucha: se muestran las trazas de la sesión y el resumen, y se vuelve al menú.

### Terminal 2: inventario

~~~
./service
~~~

El Process Manager crea Inventory y Order con `fork()` y `exec()`. El menú de negocio es:

~~~
========== INVENTORY SYSTEM ==========

1. Listar productos
2. Consultar producto
3. Realizar pedido
0. Salir
~~~

Prueba esta secuencia:

1. Elige `1` para listar productos.
2. Elige `2` e ingresa `PRODUCT-001`.
3. Elige `3`, `PRODUCT-001` y cantidad `2`. El stock se descuenta y se persiste.
4. Elige `3`, `PRODUCT-003` y cantidad `999`. Se obtiene stock insuficiente y una alerta de error.
5. Elige `0`. El padre recoge a sus hijos con `waitpid()` y limpia el socket de negocio.

La primera terminal muestra spans correlacionados, alertas y el resumen de sesión.

## Historial

Los spans válidos se agregan a `traces.jsonl`; nunca se sobrescriben ejecuciones anteriores. En TraceLite selecciona `2. Historial de trazas`:

~~~
1. Listar trazas
2. Buscar por Trace ID
3. Consultar trazas con anomalías
0. Regresar
~~~

- **Listar** muestra Trace ID, operación principal, duración, estado y anomalías.
- **Buscar** acepta el ID en decimal o hexadecimal, por ejemplo `0x1234`.
- **Anomalías** muestra sólo trazas ya clasificadas al recibir sus spans.

Los registros JSONL dañados se informan y se omiten sin cerrar la aplicación.

## Demoras y alertas

Los pedidos aplican `nanosleep()` dentro del span de Inventory. Una operación es anómala si su span queda en `ERROR` o tarda más de **400 ms**. Un pedido exitoso también puede generar alerta de latencia.

Para una prueba reproducible, sin añadir opciones al menú:

~~~
TRACELITE_ORDER_DELAY_MS=450 ./service
~~~

Sin esa variable, la demora es aleatoria entre 10 y 500 ms.

## Configuración

| Variable | Uso | Predeterminado |
|---|---|---|
| `TRACELITE_COLLECTOR_SOCKET` | Socket de telemetría. | `/tmp/tracelite/collector.sock` |
| `TRACELITE_INVENTORY_SOCKET` | Socket de negocio. | `/tmp/tracelite/inventory.sock` |
| `TRACELITE_INVENTORY_FILE` | Inventario de entrada. | `data/inventory.json` |
| `TRACELITE_TRACES_FILE` | Historial JSON Lines. | `traces.jsonl` |
| `TRACELITE_ORDER_DELAY_MS` | Demora de prueba (0 a 60000 ms). | no definida |

Las constantes 10 ms, 500 ms y 400 ms se definen en `service_config.h` y `config.h`.

## Pruebas

~~~
make test
~~~

<<<<<<< HEAD
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
=======
| Objetivo | Comprueba |
>>>>>>> 727df6f (project completed)
|---|---|
| `make test-library` | IDs, contextos, duración, ciclo de vida y Collector ausente. |
| `make test-collector` | Datagramas válidos/malformados, jerarquía, persistencia y resumen. |
| `make test-tracing` | Inventario, correlación, alerta de error y latencia. |
| `make test-resilience` | Collector ausente, cuatro solicitudes normales, JSONL inválido e historial. |

Las pruebas usan directorios temporales y copias del inventario: no alteran `data/inventory.json` ni el historial normal.

## Arquitectura

~~~
                    process_manager
                    /               \
             fork/exec             fork/exec
                  |                     |
          inventory_service <--- order_service
                 SOCK_STREAM (negocio)
                  |                     |
                  +---- SOCK_DGRAM -----+
                            spans
                              |
                         ./tracelite
~~~

- `SOCK_STREAM` transporta solicitudes y respuestas de negocio.
- `SOCK_DGRAM` transporta spans; un fallo de telemetría no bloquea el negocio.
- Los datos están separados: `inventory.json` para negocio y `traces.jsonl` para observabilidad.

Para la explicación de procesos, IPC, formato de spans, persistencia y detección de anomalías consulta [la guía técnica](docs/technical-guide.md).

## Modo integrado compatible

Para el modo legado, donde el Process Manager también lanza el Collector:

<<<<<<< HEAD
=======
~~~
make run-integrated
~~~

El modo recomendado es usar `./tracelite` y `./service` en terminales independientes.

>>>>>>> 727df6f (project completed)
## Limitaciones

- Un solo host Linux; sin red, autenticación ni cifrado.
- Procesamiento secuencial y sin concurrencia adicional.
- Historial limitado a las tres consultas requeridas; sin dashboard, paginación ni filtros avanzados.
- El umbral se configura al compilar; los spans nuevos guardan su clasificación para no reinterpretar el historial.
