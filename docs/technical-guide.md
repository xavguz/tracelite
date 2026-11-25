# Guía técnica de TraceLite

Este documento complementa el tutorial del README y sirve como apoyo para la defensa técnica.

## Procesos y ciclo de vida

~process_manager~ es el padre de ~inventory_service~ y ~order_service~. Usa ~fork()~ y ~execl()~ para crear ejecutables independientes. Antes de iniciar Order, espera por un ~pipe()~ a que Inventory haya creado el socket. Al terminar, usa ~waitpid()~ para recoger cada hijo y evitar zombies.

~~~text
TraceLite (terminal A)                Service (terminal B)
collector                             process_manager
  ^                                      |- inventory_service
  | spans                                `- order_service
  +-----------------------------------------------+
~~~

La independencia es importante: Service sigue atendiendo negocio cuando no hay Collector. La biblioteca envía telemetría por datagramas no bloqueantes; un error de ~sendto()~ no cambia la respuesta de inventario.

## IPC

| Comunicación | Tipo | Razón |
|---|---|---|
| Order ↔ Inventory | ~AF_UNIX / SOCK_STREAM~ | Solicitud/respuesta ordenada y framing explícito. |
| Servicios → Collector | ~AF_UNIX / SOCK_DGRAM~ | Un span por datagrama; no bloquea negocio y no necesita framing de stream. |
| Process Manager → Inventory | ~pipe()~ | Señal simple de disponibilidad antes de arrancar Order. |

El protocolo de negocio tiene una cabecera con magic, versión, tipo, tamaño de payload y request ID. Las funciones ~ipc_send_all()~ e ~ipc_recv_all()~ manejan lecturas o escrituras parciales.

## Propagación y spans

Order inicia un span raíz y coloca este contexto en la solicitud:

~~~text
trace_id = T
span_id = O
parent_span_id = 0
~~~

Inventory recibe el contexto con Trace ID T y Parent Span ID O, y crea un span con Trace ID T, Span ID I y Parent Span ID O.

Los campos relevantes de cada span son Trace ID, Span ID, Parent Span ID, servicio, operación, inicio monotónico, duración, estado y mensaje de error. El inicio y fin se leen con ~clock_gettime(CLOCK_MONOTONIC)~. La duración se guarda en nanosegundos y se muestra en milisegundos.

## Inventario seguro

Inventory interpreta ~inventory.json~ con cJSON. Para un pedido válido:

1. Lee y valida el arreglo JSON.
2. Verifica producto, cantidad positiva y stock.
3. Aplica la demora simulada dentro del span.
4. Descuenta en memoria.
5. Escribe un archivo temporal con ~mkstemp()~.
6. Hace ~fflush()~, ~fsync()~ y ~rename()~.

~rename()~ evita que un lector encuentre una modificación parcialmente escrita. Si cualquier paso falla, se devuelve un error y no se confirma el pedido.

## Collector, persistencia e historial

El Collector valida tamaño, magic, versión, estado, textos terminados e IDs antes de aceptar un datagrama. Almacena por Trace ID; el impresor reconstruye padre-hijo usando IDs, no el orden de llegada.

Cada span aceptado se agrega a ~traces.jsonl~ como un objeto JSON independiente. Los IDs y tiempos de 64 bits se serializan como cadenas para no perder precisión. ~anomaly_flags~ conserva la clasificación histórica:

| Valor | Significado |
|---:|---|
| 0 | Normal |
| 1 | Error |
| 2 | Latencia |
| 3 | Error y latencia |

El historial vuelve a leer las líneas con cJSON. Una línea inválida se informa y se descarta, de modo que un registro corrupto no invalida el resto del archivo.

## Anomalías

- **Error:** estado ~ERROR~.
- **Latencia:** duración superior a 400 ms.

El valor de 400 ms se eligió dentro del intervalo de 10 a 500 ms: deja un margen para operaciones normales y permite generar una anomalía reproducible usando ~TRACELITE_ORDER_DELAY_MS=450~.

Para reducir alertas duplicadas, cuando el hijo de Inventory ya llegó y explica la misma condición, se suprime la alerta equivalente del span raíz de Order. Así se prioriza el servicio donde se originó el problema.

## Señales y limpieza

Los manejadores de ~SIGINT~ y ~SIGTERM~ sólo actualizan una variable de tipo ~sig_atomic_t~. El trabajo de limpieza ocurre en el flujo normal: cerrar descriptores, eliminar sockets, imprimir resumen y liberar memoria. No se realizan operaciones de E/S inseguras dentro del manejador.

## Pruebas reales

~~~sh
make check
make test
~~~

La suite cubre biblioteca, datagramas malformados, correlación, persistencia, alertas, Collector ausente, cuatro solicitudes normales, historial tras reinicio y JSON Lines inválido.
