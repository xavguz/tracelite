#!/bin/sh
# Prueba end-to-end: Service y TraceLite independientes, con datos temporales.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(dirname -- "$SCRIPT_DIR")
PROCESS_MANAGER="$ROOT_DIR/Services/bin/process_manager"
COLLECTOR="$ROOT_DIR/TraceLite-Core/bin/collector"
RUNTIME_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tracelite-tracing-XXXXXX")
INVENTORY_FILE="$RUNTIME_DIR/inventory.json"
INVENTORY_SOCKET="$RUNTIME_DIR/inventory.sock"
COLLECTOR_SOCKET="$RUNTIME_DIR/collector.sock"
TRACES_FILE="$RUNTIME_DIR/traces.jsonl"
SERVICE_LOG="$RUNTIME_DIR/service.log"
COLLECTOR_LOG="$RUNTIME_DIR/collector.log"
INPUT_FILE="$RUNTIME_DIR/input.txt"
SERVICE_PID=""
COLLECTOR_PID=""

cleanup() {
    if [ -n "$SERVICE_PID" ] && kill -0 "$SERVICE_PID" 2>/dev/null; then
        kill -TERM "$SERVICE_PID" 2>/dev/null || true
        wait "$SERVICE_PID" 2>/dev/null || true
    fi
    if [ -n "$COLLECTOR_PID" ] && kill -0 "$COLLECTOR_PID" 2>/dev/null; then
        kill -TERM "$COLLECTOR_PID" 2>/dev/null || true
        wait "$COLLECTOR_PID" 2>/dev/null || true
    fi
    rm -rf "$RUNTIME_DIR"
}

