# TraceLite - Protocolos

Documento actualizado a la Etapa 2. El protocolo de negocio y el protocolo de
telemetría del Collector están implementados; los servicios todavía no generan
spans.

Los procesos comparten host y ABI. Los enteros se transmiten en el orden y
endianness nativos. Las estructuras se inicializan a cero antes de rellenarlas
para que los bytes de padding sean deterministas.

## 1. Protocolo de negocio

### Transporte

- Unix Domain Socket de tipo `SOCK_STREAM`.
- Ruta por defecto: `/tmp/tracelite/inventory.sock`.
- Process Manager usa una ruta única dentro de
  `/tmp/tracelite-XXXXXX/inventory.sock` y la pasa como `argv[1]` a ambos
  servicios.
- Order Service actúa como cliente; Inventory Service como servidor.
- Un intercambio síncrono petición/respuesta por vez.

### Framing

```text
+---------------------------+--------------------------------+
| tl_business_header_t (16) | payload (payload_len bytes)    |
+---------------------------+--------------------------------+
```

Reglas de recepción:

1. Leer exactamente `sizeof(tl_business_header_t)` bytes.
2. Validar `magic == TL_BUSINESS_MAGIC`.
3. Validar `version == TL_BUSINESS_VERSION`.
4. Validar que `kind` sea un valor conocido y el esperado.
5. Validar `payload_len <= TL_BUSINESS_MAX_PAYLOAD`.
6. Validar que `payload_len` coincida con el tamaño exacto del payload.
7. Leer exactamente `payload_len` bytes.

Las funciones `business_send_*()` y `business_recv_*()` implementan estas
reglas. `ipc_send_all()` e `ipc_recv_all()` repiten `send()`/`recv()` hasta
completar la transferencia o detectar un error.

### Cabecera

`tl_business_header_t` ocupa 16 bytes:

| Campo | Tipo | Descripción |
|---|---|---|
| `magic` | `uint32_t` | `TL_BUSINESS_MAGIC` (`0x544C4250`) |
| `version` | `uint16_t` | `TL_BUSINESS_VERSION` (1) |
| `kind` | `uint16_t` | `TL_BUSINESS_MESSAGE_REQUEST` o `..._RESPONSE` |
| `payload_len` | `uint32_t` | Longitud exacta del payload |
| `request_id` | `uint32_t` | Correlación petición/respuesta |

Order Service usa `request_id = 1, 2, 3` y Inventory lo repite en la respuesta.

### Petición: `tl_stock_request_t`

Se envía con `kind = TL_BUSINESS_MESSAGE_REQUEST`.

| Campo | Tipo | Descripción |
|---|---|---|
| `order_id` | `char[32]` | Identificador textual, por ejemplo `"ORD-001"` |
| `product_id` | `char[32]` | Identificador textual, por ejemplo `"PRODUCT-001"` |
| `quantity` | `uint32_t` | Unidades solicitadas |

### Respuesta: `tl_stock_response_t`

Se envía con `kind = TL_BUSINESS_MESSAGE_RESPONSE`.

| Campo | Tipo | Descripción |
|---|---|---|
| `order_id` | `char[32]` | Eco del pedido |
| `status` | `int32_t` | Valor de `tl_stock_status_t` |
| `available_quantity` | `uint32_t` | Stock disponible tras la operación |
| `message` | `char[128]` | Mensaje legible terminado en `'\0'` |

### Estados

| Valor | Significado |
|---|---|
| `TL_STOCK_STATUS_OK` | Reserva realizada |
| `TL_STOCK_STATUS_OUT_OF_STOCK` | Stock insuficiente |
| `TL_STOCK_STATUS_INVALID_REQUEST` | Petición inválida |
| `TL_STOCK_STATUS_SIMULATED_ERROR` | Error controlado de ORD-003 |
| `TL_STOCK_STATUS_INTERNAL_ERROR` | Error interno |
| `TL_STOCK_STATUS_UNKNOWN_PRODUCT` | Producto inexistente |

### Secuencia

