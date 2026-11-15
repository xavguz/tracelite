# TraceLite — Checklist de avance

**Entrega:** avance parcial — primera versión funcional  
**Estado global:** requerimientos mínimos y checklist del enunciado cumplidos

---

## 1. Requerimientos mínimos

- [x] **Arquitectura de procesos.**
  - [x] Proceso principal: `process_manager`.
  - [x] Al menos dos servicios/procesos hijos: `order_service`, `inventory_service`.
  - [x] Proceso recolector adicional: `collector`.
  - [x] Creación con `fork()`.
  - [x] Arranque con `execl()`.
  - [x] Responsabilidades y relaciones documentadas.

- [x] **Instrumentación de operaciones (spans).**
  - [x] Trace ID.
  - [x] Span ID.
  - [x] Parent Span ID (0 en el span raíz).
  - [x] Nombre del servicio.
  - [x] Operación.
  - [x] Inicio con `CLOCK_MONOTONIC`.
  - [x] Fin derivado (`inicio + duración`).
  - [x] Duración en milisegundos en la salida.
  - [x] Estado `SUCCESS` / `ERROR`.

- [x] **Propagación de contexto.**
  - [x] `trace_context { trace_id, parent_span_id }` en la petición.
  - [x] Inventory genera su propio Span ID.
  - [x] Inventory conserva el Trace ID recibido.
  - [x] Relación padre-hijo verificada.

- [x] **Comunicación y recolección.**
  - [x] Collector Lite implementado.
  - [x] IPC funcional entre procesos independientes.
  - [x] `AF_UNIX/SOCK_STREAM` para negocio.
  - [x] `AF_UNIX/SOCK_DGRAM` para telemetría.
  - [x] Mecanismo justificado frente a pipes y memoria compartida.
  - [x] Formato de mensajes documentado.

- [x] **Visualización inicial en terminal.**
  - [x] Agrupación por Trace ID.
  - [x] Servicio y operación.
  - [x] Estado.
  - [x] Duración en milisegundos.
  - [x] Relación padre-hijo.

- [x] **Prueba básica de funcionamiento.**
  - [x] Tres solicitudes completas: ORD-001, ORD-002, ORD-003.
  - [x] Demora de 150 ms en ORD-002 reflejada en la duración.
  - [x] Error de negocio en ORD-003 registrado en los spans.
  - [x] Los procesos no terminan de forma anómala.

---

## 2. Checklist de verificación del enunciado

- [x] Se crean al menos dos servicios/procesos y se utiliza `fork()` y `exec()`.
- [x] Los spans contienen los identificadores, tiempos y estados solicitados.
- [x] El contexto de trazabilidad se propaga correctamente entre servicios.
- [x] Los spans llegan al recolector mediante un mecanismo IPC.
- [x] Se muestran trazas completas y una prueba de anomalía en terminal.

---

## 3. Condiciones técnicas

- [x] Implementación principal en C sobre GNU/Linux.
- [x] Compilación mediante Makefile.
- [x] IPC funcional entre procesos independientes.
- [x] Tiempos reales con `clock_gettime(CLOCK_MONOTONIC)`.
- [x] Sin SDK de OpenTelemetry, Grafana Tempo ni plataformas equivalentes.
- [x] Sin dependencias externas.
- [x] Compilación sin advertencias con `-Wall -Wextra -Wpedantic`.

---

## 4. Entregables del avance

- [x] Código fuente en C.
- [x] Makefile raíz y Makefiles de cada proyecto.
- [x] README con instrucciones de compilación y ejecución.
- [x] Informe de avance consolidado en `docs/informe-avance.md`.
- [x] Diagrama de arquitectura y modelo de procesos.
- [x] Descripción del mecanismo IPC y su justificación.
- [x] Formato de span y de mensajes.
- [x] Evidencias de pruebas en texto en `docs/evidencias/`.
- [x] Checklist de funcionalidades implementadas y pendientes.

> Las capturas gráficas y el video quedan a cargo del autor de la entrega.

---

## 5. Pendientes para la entrega final

Estos puntos no forman parte del avance parcial y quedan para la siguiente
etapa:

- [ ] Dashboard con actualización periódica.
- [ ] Persistencia de trazas (archivo o base de datos).
- [ ] Comunicación entre múltiples hosts.
- [ ] Procesamiento concurrente y biblioteca thread-safe.
- [ ] Autenticación y cifrado del canal local.
- [ ] Reintentos o buffer de telemetría ante fallos del Collector.
- [ ] Escenarios de negocio configurables.
- [ ] Capturas gráficas de las pruebas, si la rúbrica las exige.

### Observaciones

- La demora de ORD-002 es fija (150 ms). El enunciado permite “incremento
  aleatorio **o** un error”; el error de ORD-003 cumple esa condición. Si se
  exige aleatoriedad literal, puede convertirse la demora en un rango.
- Las fuentes de `TraceLite-Core/src/lib/` ya están incluidas en el área de
  preparación de Git, por lo que un clon limpio puede compilar el proyecto.

---

## 6. Evidencias

Los resultados completos están en:

- `docs/evidencias/build.txt`
- `docs/evidencias/check.txt`
- `docs/evidencias/test.txt`
- `docs/evidencias/run.txt`
- `docs/evidencias/cleanup.txt`

Comandos de reproducción:

```sh
make clean
make
make check
make test
make run
```
