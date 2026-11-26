#!/bin/sh
#
# Prueba de humo del Collector Lite (Etapa 2).
#
# Crea un directorio temporal unico, arranca el Collector con una ruta de
# socket propia, espera a que el socket exista, ejecuta el emisor de pruebas y
# comprueba que el Collector cierra ordenadamente y elimina el socket.
#
# No usa una ruta fija compartida entre ejecuciones.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CORE_DIR=$(dirname -- "$SCRIPT_DIR")
BIN_DIR="$CORE_DIR/bin"
RUNTIME_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tracelite-collector-XXXXXX")
SOCKET_PATH="$RUNTIME_DIR/collector.sock"
COLLECTOR_LOG="$RUNTIME_DIR/collector.log"
COLLECTOR_PID=""

cleanup() {
    if [ -n "$COLLECTOR_PID" ] && kill -0 "$COLLECTOR_PID" 2>/dev/null; then
        kill -TERM "$COLLECTOR_PID" 2>/dev/null || true
        wait "$COLLECTOR_PID" 2>/dev/null || true
    fi
    rm -rf "$RUNTIME_DIR"
}

on_exit() {
    status=$?
    trap - EXIT
    if [ "$status" -ne 0 ] && [ -f "$COLLECTOR_LOG" ]; then
        printf '%s\n' "--- log del collector ---" >&2
        cat "$COLLECTOR_LOG" >&2 || true
    fi
    cleanup
    exit "$status"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

TRACELITE_TRACES_FILE="$RUNTIME_DIR/traces.jsonl" \
    "$BIN_DIR/collector" "$SOCKET_PATH" >"$COLLECTOR_LOG" 2>&1 &
COLLECTOR_PID=$!

attempt=0
while [ ! -S "$SOCKET_PATH" ]; do
    if ! kill -0 "$COLLECTOR_PID" 2>/dev/null; then
        printf '%s\n' "test-collector: el collector termino antes de crear el socket" >&2
        exit 1
    fi

    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        printf '%s\n' "test-collector: timeout esperando el socket" >&2
        exit 1
    fi
    sleep 0.05
done

printf '%s\n' "test-collector: collector listo en $SOCKET_PATH"
"$BIN_DIR/collector_test_sender" "$SOCKET_PATH"

set +e
wait "$COLLECTOR_PID"
collector_status=$?
set -e
COLLECTOR_PID=""

cat "$COLLECTOR_LOG"

if [ "$collector_status" -ne 0 ]; then
    printf 'test-collector: el collector termino con codigo %d\n' \
        "$collector_status" >&2
    exit 1
fi

if [ -e "$SOCKET_PATH" ]; then
    printf '%s\n' "test-collector: el socket no fue eliminado" >&2
    exit 1
fi

require_log_line() {
    if ! grep -Fq -- "$1" "$COLLECTOR_LOG"; then
        printf 'test-collector: falta en la salida: %s\n' "$1" >&2
        exit 1
    fi
}

require_log_line "[collector] SHUTDOWN recibido; cerrando"
require_log_line "[collector] spans aceptados=4 rechazados=8"
require_log_line "=== TraceLite Collector: 2 traza(s), 4 span(s) ==="
require_log_line "---------- Traza 1/2 ----------"
require_log_line "ID:    0x1111111111111111"
require_log_line "ID:    0x2222222222222222"
require_log_line "Jerarquia:"
require_log_line '`-- process-order  [SUCCESS]  order-service  12.500 ms  (raiz)'
require_log_line '|-- reserve-stock  [ERROR]  inventory-service  4.500 ms'
require_log_line "span_id: 0x1000000000000002 | parent: 0x1000000000000001"
require_log_line "Total requests:       2"
require_log_line "Spans received:       4"
require_log_line "Spans persisted:      4"
require_log_line "Anomalous requests:   1"
require_log_line "Total alerts:         1"

if [ "$(wc -l <"$RUNTIME_DIR/traces.jsonl")" -ne 4 ]; then
    printf '%s\n' "test-collector: persistencia incompleta" >&2
    exit 1
fi

printf '%s\n' "test-collector: OK"