```text
Order Service                                Inventory Service
     |                                                |
     |  header(kind=REQUEST, len=sizeof(req))         |
     |  tl_stock_request_t                            |
     |----------------------------------------------->|
     |                                                | valida
     |                                                | consulta inventario
     |  header(kind=RESPONSE, len=sizeof(resp))       |
     |  tl_stock_response_t                           |
     |<-----------------------------------------------|
     | valida y continúa                              |
```

Si un mensaje no respeta el contrato, el receptor descarta la conexión y
finaliza con error.

### Escenarios de prueba

| Pedido | Producto | Cantidad | Resultado |
|---|---|---:|---|
| ORD-001 | PRODUCT-001 | 5 | `OK`, stock 20 → 15 |
| ORD-002 | PRODUCT-002 | 3 | `OK`, stock 10 → 7 |
| ORD-003 | PRODUCT-003 | 1 | `SIMULATED_ERROR`, stock sin cambios |

Los pedidos se envían y procesan estrictamente en ese orden.

## 2. Inventario en memoria

| Producto | Stock inicial |
|---|---:|
| PRODUCT-001 | 20 |
| PRODUCT-002 | 10 |
| PRODUCT-003 | 5 |

No se usan archivos ni bases de datos.

## 3. Telemetría del Collector (implementada en la Etapa 2)

### Transporte

- Unix Domain Socket `AF_UNIX` de tipo `SOCK_DGRAM`.
- Un datagrama contiene exactamente un mensaje.
- No se usa framing por longitud de `SOCK_STREAM`: el datagrama ya delimita el
  mensaje.
- Ruta recibida como argumento; por defecto `TL_COLLECTOR_SOCKET_PATH`.
- Emisor y receptor comparten host y ABI; los campos viajan en orden y
  endianness nativos.
- Tamaño máximo aceptado: `TL_COLLECTOR_MAX_DATAGRAM = 512` bytes.

### Mensaje de span: `tl_collector_message_t`

| Campo | Tipo | Descripción |
|---|---|---|
| `magic` | `uint32_t` | `TL_COLLECTOR_MAGIC` (`0x544C4350`) |
| `version` | `uint16_t` | `TL_COLLECTOR_VERSION` (1) |
| `type` | `uint16_t` | `TL_COLLECTOR_MESSAGE_SPAN` (1) |
| `span_size` | `uint32_t` | Debe ser `sizeof(tl_span_t)` |
| `reserved` | `uint32_t` | Debe valer 0 |
| `span` | `tl_span_t` | Span completo |

En la compilación actual `sizeof(tl_collector_message_t) == 288` bytes; el
límite del datagrama es 512.

### Span: `tl_span_t`

| Campo | Tipo | Descripción |
|---|---|---|
| `trace_id` | `uint64_t` | Identificador de la traza; 0 es inválido |
| `span_id` | `uint64_t` | Identificador del span; 0 es inválido |
| `parent_span_id` | `uint64_t` | 0 indica span raíz |
| `start_monotonic_ns` | `int64_t` | Inicio en `CLOCK_MONOTONIC` |
| `duration_ns` | `int64_t` | Duración en nanosegundos |
| `status` | `int32_t` | Valor de `tl_span_status_t` |
| `flags` | `uint32_t` | Reservado; debe valer 0 |
| `service_name` | `char[32]` | Texto terminado en `'\0'` |
| `operation_name` | `char[64]` | Texto terminado en `'\0'` |
| `error_message` | `char[128]` | Texto terminado en `'\0'`; puede estar vacío |

Estados: `TL_SPAN_STATUS_OK` (se muestra como `SUCCESS`), `ERROR`, `TIMEOUT` y
`CANCELLED`. Un span recibido no puede tener `TL_SPAN_STATUS_UNSET`.

### Mensaje de control `SHUTDOWN`

La Etapa 0 no definía mensajes de control. La Etapa 2 añade una ampliación
compatible:

- Nuevo valor `TL_COLLECTOR_MESSAGE_SHUTDOWN = 2`.
- Nueva estructura `tl_collector_control_t` de exactamente 8 bytes:

| Campo | Tipo | Descripción |
|---|---|---|
| `magic` | `uint32_t` | `TL_COLLECTOR_MAGIC` |
| `version` | `uint16_t` | `TL_COLLECTOR_VERSION` |
| `type` | `uint16_t` | `TL_COLLECTOR_MESSAGE_SHUTDOWN` |

