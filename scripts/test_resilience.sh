#!/bin/sh
# Pruebas de resiliencia de la entrega: no altera datos ni sockets normales.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SERVICE="$ROOT_DIR/Services/bin/process_manager"
COLLECTOR="$ROOT_DIR/TraceLite-Core/bin/collector"
TRACELITE="$ROOT_DIR/tracelite"
RUNTIME_DIR=$(mktemp -d /tmp/tracelite-resilience-XXXXXX)
INVENTORY_FILE="$RUNTIME_DIR/inventory.json"
INVENTORY_SOCKET="$RUNTIME_DIR/inventory.sock"
COLLECTOR_SOCKET="$RUNTIME_DIR/collector.sock"
TRACES_FILE="$RUNTIME_DIR/traces.jsonl"
INPUT_FILE="$RUNTIME_DIR/input.txt"
COLLECTOR_PID=""

cleanup() {
    if [ -n "$COLLECTOR_PID" ] && kill -0 "$COLLECTOR_PID" 2>/dev/null; then
        kill -TERM "$COLLECTOR_PID" 2>/dev/null || true
        wait "$COLLECTOR_PID" 2>/dev/null || true
    fi
    rm -rf "$RUNTIME_DIR"
}
trap 'status=$?; cleanup; exit "$status"' EXIT

fail() {
    printf 'test-resilience: %s\n' "$1" >&2
    exit 1
}

[ -x "$SERVICE" ] || fail "falta process_manager"
[ -x "$COLLECTOR" ] || fail "falta collector"
cp "$ROOT_DIR/data/inventory.json" "$INVENTORY_FILE"

# El negocio debe terminar correctamente aunque no exista Collector.
printf '1\n0\n' >"$INPUT_FILE"
TRACELITE_COLLECTOR_SOCKET="$RUNTIME_DIR/missing.sock" \
TRACELITE_INVENTORY_SOCKET="$INVENTORY_SOCKET" \
TRACELITE_INVENTORY_FILE="$INVENTORY_FILE" \
"$SERVICE" <"$INPUT_FILE" >"$RUNTIME_DIR/no-collector.log" 2>&1 ||
    fail "Service fallo sin Collector"
grep -Fq 'Resultado: OK — 3 product(s)' "$RUNTIME_DIR/no-collector.log" ||
    fail "Service no atendio negocio sin Collector"
[ ! -e "$INVENTORY_SOCKET" ] || fail "socket de Inventory residual"

TRACELITE_TRACES_FILE="$TRACES_FILE" \
    "$COLLECTOR" "$COLLECTOR_SOCKET" >"$RUNTIME_DIR/collector.log" 2>&1 &
COLLECTOR_PID=$!
attempt=0
while [ ! -S "$COLLECTOR_SOCKET" ]; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || fail "timeout esperando Collector"
    sleep 0.05
done

# Cuatro solicitudes normales con demora controlada inferior al umbral.
cat >"$INPUT_FILE" <<'EOF'
1
2
PRODUCT-001
2
PRODUCT-002
3
PRODUCT-003
1
0
EOF
TRACELITE_COLLECTOR_SOCKET="$COLLECTOR_SOCKET" \
TRACELITE_INVENTORY_SOCKET="$INVENTORY_SOCKET" \
TRACELITE_INVENTORY_FILE="$INVENTORY_FILE" \
TRACELITE_ORDER_DELAY_MS=10 \
TRACELITE_TRACES_FILE="$TRACES_FILE" \
"$SERVICE" <"$INPUT_FILE" >"$RUNTIME_DIR/service.log" 2>&1 ||
    fail "Service fallo en solicitudes normales"

kill -TERM "$COLLECTOR_PID"
wait "$COLLECTOR_PID" || fail "Collector fallo"
COLLECTOR_PID=""
[ ! -e "$COLLECTOR_SOCKET" ] || fail "socket de Collector residual"
grep -Fq 'Total requests:       4' "$RUNTIME_DIR/collector.log" ||
    fail "conteo de solicitudes normal incorrecto"
grep -Fq 'Normal requests:      4' "$RUNTIME_DIR/collector.log" ||
    fail "solicitudes normales incorrectas"
if grep -Fq '[ALERT]' "$RUNTIME_DIR/collector.log"; then
    fail "una solicitud normal genero alerta"
fi
[ "$(wc -l <"$TRACES_FILE")" -eq 8 ] || fail "persistencia normal incompleta"

# TraceLite reiniciado debe leer lo persistido y omitir JSON Lines corrupto.
printf '{ json invalido\n' >>"$TRACES_FILE"
printf '2\n1\n0\n0\n' | TRACELITE_TRACES_FILE="$TRACES_FILE" \
    "$TRACELITE" >"$RUNTIME_DIR/history.log"
grep -Fq '[history] linea 9 omitida: registro invalido' "$RUNTIME_DIR/history.log" ||
    fail "historial no omitio JSON invalido"
grep -Fq 'Trace ID:' "$RUNTIME_DIR/history.log" ||
    fail "historial no recupero spans persistidos"

printf '%s\n' 'test-resilience: Collector ausente, normales e historial OK'