fail() {
    printf 'test-tracing: %s\n' "$1" >&2
    [ ! -f "$SERVICE_LOG" ] || { printf '%s\n' '--- Service ---' >&2; cat "$SERVICE_LOG" >&2; }
    [ ! -f "$COLLECTOR_LOG" ] || { printf '%s\n' '--- TraceLite ---' >&2; cat "$COLLECTOR_LOG" >&2; }
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

[ -x "$PROCESS_MANAGER" ] || fail "falta $PROCESS_MANAGER; ejecute make primero"
[ -x "$COLLECTOR" ] || fail "falta $COLLECTOR; ejecute make primero"
cp "$ROOT_DIR/data/inventory.json" "$INVENTORY_FILE"

TRACELITE_TRACES_FILE="$TRACES_FILE" \
    "$COLLECTOR" "$COLLECTOR_SOCKET" >"$COLLECTOR_LOG" 2>&1 &
COLLECTOR_PID=$!

attempt=0
while [ ! -S "$COLLECTOR_SOCKET" ]; do
    kill -0 "$COLLECTOR_PID" 2>/dev/null || fail "TraceLite termino antes de crear su socket"
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || fail "timeout esperando el socket de TraceLite"
    sleep 0.05
done

cat >"$INPUT_FILE" <<'EOF'
1
2
PRODUCT-002
3
PRODUCT-001
5
3
PRODUCT-003
51
0
EOF

TRACELITE_COLLECTOR_SOCKET="$COLLECTOR_SOCKET" \
TRACELITE_INVENTORY_SOCKET="$INVENTORY_SOCKET" \
TRACELITE_INVENTORY_FILE="$INVENTORY_FILE" \
TRACELITE_ORDER_DELAY_MS=450 \
"$PROCESS_MANAGER" <"$INPUT_FILE" >"$SERVICE_LOG" 2>&1 &
SERVICE_PID=$!

set +e
wait "$SERVICE_PID"
service_status=$?
set -e
SERVICE_PID=""
[ "$service_status" -eq 0 ] || fail "Service termino con codigo $service_status"

kill -TERM "$COLLECTOR_PID"
set +e
wait "$COLLECTOR_PID"
collector_status=$?
set -e
COLLECTOR_PID=""
[ "$collector_status" -eq 0 ] || fail "TraceLite termino con codigo $collector_status"

require_line() {
    grep -Fq -- "$1" "$2" || fail "falta '$1' en $2"
}

require_count() {
    actual=$(grep -Fc -- "$1" "$2" 2>/dev/null || true)
    [ "$actual" -eq "$3" ] || fail "se esperaban $3 coincidencias de '$1'; hay $actual"
}

require_line "[process-manager] modo Service independiente" "$SERVICE_LOG"
require_line "Resultado: OK — 3 product(s)" "$SERVICE_LOG"
require_line "Resultado: OK — product found" "$SERVICE_LOG"
require_line "Resultado: OK — order completed" "$SERVICE_LOG"
require_line "Resultado: OUT_OF_STOCK — insufficient stock" "$SERVICE_LOG"
require_line "[inventory] demora de 450 ms para pedido" "$SERVICE_LOG"
require_line "[process-manager] ejecucion completada correctamente" "$SERVICE_LOG"
grep -Fq '95' "$INVENTORY_FILE" || fail "el pedido no persistio stock=95"

require_line "=== TraceLite Collector: 4 traza(s), 8 span(s) ===" "$COLLECTOR_LOG"
require_count "[collector] span aceptado" "$COLLECTOR_LOG" 8
require_count "[raiz] depth=0" "$COLLECTOR_LOG" 4
require_count "[hijo] depth=1" "$COLLECTOR_LOG" 4
require_count "status=SUCCESS" "$COLLECTOR_LOG" 6
require_count "status=ERROR" "$COLLECTOR_LOG" 2
require_line "operation=list-products-request" "$COLLECTOR_LOG"
require_line "operation=list-products" "$COLLECTOR_LOG"
require_line "operation=find-product-request" "$COLLECTOR_LOG"
require_line "operation=find-product" "$COLLECTOR_LOG"
require_line "operation=process-order" "$COLLECTOR_LOG"
require_line "operation=reserve-stock" "$COLLECTOR_LOG"
[ "$(wc -l <"$TRACES_FILE")" -eq 8 ] ||
    fail "se esperaban 8 spans persistidos"
require_count "[ALERT] Latency anomaly detected" "$COLLECTOR_LOG" 1
require_count "[ALERT] Error anomaly detected" "$COLLECTOR_LOG" 1
require_line '"anomaly_flags":2' "$TRACES_FILE"
require_line '"anomaly_flags":1' "$TRACES_FILE"
require_line "Total requests:       4" "$COLLECTOR_LOG"
require_line "Spans received:       8" "$COLLECTOR_LOG"
require_line "Spans persisted:      8" "$COLLECTOR_LOG"
require_line "Anomalous requests:   2" "$COLLECTOR_LOG"
require_line "Total alerts:         2" "$COLLECTOR_LOG"

max_duration_ms=$(awk '
/duration_ms=/ {
    for (i = 1; i <= NF; i++) {
        if ($i ~ /^duration_ms=/) {
            split($i, value, "=")
            if (value[2] + 0 > maximum) maximum = value[2] + 0
        }
    }
}
END { print maximum + 0 }
' "$COLLECTOR_LOG")
awk -v duration="$max_duration_ms" 'BEGIN { exit !(duration >= 440) }' ||
    fail "la demora controlada no quedo incluida en un span (${max_duration_ms} ms)"

# Para cada hijo, su Trace ID y Parent Span ID deben coincidir con su raiz.
if ! awk '
/\[raiz\] depth=0/ {
    match($0, /trace_id=0x[0-9a-f]+/); trace = substr($0, RSTART + 9, RLENGTH - 9)
    match($0, /span_id=0x[0-9a-f]+/); root[trace] = substr($0, RSTART + 8, RLENGTH - 8)
    next
}
/\[hijo\] depth=1/ {
    match($0, /trace_id=0x[0-9a-f]+/); trace = substr($0, RSTART + 9, RLENGTH - 9)
    match($0, /parent_span_id=0x[0-9a-f]+/); parent = substr($0, RSTART + 15, RLENGTH - 15)
    if (!(trace in root) || root[trace] != parent) bad = 1
    children++
}
END { exit bad || children != 4 }
' "$COLLECTOR_LOG"; then
    fail "la correlacion padre-hijo no es coherente"
fi

printf '%s\n' 'test-tracing: Service independiente, inventario y correlacion OK'
