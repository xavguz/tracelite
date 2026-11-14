#!/bin/sh
#
# Prueba end-to-end de la Etapa 3.
#
# Ejecuta process_manager, que a su vez lanza el Collector, Inventory Service y
# Order Service. Comprueba:
#   - 3 trazas y 6 spans (raiz + hijo por pedido).
#   - Una traza distinta por pedido.
#   - Jerarquia correcta: el parent_span_id del hijo es el span_id de su raiz.
#   - Estados: 4 spans SUCCESS y 2 ERROR (ORD-003).
#   - Mensaje de error del fallo simulado.
#   - Cierre ordenado del Collector mediante SHUTDOWN.
#   - Eliminacion del runtime temporal y del socket.
#
# No usa rutas fijas: process_manager crea su propio directorio temporal.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(dirname -- "$SCRIPT_DIR")
PROCESS_MANAGER="$ROOT_DIR/Services/bin/process_manager"

if [ ! -x "$PROCESS_MANAGER" ]; then
    printf '%s\n' \
        "test-tracing: falta $PROCESS_MANAGER; ejecute 'make' primero" >&2
    exit 1
fi

LOG_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tracelite-tracing-XXXXXX")
LOG="$LOG_DIR/run.log"
PM_PID=""

cleanup() {
    if [ -n "$PM_PID" ] && kill -0 "$PM_PID" 2>/dev/null; then
        kill -TERM "$PM_PID" 2>/dev/null || true
        wait "$PM_PID" 2>/dev/null || true
    fi
    rm -rf "$LOG_DIR"
}

fail() {
    printf 'test-tracing: %s\n' "$1" >&2
    if [ -f "$LOG" ]; then
        printf '%s\n' "--- salida de process_manager ---" >&2
        cat "$LOG" >&2 || true
    fi
    exit 1
}

on_exit() {
    status=$?
    trap - EXIT
    cleanup
    exit "$status"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

printf '%s\n' "test-tracing: ejecutando process_manager"

"$PROCESS_MANAGER" >"$LOG" 2>&1 &
PM_PID=$!

set +e
wait "$PM_PID"
pm_status=$?
set -e
PM_PID=""

if [ "$pm_status" -ne 0 ]; then
    fail "process_manager termino con codigo $pm_status"
fi

count_lines() {
    grep -Fc -- "$1" "$LOG" 2>/dev/null || true
}

expect_count() {
    actual=$(count_lines "$1")
    if [ "$actual" -ne "$2" ]; then
        fail "se esperaban $2 lineas con '$1', encontradas $actual"
    fi
}

require_line() {
    if ! grep -Fq -- "$1" "$LOG"; then
        fail "falta en la salida: $1"
    fi
}

reject_line() {
    if grep -Fq -- "$1" "$LOG"; then
        fail "no se esperaba en la salida: $1"
    fi
}

require_line "[process-manager] ejecucion completada correctamente"
require_line "[collector] SHUTDOWN recibido; cerrando"
require_line "[collector] spans aceptados=6 rechazados=0"
require_line "=== TraceLite Collector: 3 traza(s), 6 span(s) ==="

expect_count "[raiz] depth=0" 3
expect_count "[hijo] depth=1" 3
expect_count "spans=2" 3
expect_count "[collector] span aceptado" 6
expect_count "service=order-service operation=process-order" 3
expect_count "service=inventory-service operation=reserve-stock" 3
expect_count "status=SUCCESS" 4
expect_count "status=ERROR" 2
expect_count 'error="simulated inventory failure for ORD-003"' 2

reject_line "[huerfano]"
reject_line "status=UNSET"

# El padre del hijo debe coincidir con el span raiz de la misma traza. Tambien
# se comprueba que las tres raices pertenecen a trazas distintas.
if ! awk '
/^Traza [0-9]+\/3: trace_id=0x/ {
    headers++
    next
}
/\[raiz\] depth=0/ {
    if (!match($0, /trace_id=0x[0-9a-f]+/)) {
        bad = 1
        next
    }
    tid = substr($0, RSTART + 9, RLENGTH - 9)
    if (!match($0, /span_id=0x[0-9a-f]+/)) {
        bad = 1
        next
    }
    sid = substr($0, RSTART + 8, RLENGTH - 8)
    if (tid in root_span_id) {
        duplicate = 1
    }
    root_span_id[tid] = sid
    roots++
    next
}
/\[hijo\] depth=1/ {
    if (!match($0, /trace_id=0x[0-9a-f]+/)) {
        bad = 1
        next
    }
    tid = substr($0, RSTART + 9, RLENGTH - 9)
    if (!match($0, /parent_span_id=0x[0-9a-f]+/)) {
        bad = 1
        next
    }
    pid = substr($0, RSTART + 15, RLENGTH - 15)
    if (!(tid in root_span_id) || root_span_id[tid] != pid) {
        bad = 1
    }
    children++
    next
}
END {
    if (headers != 3 || roots != 3 || children != 3 || duplicate || bad) {
        exit 1
    }
}
' "$LOG"; then
    fail "la jerarquia de spans no es coherente"
fi

RUNTIME_DIR=$(sed -n 's/^\[process-manager\] runtime: //p' "$LOG" | head -n 1)
if [ -z "$RUNTIME_DIR" ]; then
    fail "no se pudo determinar el runtime temporal"
fi
if [ -e "$RUNTIME_DIR" ]; then
    fail "el runtime temporal no fue eliminado: $RUNTIME_DIR"
fi

printf '%s\n' "test-tracing: jerarquia OK (3 raices, 3 hijos)"
printf '%s\n' "test-tracing: OK"
