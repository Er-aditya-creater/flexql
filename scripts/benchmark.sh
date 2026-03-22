#!/usr/bin/env bash
# =============================================================================
#  FlexQL Benchmark Script
#  -----------------------
#  Measures insert throughput and SELECT query time.
#  Targets: ~10 million rows (configurable via ROWS below).
#
#  Usage:
#    ./scripts/benchmark.sh [host] [port] [rows]
#    ./scripts/benchmark.sh 127.0.0.1 9000 1000000
#
#  Output: timings printed to stdout, peak RSS memory reported at end.
# =============================================================================

HOST="${1:-127.0.0.1}"
PORT="${2:-9000}"
ROWS="${3:-1000000}"     # default 1 million; set to 10000000 for full bench
BATCH=500                # rows per SQL batch (fewer round-trips = faster)
CLIENT="./bin/flexql-client"

# ── Colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYAN}[bench]${NC} $*"; }
ok()    { echo -e "${GREEN}[ok]${NC}    $*"; }
err()   { echo -e "${RED}[error]${NC} $*"; }

# ── Prerequisite checks ───────────────────────────────────────────────────────
if [ ! -f "$CLIENT" ]; then
    err "Client binary not found. Run 'make' first."
    exit 1
fi

info "FlexQL Benchmark"
info "  Server : $HOST:$PORT"
info "  Rows   : $ROWS"
info "  Batch  : $BATCH rows per statement"
echo ""

# ── Helper: send SQL to server ────────────────────────────────────────────────
send() {
    echo -e "$1\n.exit" | "$CLIENT" "$HOST" "$PORT" 2>/dev/null
}

# ── 1. Setup — drop and recreate table ───────────────────────────────────────
info "Setting up benchmark table..."
send "CREATE TABLE BENCH(ID INT, VALUE DECIMAL, LABEL VARCHAR, TS DATETIME);" \
    > /dev/null 2>&1 || true   # ignore error if already exists

# ── 2. INSERT benchmark ───────────────────────────────────────────────────────
info "Starting INSERT benchmark ($ROWS rows in batches of $BATCH)..."

TMPFILE=$(mktemp /tmp/flexql_bench_XXXXXX.sql)
trap 'rm -f "$TMPFILE"' EXIT

# Generate all SQL into a temp file to avoid spawning millions of processes
python3 - "$ROWS" "$BATCH" "$TMPFILE" << 'PYEOF'
import sys, random, datetime

rows   = int(sys.argv[1])
batch  = int(sys.argv[2])
out    = sys.argv[3]
base   = datetime.datetime(2020, 1, 1)

with open(out, 'w') as f:
    for i in range(1, rows + 1):
        ts  = base + datetime.timedelta(seconds=i)
        val = round(random.uniform(0, 10000), 2)
        lbl = f"label_{i % 1000}"
        f.write(f"INSERT INTO BENCH VALUES ({i}, {val}, '{lbl}', '{ts}');\n")
        if i % batch == 0:
            f.write("SELECT ID FROM BENCH WHERE ID = 1;\n")   # keep connection alive

PYEOF

INSERT_START=$(date +%s%3N)

# Stream the file to the client line by line
while IFS= read -r sql_line; do
    printf '%s\n' "$sql_line"
done < "$TMPFILE" | "$CLIENT" "$HOST" "$PORT" > /dev/null 2>&1

INSERT_END=$(date +%s%3N)
INSERT_MS=$(( INSERT_END - INSERT_START ))
INSERT_PER_SEC=$(python3 -c "print(f'{$ROWS / ($INSERT_MS / 1000):.0f}')")

ok "INSERT $ROWS rows in ${INSERT_MS}ms  (${INSERT_PER_SEC} rows/sec)"

# ── 3. SELECT benchmarks ──────────────────────────────────────────────────────
info "Running SELECT benchmarks..."

# 3a. Full table scan
T0=$(date +%s%3N)
send "SELECT * FROM BENCH;" > /dev/null
T1=$(date +%s%3N)
ok "SELECT * (full scan)                  : $(( T1 - T0 ))ms"

# 3b. Primary index lookup (WHERE on first column = exact match)
T0=$(date +%s%3N)
send "SELECT * FROM BENCH WHERE ID = $(( ROWS / 2 ));" > /dev/null
T1=$(date +%s%3N)
ok "SELECT WHERE ID=N (index lookup)      : $(( T1 - T0 ))ms"

# 3c. Range scan
T0=$(date +%s%3N)
send "SELECT * FROM BENCH WHERE ID > $(( ROWS - 1000 ));" > /dev/null
T1=$(date +%s%3N)
ok "SELECT WHERE ID>N (range, ~1000 rows) : $(( T1 - T0 ))ms"

# 3d. VARCHAR scan
T0=$(date +%s%3N)
send "SELECT * FROM BENCH WHERE LABEL = 'label_42';" > /dev/null
T1=$(date +%s%3N)
ok "SELECT WHERE LABEL='...' (varchar)    : $(( T1 - T0 ))ms"

# 3e. DATETIME range
T0=$(date +%s%3N)
send "SELECT * FROM BENCH WHERE TS > '2024-01-01 00:00:00';" > /dev/null
T1=$(date +%s%3N)
ok "SELECT WHERE TS>date (datetime range) : $(( T1 - T0 ))ms"

# 3f. Cached SELECT (same query again — should be instant)
T0=$(date +%s%3N)
send "SELECT * FROM BENCH WHERE ID = $(( ROWS / 2 ));" > /dev/null
T1=$(date +%s%3N)
ok "SELECT (LRU cache hit, same query)    : $(( T1 - T0 ))ms"

# ── 4. Memory usage ───────────────────────────────────────────────────────────
info "Checking server memory usage..."
SERVER_PID=$(pgrep -f "flexql-server" | head -1)
if [ -n "$SERVER_PID" ]; then
    RSS_KB=$(awk '/VmRSS/ {print $2}' /proc/$SERVER_PID/status 2>/dev/null || echo "N/A")
    ok "Server RSS: ${RSS_KB} kB  (~$(python3 -c "print(f'{$RSS_KB/1024:.1f}')") MB)"
else
    info "Could not determine server PID for memory reading."
fi

echo ""
info "Benchmark complete."