La ampliación conserva `magic` y `version`, y añade un tipo nuevo. Los emisores
que solo conocen `TL_COLLECTOR_MESSAGE_SPAN` no se ven afectados. El receptor
distingue ambos mensajes por el tamaño exacto del datagrama: 8 bytes para
control y `sizeof(tl_collector_message_t)` para span.

### Validación en el receptor

1. Rechazar si `recvmsg()` marca `MSG_TRUNC` o si el tamaño supera 512 bytes.
2. Aceptar solo dos tamaños exactos: control (8) o span
   (`sizeof(tl_collector_message_t)`).
3. Validar `magic`, `version` y `type` según el tamaño.
4. En un span, validar `span_size == sizeof(tl_span_t)` y `reserved == 0`.
5. Rechazar `trace_id == 0`, `span_id == 0` y `parent_span_id == span_id`.
6. Rechazar estados fuera de rango o `UNSET`, `flags != 0`, tiempos negativos
   y desbordamiento en `start_monotonic_ns + duration_ns`.
7. Exigir que `service_name`, `operation_name` y `error_message` contengan un
   terminador dentro de su buffer. `service_name` y `operation_name` no pueden
   estar vacíos.
8. Un span con el mismo `span_id` dentro de la misma traza se rechaza en el
   almacenamiento.

Un datagrama rechazado se registra y se descarta; el Collector continúa
escuchando. Los textos nunca se interpretan como cadenas terminadas sin haber
verificado antes el terminador.

### Agrupación y jerarquía

- Los spans se agrupan por `trace_id`.
- Un `parent_span_id == 0` identifica un span raíz.
- La relación padre-hijo se resuelve con `parent_span_id`, no con el orden de
  recepción.
- Un span cuyo padre no está en la traza se muestra como `huerfano`.
- El emisor de pruebas envía primero los hijos y después los padres para
  demostrar que el orden no afecta a la jerarquía.

### Cierre controlado

1. El emisor envía un datagrama `SHUTDOWN` de 8 bytes.
2. El Collector deja de recibir mensajes nuevos.
3. Imprime todos los spans almacenados.
4. Libera la memoria dinámica.
5. Cierra el socket.
6. Elimina el archivo de socket.
7. Finaliza con código 0 si no hubo errores de E/S.

También se atienden `SIGINT` y `SIGTERM` para cerrar de forma ordenada sin
perder los spans ya almacenados.

### Datos sintéticos de prueba

`collector_test_sender` envía estos cuatro spans, además de casos malformados:

| Traza | Span | Padre | Servicio | Operación | Estado | Duración |
|---|---|---|---|---|---|---:|
| `0x1111111111111111` | `0x1000000000000001` | ninguno | `order-service` | `process-order` | `SUCCESS` | 12.500 ms |
| `0x1111111111111111` | `0x1000000000000002` | `0x...0001` | `inventory-service` | `reserve-stock` | `SUCCESS` | 8.000 ms |
| `0x2222222222222222` | `0x2000000000000001` | ninguno | `order-service` | `process-order` | `ERROR` | 9.250 ms |
| `0x2222222222222222` | `0x2000000000000002` | `0x...0001` | `inventory-service` | `reserve-stock` | `ERROR` | 4.500 ms |

Los tiempos y estados son datos de prueba, no mediciones reales de los
servicios.

## 4. Límites

- `TL_BUSINESS_MAX_PAYLOAD = 256`
- `TL_ORDER_ID_MAX = 32`
- `TL_PRODUCT_ID_MAX = 32`
- `TL_MESSAGE_MAX = 128`
- `TL_SERVICE_NAME_MAX = 32`
- `TL_OPERATION_NAME_MAX = 64`
- `TL_ERROR_MESSAGE_MAX = 128`
- `TL_COLLECTOR_MAX_DATAGRAM = 512`

Los buffers de texto incluyen el terminador `'\0'`. En el protocolo de
negocio, el receptor fuerza el último byte a cero antes de usar un texto
recibido. En el Collector se exige que el terminador exista dentro del campo y,
si no existe, el datagrama se rechaza.
